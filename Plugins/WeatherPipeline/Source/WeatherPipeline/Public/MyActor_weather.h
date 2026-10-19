// Fill out your copyright notice in the Description page of Project Settings.

#pragma once
#include "Engine/SceneCapture2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HighResScreenshot.h"
#include "ImageUtils.h"

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "RTSPStreamer.h"
#include "MyActor_weather.generated.h"

class ACharacter;
class UExponentialHeightFogComponent;

namespace WeatherConstants
{
	// Number of snapshots captured per weather type (rain, snow, fog) in a single pipeline run.
	// Also the expected length of each of the three per-type sweep arrays.
	constexpr int32 CapturesPerWeatherType = 5;

	// Gap between phases (rain -> snow -> fog). Deliberately tiny so the change reads as abrupt.
	// Not 0: FTimerManager::SetTimer treats a rate <= 0 as "clear this timer", so a zero here would
	// silently stop the chain instead of running it immediately.
	constexpr float WeatherChainDelaySeconds = 0.05f;

	// Litmus preview hold time per weather type (visual check only, no capture)
	constexpr float LitmusStepDurationSecs    = 3.0f;
	constexpr float LitmusStepDurationFogSecs = 3.0f;

	// After a litmus type's hold expires the weather is cleared and this gap is allowed to pass
	// before the next type starts. Clearing is instant but the particles already in flight are not:
	// without the gap, snow would begin while the last of the rain is still falling, which is exactly
	// the overlap the litmus is meant to rule out.
	constexpr float LitmusClearGapSeconds = 1.0f;

	// Delay after StopFog() before requesting game exit, to allow file I/O to flush
	constexpr float ExitDelaySeconds = 1.0f;

	// Ultra Dynamic Sky grades Rain, Snow, Fog, Cloud Coverage, Wind Intensity and Thunder/Lightning
	// on 0..10, NOT 0..1. Confirmed from the shipped presets: Rain_Thunderstorm reads rain=10.0,
	// cloud=8.0, wind=10.0. Material Wetness and Material Snow Coverage are the exceptions and really
	// are 0..1. Treating the first group as 0..1 makes every value a tenth of what was intended,
	// which is exactly why the hand-authored sweep produced barely visible weather.
	constexpr float UDSStateScale = 10.0f;
}

/** Which weather effect a pipeline step is applying. */
UENUM(BlueprintType)
enum class EWeatherKind : uint8
{
	Rain UMETA(DisplayName = "Rain"),
	Snow UMETA(DisplayName = "Snow"),
	Fog  UMETA(DisplayName = "Fog")
};

/**
 * One discrete weather condition in a matched-capture run: a UDS preset plus the short name used in
 * filenames and metadata.
 *
 * The dataset's value comes from *matched* captures - the same camera pose under different weather,
 * with the sun held still - so weather is a small set of distinct conditions rather than a continuous
 * sweep. Each condition is flown over the whole path before the next one is applied, because
 * changing weather costs a transition and a settle while moving the camera costs nothing.
 */
USTRUCT(BlueprintType)
struct FWeatherCondition
{
	GENERATED_BODY()

	/** Object path of a UDS_Weather_Settings preset, e.g. .../Weather_Presets/Rain.Rain */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	FString PresetPath;

	/** Short name for filenames and the "condition" metadata field. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	FString Label;

	/**
	 * Compass direction the wind blows FROM (0 = north, 90 = east), or < 0 for the level's own.
	 *
	 * A scene setting on the UDW actor, not a preset channel: a preset's own Wind Direction never
	 * reaches UDW (set to 90 in a preset, the live value still read the level's 131.5).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float WindFromCompassDeg = -1.0f;

	FWeatherCondition& FromCompass(float CompassDeg) { WindFromCompassDeg = CompassDeg; return *this; }

	/**
	 * Pins Wind Intensity (0..10) after the preset is applied. < 0 leaves the preset's own value.
	 *
	 * Exists because UDS's heavy snow presets carry very high wind, which blows the flakes into near
	 * horizontal streaks - visually indistinguishable from rain. Snow reads as snow when it falls
	 * slowly, so the snow conditions pin the wind down while keeping everything else the preset says.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float WindIntensityOverride = -1.0f;

	/**
	 * Pins Fog (0..10) after the preset is applied. < 0 leaves the preset's own value.
	 *
	 * UDS's Snow preset carries fog=5.0, which greys the whole scene out; combined with its
	 * cloud=8.5 the image goes so dark that white flakes stop reading against it at all. Thinning the
	 * fog is what brings the snow back, and it is a bigger lever than the particle settings.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float FogOverride = -1.0f;

	/** Pins Cloud Coverage (0..10). < 0 leaves the preset's value. Lower means a brighter sky. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float CloudCoverageOverride = -1.0f;

	/**
	 * Pin precipitation (0..10). < 0 leaves the preset's value.
	 *
	 * UDS ships three snow presets and three rain presets, which is coarser than a dataset wants -
	 * there is no asset between Snow_Light and Snow, for instance. Pinning the amount lets an
	 * intermediate step be expressed without authoring new preset assets.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float RainOverride = -1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float SnowOverride = -1.0f;

	/**
	 * How much snow has settled on surfaces, 0..1. < 0 uses the preset's value.
	 *
	 * Note the scale: unlike Rain/Snow/Fog this really is 0..1, not 0..10. Separate from SnowOverride
	 * because falling snow and settled snow are different quantities - UDS's Snow and Snow_Blizzard
	 * presets both ship coverage 1.0 despite very different snowfall, so relying on the presets gives
	 * a flat ground response while the sky intensifies.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float MaterialSnowCoverageOverride = -1.0f;

	/** How wet surfaces look, 0..1. < 0 uses the preset's value. The rain equivalent of the above. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float MaterialWetnessOverride = -1.0f;

	/**
	 * Multiplier on UDS's height fog density for this condition. <= 0 leaves it alone.
	 *
	 * The Fog channel tops out at 10, so this is the only way to go denser than the heaviest preset -
	 * needed once the fog conditions are shifted up and the top one has nowhere left to climb.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float FogDensityScaleOverride = -1.0f;

	// ---- Particle look, applied after the transition finishes ----
	// These cannot be written up front: UDW's transition recomputes particle count and scale from the
	// weather state as it eases toward the preset, so anything set beforehand is overwritten on the
	// way there. They are applied once the transition has landed.

	/** Rain/snow particle count, whichever the condition precipitates. 0 leaves UDW's own value. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	int32 ParticleSpawnCount = 0;

	/** Drop/flake size multiplier. 0 leaves UDW's own value. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float ParticleScale = 0.0f;

	/** Drop/flake opacity 0..1. 0 leaves UDW's own value. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float ParticleAlpha = 0.0f;

	/**
	 * Post Process Wind Fog strength for this condition.
	 *
	 *   < 0  use the actor-level default (bEnableMovingFog / MovingFogIntensityScale)
	 *     0  explicitly OFF - what makes the static fog conditions static
	 *   > 0  explicitly ON at this intensity
	 *
	 * Per-condition rather than global because "moving fog" is only a distinct category if the
	 * ordinary fog frames are demonstrably not moving. With one global switch every fog frame would
	 * drift and the two categories would be the same picture.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float MovingFogIntensity = -1.0f;

	/**
	 * Put the volumetric cloud layer this far above the terrain for this condition (cm). < 0 uses the
	 * actor's own cloud settings.
	 *
	 * Dropping the real cloud layer down to ground level is a better ground fog than any fog setting:
	 * volumetric clouds have genuine 3D structure and drift with the wind, so they thin, part and
	 * roll over terrain the way fog banks do. Height fog can only ever be a distance gradient.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	bool bOverrideCloudHeight = false;

	/** Replaces the actor's cloud height for this condition (m), interpreted in whichever frame
	 *  CloudAltitudeMode uses. Negative is meaningful: with the mode set to AboveDrone, -60 puts the
	 *  layer 60 m *below* the camera, which is how a cloud deck becomes fog you are looking down on.
	 *  A separate bool rather than a negative sentinel, precisely so negatives stay usable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float CloudHeightOffsetM = 0.0f;

	/** Measure this condition's cloud height from the terrain rather than from whatever frame
	 *  CloudAltitudeMode uses. Valley fog has to sit at ground level to fill the valleys and let the
	 *  peaks through, and that is true regardless of how high the drone happens to be flying. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	bool bCloudHeightFromGround = false;

	/**
	 * Turn Cesium's "Generate Smooth Normals" on for this condition only.
	 *
	 * Snow accumulation needs smooth normals to decide which surfaces face up - with the flat normals
	 * Cesium generates by default, photogrammetry gives a faceted result and the snow mask breaks up.
	 * But smooth normals also change how the geometry itself reads, so leaving them on would alter
	 * every frame in the dataset, not just the snowy ones. Toggling per condition keeps the clear and
	 * rain frames looking as they did.
	 *
	 * Costs a tileset reload each time it flips, which the capture gate waits out.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	bool bRequiresSmoothNormals = false;

	/** Whether UDS renders its exponential height fog at all for this condition.
	 *
	 *  The one genuinely modular control over height fog. Every other lever - base density, the four
	 *  falloffs, the density contributions - is a shared scalar that UDS recomputes from the weather
	 *  state, so suppressing one condition's haze through them means fighting the update chain and
	 *  losing a condition later. This is a plain bool UDS reads directly: off means off, with nothing
	 *  to recompute it back.
	 *
	 *  It disables volumetric fog too, since they share the component. That is a real cost for the
	 *  fog conditions and none at all for the clear ones - a sky with no fog has no volumetric fog to
	 *  lose. So the split is off wherever haze is unwanted, on wherever fog is the point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	bool bRenderHeightFog = true;

	/** UDS "Extinction Scale" for this condition - how opaque the cloud volume is. <= 0 leaves it.
	 *  Low values are what turn a solid cloud deck into something translucent enough to read as fog. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float CloudExtinctionOverride = -1.0f;
};

/**
 * What the cloud base altitude is measured from.
 *
 * The three are genuinely different references, and picking the wrong one is why cloud settings kept
 * behaving unpredictably across locations: UDS's own altitudes are measured from world Z=0, but in a
 * Cesium world the terrain is nowhere near Z=0 and moves whenever the georeference does.
 */
UENUM(BlueprintType)
enum class ECloudAltitudeMode : uint8
{
	/** Leave the cloud layer exactly as the UDS preset authored it - write nothing.
	 *
	 *  Every other mode pins ONE altitude across every condition, which is what makes the deck read as
	 *  a slab at a fixed height whatever the weather. UDS gives each preset its own layer altitude and
	 *  thickness - a storm deck sits lower than fair-weather cloud - and overwriting that with a single
	 *  number throws away the part of the preset that made it look like weather.
	 *
	 *  Per-condition overrides still apply, so valley fog can still be pulled down to the terrain. */
	UsePreset UMETA(DisplayName = "Leave it to the UDS preset"),

	/** Height above the terrain sampled ONCE when the run starts, then held for the whole run.
	 *
	 *  Adapts to wherever the Cesium georeference has been moved to - fly the Alps or the Netherlands
	 *  and the layer lands sensibly without retuning - while staying fixed during the run, so it does
	 *  not ride up and down with the relief the way live tracing does. */
	AboveTerrainAtStart UMETA(DisplayName = "Above the terrain, sampled at run start"),

	/** Height above the terrain directly below the drone, re-measured at every capture. Correct over
	 *  flat ground; over mountains the reference swings with each valley and ridge. */
	AboveGround UMETA(DisplayName = "Above the ground below the drone (live)"),

	/** Height above the drone itself. The layer follows the camera and can never be flown through. */
	AboveDrone  UMETA(DisplayName = "Above the drone"),

	/** Real-world altitude in metres above the WGS84 ellipsoid, converted through the Cesium
	 *  georeference. The same number means the same altitude anywhere on the globe, and matches what
	 *  a real cloud base would be quoted as. */
	RealWorldAltitude UMETA(DisplayName = "Real-world altitude (m above ellipsoid)")
};

/**
 * One location to capture. A run visits each in turn, flying the whole trajectory in every enabled
 * weather condition before moving on to the next.
 *
 * Sites are visited within a single session rather than by restarting the editor per location:
 * Cesium's georeference can be moved at runtime (that is what origin rebasing does), so the globe
 * simply slides under the level and the tiles restream. That keeps one run reproducible as one
 * artifact instead of N separate ones that have to be stitched together afterwards.
 */
USTRUCT(BlueprintType)
struct FCaptureSite
{
	GENERATED_BODY()

	/** Name for the output subfolder and the metadata. Keep it filename-safe. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Site")
	FString Name = TEXT("site");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Site")
	double Latitude = 48.7665;

	/** Degrees, west negative - the signed convention, not 0..360. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Site")
	double Longitude = 11.4258;

	/** Height of the level origin above the WGS84 ellipsoid (m). The default keeps whatever height
	 *  the level was authored with - see AMyActor_weather::KeepLevelOriginHeight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Site")
	double HeightM = -1.0e9;

	/** Clock for this site's captures, HHMM. Negative keeps the run-wide TimeOfDay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Site")
	float TimeOfDay = -1.0f;

	/** Condition labels to capture here, in order. Empty keeps the run-wide CaptureConditionFilter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Site")
	TArray<FString> Conditions;

	/** "Key=Value" settings of AMyActor_weather for this site only - its camera: path pattern, pitch,
	 *  height and path geometry. Each Key must be a Config property; applied when the site starts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Site")
	TArray<FString> Settings;
};

/**
 * One capture station on the drone's route: where the camera sits and where it looks.
 *
 * Captures happen at exact waypoints rather than wherever a simulated flight happens to be, because
 * matched pairs need the pose to reproduce to the pixel across passes - which a physics-driven
 * approach cannot guarantee. Flying smoothly between waypoints is therefore cosmetic, and only
 * matters if the run is later extended to record video.
 */
USTRUCT(BlueprintType)
struct FCaptureWaypoint
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	FVector Location = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	FRotator Rotation = FRotator::ZeroRotator;

	/** Clearance this waypoint holds over the ground directly beneath it, in cm. Set when the path is
	 *  generated: SpawnHeightAboveGroundM plus any tier or climb offset. Used by terrain following. */
	UPROPERTY()
	float HeightAboveGroundCm = 0.0f;

	/** True once the ground under this waypoint has been probed and Location.Z set from it. Probed on
	 *  the first visit only, so every condition reuses the identical pose and the images stay paired. */
	UPROPERTY()
	bool bGroundResolved = false;

	/** Whether the probe actually hit terrain. False means GroundZ is unknown and the metadata writes
	 *  null for the ground fields rather than a number nobody measured. */
	UPROPERTY()
	bool bGroundMeasured = false;

	/** World Z of the ground under this waypoint, in cm. Valid only when bGroundMeasured. */
	UPROPERTY()
	double GroundZ = 0.0;

	/** The random camera spot this capture point belongs to (an orbit shot has several). */
	UPROPERTY()
	int32 Spot = 0;

	/** The spot's time of day (HHMM); negative = the run's TimeOfDay. */
	UPROPERTY()
	float TimeOfDay = -1.0f;
};

/**
 * One capture step: everything the pipeline pushes into Ultra Dynamic Weather / Ultra Dynamic Sky
 * before taking a screenshot.
 *
 * A value of 0 means "don't write this property, keep whatever UDW/UDS already has", so a step only
 * has to specify what it actually cares about. The exception is TimeOfDayOverride, which uses a
 * negative sentinel because 0 is a valid clock reading (midnight).
 *
 * Edit the defaults in AMyActor_weather::BuildDefaultWeatherSteps() — that function is the tuning
 * surface for the whole dataset.
 */
USTRUCT(BlueprintType)
struct FUDWWeatherStep
{
	GENERATED_BODY()

	// ---- Weather state: written into UDW's "Manual Weather State" object (0..1 unless noted) ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float Rain = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float Snow = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float Fog = 0.0f;

	/** Drives UDS cloud thickness, sky colour and sun scattering — the "matching sky" response. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float CloudCoverage = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float WindIntensity = 0.0f;

	/** Wind heading in degrees, 0..360. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float WindDirectionDeg = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float ThunderLightning = 0.0f;

	/** Wet-surface darkening/specular on materials that include the UDS wetness functions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float MaterialWetness = 0.0f;

	/** Ground snow accumulation target. DLWE builds depth from this value over time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float MaterialSnowCoverage = 0.0f;

	// ---- Particle look / extent: written straight onto the Ultra_Dynamic_Weather actor ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Particles")
	int32 RainParticleSpawnCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Particles")
	int32 SnowParticleSpawnCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Particles")
	float RainDropsScale = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Particles")
	float SnowFlakesScale = 0.0f;

	/** Particle opacity, 0..1. UDW "Rain Drops Alpha" / "Snow Flakes Alpha". 0 => leave UDW's value. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Particles")
	float RainDropsAlpha = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Particles")
	float SnowFlakesAlpha = 0.0f;

	/** How far the weather particles extend around the control point (cm). UDW "Max Spawn Distance". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Particles")
	float SpawnRadius = 0.0f;

	/** Nearest distance to the camera at which particles may spawn (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Particles")
	float MinimumParticleDistance = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Particles")
	float RainWindVelocity = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Particles")
	float SnowWindVelocity = 0.0f;

	// ---- Ground accumulation (Dynamic Landscape Weather Effects) ----

	/** Max snow depth DLWE builds on the ground (cm). Needs DLWE_SnowCoverage in the surface material. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|DLWE")
	float SnowDepth = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|DLWE")
	float PuddleCoverage = 0.0f;

	// ---- Sky: written onto the Ultra_Dynamic_Sky actor ----

	/** < 0 => use the actor-level TimeOfDay. Otherwise a 0..2400 clock reading for this step only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Sky")
	float TimeOfDayOverride = -1.0f;

	/** Volumetric cloud density on top of what Cloud Coverage already gives. UDS "Extinction Scale". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Sky")
	float CloudDensity = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Sky")
	float CloudDirectionDeg = 0.0f;

	// ---- Fog strength (UDS). The Fog state value alone is tuned for a ground-level viewer; from a
	// drone looking across a city these are what make it read as fog rather than a faint haze. ----

	/** Multiplier on the height fog density the weather state produces. UDS "Scale Fog Density". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Fog")
	float FogDensityScale = 0.0f;

	/** How far from the camera fog begins (cm). Written to both UDS fog start-distance bounds so the
	 *  value is used regardless of fog amount. < 0 => leave UDS alone. 0 is legitimate (fog at the
	 *  camera), which is why the sentinel is negative rather than zero. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Fog")
	float FogStartDistance = -1.0f;

	/** How far volumetric fog is computed (cm). UE's default is far too short for a city view. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Fog")
	float VolumetricFogDistance = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Fog")
	float VolumetricFogExtinction = 0.0f;

	// ---- Escape hatches: reach any UDW/UDS property by name without changing this struct ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Advanced")
	TMap<FName, float> ExtraFloats;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Advanced")
	TMap<FName, bool> ExtraBools;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Advanced")
	TMap<FName, float> ExtraSkyFloats;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather|Advanced")
	TMap<FName, bool> ExtraSkyBools;

	// ---- Bookkeeping ----

	/** Short human label, e.g. "drizzle" / "heavy". Used in the PNG filename and the JSON metadata. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	FString Label;

	/**
	 * Dwell between applying this step and taking the screenshot. Falling particles and clouds settle
	 * in ~3 s, but DLWE ground accumulation is stateful and builds over time — so snow steps use a
	 * longer value to make sure the image matches the metadata.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	float SettleSeconds = 3.0f;
};

/**
 * Manages the automated weather data-capture pipeline for synthetic dataset generation.
 *
 * Execution flow (filtered by bEnableRain / bEnableSnow / bEnableFog flags):
 *   StartRain()  -> UpdateRainInterval() walks RainSteps
 *               -> ChainAfterRain() picks Snow / Fog / Exit based on flags
 *   StartSnow()  -> UpdateSnowInterval() walks SnowSteps
 *               -> ChainAfterSnow() picks Fog / Exit based on flags
 *   StartFog()   -> UpdateFogInterval() walks FogSteps
 *               -> calls RequestGameExit() on completion
 *
 * All weather comes from the Ultra Dynamic Weather / Ultra Dynamic Sky actors placed in the level.
 * Each step is pushed through ApplyWeatherStep(), which writes UDW's per-value "Manual Weather State"
 * and flips the matching "<X> - Manual Override" booleans so those values win over UDW's own
 * simulation. UDS then derives clouds, sky colour, sun scattering and volumetric fog from the same
 * state, which is what keeps the imagery physically coherent.
 *
 * ActiveCaptureKind / ActiveCaptureStep / ActiveCaptureTimeOfDay are buffered when a step is applied,
 * so the delayed snapshot callback records exactly what was applied regardless of timer jitter.
 */
UCLASS(Config = Game)
class WEATHERPIPELINE_API AMyActor_weather : public AActor
{
	GENERATED_BODY()

public:
	AMyActor_weather();

protected:
	virtual void BeginPlay() override;

public:
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	// Set in the Blueprint editor; auto-found at BeginPlay if not assigned
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	ACharacter* TargetThirdPersonCharacter;

	// Scene capture component; used to render to texture and read pixel data
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Camera")
	USceneCaptureComponent2D* Capture;

	// --- Per-type parameter sweeps: one entry per capture step ---
	// Filled by BuildDefaultWeatherSteps() at BeginPlay when left empty. Override the defaults in
	// C++ (the readable table in that function), in the Details panel, or per-project in
	// Config/DefaultGame.ini under [/Script/WeatherPipeline.MyActor_weather].
	UPROPERTY(EditAnywhere, Config, Category = "Weather|UDW")
	TArray<FUDWWeatherStep> RainSteps;

	UPROPERTY(EditAnywhere, Config, Category = "Weather|UDW")
	TArray<FUDWWeatherStep> SnowSteps;

	UPROPERTY(EditAnywhere, Config, Category = "Weather|UDW")
	TArray<FUDWWeatherStep> FogSteps;

	// Force UDW to apply a state change immediately instead of interpolating toward it, so a
	// screenshot taken shortly after a step shows that step and not a blend of two.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|UDW")
	bool bForceInstantWeatherChange = true;

	// Log every settable property and callable function on UDW/UDS once at BeginPlay. This dump is
	// the authoritative, version-correct list to reconcile the pipeline's hard-coded names against.
	// Off by default because it is roughly 1500 log lines — turn it on for a single run after
	// upgrading Ultra Dynamic Sky, fix any "not found" warnings it reveals, then turn it back off.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|UDW")
	bool bLogUDWProperties = false;

	// Pin the weather to the drone: switch UDW's control point to "Custom Location" and feed it the
	// character's position every frame, so particles are always spawned in a bubble around the
	// character rather than wherever UDW's own camera lookup lands. On besides being the behaviour we
	// want, this is the only reliable option in Simulate In Editor, which has no player camera at all.
	// The size of that bubble is FUDWWeatherStep::SpawnRadius.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|UDW")
	bool bDriveControlPointFromCharacter = true;

	// --- Weather presets (Ultra Dynamic Sky) ---
	// UDS ships preconfigured weather states as Blueprint assets under
	// Blueprints/Weather_Effects/Weather_Presets. Each one carries a *coherent* set of values -
	// rain together with the cloud coverage, fog and wetness that belong with it - which is exactly
	// what hand-authored numbers kept getting subtly wrong. The preset defines the full-intensity
	// end of each sweep, and the lighter steps are scaled down from it.

	// Build the sweeps from the UDS presets. When false, or when a preset fails to load, the
	// hand-authored table in BuildDefaultWeatherSteps() is used instead.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Presets")
	bool bBuildStepsFromPresets = true;

	// Each phase walks its chain of presets from clear sky to the extreme, blending between adjacent
	// entries. The keyframes are UDS's own presets, so every intermediate state is a mix of two
	// authored, internally consistent weather states rather than an invented one - that is what makes
	// the progression read as real. The LAST entry is the extreme, and it is what the litmus shows.
	// Each chain starts already overcast - the sky one beat before the weather breaks - rather than
	// at clear skies. Starting from Clear_Skies spent the opening steps on an empty blue sky with
	// nothing happening, which is neither interesting to look at nor useful as training data.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Presets")
	TArray<FString> RainPresetChain = {
		TEXT("/Game/UltraDynamicSky/Blueprints/Weather_Effects/Weather_Presets/Overcast.Overcast"),
		TEXT("/Game/UltraDynamicSky/Blueprints/Weather_Effects/Weather_Presets/Rain_Light.Rain_Light"),
		TEXT("/Game/UltraDynamicSky/Blueprints/Weather_Effects/Weather_Presets/Rain.Rain"),
		TEXT("/Game/UltraDynamicSky/Blueprints/Weather_Effects/Weather_Presets/Rain_Thunderstorm.Rain_Thunderstorm")
	};

	UPROPERTY(EditAnywhere, Config, Category = "Weather|Presets")
	TArray<FString> SnowPresetChain = {
		TEXT("/Game/UltraDynamicSky/Blueprints/Weather_Effects/Weather_Presets/Overcast.Overcast"),
		TEXT("/Game/UltraDynamicSky/Blueprints/Weather_Effects/Weather_Presets/Snow_Light.Snow_Light"),
		TEXT("/Game/UltraDynamicSky/Blueprints/Weather_Effects/Weather_Presets/Snow.Snow"),
		TEXT("/Game/UltraDynamicSky/Blueprints/Weather_Effects/Weather_Presets/Snow_Blizzard.Snow_Blizzard")
	};

	// Foggy itself reads cloud=0, so the fog build-up starts from Cloudy and the overcast thins as
	// the fog thickens - which is what the presets specify, and how ground fog actually behaves.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Presets")
	TArray<FString> FogPresetChain = {
		TEXT("/Game/UltraDynamicSky/Blueprints/Weather_Effects/Weather_Presets/Cloudy.Cloudy"),
		TEXT("/Game/UltraDynamicSky/Blueprints/Weather_Effects/Weather_Presets/Foggy.Foggy")
	};

	// Captures per phase. This is also the resolution of the ramp: more steps means smaller jumps
	// between states and a smoother-looking build-up, at one more image each.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Presets")
	int32 StepsPerPhase = 10;

	// Override the preset's particle look and fog range with values tuned for a high aerial camera.
	//
	// OFF by default, and that is the important part: UDW derives particle count, drop scale and fog
	// falloff from the weather state the preset defines, and those derived values ARE the preset's
	// look. Writing our own on top is what made the render stop resembling the preset. Turn this on
	// only if the faithful preset look proves too subtle from the drone's altitude, and expect it to
	// diverge from the preset by design.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Presets")
	bool bOverrideLookForAerialView = false;

	// Ramp from the previous step to the next in small increments rather than jumping straight to it.
	//
	// UDW has its own easing, but the pipeline defeats it: forcing the state update chain every step
	// makes UDW adopt the new state immediately, which is precisely what produces the visible snap
	// from Overcast to Rain_Light. Driving the interpolation here keeps it deterministic - and keeps
	// the capture exact, because the ramp always finishes on the target step before the shutter.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Presets")
	bool bInterpolateBetweenSteps = true;

	// How long one step-to-step ramp takes. Each step's settle time is extended by this, so the
	// capture still happens after the weather has arrived.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Presets")
	float TransitionSeconds = 5.0f;

	// Increments per second during a ramp. Higher is smoother and costs one UDW state update each -
	// the update chain is not free, so this trades frame time for smoothness.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Presets")
	float TransitionUpdatesPerSecond = 10.0f;

	// Let UDW ease between steps within a phase instead of snapping. The phase *boundaries*
	// (rain -> snow -> fog) stay hard cuts either way - only the build-up inside a phase is eased.
	// Each capture still waits for SettleSeconds and is verified against the live state, so a frame
	// is never taken mid-blend.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Presets")
	bool bSmoothWithinPhase = true;

	// --- Matched capture run (conditions x waypoints) ---

	// Use UDW's own Change Weather(preset, seconds) instead of pinning individual state channels.
	// This is the API the presets were authored for, so clouds, sun, particles and material wetness
	// all transition together through UDW's own system rather than being forced channel by channel.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	bool bUseChangeWeatherFunction = true;

	// The weather conditions to capture, in order. Each is flown over the entire path.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	TArray<FWeatherCondition> CaptureConditions;

	// When non-empty, only conditions whose label contains this substring are run. Lets one category
	// be tested in a minute rather than sitting through the whole table - "fog" runs the six fog
	// conditions, "snow" the four snow ones. Clear it to capture the full set again.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	FString ConditionFilter = TEXT("");

	/** Exact condition labels to capture. Empty means all of them.
	 *
	 *  A list rather than ConditionFilter's single substring, because a run usually wants a specific
	 *  set - "rain, snow, foggy" - which no substring expresses. The Python launcher had been writing
	 *  this name for some time before it existed here, so the filter silently did nothing and every
	 *  run captured the whole table. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	TArray<FString> CaptureConditionFilter;

	// Fly the path, capturing at every waypoint. With this off the run stays at the drone's start
	// pose and takes one capture per condition.
	//
	// On, the run becomes conditions x waypoints, so the image count multiplies - 14 conditions over
	// the default 8-waypoint orbit is 112 captures, not 14. Waypoint k under one condition is the
	// same pose as waypoint k under every other, which is what makes the set matched.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	bool bUseFlightPath = true;

	// The drone's route. Left empty, a default orbit is generated around the character's start
	// position at BeginPlay so the structure runs end to end; replace it with a real route and this
	// becomes a data change rather than a code change.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	TArray<FCaptureWaypoint> FlightPath;

	// --- Random jump and capture -------------------------------------------------------------------
	// SpotCount camera spots, each drawn at random inside these limits: a position in the square
	// around the start (the location, or where P was pressed), a height above the ground, a pitch,
	// a yaw, a roll and a time of day. Every condition is captured at the same spots, so the images
	// pair across weathers. weather_pipeline.py writes all of these per location.

	/** How many camera spots to draw. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run", meta = (ClampMin = "1"))
	int32 SpotCount = 1;

	/** The square the spots are spread over, km2, centred on the start. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run", meta = (ClampMin = "0"))
	float SpotAreaKm2 = 1.0f;

	/** The same seed draws the same spots every run; negative = new spots every run. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	int32 SpotSeed = 1;

	/** Height above the ground under each picture, m. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	float SpotHeightMinM = 50.0f;
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	float SpotHeightMaxM = 50.0f;

	/** Pitch, deg: 0 level, negative looks down. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	float SpotPitchMinDeg = -15.0f;
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	float SpotPitchMaxDeg = -15.0f;

	/** Yaw, deg (Unreal yaw): drawn from SpotYawStartDeg through SpotYawSpanDeg more, so a range across
	 *  north (e.g. compass 300 to 60) needs no special case. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	float SpotYawStartDeg = 0.0f;
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run", meta = (ClampMin = "0", ClampMax = "360"))
	float SpotYawSpanDeg = 360.0f;

	/** Roll, deg: tilts the horizon. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	float SpotRollMinDeg = 0.0f;
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	float SpotRollMaxDeg = 0.0f;

	/** Time of day, HHMM. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	float SpotTimeMin = 1200.0f;
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	float SpotTimeMax = 1200.0f;

	/** What is captured at each spot: the name of a shot class ("orbit" is UOrbitCaptureShot) - see
	 *  CaptureShots.h for the shots and how to add your own. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	FString CaptureShot = TEXT("frame");

	/** The shot's parameters by name, e.g. radius_m and pictures for an orbit. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	TMap<FName, float> ShotParameters;

	/** A spot in or against a building, facing a wall or over no ground is drawn again, up to this
	 *  many times; after that it is raised clear instead, so a run can never stall on one spot. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run", meta = (ClampMin = "0"))
	int32 MaxSpotRedraws = 50;

	// Handed to Change Weather as the transition time.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	float ConditionTransitionSeconds = 3.0f;

	// Extra dwell after the transition, before the condition's capture. Ground accumulation and
	// material wetness keep developing after the sky has finished changing, so this is not dead time;
	// dropping it to zero would capture wet-looking roads before they had finished wetting.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	float ConditionSettleSeconds = 5.0f;

	// Hold the shutter until UDW's live state actually matches the condition, rather than trusting a
	// fixed delay. The sky finishes transitioning before material wetness and snow accumulation do,
	// and Cesium finishes streaming on its own schedule - so any single delay is wrong for something.
	// Gating on the real state is what keeps the image and its metadata describing the same scene.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	bool bWaitForWeatherSettled = true;

	// Upper bound on that wait. On timeout the capture proceeds and logs a Warning naming the worst
	// remaining difference, so a frame that does not match its labels is never silent.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	float WeatherSettleTimeoutSeconds = 25.0f;

	// How close the live state must be to the target, on the 0..10 UDS scale, to count as settled.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	float WeatherSettleTolerance = 0.25f;

	/** Previous live weather sample, for deciding whether the transition has finished.
	 *
	 *  Settling is judged by comparing consecutive readings rather than by matching the requested
	 *  channels: UDW generates the weather from a preset and lands near those numbers rather than on
	 *  them, so a target comparison never converged and every capture waited out the full timeout.
	 *  Mutable because the check is const and has to remember the last sample. */
	mutable FUDWWeatherStep LastSettleSample;
	mutable bool            bHaveSettleSample = false;

	// --- Quick litmus: eyeball every condition without running a capture --------------------------
	//
	// The litmus originally previewed three presets (rain / snow / fog) as a sanity check before a
	// capture run. Repointed here at the whole capture table, it becomes the fast way to judge a
	// tuning change - fog density, falloff, colour source - across every condition in well under a
	// minute, instead of running a capture and reading images afterwards.

	/** P runs the litmus preview instead of going straight into the capture run. A second press
	 *  cancels either, through the same path. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Litmus")
	bool bStartWithLitmus = false;

	/**
	 * On Play, preview every capture condition at the drone's CURRENT position - no spawn move, no
	 * flight path, no terrain or sky adjustment - then wait for P. P (or pressing it mid-preview)
	 * starts the real run, which is when the scene is set up. Headless runs (-WeatherAutoStart)
	 * skip the preview.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Litmus")
	bool bLitmusOnPlay = true;

	/** The running litmus was started by Play: stop after one pass instead of chaining into a run. */
	bool bLitmusPreviewOnly = false;
	bool bLitmusOnPlayScheduled = false;

	/** Preview every entry in CaptureConditions rather than the three-preset summary list. Built
	 *  after ConditionFilter is applied, so the preview matches what a run would actually record.
	 *  Ignored if LitmusConditions has been populated by hand. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Litmus")
	bool bLitmusPreviewsAllConditions = true;

	/** Hold per condition, seconds. 17 conditions at 2.0 plus gaps is roughly 38s for a full pass. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Litmus")
	float LitmusStepSeconds = 2.0f;

	/** Blank gap between conditions, seconds - it proves each effect switches off cleanly rather
	 *  than bleeding into the next. 0 cuts straight from one condition to the next. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Litmus")
	float LitmusGapSeconds = 0.25f;

	/** Continue into the full capture run once the preview finishes.
	 *
	 *  On: P gives one pass - a quick visual check of every condition, then the real capture run.
	 *  The preview doubles as a pre-flight, so a broken weather apply shows up in seconds rather
	 *  than after the run has written its first batch of images.
	 *
	 *  Turn off when using the litmus purely to judge a tuning change, so it stops after the
	 *  preview instead of writing images nobody asked for. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Litmus")
	bool bLitmusChainsToCapture = true;

	// Weather shown by the litmus, in order, before the capture run starts. Each is applied as a hard
	// cut, held, then cleared. Snow uses the mid-strength preset with the wind pinned down, because
	// Snow_Blizzard's wind=10 turns the flakes into horizontal streaks that read as rain.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	TArray<FWeatherCondition> LitmusConditions;

	// Dwell after moving to a waypoint, on top of the Niagara warm-up. Kept at a couple of seconds
	// rather than a fraction of one: the warm-up fills the particle volume, but splashes, ripples and
	// the wetness/snow material response still develop over their own timers, and none of that is
	// visible to the settle gate - which only compares weather *state* values.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	float WaypointSettleSeconds = 2.0f;

	// Keep the cloud deck and the fog layer at a fixed height above the drone, instead of at a fixed
	// world altitude.
	//
	// UDW's particle spawn box is already relative to the control point - its own tooltip says the
	// height is measured "above the player camera" - so rain and snow follow the drone to any
	// coordinates. Clouds and fog are not: their altitudes are absolute world Z, so moving the
	// Cesium georeference to higher terrain leaves the drone above the weather, which is exactly how
	// a run over mountains produced a clear sky with the cloud deck below the camera.
	//
	// Trade-off: with this on you can never fly above the clouds, because they climb with you. That
	// is right for a dataset (every capture gets a consistent sky) and wrong for a drone actually
	// gaining altitude, which is why it is a flag rather than unconditional.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	bool bAnchorWeatherToDrone = true;

	// All cloud and fog distances below are in METRES, so they can be reasoned about against the
	// altitude and height-above-ground figures written into the metadata and the calibration log.

	/** Height of the cloud base above the drone (m). Used when the mode is AboveDrone. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float CloudHeightAboveDroneM = 1500.0f;

	// What the cloud base is measured from.
	//
	// RealWorldAltitude is the default: the deck sits at a fixed altitude on the globe and stays
	// there, so flying up, down or across the map moves the drone relative to the clouds rather than
	// dragging the clouds along. That is how a real cloud base behaves, it is the only mode whose
	// number means the same thing at every location, and it is what the recorded altitude metadata
	// can be checked against.
	//
	// AboveDrone pins the layer a fixed distance overhead, so it can never be flown through or seen
	// from above, and the deck follows the camera everywhere. Useful for guaranteeing clouds are in
	// frame, wrong for anything that has to look geographically real.
	//
	// AboveGround takes its reference from whatever is directly beneath the drone, so across
	// mountains the base rises and falls by a kilometre between valleys and ridges and the layer ends
	// up threaded between the peaks. It is the better choice over flat ground, where the reference
	// barely moves and "500 m up" is exactly what is wanted.
	//
	// Per-condition overrides still win: moving_fog sets bCloudHeightFromGround, so its valley fog
	// stays ground-referenced whatever this is set to.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	ECloudAltitudeMode CloudAltitudeMode = ECloudAltitudeMode::UsePreset;

	/** Cloud base height above the terrain (m). Used by both ground-referenced modes. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float CloudHeightAboveGroundM = 1200.0f;

	/** Cloud base as a real-world altitude in metres above **mean sea level**, which is how a cloud
	 *  base is quoted in a weather report or on an aviation chart. Converted to the ellipsoid through
	 *  the EGM96 grid, the same path the altitude readout and the fog plane use.
	 *
	 *  Geo-referenced rather than terrain-following, and deliberately so: a cloud base is a property
	 *  of the atmosphere, not of the ground under it. Unlike the fog plane there is no argument for
	 *  tying it to terrain - real cloud decks sit at an altitude and mountains intersect them.
	 *
	 *  Used when the mode is RealWorldAltitude. Read the ground and drone altitudes off the "Height calibration" log line
	 *  and set this above the summits for a deck over the range, or between valley floor and summit
	 *  for cloud threading through the peaks. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float CloudAltitudeM = 1500.0f;

	/** How far down to look for the ground when measuring terrain height (m). Needs to exceed the
	 *  drone's height above terrain, which over mountains can be several kilometres. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float GroundTraceDistanceM = 20000.0f;

	// --- Moving fog (UDW "Post Process Wind Fog") ---
	// Height fog is a static gradient: it thickens with distance but never moves. UDW's Post Process
	// Wind Fog renders 3D noise that drifts with the wind, which is what gives fog the rolling,
	// cloud-like motion seen on hillsides. It scales with the Fog and Wind channels of the weather
	// state, so it strengthens automatically on the foggy and stormy conditions.

	UPROPERTY(EditAnywhere, Config, Category = "Weather|Fog")
	// Off globally. This switched UDW's Post Process Wind Fog on at BeginPlay for the whole session,
	// and PPWF is a screen-space effect scaled by the weather's wind and fog - so it stayed silent
	// through 'clear' (no wind, no fog to drive it) and appeared the moment a preset with wind ran,
	// which is exactly the haze that arrived at partly_cloudy and distorted the frame.
	//
	// It also explains why it survived leaving the pipeline: nothing restores it, so it persisted
	// until a condition with no wind happened to scale it back to nothing. Diagnosed by reading every
	// fog channel side by side - PPWF was at 1.50 while every other channel sat at zero.
	//
	// The moving_fog condition sets its own MovingFogIntensity, so the effect is still available
	// where it is actually wanted, driven per condition rather than left on globally.
	bool bEnableMovingFog = false;

	/** Overall strength of the effect. 0 leaves UDW's own value. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Fog")
	float MovingFogIntensityScale = 1.5f;

	/** How much the moving fog responds to the Fog channel of the weather state. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Fog")
	float MovingFogIntensityFromFog = 1.0f;

	/** How much it responds to wind. This is what makes the fog visibly drift rather than sit. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Fog")
	float MovingFogIntensityFromWind = 1.0f;

	/** Size of the drifting noise features; UDW ships 2000. Raised for broader banks with clearer
	 *  gaps between them - at the default the variation is fine-grained enough to average out into
	 *  an even haze at city distances rather than reading as distinct patches. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Fog")
	float MovingFogNoiseScale = 3500.0f;

	/** How far the noise is sampled (cm); UDW ships 1800, which is under 20 m - the effect was
	 *  confined to the immediate foreground. Extended so the fog reaches into the scene. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Fog")
	float MovingFogMaxSampleDistance = 5000.0f;

	/** Spacing between noise samples (cm); UDW ships 120. Raised in step with the sample distance so
	 *  the sample *count* stays near UDW's ~15 - extending the range at the shipped step would have
	 *  tripled the per-pixel cost of a post-process effect for no extra visible structure. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Fog")
	float MovingFogSampleStepDistance = 340.0f;

	// Ultra Dynamic Sky's "Base Fog Density" - the height fog present before any weather adds to it.
	//
	// This is coupled to HeightFogFalloffClear/Cloudy below and has to be tuned with them, not
	// independently. Density sets how thick the fog is at the fog plane; falloff sets how fast that
	// thins with height. Raising falloff alone - which is what thins the horizon band - also strips
	// the haze out of everything the drone looks down at, leaving a flat, depthless scene. Raising
	// density back up restores that depth without putting the band back, because the extra density
	// is confined to low altitude by the steep falloff.
	//
	// So: dense but shallow, rather than thin but endless. That is also closer to how real haze
	// behaves, sitting in the boundary layer rather than spread evenly through the column.
	//
	// History: UDS's own value measures 0.0228 in the mountain level. 0.002 was chosen back when the
	// falloff was still UDS's default, where anything higher read as a permanent grey haze over a
	// city-scale view from altitude. With falloff at 2.5 that constraint is gone, so this returns to
	// roughly UDS's own figure. <= 0 leaves UDS's value untouched.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	// 0.002 is the tuned value. It was briefly raised to 0.02 to compensate for a height fog falloff
	// of 2.5; that falloff has since been reverted to UDS's own 0.065 and the compensation was left
	// behind, which is what made partly_cloudy read as heavy haze - ten times the intended density
	// against the original falloff.
	// UDS's own authored value, read out of the level: Base Fog Density = 0.022773.
	//
	// This is NOT the density that reaches the fog component. UDS multiplies it by Scale Fog Density
	// (0.2 in this level), so 0.022773 x 0.2 = 0.0045546 - exactly the component's fog_density. Writing
	// 0.004555 here, as if this field were the component's, would have landed at 0.000911: five times
	// too thin, and the reason every earlier density and falloff experiment looked identical.
	float BaseFogDensity = 0.022773f;

	// --- Live altitude readout --------------------------------------------------------------------
	//
	// A continuous report of where the drone actually is, for checking the height maths by flying
	// rather than by reading a log after the fact. Two independent numbers, because they fail in
	// different ways: the altitude comes from the Cesium georeference and is wrong if the
	// georeference is misconfigured; the height above ground comes from a physics trace and is wrong
	// if collision is missing or the tiles are re-streaming. Seeing both at once says which.
	//
	// Suppressed once a run starts, so the capture log stays readable.

	// --- Vertical datum ---------------------------------------------------------------------------
	//
	// Cesium reports height above the WGS84 *ellipsoid*. Published elevations - maps, Wikipedia,
	// aviation charts, a barometric altimeter - are *orthometric*, above mean sea level, which
	// follows the geoid. The two differ by the local geoid undulation, and Cesium's own header warns
	// about the confusion: the sampled height "should not be confused with a height above mean sea
	// level".
	//
	// Over Bavaria the undulation is about +45 to +48 m, which is exactly the gap between Cesium's
	// reported ~421 m at Ingolstadt and the published ~376 m. Nothing is wrong with the terrain or
	// the georeference; the number simply answers a different question.
	//
	// It matters because a recorded altitude of 421 m read as MSL is wrong by 45 m, and nothing
	// downstream can detect that unless the datum is stated. Relative figures - height above ground -
	// are unaffected, since both endpoints share whatever datum is in use.
	//
	// A single constant is accurate here: geoid undulation varies by centimetres per kilometre, so
	// across one tileset it is effectively fixed. It is NOT valid if the georeference is moved to
	// another part of the world - look the value up again for the new location.

	/** Fallback separation in metres, used only when no geoid grid is loaded. 0 reports raw
	 *  ellipsoid height. Superseded by GeoidGridPath, which is exact everywhere. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Debug")
	float GeoidSeparationM = 0.0f;

	/** EGM96 geoid grid (NGA's WW15MGH.GRD). Empty uses Content/Geoid/WW15MGH.GRD.
	 *
	 *  With this loaded the separation is looked up per position instead of assumed, so altitudes
	 *  are correct above mean sea level anywhere on the globe - the constant above is only ever
	 *  right near the one place it was calibrated for. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Debug")
	FString GeoidGridPath;

	// Parsed grid. Rows run north to south, columns west to east, values in metres.
	// Mutable because the load is lazy and the lookup is logically a pure query - callers that only
	// read a height should not have to be non-const to trigger a one-off file read.
	mutable TArray<float> GeoidGrid;
	mutable int32  GeoidRows = 0;
	mutable int32  GeoidCols = 0;
	mutable double GeoidNorth = 0.0;
	mutable double GeoidWest  = 0.0;
	mutable double GeoidDLat  = 0.0;
	mutable double GeoidDLon  = 0.0;
	mutable bool   bGeoidLoadAttempted = false;

	/** Loads the grid once per session. Returns false when unavailable, leaving the constant in use. */
	bool LoadGeoidGrid() const;

	/** Geoid undulation at a position, in metres: ellipsoid height minus this is height above mean
	 *  sea level. Falls back to GeoidSeparationM when no grid is loaded. */
	float GetGeoidSeparationAt(double LatitudeDeg, double LongitudeDeg) const;

	// --- Ground elevation without measurement -----------------------------------------------------
	//
	// The drone's altitude above mean sea level is exact arithmetic - Cesium's ellipsoid height minus
	// the geoid undulation - with no trace, no collision and no tiles involved. So height above
	// ground needs only one further number: the elevation of the ground itself. That is a property of
	// the site, not something to re-measure every frame.
	//
	// Deriving it this way is strictly better than tracing for imagery. A trace hits whatever is
	// directly below, which over a city is a rooftop: the same stationary drone logged "terrain
	// below" ranging 418.8 to 433.6 m over flat Ingolstadt, a 15 m error purely from what happened to
	// be underneath. A site elevation cannot do that - AGL is always measured from the ground, which
	// is what a drone altimeter reports and what the dataset should record.
	//
	// It also removes every dependency that made the trace fragile: collision meshes, streamed tiles,
	// the smooth-normals rebuild that produced the "no ground found" warnings, and Gaussian splats
	// having no collision at all.
	//
	// The assumption is that terrain is reasonably flat across the capture area. True for Ingolstadt
	// (362-411 m over the whole city). Over real relief it is not, and the trace remains available.

	// --- Height fog plane ------------------------------------------------------------------------
	//
	// UE's exponential height fog measures from the fog component's world Z (SceneCore.cpp:398,
	// "FogData[0].Height = InComponent->GetComponentLocation().Z"). UDS leaves that component on its
	// own actor at the world origin, so the fog's reference is world Z = 0 - a number with no
	// relationship to terrain anywhere on the globe.
	//
	// At Ingolstadt the georeference happens to put ground near Z = 0, so the fog looks right by
	// coincidence. Move to a sea-level site and the terrain sits well below the plane; since density
	// goes as exp(-falloff * (h - fogHeight)), ground *below* the plane gets exponentially denser
	// fog. That is the "too thick in Singapore" report, and it is a datum bug, not a tuning problem.
	//
	// Anchoring the plane to a real-world altitude fixes it everywhere at once, using the same geoid
	// lookup as the altitude readout.

	/** Place the fog plane at a real-world altitude instead of leaving it at world Z = 0. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	// Off. The reasoning behind this was wrong: it assumed UDS parks its fog component at ground
	// level, so that re-anchoring it to sea level would be a small correction. The log shows
	// otherwise - UDS puts the component on the SkyAtmosphere's planet surface, roughly 1800 m below
	// the terrain in a Cesium world, and anchoring moved it a further 436 m down at Singapore rather
	// than the ~27 m the datum change should have implied.
	//
	// Until that discrepancy is understood, UDS's own plane is the one every fog value here was
	// tuned against, and leaving it alone is the only setting known to be correct.
	bool bAnchorFogPlaneToAltitude = false;

	/** Keep the fog plane at the site's ground elevation rather than at a fixed altitude.
	 *
	 *  Off by default. Following the ground sounds right but inherits every weakness of measuring
	 *  terrain: it needs tiles streamed, collision present and a good seed, and a bad measurement
	 *  moves the fog plane hundreds of metres - which is what a -40 m ground reading did to the fog.
	 *
	 *  The geoid surface has none of those failure modes. It is defined everywhere, computed purely
	 *  from the georeference and the EGM96 grid, available on the first frame, and identical between
	 *  runs. It is also the physically correct reference: atmospheric haze sits in the boundary layer
	 *  above sea level, which is why high terrain rises out of it. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	bool bFogPlaneFollowsSiteGround = false;

	/** Height of the fog plane above the site's ground, in metres. 0 puts the fog base at ground
	 *  level, which is the look already tuned here. Negative sinks it below ground, thinning the
	 *  fog seen from above without changing its density. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float FogPlaneOffsetM = 0.0f;

	/** Fog plane altitude in metres above mean sea level - the geoid surface at 0.
	 *
	 *  This is the active setting by default. It resolves through the same EGM96 lookup as the
	 *  altitude readout, so it is exact anywhere on the globe with no terrain involved at all.
	 *
	 *  Consequence worth knowing: fog thickness now varies with site elevation, as it does in
	 *  reality. Singapore at ~14 m sits almost entirely within the layer; Ingolstadt at ~376 m sits
	 *  well up it and will read noticeably clearer than before, when the plane happened to sit at
	 *  ground level. If that reads as too thin, raise BaseFogDensity rather than lifting the plane -
	 *  the plane is the datum, the density is the tuning. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float FogPlaneAltitudeM = 0.0f;

	/** Moves the fog component to FogPlaneAltitudeM. Implemented in GeoidModel.cpp. */
	void AnchorFogPlane();

	// --- Bare-earth terrain ------------------------------------------------------------------------
	//
	// The scientifically correct source for ground elevation, and the simplest. Cesium World Terrain
	// is a digital elevation model: buildings are not in the dataset at all, so a trace against it
	// returns terrain by definition rather than by inference. No percentile, no assumption that
	// terrain is flat, correct per position over any relief - and it can be checked against published
	// spot heights, which a statistical estimate cannot honestly be.
	//
	// Add "Cesium World Terrain" (ion asset 1) to the level, tick Hidden in Game, leave physics on.
	// It streams cheaply - heightmaps, no textures - alongside the photorealistic tileset.
	//
	// When absent, the site-elevation and trace paths below still apply, so a level without it
	// behaves exactly as before.

	// --- Spherical sky atmosphere ------------------------------------------------------------------
	//
	// UDS anchors its SkyAtmosphere with PlanetTopAtAbsoluteWorldOrigin - a tangent plane at the
	// world origin, which every log records as "atmosphere ground Z=-180492cm", a fixed world Z.
	//
	// UE's volumetric clouds are positioned as LayerBottomAltitude above that planet surface and
	// curve with it, so they are following the planet faithfully; the planet is simply flat. Cesium
	// renders a real globe, so the terrain's horizon drops with distance while the cloud deck stays
	// level - which is why the clouds read as a flat ceiling that never meets the horizon.
	//
	// ACesiumSunSky solves this by placing its atmosphere at the centre of the Earth with
	// TransformMode = PlanetCenterAtComponentTransform and a ground radius taken from the ellipsoid.
	// This applies the same three settings to UDS's own atmosphere.
	//
	// Off by default: it changes the sky's fundamental frame of reference, and an earlier attempt at
	// this was reverted. The suspected cause of that failure was applying it per step while UDS
	// repositions its sky each refresh, so the two fought - it is now applied once, after the update
	// chain, which is the ordering that fixed the particle spawn, the cloud altitude and the
	// lightning suppression.

	/** Re-anchor UDS's SkyAtmosphere to the globe so clouds curve with the Earth and meet the
	 *  horizon. Also corrects the flat-atmosphere horizon band. */
	//  Off: tested against UDS's own flat anchor and the remaining separation between the cloud deck
	//  and the horizon was small enough not to be worth changing the sky's frame of reference for.
	//  Left in place because it works and is one flag away if the flat horizon ever becomes a problem.
	//
	//  Note the actor is spawned by WeatherPipelineGameMode rather than placed, so there is no level
	//  instance whose Details panel can enable this - it has to be set here or in DefaultGame.ini.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	bool bCurveSkyAtmosphereToGlobe = false;

	/** Applies the spherical anchor. Implemented in GeoidModel.cpp beside the other georeferenced
	 *  placement maths. */
	void AnchorSkyAtmosphereToGlobe();

	// --- Georeference origin -----------------------------------------------------------------------
	//
	// Where on Earth the level sits. This is a property of the CesiumGeoreference actor, not of this
	// one, so it used to be unreachable from configuration - you could set SkyLatitude/SkyLongitude
	// and move only the sun, leaving the correct sun over the wrong city. These three close that gap
	// so a single config file fully describes a run.
	//
	// Off by default: a level that is already georeferenced correctly should not be moved by merely
	// installing this plugin. The Python launcher turns it on, because there the whole point is that
	// the file is authoritative.

	/** Move the level's CesiumGeoreference to the coordinates below on BeginPlay.
	 *
	 *  Off, and it should stay off unless the level is built to be relocated. Sliding the globe under
	 *  a level authored around a fixed origin leaves the drone underground with no terrain in front
	 *  of it and a blurred sky - the camera ends up below the atmosphere floor, which reads as a
	 *  rendering fault rather than the placement problem it is. SkyLatitude/SkyLongitude still move
	 *  the sun without this, which is what a level already georeferenced to its site actually needs. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site")
	bool bSetGeoreferenceOrigin = false;

	/** Latitude the level is anchored to (deg). Used only when bSetGeoreferenceOrigin is true. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site")
	double GeoreferenceOriginLatitude = 48.7665;

	/** Longitude the level is anchored to (deg, west negative). */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site")
	double GeoreferenceOriginLongitude = 11.4258;

	/** Height of the origin above the WGS84 ellipsoid (m).
	 *
	 *  KeepLevelOriginHeight (the default) reuses whatever height the level was authored with, and
	 *  changes only latitude and longitude. Forcing a height here is almost never what you want:
	 *  writing 0 moved a level authored at ~10.8 km down to the ellipsoid, which put the drone that
	 *  far underground with no terrain in front of it and a blurred sky, because the camera ended up
	 *  below the atmosphere floor. Latitude and longitude are what a site is; the origin height is a
	 *  property of how the level was built. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site")
	double GeoreferenceOriginHeightM = KeepLevelOriginHeight;

	/** Sentinel for "leave the level's own origin height alone". Far outside any real value. */
	static constexpr double KeepLevelOriginHeight = -1.0e9;

	/** Applies the origin above to the level's CesiumGeoreference. Implemented in GeoidModel.cpp. */
	void ApplyGeoreferenceOrigin();

	// --- Spawn height ------------------------------------------------------------------------------
	//
	// Put the drone a fixed distance above the terrain at whatever location the run is at, so a site
	// in Singapore (ground ~15 m) and one in Ingolstadt (ground ~375 m) both start with the same
	// clearance instead of the same absolute Z. Without this, moving the georeference leaves the
	// drone at its authored world height, which is underground at one site and in the stratosphere
	// at another.
	//
	// Measured from the ground rather than computed from the geoid: the geoid gives mean sea level,
	// and what a drone actually needs to clear is the terrain and the buildings on it.

	/** Reposition the drone to SpawnHeightAboveGroundM above the terrain once tiles have loaded. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site")
	bool bSpawnAtHeightAboveGround = true;

	/** Height above the terrain to place the drone at (m). */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site")
	float SpawnHeightAboveGroundM = 50.0f;

	// --- Open air: the camera is never in, against or facing into a building --------------------
	//
	// A pose is clear when (1) nothing is above the camera - it is not inside or under a building,
	// (2) nothing is within CameraClearanceRadiusM - it is not touching one, and (3) fewer than 3 of
	// 5 rays across the view hit something within MinViewDistanceM - it is not facing a wall. Close
	// to a building is fine. A spawn or capture point that is not clear is raised in RaiseStepM steps
	// until it is (at most MaxRaiseM); the metadata records the measured height. Smooth flight checks
	// each leg with the same sphere and goes up, across and down when the straight line would hit.

	/** Radius of the free space the camera needs around it, m. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site", meta = (ClampMin = "0.0"))
	float CameraClearanceRadiusM = 3.0f;

	/** A view whose rays mostly hit something closer than this is blocked, m. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site", meta = (ClampMin = "0.0"))
	float MinViewDistanceM = 30.0f;

	/** How far a blocked pose (or leg) is raised per try, and at most, m. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site", meta = (ClampMin = "1.0"))
	float RaiseStepM = 10.0f;

	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site", meta = (ClampMin = "0.0"))
	float MaxRaiseM = 400.0f;

	enum class EPoseBlock : uint8 { None, UnderSurface, Touching, ViewBlocked };
	static const TCHAR* PoseBlockText(EPoseBlock Block);

	/** Which of the three open-air rules the pose breaks first, or None. Implemented in GeoidModel.cpp. */
	EPoseBlock CheckPose(const FVector& Location, const FRotator& Rotation) const;

	/** Raises Location until CheckPose passes. OutFirst is what blocked the original pose; OutRaisedM
	 *  how far it went up. False when still blocked at MaxRaiseM. */
	bool RaiseUntilClear(FVector& Location, const FRotator& Rotation, EPoseBlock& OutFirst, double& OutRaisedM) const;

	/** A CameraClearanceRadiusM sphere can move from From to To without touching anything. */
	bool IsSegmentClear(const FVector& From, const FVector& To) const;

	/** The points to fly through from From to To, To excluded: none when the straight line is clear,
	 *  else up above From, across, and down onto To. */
	TArray<FVector> PlanLeg(const FVector& From, const FVector& To) const;

	/** Position a fraction Alpha (0..1) of the way along From -> Points, by distance. */
	static FVector PointAlongPath(const FVector& From, const TArray<FVector>& Points, double Alpha);

	/** Fish-eye geometry for CaptureFOV at CaptureResolutionX x Y: the normal view's horizontal FOV to
	 *  render, and how much larger than the image to render it. Pure, so it can be tested. */
	static void FisheyeRender(float FisheyeFovDeg, int32 Width, int32 Height, float& OutRenderFovDeg, float& OutRenderScale);

	/** Applies the spawn height. Self-disabling: does nothing until the ground trace is trustworthy,
	 *  then runs once per site. Implemented in GeoidModel.cpp. */
	void ApplySpawnHeight();

	/** Cleared whenever the globe moves, so each site gets its own placement. */
	bool bSpawnHeightApplied = false;

	/** Set when the globe moves: the drone has to be brought back over the new origin before its
	 *  height is measured, or it measures terrain at wherever it happened to be left. */
	bool bRecentreDroneOnSite = false;

	/** Seconds spent waiting for a ground trace to succeed before falling back to the origin. */
	float SpawnFallbackElapsed = 0.0f;

	/**
	 * Measure the ground at spawn and move the georeference's Origin Height onto it, so Unreal Z = 0
	 * is the ground wherever the coordinates point - no per-city ground height to look up and type in.
	 *
	 * The origin height only has to be roughly right (UDS places its height fog and atmosphere in
	 * Unreal space, relative to Z = 0); every altitude the pipeline records goes through the Cesium
	 * conversion and is correct either way. The drone is then placed relative to the ground measured
	 * after the shift, so a residual of a few metres costs nothing.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site")
	bool bCalibrateOriginToGround = true;

	/** World time the origin was shifted, or < 0 before. Spawn waits a moment after it so the tiles
	 *  have moved before the ground is measured again - probing too early would read the old ground
	 *  and a second shift would overshoot. One shift per session. */
	double OriginCalibratedAtSeconds = -1.0;

	/**
	 * Logs where every height-dependent layer sits RELATIVE TO THE MEASURED GROUND: the drone, the
	 * height fog plane, the sky atmosphere's planet surface and the cloud base. These are what decide
	 * haze and cloud look, and they are only comparable between cities (or between a terrain offset and
	 * a fog offset) as heights above the ground. grep "[Scene]". Implemented in GeoidModel.cpp.
	 */
	void LogSceneGeometry(const TCHAR* When) const;

	/**
	 * Calculate the ground's true height at the site from the tileset's own data (Cesium
	 * SampleHeightMostDetailed, metres above the WGS84 ellipsoid) and make it the georeference's
	 * Origin Height, so Unreal Z = 0 is the geo-accurate ground in every city.
	 *
	 * With Z = 0 on the ground, Ultra Dynamic Sky - saved in the level at a fixed Z below 0 - sits the
	 * same distance below the ground everywhere, so the haze it draws is the same in every city.
	 * Lowest of five samples GroundProbeRadiusM apart: the photogrammetry includes buildings, so one
	 * sample over a city can be a roof. Falls back to the collision probe (bCalibrateOriginToGround)
	 * when sampling is unavailable or fails.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site")
	bool bCalculateOriginHeight = true;

	/**
	 * Manual frame: the user flies to a view and presses P; every condition is captured from exactly
	 * that view (the live player camera - the flight path is off). At Play the ground height is
	 * calculated and the drone placed SpawnHeightAboveGroundM above the ground to fly from, before the
	 * user moves, so the terrain never shifts under the chosen frame. Controls are locked from P until
	 * the run ends, so every condition shares the same pose.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	bool bManualFrame = false;

	/** Stop (or restore) the player's move and look input - during a run nothing may move the frame. */
	void SetPlayerInputLocked(bool bLocked);

	bool bGroundCalculationPending = false;
	bool bOriginFromCalculation    = false;

	/** Starts the asynchronous height sampling; FinishGroundHeightCalculation completes it. */
	void StartGroundHeightCalculation();
	void FinishGroundHeightCalculation(const TArray<double>& EllipsoidHeights);

	/** The georeference's origin, read and written through reflection so no Cesium type is needed. */
	bool GetGeoreferenceOrigin(double& OutLatitude, double& OutLongitude, double& OutHeight) const;
	bool SetGeoreferenceOriginHeight(double HeightM);

	/**
	 * The ground measured at spawn (lowest-of-five probe, so not a roof). The ONE reference every
	 * height-dependent layer is measured against - cloud base and the [Scene] log - so they agree with
	 * each other wherever the terrain slopes. A single trace for the cloud base once landed on a 230 m
	 * Sydney tower and lifted the clouds by that much.
	 */
	double SpawnGroundZ = 0.0;
	bool   bHaveSpawnGround = false;

	/**
	 * Scene setup - tileset correction, ground calibration, spawn height, sky anchor - happens when a
	 * run is requested, not at Play. Before that the level is exactly as authored, so the Play preview
	 * shows the editor's look from where the drone was left.
	 */
	bool bSceneSetupRequested = false;
	FTimerHandle SceneSetupHandle;

	/** Starts the scene setup (once) and the capture run as soon as the drone has been placed. */
	void BeginSceneSetupThenRun();
	void WaitForSceneSetupThenRun();

	/** True when Unreal was launched by the pipeline to capture unattended (-WeatherAutoStart). */
	static bool IsHeadlessAutoStart();

	/**
	 * Compass <-> UDW "Wind Direction". In the georeferenced world Unreal +X is east and +Y is south,
	 * so yaw = compass bearing - 90; UDW's value is the direction the wind blows TOWARD, which is the
	 * compass FROM-direction + 180. Together: yaw = from + 90. Verified from the slant of the rain.
	 */
	static float UdwYawForWindFrom(float CompassFromDeg) { return FMath::Fmod(CompassFromDeg + 90.0f + 360.0f, 360.0f); }
	static float WindFromForUdwYaw(float YawDeg)         { return FMath::Fmod(YawDeg - 90.0f + 720.0f, 360.0f); }

	/** The level's own UDW wind direction, restored for every condition that does not set one. */
	bool  bHaveLevelWindDirection = false;
	float LevelWindDirectionYaw   = 0.0f;


	/** Adds DeltaM to the CesiumGeoreference's Origin Height, keeping its latitude/longitude. */
	bool ShiftGeoreferenceOriginHeight(double DeltaM);

	/**
	 * Keep every waypoint SpawnHeightAboveGroundM above the ground directly beneath IT, not above the
	 * ground at the spawn point. With this off the path is flown at one flat altitude, so the clearance
	 * drifts with the terrain - 250 m over the start becomes 150 m over a 100 m rise.
	 *
	 * The ground under each waypoint is probed once, on its first visit, and reused for every other
	 * condition, so the pose repeats exactly across conditions.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site")
	bool bFollowTerrain = true;

	/**
	 * Fly between waypoints instead of teleporting. The drone moves continuously at FlightSpeedMps
	 * along the path; at each waypoint the picture is taken the moment the tiles are ready and the
	 * drone flies straight on. Pictures are triggered by position, not time, and every condition
	 * re-flies the same route, so image N of every condition has the identical pose - pairs stay
	 * matched. Only the first waypoint of a condition is reached by teleport.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Flight Path")
	bool bFlyBetweenWaypoints = false;

	/** UDW "Rain Drops Scale": size of the rain particles. <= 0 leaves the level's value. Not a preset
	 *  channel - the presets say how much it rains, this says how big a drop looks. Recorded per image
	 *  in the metadata (read back from UDW). */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Particles")
	float RainDropScale = -1.0f;

	/** UDW "Snow Flakes Scale": size of the snow particles. <= 0 leaves the level's value. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Particles")
	float SnowFlakeScale = -1.0f;

	/** Ground speed while flying between waypoints, m/s. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Flight Path", meta = (ClampMin = "0.5", ClampMax = "200.0"))
	float FlightSpeedMps = 15.0f;

	/** Seconds to fly DistanceCm at SpeedMps. Never below one short frame, so a zero-length leg still
	 *  completes through the same path as any other. */
	static double GlideSeconds(double DistanceCm, float SpeedMps)
	{
		return FMath::Max(DistanceCm / 100.0 / FMath::Max(SpeedMps, 0.5f), 0.02);
	}

	/** Flight state between two waypoints. */
	bool       bGliding = false;
	FTransform GlideFrom = FTransform::Identity;
	TArray<FVector> GlidePoints;   // the leg after GlideFrom: any detour points, then the waypoint
	double     GlideElapsedSeconds = 0.0;
	double     GlideDurationSeconds = 0.0;

	/** Starts the flight to FlightPath[CurrentWaypointIndex]; TickGlide finishes it and captures. */
	void StartGlideToCurrentWaypoint();
	void TickGlide(float DeltaTime);

	/**
	 * Radius of the ground probe, in metres. The probe takes the LOWEST of five hits - the waypoint
	 * itself and four points this far away - because the photogrammetry mesh includes buildings, and a
	 * single trace over a city lands on a roof. The lowest of five is street level unless all five
	 * points are roofs. 0 = a single trace.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site", meta = (ClampMin = "0.0", ClampMax = "200.0"))
	float GroundProbeRadiusM = 30.0f;

	/**
	 * Move any Cesium3DTileset actor that has been offset from the origin back to it at BeginPlay.
	 *
	 * An offset tileset draws the terrain somewhere other than where the georeference says it is, so
	 * every altitude derived from the georeference is wrong by exactly that offset - a tileset left at
	 * Z = -1525 m produced altitude_m = -901 over a city at +375. Height belongs on the georeference's
	 * OriginHeight, which moves the terrain and the conversion together.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site")
	bool bResetTilesetOffset = true;

	/** Puts offset tilesets back at the origin. Implemented in GeoidModel.cpp. */
	void ResetTilesetOffsets();

	/** True while the snow coverage manual override is bound in UDW, so the next release knows it has
	 *  to rebuild UDW's bindings rather than just clear the bool. */
	bool bSnowCoveragePinned = false;

	/**
	 * World Z of the ground at (X, Y): the lowest hit of the probe pattern described on
	 * GroundProbeRadiusM, each trace running from 50 km above to 50 km below NearZ. False when no
	 * trace hits anything - tiles not streamed, or the tileset has no physics meshes.
	 */
	bool ProbeGroundZ(double X, double Y, double NearZ, double& OutGroundZ) const;

	/**
	 * First visit to a waypoint: probe the ground beneath it and set its Z to ground + clearance.
	 * Returns true when the pose moved, meaning the tiles need another settle before the shutter.
	 */
	bool ResolveWaypointGround(FCaptureWaypoint& Waypoint);

	// --- Multi-site runs ---------------------------------------------------------------------------
	//
	// Leave empty for a single-location run driven by the origin above. With entries, one session
	// visits each in turn: the first is the level's own location (saved there beforehand); for each
	// next one the globe moves, the ground height is calculated and the drone placed exactly as at the
	// first, then every condition is captured into the site's own folder (<Name>) with its own clock
	// and condition list. Manual frame: the drone is placed and the controls unlocked at each site,
	// and the run waits for P again. The game closes only after the last site.

	/** Locations to visit, in order. Empty means a single run at GeoreferenceOrigin*. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site")
	TArray<FCaptureSite> CaptureSites;

	/** Seconds after moving the globe before the next site's setup starts. Short: the setup itself
	 *  waits for the tiles (ground calculation, spawn and the pre-run Cesium wait all gate on them). */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Site")
	float SiteSettleSeconds = 1.0f;

	/** Index into CaptureSites currently being captured. */
	int32 CurrentSiteIndex = 0;

	/** Makes CaptureSites[Index] current: its folder, clock and conditions, and - with bMoveGlobe -
	 *  its location, resetting the per-site setup so the ground is calculated again.
	 *  Returns false when the index is out of range. */
	bool ApplyCaptureSite(int32 Index, bool bMoveGlobe);

	/** Advances to the next site and sets it up for its run.
	 *  Returns false when every site is done, which is what ends the run. */
	bool AdvanceToNextSite();

	/** After the settle wait: automated - set up the scene and run; manual - place the drone and wait
	 *  for P. */
	void BeginRunAtCurrentSite();

	FTimerHandle SiteSettleTimerHandle;

	mutable bool bLoggedSkyAtmosphereAnchor = false;

	/** One line per run when the cloud layer is left to the preset. */
	bool bLoggedPresetCloudAltitude = false;

	/** Leave each preset's own height fog alone instead of pinning density and falloff globally.
	 *
	 *  The same fix as ECloudAltitudeMode::UsePreset, one system across. With this off, BaseFogDensity,
	 *  CloudyFogDensityContribution and the three HeightFogFalloff values are written on every step, so
	 *  all 16 conditions share one fog profile and the fog UDS authored per preset never reaches the
	 *  frame. That is why presets look right in isolation and wrong once the pipeline drives them.
	 *
	 *  Per-condition FogOverride still applies through the weather state, so a condition can still ask
	 *  for more or less fog - it just no longer replaces the preset's whole fog profile. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	bool bUsePresetFog = true;

	/** One line per run when the fog is left to the preset. */
	bool bLoggedPresetFog = false;

	/** Apply each UDS preset exactly as authored - no channel overrides at all.
	 *
	 *  A preset is a set of values tuned together. Overriding one channel leaves cloud shape, coverage
	 *  and density disagreeing with the fog and particles they were authored alongside, which is why a
	 *  condition in PIE could look like different weather from the same preset applied by hand.
	 *
	 *  Moving fog and its ground-level cloud offset survive this, because Post Process Wind Fog is a
	 *  separate system rather than part of any preset. */
	// ===============================================================================================
	//  USING THE PRESETS AS THEY ARE  -  and what happens when you do not
	// ===============================================================================================
	//
	// With this ON, a condition is a UDS preset handed to UDW's own "Change Weather" function, and
	// nothing else. UDW then generates the weather itself: clouds, particles, material wetness and
	// the transitions between them all come from the system the presets were authored against.
	//
	// With this OFF the pipeline reconstructs each condition instead - it reads the preset's channels,
	// applies per-condition overrides on top, and pins every channel as a manual override. That path
	// still exists and still works, but it has cost this project a long run of visual bugs, and they
	// share one shape: a value written on top of a coherent set.
	//
	//   * Pinned channels replace UDW's weather GENERATION with constants. UDW does not hold weather
	//     as eight fixed numbers - it varies them - so a clamped Cloud Coverage renders as an evenly
	//     spread slab instead of the drifting, uneven deck the same preset produces by hand.
	//
	//   * Overriding one channel breaks the set. A preset's fog, cloud, wind and wetness were tuned
	//     together; changing fog alone leaves the clouds disagreeing with it, which is why a condition
	//     could look like different weather from the preset it names.
	//
	//   * Values written once and never restored contaminate everything after them. Four separate bugs
	//     had this cause - Cloudy Density Contribution, Base Fog Density, Extinction Scale and the
	//     cloud coverage write - each fine for the condition that set it and wrong for every condition
	//     that followed.
	//
	//   * Collapsing a range to a constant removes variation the author relied on. Min/Max Fog Start
	//     Distance is a range UDS interpolates across by fog amount; writing one number into both
	//     gives fog of uniform depth everywhere.
	//
	// So: modifying presets is supported and sometimes necessary, but it is the source of nearly every
	// visual defect this pipeline has had. If you turn this off, change ONE thing, look at it, and
	// check the [FogChannels] and [Clouds] log lines rather than trusting that the write landed the
	// way you expected. Saved/wp_presets.txt holds every preset's authored channel values to compare
	// against.
	//
	// Nothing else is changed per condition in verbatim mode: every weather channel UDW holds is the
	// preset's. (Moving fog survives only for the commented-out moving_fog condition, which no stock
	// preset uses.) Cloud layer altitude is not a preset channel - presets hold only the eight weather
	// channels - so CloudAltitudeMode stays a per-city scene setting.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Conditions")
	bool bUsePresetsVerbatim = true;

	/** Scale snow on roofs and ground to each preset's own snowfall rate, rather than using the
	 *  preset's authored coverage.
	 *
	 *  The presets saturate: Snow ships coverage 1.00 at snowfall 6.0, and Snow_Blizzard ships the
	 *  same 1.00 at snowfall 10.0 - so the two heaviest conditions accumulate identically and the
	 *  ground stops distinguishing them. Coverage = snowfall / 10 keeps it monotonic across the set.
	 *
	 *  OFF: every condition is a 1:1 copy of its preset asset, snow coverage included. Coverage that
	 *  follows snowfall is authored IN the project presets instead (WP_Snow_Light 0.3, WP_Snow 0.6,
	 *  Snow_Fog 0.6, stock Snow_Blizzard 1.0), where it is visible and editable. Turning this on is a deliberate deviation from the
	 *  presets; the metadata then records the pinned value, since it reads UDW's live state.
	 *  The metadata validator compares every channel with the preset, so a deviation shows up. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Conditions")
	bool bSnowCoverageFollowsSnowfall = false;

	/** Render volumetric clouds at full resolution instead of the downsampled, reprojected default.
	 *
	 *  The engine draws clouds into a half-resolution target and reprojects between frames. In the
	 *  editor viewport that is invisible; in PIE at capture resolution it shows as a regular grid of
	 *  rectangular blocks across the deck - the reprojection grid, not the cloud shape. Costs frame
	 *  time, which is the right trade for stills that become training data. */
	//  ON. It was added on the theory that the blocky cloud deck was a half-resolution render target,
	//  and that theory was wrong - the cause was Cloud Coverage being written over the preset. But it
	//  earns its place on its own merits, which is a separate question from the bug it failed to fix.
	//
	//  The engine default renders clouds at half resolution and REPROJECTS between frames: it trades
	//  per-frame accuracy for temporal cost, which is the right trade for a game and the wrong one for
	//  stills. A reprojection artefact in a still is baked in permanently, and these images are the
	//  dataset. Costs frame time, which on a long run costs wall-clock time and nothing else.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	bool bHighQualityVolumetricClouds = true;

	/** Prefer a bare-earth DEM tileset for ground height when one is present in the level.
	 *
	 *  Off: tried against Cesium World Terrain and rejected in practice. The theory holds - a DEM is
	 *  terrain by definition - but the elevations did not agree well enough with the photogrammetry
	 *  the imagery is actually rendered from, and a ground reference that disagrees with the visible
	 *  surface is worse than a slightly approximate one that matches it. The calculated site
	 *  elevation below is derived from the same mesh the camera sees, which is the property that
	 *  matters for imagery metadata. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	bool bUseBareEarthTerrain = false;

	/** Substring identifying the bare-earth tileset actor. Cesium names the World Terrain actor
	 *  "Cesium World Terrain", which becomes "CesiumWorldTerrain". Matched by name rather than ion
	 *  asset id so a custom or offline DEM works the same way. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	FString BareEarthTilesetName = TEXT("CesiumWorldTerrain");

	/** Derive height above ground from the site elevation instead of tracing. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	bool bUseSiteGroundElevation = true;

	/** Manual override for the site's ground elevation, metres above mean sea level. 0 means "no
	 *  override", and is the default - an unset elevation falls through to a live trace.
	 *
	 *  Automatic seeding used to fill this in by probing the terrain and holding the result. It was
	 *  removed: measuring during tile streaming sampled coarse placeholder geometry and locked in a
	 *  -40 m elevation for a site that is near +376 m, and a held value cannot recover from a bad
	 *  first measurement. Set this only to pin a known elevation deliberately. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float SiteGroundElevationM = 0.0f;
	/** Print the drone's altitude and height above ground continuously while idle. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Debug")
	bool bLogLiveAltitude = true;

	/** How often to print, in seconds. Low enough to feel live, high enough not to flood the log. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Debug")
	float LiveAltitudeIntervalSeconds = 0.5f;

	/** Also draw it on screen. Far easier to read than the log while actually flying. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Debug")
	bool bDrawLiveAltitudeOnScreen = true;

	/** Pins the capture camera to a fixed manual exposure.
	 *
	 *  On by default because auto-exposure is wrong for this dataset: it normalises brightness, so
	 *  an overcast frame and a clear frame come out similarly exposed and the real difference
	 *  between them is erased before the image is written. It also removes the colour wash on
	 *  condition changes, which is eye adaptation reacting to UDS's per-weather exposure bias.
	 *  Affects the capture only - the viewport keeps auto-exposure. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture")
	bool bLockCaptureExposure = true;

	/** Fixed exposure bias (EV) used when bLockCaptureExposure is on. Raise if the whole dataset
	 *  comes out dark, lower if the bright conditions clip - but change it once, before a run, not
	 *  between runs, or images stop being comparable. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture")
	float CaptureExposureBias = 1.0f;

	/** Expose like a real camera: histogram metering over the frame, ignoring its brightest 2% (so a
	 *  sun or moon in view does not darken the picture), with CaptureExposureBias as compensation
	 *  (+1 = twice as bright). Ultra Dynamic Sky's lighting is built for this - overcast is far dimmer
	 *  in absolute terms than clear sky - so one fixed exposure renders overcast near black. The
	 *  capture keeps its adaptation between frames and renders every frame so the exposure has
	 *  settled by the shutter. Off: the fixed exposure of bLockCaptureExposure. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture")
	bool bCaptureAutoExposure = true;

	/** Diagnostic: also save the game window's view at each capture (<image>_viewport.png), to compare
	 *  what is on screen with what is saved. Off for datasets. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Debug")
	bool bSaveViewportComparison = false;

	/** Auto exposure's allowed range, EV100 - Ultra Dynamic Sky's own "Exposure Brightness Range". */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture")
	FVector2D AutoExposureRangeEV = FVector2D(-10.0, 20.0);

	/** How fast auto exposure adapts (stops per second, both ways); fast enough to settle in the
	 *  2 s wait after each teleport. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture", meta = (ClampMin = "0.1"))
	float AutoExposureSpeed = 10.0f;

	// --- Cloud movement ---------------------------------------------------------------------------
	//
	// Applied to every condition, not only the windy ones, so the deck drifts in clear weather too.
	//
	// Direction is not settable from here by design. UDS's own tooltip on "Cloud Direction" says
	// "Control of this variable will be taken by Ultra Dynamic Weather if you have one in your level.
	// UDW controls cloud direction using its Wind Direction" - so a write here would be reverted, and
	// the per-condition WindDirection is the supported way in.

	/** UDS "Cloud Speed" - scales how fast the deck drifts. UDS's shipped value is logged as
	 *  old -> new in the fog defaults line, so the real multiplier is visible rather than assumed.
	 *  <= 0 leaves UDS's own value. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Clouds")
	float CloudSpeed = 0.35f;   // UDS's authored value, read from the level (was 4.0)

	/** UDS "Formation Change Speed" (UDS range 0-1.5) - how fast the cloud shapes morph as they
	 *  drift, relative to the drift speed itself. <= 0 leaves UDS's own value. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Clouds")
	float CloudFormationChangeSpeed = 0.7f;   // UDS's authored value (was 1.0)

	// --- Fog colour source: removing the seam between fog and sky ---------------------------------
	//
	// The band reads as a hard line partly because the fog and the sky are coloured by two different
	// systems that do not agree. UDS's own palette (Sun Fog Color, Moon Fog Color, the inscattering
	// colour curves) is hand-authored to approximate the sky; wherever the approximation diverges
	// from what the atmosphere actually renders, the boundary between them becomes visible.
	//
	// "Sky Atmosphere" mode makes the fog take its colour from the rendered atmosphere instead, so
	// the two cannot disagree. Critically this stays correct as the time of day changes without any
	// per-step work from us: the shader reads AtmosphereLightIlluminanceOnGroundPostTransmittance
	// (HeightFogCommon.ush:319), which the renderer recomputes every frame from the live sun
	// direction - the same quantity that colours the sky. This is a mode, not a colour. Sunset
	// reddening and blue hour propagate into the fog on their own.
	//
	// Requires the r.SupportSkyAtmosphereAffectsHeightFog project setting, which defaults to 1 and is
	// not overridden in DefaultEngine.ini, so it is already active. It is ECVF_ReadOnly - a restart
	// would be needed if it were ever turned off.

	/** UDS "Fog Color Mode", set by display name. "Sky Atmosphere" derives fog colour from the
	 *  rendered atmosphere - dynamic across time of day for free. "Automatic" defers to the project
	 *  setting, "None" forces UDS's hand-authored palette. Empty leaves UDS's own setting alone. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	FString FogColorMode = TEXT("Sky Atmosphere");

	/** UDS "Sky Atmosphere Fog Influence Sun/Moon/None" (UDS range 0-4) - how strongly the sky
	 *  atmosphere tints the fog. Written to all three so the blend holds through the whole day
	 *  rather than only while the sun is up. <= 0 leaves UDS's own values, which is the default:
	 *  change the mode first and only reach for this if the seam survives it. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float SkyAtmosphereFogInfluence = 0.0f;

	// --- Height fog falloff: how tall the fog slab stands -----------------------------------------
	//
	// The flat band at the horizon is the fog slab seen edge-on. Its *vertical* extent is set by how
	// fast density drops with height, not by how far the fog reaches - so falloff is the knob that
	// thins it, and the two distance caps below are not. Raising falloff collapses the density with
	// height, so a view ray only slightly above horizontal leaves the fog quickly and the band's top
	// edge drops.
	//
	// UDS blends four of these by weather state and writes the result to the component's
	// FogHeightFalloff on every refresh; writing the component directly is therefore reverted. These
	// go to the four source values instead. Engine default is 0.2.
	//
	// UDS's own slider stops at 0.5, but that is a UI clamp (UIMin/UIMax) - it constrains dragging in
	// the Details panel and nothing else. These values are written by reflection straight onto the
	// property, which bypasses editor clamping entirely, so the range above 0.5 is reachable from
	// here even though it cannot be dialled in by hand. Band thickness goes roughly as 1/falloff, so
	// 2.5 is about five times thinner than 0.5.
	//
	// The one thing this cannot rule out is UDS clamping the value internally when it blends the
	// four - that would show up as no further change past 0.5, and the fix would then be Fog Max
	// Opacity instead. The cache log prints old -> new for each so the write itself is verifiable.
	//
	// Foggy is deliberately left alone by default: in a fog condition the fog is *supposed* to reach
	// high, and tightening it there would flatten the fog conditions to a shallow ground layer.

	// These three are set to Ultra_Dynamic_Sky's own authored values as measured in the Ingolstadt
	// level, rather than to 0 (which would skip the write and let UDS's values stand). The rendered
	// result is identical today; what pinning them buys is reproducibility - a dataset captured
	// months apart stays comparable even if the level's UDS actor, or a preset it derives from, is
	// re-authored in the meantime. Set any of them to 0 to hand control back to UDS.
	//
	// The component's own FogHeightFalloff is derived, not set from here: UDS blends these four by
	// weather state and writes the result down. In clear weather it follows Base, hence 0.065.

	/** UDS "Base Height Fog Falloff" - clear weather. <= 0 leaves UDS's own value. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	// UDS's own authored value. 1.6 and 0.03 were both tried and neither changed anything visible,
	// because the density this scales was pinned near zero - falloff shapes a curve that has to have
	// some height to it before the shape matters. Back at the authored value now that BaseFogDensity
	// carries a figure you can actually see.
	float HeightFogFalloffClear = 0.065f;

	/** UDS "Cloudy Height Fog Falloff" - cloudy but not foggy. <= 0 leaves UDS's own value. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float HeightFogFalloffCloudy = 0.050f;

	/** UDS "Foggy Height Fog Falloff" - fog conditions, where the fog is meant to reach high.
	 *  <= 0 leaves UDS's own value. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float HeightFogFalloffFoggy = 0.015f;

	// --- Height fog far-field cap: the flat-horizon fix -------------------------------------------
	//
	// UE's exponential height fog is a flat infinite slab. Its density is a function of world Z alone
	// - Engine/Shaders/Private/HeightFogCommon.ush states the model outright at line 191,
	// "d = GlobalDensity * exp(-HeightFalloff * z)" - and no term anywhere in that file references a
	// planet radius or centre. There is no curvature option and no cvar for one.
	//
	// Cesium renders a globe. From altitude the terrain's horizon drops below eye level while the fog
	// slab stays level, so the slab is seen edge-on as a flat pale band sitting above the true
	// horizon, widening the higher the drone climbs. UDS acknowledges the same limitation in its own
	// "Compensate Simulation For Flat Fog Horizon" tooltip ("...to compensate for the flat horizon of
	// unreal's height fog"), though that setting only nudges sun/moon orientation and does not touch
	// the visual.
	//
	// Capping how far the fog integrates removes the band while leaving the near and mid field
	// untouched. Both properties act ONLY on the analytic height fog:
	//   - neither name appears anywhere in Engine/Shaders/Private/VolumetricFog.usf, and
	//   - ExponentialHeightFogComponent.h:154 states "Volumetric fog currently does not support
	//     StartDistance, FogMaxOpacity and FogCutoffDistance".
	// Volumetric fog and its light shafts are therefore unaffected, which is why this is preferred
	// over disabling "Render Exponential Height Fog" - that kills volumetric fog with it.
	//
	// These are also the only fog values here that survive a UDS refresh unaided: neither name exists
	// in the UDS name table, so UDS never writes them and never reverts them.
	//
	// Reference geometry: horizon distance is ~sqrt(2*R*h), so ~113 km at 1 km altitude and ~160 km
	// at 2 km. A cap well below that but far beyond any useful view distance in the imagery - 30 to
	// 50 km - removes the band without being visible in the frame.

	/** Soft cap (metres): fog stops accumulating past this range, leaving no visible edge. Preferred
	 *  over the hard cutoff - try this first. <= 0 leaves UE's default (disabled). */
	/** UDS "Cloudy Density Contribution" - how much fog density it ADDS as cloud coverage rises.
	 *
	 *  This is the haze that appeared at cloudy and overcast. UDS ships it at 0.0302, fifteen times
	 *  the base density of 0.0020, so a covered sky was contributing far more fog than the base ever
	 *  did - and lowering the base could never reach it, because it is a separate addend rather than
	 *  a multiplier. The diagnostic showed cloud coverage climbing 0 -> 3.8 -> 5.0 in exact step with
	 *  the haze, while base stayed flat.
	 *
	 *  Sized from the measurements rather than guessed: at overcast (cloud cover 5.0) UDS's 0.0302
	 *  added 0.0020 on top of the 0.0004 base - six times the base, which is the haze that read as
	 *  wrong. 0.008 is roughly a quarter of that, so a covered sky still sits visibly hazier than a
	 *  clear one without the air turning to soup.
	 *
	 *  0 removes cloud-driven fog entirely - correct-looking but sterile, since real overcast days
	 *  genuinely are hazier. Raise toward UDS's 0.03 for more, and watch the component figure in the
	 *  [FogChannels] line rather than judging by eye: it is the number that actually moved. */
	//
	//  0.020 rather than 0.008: raising this is self-targeting, because it is multiplied by cloud
	//  cover. clear and partly_cloudy measure 0.00 cover, so they are untouched no matter how high
	//  this goes, while overcast (5.0), rain (6.0) and rain_light (7.5) scale with it. That is why a
	//  single global value can make the wet and covered conditions hazier without ever fogging a
	//  clear sky - the per-condition variation is already carried by the cloud cover.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	// UDS's authored 0.030187, read out of the level. Previously 0.020 - a hand-tuned figure from when
	// this addend was blamed for the haze leak. The leak was real but the cause was the base density
	// being written wrong, so the authored value can stand.
	float CloudyFogDensityContribution = 0.030187f;

	// 4 km. Halved from 8 km to thin the mid and far field - fog accumulates along the view ray, so a
	// shorter integration range is less fog, not more.
	//
	// This is the control that actually addresses the location dependence.
	//
	// UDS parks its fog plane at a fixed world Z, ~240 m below the terrain here, and density goes as
	// 2^(-falloff x (height - fogHeight)) - so terrain below the plane accumulates fog exponentially
	// along the view ray, and how far below it sits changes with the georeference. Capping how far
	// the fog integrates bounds that accumulation directly, without needing to know where the plane
	// is or moving it.
	//
	// Soft rather than hard: fog stops accumulating past this range instead of vanishing, so there
	// is no edge in the frame. 8 km is well beyond anything readable in the imagery from 300 m and
	// far short of the horizon, where the unbounded integral is what turns haze into a wall.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	// OFF. This is written only at BeginPlay, which is why the editor viewport looks right and Play
	// does not: capping where the fog stops being applied puts a hard edge at that exact distance, and
	// beyond it the unfogged scene shows through as a bright horizontal band across the horizon.
	//
	// It was added to stop the flat fog slab forming a false horizon. It was creating one instead.
	// The level's own fog uses start_distance 8346 and no cutoff, and reads correctly.
	float HeightFogEndDistanceM = 0.0f;

	/** Hard cap (metres): geometry past this range receives no fog at all. Can leave a visible ring
	 *  at the boundary if set too near, so it is the fallback for when the soft cap is not enough.
	 *  <= 0 leaves UE's default (disabled). */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float HeightFogCutoffDistanceM = 0.0f;

	// --- Camera / capture format ---
	// Capture through the SceneCapture component at a fixed resolution and a pose we set explicitly,
	// rather than by reading the PIE viewport.
	//
	// Reading the viewport made every image depend on the editor window: resolution changed between
	// sessions (2552x1115, then x1211, then x1550 across three runs) and the orientation followed
	// whatever the player camera was doing, so mouse-looking during a run silently changed the
	// framing. Neither is acceptable in a dataset - matched poses across weather passes mean nothing
	// if the frame moved because someone dragged the window.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Camera")
	bool bUseFixedCameraCapture = true;

	UPROPERTY(EditAnywhere, Config, Category = "Weather|Camera")
	int32 CaptureResolutionX = 1920;

	UPROPERTY(EditAnywhere, Config, Category = "Weather|Camera")
	int32 CaptureResolutionY = 1080;

	// Horizontal field of view in degrees. Lower is longer-lensed: less of the scene, flatter
	// perspective, distant buildings larger. Higher is wider: more scene, stronger perspective and
	// more apparent depth. Typical consumer drones sit around 75-85 horizontal; cinema rigs go lower.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Camera")
	float CaptureFOV = 75.0f;

	// --- Cesium tile streaming ---
	// Cesium loads tiles progressively and culls what the camera cannot see, which is right for
	// interactive play and wrong for a dataset: a capture taken mid-stream bakes unloaded tiles into
	// the image as blank gaps. These settings trade loading speed and memory for a complete frame.

	// Apply capture-friendly settings to every Cesium3DTileset at BeginPlay (forbid holes, disable
	// frustum and fog culling). Turn off to leave the tilesets exactly as authored.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Cesium")
	bool bConfigureCesiumForCapture = true;

	// Block each capture until every tileset reports 100% loaded.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Cesium")
	bool bWaitForCesiumTiles = true;

	// Hold the whole run at the starting line until Cesium has finished its initial load.
	//
	// The per-capture gate is not enough on its own: pressing P immediately after PIE starts begins
	// the sequence while the tileset is still streaming, so the opening conditions are captured
	// against half-built geometry. Waiting once up front costs a few seconds and removes that.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Cesium")
	bool bWaitForCesiumBeforeRun = true;

	// Upper bound on that initial wait. Generous, because a cold tile cache over a slow link can
	// legitimately take a while - but bounded, since a dropped connection would otherwise hang the
	// run forever waiting for a progress figure that will never arrive.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Cesium")
	float CesiumPreRunTimeoutSeconds = 180.0f;

	// Upper bound on that wait. On timeout the capture proceeds and logs a Warning naming the
	// progress reached, so an incomplete frame is always visible in the log rather than silent.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Cesium")
	float CesiumTileLoadTimeoutSeconds = 20.0f;

	// How often to re-check tile progress while waiting.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Cesium")
	float CesiumTileCheckIntervalSeconds = 0.25f;

	// Cesium's "MaximumSimultaneousTileLoads". Raised from its default of 20 because every capture
	// now waits on the result, so settling sooner is worth the extra bandwidth.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Cesium")
	int32 CesiumSimultaneousTileLoads = 64;   // the level's own value

	/** Memory Cesium may keep loaded tiles in, MB (its default is 256). Every condition revisits the
	 *  same route; with 256 MB the start of a long route is dropped before its end is reached, so it
	 *  reloads on every condition. Enough to hold a whole route keeps conditions 2..N off the network.
	 *  0 leaves the tileset's own value. Costs RAM and graphics memory, not image quality. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture", meta = (ClampMin = "0"))
	int32 CesiumTileMemoryMB = 2048;

	/** Tileset load percentage treated as fully streamed when deciding a capture may proceed.
	 *
	 *  Not 100: a single tile lost to a network error never arrives, pinning Cesium's reported
	 *  progress at 99.9% and burning the whole settle timeout on every capture that follows. Over a
	 *  136-image run that is minutes spent waiting for a tile that is never coming. At 99.5% the
	 *  missing geometry is a fraction of one tile - well below anything a frame would show - while a
	 *  genuinely half-streamed tileset still blocks the capture. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Cesium", meta = (ClampMin = "50.0", ClampMax = "100.0"))
	float CesiumLoadCompleteThresholdPercent = 99.5f;

	// Cesium's "MaximumScreenSpaceError": lower means finer tiles and a sharper capture, at the cost
	// of loading more of them. 0 leaves Cesium's own value (16.0) alone.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Cesium")
	// 8.0 rather than Cesium's default 16.0, on measured evidence: a controlled benchmark - each
	// value run twice in one PIE session, warm-up discarded - put both at 20.8s for 5 images, while
	// SSE 8 produced consistently larger PNGs at all five poses (+2.5% to +4.0%, mean +3.2%). More
	// detail for no extra time, because the pipeline's fixed transition and settle delays dominate
	// the run, not tile streaming.
	//
	// The cost is tile requests on a cold cache - bandwidth and Google Photorealistic quota - not
	// wall-clock. Raise back toward 16 if quota matters more than the last few percent of detail.
	// The level's own 12.0, read from the tileset. Was 8.0, which measured ~3% sharper but roughly
	// doubles the tiles loaded - and the level was already tuned to 12 by hand.
	float CesiumMaximumScreenSpaceError = 12.0f;

	/** Unreal's render quality for the session - one scalability level for every group: 0 Low,
	 *  1 Medium, 2 High, 3 Epic, 4 Cinematic. Higher = sharper shadows, better lighting, reflections
	 *  and effects, slower frames. -1 keeps the project's own scalability settings. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture", meta = (ClampMin = "-1", ClampMax = "4"))
	int32 RenderQuality = -1;

	/** Applies RenderQuality, once at the start of the session. */
	void ApplyRenderQuality();

	// --- Sky / time of day (Ultra Dynamic Sky) ---

	// Master switch for every UDS write. When false UDS keeps whatever is set in the level.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	bool bDriveSkyFromCode = true;

	// UDS clock, 0..2400. 1200 = midday. This is LOCAL time at the site, so it needs TimeZoneHours
	// below to mean anything once the real-sun solver is on.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float TimeOfDay = 1200.0f;

	/** Hours offset from UTC that TimeOfDay is expressed in. Negative sentinel (-999) derives it from
	 *  longitude at 15 degrees per hour, which is what the solar position actually depends on.
	 *
	 *  Without this the zone stays at whatever the previous site used, so noon over Sydney gets
	 *  solved as noon in Germany and renders a night sky - the same clock reading, the wrong meridian. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float TimeZoneHours = -999.0f;

	// Frozen by default so lighting is identical across all captures and weather is the only variable.
	// Set true (with DayLengthMinutes) to produce a day-cycle dataset instead.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	bool bAnimateTimeOfDay = false;

	// Written to UDS "Day Length" only when bAnimateTimeOfDay is true.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float DayLengthMinutes = 15.0f;

	// Place the sun using real-world coordinates instead of UDS's authored defaults.
	// Defaults are Ingolstadt, matching the Cesium georeference of the shipped map.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	bool bUseGeoreferencedSun = false;

	/** Real-sun mode: the date the sun is solved for. UDS's own default is 26 March 2021. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky", meta = (ClampMin = "1900", ClampMax = "2200"))
	int32 SunYear = 2026;
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky", meta = (ClampMin = "1", ClampMax = "12"))
	int32 SunMonth = 9;
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky", meta = (ClampMin = "1", ClampMax = "31"))
	int32 SunDay = 30;

	/** Real-sun mode: the world yaw that points north. In the georeferenced world +X is east and +Y
	 *  south, so north (-Y) is yaw 270. UDS's own default, 0, would put north where east is. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float SunNorthYawDeg = 270.0f;

	/** The metadata's visibility_km is the height-fog formula's distance times this: the haze in the
	 *  pictures looks about 10x as thick as the formula alone gives. Written by weather_pipeline.py. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky", meta = (ClampMin = "0.001"))
	float VisibilityCalibration = 0.1f;

	/**
	 * Write the Sun settings below to Ultra Dynamic Sky at Play, and switch its real-sun simulation
	 * off. With real-sun on, UDS solves the sun from latitude, longitude, date and clock, and Sun Yaw
	 * and Sun Pitch are ignored - so the two are mutually exclusive; this wins when both are set.
	 *
	 * The values are the level's own authored Sun panel (read from the Ultra_Dynamic_Sky actor), so
	 * every city is lit by the same sun and weather stays the only variable between images.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	bool bApplySunSettings = true;

	/** UDS "Sun Yaw", degrees. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float SunYawDeg = 0.0f;

	/** UDS "Sun Pitch", degrees. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float SunPitchDeg = 30.0f;

	/** UDS "Sun Light Intensity", lux. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float SunLightIntensityLux = 5.0f;

	/** The regular day-night cycle (bApplySunSettings): when the sun rises and sets, HHMM like
	 *  TimeOfDay - UDS "Dawn Time" and "Dusk Time". */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float SunriseTime = 557.0f;
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float SunsetTime = 1742.0f;

	/** UDS "Sun Scale": how large the sun disc is drawn, in both sun modes - UDS's own physical sizing
	 *  is switched off, so this is the size. weather_pipeline.py converts SUN "size" to it. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky", meta = (ClampMin = "0.01"))
	float SunScale = 1.0f;

	/** The sun's lens flare - the ring and hexagon reflections a camera lens throws off a bright light:
	 *  Ultra Dynamic Sky's sun flare and the engine's post-process lens flare in the capture. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	bool bSunLensFlare = true;

	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float SkyLatitude = 48.7665f;

	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	float SkyLongitude = 11.4258f;

	// UDS owns its own sun, sky light, atmosphere and volumetric clouds. A second set of those actors
	// in the level double-lights the scene (this is what produced the red/yellow and brown haze before
	// UDS was adopted). When true, any such actor that is not part of UDS is hidden at BeginPlay.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	bool bDisableConflictingSkyActors = true;

	// --- Ground accumulation (Dynamic Landscape Weather Effects) ---

	// Gate for writing UDW's ground-accumulation settings (snow depth, puddles, trails). UDS has no
	// single master DLWE toggle — accumulation follows Material Snow Coverage and Snow Depth — so
	// this flag simply controls whether the pipeline touches those values at all.
	// Any of it is only *visible* on surfaces whose material includes the UDS DLWE functions.
	// Suppress lightning flashes entirely - no bolts, no full-screen bloom.
	//
	// On for capture by default, and not only because the yellow flash is unwanted: flashes fire on a
	// random timer, so whether one lands during a capture is chance. That makes the thunderstorm
	// frames non-reproducible, and two runs of the same condition would not match - which is the one
	// property the whole matched-pose design depends on.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Sky")
	bool bDisableLightningFlashes = true;

	// Let UDW ramp material wetness and snow coverage in gradually. Off for capture: each condition
	// lasts seconds, not the minutes those ramps are tuned for, so the frame would show a surface
	// part-way to the state its metadata claims.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|DLWE")
	bool bSimulateMaterialStateOverTime = false;

	// Off: DLWE only sets up snow TRAILS (carved by something touching the ground) and their render
	// target. Nothing touches the ground in an aerial capture, so these were three overrides with no
	// visible effect. Snow and wetness on the ground come from the preset's material channels either way.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|DLWE")
	bool bEnableDLWE = false;

	// Carve trails through accumulated snow. Needs a DLWE_Interaction component on a ground-contacting
	// actor to have any effect.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|DLWE")
	bool bEnableSnowTrails = true;

	// Side length (cm) of the square, centred on the control point, inside which snow/dust
	// interactions exist. UDW "Snow/Dust Interaction Render Target Area". Must exceed how far the
	// character travels during a phase, or trails stop at a visible hard edge.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|DLWE")
	float DLWEAreaSize = 20000.0f;

	// UDW "Snow/Dust Interaction Render Target Resolution" — texels across DLWEAreaSize.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|DLWE")
	int32 DLWETargetResolution = 2048;

	// --- Phase timing ---

	// Total wall-clock duration of each capture phase (seconds). The CapturesPerWeatherType snapshots
	// are spread evenly across the window; weather runs continuously the whole time.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Timing")
	float RainPhaseDurationSeconds = 120.0f;

	UPROPERTY(EditAnywhere, Config, Category = "Weather|Timing")
	float SnowPhaseDurationSeconds = 120.0f;

	UPROPERTY(EditAnywhere, Config, Category = "Weather|Timing")
	float FogPhaseDurationSeconds = 120.0f;

	// Auto-start the capture pipeline shortly after BeginPlay instead of waiting for the P key.
	// REQUIRED for Simulate In Editor (SIE), which has no player input. Leave false for normal
	// PIE (press P). AutoStartDelaySeconds gives Cesium tiles time to stream in before capture.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Timing")
	bool bAutoStartPipeline = false;

	// Delay before the auto-start fires (seconds). Lets the world / Cesium tiles settle first.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Timing")
	float AutoStartDelaySeconds = 5.0f;

	// Root for all capture output. Empty => <ProjectSavedDir>/WeatherPipeline
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Output")
	FString OutputRootDir;

	/** Optional subfolder inside WeatherSnapshots, so several runs can share one output root.
	 *
	 *  Capture filenames are condition plus waypoint index and carry nothing about location, so two
	 *  runs over different cities write identical names and the second overwrites the first - images
	 *  and metadata alike. Set this per run and each city keeps its own folder. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Output")
	FString OutputSubfolder;

	// --- RTSP streaming (optional; only used when StartStreamRTSP is called) ---
	// ffmpeg executable. Bare name resolves via PATH; or set an absolute path.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Streaming")
	FString FFmpegExePath = TEXT("ffmpeg.exe");

	UPROPERTY(EditAnywhere, Config, Category = "Weather|Streaming")
	FString StreamTargetIP = TEXT("127.0.0.1");

	UPROPERTY(EditAnywhere, Config, Category = "Weather|Streaming")
	int32 StreamVideoPort = 9000;

	UPROPERTY(EditAnywhere, Config, Category = "Weather|Streaming")
	int32 StreamDataPort = 9001;


	// --- Pipeline enable flags ---
	// Manual 1/0 switches. If all three are true the pipeline runs Rain → Snow → Fog → Exit as before.
	// Setting a flag to false skips that weather type in BOTH the litmus visual check AND the capture pipeline.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather Pipeline")
	bool bEnableRain = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather Pipeline")
	bool bEnableSnow = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather Pipeline")
	bool bEnableFog = true;

	// Public init — called by AMyActor_controllingWeather after it finds this actor
	void InitializeCharacterReference();

	// Returns the character reference (Blueprint accessible)
	UFUNCTION(BlueprintCallable, Category = "Weather")
	ACharacter* TargetCharacter();

	// --- Weather sequence start/stop ---
	void StartRain();
	void StopRain();
	void StartSnow();
	void StopSnow();
	void StartFog();
	void StopFog();

	// Clears every manual override so the level is left with UDW's own weather.
	UFUNCTION(BlueprintCallable, Category = "Weather")
	/**
	 * Releases the manual weather overrides, and by default also puts UDS's sky and fog settings back.
	 *
	 * @param bRestoreSkyDefaults  Pass false when clearing *between* conditions rather than at the
	 *        end of a run. This distinction is load-bearing: the function has seven callers, six of
	 *        which mean "the session is over" and one - the litmus step - which means "make way for
	 *        the next condition". Restoring UDS's own values in that second case writes its defaults
	 *        back before every condition, and because UDS only pushes a new density to the fog
	 *        component on its next refresh, the following condition renders with the restored value.
	 *        Two separate bugs in this file came from exactly that, so the two meanings are now
	 *        distinct at the call site rather than implied by context.
	 */
	void ClearUDWWeather(bool bRestoreSkyDefaults = true);

	// Cancels an in-flight litmus or capture run: stops every timer, releases the weather, and leaves
	// PIE running. This is the second press of the P key.
	UFUNCTION(BlueprintCallable, Category = "Weather")
	void StopWeatherCapture();

	// Applies the heaviest configured step of a type immediately. Useful for eyeballing a look from
	// Blueprint without running the whole pipeline.
	UFUNCTION(BlueprintCallable, Category = "Weather")
	void ApplyHeaviestStep(EWeatherKind Kind);

	// --- Litmus test: shows Rain→Snow→Fog at max intensity for a few seconds each, then starts capture ---
	UFUNCTION(BlueprintCallable, Category = "Weather")
	void RunWeatherLitmusTest();

	// Starts the real Rain -> Snow -> Fog capture run directly (no litmus preview).
	// Bound to the P key by AMyCharacterBase so PIE runs exactly rain/snow/fog capture and exits.
	UFUNCTION(BlueprintCallable, Category = "Weather")
	void StartWeatherCapture();

	/**
	 * Fired after a step has been pushed to UDW/UDS. Optional escape hatch: implement it in a
	 * Blueprint subclass to reach anything C++ reflection cannot. The pipeline does not require it.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Weather")
	void OnUDWStepApplied(EWeatherKind Kind, const FUDWWeatherStep& Step);

	// --- Camera movement API ---
	void MoveForwardForDistance(float DistanceInCm, float Speed);
	void RotateCCW(FRotator Angle, float AngularSpeed);
	void MoveObj(float DeltaTime);
	void RotateObj(float DeltaTime);

	// --- Image capture API ---
	void SaveActiveCameraImage(bool keep_size = false);
	void StartCapture(float InFrameRate);
	void StopCapture();
	void StartStreamRTSP(float InFPS = 30, FString ServerURL = TEXT("rtsp://10.116.88.38:8554/mystream"));
	void StreamRTSP();
	void ShareRate(float DeltaTime);

private:
	// Grants the automation tests access to the internals they need to assert on - the condition
	// table builder and the settings validator - without widening the real public surface. Tests
	// should not dictate a class's API, and making BuildDefaultCaptureRun() public would invite
	// callers to rebuild the run mid-capture, which is exactly what the lazy-fill guards prevent.
	friend struct FWeatherPipelineTestAccess;

	// --- Weather state flags ---
	bool rainState = false;
	bool snowState = false;
	bool fogState  = false;

	// --- Capture state flags (internal) ---
	bool bCaptureActive  = false;
	bool bCapturePending = false;
	bool bStreamActive   = false;
	bool bStreamPending  = false;

	// --- Movement state (internal) ---
	bool     bMoveCamera       = false;
	bool     bRotateCamera     = false;
	FVector  StartLocation;
	FVector  TargetLocation;
	FRotator StartRotator;
	FRotator TargetRotator;
	float    MoveSpeed         = 0.0f;
	float    AngularSpeed      = 0.0f;
	bool     TargetDestination = false;
	bool     TargetAngle       = false;

	// --- Viewport dimensions (synced at init and on weather start) ---
	int32 ViewportWidth  = 0;
	int32 ViewportHeight = 0;

	// --- Image capture ---
	float   bCaptureFrameRate    = 0.0f;
	float   CaptureInterval      = 0.0f;
	float   TimeSinceLastCapture = 0.0f;
	FString ScreenshotPath;
	FString ScreenshotPath_camera;

	// Output directories resolved at BeginPlay from OutputRootDir (see ResolveOutputDirs)
	FString ResolvedRecordingsDir;
	FString ResolvedSingleCamDir;
	FString ResolvedSnapshotDir;

	// Computes the three output directories and creates them on disk
	void ResolveOutputDirs();

	// --- RTSP stream ---
	FRTSPStreamer Streamer;
	float StreamInterval         = 0.0f;
	float streamRate             = 0.0f;
	float TimeSinceLastImgStream = 0.0f;

	// --- Ultra Dynamic Sky / Weather actor references, resolved lazily ---
	TWeakObjectPtr<AActor>  UDWActor;
	TWeakObjectPtr<AActor>  UDSActor;
	TWeakObjectPtr<UObject> UDWManualState;
	bool bDLWEInitialized = false;

	// UDW's control-point mode only has to be switched to "Custom Location" once; after that only the
	// location itself is written per frame.
	bool bControlPointModeSet = false;

	// Whether the *current* sequence wants weather changes to snap. The litmus sets this true so its
	// rain/snow/fog cuts are hard; the capture pipeline clears it when bSmoothWithinPhase, so the
	// build-up inside a phase eases instead.
	bool bUseInstantWeatherNow = true;

	// UDS fog settings as they were before the pipeline touched them. The fog sweep drives these hard
	// (density x9, start distance to 0), and unlike the weather state they are NOT part of the manual
	// override system - so nothing puts them back. Without this the sky stays permanently fogged
	// after the fog phase, through every later phase and every later run in the same session.
	bool  bLoggedPPWFDefaults      = false;
	bool  bLoggedCloudAnchor       = false;
	bool  bLoggedHeightFogFarField = false;
	bool  bLoggedFogPlaneAnchor    = false;
	mutable bool bLoggedBareEarth  = false;
	bool  bCachedSkyFogDefaults    = false;
	float DefaultScaleFogDensity   = 1.0f;
	float DefaultMinFogStart       = 0.0f;
	float DefaultMaxFogStart       = 0.0f;
	float DefaultVolFogDistance    = 0.0f;
	float DefaultVolFogExtinction  = 0.0f;

	// Cached on the ExponentialHeightFogComponent rather than on UDS - these two live on the engine
	// component, not in UDS's property set, so they are read and restored through it directly.
	float DefaultHeightFogEndDistance = 0.0f;
	float DefaultHeightFogCutoff      = 0.0f;

	float DefaultFalloffClear  = 0.0f;
	float DefaultFalloffCloudy = 0.0f;
	float DefaultFalloffFoggy  = 0.0f;
	/** UDS's authored Extinction Scale, snapshotted so a condition that lowers it can restore it. */
	float DefaultCloudExtinction = 0.0f;

	float DefaultCloudSpeed     = 0.0f;
	float DefaultFormationSpeed = 0.0f;

	// UDS's own base fog density, and where its fog plane sat before the pipeline moved it. Both were
	// being changed and never put back, which is why fog lingered in the level after a run ended: the
	// density stayed at the pipeline's value and the plane stayed anchored to sea level, hundreds of
	// metres below the terrain at any inland site.
	float   DefaultBaseFogDensity  = 0.0f;
	double  DefaultFogPlaneZ       = 0.0;
	bool    bHaveDefaultFogPlaneZ  = false;

	// --- Active capture metadata (buffered when a step is applied, read at snapshot time) ---
	// The snapshot fires on a delay, by which point the sequence index has already advanced to the
	// next step — so the index is buffered here rather than read live at capture time.
	EWeatherKind     ActiveCaptureKind      = EWeatherKind::Rain;
	FUDWWeatherStep  ActiveCaptureStep;
	int32            ActiveCaptureStepIndex = 0;
	float            ActiveCaptureTimeOfDay = 0.0f;

	// Moving-fog strength actually in force for the frame being captured, recorded so the JSON says
	// which category an image belongs to rather than leaving it to be inferred from the filename.
	float            ActiveMovingFogIntensity = 0.0f;

	// Cloud height the active condition asked for, in cm above ground. Negative means the condition
	// did not override it and the actor's own CloudAltitudeMode applies.
	float            ActiveCloudHeightOffsetM      = 0.0f;
	bool             bHaveActiveCloudHeightOffset = false;
	bool             bActiveCloudHeightFromGround = false;

	// Terrain height sampled once, for AboveTerrainAtStart. Held for the run so the cloud base cannot
	// drift as the drone crosses relief, while still adapting to a change of Cesium coordinates.
	float            CachedTerrainZ      = 0.0f;
	bool             bHaveCachedTerrainZ = false;

	// Last ground height a trace actually resolved, kept so a momentarily un-traceable world does
	// not silently change what the cloud altitude is measured from. See AnchorWeatherToDrone.
	double           LastGoodGroundZ     = 0.0;
	bool             bHaveLastGoodGround = false;

	// --- Sequence progression indices ---
	int32 CurrentRainSequenceIndex = 0;
	int32 CurrentSnowSequenceIndex = 0;
	int32 CurrentFogSequenceIndex  = 0;

	// --- Timer handles ---
	FTimerHandle RainSequenceTimerHandle;
	FTimerHandle SnowSequenceTimerHandle;
	FTimerHandle FogSequenceTimerHandle;
	FTimerHandle SnapshotTimerHandle;
	FTimerHandle ExitTimerHandle;

	// --- Litmus test state ---
	// bLitmusActive guards against re-entry: the Blueprint litmusVar toggle is polled every Tick, and
	// if it gets re-set while a preview is already running, restarting would reset LitmusStep to 0
	// forever and the capture pipeline it chains into would never start.
	FTimerHandle LitmusHandle;
	int32 LitmusStep = 0;
	bool  bLitmusActive = false;

	// True once the litmus has actually pushed a step, so the next litmus tick can verify that step
	// landed. The litmus runs seconds into PIE, which is where a broken apply path should be reported
	// - waiting for the first capture buries the diagnostic two minutes deep.
	bool  bLitmusAppliedAStep = false;

	// True while the litmus is in the blank gap between two weather types, so the next timer firing
	// advances to the following type instead of clearing again.
	bool  bLitmusClearing = false;

	// True from the moment a capture run starts until it exits. The rainVar / snowVar / fogVar
	// Blueprint debug toggles are polled every Tick and start their own phase — which shares
	// SnapshotTimerHandle and ActiveCaptureStep with the real run. Letting them interleave produces
	// snapshots labelled with one weather type and the parameters of another, so they are ignored
	// while a sequence owns those resources.
	bool bPipelineActive = false;

	/** True when a litmus preview or capture run currently owns the shared timers/state. */
	bool IsSequenceRunning() const { return bLitmusActive || bPipelineActive; }

	// --- Auto-start (Simulate In Editor / no player input) ---
	FTimerHandle AutoStartTimerHandle;

	// Retry timer for the "wait for Cesium tiles" gate, plus how long the current capture has been
	// waiting. Reset once a capture proceeds, so the timeout is per-capture and not cumulative.
	// How long the current waypoint has been waiting for the scene and weather to settle. The wait
	// itself runs on RunTimerHandle, so the run cannot advance past a capture that has not happened.
	float        CaptureWaitElapsed = 0.0f;

	// Smooth-normal state currently applied to the tilesets, so a flip only happens on a real change.
	bool         bSmoothNormalsCurrentlyOn = false;
	bool         bKnowSmoothNormalsState   = false;

	// How long the run has been held at the starting line waiting for Cesium's initial load.
	float        PreRunWaitElapsed  = 0.0f;
	bool         bLoggedPreRunWait  = false;

	// Delete the previous run's images from the snapshot folder before a new run starts.
	//
	// Without this a partial run leaves a directory that LOOKS like a complete dataset but is not:
	// same-named PNGs get overwritten while differently-named ones survive, and the metadata JSON is
	// truncated - so a run stopped early splices its own frames onto leftovers from a previous run,
	// potentially from an entirely different Cesium location. That is a silent corruption, and worse
	// than an obvious failure. Only *.png and weather_metadata.json directly inside the snapshot
	// folder are removed; subfolders are left alone, so archived runs are safe.
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Capture Run")
	bool bClearSnapshotFolderOnRun = true;

	// Images actually written this run. Logged at the end, so a condition that silently produced
	// nothing - the fog frame going missing, say - is visible rather than something to count by hand.
	int32        CapturedImageCount = 0;

	// In-flight step-to-step ramp. LastAppliedStep is the ramp's starting point and is kept even when
	// interpolation is off, so switching it on mid-run does not ramp from a default-constructed step.
	FTimerHandle    TransitionTimerHandle;
	FUDWWeatherStep TransitionFrom;
	FUDWWeatherStep TransitionTo;
	FUDWWeatherStep LastAppliedStep;
	// Matched-run cursor. CurrentCondition x CurrentWaypoint identifies exactly one captured image,
	// and the same waypoint index under a different condition is its matched counterpart.
	FTimerHandle RunTimerHandle;
	int32        CurrentConditionIndex = 0;
	int32        CurrentWaypointIndex  = 0;
	FTransform   CharacterStartTransform = FTransform::Identity;
	bool         bHaveCharacterStart     = false;

	// The pose the next capture must be taken from, written when the drone is placed at a waypoint.
	// Applied to the SceneCapture component immediately before the shutter, so the framing cannot be
	// disturbed by the pawn, the controller, or the user looking around mid-run.
	FTransform   ActiveCapturePose     = FTransform::Identity;
	bool         bHaveActiveCapturePose = false;
	// The capture component's place on the pawn, restored when no pose is pinned (ReleaseCapturePose).
	FTransform   CaptureAttachTransform     = FTransform::Identity;

	// The random camera spots: one draw, the stream it comes from, and per spot how often it was drawn
	// again for being blocked.
	struct FSpotDraw
	{
		FVector2D OffsetCm  = FVector2D::ZeroVector;   // from the centre of the area
		float     HeightCm  = 0.0f;                    // above the ground under each picture
		FRotator  View      = FRotator::ZeroRotator;
		float     TimeOfDay = 1200.0f;                 // HHMM
	};
	enum class ESpotSettle : uint8 { Ready, Moved, Redrawn };

	FRandomStream SpotStream;
	FVector       SpotsCentre          = FVector::ZeroVector;
	TArray<int32> SpotRedraws;
	bool          bHaveManualStartSpot = false;
	FSpotDraw     ManualStartSpot;
	/** The time of day of the spot being captured (HHMM); negative before the first spot. */
	float         SpotTimeOfDay        = -1.0f;

	FSpotDraw DrawSpot();
	/** The capture points of one spot: the poses its shot (CaptureShot) takes there. */
	TArray<FCaptureWaypoint> BuildShot(int32 Spot, const FSpotDraw& Draw) const;
	void ReplaceSpot(int32 Spot, const FSpotDraw& Draw);
	/** First visit to a spot: measures the ground under each of its capture points and places them
	 *  their height above it. A point inside or against a building, facing a wall or over no ground
	 *  sends the whole spot to be drawn again (the user's own view is taken as it is). */
	ESpotSettle SettleSpotInOpenAir(int32 Spot);
	static float HhmmToMinutes(float Hhmm);
	static float MinutesToHhmm(float Minutes);

	/** The sun's height above the horizon and the compass direction it shines from, read from Ultra
	 *  Dynamic Sky's sun light. False without one. */
	bool GetSunDirection(float& OutElevationDeg, float& OutFromCompassDeg) const;
	bool         bHaveCaptureAttachTransform = false;

	EWeatherKind    TransitionKind    = EWeatherKind::Rain;
	float           TransitionElapsed = 0.0f;
	bool            bTransitionActive = false;
	bool            bHaveLastStep     = false;
	bool bAutoStartScheduled = false;

	// Set once the "weather did not take effect" diagnostic has fired, so the callable-function dump
	// it triggers is printed a single time rather than on every capture.
	bool bLoggedOverrideFailure = false;

	// The UDW function that makes a written manual state actually take effect, resolved once from a
	// list of candidate names (see UDWApplyFunctionCandidates). NAME_None means "not resolved yet";
	// bResolvedApplyFunction distinguishes that from "resolved and none of them exist".
	FName ResolvedApplyFunction  = NAME_None;
	bool  bResolvedApplyFunction = false;

	// False until the first snapshot of a run writes the metadata file. Reset by StartWeatherPipeline
	// so each run starts a fresh weather_metadata.json rather than appending to the previous run's.
	bool bMetadataFileStarted = false;

	// --- Private methods ---

	// Reads the current frame into OutBitmap. Uses the PIE game viewport when one exists, else
	// falls back to the SceneCapture render target — so captures also work in Simulate In Editor,
	// which has no game viewport. Returns true on a non-empty read.
	bool AcquireSceneImage(TArray<FColor>& OutBitmap, int32& OutWidth, int32& OutHeight);

	/** The capture's image as the screen shows it: linear read-back with the sRGB display curve -
	 *  through the fish-eye lens when bFisheyeLens, at CaptureResolutionX x Y either way. */
	bool ReadCaptureAsDisplayed(TArray<FColor>& OutBitmap) const;

	/** Fish-eye lens: the image uses the equidistant projection of a real fish-eye (distance from the
	 *  centre proportional to the angle), with CaptureFOV as its horizontal field of view, at the same
	 *  CaptureResolution. Rendered as a wider normal view at FisheyeRenderScale times the resolution -
	 *  enough that the centre stays as sharp as a normal render - and remapped. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Camera")
	bool bFisheyeLens = false;

	float FisheyeRenderFovDeg = 90.0f;
	float FisheyeRenderScale  = 1.0f;

	void ResizeBitmap(const TArray<FColor>& Src, int SrcW, int SrcH, TArray<FColor>& Dst);
	void SetupCaptureComponent();
	void SetupCameraDebugLog();

	// --- Ultra Dynamic Weather / Sky plumbing ---

	// Resolve (and cache) the placed UDW / UDS actors. Safe to call repeatedly; returns false and
	// logs once if the level has no such actor.
	bool ResolveUDWActor();
	bool ResolveUDSActor();

	// Fills RainSteps / SnowSteps / FogSteps with the in-code default sweeps when they are empty.
	void BuildDefaultWeatherSteps();

	// The original hand-authored sweep. Still the fallback whenever a preset cannot be read, and
	// still only fills arrays that are empty, so it never overwrites preset-built steps.
	void BuildHandAuthoredWeatherSteps();

	// Loads a UDS weather preset and returns its Class Default Object, which is where the values
	// live - the presets are Blueprint classes, not data asset instances. Null if it will not load.
	UObject* LoadPresetDefaults(const FString& PresetPath) const;

	// Builds one kind's sweep by walking a chain of presets and blending between them. Returns false
	// if fewer than two presets loaded, so the caller can fall back to the hand-authored table.
	bool BuildStepsFromPresetChain(EWeatherKind Kind, const TArray<FString>& Chain,
	                               TArray<FUDWWeatherStep>& OutSteps) const;

	// Entry point for setting weather. Ramps from the previously applied step when interpolation is
	// on and a previous step exists; otherwise applies immediately. The litmus always goes immediate.
	void ApplyWeatherStep(EWeatherKind Kind, const FUDWWeatherStep& Step);

	// Pushes one step to UDW (state + overrides + particles + DLWE) and to UDS (sky + time of day).
	// This is the single place weather is actually written.
	void ApplyWeatherStepImmediate(EWeatherKind Kind, const FUDWWeatherStep& Step);

	// One increment of an in-flight ramp. Re-arms itself until it lands exactly on the target.
	void TransitionTick();

	// Stops any ramp in progress. Called whenever the weather is cleared or the run is cancelled.
	void CancelWeatherTransition();

	// --- Matched capture run ---

	// Fills CaptureConditions / LitmusConditions / FlightPath with defaults when they are empty.
	void BuildDefaultCaptureRun();

	// Removes the previous run's PNGs and metadata from the snapshot folder. Scoped deliberately
	// narrowly - see bClearSnapshotFolderOnRun.
	void ClearSnapshotFolder();

	// Applies one condition: pins whatever channels the condition overrides, then hands the preset to
	// UDW's Change Weather. Returns false if the preset could not be resolved.
	bool ApplyConditionPreset(const FWeatherCondition& Condition, float InTransitionSeconds);

	// Writes the condition's particle look. Called after the transition has landed, because the
	// transition itself recomputes these from the weather state on its way to the target.
	void ApplyConditionLookOverrides(const FWeatherCondition& Condition);

	// Starts the conditions x waypoints run.
	void StartMatchedCaptureRun();

	// Applies the current condition and schedules the first waypoint once it has settled.
	void BeginCurrentCondition();

	// Moves the camera to the current waypoint and schedules its capture.
	void GoToCurrentWaypoint();

	// Captures at the current waypoint, then advances to the next waypoint or condition.
	void CaptureCurrentWaypoint();

	// Places the drone exactly at a waypoint. Exact placement is what makes poses reproduce across
	// conditions, so this teleports rather than flies.
	void PlaceCameraAtWaypoint(const FCaptureWaypoint& Waypoint);

	// Manual runs (bManualFrame): the spots start at the user's view when P is pressed.
	void StartPathFromUserView();

	// Makes the user's view spot 1, taken as it is, with the capture area centred on it.
	void StartPathAt(const FVector& Location, const FRotator& View, float HeightAboveGroundM);

	// Pins the capture camera in world space at the pose the picture is taken from, from now until the
	// shutter: every frame it renders while settling is then from that exact pose, so the shutter
	// frame has no camera jump behind it (which motion blur and temporal anti-aliasing smear).
	void SetCapturePose(const FTransform& Pose);

	// Unpins it: the capture camera follows the pawn again (manual flying).
	void ReleaseCapturePose();

	// All Ultra Dynamic Sky writes: time of day, animation, geographic sun, cloud look.
	void ApplySkySettings(const FUDWWeatherStep& Step);

	// Turns UDW's Post Process Wind Fog on or off and sets its strength. Called once at BeginPlay for
	// the actor default, then per condition so each one can opt in or out.
	void SetMovingFog(bool bEnable, float IntensityScale);

	// Traces straight down from the drone to find the terrain under it. Returns false when nothing is
	// hit - Cesium tiles carry collision (CreatePhysicsMeshes defaults on), so a miss means the tiles
	// below have not streamed in yet rather than that there is no ground.
	bool MeasureGroundBelowDrone(float& OutGroundZ, float& OutHeightAboveGround) const;

	// Converts an Unreal position into real-world longitude (deg), latitude (deg) and height above
	// the WGS84 ellipsoid (metres) via the CesiumGeoreference. False when Cesium is absent, in which
	// case Unreal coordinates are all the dataset can honestly claim.
	bool GetGeographicPosition(const FVector& UnrealPosition, FVector& OutLonLatHeight) const;

	// Height in metres of a world position above the terrain directly beneath it.
	bool MeasureHeightAboveGroundAt(const FVector& WorldPosition, float& OutHeightMetres) const;

	// Unreal Z corresponding to a real-world altitude, at the longitude/latitude of NearPosition.
	// The globe curves, so an altitude does not map to a single Z everywhere - it has to be resolved
	// at a location, and the drone's own position is the one that matters.
	bool GetUnrealZForAltitude(const FVector& NearPosition, float AltitudeMetres, double& OutZ) const;

	// Moves the cloud deck and fog layer to sit a fixed distance from the drone, so weather frames
	// the same way regardless of the terrain elevation at the current Cesium coordinates.
	void AnchorWeatherToDrone();

	// Ultra_Dynamic_Sky's own ExponentialHeightFogComponent, or null. Found by class rather than by
	// reflected name because it is a plain engine component - no UDS property lookup involved.
	// Note this deliberately does NOT follow UDS's "Custom Height Fog Actor" override: that route
	// needs a standalone AExponentialHeightFog in the level, which EnforceSingleSkySource disables.
	UExponentialHeightFogComponent* GetSkyHeightFogComponent() const;

	// Applies HeightFogEndDistanceM / HeightFogCutoffDistanceM to that component. Fixes the flat fog
	// horizon over Cesium's globe without disturbing volumetric fog. See the property comments.
	void ApplyHeightFogFarField();

	/**
	 * Range- and coherence-checks the whole configuration surface at startup, returning false when
	 * an issue would make the captured data invalid rather than merely odd-looking.
	 *
	 * Reports only, never repairs: silently correcting a value would hide the mistake and leave the
	 * recorded metadata disagreeing with the settings the operator believes are in force.
	 * Implemented in MyActor_weather_Validation.cpp.
	 */
	bool ValidateSettings();

	/** Logs which optional systems this world provides (Cesium, UDS, UDW), so running outside a
	 *  Cesium level is a stated configuration rather than an untested one. */
	void LogEnvironmentCapabilities();

	/** Prints the drone's real-world altitude and its height above the terrain. Throttled by
	 *  LiveAltitudeIntervalSeconds and driven from Tick while no run is in progress. */
	void ReportLiveAltitude(float DeltaTime);

	// Seconds since the live readout was last printed.
	float LiveAltitudeAccumulator = 0.0f;

	// Takeoff-relative altitude was tried here and removed. It is what a real drone's controller
	// shows - the barometer zeroes at launch - but this pipeline spawns the camera already airborne,
	// so it read zero at altitude and described the session rather than the location.

	// Wall-clock start of the current capture run, for the seconds-per-image figure.
	double CaptureRunStartTime = 0.0;

	// --- Screen-space-error benchmark -------------------------------------------------------------
	//
	// Measuring tile quality against run time by hand does not work: two runs at identical settings
	// came out 38.4s and 19.8s, and the whole 18.6s difference was Cesium's tile cache being cold the
	// first time. That effect is twice the size of the thing being measured, so a cold run compared
	// against a warm one would produce a confident and meaningless answer favouring whichever ran
	// second. Editor viewport size is a second confound, since screen-space error is by definition
	// relative to the view.
	//
	// This runs every value twice inside one PIE session - first pass warms the cache and is
	// discarded, second is measured - so cache state, viewport, weather and flight path are all held
	// constant and only the setting varies.

	/** Run the benchmark instead of a normal capture. Each value is run twice; the first is a
	 *  discarded warm-up. Results are logged as a table at the end, then the session exits. */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Debug")
	bool bRunScreenSpaceErrorBenchmark = false;

	/** Values to compare. Empty falls back to 8 and 16 (Cesium's default). */
	UPROPERTY(EditAnywhere, Config, Category = "Weather|Debug")
	TArray<float> ScreenSpaceErrorBenchmarkValues;

	// Pass counter: value index is PassIndex/2, and an odd PassIndex is the measured run.
	int32          BenchmarkPassIndex = 0;
	TArray<double> BenchmarkWarmSeconds;
	TArray<int32>  BenchmarkWarmImages;

	/** Records this pass and starts the next. Returns true when another pass was started, meaning
	 *  the caller must not exit. */
	bool AdvanceScreenSpaceErrorBenchmark();

	// Records UDS's fog settings once, before the pipeline first overwrites them.
	void CacheSkyFogDefaults();

	// Puts those fog settings back. Called whenever the weather is cleared.
	void RestoreSkyFogDefaults();

	// Makes a written manual weather state take effect. Three things have to happen, in order: the
	// Manual Weather State object is synced into the flat actor variables UDW actually reads; the
	// override RepNotify handlers are fired by hand (reflection writes do not trigger OnRep, so UDW
	// otherwise never learns the override set changed and keeps its BeginPlay bindings); and only
	// then is the global/local weather state regenerated through the rebound chain.
	void ForceUDWWeatherUpdate();

	// Reads UDW's live state back and logs a Warning if it does not match what Step asked for.
	// Context names the caller so an early litmus failure is distinguishable from a capture failure.
	void VerifyWeatherApplied(const FUDWWeatherStep& Step, const TCHAR* Context);

	// Keeps UDW's control point on the character. Called every Tick, because the drone moves
	// continuously and a per-step update would leave the weather trailing behind it.
	void UpdateWeatherControlPoint();

	// Collects every Cesium3DTileset in the level by reflection. Returns false when Cesium is not
	// installed at all, which is not an error - the pipeline simply has nothing to wait for.
	bool GatherCesiumTilesets(TArray<AActor*>& OutTilesets) const;

	// One-time: makes the tilesets render complete frames rather than streaming-optimised ones.
	void ConfigureCesiumForCapture();

	// Toggles Cesium's smooth-normal generation on every tileset. Only acts when the value actually
	// changes, because each flip rebuilds the tiles.
	void SetCesiumSmoothNormals(bool bEnable);

	// True once every tileset reports 100% loaded. OutLowestProgress is the worst tileset's percent,
	// for the timeout message.
	bool AreCesiumTilesetsLoaded(float& OutLowestProgress) const;

	// True once UDW's live state matches the condition being captured. OutWorstDelta is the largest
	// remaining difference across the driven channels, for the timeout message.
	bool IsWeatherSettled(float& OutWorstDelta) const;

	// One-time DLWE configuration (interactions, trails, accumulation area).
	void SetupDLWE();

	// Hides sun/sky/cloud/fog actors that are not part of UDS, so there is exactly one sky source.
	void EnforceSingleSkySource();

	// The sweep array for a weather kind.
	const TArray<FUDWWeatherStep>& StepsFor(EWeatherKind Kind) const;

	static FString WeatherKindToString(EWeatherKind Kind);

	// State machine step for each weather type — called by timer, advances the sweep index
	void UpdateRainInterval();
	void UpdateSnowInterval();
	void UpdateFogInterval();

	// Timer snapshot wrappers — called SettleSeconds after each step is applied
	void TriggerRainSnapshot();
	void TriggerSnowSnapshot();
	void TriggerFogSnapshot();

	// Saves a labeled PNG snapshot and appends to weather_metadata.json
	void CaptureWeatherSnapshot(const FString& WeatherType);

	// Requests clean game exit after all sequences complete; called by timer to allow I/O to flush
	void RequestGameExit();

	// Internal step function driven by LitmusHandle timer
	void LitmusTestStep();

	// Pipeline entry point — starts the first enabled weather type (or exits if all disabled).
	void StartWeatherPipeline();

	// Chain helpers — called at the end of each weather type's sequence to pick the next enabled type.
	// These respect bEnableRain / bEnableSnow / bEnableFog and exit cleanly if no enabled type remains.
	void ChainAfterRain();
	void ChainAfterSnow();
};
