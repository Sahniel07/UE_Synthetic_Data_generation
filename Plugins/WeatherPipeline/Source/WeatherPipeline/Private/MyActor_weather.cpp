
#include "MyActor_weather.h"
#include "CaptureShots.h"
#include "MyCharacterBase.h"
#include "UDWBridge.h"
#include "GameFramework/Character.h"
#include "Components/CapsuleComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/MeshComponent.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "UnrealClient.h"
#include "Camera/CameraComponent.h"
#include "Scalability.h"
#include "RenderingThread.h"

#if WITH_EDITOR
#include "Editor.h"
#endif

// UDW / UDS property and function names, kept in one place so the log dump can be diffed against
// them in one pass if a UDS update renames something. Every one of these is resolved by reflection
// at runtime; UDWBridge logs a named warning for any that goes missing.
namespace UDWNames
{
	// Weather state values, on UDW's "Manual Weather State" object.
	static const FName Rain(TEXT("Rain"));
	static const FName Snow(TEXT("Snow"));
	static const FName Fog(TEXT("Fog"));
	static const FName CloudCoverage(TEXT("Cloud Coverage"));
	static const FName WindIntensity(TEXT("Wind Intensity"));
	static const FName WindDirection(TEXT("Wind Direction"));
	static const FName ThunderLightning(TEXT("Thunder/Lightning"));
	static const FName MaterialWetness(TEXT("Material Wetness"));
	static const FName MaterialSnowCoverage(TEXT("Material Snow Coverage"));

	// The matching "use the manual value" toggles, on the UDW actor itself.
	static const FName RainOverride(TEXT("Rain - Manual Override"));
	static const FName SnowOverride(TEXT("Snow - Manual Override"));
	static const FName FogOverride(TEXT("Fog - Manual Override"));
	static const FName CloudCoverageOverride(TEXT("Cloud Coverage - Manual Override"));
	static const FName WindIntensityOverride(TEXT("Wind Intensity - Manual Override"));
	static const FName ThunderLightningOverride(TEXT("Thunder/Lightning - Manual Override"));
	static const FName MaterialWetnessOverride(TEXT("Material Wetness - Manual Override"));
	static const FName MaterialSnowCoverageOverride(TEXT("Material Snow Coverage - Manual Override"));

	// Particle look / extent, on the UDW actor.
	static const FName RainParticleSpawnCount(TEXT("Rain Particle Spawn Count"));
	static const FName SnowParticleSpawnCount(TEXT("Snow Particle Spawn Count"));
	static const FName RainDropsScale(TEXT("Rain Drops Scale"));
	static const FName SnowFlakesScale(TEXT("Snow Flakes Scale"));
	// UDS 9.6 calls the particle extent "Max Spawn Distance" — there is no "Spawn Radius".
	static const FName MaxSpawnDistance(TEXT("Max Spawn Distance"));
	static const FName MinimumParticleDistance(TEXT("Minimum Particle Distance"));
	static const FName RainWindVelocity(TEXT("Rain Wind Velocity"));
	static const FName SnowWindVelocity(TEXT("Snow Wind Velocity"));
	static const FName RainDropsAlpha(TEXT("Rain Drops Alpha"));
	static const FName SnowFlakesAlpha(TEXT("Snow Flakes Alpha"));

	// Dynamic Landscape Weather Effects, on the UDW actor.
	// There is no master "DLWE Interactions" toggle in 9.6: accumulation is driven purely by
	// Material Snow Coverage plus these depth/area settings, and bEnableDLWE gates whether we
	// write them at all.
	static const FName DLWESnowTrails(TEXT("DLWE Snow Trails Activated"));
	static const FName SnowDepth(TEXT("Snow Depth"));
	static const FName PuddleCoverage(TEXT("Puddle Coverage"));
	static const FName DLWEInteractionArea(TEXT("Snow/Dust Interaction Render Target Area"));
	static const FName DLWEInteractionResolution(TEXT("Snow/Dust Interaction Render Target Resolution"));

	// Misc UDW.
	static const FName InstantWeatherUpdating(TEXT("Instant Weather Updating"));
	static const FName CustomControlPointLocation(TEXT("Custom Control Point Location"));
	static const FName ControlPointLocationSource(TEXT("Control Point Location Source"));

	// Both UDS and UDW expose "Refresh Settings" as a bool rather than an update *function*: setting
	// it true is how you tell them to re-apply after writing properties directly. There is no
	// "Force Update" or "Set Time of Day" function on either class in 9.6 — the first run's log
	// confirmed both are absent.
	static const FName RefreshSettings(TEXT("Refresh Settings"));

	// Ultra Dynamic Sky.
	static const FName TimeOfDay(TEXT("Time of Day"));
	static const FName AnimateTimeOfDay(TEXT("Animate Time of Day"));
	static const FName DayLength(TEXT("Day Length"));
	// The non-simulated sun's sunrise and sunset, Time of Day units.
	static const FName DawnTime(TEXT("Dawn Time"));
	static const FName DuskTime(TEXT("Dusk Time"));
	static const FName SimulateRealSun(TEXT("Simulate Real Sun"));
	static const FName Latitude(TEXT("Latitude"));
	static const FName Longitude(TEXT("Longitude"));
	static const FName TimeZone(TEXT("Time Zone"));
	static const FName Month(TEXT("Month"));
	static const FName Day(TEXT("Day"));
	static const FName Year(TEXT("Year"));
	static const FName NorthYaw(TEXT("North Yaw"));
	static const FName DaylightSavingsTime(TEXT("Daylight Savings Time"));
	static const FName SunYaw(TEXT("Sun Yaw"));
	static const FName SunPitch(TEXT("Sun Pitch"));
	static const FName SunLightIntensity(TEXT("Sun Light Intensity"));
	// The sun disc's size, and UDS's switch that otherwise sizes it from the real-sun simulation.
	static const FName SunScale(TEXT("Sun Scale"));
	static const FName SimulationControlsSunScale(TEXT("Simulation Controls Sun Scale"));
	// UDS's sun lens flare, and the function that applies a change to it.
	static const FName EnableSunLensFlare(TEXT("Enable Sun Lens Flare"));
	static const FName ApplyLensFlare(TEXT("Static Properties - Lens Flare"));
	static const FName ManuallyPositionSunTarget(TEXT("Manually Position Sun Target"));
	// UDS has no "Cloud Density"; the volumetric cloud density control is "Extinction Scale".
	static const FName CloudExtinctionScale(TEXT("Extinction Scale"));
	static const FName CloudDirection(TEXT("Cloud Direction"));

	// UDS fog. The weather state's Fog value feeds these; scaling them is how fog is made to read
	// from an aerial viewpoint, where the default start distance leaves the near field clear.
	// Cloud and fog altitudes are absolute world Z, unlike UDW's particle spawn box which is already
	// relative to the control point. That asymmetry is why precipitation follows the drone anywhere
	// on the globe but the cloud deck does not.
	// The volumetric cloud layer, in kilometres. "Bottom Altitude" is the authored input; UDS turns
	// it into the renderer's LayerBottomAltitude inside "Static Properties - Volumetric Clouds
	// Altitude", which is why writing the component directly was being overwritten.
	static const FName CloudBottomAltitude(TEXT("Bottom Altitude"));
	static const FName OnRepBottomAltitude(TEXT("OnRep_Bottom Altitude"));
	static const FName ApplyCloudAltitude(TEXT("Static Properties - Volumetric Clouds Altitude"));

	// Post Process Wind Fog: UDW's own description is "a post process effect which renders moving fog
	// noise with weather and wind, for extra volume in foggy/stormy weather", and it "moves the 3D
	// noise samples with wind/particles". That is the drifting, cloud-like fog that static height fog
	// cannot produce.
	static const FName PostProcessWindFog(TEXT("Post Process Wind Fog"));
	static const FName PPWFIntensityScale(TEXT("PPWF Intensity Scale"));
	static const FName PPWFIntensityFromFog(TEXT("PPWF Intensity from Fog"));
	static const FName PPWFIntensityFromWind(TEXT("PPWF Intensity From Wind"));
	static const FName PPWFNoiseScale(TEXT("PPWF 3D Noise Scale"));
	static const FName PPWFMaxSampleDistance(TEXT("PPWF Max Sample Distance"));
	static const FName PPWFSampleStepDistance(TEXT("PPWF Sample Step Distance"));

	// Teleporting the drone leaves UDW's particle spawn box empty at the new location, and it refills
	// only at the normal spawn rate. This forces the Niagara systems forward so the volume is already
	// populated. UDW's own note: "Advance the niagara systems to update them instantly with
	// particles. An expensive operation." - acceptable here, since correctness beats speed offline.
	// When true UDW ramps material wetness and snow coverage in over their own durations. That suits
	// continuous play and defeats this pipeline: a condition lasts about eleven seconds, so the
	// capture would catch bare roofs part-way through the ramp. Off means the material state follows
	// the weather state directly, which is what a per-condition dataset needs.
	static const FName SimulateMaterialStateOverTime(TEXT("Simulate Changing Material State Over Time"));

	// Lightning. The full-screen yellow flash is the bloom post-process; "Spawn Lightning Flashes" is
	// the master switch for the whole effect including the bolts.
	static const FName SpawnLightningFlashes(TEXT("Spawn Lightning Flashes"));
	static const FName LightningFlashBloomIntensity(TEXT("Lightning Flash Light Bloom Intensity"));
	static const FName LightningFlashLightIntensity(TEXT("Lightning Flash Light Intensity"));

	static const FName WarmUpNiagaraSystems(TEXT("Warm Up Niagara Systems"));
	static const FName WarmUpParticlesOnBeginPlay(TEXT("Warm Up Weather Particles on Begin Play"));
	static const FName UpdateParticlesParentLocation(TEXT("Update Weather Particles Parent Location"));

	// Read back for the per-condition fog diagnostic. Fog reaches the screen through several
	// independent channels, and haze arriving via one looks identical to haze arriving via another -
	// which is why this needed measuring rather than reasoning about.
	// UDS adds to the height fog density from the weather state, on top of Base Fog Density. These
	// are the addends, and they are what makes the component rise while the base stays constant.
	// "Toggle off to completely disable the exponential height fog", per UDS's own tooltip. A direct
	// bool with no derived value behind it, which is what makes it the only per-condition control
	// here that the update chain cannot undo.
	static const FName RenderExponentialHeightFog(TEXT("Render Exponential Height Fog"));

	static const FName FoggyDensityContribution(TEXT("Foggy Density Contribution"));
	static const FName CloudyDensityContribution(TEXT("Cloudy Density Contribution"));
	static const FName DustDensityContribution(TEXT("Dust Density Contribution"));
	static const FName CurrentFogDensity(TEXT("Current Fog Density"));

	static const FName BaseFogDensity(TEXT("Base Fog Density"));
	static const FName ScaleFogDensity(TEXT("Scale Fog Density"));
	static const FName MinFogStartDistance(TEXT("Min Fog Start Distance"));
	static const FName MaxFogStartDistance(TEXT("Max Fog Start Distance"));
	static const FName UseVolumetricFog(TEXT("Use Volumetric Fog"));
	static const FName VolumetricFogDistance(TEXT("Volumetric Fog Distance"));
	static const FName VolumetricFogExtinction(TEXT("Volumetric Fog Extinction"));

	// Height fog falloff, per weather state. UDS blends these four by how cloudy/foggy/dusty it is
	// and pushes the result to the component's FogHeightFalloff every refresh - so the component
	// itself must not be written directly, only these source values. Verified against the asset's
	// own tooltips, which are identical for all four bar the leading clause.
	static const FName BaseHeightFogFalloff(TEXT("Base Height Fog Falloff"));
	static const FName CloudyHeightFogFalloff(TEXT("Cloudy Height Fog Falloff"));
	static const FName FoggyHeightFogFalloff(TEXT("Foggy Height Fog Falloff"));

	// Where the fog takes its colour from. A UDS_FogColorMode enum: "Automatic", "Sky Atmosphere",
	// "None". Set by display name rather than index - the underlying entries are the unhelpful
	// NewEnumerator0/1/2, so the index carries no meaning and could be reordered by a UDS update.
	static const FName FogColorMode(TEXT("Fog Color Mode"));

	// Strength of the sky-atmosphere contribution to fog colour, split by which body is up. UDS's
	// range is 0-4. "None" applies when neither the sun nor the moon is above the horizon.
	static const FName SkyAtmoFogInfluenceSun(TEXT("Sky Atmosphere Fog Influence Sun"));
	static const FName SkyAtmoFogInfluenceMoon(TEXT("Sky Atmosphere Fog Influence Moon"));
	static const FName SkyAtmoFogInfluenceNone(TEXT("Sky Atmosphere Fog Influence None"));

	// Cloud movement. "Cloud Speed" scales how fast the deck drifts; "Formation Change Speed" scales
	// how fast the shapes themselves morph as they go. Direction is deliberately absent: UDS's own
	// tooltip states "Control of this variable will be taken by Ultra Dynamic Weather if you have one
	// in your level. UDW controls cloud direction using its Wind Direction" - so writing
	// "Cloud Direction" here would be overwritten, and the per-condition WindDirection is the way in.
	static const FName CloudSpeed(TEXT("Cloud Speed"));
	static const FName CloudFormationChangeSpeed(TEXT("Formation Change Speed"));
}

// Sets default values
AMyActor_weather::AMyActor_weather()
{
	// Set this actor to call Tick() every frame. You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
}

// Called when the game starts or when spawned
void AMyActor_weather::BeginPlay()
{
	Super::BeginPlay();

	// First, before anything reads a world position. Moving the georeference relocates the entire
	// globe under the level, so every altitude, ground trace and cloud anchor computed before this
	// would describe the old location.
	ApplyGeoreferenceOrigin();

	// Volumetric clouds render to a downsampled target and are reprojected between frames. That is a
	// sensible default for a game, but it is what makes the deck read as regular rectangular blocks in
	// PIE while the editor viewport looks smooth - the blocks are the reprojection grid, not the
	// cloud. For stills being captured as data, resolution matters more than the frame cost.
	if (bHighQualityVolumetricClouds)
	{
		if (UWorld* W = GetWorld())
		{
			// Mode 0 renders the clouds at full resolution with no reprojection.
			GEngine->Exec(W, TEXT("r.VolumetricRenderTarget.Mode 0"));

			// More samples along the view ray: the shipped 2 is what leaves banding in the deck.
			GEngine->Exec(W, TEXT("r.VolumetricCloud.DistanceToSampleMaxCount 16"));

			UE_LOG(LogTemp, Log,
				TEXT("[Clouds] Full-resolution volumetric clouds: VolumetricRenderTarget.Mode 0, "
				     "DistanceToSampleMaxCount 16. Costs frame time, removes the reprojection blocks."));
		}
	}

	ResolveOutputDirs();
	BuildDefaultWeatherSteps();
	InitializeCharacterReference();
}

void AMyActor_weather::ResolveOutputDirs()
{
	const FString Root = OutputRootDir.IsEmpty()
		? FPaths::ProjectSavedDir() / TEXT("WeatherPipeline")
		: OutputRootDir;

	ResolvedRecordingsDir = Root / TEXT("Recordings/");
	ResolvedSingleCamDir  = Root / TEXT("");
	ResolvedSnapshotDir   = Root / TEXT("WeatherSnapshots/");

	// Optional per-run subfolder inside WeatherSnapshots. Capture filenames are built from condition
	// and waypoint index and carry nothing about location, so two runs over different cities produce
	// identical names - and the second overwrites the first image for image, metadata included.
	// Sanitised rather than trusted: this reaches the filesystem and can come from a config file.
	if (!OutputSubfolder.IsEmpty())
	{
		ResolvedSnapshotDir = ResolvedSnapshotDir / FPaths::MakeValidFileName(OutputSubfolder) / TEXT("");
	}

	IFileManager& FM = IFileManager::Get();
	FM.MakeDirectory(*ResolvedRecordingsDir, true);
	FM.MakeDirectory(*ResolvedSnapshotDir, true);

	UE_LOG(LogTemp, Log, TEXT("WeatherPipeline output root: %s  (snapshots -> %s)"),
		*Root, *ResolvedSnapshotDir);
}

void AMyActor_weather::InitializeCharacterReference()
{
	// In PIE the player possesses the weather pawn, so GetPlayerCharacter returns it. In Simulate
	// In Editor there is no possession, so fall back to finding the weather character by class.
	// Whether we found it via the player tells us which mode we're in (drives auto-start below).
	bool bFoundViaPlayer = false;
	TargetThirdPersonCharacter = UGameplayStatics::GetPlayerCharacter(this, 0);
	if (TargetThirdPersonCharacter)
	{
		bFoundViaPlayer = true;
	}
	else
	{
		TargetThirdPersonCharacter = Cast<ACharacter>(
			UGameplayStatics::GetActorOfClass(GetWorld(), AMyCharacterBase::StaticClass()));
	}
	if (!TargetThirdPersonCharacter)
	{
		UE_LOG(LogTemp, Warning, TEXT("AMyActor_weather: Weather character not found yet (will retry)."));
	}

	if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		FIntPoint Size = GEngine->GameViewport->Viewport->GetSizeXY();
		ViewportWidth  = Size.X;
		ViewportHeight = Size.Y;
		UE_LOG(LogTemp, Log, TEXT("Viewport synced: %d x %d"), ViewportWidth, ViewportHeight);
	}

	// Resolve the Ultra Dynamic Weather / Sky actors placed in the level. These are the only source
	// of weather now — there is no fallback particle system to fall back to.
	const bool bHaveUDW = ResolveUDWActor();
	const bool bHaveUDS = ResolveUDSActor();

	if (bHaveUDW && bLogUDWProperties)
	{
		// The authoritative list of names this build can actually write, and can actually call.
		// Reconcile UDWNames against this dump whenever Ultra Dynamic Sky is updated. Off by default
		// because it is ~1500 log lines; turn it on for one run after a UDS upgrade.
		UDWBridge::DumpTunableProperties(UDWActor.Get(), TEXT("Ultra_Dynamic_Weather"));
		UDWBridge::DumpCallableFunctions(UDWActor.Get(), TEXT("Ultra_Dynamic_Weather"));
		UDWBridge::DumpTunableProperties(UDWManualState.Get(), TEXT("Manual Weather State"));
		if (bHaveUDS)
		{
			UDWBridge::DumpTunableProperties(UDSActor.Get(), TEXT("Ultra_Dynamic_Sky"));
			UDWBridge::DumpCallableFunctions(UDSActor.Get(), TEXT("Ultra_Dynamic_Sky"));
		}
	}

	EnforceSingleSkySource();

	if (bHaveUDW && bForceInstantWeatherChange)
	{
		// Without this UDW eases toward a new state over several seconds, so a screenshot taken
		// shortly after a step would show a blend of two steps rather than the one in the metadata.
		UDWBridge::SetBool(UDWActor.Get(), UDWNames::InstantWeatherUpdating, true);
	}

	// Set here rather than inside SetupDLWE, which returns early when DLWE is disabled - the material
	// ramp applies to wetness and snow coverage whether or not ground interaction is in use.
	if (bHaveUDW)
	{
		UDWBridge::SetBool(UDWActor.Get(), UDWNames::SimulateMaterialStateOverTime,
			bSimulateMaterialStateOverTime);

		// Start with a populated particle volume rather than one that fills over the first seconds of
		// play. (Was inside SetupDLWE, which is now off by default.)
		UDWBridge::SetBool(UDWActor.Get(), UDWNames::WarmUpParticlesOnBeginPlay, true);

		if (bDisableLightningFlashes)
		{
			AActor* UDW = UDWActor.Get();

			// The master switch stops bolts and flashes being spawned at all.
			UDWBridge::SetBool(UDW, UDWNames::SpawnLightningFlashes, false);

			// Zero the bloom and flash light as well, so anything already in flight when a run starts
			// - or spawned by a path that ignores the switch - cannot blow out a capture.
			UDWBridge::SetFloat(UDW, UDWNames::LightningFlashBloomIntensity, 0.0f);
			UDWBridge::SetFloat(UDW, UDWNames::LightningFlashLightIntensity, 0.0f);

			UE_LOG(LogTemp, Log, TEXT("Lightning flashes disabled - no bolts and no screen bloom."));
		}

		UE_LOG(LogTemp, Log, TEXT("Material state ramp %s - surfaces %s the weather state."),
			bSimulateMaterialStateOverTime ? TEXT("on")  : TEXT("off"),
			bSimulateMaterialStateOverTime ? TEXT("ease toward") : TEXT("follow"));
	}

	// Check the configuration before anything acts on it, so a bad value is reported once, up front,
	// rather than inferred hours later from images that look plausible and are wrong.
	LogEnvironmentCapabilities();
	ValidateSettings();

	// Cesium warns about this on every tileset load, and it is right to: world bounds checks cull
	// actors that stray outside the world extent, and a georeferenced globe puts positions far
	// outside the default bounds. Left on, the drone can be destroyed mid-run for flying "too far"
	// from an origin that has no meaning on a planet. Disabled here rather than left to the level's
	// World Settings so a fresh level cannot silently reintroduce it.
	if (UWorld* World = GetWorld())
	{
		if (AWorldSettings* Settings = World->GetWorldSettings())
		{
			if (Settings->bEnableWorldBoundsChecks)
			{
				Settings->bEnableWorldBoundsChecks = false;
				UE_LOG(LogTemp, Log,
					TEXT("World bounds checks disabled - they cull actors outside the world extent, "
					     "which a georeferenced globe routinely exceeds."));
			}
		}
	}

	SetupDLWE();
	SetMovingFog(bEnableMovingFog, MovingFogIntensityScale);
	ApplyRenderQuality();
	ConfigureCesiumForCapture();

	// Baseline sky before any weather runs, so the level looks right even pre-capture.
	if (bHaveUDS)
	{
		ApplySkySettings(FUDWWeatherStep());
	}

	SetupCaptureComponent();
	SetupCameraDebugLog();

	// Hide every visible part of the drone/camera so nothing of it appears on screen or in the
	// captured images — including the blue editor sprite/billboard of the camera & SceneCapture
	// components, which sits at view center. UDW spawns its weather particles on its own actor
	// around the control point, not on this character, so everything here can be hidden.
	if (TargetThirdPersonCharacter)
	{
		// Remember where the drone started. The default orbit and the line passes are built around
		// this, so it has to be where the drone will actually fly from.
		//
		// ApplySpawnHeight updates it when it repositions the drone: it runs later, once the tiles
		// have loaded, and without that update the waypoints keep this authored Z while the drone
		// moves to ground + clearance - captures then happen at a different altitude from the one
		// the spawn logic chose.
		CharacterStartTransform = TargetThirdPersonCharacter->GetActorTransform();
		bHaveCharacterStart     = true;

		TArray<UPrimitiveComponent*> Prims;
		TargetThirdPersonCharacter->GetComponents<UPrimitiveComponent>(Prims);
		for (UPrimitiveComponent* Prim : Prims)
		{
			if (Prim)
			{
				Prim->SetHiddenInGame(true);
			}
		}
		UE_LOG(LogTemp, Log, TEXT("AMyActor_weather: hid %d drone/camera primitive(s)."), Prims.Num());
	}

	// Auto-start the capture run on a timer (there is no P key without player input). Triggers when
	// explicitly enabled via bAutoStartPipeline, OR automatically in Simulate In Editor — detected
	// by the character having been found by class rather than via player possession. Guarded so it
	// schedules only once across init retries. Normal PIE keeps using the P key.
	const bool bShouldAutoStart = bAutoStartPipeline || IsHeadlessAutoStart() ||
		(TargetThirdPersonCharacter && !bFoundViaPlayer);

	// Automated runs take no input at all: the frame is chosen by the pipeline.
	if (IsHeadlessAutoStart())
	{
		SetPlayerInputLocked(true);
	}

	// Multi-site: the first site is the level's own location (saved there beforehand), so the globe
	// stays put; only its folder, clock and conditions are made current.
	if (CaptureSites.Num() > 0 && CurrentSiteIndex == 0)
	{
		ApplyCaptureSite(0, /*bMoveGlobe=*/false);
	}

	// Manual frame: settle the geo-accurate height now, before the user flies, so the terrain never
	// moves under the frame they choose.
	if (bManualFrame && !bSceneSetupRequested && !bGroundCalculationPending && !bOriginFromCalculation)
	{
		ResetTilesetOffsets();
		StartGroundHeightCalculation();
	}
	if (bShouldAutoStart && !bAutoStartScheduled && TargetThirdPersonCharacter)
	{
		bAutoStartScheduled = true;
		const float Delay = FMath::Max(0.1f, AutoStartDelaySeconds);
		GetWorldTimerManager().SetTimer(AutoStartTimerHandle, this, &AMyActor_weather::StartWeatherCapture, Delay, false);
		UE_LOG(LogTemp, Log, TEXT("AMyActor_weather: auto-starting capture in %.1fs (%s)."),
			Delay, bFoundViaPlayer ? TEXT("bAutoStartPipeline") : TEXT("Simulate In Editor / no possession"));
	}
	else if (!bShouldAutoStart && bLitmusOnPlay && !bLitmusOnPlayScheduled && TargetThirdPersonCharacter)
	{
		// Preview every condition where the drone stands; the run waits for P.
		bLitmusOnPlayScheduled = true;
		bLitmusPreviewOnly     = true;
		GetWorldTimerManager().SetTimer(LitmusHandle, this, &AMyActor_weather::RunWeatherLitmusTest,
			FMath::Max(0.1f, AutoStartDelaySeconds), false);
		UE_LOG(LogTemp, Log, TEXT("[Litmus] Previewing every condition at the drone's current position; "
			"press P to start the capture run."));
	}
}

// Called every frame
void AMyActor_weather::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Place the drone at its configured clearance above the terrain - once a run has been requested
	// (BeginSceneSetupThenRun), so Play itself leaves the drone where it was. Self-disabling after.
	// Manual frame: place the drone at its starting height at Play, above the buildings, so it can be
	// flown from there - left where the level puts it, it starts at street level and cannot climb out.
	// Not while a multi-site move is settling: the next site's ground calculation has not started yet.
	if ((bSceneSetupRequested || bManualFrame) && !GetWorldTimerManager().IsTimerActive(SiteSettleTimerHandle))
	{
		ApplySpawnHeight();
	}

	// Continuous flight between waypoints (bFlyBetweenWaypoints). A bool test when not flying.
	TickGlide(DeltaTime);

	// Live height readout while idle, for verifying the altitude maths by flying rather than by
	// reading a log afterwards. Self-throttling, and silent once a run starts.
	ReportLiveAltitude(DeltaTime);

	if (bCapturePending) {
		StartCapture(bCaptureFrameRate);
	}
	if (bStreamPending) {
		StartStreamRTSP(streamRate);
	}

	if (bMoveCamera) {
		MoveObj(DeltaTime);
	}
	if (bCaptureActive) {
		TimeSinceLastCapture += DeltaTime;
		if (TimeSinceLastCapture >= CaptureInterval) {
			SaveActiveCameraImage();
			TimeSinceLastCapture = 0.0f; // reset timer
		}
	}
	// Pawn may spawn after this actor (GameMode-spawned pawns), or be a non-weather pawn.
	// Retry the full init only once the player character actually exists (avoids log spam).
	if (!TargetThirdPersonCharacter && UGameplayStatics::GetPlayerCharacter(this, 0))
	{
		InitializeCharacterReference();
	}
	AMyCharacterBase* Char = Cast<AMyCharacterBase>(TargetThirdPersonCharacter);
	if (!Char)
	{
		return; // no weather character available — nothing to poll
	}
	if (Char->recImg) {
		SaveActiveCameraImage(true);
		Char->recImg = false;
	}
	if (Char->recImg_resize) {
		SaveActiveCameraImage();
		Char->recImg_resize = false;
	}

	if (bStreamActive) {
		TimeSinceLastImgStream += DeltaTime;
		if (TimeSinceLastImgStream >= StreamInterval) {
			StreamRTSP();
			TimeSinceLastImgStream = 0.0f; // reset stream timer
		}
	}
	if (bRotateCamera) {
		
		RotateObj(DeltaTime);
	}

	if (Streamer.ReceiveMetadata()) {
		ShareRate(DeltaTime);
	}

	// The rain/snow/fog toggles below are manual Blueprint debug switches. They start a full phase,
	// which shares SnapshotTimerHandle and ActiveCaptureStep with the litmus preview and the capture
	// run. If one fires mid-sequence the two state machines interleave and a snapshot ends up
	// labelled with one weather type but carrying another's parameters, so they are consumed and
	// ignored while a sequence is running.
	const bool bSequenceOwnsState = IsSequenceRunning();
	if (bSequenceOwnsState && (Char->rainVar || Char->snowVar || Char->fogVar))
	{
		Char->rainVar = false;
		Char->snowVar = false;
		Char->fogVar  = false;
		UE_LOG(LogTemp, Warning,
			TEXT("Ignoring rain/snow/fog debug toggle: a weather sequence is already running."));
	}

	if (Char->rainVar) {
		if (!rainState) {
			StartRain();
			rainState = true;
		}
		else {
			StopRain();
			rainState = false;
		}
		Char->rainVar = false;
	}

	if (Char->snowVar) {
		if (!snowState) {
			StartSnow();
			snowState = true;
		}
		else {
			StopSnow();
			snowState = false;
		}
		Char->snowVar = false;
	}

	if (Char->fogVar) {
		if (!fogState) {
			StartFog();
			fogState = true;
		}
		else {
			StopFog();
			fogState = false;
		}
		Char->fogVar = false;
	}

	if (Char->litmusVar)
	{
		RunWeatherLitmusTest();
		Char->litmusVar = false;
	}

	// P toggles: first press starts the run, second press cancels it. Consume the flag before acting
	// so a press during teardown cannot re-trigger on the following frame.
	if (Char->pipelineVar)
	{
		Char->pipelineVar = false;

		if (IsSequenceRunning())
		{
			// P during the Play preview means "start the run now", not "cancel".
			const bool bWasPreview = bLitmusActive && bLitmusPreviewOnly;
			StopWeatherCapture();
			if (bWasPreview)
			{
				StartWeatherCapture();
			}
		}
		else
		{
			StartWeatherCapture();
		}
	}

	// The drone moves every frame, so the weather has to follow it every frame.
	UpdateWeatherControlPoint();
}

// ---------------------------------------------------------------------------------------------
// Ultra Dynamic Weather / Sky plumbing
// ---------------------------------------------------------------------------------------------

bool AMyActor_weather::ResolveUDWActor()
{
	if (UDWActor.IsValid() && UDWManualState.IsValid())
	{
		return true;
	}

	if (AActor* Found = UDWBridge::FindWeatherActor(GetWorld()))
	{
		UDWActor = Found;
		UDWManualState = UDWBridge::GetManualWeatherState(Found);
		return UDWManualState.IsValid();
	}
	return false;
}

bool AMyActor_weather::ResolveUDSActor()
{
	if (UDSActor.IsValid())
	{
		return true;
	}

	if (AActor* Found = UDWBridge::FindSkyActor(GetWorld()))
	{
		UDSActor = Found;
		return true;
	}
	return false;
}

FString AMyActor_weather::WeatherKindToString(EWeatherKind Kind)
{
	switch (Kind)
	{
	case EWeatherKind::Rain: return TEXT("Rain");
	case EWeatherKind::Snow: return TEXT("Snow");
	case EWeatherKind::Fog:  return TEXT("Fog");
	default:                 return TEXT("Unknown");
	}
}

const TArray<FUDWWeatherStep>& AMyActor_weather::StepsFor(EWeatherKind Kind) const
{
	switch (Kind)
	{
	case EWeatherKind::Snow: return SnowSteps;
	case EWeatherKind::Fog:  return FogSteps;
	case EWeatherKind::Rain:
	default:                 return RainSteps;
	}
}

void AMyActor_weather::SetupDLWE()
{
	if (!bEnableDLWE || bDLWEInitialized || !UDWActor.IsValid())
	{
		return;
	}

	AActor* UDW = UDWActor.Get();

	// Trails carved through accumulated snow. Note none of this becomes *visible* unless the
	// surface's material includes the UDS DLWE material functions (DLWE_SnowCoverage etc.) — the
	// writes succeed either way, so check the rendered image, not just the log, when tuning.
	UDWBridge::SetBool(UDW, UDWNames::DLWESnowTrails, bEnableSnowTrails);

	// The square (centred on the control point) inside which interactions exist. If the character
	// travels beyond this during a phase, trails stop at a visible hard edge.
	UDWBridge::SetFloat(UDW, UDWNames::DLWEInteractionArea, DLWEAreaSize);
	UDWBridge::SetInt(UDW, UDWNames::DLWEInteractionResolution, DLWETargetResolution);

	bDLWEInitialized = true;
	UE_LOG(LogTemp, Log, TEXT("AMyActor_weather: DLWE configured (area=%.0fcm res=%d trails=%d)."),
		DLWEAreaSize, DLWETargetResolution, bEnableSnowTrails ? 1 : 0);
}

void AMyActor_weather::EnforceSingleSkySource()
{
	if (!bDisableConflictingSkyActors)
	{
		return;
	}

	AActor* UDS = UDSActor.Get();

	// UDS owns its own sun, sky light, atmosphere and volumetric clouds as components. A *separate*
	// actor of any of these types double-lights the scene — that is exactly what produced the
	// red/yellow and brown haze in this project before UDS was adopted. Today's Ingolstadt level has
	// none, so this is a regression guard: if one is ever dragged in, it shows up as one log line
	// instead of as inexplicably wrong imagery.
	auto DisableActorsOfClass = [this, UDS](UClass* Class, const TCHAR* Label)
	{
		TArray<AActor*> Found;
		UGameplayStatics::GetAllActorsOfClass(GetWorld(), Class, Found);
		for (AActor* Actor : Found)
		{
			if (!Actor || Actor == UDS || Actor->GetOwner() == UDS || Actor->GetAttachParentActor() == UDS)
			{
				continue;
			}

			Actor->SetActorHiddenInGame(true);
			UE_LOG(LogTemp, Warning,
				TEXT("EnforceSingleSkySource: disabling conflicting sky actor '%s' (%s) - Ultra Dynamic Sky must be the only sky source."),
				*Actor->GetName(), Label);
		}
	};

	DisableActorsOfClass(ADirectionalLight::StaticClass(),   TEXT("DirectionalLight"));
	DisableActorsOfClass(ASkyLight::StaticClass(),           TEXT("SkyLight"));
	DisableActorsOfClass(ASkyAtmosphere::StaticClass(),      TEXT("SkyAtmosphere"));
	DisableActorsOfClass(AVolumetricCloud::StaticClass(),    TEXT("VolumetricCloud"));
	DisableActorsOfClass(AExponentialHeightFog::StaticClass(), TEXT("ExponentialHeightFog"));
}

void AMyActor_weather::ApplySkySettings(const FUDWWeatherStep& Step)
{
	if (!bDriveSkyFromCode || !ResolveUDSActor())
	{
		return;
	}

	AActor* UDS = UDSActor.Get();

	// A negative override means "use the run-wide clock"; 0 is a real reading (midnight), which is
	// why the sentinel is negative rather than zero like the other fields.
	ActiveCaptureTimeOfDay = (Step.TimeOfDayOverride >= 0.0f) ? Step.TimeOfDayOverride
		: (SpotTimeOfDay >= 0.0f) ? SpotTimeOfDay : TimeOfDay;      // the spot's own time, once at one

	// The sun disc's size, the same in both sun modes.
	UDWBridge::SetBool(UDS, UDWNames::SimulationControlsSunScale, false);
	UDWBridge::SetFloat(UDS, UDWNames::SunScale, SunScale);
	UDWBridge::SetBool(UDS, UDWNames::EnableSunLensFlare, bSunLensFlare);
	UDWBridge::CallFunction(UDS, UDWNames::ApplyLensFlare);

	UDWBridge::SetFloat(UDS, UDWNames::TimeOfDay, ActiveCaptureTimeOfDay);
	UDWBridge::SetBool(UDS, UDWNames::AnimateTimeOfDay, bAnimateTimeOfDay);
	if (bAnimateTimeOfDay)
	{
		UDWBridge::SetFloat(UDS, UDWNames::DayLength, DayLengthMinutes);
	}

	if (bApplySunSettings)
	{
		// The level's own Sun panel. Real-sun simulation is switched OFF, because with it on UDS solves
		// the sun from coordinates and clock and ignores Sun Yaw / Sun Pitch entirely.
		UDWBridge::SetBool(UDS,  UDWNames::SimulateRealSun,           false);
		UDWBridge::SetBool(UDS,  UDWNames::ManuallyPositionSunTarget, false);
		UDWBridge::SetFloat(UDS, UDWNames::SunYaw,                    SunYawDeg);
		UDWBridge::SetFloat(UDS, UDWNames::SunPitch,                  SunPitchDeg);
		UDWBridge::SetFloat(UDS, UDWNames::SunLightIntensity,         SunLightIntensityLux);
		UDWBridge::SetFloat(UDS, UDWNames::DawnTime,                  SunriseTime);
		UDWBridge::SetFloat(UDS, UDWNames::DuskTime,                  SunsetTime);

		float Yaw = 0.0f, Pitch = 0.0f, Lux = 0.0f, Dawn = 0.0f, Dusk = 0.0f;
		UDWBridge::GetFloat(UDS, UDWNames::SunYaw,            Yaw);
		UDWBridge::GetFloat(UDS, UDWNames::SunPitch,          Pitch);
		UDWBridge::GetFloat(UDS, UDWNames::SunLightIntensity, Lux);
		UDWBridge::GetFloat(UDS, UDWNames::DawnTime,          Dawn);
		UDWBridge::GetFloat(UDS, UDWNames::DuskTime,          Dusk);
		UE_LOG(LogTemp, Log,
			TEXT("[Sun] Day-night cycle, real-sun simulation off: sunrise %.0f, sunset %.0f, Sun Yaw %.1f, "
			     "Sun Pitch %.1f, Sun Light Intensity %.1f lux (read back)."), Dawn, Dusk, Yaw, Pitch, Lux);
	}
	else if (bUseGeoreferencedSun)
	{
		// Latitude/Longitude are only consulted when UDS is in real-sun simulation mode, so enable
		// that alongside them rather than silently writing coordinates that have no effect.
		UDWBridge::SetBool(UDS, UDWNames::SimulateRealSun, true);
		UDWBridge::SetFloat(UDS, UDWNames::Latitude, SkyLatitude);
		UDWBridge::SetFloat(UDS, UDWNames::Longitude, SkyLongitude);

		// The time zone has to travel with the coordinates. Time of Day is a LOCAL clock reading, so
		// the real-sun solver needs to know which zone that clock is in - left at the previous site's
		// value, 12:00 over Sydney is solved as though it were 12:00 in Germany and comes out well
		// after dark. That is what "same time of day, night sky" looks like.
		//
		// Derived from longitude (15 degrees per hour, unrounded) rather than from a timezone database:
		// Time of Day is then the sun's own time at the site - 1200 is the sun at its highest.
		const float ZoneHours = (TimeZoneHours > -900.0f)
			? TimeZoneHours
			: SkyLongitude / 15.0f;

		UDWBridge::SetFloat(UDS, UDWNames::TimeZone, ZoneHours);

		// The date, and which way is north - without them UDS solves 26 March 2021 with north along +X
		// (east here), which puts the sun in the wrong part of the sky. The zone already includes any
		// summer time, so UDS must not add its own hour on top.
		UDWBridge::SetInt(UDS, UDWNames::Year, SunYear);
		UDWBridge::SetInt(UDS, UDWNames::Month, SunMonth);
		UDWBridge::SetInt(UDS, UDWNames::Day, SunDay);
		UDWBridge::SetBool(UDS, UDWNames::DaylightSavingsTime, false);
		UDWBridge::SetFloat(UDS, UDWNames::NorthYaw, SunNorthYawDeg);
		UDWBridge::SetFloat(UDS, UDWNames::SunLightIntensity, SunLightIntensityLux);

		UE_LOG(LogTemp, Log,
			TEXT("[Sun] Real sun at %.4f, %.4f on %04d-%02d-%02d, time zone UTC%+.1f (%s), north yaw %.0f. "
			     "Time of Day %.0f is the time at that zone."),
			SkyLatitude, SkyLongitude, SunYear, SunMonth, SunDay, ZoneHours,
			(TimeZoneHours > -900.0f) ? TEXT("configured") : TEXT("from longitude"),
			SunNorthYawDeg, TimeOfDay);
	}

	// Cloud coverage already tracks the weather state; these are for deliberate art direction on top.
	if (Step.CloudDensity > 0.0f)
	{
		UDWBridge::SetFloat(UDS, UDWNames::CloudExtinctionScale, Step.CloudDensity);
	}
	if (Step.CloudDirectionDeg > 0.0f)
	{
		UDWBridge::SetFloat(UDS, UDWNames::CloudDirection, Step.CloudDirectionDeg);
	}

	// Keep the cloud deck and fog layer with the drone, whatever terrain the current Cesium
	// coordinates put underneath it. Re-applied per step because UDS recomputes on its refreshes.
	AnchorWeatherToDrone();

	// Snapshot the fog settings before the first write, so they can be put back on clear.
	CacheSkyFogDefaults();

	// UsePresetFog: write no fog at all and let each preset's own height fog stand.
	//
	// The same problem the cloud layer had. Every value below is written on every step, so all 16
	// conditions ended up sharing one baseline density, one addend and one set of falloffs - the fog
	// UDS authored per preset never reached the frame. That is why a preset looks right in isolation
	// and wrong once the pipeline drives it, and why tuning the falloff by hand changed nothing
	// visible: the density it scales was pinned to the same near-zero number throughout.
	//
	// Per-condition FogOverride still applies through the weather state, so a condition can still ask
	// for more or less fog than its preset - it just no longer replaces the preset's fog profile.
	if (bUsePresetFog)
	{
		if (!bLoggedPresetFog)
		{
			bLoggedPresetFog = true;
			UE_LOG(LogTemp, Log,
				TEXT("[Fog] Using each preset's own height fog: base density, cloud addend and the "
				     "three falloffs are left as UDS authored them, so fog varies by weather."));
		}
	}
	else
	{
	// Baseline haze, independent of the weather. Written every step rather than once at BeginPlay
	// because UDS recomputes its fog on its own refreshes and would otherwise drift back.
	if (BaseFogDensity > 0.0f)
	{
		UDWBridge::SetFloat(UDS, UDWNames::BaseFogDensity, BaseFogDensity);
	}

	// The cloud-driven fog addend. Written every step alongside the base for the same reason - UDS
	// recomputes on its refreshes - and always, including zero, because zero is the whole point:
	// unlike the other fog values a "leave it alone" default would keep UDS's 0.0302 in play.
	UDWBridge::SetFloat(UDS, UDWNames::CloudyDensityContribution, FMath::Max(CloudyFogDensityContribution, 0.0f));

	// How tall the fog slab stands, per weather state. Written next to the density above and for the
	// same reason: UDS recomputes the blended falloff on its own refreshes, so a single write at
	// BeginPlay would drift back. This is what thins the flat band at the horizon - see the header.
	if (HeightFogFalloffClear > 0.0f)
	{
		UDWBridge::SetFloat(UDS, UDWNames::BaseHeightFogFalloff, HeightFogFalloffClear);
	}
	if (HeightFogFalloffCloudy > 0.0f)
	{
		UDWBridge::SetFloat(UDS, UDWNames::CloudyHeightFogFalloff, HeightFogFalloffCloudy);
	}
	if (HeightFogFalloffFoggy > 0.0f)
	{
		UDWBridge::SetFloat(UDS, UDWNames::FoggyHeightFogFalloff, HeightFogFalloffFoggy);
	}
	}   // end of the pinned-fog branch

	// Colour the fog from the rendered atmosphere rather than UDS's authored palette, so the fog and
	// the sky cannot disagree at the horizon. Written per step like the values above, but unlike
	// them it needs no per-frame maintenance: it selects a source, and the renderer resolves the
	// actual colour every frame from the live sun position. Time of day therefore stays correct on
	// its own. SetEnumByDisplayName logs the valid entries if the name ever stops matching.
	if (!FogColorMode.IsEmpty())
	{
		UDWBridge::SetEnumByDisplayName(UDS, UDWNames::FogColorMode, FogColorMode);
	}
	// Skipped on the preset path. UDS authors these three separately - 2.3 / 1.65 / 0.8 in this level -
	// because sun, moon and unlit fog pick up atmospheric colour by different amounts. Writing one
	// number into all three flattens that, and it is part of why the preset's horizon carries a soft
	// graded haze while ours does not.
	if (!bUsePresetsVerbatim && SkyAtmosphereFogInfluence > 0.0f)
	{
		// All three, so the blend survives dusk and night rather than breaking when the sun sets.
		UDWBridge::SetFloat(UDS, UDWNames::SkyAtmoFogInfluenceSun,  SkyAtmosphereFogInfluence);
		UDWBridge::SetFloat(UDS, UDWNames::SkyAtmoFogInfluenceMoon, SkyAtmosphereFogInfluence);
		UDWBridge::SetFloat(UDS, UDWNames::SkyAtmoFogInfluenceNone, SkyAtmosphereFogInfluence);
	}

	// Cloud drift, applied to every condition rather than only the windy ones. Written per step for
	// the same reason as the fog values - UDS recomputes on its refreshes.
	if (CloudSpeed > 0.0f)
	{
		UDWBridge::SetFloat(UDS, UDWNames::CloudSpeed, CloudSpeed);
	}
	if (CloudFormationChangeSpeed > 0.0f)
	{
		UDWBridge::SetFloat(UDS, UDWNames::CloudFormationChangeSpeed, CloudFormationChangeSpeed);
	}

	// Fog strength. The Fog weather value sets how foggy it *is*; these decide whether that reads
	// from a drone's viewpoint, where UDS's default start distance leaves the near field clear and
	// UE's default volumetric fog range stops well short of the far side of the city.
	// Skipped on the preset path. This is a MULTIPLIER over the whole fog profile - the level holds
	// 0.2, and UDS arrives at the fog component's density as Base x Scale. Writing anything else here
	// rescales every fog value at once, so the preset's density, falloff and start distance all land
	// somewhere other than where they were authored.
	if (!bUsePresetsVerbatim && Step.FogDensityScale > 0.0f)
	{
		UDWBridge::SetFloat(UDS, UDWNames::ScaleFogDensity, Step.FogDensityScale);
	}
	// Skipped on the preset path. UDS holds a RANGE here - 0 to 15000 in this level - and interpolates
	// across it by fog amount, which is what gives the haze depth that varies with distance. Writing
	// the same number into both bounds collapses the range to a constant, and a constant start
	// distance is a uniformly spread fog: the "equal distribution" that stops it looking like weather.
	if (!bUsePresetsVerbatim && Step.FogStartDistance >= 0.0f)
	{
		// Both bounds, so the value holds regardless of where UDS interpolates for this fog amount.
		UDWBridge::SetFloat(UDS, UDWNames::MinFogStartDistance, Step.FogStartDistance);
		UDWBridge::SetFloat(UDS, UDWNames::MaxFogStartDistance, Step.FogStartDistance);
	}
	// Not on the preset path. The level has "Use Volumetric Fog" OFF, and turning it on changes how
	// both fog and clouds are lit and distributed - it is what flattens the deck into an evenly spread
	// slab and removes the soft haze the preset shows along the horizon. Enabling a renderer feature
	// the preset was not authored against is not a 1:1 copy of it, whatever the channel values say.
	if (!bUsePresetsVerbatim && Step.VolumetricFogDistance > 0.0f)
	{
		UDWBridge::SetBool(UDS, UDWNames::UseVolumetricFog, true);
		UDWBridge::SetFloat(UDS, UDWNames::VolumetricFogDistance, Step.VolumetricFogDistance);
	}
	if (Step.VolumetricFogExtinction > 0.0f)
	{
		UDWBridge::SetFloat(UDS, UDWNames::VolumetricFogExtinction, Step.VolumetricFogExtinction);
	}

	// Every fog channel, read back live, once per condition. Haze from UDS's height fog, from its
	// weather-fog state, from the cloud fog post process and from UDW's post process wind fog all
	// look the same on screen but come from unrelated systems with unrelated restore paths - so the
	// only way to say which one turns on between two conditions is to print them side by side.
	{
		float LiveBaseDensity = 0.0f, LiveScale = 0.0f, LiveWeatherFog = 0.0f, LivePPWF = 0.0f;

		UDWBridge::GetFloat(UDS, UDWNames::BaseFogDensity,  LiveBaseDensity);
		UDWBridge::GetFloat(UDS, UDWNames::ScaleFogDensity, LiveScale);

		if (AActor* UDWForFog = UDWActor.Get())
		{
			UDWBridge::GetFloat(UDWForFog, UDWNames::Fog,                LiveWeatherFog);
			UDWBridge::GetFloat(UDWForFog, UDWNames::PPWFIntensityScale, LivePPWF);
		}

		float ComponentDensity = -1.0f, ComponentFalloff = -1.0f, ComponentMaxOpacity = -1.0f;
		if (const UExponentialHeightFogComponent* FogComp = GetSkyHeightFogComponent())
		{
			ComponentDensity    = FogComp->FogDensity;
			ComponentFalloff    = FogComp->FogHeightFalloff;
			ComponentMaxOpacity = FogComp->FogMaxOpacity;
		}

		UE_LOG(LogTemp, Log,
			TEXT("[FogChannels] weatherFog=%.2f | heightFog base=%.4f scale=%.2f -> "
			     "component density=%.4f falloff=%.3f maxOpacity=%.2f | PPWF=%.2f"),
			LiveWeatherFog, LiveBaseDensity, LiveScale,
			ComponentDensity, ComponentFalloff, ComponentMaxOpacity, LivePPWF);
	}

	// Cap the far field of the flat height fog slab so it stops forming a false horizon above
	// Cesium's curved one. Applied after the volumetric fog writes above, and independent of them -
	// these two properties are ignored by the volumetric fog path entirely. Re-applied per step for
	// symmetry with the writes above, though unlike them it is not strictly required: UDS has no
	// name for either property and so never overwrites it. Both setters no-op when unchanged.
	ApplyHeightFogFarField();

	// Put the fog plane at a real-world altitude. Must follow the writes above: UDS pushes its own
	// values onto the component during the update chain, and the plane is positional state that
	// nothing else restores.
	AnchorFogPlane();

	// Re-anchor the atmosphere to the globe, once, after the update chain. Applying it before would
	// be undone by UDS repositioning its sky - the suspected cause of the earlier attempt at this
	// looking wrong, and the same ordering rule that the particle counts and lightning obey.
	AnchorSkyAtmosphereToGlobe();

	for (const TPair<FName, float>& Pair : Step.ExtraSkyFloats)
	{
		UDWBridge::SetFloat(UDS, Pair.Key, Pair.Value);
	}
	for (const TPair<FName, bool>& Pair : Step.ExtraSkyBools)
	{
		UDWBridge::SetBool(UDS, Pair.Key, Pair.Value);
	}

	// Cloud coverage, written straight onto UDS as well as into the weather state.
	//
	// Normally UDW publishes coverage to UDS itself, as part of the state push ("UDW State Apply" /
	// "Get UDS Values Controlled by UDW"). Driving UDW through manual overrides updates UDW's own
	// state - which read back correctly all along - but does not reliably reach UDS, so the sky kept
	// rendering clear while UDW believed it was overcast. Writing it here closes that gap.
	// Skipped when presets drive the run. Cloud coverage is what decides cloud SHAPE - scattered puffs
	// against broad streaks - so writing it over a preset gives the preset's fog and light with a
	// different sky's clouds. That is the PIE-vs-preset difference: same weather, wrong cloud form.
	//
	// The write exists because driving UDW through manual channel overrides does not reliably reach
	// UDS. Applying a preset does reach it, through UDS's own UDW link, so the workaround is not
	// needed on this path and actively undoes what the preset asked for.
	if (!bUsePresetsVerbatim)
	{
		UDWBridge::SetFloat(UDS, UDWNames::CloudCoverage, Step.CloudCoverage);
	}

	// Tell UDS to re-apply everything written above. Without this the properties change but the
	// rendered sun/sky/cloud state can lag until UDS's own next periodic refresh.
	UDWBridge::SetBool(UDS, UDWNames::RefreshSettings, true);

	// Read it back. If UDS is recomputing coverage from its own UDW link straight after we write it,
	// this is the line that will say so instead of leaving an empty sky unexplained.
	float LiveSkyCloud = 0.0f;
	if (UDWBridge::GetFloat(UDS, UDWNames::CloudCoverage, LiveSkyCloud))
	{
		if (bUsePresetsVerbatim)
		{
			// Nothing was written, so there is nothing to disagree with - just report what the preset
			// asked for, which is the number that decides the cloud shape on screen.
			UE_LOG(LogTemp, Log, TEXT("[Clouds] Coverage from the preset: %.2f (not overridden)."),
				LiveSkyCloud);
		}
		else if (!FMath::IsNearlyEqual(LiveSkyCloud, Step.CloudCoverage, 0.25f))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("Ultra_Dynamic_Sky cloud coverage did not hold: wrote %.2f, reads %.2f. The sky will not match the weather."),
				Step.CloudCoverage, LiveSkyCloud);
		}
	}
}

void AMyActor_weather::SetMovingFog(bool bEnable, float IntensityScale)
{
	if (!ResolveUDWActor())
	{
		return;
	}

	AActor* UDW = UDWActor.Get();

	// The enum's entries are: Disable / Enable Without Volumetric Fog / Enable / UDS MAX.
	// "Enable" is the full-quality option; the other enabled value exists for projects with
	// volumetric fog switched off at the project level, which this one does not have.
	const TCHAR* Value = bEnable ? TEXT("Enable") : TEXT("Disable");
	if (!UDWBridge::SetEnumByDisplayName(UDW, UDWNames::PostProcessWindFog, Value))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Could not set Post Process Wind Fog to '%s'; leaving it as it was."), Value);
		return;
	}

	if (!bEnable)
	{
		// Zero the intensity as well as the enum. Setting the enum to Disable left PPWF Intensity
		// Scale at UDW's own default of 1.0 - the diagnostic read it back as 1.00 after a disable -
		// and any path that re-arms the effect then finds it at full strength. The enum says whether
		// it renders; this says it would render nothing even if something switched it back on.
		UDWBridge::SetFloat(UDW, UDWNames::PPWFIntensityScale, 0.0f);
		UDWBridge::CallFunction(UDW, FName(TEXT("OnRep_Post Process Wind Fog")));

		UE_LOG(LogTemp, Log,
			TEXT("Post Process Wind Fog disabled and its intensity zeroed. It is a screen-space "
			     "effect scaled by wind and fog, so left on it stays invisible in clear weather and "
			     "appears the moment a windy preset runs."));
		return;
	}

	{
		// Report UDW's own noise settings once. Patch size is governed by the noise scale, and the
		// sensible range for it is only knowable relative to the shipped default - so print it rather
		// than have the value tuned blind.
		if (!bLoggedPPWFDefaults)
		{
			bLoggedPPWFDefaults = true;

			float DefaultNoiseScale = 0.0f, DefaultMaxSample = 0.0f, DefaultStep = 0.0f;
			UDWBridge::GetFloat(UDW, UDWNames::PPWFNoiseScale, DefaultNoiseScale);
			UDWBridge::GetFloat(UDW, UDWNames::PPWFMaxSampleDistance, DefaultMaxSample);
			UDWBridge::GetFloat(UDW, UDWNames::PPWFSampleStepDistance, DefaultStep);

			UE_LOG(LogTemp, Log,
				TEXT("Post Process Wind Fog defaults: noiseScale=%.3f maxSampleDistance=%.0f sampleStep=%.0f. Raise MovingFogNoiseScale above this for larger, patchier banks."),
				DefaultNoiseScale, DefaultMaxSample, DefaultStep);
		}

		// Zero means "leave UDW's own value", so a knob is written only when it is being changed.
		if (IntensityScale             > 0.0f) UDWBridge::SetFloat(UDW, UDWNames::PPWFIntensityScale,    IntensityScale);
		if (MovingFogIntensityFromFog  > 0.0f) UDWBridge::SetFloat(UDW, UDWNames::PPWFIntensityFromFog,  MovingFogIntensityFromFog);
		if (MovingFogIntensityFromWind > 0.0f) UDWBridge::SetFloat(UDW, UDWNames::PPWFIntensityFromWind, MovingFogIntensityFromWind);
		if (MovingFogNoiseScale        > 0.0f) UDWBridge::SetFloat(UDW, UDWNames::PPWFNoiseScale,        MovingFogNoiseScale);
		if (MovingFogMaxSampleDistance  > 0.0f) UDWBridge::SetFloat(UDW, UDWNames::PPWFMaxSampleDistance,  MovingFogMaxSampleDistance);
		if (MovingFogSampleStepDistance > 0.0f) UDWBridge::SetFloat(UDW, UDWNames::PPWFSampleStepDistance, MovingFogSampleStepDistance);
	}

	// Writing the enum stores the value but never fires its RepNotify, so UDW does not rebuild the
	// post-process material and the effect stays invisible despite the setting reading "Enable".
	// Exactly the trap the manual weather overrides hit - call the handlers by hand.
	UDWBridge::CallFunction(UDW, FName(TEXT("OnRep_Post Process Wind Fog")));
	UDWBridge::CallFunction(UDW, FName(TEXT("Updates Needed on PPWF Toggle")));

	ActiveMovingFogIntensity = bEnable ? IntensityScale : 0.0f;
}

bool AMyActor_weather::MeasureGroundBelowDrone(float& OutGroundZ, float& OutHeightAboveGround) const
{
	OutGroundZ = 0.0f;
	OutHeightAboveGround = 0.0f;

	if (!TargetThirdPersonCharacter || !GetWorld())
	{
		return false;
	}

	const FVector Start = TargetThirdPersonCharacter->GetActorLocation();

	// Derived, not measured. The site's ground elevation is a known constant in mean-sea-level terms,
	// and the geoid grid converts that back to the ellipsoid heights Cesium works in - so the ground
	// position follows from arithmetic alone. No trace, no collision, no streamed tiles, and no
	// rooftop under the drone to bias the answer.
	// Bare-earth DEM first, when the level has one. This is terrain by definition - the dataset
	// contains no buildings, so there is no rooftop to land on and nothing to infer. It also works
	// per position rather than assuming the site is flat, so it stays correct over real relief.
	if (bUseBareEarthTerrain)
	{
		TArray<AActor*> Tilesets;
		GatherCesiumTilesets(Tilesets);

		AActor* BareEarth = nullptr;
		for (AActor* Tileset : Tilesets)
		{
			if (Tileset && Tileset->GetName().Contains(BareEarthTilesetName))
			{
				BareEarth = Tileset;
				break;
			}
		}

		if (BareEarth)
		{
			// Every other tileset is ignored, so the photogrammetry mesh - which does contain
			// buildings - cannot supply the hit even though it sits between the drone and the DEM.
			FCollisionQueryParams BareParams(SCENE_QUERY_STAT(WeatherBareEarthProbe), /*bTraceComplex=*/true);
			BareParams.AddIgnoredActor(TargetThirdPersonCharacter);
			for (AActor* Tileset : Tilesets)
			{
				if (Tileset && Tileset != BareEarth)
				{
					BareParams.AddIgnoredActor(Tileset);
				}
			}

			const FVector BareEnd = Start - FVector(0.0f, 0.0f, FMath::Max(GroundTraceDistanceM * 100.0f, 1000.0f));

			FHitResult BareHit;
			if (GetWorld()->LineTraceSingleByChannel(BareHit, Start, BareEnd, ECC_WorldStatic, BareParams))
			{
				OutGroundZ           = static_cast<float>(BareHit.ImpactPoint.Z);
				OutHeightAboveGround = static_cast<float>(Start.Z - BareHit.ImpactPoint.Z);

				if (!bLoggedBareEarth)
				{
					bLoggedBareEarth = true;
					UE_LOG(LogTemp, Log,
						TEXT("Ground height from bare-earth terrain '%s'. Elevations are true terrain "
						     "rather than the photogrammetry surface, so rooftops cannot bias height "
						     "above ground and no flat-site assumption applies."),
						*BareEarth->GetName());
				}
				return true;
			}
		}
	}

	if (bUseSiteGroundElevation)
	{
		// Establish the elevation from the terrain actually present, once. Without this the constant
		// is whatever was typed for the last location - which is how a move to Singapore kept
		// reporting Ingolstadt's 376 m.
		// A deliberate manual override only. Automatic seeding was tried and removed: measuring the
		// photogrammetry surface once and holding it produced a -40 m ground elevation when it
		// sampled placeholder tiles during streaming, and a held value cannot recover from that.
		// Without an override this falls through to a live trace rather than inventing an elevation.
		const bool  bHaveElevation = SiteGroundElevationM > 0.0f;
		const float ElevationMSL   = SiteGroundElevationM;

		FVector DroneGeo(0.0);
		double  GroundWorldZ = 0.0;

		if (bHaveElevation && GetGeographicPosition(Start, DroneGeo))
		{
			// The elevation is orthometric; GetUnrealZForAltitude expects height above the
			// ellipsoid, so the undulation has to be added back on. Dropping this step would place
			// the ground 46 m out at Ingolstadt - the exact error the geoid work removed.
			const float Undulation      = GetGeoidSeparationAt(DroneGeo.Y, DroneGeo.X);
			const float GroundEllipsoid = ElevationMSL + Undulation;

			if (GetUnrealZForAltitude(Start, GroundEllipsoid, GroundWorldZ))
			{
				OutGroundZ           = static_cast<float>(GroundWorldZ);
				OutHeightAboveGround = static_cast<float>(Start.Z - GroundWorldZ);
				return true;
			}
		}

		// No georeference, so there is no way to turn an elevation into a world position. Fall
		// through to the trace rather than returning a fabricated ground height.
		UE_LOG(LogTemp, Verbose,
			TEXT("Site ground elevation needs a Cesium georeference; falling back to a trace."));
	}

	const FVector End   = Start - FVector(0.0f, 0.0f, FMath::Max(GroundTraceDistanceM * 100.0f, 1000.0f));

	FCollisionQueryParams Params(SCENE_QUERY_STAT(WeatherGroundProbe), /*bTraceComplex=*/true);
	Params.AddIgnoredActor(TargetThirdPersonCharacter);

	FHitResult Hit;
	if (!GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Params))
	{
		return false;
	}

	OutGroundZ           = static_cast<float>(Hit.ImpactPoint.Z);
	OutHeightAboveGround = static_cast<float>(Start.Z - Hit.ImpactPoint.Z);
	return true;
}

bool AMyActor_weather::GetGeographicPosition(const FVector& UnrealPosition, FVector& OutLonLatHeight) const
{
	UClass* GeoClass = LoadObject<UClass>(nullptr, TEXT("/Script/CesiumRuntime.CesiumGeoreference"));
	if (!GeoClass)
	{
		return false; // No Cesium in this project - there is no real-world frame to convert into.
	}

	AActor* Georeference = UGameplayStatics::GetActorOfClass(GetWorld(), GeoClass);
	if (!Georeference)
	{
		return false;
	}

	static const FName ToLonLatHeight(TEXT("TransformUnrealPositionToLongitudeLatitudeHeight"));
	return UDWBridge::CallVectorToVector(Georeference, ToLonLatHeight, UnrealPosition, OutLonLatHeight);
}

bool AMyActor_weather::GetUnrealZForAltitude(const FVector& NearPosition, float AltitudeMetres, double& OutZ) const
{
	UClass* GeoClass = LoadObject<UClass>(nullptr, TEXT("/Script/CesiumRuntime.CesiumGeoreference"));
	if (!GeoClass)
	{
		return false;
	}

	AActor* Georeference = UGameplayStatics::GetActorOfClass(GetWorld(), GeoClass);
	if (!Georeference)
	{
		return false;
	}

	// Take the drone's own longitude/latitude and swap in the requested altitude, then convert back.
	// Doing it at the drone's position matters: on a globe a given altitude is a different Unreal Z
	// depending where you are, so resolving it anywhere else would be wrong by the curvature between.
	FVector LonLatHeight;
	if (!GetGeographicPosition(NearPosition, LonLatHeight))
	{
		return false;
	}
	LonLatHeight.Z = AltitudeMetres;

	static const FName ToUnreal(TEXT("TransformLongitudeLatitudeHeightPositionToUnreal"));
	FVector UnrealPos;
	if (!UDWBridge::CallVectorToVector(Georeference, ToUnreal, LonLatHeight, UnrealPos))
	{
		return false;
	}

	OutZ = UnrealPos.Z;
	return true;
}

bool AMyActor_weather::MeasureHeightAboveGroundAt(const FVector& WorldPosition, float& OutHeightMetres) const
{
	OutHeightMetres = 0.0f;
	if (!GetWorld())
	{
		return false;
	}

	const FVector End = WorldPosition - FVector(0.0f, 0.0f, FMath::Max(GroundTraceDistanceM * 100.0f, 1000.0f));

	FCollisionQueryParams Params(SCENE_QUERY_STAT(WeatherAglProbe), /*bTraceComplex=*/true);
	if (TargetThirdPersonCharacter)
	{
		Params.AddIgnoredActor(TargetThirdPersonCharacter);
	}

	FHitResult Hit;
	if (!GetWorld()->LineTraceSingleByChannel(Hit, WorldPosition, End, ECC_WorldStatic, Params))
	{
		return false;
	}

	OutHeightMetres = static_cast<float>((WorldPosition.Z - Hit.ImpactPoint.Z) / 100.0);
	return true;
}

void AMyActor_weather::AnchorWeatherToDrone()
{
	AActor* UDS = UDSActor.Get();
	if (!UDS)
	{
		return;
	}

	// Every mode now needs the drone: the ground-referenced ones trace down from it, and even
	// real-world altitude is resolved at its longitude/latitude, because a given altitude maps to a
	// different Unreal Z depending where on the globe you are.
	if (!bAnchorWeatherToDrone || !TargetThirdPersonCharacter)
	{
		return;
	}

	const double DroneZ = TargetThirdPersonCharacter
		? TargetThirdPersonCharacter->GetActorLocation().Z
		: 0.0;

	// Where the SkyAtmosphere puts the planet surface. UE measures cloud altitude in "kilometres
	// above the ground", and "the ground" is this surface - NOT world Z=0. Treating it as world Z is
	// why a layer computed 1500 m above the drone still rendered below it: the two references were
	// offset by however UDS has positioned its atmosphere.
	double PlanetSurfaceZ = 0.0;
	if (USkyAtmosphereComponent* Atmosphere = UDS->FindComponentByClass<USkyAtmosphereComponent>())
	{
		const double AtmoZ = Atmosphere->GetComponentLocation().Z;
		switch (Atmosphere->TransformMode)
		{
		case ESkyAtmosphereTransformMode::PlanetTopAtAbsoluteWorldOrigin:
			PlanetSurfaceZ = 0.0;
			break;
		case ESkyAtmosphereTransformMode::PlanetTopAtComponentTransform:
			PlanetSurfaceZ = AtmoZ;
			break;
		case ESkyAtmosphereTransformMode::PlanetCenterAtComponentTransform:
			// Centre is one ground-radius below the surface; BottomRadius is in kilometres.
			PlanetSurfaceZ = AtmoZ + static_cast<double>(Atmosphere->BottomRadius) * 100000.0;
			break;
		}
	}

	// Where the base of the cloud layer should sit, in UDS's kilometres.
	float GroundZ = 0.0f, HeightAboveGround = 0.0f;
	bool bHaveGround   = MeasureGroundBelowDrone(GroundZ, HeightAboveGround);
	bool bReusedGround = false;

	// Cesium destroys and rebuilds its collision meshes whenever the tileset re-streams, and the
	// per-condition smooth-normals toggle forces exactly that before every snow condition. For the
	// frames the rebuild takes, a downward trace hits nothing - which is the entire explanation for
	// the "no ground found" warnings, every one of which follows a smooth-normals line in the log.
	//
	// The terrain has not moved, so the last measurement is still correct. Reusing it is strictly
	// better than the old fallback to a drone-relative height, which quietly changed what the cloud
	// altitude was measured from partway through a run and made snow conditions inconsistent with
	// every other condition at the same waypoint.
	if (!bHaveGround && bHaveLastGoodGround)
	{
		GroundZ           = static_cast<float>(LastGoodGroundZ);
		HeightAboveGround = static_cast<float>(DroneZ - LastGoodGroundZ);
		bHaveGround       = true;
		bReusedGround     = true;
	}
	else if (bHaveGround)
	{
		LastGoodGroundZ     = GroundZ;
		bHaveLastGoodGround = true;
	}

	// UsePreset: write nothing and let the preset's own cloud layer stand.
	//
	// Every other mode pins one altitude for every condition, which is what made the deck read as a
	// slab at a fixed height regardless of the weather. UDS authors a different layer altitude and
	// thickness per preset - a storm deck sits lower than fair-weather cloud - and overwriting that
	// with a single number discards the part of the preset that made it look like weather.
	//
	// A per-condition override still applies, so valley fog can still be pulled down to the terrain.
	if (CloudAltitudeMode == ECloudAltitudeMode::UsePreset && !bHaveActiveCloudHeightOffset)
	{
		if (!bLoggedPresetCloudAltitude)
		{
			bLoggedPresetCloudAltitude = true;
			UE_LOG(LogTemp, Log,
				TEXT("[Cloud] Altitude mode is UsePreset: cloud layer altitude and thickness are left "
				     "to each UDS preset, so they vary by weather instead of sitting at one height."));
		}
		return;
	}

	// A per-condition height wins over the actor setting, and is always ground-relative - it exists
	// so a condition can put clouds down at terrain level and have them read as fog.
	const bool   bConditionOverride  = bHaveActiveCloudHeightOffset;
	const float  HeightAboveGroundM  = bConditionOverride ? ActiveCloudHeightOffsetM : CloudHeightAboveGroundM;
	const double HeightAboveGroundCm = static_cast<double>(HeightAboveGroundM) * 100.0;

	// Sample the terrain once and keep it. Re-measuring per capture is what made the layer ride the
	// relief; never measuring is what made it need retuning after every coordinate change.
	// The spawn ground first: it is probed rooftop-proof and is what the sky is anchored to, so cloud
	// base and haze share one reference. The single trace below is only the fallback.
	if (!bHaveCachedTerrainZ && bHaveSpawnGround)
	{
		CachedTerrainZ      = static_cast<float>(SpawnGroundZ);
		bHaveCachedTerrainZ = true;
	}
	if (!bHaveCachedTerrainZ && bHaveGround)
	{
		CachedTerrainZ      = GroundZ;
		bHaveCachedTerrainZ = true;
	}

	// The height for this layer, in the frame the mode works in. A condition's override simply
	// replaces it, so -60 with the mode on AboveDrone means 60 m below the camera.
	const float HeightM =
		bConditionOverride                                            ? ActiveCloudHeightOffsetM :
		(CloudAltitudeMode == ECloudAltitudeMode::AboveDrone)         ? CloudHeightAboveDroneM   :
		(CloudAltitudeMode == ECloudAltitudeMode::RealWorldAltitude)  ? CloudAltitudeM           :
		                                                                CloudHeightAboveGroundM;

	// The world Z the cloud base should end up at.
	double TargetWorldZ = 0.0;
	if (bConditionOverride && bActiveCloudHeightFromGround)
	{
		// Ground-referenced regardless of the actor's mode: fog belongs at terrain level whatever
		// altitude the drone is at, so that it fills the valleys and the peaks stand out of it.
		const double BaseZ = bHaveGround ? static_cast<double>(GroundZ) : DroneZ;
		TargetWorldZ = BaseZ + static_cast<double>(HeightM) * 100.0;
	}
	else if (CloudAltitudeMode == ECloudAltitudeMode::RealWorldAltitude && !bConditionOverride)
	{
		// Resolved through the georeference at the drone's own longitude/latitude, and treated as
		// mean sea level so it agrees with every other altitude the pipeline reports. Previously it
		// went in as a raw ellipsoid height, which made the same setting mean 2954 m MSL over
		// Ingolstadt and 2992 m over Singapore - geo-referenced, but not quite the same altitude.
		const FVector CloudRef  = TargetThirdPersonCharacter->GetActorLocation();
		FVector       CloudGeoPos(0.0);
		const float   CloudUndulation = GetGeographicPosition(CloudRef, CloudGeoPos)
			? GetGeoidSeparationAt(CloudGeoPos.Y, CloudGeoPos.X)
			: 0.0f;

		if (!GetUnrealZForAltitude(CloudRef, HeightM + CloudUndulation, TargetWorldZ))
		{
			// No Cesium, or the conversion failed - treat the number as height above terrain rather
			// than silently placing the layer somewhere arbitrary.
			const double BaseZ = bHaveGround ? static_cast<double>(GroundZ) : DroneZ;
			TargetWorldZ = BaseZ + static_cast<double>(HeightM) * 100.0;
		}
	}
	else
	{
		// Ground-referenced modes fall back to the drone's own height when the trace finds nothing:
		// the closest available reference, and better than world Z=0, which in a Cesium world is an
		// arbitrary plane that may be kilometres from the terrain.
		const double BaseZ =
			(CloudAltitudeMode == ECloudAltitudeMode::AboveDrone)                          ? DroneZ :
			(CloudAltitudeMode == ECloudAltitudeMode::AboveTerrainAtStart && bHaveCachedTerrainZ)
				? static_cast<double>(CachedTerrainZ)
				: (bHaveGround ? static_cast<double>(GroundZ) : DroneZ);

		TargetWorldZ = BaseZ + static_cast<double>(HeightM) * 100.0;
	}

	// Convert that world Z into the atmosphere-relative kilometres the cloud layer is expressed in.
	const float BottomKm = static_cast<float>((TargetWorldZ - PlanetSurfaceZ) / 100000.0);

	// Go through UDS's own apply path rather than around it.
	//
	// Setting the variable alone did nothing, and setting the renderer component directly was undone
	// within the frame - because UDS computes the component's LayerBottomAltitude itself, inside
	// "Static Properties - Volumetric Clouds Altitude", from this variable. Writing the variable and
	// then invoking that function is what makes the change reach the renderer and survive: it is the
	// same route the Details panel takes. The RepNotify has to be fired by hand for the same reason
	// the manual weather overrides did - a reflection write never triggers one.
	UDWBridge::SetFloat(UDS, UDWNames::CloudBottomAltitude, BottomKm);

	// Thickness is not set here: UDS has no layer-height variable, only "Layer Height Scale", which
	// is part of the cloud look saved into the level (weather_pipeline.py CLOUDS "thickness").

	UDWBridge::CallFunction(UDS, UDWNames::OnRepBottomAltitude);
	UDWBridge::CallFunction(UDS, UDWNames::ApplyCloudAltitude);

	if (!bLoggedCloudAnchor)
	{
		bLoggedCloudAnchor = true;

		// The calibration line: everything needed to reason about cloud height in one place. Without
		// the ground figure, "clouds at 2.6 km" says nothing about whether they are above the terrain.
		if (bHaveGround)
		{
			// Everything in metres, matching the units the settings are typed in and the metadata is
			// written in, so the number to change can be read straight off this line.
			// Sampled at the drone's own X/Y, not at the origin - on a globe the altitude for a
			// given Z changes with position, so converting at (0,0) misreports it the further out
			// the drone is.
			const FVector DroneXY = TargetThirdPersonCharacter->GetActorLocation();
			FVector GroundGeo(0.0), DroneGeo(0.0), CloudGeo(0.0);
			GetGeographicPosition(FVector(DroneXY.X, DroneXY.Y, GroundZ),      GroundGeo);
			GetGeographicPosition(DroneXY,                                     DroneGeo);
			GetGeographicPosition(FVector(DroneXY.X, DroneXY.Y, TargetWorldZ), CloudGeo);

			UE_LOG(LogTemp, Log,
				// Every altitude on this line is height above the WGS84 ellipsoid, which is what Cesium
				// returns and what the cloud settings are expressed in. Stated explicitly because the
				// [Altitude] readout reports mean sea level instead - at Ingolstadt the same ground is
				// 422 m here and 376 m there, and without the datum named that reads as a bug.
				TEXT("Height calibration (ellipsoid datum) | ground %.0fm%s | drone %.0fm, %.0fm above ground | cloud base %.0fm (%.0fm rel. drone) | atmosphere ground Z=%.0fcm, layer %.3f km | mode: %s"),
				GroundGeo.Z,
				bReusedGround ? TEXT(" (reused - tiles re-streaming)") : TEXT(""),
				DroneGeo.Z, HeightAboveGround / 100.0f,
				CloudGeo.Z,
				(TargetWorldZ - DroneZ) / 100.0,
				PlanetSurfaceZ, BottomKm,
				CloudAltitudeMode == ECloudAltitudeMode::RealWorldAltitude   ? TEXT("real-world altitude") :
				CloudAltitudeMode == ECloudAltitudeMode::AboveTerrainAtStart ? TEXT("above terrain, sampled at start") :
				CloudAltitudeMode == ECloudAltitudeMode::AboveGround         ? TEXT("above ground, live") : TEXT("above drone"));
		}
		else
		{
			UE_LOG(LogTemp, Warning,
				TEXT("Height calibration: no ground found within %.0fm below the drone - Cesium tiles may not have streamed in. Falling back to drone-relative cloud height."),
				GroundTraceDistanceM);
		}
	}

	// Confirm against the renderer, which is the only thing that actually decides where clouds are.
	if (UVolumetricCloudComponent* Cloud = UDS->FindComponentByClass<UVolumetricCloudComponent>())
	{
		if (!FMath::IsNearlyEqual(Cloud->LayerBottomAltitude, BottomKm, 0.05f) && !bLoggedCloudAnchor)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("Cloud layer still did not hold: asked for %.3f km, the VolumetricCloud component reads %.3f km."),
				BottomKm, Cloud->LayerBottomAltitude);
		}
	}
}

UExponentialHeightFogComponent* AMyActor_weather::GetSkyHeightFogComponent() const
{
	AActor* UDS = UDSActor.Get();
	return UDS ? UDS->FindComponentByClass<UExponentialHeightFogComponent>() : nullptr;
}

void AMyActor_weather::ApplyHeightFogFarField()
{
	UExponentialHeightFogComponent* FogComp = GetSkyHeightFogComponent();

	// Report the disabled case too. Previously this returned silently when both values were zero,
	// which made "the parameter never reached this actor" look identical to "the parameter was
	// applied and did nothing" - the log was empty either way. Given how much of this pipeline's
	// history is silent no-ops, the state of the knob is worth one line.
	if (HeightFogEndDistanceM <= 0.0f && HeightFogCutoffDistanceM <= 0.0f)
	{
		if (!bLoggedHeightFogFarField)
		{
			bLoggedHeightFogFarField = true;
			UE_LOG(LogTemp, Log,
				TEXT("Height fog far-field cap is OFF (HeightFogEndDistanceM=%.0f HeightFogCutoffDistanceM=%.0f). ")
				TEXT("Live component state: EndDistance=%.0fm FogCutoffDistance=%.0fm MaxOpacity=%.2f ")
				TEXT("Density=%.4f Falloff=%.3f. Set one of the two on the AMyActor_weather instance in ")
				TEXT("the level (Weather|Sky), not on the Blueprint class defaults."),
				HeightFogEndDistanceM, HeightFogCutoffDistanceM,
				FogComp ? FogComp->EndDistance * 0.01f : -1.0f,
				FogComp ? FogComp->FogCutoffDistance * 0.01f : -1.0f,
				FogComp ? FogComp->FogMaxOpacity : -1.0f,
				FogComp ? FogComp->FogDensity : -1.0f,
				FogComp ? FogComp->FogHeightFalloff : -1.0f);
		}
		return;
	}

	if (!FogComp)
	{
		// Logged once rather than per step: without the component there is nothing to retry against.
		if (!bLoggedHeightFogFarField)
		{
			bLoggedHeightFogFarField = true;
			UE_LOG(LogTemp, Warning,
				TEXT("Height fog far-field cap requested (end=%.0fm cutoff=%.0fm) but Ultra_Dynamic_Sky ")
				TEXT("has no ExponentialHeightFogComponent - the flat horizon band will remain."),
				HeightFogEndDistanceM, HeightFogCutoffDistanceM);
		}
		return;
	}

	// Config is in metres, to match the cloud and fog altitude parameters; the component is in cm.
	if (HeightFogEndDistanceM > 0.0f)
	{
		FogComp->SetEndDistance(HeightFogEndDistanceM * 100.0f);
	}
	if (HeightFogCutoffDistanceM > 0.0f)
	{
		FogComp->SetFogCutoffDistance(HeightFogCutoffDistanceM * 100.0f);
	}

	if (!bLoggedHeightFogFarField)
	{
		bLoggedHeightFogFarField = true;

		// Read back rather than echoing what was written. A setter that silently did nothing has
		// cost this pipeline several rounds of misdiagnosis; the log should show the component's
		// actual state, not the intent.
		// Density/falloff/opacity are logged alongside because they decide the distance at which the
		// fog reaches full opacity. A distance cap set beyond that point cannot change the horizon
		// band at all - the fog is already saturated there - so these three numbers are what say
		// whether a given cap value could ever have had an effect.
		UE_LOG(LogTemp, Log,
			TEXT("Height fog far field capped on %s: EndDistance=%.0fm (was %.0fm) ")
			TEXT("FogCutoffDistance=%.0fm (was %.0fm). Fog state: Density=%.4f Falloff=%.3f ")
			TEXT("MaxOpacity=%.2f. Volumetric fog is unaffected - neither property is read by the ")
			TEXT("volumetric fog path."),
			*FogComp->GetOwner()->GetName(),
			FogComp->EndDistance * 0.01f,        DefaultHeightFogEndDistance * 0.01f,
			FogComp->FogCutoffDistance * 0.01f,  DefaultHeightFogCutoff * 0.01f,
			FogComp->FogDensity, FogComp->FogHeightFalloff, FogComp->FogMaxOpacity);
	}
}

void AMyActor_weather::CacheSkyFogDefaults()
{
	AActor* UDS = UDSActor.Get();
	if (bCachedSkyFogDefaults || !UDS)
	{
		return;
	}

	// Stored, not just logged. This was previously read into a local for the log line and thrown
	// away, so RestoreSkyFogDefaults had nothing to put back and the pipeline's density stayed on
	// UDS for the rest of the session - the residual fog left behind after a run.
	UDWBridge::GetFloat(UDS, UDWNames::BaseFogDensity, DefaultBaseFogDensity);
	const float ExistingBaseFogDensity = DefaultBaseFogDensity;

	UDWBridge::GetFloat(UDS, UDWNames::ScaleFogDensity, DefaultScaleFogDensity);
	UDWBridge::GetFloat(UDS, UDWNames::MinFogStartDistance, DefaultMinFogStart);
	UDWBridge::GetFloat(UDS, UDWNames::MaxFogStartDistance, DefaultMaxFogStart);
	UDWBridge::GetFloat(UDS, UDWNames::VolumetricFogDistance, DefaultVolFogDistance);
	UDWBridge::GetFloat(UDS, UDWNames::VolumetricFogExtinction, DefaultVolFogExtinction);

	UDWBridge::GetFloat(UDS, UDWNames::CloudSpeed,                DefaultCloudSpeed);
	UDWBridge::GetFloat(UDS, UDWNames::CloudFormationChangeSpeed, DefaultFormationSpeed);

	// Snapshotted so a condition that lowers the extinction can hand it back afterwards.
	UDWBridge::GetFloat(UDS, UDWNames::CloudExtinctionScale,      DefaultCloudExtinction);

	UDWBridge::GetFloat(UDS, UDWNames::BaseHeightFogFalloff,   DefaultFalloffClear);
	UDWBridge::GetFloat(UDS, UDWNames::CloudyHeightFogFalloff, DefaultFalloffCloudy);
	UDWBridge::GetFloat(UDS, UDWNames::FoggyHeightFogFalloff,  DefaultFalloffFoggy);

	// The far-field caps and the plane's position live on the engine component, not in UDS's
	// property set. The plane is positional state - nothing else in UDS or this pipeline restores a
	// component transform - so it has to be remembered here or the move is permanent.
	if (const UExponentialHeightFogComponent* FogComp = GetSkyHeightFogComponent())
	{
		DefaultHeightFogEndDistance = FogComp->EndDistance;
		DefaultHeightFogCutoff      = FogComp->FogCutoffDistance;
		DefaultFogPlaneZ            = FogComp->GetComponentLocation().Z;
		bHaveDefaultFogPlaneZ       = true;
	}

	bCachedSkyFogDefaults = true;

	UE_LOG(LogTemp, Log,
		TEXT("Cached Ultra_Dynamic_Sky fog defaults: baseDensity=%.4f (overriding with %.4f) ")
		TEXT("scale=%.2f start=%.0f/%.0f volDist=%.0f volExt=%.2f | falloff clear=%.3f->%.3f ")
		TEXT("cloudy=%.3f->%.3f foggy=%.3f->%.3f | cloud speed=%.2f->%.2f formation=%.2f->%.2f ")
		TEXT("(0 = left at UDS's value)"),
		ExistingBaseFogDensity, BaseFogDensity,
		DefaultScaleFogDensity, DefaultMinFogStart, DefaultMaxFogStart,
		DefaultVolFogDistance, DefaultVolFogExtinction,
		DefaultFalloffClear,  HeightFogFalloffClear,
		DefaultFalloffCloudy, HeightFogFalloffCloudy,
		DefaultFalloffFoggy,  HeightFogFalloffFoggy,
		DefaultCloudSpeed,     CloudSpeed,
		DefaultFormationSpeed, CloudFormationChangeSpeed);
}

void AMyActor_weather::RestoreSkyFogDefaults()
{
	AActor* UDS = UDSActor.Get();
	if (!bCachedSkyFogDefaults || !UDS)
	{
		return;
	}

	UDWBridge::SetFloat(UDS, UDWNames::ScaleFogDensity, DefaultScaleFogDensity);
	UDWBridge::SetFloat(UDS, UDWNames::MinFogStartDistance, DefaultMinFogStart);
	UDWBridge::SetFloat(UDS, UDWNames::MaxFogStartDistance, DefaultMaxFogStart);
	UDWBridge::SetFloat(UDS, UDWNames::VolumetricFogDistance, DefaultVolFogDistance);
	UDWBridge::SetFloat(UDS, UDWNames::VolumetricFogExtinction, DefaultVolFogExtinction);

	UDWBridge::SetFloat(UDS, UDWNames::CloudSpeed,                DefaultCloudSpeed);
	UDWBridge::SetFloat(UDS, UDWNames::CloudFormationChangeSpeed, DefaultFormationSpeed);

	UDWBridge::SetFloat(UDS, UDWNames::BaseHeightFogFalloff,   DefaultFalloffClear);
	UDWBridge::SetFloat(UDS, UDWNames::CloudyHeightFogFalloff, DefaultFalloffCloudy);
	UDWBridge::SetFloat(UDS, UDWNames::FoggyHeightFogFalloff,  DefaultFalloffFoggy);

	// Put the far-field caps back too. UDS would not do it - it does not know these exist - so
	// without this the cap would outlive the run and quietly alter the editor viewport afterwards.
	// Safe to restore again now that ClearUDWWeather only calls this when a session is ending.
	// Restoring it unconditionally is what put UDS's 0.0228 back between conditions and left the
	// following condition rendering at eleven times the intended density.
	UDWBridge::SetFloat(UDS, UDWNames::BaseFogDensity, DefaultBaseFogDensity);

	if (UExponentialHeightFogComponent* FogComp = GetSkyHeightFogComponent())
	{
		FogComp->SetEndDistance(DefaultHeightFogEndDistance);
		FogComp->SetFogCutoffDistance(DefaultHeightFogCutoff);

		// Written straight to the component, because a property write to UDS only reaches it on the
		// next refresh - which at teardown may never come, leaving the last condition's density in
		// the level. That was the fog remaining after a run.
		FogComp->SetFogDensity(DefaultBaseFogDensity * DefaultScaleFogDensity);

		// Put the plane back where UDS had it. Anchoring moved it to the geoid surface, which at any
		// inland site is far below the terrain - leaving it there means every later frame is viewed
		// from deep inside the fog layer.
		if (bHaveDefaultFogPlaneZ)
		{
			const FVector Current = FogComp->GetComponentLocation();
			FogComp->SetWorldLocation(FVector(Current.X, Current.Y, DefaultFogPlaneZ));
		}
	}

	// Post Process Wind Fog had no restore path at all, which is why haze survived the run ending and
	// only cleared when a later condition with no wind happened to scale it to nothing. It is UDW's,
	// not UDS's, so it was missed by every restore here.
	SetMovingFog(bEnableMovingFog, MovingFogIntensityScale);

	// So a later run re-anchors and logs rather than assuming the plane is still where it left it.
	bLoggedFogPlaneAnchor = false;

	// The state actually left behind, read back after everything above has run. Every previous
	// attempt at this reasoned about what the restore should have done; this reports what it did.
	{
		float LeftBaseDensity = 0.0f, LeftPPWF = 0.0f, LeftWeatherFog = 0.0f;
		UDWBridge::GetFloat(UDS, UDWNames::BaseFogDensity, LeftBaseDensity);

		if (AActor* UDWForFog = UDWActor.Get())
		{
			UDWBridge::GetFloat(UDWForFog, UDWNames::PPWFIntensityScale, LeftPPWF);
			UDWBridge::GetFloat(UDWForFog, UDWNames::Fog,                LeftWeatherFog);
		}

		float LeftComponent = -1.0f;
		if (const UExponentialHeightFogComponent* FogComp = GetSkyHeightFogComponent())
		{
			LeftComponent = FogComp->FogDensity;
		}

		UE_LOG(LogTemp, Log,
			TEXT("[FogChannels] AFTER RESTORE: weatherFog=%.2f | heightFog base=%.4f -> component=%.4f "
			     "| PPWF=%.2f (component should match base x scale = %.4f)"),
			LeftWeatherFog, LeftBaseDensity, LeftComponent, LeftPPWF,
			DefaultBaseFogDensity * DefaultScaleFogDensity);
	}

	UDWBridge::SetBool(UDS, UDWNames::RefreshSettings, true);
}

namespace
{
	// Blends every driven field of a step. Used to synthesise the intermediate states of a ramp.
	FUDWWeatherStep LerpWeatherStep(const FUDWWeatherStep& A, const FUDWWeatherStep& B, float T)
	{
		FUDWWeatherStep R = B; // carries Label, SettleSeconds and the Extra* maps from the target

		R.Rain                 = FMath::Lerp(A.Rain,                 B.Rain,                 T);
		R.Snow                 = FMath::Lerp(A.Snow,                 B.Snow,                 T);
		R.Fog                  = FMath::Lerp(A.Fog,                  B.Fog,                  T);
		R.CloudCoverage        = FMath::Lerp(A.CloudCoverage,        B.CloudCoverage,        T);
		R.WindIntensity        = FMath::Lerp(A.WindIntensity,        B.WindIntensity,        T);
		R.ThunderLightning     = FMath::Lerp(A.ThunderLightning,     B.ThunderLightning,     T);
		R.MaterialWetness      = FMath::Lerp(A.MaterialWetness,      B.MaterialWetness,      T);
		R.MaterialSnowCoverage = FMath::Lerp(A.MaterialSnowCoverage, B.MaterialSnowCoverage, T);

		// Wind direction is a heading: interpolate the short way round so 350 -> 10 crosses north
		// rather than sweeping backwards through every intermediate angle.
		R.WindDirectionDeg = A.WindDirectionDeg +
			FMath::UnwindDegrees(B.WindDirectionDeg - A.WindDirectionDeg) * T;

		R.RainParticleSpawnCount = FMath::RoundToInt(FMath::Lerp(
			static_cast<float>(A.RainParticleSpawnCount), static_cast<float>(B.RainParticleSpawnCount), T));
		R.SnowParticleSpawnCount = FMath::RoundToInt(FMath::Lerp(
			static_cast<float>(A.SnowParticleSpawnCount), static_cast<float>(B.SnowParticleSpawnCount), T));

		R.RainDropsScale          = FMath::Lerp(A.RainDropsScale,          B.RainDropsScale,          T);
		R.SnowFlakesScale         = FMath::Lerp(A.SnowFlakesScale,         B.SnowFlakesScale,         T);
		R.RainDropsAlpha          = FMath::Lerp(A.RainDropsAlpha,          B.RainDropsAlpha,          T);
		R.SnowFlakesAlpha         = FMath::Lerp(A.SnowFlakesAlpha,         B.SnowFlakesAlpha,         T);
		R.SpawnRadius             = FMath::Lerp(A.SpawnRadius,             B.SpawnRadius,             T);
		R.MinimumParticleDistance = FMath::Lerp(A.MinimumParticleDistance, B.MinimumParticleDistance, T);
		R.RainWindVelocity        = FMath::Lerp(A.RainWindVelocity,        B.RainWindVelocity,        T);
		R.SnowWindVelocity        = FMath::Lerp(A.SnowWindVelocity,        B.SnowWindVelocity,        T);

		R.SnowDepth      = FMath::Lerp(A.SnowDepth,      B.SnowDepth,      T);
		R.PuddleCoverage = FMath::Lerp(A.PuddleCoverage, B.PuddleCoverage, T);

		R.CloudDensity      = FMath::Lerp(A.CloudDensity,      B.CloudDensity,      T);
		R.CloudDirectionDeg = FMath::Lerp(A.CloudDirectionDeg, B.CloudDirectionDeg, T);

		R.FogDensityScale         = FMath::Lerp(A.FogDensityScale,         B.FogDensityScale,         T);
		R.VolumetricFogDistance   = FMath::Lerp(A.VolumetricFogDistance,   B.VolumetricFogDistance,   T);
		R.VolumetricFogExtinction = FMath::Lerp(A.VolumetricFogExtinction, B.VolumetricFogExtinction, T);

		// Negative is the "leave it alone" sentinel for both of these, so only interpolate when both
		// ends are real values - lerping through a sentinel would produce a meaningless middle.
		R.FogStartDistance = (A.FogStartDistance >= 0.0f && B.FogStartDistance >= 0.0f)
			? FMath::Lerp(A.FogStartDistance, B.FogStartDistance, T) : B.FogStartDistance;
		R.TimeOfDayOverride = (A.TimeOfDayOverride >= 0.0f && B.TimeOfDayOverride >= 0.0f)
			? FMath::Lerp(A.TimeOfDayOverride, B.TimeOfDayOverride, T) : B.TimeOfDayOverride;

		return R;
	}
}

// ---------------------------------------------------------------------------
// Matched capture run: every condition flown over every waypoint
// ---------------------------------------------------------------------------

void AMyActor_weather::BuildDefaultCaptureRun()
{
	if (CaptureConditions.Num() == 0)
	{
		// Discrete conditions rather than a blended sweep: for matched pairs, what matters is that
		// each condition is a real, internally consistent weather that recurs identically at every
		// waypoint - not that the states in between are finely resolved.
		// Referenced immediately after each Add, and reserved up front so the reference cannot be
		// invalidated by a reallocation partway through the table.
		CaptureConditions.Reserve(11);

		auto Add = [this](const TCHAR* Asset, const TCHAR* Label) -> FWeatherCondition&
		{
			FWeatherCondition C;
			C.PresetPath = FString::Printf(
				TEXT("/Game/UltraDynamicSky/Blueprints/Weather_Effects/Weather_Presets/%s.%s"), Asset, Asset);
			C.Label = Label;
			return CaptureConditions[CaptureConditions.Add(C)];
		};

		// Presets this project adds, saved as ordinary UDS_Weather_Settings assets beside the stock
		// ones' format. Each is a stock preset with one channel changed IN THE ASSET - so the condition
		// is still a 1:1 copy of a preset, applied through Change Weather like the rest, and the change
		// is visible and editable in the Content Browser rather than hidden in code.
		//   Rain_Fog      = Rain with Fog 10 (the Foggy preset's fog)
		//   Snow_Fog      = Snow with Fog 10, Material Snow Coverage 0.6
		//   WP_Snow       = Snow with Material Snow Coverage 0.6
		//   WP_Snow_Light = Snow_Light with Material Snow Coverage 0.3, Cloud Coverage 3.8
		auto AddProjectPreset = [this](const TCHAR* Asset, const TCHAR* Label) -> FWeatherCondition&
		{
			FWeatherCondition C;
			C.PresetPath = FString::Printf(TEXT("/Game/WeatherPipeline/Presets/%s.%s"), Asset, Asset);
			C.Label = Label;
			return CaptureConditions[CaptureConditions.Add(C)];
		};

		// Stock UDS presets only, in one progression from clear to extreme.
		//
		// Every condition here IS a shipped preset, handed to UDW's own Change Weather untouched, so
		// what renders is what that preset renders when picked by hand in the editor.
		//
		// The in-between conditions this pipeline used to synthesise - rain_fog, snow_medium, snow_fog,
		// fog_light, fog_medium, moving_fog - are commented out at the bottom rather than deleted.
		// Every one of them existed by overriding a channel of a preset, which is the practice that
		// produced this project's run of visual defects; see bUsePresetsVerbatim in the header.
		//
		// Sand_Dust_Calm and Sand_Dust_Storm are the two shipped presets deliberately left out.
		//
		// Authored channel values, read from the assets into Saved/wp_presets.txt:
		//   Clear_Skies   cloud  0.0  fog  0.0  wind  2.0
		//   Partly_Cloudy cloud  3.8  fog  1.0  wind  2.0
		//   Cloudy        cloud  5.0  fog  1.0  wind  2.5
		//   Overcast      cloud  7.5  fog  1.5  wind  3.0
		//   Rain_Light    cloud  6.0  fog  1.5  rain  3.0  wet 0.75
		//   Rain          cloud  7.5  fog  3.0  rain  7.0  wet 1.00
		//   Thunderstorm  cloud  8.0  fog  6.5  rain 10.0  wet 1.00  wind 10.0
		//   Snow_Light    cloud  6.0  fog  3.0  snow  3.0  snowCov 0.5
		//   Snow          cloud  8.5  fog  5.0  snow  6.0  snowCov 1.0
		//   Snow_Blizzard cloud 10.0  fog 10.0  snow 10.0  snowCov 1.0  wind 10.0
		//   Foggy         cloud  0.0  fog 10.0  wind  1.0  wet 0.55

		// --- Cloud ramp. Clear is the reference frame every other condition is judged against. ---
		Add(TEXT("Clear_Skies"),       TEXT("clear"));
		Add(TEXT("Partly_Cloudy"),     TEXT("partly_cloudy"));
		Add(TEXT("Cloudy"),            TEXT("cloudy"));
		Add(TEXT("Overcast"),          TEXT("overcast"));

		// --- Rain ---
		Add(TEXT("Rain_Light"),        TEXT("rain_light"));
		Add(TEXT("Rain"),              TEXT("rain_medium"));
		AddProjectPreset(TEXT("Rain_Fog"), TEXT("rain_fog"));
		// Heavy rain: the thunderstorm preset with the wind at 0, so the downpour falls straight.
		AddProjectPreset(TEXT("WP_Rain_Heavy"), TEXT("rain_heavy"));

		// --- Snow. Smooth normals are not a weather channel - they are a Cesium tileset setting the
		//     snow material needs in order to land on roofs and streets at all - so they stay even
		//     though every channel override is gone. ---
		// Light snow and snow are project copies: snow cover follows snowfall (0.3 / 0.6, only the
		// blizzard buries everything) and light snow falls from a partly cloudy sky (cloud 3.8).
		AddProjectPreset(TEXT("WP_Snow_Light"), TEXT("snow_light")).bRequiresSmoothNormals = true;
		AddProjectPreset(TEXT("WP_Snow"),       TEXT("snow_medium")).bRequiresSmoothNormals = true;
		AddProjectPreset(TEXT("Snow_Fog"), TEXT("snow_fog")).bRequiresSmoothNormals = true;
		Add(TEXT("Snow_Blizzard"),     TEXT("snow_heavy")).bRequiresSmoothNormals  = true;

		// --- Fog ---
		// Wind from the four compass points at wind 7: medium rain / medium snow copies with Wind
		// Intensity 7. The direction is set on the UDW actor per condition (FromCompass).
		AddProjectPreset(TEXT("WP_Rain_Wind"), TEXT("rain_wind_n")).FromCompass(0.0f);
		AddProjectPreset(TEXT("WP_Rain_Wind"), TEXT("rain_wind_e")).FromCompass(90.0f);
		AddProjectPreset(TEXT("WP_Rain_Wind"), TEXT("rain_wind_s")).FromCompass(180.0f);
		AddProjectPreset(TEXT("WP_Rain_Wind"), TEXT("rain_wind_w")).FromCompass(270.0f);
		AddProjectPreset(TEXT("WP_Snow_Wind"), TEXT("snow_wind_n")).FromCompass(0.0f).bRequiresSmoothNormals   = true;
		AddProjectPreset(TEXT("WP_Snow_Wind"), TEXT("snow_wind_e")).FromCompass(90.0f).bRequiresSmoothNormals  = true;
		AddProjectPreset(TEXT("WP_Snow_Wind"), TEXT("snow_wind_s")).FromCompass(180.0f).bRequiresSmoothNormals = true;
		AddProjectPreset(TEXT("WP_Snow_Wind"), TEXT("snow_wind_w")).FromCompass(270.0f).bRequiresSmoothNormals = true;

		// Light and medium fog: Foggy with a lower Fog channel (UDS derives the density from it).
		AddProjectPreset(TEXT("WP_Fog_Light"),  TEXT("fog_light"));
		AddProjectPreset(TEXT("WP_Fog_Medium"), TEXT("fog_medium"));
		Add(TEXT("Foggy"),             TEXT("fog_heavy"));

		// Fog under clouds: Partly_Cloudy with WP_Fog_Medium's fog.
		AddProjectPreset(TEXT("WP_Partly_Cloudy_Fog"), TEXT("partly_cloudy_fog"));

		// The day after snow: Cloudy with WP_Snow's snow cover on the ground and roofs, no snowfall.
		AddProjectPreset(TEXT("WP_Day_After_Snow"), TEXT("day_after_snow")).bRequiresSmoothNormals = true;

		// --- Commented out: the conditions this pipeline synthesised from preset channels. ---
		//
		// Each needs an override to exist at all, so none can be restored without also turning
		// bUsePresetsVerbatim off - and that re-enables the channel pinning which replaces UDW's
		// weather generation with constants and flattens the cloud deck.
		//
		// Add(TEXT("Snow"),  TEXT("snow_medium")).SnowOverride  = 4.5f;   // between Snow_Light and Snow
		// Add(TEXT("Foggy"), TEXT("fog_light")).FogOverride     = 5.5f;   // Foggy, thinned
		// Add(TEXT("Foggy"), TEXT("fog_medium")).FogOverride    = 8.0f;   // Foggy, thinned less
		//
		// moving_fog is the one exception worth keeping in mind: it is not a thinned preset but Post
		// Process Wind Fog, a separate system no preset covers, plus a ground-level cloud layer.
		//
		// {
		//     FWeatherCondition& C   = Add(TEXT("Foggy"), TEXT("moving_fog"));
		//     C.FogOverride          = 2.5f;
		//     C.WindIntensityOverride = 6.0f;
		//     C.MovingFogIntensity   = 2.5f;
		//     C.bOverrideCloudHeight = true;
		//     C.CloudHeightOffsetM   = 50.0f;
		//     C.CloudExtinctionOverride = 0.35f;
		// }
	}

	// Applied after the defaults are built, so the filter narrows the full table rather than being
	// something the table has to know about.
	if (!ConditionFilter.IsEmpty())
	{
		const int32 Before = CaptureConditions.Num();
		CaptureConditions.RemoveAll([this](const FWeatherCondition& C)
		{
			return !C.Label.Contains(ConditionFilter, ESearchCase::IgnoreCase);
		});

		UE_LOG(LogTemp, Log, TEXT("Condition filter '%s': running %d of %d conditions."),
			*ConditionFilter, CaptureConditions.Num(), Before);
	}

	// Exact-label list. Separate from ConditionFilter above because a run usually wants a named set -
	// "rain, snow, foggy" - which no single substring expresses.
	//
	// The launcher had been writing this property name for some time before it existed here, so the
	// filter silently did nothing and every run captured the whole table. A name that does not resolve
	// is not an error in Unreal's config parser; it is simply ignored.
	if (CaptureConditionFilter.Num() > 0)
	{
		const int32 Before = CaptureConditions.Num();

		CaptureConditions.RemoveAll([this](const FWeatherCondition& C)
		{
			return !CaptureConditionFilter.ContainsByPredicate(
				[&C](const FString& Wanted) { return C.Label.Equals(Wanted, ESearchCase::IgnoreCase); });
		});

		UE_LOG(LogTemp, Log, TEXT("Condition list [%s]: running %d of %d conditions."),
			*FString::Join(CaptureConditionFilter, TEXT(", ")), CaptureConditions.Num(), Before);

		// A filter that matches nothing would otherwise produce a run with no images and no
		// explanation, which looks like a capture failure rather than a configuration one.
		if (CaptureConditions.Num() == 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("Condition list [%s] matched none of the %d conditions in the table - check the "
				     "labels against the ones the table ships."),
				*FString::Join(CaptureConditionFilter, TEXT(", ")), Before);
		}
	}

	// Preview the whole capture table rather than the three-preset summary. Built from
	// CaptureConditions *after* ConditionFilter has been applied above, so the preview shows exactly
	// the set that a capture run would record - a litmus that previewed conditions the run then
	// skipped would be worse than none.
	if (bLitmusPreviewsAllConditions && LitmusConditions.Num() == 0)
	{
		LitmusConditions = CaptureConditions;

		UE_LOG(LogTemp, Log,
			TEXT("[Litmus] Previewing all %d capture conditions at %.2fs each (%.1fs gap) - about %.0fs total."),
			LitmusConditions.Num(), LitmusStepSeconds, LitmusGapSeconds,
			LitmusConditions.Num() * (LitmusStepSeconds + LitmusGapSeconds));
	}

	// Fallback set, used only when the "preview every condition" flag is off. Stock presets with no
	// overrides, for the same reason the capture table is: a litmus that previews an overridden
	// condition is previewing something the run will not record.
	if (LitmusConditions.Num() == 0)
	{
		auto AddLitmus = [this](const TCHAR* Asset, const TCHAR* Label)
		{
			FWeatherCondition C;
			C.PresetPath = FString::Printf(
				TEXT("/Game/UltraDynamicSky/Blueprints/Weather_Effects/Weather_Presets/%s.%s"), Asset, Asset);
			C.Label = Label;
			LitmusConditions.Add(C);
		};

		// One from each family, enough to tell at a glance that weather is reaching the sky at all.
		// Snow uses the mid-strength preset rather than Snow_Blizzard: at blizzard wind the flakes
		// streak horizontally and read as heavy rain, which makes it a poor smoke test.
		AddLitmus(TEXT("Rain_Thunderstorm"), TEXT("rain"));
		AddLitmus(TEXT("Snow"),              TEXT("snow"));
		AddLitmus(TEXT("Foggy"),             TEXT("fog"));
	}

	// Strip every channel override so each condition is its preset and nothing else.
	//
	// The overrides above were each added for a defensible reason - fog too heavy from altitude, snow
	// too dark, a preset's cloud coverage nearly overcast on a "light" step - but every one of them
	// changes a single channel of a set the author tuned together. The result is a condition that is
	// neither the preset nor a coherent alternative: cloud shape, coverage and density stop agreeing
	// with the fog and particles they were authored alongside, which is exactly why PIE overcast and
	// preset overcast look like different weather.
	//
	// With this on, the preset path and the label are all a condition carries. Whatever UDS authored -
	// cloud shape, distribution, layer altitude, thickness, fog profile, particles - arrives intact.
	if (bUsePresetsVerbatim)
	{
		for (FWeatherCondition& C : CaptureConditions)
		{
			C.WindIntensityOverride         = -1.0f;
			C.FogOverride                   = -1.0f;
			C.CloudCoverageOverride         = -1.0f;
			C.RainOverride                  = -1.0f;
			C.SnowOverride                  = -1.0f;
			C.MaterialSnowCoverageOverride  = -1.0f;
			C.MaterialWetnessOverride       = -1.0f;
			C.FogDensityScaleOverride       = -1.0f;
			C.ParticleSpawnCount            = 0;
			C.ParticleScale                 = 0.0f;
			C.ParticleAlpha                 = 0.0f;

			// Moving fog and the ground-level cloud offset are kept: Post Process Wind Fog is a
			// separate system rather than part of any preset, and without the offset moving_fog has no
			// ground layer to move. Everything else is the preset's.
		}

		UE_LOG(LogTemp, Log,
			TEXT("[Conditions] Presets applied verbatim: %d conditions carry no channel overrides, so "
			     "cloud shape, coverage, fog and particles are exactly as UDS authored them."),
			CaptureConditions.Num());
	}

	if (FlightPath.Num() == 0 && bHaveCharacterStart)
	{
		// The random jump and capture: SpotCount spots, each drawn inside its limits, in the square
		// around the start. Every condition visits the same spots, so the images pair across weathers.
		const int32 Spots = FMath::Max(1, SpotCount);
		SpotsCentre = CharacterStartTransform.GetLocation();
		SpotStream.Initialize(SpotSeed >= 0 ? SpotSeed : FMath::Rand());
		SpotRedraws.Init(0, Spots);
		for (int32 Spot = 0; Spot < Spots; ++Spot)
		{
			FlightPath.Append(BuildShot(Spot, (Spot == 0 && bHaveManualStartSpot) ? ManualStartSpot : DrawSpot()));
		}
		UE_LOG(LogTemp, Log,
			TEXT("[Spots] %d camera spot(s), shot '%s', in a %.2f km2 square, seed %d: %d capture points."),
			Spots, *CaptureShot, SpotAreaKm2, SpotSeed, FlightPath.Num());
	}
}

float AMyActor_weather::HhmmToMinutes(float Hhmm)
{
	const int32 Time = FMath::RoundToInt(Hhmm);
	return (Time / 100) * 60.0f + (Time % 100);
}

float AMyActor_weather::MinutesToHhmm(float Minutes)
{
	const int32 Whole = FMath::RoundToInt(Minutes);
	return (Whole / 60) * 100.0f + (Whole % 60);
}

AMyActor_weather::FSpotDraw AMyActor_weather::DrawSpot()
{
	// One value per line: the draw order is then fixed, so a seed always gives the same spots.
	auto Between = [this](float Low, float High) { return FMath::Lerp(Low, FMath::Max(Low, High), SpotStream.FRand()); };
	const float HalfSideCm = FMath::Sqrt(FMath::Max(SpotAreaKm2, 0.0f)) * 100000.0f * 0.5f;

	FSpotDraw Draw;
	Draw.OffsetCm.X  = Between(-HalfSideCm, HalfSideCm);
	Draw.OffsetCm.Y  = Between(-HalfSideCm, HalfSideCm);
	Draw.HeightCm    = Between(SpotHeightMinM, SpotHeightMaxM) * 100.0f;
	Draw.View.Pitch  = Between(SpotPitchMinDeg, SpotPitchMaxDeg);
	Draw.View.Yaw    = SpotYawStartDeg + Between(0.0f, SpotYawSpanDeg);
	Draw.View.Roll   = Between(SpotRollMinDeg, SpotRollMaxDeg);
	Draw.TimeOfDay   = MinutesToHhmm(Between(HhmmToMinutes(SpotTimeMin), HhmmToMinutes(SpotTimeMax)));
	return Draw;
}

TArray<FCaptureWaypoint> AMyActor_weather::BuildShot(int32 Spot, const FSpotDraw& Draw) const
{
	const UCaptureShot* Shot = UCaptureShot::Find(CaptureShot);
	if (!Shot)
	{
		UE_LOG(LogTemp, Error, TEXT("[Spots] No capture shot '%s' (a class U<Name>CaptureShot, see CaptureShots.h): "
			"taking a frame."), *CaptureShot);
		Shot = GetDefault<UFrameCaptureShot>();
	}

	// Z is the centre's for now: the ground is measured on the first visit, when its tiles are loaded.
	FCaptureSpot Where;
	Where.Location = SpotsCentre + FVector(Draw.OffsetCm.X, Draw.OffsetCm.Y, 0.0);
	Where.View     = Draw.View;

	TArray<FCaptureWaypoint> Points;
	for (const FCapturePose& Pose : Shot->BuildPoses(Where, ShotParameters))
	{
		FCaptureWaypoint W;
		W.Location            = Pose.Location;
		W.Rotation            = Pose.Rotation;
		W.HeightAboveGroundCm = Draw.HeightCm;   // every picture of a spot: its height and time
		W.Spot                = Spot;
		W.TimeOfDay           = Draw.TimeOfDay;
		Points.Add(W);
	}
	return Points;
}

void AMyActor_weather::ReplaceSpot(int32 Spot, const FSpotDraw& Draw)
{
	const TArray<FCaptureWaypoint> Shot = BuildShot(Spot, Draw);
	int32 Next = 0;
	for (FCaptureWaypoint& W : FlightPath)
	{
		if (W.Spot == Spot && Shot.IsValidIndex(Next))
		{
			W = Shot[Next++];
		}
	}
}

AMyActor_weather::ESpotSettle AMyActor_weather::SettleSpotInOpenAir(int32 Spot)
{
	const bool bUsersView = bHaveManualStartSpot && Spot == 0;   // chosen by the user: taken as it is
	bool       bNoGround  = false;
	bool       bMoved     = false;
	EPoseBlock Block      = EPoseBlock::None;

	for (FCaptureWaypoint& W : FlightPath)
	{
		if (W.Spot != Spot)
		{
			continue;
		}
		double GroundZ = 0.0;
		W.bGroundMeasured = ProbeGroundZ(W.Location.X, W.Location.Y, W.Location.Z, GroundZ);
		if (!W.bGroundMeasured)
		{
			bNoGround = true;
			continue;
		}
		W.GroundZ = GroundZ;
		const double Z = GroundZ + W.HeightAboveGroundCm;
		bMoved |= FMath::Abs(Z - W.Location.Z) > 1.0;               // > 1 cm
		W.Location.Z = Z;
		if (!bUsersView && Block == EPoseBlock::None)
		{
			Block = CheckPose(W.Location, W.Rotation);
		}
	}

	if (!bUsersView && (bNoGround || Block != EPoseBlock::None))
	{
		if (SpotRedraws.IsValidIndex(Spot) && SpotRedraws[Spot] < MaxSpotRedraws)
		{
			++SpotRedraws[Spot];
			UE_LOG(LogTemp, Log, TEXT("[Spots] Spot %d: %s - drawn again (%d of %d)."), Spot,
				bNoGround ? TEXT("no ground under it") : PoseBlockText(Block), SpotRedraws[Spot], MaxSpotRedraws);
			ReplaceSpot(Spot, DrawSpot());
			return ESpotSettle::Redrawn;
		}
		UE_LOG(LogTemp, Warning, TEXT("[Spots] Spot %d is still blocked after %d draws: raised clear instead, "
			"so it is above its height range."), Spot, MaxSpotRedraws);
		for (FCaptureWaypoint& W : FlightPath)
		{
			EPoseBlock First = EPoseBlock::None;
			double RaisedM = 0.0;
			if (W.Spot == Spot && W.bGroundMeasured)
			{
				RaiseUntilClear(W.Location, W.Rotation, First, RaisedM);
				bMoved |= RaisedM > 0.0;
			}
		}
	}

	for (FCaptureWaypoint& W : FlightPath)
	{
		if (W.Spot != Spot)
		{
			continue;
		}
		W.bGroundResolved = true;
		if (W.bGroundMeasured)
		{
			UE_LOG(LogTemp, Log,
				TEXT("[Terrain] Waypoint at (%.0f, %.0f): ground Z=%.0fcm, flying %.1f m above it (%.1f m asked)."),
				W.Location.X, W.Location.Y, W.GroundZ, (W.Location.Z - W.GroundZ) / 100.0, W.HeightAboveGroundCm / 100.0f);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[Terrain] No ground found under waypoint at (%.0f, %.0f); its "
				"ground fields are recorded as null."), W.Location.X, W.Location.Y);
		}
	}
	return bMoved ? ESpotSettle::Moved : ESpotSettle::Ready;
}

void AMyActor_weather::StartGlideToCurrentWaypoint()
{
	ACharacter* Char = TargetThirdPersonCharacter;
	if (!Char || !FlightPath.IsValidIndex(CurrentWaypointIndex))
	{
		return;
	}

	FCaptureWaypoint& Target = FlightPath[CurrentWaypointIndex];

	// Terrain following needs the destination's ground BEFORE the flight, so the drone flies to the
	// right height rather than jumping at the end. The destination is one capture spacing ahead and
	// in view, so its tiles are loaded. Resolved once; later conditions fly to the same pose.
	if (bFollowTerrain)
	{
		ResolveWaypointGround(Target);
	}

	// From the pose the last picture was taken at, so every leg starts exactly where the previous
	// one ended, whatever the pawn did in between.
	GlideFrom = bHaveActiveCapturePose ? ActiveCapturePose : Char->GetActorTransform();
	GlideElapsedSeconds  = 0.0;
	// Straight when that is clear; up, across and down when a building is in the way.
	GlidePoints = PlanLeg(GlideFrom.GetLocation(), Target.Location);
	const bool bDetour = GlidePoints.Num() > 0;
	GlidePoints.Add(Target.Location);
	double DistanceCm = 0.0;
	FVector Previous = GlideFrom.GetLocation();
	for (const FVector& Point : GlidePoints)
	{
		DistanceCm += FVector::Dist(Previous, Point);
		Previous = Point;
	}
	GlideDurationSeconds = GlideSeconds(DistanceCm, FlightSpeedMps);
	bGliding = true;

	// One line per leg: the pipeline counts them to confirm a smooth run really flew every leg.
	UE_LOG(LogTemp, Log, TEXT("[Flight] Flying to waypoint %d: %.0f m at %.1f m/s, %.1f s%s."),
		CurrentWaypointIndex, DistanceCm / 100.0, FlightSpeedMps, GlideDurationSeconds,
		bDetour ? *FString::Printf(TEXT(", over a building (%.0f m up)"),
			(GlidePoints[0].Z - GlideFrom.GetLocation().Z) / 100.0) : TEXT(""));
}

void AMyActor_weather::TickGlide(float DeltaTime)
{
	ACharacter* Char = TargetThirdPersonCharacter;
	if (!bGliding || !Char || !FlightPath.IsValidIndex(CurrentWaypointIndex))
	{
		bGliding = false;
		return;
	}

	const FCaptureWaypoint& Target = FlightPath[CurrentWaypointIndex];
	GlideElapsedSeconds += DeltaTime;
	const double Alpha = FMath::Clamp(GlideElapsedSeconds / GlideDurationSeconds, 0.0, 1.0);

	if (Alpha >= 1.0)
	{
		// Arrive exactly on the waypoint pose - the picture is taken from it, identically in every
		// condition - then capture straight away (CaptureCurrentWaypoint still holds if the tiles
		// are not ready).
		bGliding = false;
		PlaceCameraAtWaypoint(Target);
		CaptureWaitElapsed = 0.0f;
		CaptureCurrentWaypoint();
		return;
	}

	const FVector Location = PointAlongPath(GlideFrom.GetLocation(), GlidePoints, Alpha);
	const FQuat   Rotation = FQuat::Slerp(GlideFrom.GetRotation(), Target.Rotation.Quaternion(), Alpha);

	Char->SetActorLocationAndRotation(Location, Rotation, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
	if (AController* Controller = Char->GetController())
	{
		Controller->SetControlRotation(Rotation.Rotator());
	}
	SetCapturePose(FTransform(Rotation, Location));

	// Weather particles are centred on the drone, so they travel with it.
	UpdateWeatherControlPoint();
}

void AMyActor_weather::PlaceCameraAtWaypoint(const FCaptureWaypoint& Waypoint)
{
	ACharacter* Char = TargetThirdPersonCharacter;
	if (!Char)
	{
		return;
	}

	// Teleport, not fly. Matched pairs need the pose to reproduce exactly across passes, and a
	// simulated approach would land somewhere slightly different every time.
	Char->SetActorLocation(Waypoint.Location, false, nullptr, ETeleportType::TeleportPhysics);
	Char->SetActorRotation(Waypoint.Rotation);

	// The view follows the controller, not the pawn, so the control rotation has to move too.
	if (AController* Controller = Char->GetController())
	{
		Controller->SetControlRotation(Waypoint.Rotation);
	}

	// Record the pose the capture must be taken from. The SceneCapture reads this immediately before
	// the shutter, so the framing is the waypoint's regardless of what the pawn does afterwards.
	SetCapturePose(FTransform(Waypoint.Rotation, Waypoint.Location));

	// The spot's own time of day: set on arrival, so the sun and the exposure settle with the view.
	if (Waypoint.TimeOfDay >= 0.0f)
	{
		SpotTimeOfDay          = Waypoint.TimeOfDay;
		ActiveCaptureTimeOfDay = Waypoint.TimeOfDay;
		if (AActor* UDS = UDSActor.Get())
		{
			UDWBridge::SetFloat(UDS, UDWNames::TimeOfDay, Waypoint.TimeOfDay);
		}
	}

	// Weather particles are centred on the drone, so bring them along in the same frame.
	UpdateWeatherControlPoint();
}

void AMyActor_weather::SetCapturePose(const FTransform& Pose)
{
	ActiveCapturePose      = Pose;
	bHaveActiveCapturePose = true;
	if (!Capture || !bUseFixedCameraCapture)
	{
		return;
	}
	// The capture renders every frame (auto exposure), from wherever it is. Attached to the pawn and
	// moved to the pose only at the shutter, the shutter frame followed a camera jump - the drone's
	// height and the pose differed - and motion blur and the anti-aliasing history smeared the picture
	// along it. Pinned here in world space, every frame of the settle is from the picture's own pose.
	Capture->SetUsingAbsoluteLocation(true);
	Capture->SetUsingAbsoluteRotation(true);
	Capture->SetWorldLocationAndRotation(Pose.GetLocation(), Pose.GetRotation());
}

void AMyActor_weather::ReleaseCapturePose()
{
	bHaveActiveCapturePose = false;
	if (!Capture || !bHaveCaptureAttachTransform)
	{
		return;
	}
	Capture->SetUsingAbsoluteLocation(false);
	Capture->SetUsingAbsoluteRotation(false);
	Capture->SetRelativeTransform(CaptureAttachTransform);
}

bool AMyActor_weather::ApplyConditionPreset(const FWeatherCondition& Condition, float InTransitionSeconds)
{
	UObject* Preset = LoadPresetDefaults(Condition.PresetPath);

	// Read the preset's state up front, whichever path applies it. This becomes ActiveCaptureStep,
	// so the metadata records the real values and the read-back verification has something true to
	// compare against - handing Change Weather the preset and leaving the step at zeros would make
	// every capture report "weather did NOT take effect" and write a row of zeros to the JSON.
	FUDWWeatherStep Step;
	if (Preset)
	{
		UDWBridge::GetFloat(Preset, UDWNames::Rain, Step.Rain);
		UDWBridge::GetFloat(Preset, UDWNames::Snow, Step.Snow);
		UDWBridge::GetFloat(Preset, UDWNames::Fog, Step.Fog);
		UDWBridge::GetFloat(Preset, UDWNames::CloudCoverage, Step.CloudCoverage);
		UDWBridge::GetFloat(Preset, UDWNames::WindIntensity, Step.WindIntensity);
		UDWBridge::GetFloat(Preset, UDWNames::ThunderLightning, Step.ThunderLightning);
		UDWBridge::GetFloat(Preset, UDWNames::MaterialWetness, Step.MaterialWetness);
		UDWBridge::GetFloat(Preset, UDWNames::MaterialSnowCoverage, Step.MaterialSnowCoverage);
	}
	// Overridden channels replace the preset's values in the metadata too, or the JSON would describe
	// a scene that was never rendered.
	if (Condition.WindIntensityOverride  >= 0.0f) { Step.WindIntensity  = Condition.WindIntensityOverride;  }
	if (Condition.FogOverride            >= 0.0f) { Step.Fog            = Condition.FogOverride;            }
	if (Condition.CloudCoverageOverride  >= 0.0f) { Step.CloudCoverage  = Condition.CloudCoverageOverride;  }
	if (Condition.RainOverride           >= 0.0f) { Step.Rain           = Condition.RainOverride;           }
	if (Condition.SnowOverride           >= 0.0f) { Step.Snow           = Condition.SnowOverride;           }
	if (Condition.MaterialSnowCoverageOverride >= 0.0f) { Step.MaterialSnowCoverage = Condition.MaterialSnowCoverageOverride; }
	if (Condition.MaterialWetnessOverride      >= 0.0f) { Step.MaterialWetness      = Condition.MaterialWetnessOverride;      }
	if (Condition.FogDensityScaleOverride      >  0.0f) { Step.FogDensityScale      = Condition.FogDensityScaleOverride;      }
	Step.Label = Condition.Label;

	// Pin the overridden channels BEFORE handing the preset to Change Weather. A manual override is
	// applied at the very end of every local-weather generation, so it survives the transition
	// without the update chain having to be forced again - which would collapse it into a snap.
	if (bUsePresetsVerbatim && ResolveUDWActor())
	{
		// Hand the preset to UDW and let it run its own weather generation.
		//
		// The pinning path below clamps every channel to a constant. UDW does not hold weather as
		// eight fixed numbers - it generates variation from them, which is where the drifting, uneven
		// cloud distribution comes from. Clamping the channels replaces that generation with a flat
		// value, and a flat Cloud Coverage is exactly the evenly-spread slab that does not look like
		// the preset does when picked by hand.
		//
		// So the overrides are RELEASED here rather than merely not set: a leftover override from an
		// earlier condition would keep clamping this one, and the booleans persist on the UDW actor.
		AActor* UDW = UDWActor.Get();

		for (const FName OverrideName : {
				UDWNames::RainOverride,           UDWNames::SnowOverride,
				UDWNames::FogOverride,            UDWNames::CloudCoverageOverride,
				UDWNames::WindIntensityOverride,  UDWNames::ThunderLightningOverride,
				UDWNames::MaterialWetnessOverride, UDWNames::MaterialSnowCoverageOverride })
		{
			UDWBridge::SetBool(UDW, OverrideName, false);
		}

		// A release is a change to the override set too, and needs the same RepNotify pass - otherwise
		// the previous snow condition's coverage pin would stay bound into the next condition.
		if (bSnowCoveragePinned)
		{
			ForceUDWWeatherUpdate();
			bSnowCoveragePinned = false;
		}

		UE_LOG(LogTemp, Log,
			TEXT("[Conditions] '%s': manual overrides released - UDW generates the weather from the "
			     "preset itself, so cloud distribution is its own rather than a clamped constant."),
			*Condition.Label);
	}
	else if (ResolveUDWActor())
	{
		AActor*  UDW   = UDWActor.Get();
		UObject* State = UDWManualState.Get();

		auto Pin = [UDW, State](FName StateName, FName OverrideName, float Value)
		{
			UDWBridge::SetFloat(State, StateName, Value);
			UDWBridge::SetBool(UDW, OverrideName, true);
		};

		// Pin EVERY driven channel, not just the ones this condition overrides.
		//
		// Change Weather's transition is not reliable enough to be the source of truth: a run has
		// been seen holding snow=0.00 while the condition asked for 10.00, at the same moment the
		// pinned fog and cloud matched exactly. Manual overrides are applied at the end of every
		// local-weather generation, so they land deterministically. Change Weather is still called
		// below - it drives the particle systems and material effects - but the state comes from
		// here, which is also what lets the settle gate pass immediately instead of timing out.
		Pin(UDWNames::Rain,                 UDWNames::RainOverride,                 Step.Rain);
		Pin(UDWNames::Snow,                 UDWNames::SnowOverride,                 Step.Snow);
		Pin(UDWNames::Fog,                  UDWNames::FogOverride,                  Step.Fog);
		Pin(UDWNames::CloudCoverage,        UDWNames::CloudCoverageOverride,        Step.CloudCoverage);
		Pin(UDWNames::WindIntensity,        UDWNames::WindIntensityOverride,        Step.WindIntensity);
		Pin(UDWNames::ThunderLightning,     UDWNames::ThunderLightningOverride,     Step.ThunderLightning);
		Pin(UDWNames::MaterialWetness,      UDWNames::MaterialWetnessOverride,      Step.MaterialWetness);
		Pin(UDWNames::MaterialSnowCoverage, UDWNames::MaterialSnowCoverageOverride, Step.MaterialSnowCoverage);

		// Wind direction has no override toggle of its own; the state value is enough.
		UDWBridge::SetFloat(State, UDWNames::WindDirection, Step.WindDirectionDeg);

		ForceUDWWeatherUpdate();
	}

	bool bApplied = false;
	if (Preset && bUseChangeWeatherFunction && ResolveUDWActor())
	{
		bApplied = UDWBridge::CallChangeWeather(UDWActor.Get(), Preset, InTransitionSeconds);
	}

	// Snow on roofs and ground, scaled to how hard it is actually snowing.
	//
	// The presets saturate: Snow ships snowCov 1.00 at snowfall 6.0, and Snow_Blizzard also ships 1.00
	// at snowfall 10.0 - so the heaviest two conditions accumulate identically and the ground stops
	// telling you which one you are looking at. Deriving coverage from the preset's own snowfall keeps
	// the relationship monotonic across the whole set.
	//
	// This is a deliberate deviation from the preset, and the only one left in verbatim mode besides
	// moving fog. It is written AFTER Change Weather because coverage is a value UDW derives from the
	// weather state during the transition - writing it before means writing it into a value that is
	// about to be recomputed, which is the ordering mistake that has bitten the particle counts and
	// the lightning suppression before.
	if (bSnowCoverageFollowsSnowfall && Step.Snow > 0.0f && ResolveUDWActor())
	{
		// Snowfall is on UDS's 0-10 state scale; Material Snow Coverage is one of its rare 0-1 values.
		const float Coverage = FMath::Clamp(Step.Snow / WeatherConstants::UDSStateScale, 0.0f, 1.0f);

		UDWBridge::SetFloat(UDWManualState.Get(), UDWNames::MaterialSnowCoverage, Coverage);
		UDWBridge::SetBool(UDWActor.Get(), UDWNames::MaterialSnowCoverageOverride, true);

		// Setting the override bool through reflection does not fire its RepNotify, and without that
		// UDW never adds the override to its bindings: the live state stayed at the preset's 1.0 and
		// every snow condition rendered fully white. The metadata validator caught it - coverage 1.0
		// where 0.6 was pinned. ForceUDWWeatherUpdate fires the handlers and rebuilds the bindings.
		ForceUDWWeatherUpdate();
		bSnowCoveragePinned = true;

		// Recorded so the metadata describes the scene that was rendered rather than the preset's
		// authored value, which is no longer what reaches the ground.
		Step.MaterialSnowCoverage = Coverage;

		UE_LOG(LogTemp, Log,
			TEXT("[Snow] '%s': snowfall %.1f -> ground coverage %.2f (preset authored %.2f)."),
			*Condition.Label, Step.Snow, Coverage, Condition.MaterialSnowCoverageOverride);
	}

	// Wind direction: set for conditions that name one, the level's own restored for every other,
	// so one condition's direction cannot leak into the next.
	if (AActor* WindUDW = UDWActor.Get())
	{
		if (!bHaveLevelWindDirection)
		{
			bHaveLevelWindDirection = UDWBridge::GetFloat(WindUDW, UDWNames::WindDirection, LevelWindDirectionYaw);
		}
		if (Condition.WindFromCompassDeg >= 0.0f)
		{
			UDWBridge::SetFloat(WindUDW, UDWNames::WindDirection, UdwYawForWindFrom(Condition.WindFromCompassDeg));
		}
		else if (bHaveLevelWindDirection)
		{
			UDWBridge::SetFloat(WindUDW, UDWNames::WindDirection, LevelWindDirectionYaw);
		}
	}

	if (!bApplied)
	{
		// Fall back to the manual-override path, so a missing function or preset costs fidelity
		// rather than the whole run.
		UE_LOG(LogTemp, Warning,
			TEXT("Condition '%s': Change Weather unavailable - falling back to manual overrides."),
			*Condition.Label);

		ApplyWeatherStepImmediate(EWeatherKind::Rain, Step);
	}

	// Moving fog is a per-condition property: negative means "use the actor default", so a condition
	// only has to speak up when it wants to differ.
	if (Condition.MovingFogIntensity >= 0.0f)
	{
		SetMovingFog(Condition.MovingFogIntensity > 0.0f, Condition.MovingFogIntensity);
	}
	else
	{
		SetMovingFog(bEnableMovingFog, MovingFogIntensityScale);
	}

	// Cloud height and opacity for this condition. Applied through AnchorWeatherToDrone, which runs
	// again just before the shutter, so it survives UDS's refreshes between here and the capture.
	// Applied before the settle delay, so the tiles have the whole transition window to re-stream.
	// The capture gate waits for 100% regardless, but starting early keeps the run moving.
	SetCesiumSmoothNormals(Condition.bRequiresSmoothNormals);

	bHaveActiveCloudHeightOffset = Condition.bOverrideCloudHeight;
	ActiveCloudHeightOffsetM     = Condition.CloudHeightOffsetM;
	bActiveCloudHeightFromGround = Condition.bCloudHeightFromGround;
	bLoggedCloudAnchor = false; // re-log the calibration when a condition changes the layer

	// Extinction scale is what gives volumetric clouds their depth. One condition lowers it -
	// moving_fog, whose ground layer has to be translucent enough to see through - and until now
	// nothing put it back, so every condition after that one inherited 0.35 against the level's 10.0
	// and rendered as a flat, uniformly tiled sheet instead of soft cloud.
	//
	// Restoring on the way past is the fix, not just clearing the override: conditions run in sequence
	// and the litmus runs the whole table before the capture does, so a value left behind reaches
	// every frame that follows it.
	if (UDSActor.IsValid())
	{
		if (Condition.CloudExtinctionOverride > 0.0f)
		{
			UDWBridge::SetFloat(UDSActor.Get(), UDWNames::CloudExtinctionScale,
				Condition.CloudExtinctionOverride);
		}
		else if (DefaultCloudExtinction > 0.0f)
		{
			UDWBridge::SetFloat(UDSActor.Get(), UDWNames::CloudExtinctionScale, DefaultCloudExtinction);
		}
	}

	AnchorWeatherToDrone();

	// Buffer the condition for the capture that follows.
	ActiveCaptureKind = EWeatherKind::Rain;
	// A new condition means a new transition, so the previous sample is no longer something this one
	// can be compared against - without clearing it the first check could see two readings that match
	// only because the new weather has not started arriving yet, and call it settled immediately.
	bHaveSettleSample = false;

	ActiveCaptureStep = Step;
	return bApplied;
}

bool AMyActor_weather::IsWeatherSettled(float& OutWorstDelta) const
{
	OutWorstDelta = 0.0f;

	AActor* UDW = UDWActor.Get();
	if (!UDW)
	{
		return true; // Nothing to wait for; the missing actor is already reported elsewhere.
	}

	// The GENERATED weather, not the requested channels. Reading the actor's own Rain/Snow/Fog gave 0
	// every time, so this compared 0 against 0 and reported "settled" on the second sample - fast, and
	// meaningless.
	UObject* LocalState = UDWBridge::GetLocalWeatherState(UDW);
	if (!LocalState)
	{
		return true;   // Cannot tell; the bridge reports the missing property.
	}

	float LiveRain = 0.0f, LiveSnow = 0.0f, LiveFog = 0.0f, LiveCloud = 0.0f;
	UDWBridge::GetFloat(LocalState, UDWNames::Rain, LiveRain);
	UDWBridge::GetFloat(LocalState, UDWNames::Snow, LiveSnow);
	UDWBridge::GetFloat(LocalState, UDWNames::Fog, LiveFog);
	UDWBridge::GetFloat(LocalState, UDWNames::CloudCoverage, LiveCloud);

	// Settled means STOPPED CHANGING, not "matches the number we asked for".
	//
	// The old test compared the live state against the condition's requested channels. That worked
	// while every channel was pinned as a manual override, because the pin made live equal target by
	// construction. Since conditions became presets applied through Change Weather, UDW generates the
	// weather itself and lands near those numbers rather than on them - the delta sat at 3.80 and
	// never closed, so every capture waited out the full timeout. Six images took three and a half
	// minutes, of which two and a half were this.
	//
	// Comparing consecutive samples asks the question that actually matters: has the transition
	// finished? It is also correct for a value we never requested, which is now most of them.
	const bool bSettled = bHaveSettleSample && (
		FMath::Max(FMath::Max(
			FMath::Abs(LiveRain  - LastSettleSample.Rain),
			FMath::Abs(LiveSnow  - LastSettleSample.Snow)), FMath::Max(
			FMath::Abs(LiveFog   - LastSettleSample.Fog),
			FMath::Abs(LiveCloud - LastSettleSample.CloudCoverage)))
		<= FMath::Max(WeatherSettleTolerance, KINDA_SMALL_NUMBER));

	OutWorstDelta = bHaveSettleSample
		? FMath::Max(FMath::Max(
			FMath::Abs(LiveRain  - LastSettleSample.Rain),
			FMath::Abs(LiveSnow  - LastSettleSample.Snow)), FMath::Max(
			FMath::Abs(LiveFog   - LastSettleSample.Fog),
			FMath::Abs(LiveCloud - LastSettleSample.CloudCoverage)))
		: WeatherSettleTolerance + 1.0f;   // first sample: nothing to compare against yet

	LastSettleSample.Rain          = LiveRain;
	LastSettleSample.Snow          = LiveSnow;
	LastSettleSample.Fog           = LiveFog;
	LastSettleSample.CloudCoverage = LiveCloud;
	bHaveSettleSample              = true;

	return bSettled;
}

void AMyActor_weather::ApplyConditionLookOverrides(const FWeatherCondition& Condition)
{
	if (!ResolveUDWActor())
	{
		return;
	}

	// Per-condition height fog toggle. Written here because it is a property of the condition rather
	// than of the weather step, and because it must land after the update chain that ApplySkySettings
	// triggers - a bool UDS reads directly has nothing to recompute it, but it can still be
	// overwritten by an earlier write in the same frame.
	if (AActor* UDSForToggle = UDSActor.Get())
	{
		UDWBridge::SetBool(UDSForToggle, UDWNames::RenderExponentialHeightFog, Condition.bRenderHeightFog);
	}

	// Fog channels sampled here rather than in ApplySkySettings. That runs once per session, so the
	// diagnostic printed a single line before all seventeen conditions and could never show what
	// changed at a transition - the one thing it was added to answer. This function runs per
	// condition, which is where the comparison has to be made.
	{
		AActor* UDWForFog = UDWActor.Get();
		AActor* UDSForFog = UDSActor.Get();

		float LivePPWF = 0.0f, LiveWeatherFog = 0.0f, LiveBaseDensity = 0.0f;
		UDWBridge::GetFloat(UDWForFog, UDWNames::PPWFIntensityScale, LivePPWF);
		UDWBridge::GetFloat(UDWForFog, UDWNames::Fog,                LiveWeatherFog);
		UDWBridge::GetFloat(UDSForFog, UDWNames::BaseFogDensity,     LiveBaseDensity);

		float ComponentDensity = -1.0f;
		if (const UExponentialHeightFogComponent* FogComp = GetSkyHeightFogComponent())
		{
			ComponentDensity = FogComp->FogDensity;
		}

		// The addends UDS layers on top of Base Fog Density, plus cloud coverage, which is the input
		// that rises from partly_cloudy onward and is the prime suspect for haze that tracks
		// cloudiness rather than fog. Logged as raw values so the arithmetic can be checked against
		// the resulting component rather than inferred from it.
		float LiveFoggyContrib = 0.0f, LiveCloudyContrib = 0.0f, LiveDustContrib = 0.0f;
		float LiveCurrentFogDensity = 0.0f, LiveCloudCoverage = 0.0f, ComponentFalloff = -1.0f;

		UDWBridge::GetFloat(UDSForFog, UDWNames::FoggyDensityContribution,  LiveFoggyContrib);
		UDWBridge::GetFloat(UDSForFog, UDWNames::CloudyDensityContribution, LiveCloudyContrib);
		UDWBridge::GetFloat(UDSForFog, UDWNames::DustDensityContribution,   LiveDustContrib);
		UDWBridge::GetFloat(UDSForFog, UDWNames::CurrentFogDensity,         LiveCurrentFogDensity);
		UDWBridge::GetFloat(UDSForFog, UDWNames::CloudCoverage,             LiveCloudCoverage);

		if (const UExponentialHeightFogComponent* FogComp = GetSkyHeightFogComponent())
		{
			ComponentFalloff = FogComp->FogHeightFalloff;
		}

		UE_LOG(LogTemp, Log,
			TEXT("[FogChannels] %s: weatherFog=%.2f cloudCover=%.2f | base=%.4f + foggyContrib=%.4f ")
			TEXT("+ cloudyContrib=%.4f + dustContrib=%.4f -> currentFogDensity=%.4f | component=%.4f ")
			TEXT("falloff=%.3f | PPWF=%.2f"),
			*Condition.Label, LiveWeatherFog, LiveCloudCoverage,
			LiveBaseDensity, LiveFoggyContrib, LiveCloudyContrib, LiveDustContrib,
			LiveCurrentFogDensity, ComponentDensity, ComponentFalloff, LivePPWF);
	}

	AActor* UDW = UDWActor.Get();

	// Particle size, the same for every condition (a size, not an amount).
	if (RainDropScale > 0.0f)
	{
		UDWBridge::SetFloat(UDW, UDWNames::RainDropsScale, RainDropScale);
	}
	if (SnowFlakeScale > 0.0f)
	{
		UDWBridge::SetFloat(UDW, UDWNames::SnowFlakesScale, SnowFlakeScale);
	}

	// Snow and rain have separate particle properties, so route by what this condition precipitates.
	const bool bSnowing = ActiveCaptureStep.Snow > ActiveCaptureStep.Rain;

	if (Condition.ParticleSpawnCount > 0)
	{
		UDWBridge::SetInt(UDW,
			bSnowing ? UDWNames::SnowParticleSpawnCount : UDWNames::RainParticleSpawnCount,
			Condition.ParticleSpawnCount);
	}
	if (Condition.ParticleScale > 0.0f)
	{
		UDWBridge::SetFloat(UDW,
			bSnowing ? UDWNames::SnowFlakesScale : UDWNames::RainDropsScale,
			Condition.ParticleScale);
	}
	if (Condition.ParticleAlpha > 0.0f)
	{
		UDWBridge::SetFloat(UDW,
			bSnowing ? UDWNames::SnowFlakesAlpha : UDWNames::RainDropsAlpha,
			Condition.ParticleAlpha);
	}
}

void AMyActor_weather::ClearSnapshotFolder()
{
	if (!bClearSnapshotFolderOnRun)
	{
		return;
	}

	if (ResolvedSnapshotDir.IsEmpty())
	{
		ResolveOutputDirs();
	}

	IFileManager& FM = IFileManager::Get();

	// Non-recursive, and only the two things this pipeline writes. Anything the user has filed away
	// into a subfolder, and any other file type, is deliberately out of scope - this runs
	// automatically at the start of every run, so it must never be able to reach further than the
	// output it produced itself.
	TArray<FString> StalePngs;
	FM.FindFiles(StalePngs, *(ResolvedSnapshotDir / TEXT("*.png")), true, false);

	int32 Removed = 0;
	for (const FString& File : StalePngs)
	{
		if (FM.Delete(*(ResolvedSnapshotDir / File)))
		{
			++Removed;
		}
	}

	const FString MetadataPath = ResolvedSnapshotDir / TEXT("weather_metadata.json");
	if (FM.FileExists(*MetadataPath))
	{
		FM.Delete(*MetadataPath);
	}

	if (Removed > 0)
	{
		UE_LOG(LogTemp, Log,
			TEXT("Cleared %d image(s) from the previous run out of %s."), Removed, *ResolvedSnapshotDir);
	}
}

void AMyActor_weather::StartMatchedCaptureRun()
{
	BuildDefaultCaptureRun();

	if (CaptureConditions.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("Matched capture run has no conditions - nothing to do. Exiting."));
		RequestGameExit();
		return;
	}

	// Start from an empty folder, so a run that stops early cannot leave its frames mixed with a
	// previous run's and look like a complete dataset.
	ClearSnapshotFolder();
	SpotTimeOfDay = -1.0f;                                // the spots set their own as they are visited

	// With the flight path off the run follows wherever you fly, so the pose is taken live at each
	// capture rather than pinned here. Locking it would freeze the framing at the start position and
	// defeat the point of flying manually.
	ReleaseCapturePose();

	bPipelineActive       = true;
	bMetadataFileStarted  = false;
	CurrentConditionIndex = 0;
	CurrentWaypointIndex  = 0;
	CapturedImageCount    = 0;

	// Re-sample the terrain for this run, so moving the Cesium georeference between runs is picked up.
	bHaveCachedTerrainZ   = false;

	const int32 StopsPerCondition = bUseFlightPath ? FMath::Max(1, FlightPath.Num()) : 1;

	// Wall-clock start, so the cost of a tile-quality change can be measured rather than estimated.
	// Screen-space error is logged alongside because it is the variable being compared between runs
	// and is otherwise invisible in the log - two runs would look identical.
	CaptureRunStartTime = FPlatformTime::Seconds();

	UE_LOG(LogTemp, Log,
		TEXT("Capture run: %d conditions x %d pose(s) = %d images. Flight path %s, time of day pinned at %.0f. ")
		TEXT("Cesium max screen-space error: %s (lower = finer tiles)."),
		CaptureConditions.Num(), StopsPerCondition,
		CaptureConditions.Num() * StopsPerCondition,
		bUseFlightPath ? TEXT("on") : TEXT("off (single pose)"), TimeOfDay,
		CesiumMaximumScreenSpaceError > 0.0f
			? *FString::Printf(TEXT("%.1f"), CesiumMaximumScreenSpaceError)
			: TEXT("16.0 (Cesium default, not overridden)"));

	BeginCurrentCondition();
}

void AMyActor_weather::BeginCurrentCondition()
{
	if (!CaptureConditions.IsValidIndex(CurrentConditionIndex))
	{
		const int32 StopsPerCondition = bUseFlightPath ? FMath::Max(1, FlightPath.Num()) : 1;
		const int32 Expected = CaptureConditions.Num() * StopsPerCondition;

		if (CapturedImageCount == Expected)
		{
			// Seconds per image is the comparable figure between runs - total time alone is
			// meaningless unless both runs captured the same count, which a changed filter or
			// waypoint count would silently break.
			const double Elapsed = FPlatformTime::Seconds() - CaptureRunStartTime;

			UE_LOG(LogTemp, Log,
				TEXT("Capture run complete: %d/%d images written in %.1fs (%.2fs per image) at ")
				TEXT("screen-space error %s."),
				CapturedImageCount, Expected, Elapsed,
				CapturedImageCount > 0 ? Elapsed / CapturedImageCount : 0.0,
				CesiumMaximumScreenSpaceError > 0.0f
					? *FString::Printf(TEXT("%.1f"), CesiumMaximumScreenSpaceError)
					: TEXT("16.0 (default)"));
		}
		else
		{
			// A condition that produced nothing - the fog frame going missing, say - is otherwise
			// only noticeable by counting files by hand.
			UE_LOG(LogTemp, Warning,
				TEXT("Capture run finished with %d images but expected %d. A condition did not produce its capture."),
				CapturedImageCount, Expected);
		}

		// More locations to visit? Move the globe and run the whole sequence again there. Checked
		// before the exit timer is armed, so a multi-site run never half-exits between sites.
		if (AdvanceToNextSite())
		{
			return;
		}

		ClearUDWWeather();
		GetWorldTimerManager().SetTimer(ExitTimerHandle, this, &AMyActor_weather::RequestGameExit,
			WeatherConstants::ExitDelaySeconds, false);
		return;
	}

	const FWeatherCondition& Condition = CaptureConditions[CurrentConditionIndex];
	CurrentWaypointIndex = 0;

	ApplyConditionPreset(Condition, ConditionTransitionSeconds);

	// The sky finishes transitioning before the ground does: material wetness and DLWE snow build up
	// over their own durations, so the first capture waits for both.
	const float FirstCaptureDelay = ConditionTransitionSeconds + ConditionSettleSeconds;

	UE_LOG(LogTemp, Log, TEXT("Condition %d/%d '%s': transitioning over %.1fs, first capture in %.1fs."),
		CurrentConditionIndex + 1, CaptureConditions.Num(), *Condition.Label,
		ConditionTransitionSeconds, FirstCaptureDelay);

	GetWorldTimerManager().SetTimer(RunTimerHandle, this,
		&AMyActor_weather::GoToCurrentWaypoint, FirstCaptureDelay, false);
}

void AMyActor_weather::GoToCurrentWaypoint()
{
	// With the flight path off, a condition is a single capture from wherever the drone already is.
	const int32 StopsThisCondition = bUseFlightPath ? FlightPath.Num() : 1;

	if (CurrentWaypointIndex >= StopsThisCondition)
	{
		// Done with this condition - move to the next one.
		++CurrentConditionIndex;
		BeginCurrentCondition();
		return;
	}

	if (bUseFlightPath && FlightPath.IsValidIndex(CurrentWaypointIndex))
	{
		if (bFlyBetweenWaypoints && CurrentWaypointIndex > 0)
		{
			StartGlideToCurrentWaypoint();   // TickGlide captures on arrival
			return;
		}
		PlaceCameraAtWaypoint(FlightPath[CurrentWaypointIndex]);
	}

	// The transition has landed by now, so the particle look can be written without the transition
	// recomputing it away. Re-applied at every waypoint because UDW recomputes on its own updates.
	if (CaptureConditions.IsValidIndex(CurrentConditionIndex))
	{
		ApplyConditionLookOverrides(CaptureConditions[CurrentConditionIndex]);
	}

	// Tell UDW to move its particle emitters onto the new control point now. It does this on its own
	// tick otherwise, which is a frame or more behind the teleport - and warming up before that
	// happens fills the volume at the position the drone just left.
	if (AActor* UDW = UDWActor.Get())
	{
		UDWBridge::CallFunction(UDW, UDWNames::UpdateParticlesParentLocation);
	}

	GetWorldTimerManager().SetTimer(RunTimerHandle, this,
		&AMyActor_weather::CaptureCurrentWaypoint,
		FMath::Max(WaypointSettleSeconds, 0.05f), false);
}

void AMyActor_weather::CaptureCurrentWaypoint()
{
	if (!CaptureConditions.IsValidIndex(CurrentConditionIndex))
	{
		return;
	}
	const FWeatherCondition& Condition = CaptureConditions[CurrentConditionIndex];

	// Wait here, on the run's own timer, until the scene and the weather have both arrived. Doing it
	// here rather than inside CaptureWeatherSnapshot is what keeps the run and the capture in
	// lockstep: the run cannot advance to the next condition until this waypoint's image exists.
	float LowestProgress = 100.0f;
	float WorstDelta     = 0.0f;

	const bool bTilesReady   = !bWaitForCesiumTiles    || AreCesiumTilesetsLoaded(LowestProgress);
	const bool bWeatherReady = !bWaitForWeatherSettled || IsWeatherSettled(WorstDelta);

	const float WaitTimeout = FMath::Max(
		bWaitForCesiumTiles    ? CesiumTileLoadTimeoutSeconds : 0.0f,
		bWaitForWeatherSettled ? WeatherSettleTimeoutSeconds  : 0.0f);

	if (!bTilesReady || !bWeatherReady)
	{
		if (CaptureWaitElapsed < WaitTimeout)
		{
			CaptureWaitElapsed += CesiumTileCheckIntervalSeconds;
			GetWorldTimerManager().SetTimer(RunTimerHandle, this,
				&AMyActor_weather::CaptureCurrentWaypoint,
				CesiumTileCheckIntervalSeconds, false);
			return;
		}

		// Timed out - capture anyway rather than losing the frame, but say so.
		UE_LOG(LogTemp, Warning,
			TEXT("Condition '%s': capturing after %.1fs without everything settling (Cesium %.1f%%, worst weather delta %.2f). This frame may not match its metadata."),
			*Condition.Label, CaptureWaitElapsed, LowestProgress, WorstDelta);
	}
	else if (CaptureWaitElapsed > 0.0f)
	{
		UE_LOG(LogTemp, Log, TEXT("Condition '%s': waited %.1fs for the tiles and the weather to settle."),
			*Condition.Label, CaptureWaitElapsed);
	}

	CaptureWaitElapsed = 0.0f;

	// Terrain following, first visit only. The ground is probed now rather than on arrival because only
	// now are the tiles under this waypoint loaded - a probe on arrival hits coarse placeholder tiles.
	// If the pose moved, the view changed, so go round the settle once more before the shutter. Later
	// conditions find the waypoint already resolved and fly straight to the same pose.
	if (bUseFlightPath && bFollowTerrain && FlightPath.IsValidIndex(CurrentWaypointIndex) &&
		!FlightPath[CurrentWaypointIndex].bGroundResolved)
	{
		// A spot drawn again goes round the same wait: tiles at its new place, then this settle.
		if (SettleSpotInOpenAir(FlightPath[CurrentWaypointIndex].Spot) != ESpotSettle::Ready)
		{
			PlaceCameraAtWaypoint(FlightPath[CurrentWaypointIndex]);
			GetWorldTimerManager().SetTimer(RunTimerHandle, this,
				&AMyActor_weather::CaptureCurrentWaypoint,
				FMath::Max(WaypointSettleSeconds, 0.05f), false);
			return;
		}
	}

	// Warm up here rather than on arrival. By now UDW has ticked and its emitters have followed the
	// control point to this waypoint, so advancing the Niagara systems fills the volume that is
	// actually about to be photographed. Done at arrival instead, it populated the previous
	// waypoint's volume and the frame was still filling in - the visible lag after every pose change.
	// Not in flight: the particles have followed the drone continuously, and refilling the volume
	// at every picture would pop. The first waypoint of a condition is a teleport and still needs it.
	AActor* WarmUDW = UDWActor.Get();
	if (WarmUDW && !(bFlyBetweenWaypoints && CurrentWaypointIndex > 0))
	{
		UDWBridge::CallFunction(WarmUDW, UDWNames::WarmUpNiagaraSystems);
	}

	// Re-anchor the cloud layer here too. UDS refreshes between steps and recomputes the layer from
	// its own settings, so an anchor applied only when the condition changed has had several seconds
	// and many ticks in which to be undone before this frame is taken.
	AnchorWeatherToDrone();

	// Flying manually: take the pose from the player's actual view at the moment of capture, so the
	// image is what is on screen. The waypoint path sets this itself, so only do it when free-flying.
	if (!bUseFlightPath)
	{
		if (APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0))
		{
			SetCapturePose(FTransform(Cam->GetCameraRotation(), Cam->GetCameraLocation()));
		}
		else if (TargetThirdPersonCharacter)
		{
			// Simulate In Editor has no camera manager; the pawn is the best available reference.
			SetCapturePose(TargetThirdPersonCharacter->GetActorTransform());
		}
	}

	if (CurrentWaypointIndex == 0)
	{
		LogSceneGeometry(*FString::Printf(TEXT("'%s' first capture"), *Condition.Label));
	}

	CaptureWeatherSnapshot(Condition.Label);

	++CurrentWaypointIndex;
	GetWorldTimerManager().SetTimer(RunTimerHandle, this,
		&AMyActor_weather::GoToCurrentWaypoint,
		WeatherConstants::WeatherChainDelaySeconds, false);
}

void AMyActor_weather::CancelWeatherTransition()
{
	bTransitionActive = false;
	TransitionElapsed = 0.0f;
	GetWorldTimerManager().ClearTimer(TransitionTimerHandle);
}

void AMyActor_weather::ApplyWeatherStep(EWeatherKind Kind, const FUDWWeatherStep& Step)
{
	CancelWeatherTransition();

	// Buffer the *target* now, not at the end of the ramp: the snapshot fires on a settle timer that
	// outlasts the ramp, so the metadata must describe where the weather is going, which is also
	// where it will be when the shutter opens.
	ActiveCaptureKind = Kind;
	ActiveCaptureStep = Step;

	const bool bCanRamp =
		bInterpolateBetweenSteps &&
		bHaveLastStep &&
		!bLitmusActive &&                 // the litmus is three hard cuts by definition
		TransitionSeconds > 0.0f &&
		TransitionUpdatesPerSecond > 0.0f;

	if (!bCanRamp)
	{
		ApplyWeatherStepImmediate(Kind, Step);
		LastAppliedStep = Step;
		bHaveLastStep   = true;
		return;
	}

	TransitionKind    = Kind;
	TransitionFrom    = LastAppliedStep;
	TransitionTo      = Step;
	TransitionElapsed = 0.0f;
	bTransitionActive = true;

	UE_LOG(LogTemp, Log, TEXT("Weather ramp [%s -> %s] over %.1fs."),
		*LastAppliedStep.Label, *Step.Label, TransitionSeconds);

	GetWorldTimerManager().SetTimer(TransitionTimerHandle, this,
		&AMyActor_weather::TransitionTick,
		1.0f / TransitionUpdatesPerSecond, true);
}

void AMyActor_weather::TransitionTick()
{
	if (!bTransitionActive)
	{
		GetWorldTimerManager().ClearTimer(TransitionTimerHandle);
		return;
	}

	TransitionElapsed += 1.0f / FMath::Max(TransitionUpdatesPerSecond, KINDA_SMALL_NUMBER);

	const float Alpha = FMath::Clamp(TransitionElapsed / TransitionSeconds, 0.0f, 1.0f);

	// Ease in and out, so the ramp does not start and stop abruptly at its own endpoints.
	const float Smoothed = FMath::SmoothStep(0.0f, 1.0f, Alpha);

	const FUDWWeatherStep Blended = LerpWeatherStep(TransitionFrom, TransitionTo, Smoothed);
	ApplyWeatherStepImmediate(TransitionKind, Blended);

	if (Alpha >= 1.0f)
	{
		// Land exactly on the target rather than on the last blended approximation, so the captured
		// frame matches the metadata to the digit.
		ApplyWeatherStepImmediate(TransitionKind, TransitionTo);
		LastAppliedStep = TransitionTo;
		bHaveLastStep   = true;
		CancelWeatherTransition();
	}
}

void AMyActor_weather::ApplyWeatherStepImmediate(EWeatherKind Kind, const FUDWWeatherStep& Step)
{

	if (!ResolveUDWActor())
	{
		UE_LOG(LogTemp, Error,
			TEXT("ApplyWeatherStep: no Ultra_Dynamic_Weather actor in the level - no weather will be applied."));
		return;
	}

	AActor*  UDW   = UDWActor.Get();
	UObject* State = UDWManualState.Get();

	// 1. The weather state itself. UDS reads the same state to drive clouds, sky colour, sun
	//    scattering and volumetric fog, which is what keeps sky and weather coherent.
	UDWBridge::SetFloat(State, UDWNames::Rain, Step.Rain);
	UDWBridge::SetFloat(State, UDWNames::Snow, Step.Snow);
	UDWBridge::SetFloat(State, UDWNames::Fog, Step.Fog);
	UDWBridge::SetFloat(State, UDWNames::CloudCoverage, Step.CloudCoverage);
	UDWBridge::SetFloat(State, UDWNames::WindIntensity, Step.WindIntensity);
	UDWBridge::SetFloat(State, UDWNames::WindDirection, Step.WindDirectionDeg);
	UDWBridge::SetFloat(State, UDWNames::ThunderLightning, Step.ThunderLightning);
	UDWBridge::SetFloat(State, UDWNames::MaterialWetness, Step.MaterialWetness);
	UDWBridge::SetFloat(State, UDWNames::MaterialSnowCoverage, Step.MaterialSnowCoverage);

	// 2. Tell UDW to actually use those values. Without these the manual state is inert and UDW's
	//    own weather simulation wins.
	UDWBridge::SetBool(UDW, UDWNames::RainOverride, true);
	UDWBridge::SetBool(UDW, UDWNames::SnowOverride, true);
	UDWBridge::SetBool(UDW, UDWNames::FogOverride, true);
	UDWBridge::SetBool(UDW, UDWNames::CloudCoverageOverride, true);
	UDWBridge::SetBool(UDW, UDWNames::WindIntensityOverride, true);
	UDWBridge::SetBool(UDW, UDWNames::ThunderLightningOverride, true);
	UDWBridge::SetBool(UDW, UDWNames::MaterialWetnessOverride, true);
	UDWBridge::SetBool(UDW, UDWNames::MaterialSnowCoverageOverride, true);

	// 3. Keep the particle bubble on the drone before the state update, so a step never renders even
	//    one frame at the previous control point.
	UpdateWeatherControlPoint();

	// 4. Snap or ease? The litmus wants hard cuts between rain, snow and fog; the capture sweep wants
	//    a smooth build-up within a phase. bUseInstantWeatherNow carries whichever is in force.
	if (bForceInstantWeatherChange)
	{
		UDWBridge::SetBool(UDW, UDWNames::InstantWeatherUpdating, bUseInstantWeatherNow);
	}

	// 5. Push the state through UDW's update chain. This MUST happen before the particle settings
	//    below: the chain recomputes Rain/Snow Particle Spawn Count (and the scales) from the weather
	//    state, so anything written beforehand is silently recomputed away. Writing the look
	//    afterwards is the whole reason the raised particle counts had no visible effect.
	ForceUDWWeatherUpdate();

	// 5b. Re-assert the lightning suppression. Setting it once at BeginPlay was not enough: every
	//     condition pins a Thunder/Lightning value from its preset and then runs the update chain
	//     above, which is free to re-arm the flash. rain_light carries thunder = 1.0, which is why a
	//     yellow flash reappeared there specifically - low enough to look like an artefact rather
	//     than a storm, so it read as a bug rather than as lightning.
	if (bDisableLightningFlashes)
	{
		UDWBridge::SetBool(UDW, UDWNames::SpawnLightningFlashes, false);
		UDWBridge::SetFloat(UDW, UDWNames::LightningFlashBloomIntensity, 0.0f);
		UDWBridge::SetFloat(UDW, UDWNames::LightningFlashLightIntensity, 0.0f);
	}

	// 6. Particle look and extent, layered on top of what the update chain just derived. Zero means
	//    "leave UDW's own value alone", so a step only overrides what it actually varies.
	if (Step.RainParticleSpawnCount > 0)   UDWBridge::SetInt(UDW, UDWNames::RainParticleSpawnCount, Step.RainParticleSpawnCount);
	if (Step.SnowParticleSpawnCount > 0)   UDWBridge::SetInt(UDW, UDWNames::SnowParticleSpawnCount, Step.SnowParticleSpawnCount);
	if (Step.RainDropsScale > 0.0f)        UDWBridge::SetFloat(UDW, UDWNames::RainDropsScale, Step.RainDropsScale);
	if (Step.SnowFlakesScale > 0.0f)       UDWBridge::SetFloat(UDW, UDWNames::SnowFlakesScale, Step.SnowFlakesScale);
	if (Step.SpawnRadius > 0.0f)           UDWBridge::SetFloat(UDW, UDWNames::MaxSpawnDistance, Step.SpawnRadius);
	if (Step.MinimumParticleDistance > 0.0f) UDWBridge::SetFloat(UDW, UDWNames::MinimumParticleDistance, Step.MinimumParticleDistance);
	if (Step.RainWindVelocity > 0.0f)      UDWBridge::SetFloat(UDW, UDWNames::RainWindVelocity, Step.RainWindVelocity);
	if (Step.SnowWindVelocity > 0.0f)      UDWBridge::SetFloat(UDW, UDWNames::SnowWindVelocity, Step.SnowWindVelocity);
	if (Step.RainDropsAlpha > 0.0f)        UDWBridge::SetFloat(UDW, UDWNames::RainDropsAlpha, Step.RainDropsAlpha);
	if (Step.SnowFlakesAlpha > 0.0f)       UDWBridge::SetFloat(UDW, UDWNames::SnowFlakesAlpha, Step.SnowFlakesAlpha);

	// 7. Ground accumulation depth, likewise after the update.
	if (bEnableDLWE)
	{
		if (Step.SnowDepth > 0.0f)      UDWBridge::SetFloat(UDW, UDWNames::SnowDepth, Step.SnowDepth);
		if (Step.PuddleCoverage > 0.0f) UDWBridge::SetFloat(UDW, UDWNames::PuddleCoverage, Step.PuddleCoverage);
	}

	// 8. Anything the struct does not cover, addressed by name.
	for (const TPair<FName, float>& Pair : Step.ExtraFloats)
	{
		UDWBridge::SetFloat(UDW, Pair.Key, Pair.Value);
	}
	for (const TPair<FName, bool>& Pair : Step.ExtraBools)
	{
		UDWBridge::SetBool(UDW, Pair.Key, Pair.Value);
	}

	// 9. Sky and time of day move in the same tick as the weather.
	ApplySkySettings(Step);

	// 10. "Refresh Settings" is a Details-panel button that UDS applies through
	//     PostEditChangeProperty, which never fires at runtime. It is set for editor parity only.
	//     Deliberately NOT followed by another ForceUDWWeatherUpdate(): the update chain is what
	//     recomputes the particle counts, so running it again here would undo step 6.
	UDWBridge::SetBool(UDW, UDWNames::RefreshSettings, true);

	// A ramp calls this many times a second, so its increments go to Verbose and only the settled
	// step is logged normally. UE_LOG takes the verbosity as a compile-time token, so the message is
	// built once and the choice of call is made here rather than inside the macro.
	const FString StepSummary = FString::Printf(
		TEXT("Weather step [%s/%s]: rain=%.2f snow=%.2f fog=%.2f cloud=%.2f wind=%.2f@%.0fdeg snowCov=%.2f depth=%.0f tod=%.0f"),
		*WeatherKindToString(Kind), *Step.Label,
		Step.Rain, Step.Snow, Step.Fog, Step.CloudCoverage,
		Step.WindIntensity, Step.WindDirectionDeg,
		Step.MaterialSnowCoverage, Step.SnowDepth, ActiveCaptureTimeOfDay);

	if (bTransitionActive)
	{
		UE_LOG(LogTemp, Verbose, TEXT("%s"), *StepSummary);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("%s"), *StepSummary);
	}

	OnUDWStepApplied(Kind, Step);
}

void AMyActor_weather::ForceUDWWeatherUpdate()
{
	AActor* UDW = UDWActor.Get();
	if (!UDW)
	{
		return;
	}

	// The "<X> - Manual Override" booleans are replicated properties whose RepNotify handlers are what
	// actually mark the update bindings dirty. Writing a property through reflection stores the value
	// but never fires its OnRep, so UDW kept rebuilding its local weather from the same binding set
	// that was assembled at BeginPlay - one with no manual-override contribution in it. Calling the
	// handlers by hand is what tells UDW the override set changed.
	static const FName ManualOverrideRepNotifies[] =
	{
		FName(TEXT("OnRep_Rain - Manual Override")),
		FName(TEXT("OnRep_Snow - Manual Override")),
		FName(TEXT("OnRep_Fog - Manual Override")),
		FName(TEXT("OnRep_Cloud Coverage - Manual Override")),
		FName(TEXT("OnRep_Wind Intensity - Manual Override")),
		FName(TEXT("OnRep_Thunder/Lightning - Manual Override")),
		FName(TEXT("OnRep_Material Wetness - Manual Override")),
		FName(TEXT("OnRep_Material Snow Coverage - Manual Override"))
	};

	static const FName CopyManualStateName(TEXT("Copy Manual State Object to Variables"));
	static const FName RequestBindingUpdateName(TEXT("Request Global Weather Binding Update"));

	if (!bResolvedApplyFunction)
	{
		bResolvedApplyFunction = true;

		const TArray<FName> Candidates =
		{
			FName(TEXT("Update Current Global And Local Weather State")),
			FName(TEXT("Request Global Weather Binding Update")),
			FName(TEXT("Full Reconstruction at Runtime"))
		};

		ResolvedApplyFunction = UDWBridge::ResolveNoArgFunction(UDW, Candidates);

		if (ResolvedApplyFunction != NAME_None)
		{
			UE_LOG(LogTemp, Log,
				TEXT("UDW weather apply path resolved to '%s'."), *ResolvedApplyFunction.ToString());
		}
		else
		{
			UE_LOG(LogTemp, Warning,
				TEXT("None of the candidate UDW weather-apply functions exist on this Ultra Dynamic Sky version, so manual overrides will not take effect. Dumping the callable function list so the correct name can be picked:"));
			UDWBridge::DumpCallableFunctions(UDW, TEXT("Ultra_Dynamic_Weather"));
		}
	}

	// 1. UDW reads the manual values from flat variables on the actor, not from the Manual Weather
	//    State object directly - the object is the authoring surface, and this function is the sync
	//    step between the two. Without it the object holds our values and nothing ever reads them.
	UDWBridge::CallFunction(UDW, CopyManualStateName);

	// 2. Tell UDW the set of active overrides changed, so it rebinds its update functions.
	for (const FName& RepNotify : ManualOverrideRepNotifies)
	{
		UDWBridge::CallFunction(UDW, RepNotify);
	}
	UDWBridge::CallFunction(UDW, RequestBindingUpdateName);

	// 3. Regenerate the global and local weather state through the freshly rebound chain.
	if (ResolvedApplyFunction != NAME_None)
	{
		UDWBridge::CallFunction(UDW, ResolvedApplyFunction);
	}
}

// Cesium3DTileset lives in the CesiumRuntime plugin, which this module deliberately does not depend
// on - the pipeline has to keep building if Cesium is ever removed. Everything below therefore goes
// through the same name-based reflection the UDS/UDW bridge uses. Names verified against
// Cesium3DTileset.h in the installed plugin.
namespace CesiumNames
{
	static const TCHAR* TilesetClassPath = TEXT("/Script/CesiumRuntime.Cesium3DTileset");

	static const FName ForbidHoles(TEXT("ForbidHoles"));
	static const FName EnableFrustumCulling(TEXT("EnableFrustumCulling"));
	static const FName EnableFogCulling(TEXT("EnableFogCulling"));
	static const FName EnforceCulledScreenSpaceError(TEXT("EnforceCulledScreenSpaceError"));
	static const FName PreloadSiblings(TEXT("PreloadSiblings"));
	static const FName MaximumSimultaneousTileLoads(TEXT("MaximumSimultaneousTileLoads"));
	static const FName MaximumCachedBytes(TEXT("MaximumCachedBytes"));
	static const FName MaximumScreenSpaceError(TEXT("MaximumScreenSpaceError"));
	static const FName LoadProgress(TEXT("LoadProgress"));
}

bool AMyActor_weather::GatherCesiumTilesets(TArray<AActor*>& OutTilesets) const
{
	OutTilesets.Reset();

	UClass* TilesetClass = LoadObject<UClass>(nullptr, CesiumNames::TilesetClassPath);
	if (!TilesetClass)
	{
		return false; // Cesium is not installed in this project - nothing to do, and not an error.
	}

	UGameplayStatics::GetAllActorsOfClass(GetWorld(), TilesetClass, OutTilesets);
	return true;
}

void AMyActor_weather::ApplyRenderQuality()
{
	if (RenderQuality < 0)
	{
		return;
	}
	// Session only: not saved to GameUserSettings, so the editor keeps its own scalability.
	Scalability::FQualityLevels Levels = Scalability::GetQualityLevels();
	Levels.SetFromSingleQualityLevel(FMath::Clamp(RenderQuality, 0, 4));
	Scalability::SetQualityLevels(Levels, /*bForce=*/true);

	static const TCHAR* Names[] = { TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic"), TEXT("Cinematic") };
	UE_LOG(LogTemp, Log, TEXT("[Quality] Render quality %d (%s) for this session."),
		FMath::Clamp(RenderQuality, 0, 4), Names[FMath::Clamp(RenderQuality, 0, 4)]);
}

void AMyActor_weather::ConfigureCesiumForCapture()
{
	if (!bConfigureCesiumForCapture)
	{
		return;
	}

	TArray<AActor*> Tilesets;
	if (!GatherCesiumTilesets(Tilesets) || Tilesets.Num() == 0)
	{
		return;
	}

	for (AActor* Tileset : Tilesets)
	{
		// The blue gaps are tiles that have not finished streaming. ForbidHoles makes Cesium fall
		// back to a lower-resolution parent tile instead of rendering nothing, which is the one
		// setting that actually guarantees no holes rather than merely making them less likely.
		UDWBridge::SetBool(Tileset, CesiumNames::ForbidHoles, true);

		// Both cullers unload tiles the camera cannot currently see. That is the right trade for
		// interactive play and the wrong one for capture: a tile culled at the frame edge, or near
		// the horizon by the fog culler, reappears as a gap the moment the drone turns toward it.
		UDWBridge::SetBool(Tileset, CesiumNames::EnableFrustumCulling, false);
		UDWBridge::SetBool(Tileset, CesiumNames::EnableFogCulling, false);
		UDWBridge::SetBool(Tileset, CesiumNames::EnforceCulledScreenSpaceError, false);
		UDWBridge::SetBool(Tileset, CesiumNames::PreloadSiblings, true);

		// More tiles in flight means the tileset settles sooner, which matters because every capture
		// now waits for it.
		UDWBridge::SetInt(Tileset, CesiumNames::MaximumSimultaneousTileLoads, CesiumSimultaneousTileLoads);

		// Room to keep the whole route loaded, so later conditions do not stream it again. Cesium reads
		// this every tick, so setting it at runtime takes effect straight away.
		if (CesiumTileMemoryMB > 0)
		{
			if (FInt64Property* Bytes = FindFProperty<FInt64Property>(Tileset->GetClass(), CesiumNames::MaximumCachedBytes))
			{
				Bytes->SetPropertyValue_InContainer(Tileset, static_cast<int64>(CesiumTileMemoryMB) * 1024 * 1024);
				UE_LOG(LogTemp, Log, TEXT("[Cesium] %s keeps up to %d MB of tiles in memory."),
					*Tileset->GetName(), CesiumTileMemoryMB);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[Cesium] %s has no MaximumCachedBytes; tile memory left as is."),
					*Tileset->GetName());
			}
		}

		// Lower screen-space error = finer tiles. 0 leaves Cesium's own value alone.
		if (CesiumMaximumScreenSpaceError > 0.0f)
		{
			UDWBridge::SetFloat(Tileset, CesiumNames::MaximumScreenSpaceError, CesiumMaximumScreenSpaceError);
		}
	}

	UE_LOG(LogTemp, Log,
		TEXT("Cesium configured for capture on %d tileset(s): holes forbidden, frustum/fog culling off."),
		Tilesets.Num());
}

void AMyActor_weather::SetCesiumSmoothNormals(bool bEnable)
{
	// Every flip makes Cesium rebuild its tiles, so do nothing unless the state actually changes -
	// otherwise the run would throw away and re-stream the whole tileset at every single condition.
	if (bKnowSmoothNormalsState && bSmoothNormalsCurrentlyOn == bEnable)
	{
		return;
	}

	TArray<AActor*> Tilesets;
	if (!GatherCesiumTilesets(Tilesets) || Tilesets.Num() == 0)
	{
		return;
	}

	static const FName SetSmoothNormals(TEXT("SetGenerateSmoothNormals"));
	for (AActor* Tileset : Tilesets)
	{
		// Through the setter, not the property: normals are baked when a tile is built, so only the
		// setter's rebuild makes the change visible.
		UDWBridge::CallFunctionOneBool(Tileset, SetSmoothNormals, bEnable);
	}

	bSmoothNormalsCurrentlyOn = bEnable;
	bKnowSmoothNormalsState   = true;

	UE_LOG(LogTemp, Log,
		TEXT("Cesium smooth normals %s on %d tileset(s) - tiles will re-stream before the next capture."),
		bEnable ? TEXT("ON") : TEXT("off"), Tilesets.Num());
}

bool AMyActor_weather::AreCesiumTilesetsLoaded(float& OutLowestProgress) const
{
	OutLowestProgress = 100.0f;

	TArray<AActor*> Tilesets;
	if (!GatherCesiumTilesets(Tilesets) || Tilesets.Num() == 0)
	{
		return true; // No Cesium in the level - nothing to wait for.
	}

	bool bAllLoaded = true;
	for (AActor* Tileset : Tilesets)
	{
		float Progress = 0.0f;
		if (!UDWBridge::GetFloat(Tileset, CesiumNames::LoadProgress, Progress))
		{
			continue; // Already logged by the bridge; do not block the run on a renamed property.
		}

		OutLowestProgress = FMath::Min(OutLowestProgress, Progress);

		// Cesium reports 0..100. Complete "enough" rather than exactly 100, because a single tile
		// that will never arrive - a request lost to a ConnectionError, which this project sees
		// regularly against Google's tile service - otherwise pins progress just below 100 and burns
		// the entire settle timeout on every capture. Observed twice in one 5-image run at 99.9%
		// with the weather already fully settled: 50 seconds spent waiting for nothing.
		//
		// The tolerance is small enough that a genuinely unfinished tileset still blocks: at 99.5%
		// the missing geometry is a fraction of one tile, far below what a capture would show.
		if (Progress < CesiumLoadCompleteThresholdPercent)
		{
			bAllLoaded = false;
		}
	}

	return bAllLoaded;
}

void AMyActor_weather::UpdateWeatherControlPoint()
{
	if (!bDriveControlPointFromCharacter)
	{
		return;
	}

	AActor* UDW = UDWActor.Get();
	if (!UDW || !TargetThirdPersonCharacter)
	{
		return;
	}

	// Switch UDW off its own camera lookup once. Left on "Player Camera Location" this would follow
	// the view rather than the drone, and in Simulate In Editor there is no player camera to find at
	// all - which is how the weather ended up centred on the world origin.
	if (!bControlPointModeSet)
	{
		bControlPointModeSet = UDWBridge::SetEnumByDisplayName(
			UDW, UDWNames::ControlPointLocationSource, TEXT("Custom Location"));

		if (bControlPointModeSet)
		{
			UE_LOG(LogTemp, Log, TEXT("UDW control point pinned to %s."), *TargetThirdPersonCharacter->GetName());
		}
	}

	FStructProperty* LocProp = FindFProperty<FStructProperty>(UDW->GetClass(), UDWNames::CustomControlPointLocation);
	if (LocProp && LocProp->Struct == TBaseStructure<FVector>::Get())
	{
		*LocProp->ContainerPtrToValuePtr<FVector>(UDW) = TargetThirdPersonCharacter->GetActorLocation();
	}
}

void AMyActor_weather::StopWeatherCapture()
{
	if (!IsSequenceRunning())
	{
		UE_LOG(LogTemp, Log, TEXT("Weather pipeline is not running - nothing to stop."));
		return;
	}

	// Every timer that could still fire, including the exit timer: stopping must leave PIE running.
	FTimerManager& Timers = GetWorldTimerManager();
	Timers.ClearTimer(RainSequenceTimerHandle);
	Timers.ClearTimer(SnowSequenceTimerHandle);
	Timers.ClearTimer(FogSequenceTimerHandle);
	Timers.ClearTimer(SnapshotTimerHandle);
	Timers.ClearTimer(ExitTimerHandle);
	Timers.ClearTimer(LitmusHandle);
	Timers.ClearTimer(AutoStartTimerHandle);
	Timers.ClearTimer(RunTimerHandle);
	Timers.ClearTimer(SceneSetupHandle);
	Timers.ClearTimer(SiteSettleTimerHandle);   // else the next city starts on its own
	bGliding = false;                           // else the flight lands and takes one more picture
	bLitmusPreviewOnly = false;
	if (!IsHeadlessAutoStart())
	{
		SetPlayerInputLocked(false);
	}
	CaptureWaitElapsed = 0.0f;
	PreRunWaitElapsed  = 0.0f;
	bLoggedPreRunWait  = false;
	CancelWeatherTransition();

	bPipelineActive     = false;
	bLitmusActive       = false;
	bLitmusAppliedAStep = false;
	bLitmusClearing     = false;
	rainState = snowState = fogState = false;

	ClearUDWWeather();

	UE_LOG(LogTemp, Log, TEXT("Weather pipeline stopped by request. Captures already written are kept."));
}

void AMyActor_weather::VerifyWeatherApplied(const FUDWWeatherStep& Step, const TCHAR* Context)
{
	AActor* UDW = UDWActor.Get();
	if (!UDW)
	{
		return;
	}

	float LiveRain = 0.0f, LiveSnow = 0.0f, LiveFog = 0.0f, LiveCloud = 0.0f;
	UDWBridge::GetFloat(UDW, UDWNames::Rain, LiveRain);
	UDWBridge::GetFloat(UDW, UDWNames::Snow, LiveSnow);
	UDWBridge::GetFloat(UDW, UDWNames::Fog, LiveFog);
	UDWBridge::GetFloat(UDW, UDWNames::CloudCoverage, LiveCloud);

	// Absolute tolerance on a 0..10 scale - the old 0.05 was chosen when these were believed to be
	// 0..1 and is unreasonably strict at ten times the range, especially while easing between steps.
	const float Tolerance = 0.25f;
	const bool bApplied =
		FMath::IsNearlyEqual(LiveRain,  Step.Rain,          Tolerance) &&
		FMath::IsNearlyEqual(LiveSnow,  Step.Snow,          Tolerance) &&
		FMath::IsNearlyEqual(LiveFog,   Step.Fog,           Tolerance) &&
		FMath::IsNearlyEqual(LiveCloud, Step.CloudCoverage, Tolerance);

	if (bApplied)
	{
		UE_LOG(LogTemp, Verbose, TEXT("%s: weather verified live on Ultra_Dynamic_Weather."), Context);
		return;
	}

	UE_LOG(LogTemp, Warning,
		TEXT("%s: weather did NOT take effect. Requested rain=%.2f snow=%.2f fog=%.2f cloud=%.2f, but Ultra_Dynamic_Weather reads rain=%.2f snow=%.2f fog=%.2f cloud=%.2f."),
		Context,
		Step.Rain, Step.Snow, Step.Fog, Step.CloudCoverage,
		LiveRain, LiveSnow, LiveFog, LiveCloud);

	// Dump once, and only when the resolver already believed it had a working apply path - if it
	// found nothing it has dumped the list itself and a second copy adds nothing.
	if (!bLoggedOverrideFailure && ResolvedApplyFunction != NAME_None)
	{
		bLoggedOverrideFailure = true;
		UE_LOG(LogTemp, Warning,
			TEXT("'%s' ran but did not fold the manual overrides in. Dumping Ultra_Dynamic_Weather's callable functions so a different apply path can be chosen:"),
			*ResolvedApplyFunction.ToString());
		UDWBridge::DumpCallableFunctions(UDW, TEXT("Ultra_Dynamic_Weather"));
	}
}

void AMyActor_weather::ClearUDWWeather(bool bRestoreSkyDefaults)
{
	// A ramp still in flight would immediately write weather back over the clear.
	CancelWeatherTransition();
	bHaveLastStep = false;

	if (!ResolveUDWActor())
	{
		return;
	}

	AActor*  UDW   = UDWActor.Get();
	UObject* State = UDWManualState.Get();

	// Zero the values, then release the overrides so UDW goes back to running its own weather.
	UDWBridge::SetFloat(State, UDWNames::Rain, 0.0f);
	UDWBridge::SetFloat(State, UDWNames::Snow, 0.0f);
	UDWBridge::SetFloat(State, UDWNames::Fog, 0.0f);
	UDWBridge::SetFloat(State, UDWNames::CloudCoverage, 0.0f);
	UDWBridge::SetFloat(State, UDWNames::WindIntensity, 0.0f);
	UDWBridge::SetFloat(State, UDWNames::ThunderLightning, 0.0f);
	UDWBridge::SetFloat(State, UDWNames::MaterialWetness, 0.0f);
	UDWBridge::SetFloat(State, UDWNames::MaterialSnowCoverage, 0.0f);

	UDWBridge::SetBool(UDW, UDWNames::RainOverride, false);
	UDWBridge::SetBool(UDW, UDWNames::SnowOverride, false);
	UDWBridge::SetBool(UDW, UDWNames::FogOverride, false);
	UDWBridge::SetBool(UDW, UDWNames::CloudCoverageOverride, false);
	UDWBridge::SetBool(UDW, UDWNames::WindIntensityOverride, false);
	UDWBridge::SetBool(UDW, UDWNames::ThunderLightningOverride, false);
	UDWBridge::SetBool(UDW, UDWNames::MaterialWetnessOverride, false);
	UDWBridge::SetBool(UDW, UDWNames::MaterialSnowCoverageOverride, false);

	UDWBridge::SetBool(UDW, UDWNames::RefreshSettings, true);
	ForceUDWWeatherUpdate();

	// The sky's fog settings are not part of the manual override system, so releasing the overrides
	// does nothing for them. Put them back explicitly or the level stays fogged for good - but only
	// when the session is actually ending. Between conditions this would write UDS's own values back
	// before each one, which is where two separate fog bugs in this file came from.
	if (bRestoreSkyDefaults)
	{
		RestoreSkyFogDefaults();
	}

	// Same reasoning for smooth normals: it is a tileset setting, not weather, so it would otherwise
	// stay on after a snow run and quietly change how every later frame looks.
	SetCesiumSmoothNormals(false);

	// Clear sky as well: cloud coverage is written directly to UDS, so it too has to be undone here.
	if (AActor* UDS = UDSActor.Get())
	{
		UDWBridge::SetFloat(UDS, UDWNames::CloudCoverage, 0.0f);
		UDWBridge::SetBool(UDS, UDWNames::RefreshSettings, true);
	}

	UE_LOG(LogTemp, Log, TEXT("Weather cleared - manual overrides released, sky fog restored."));
}

void AMyActor_weather::ApplyHeaviestStep(EWeatherKind Kind)
{
	const TArray<FUDWWeatherStep>& Steps = StepsFor(Kind);
	if (Steps.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyHeaviestStep: no steps configured for %s."), *WeatherKindToString(Kind));
		return;
	}
	ApplyWeatherStep(Kind, Steps.Last());
}

UObject* AMyActor_weather::LoadPresetDefaults(const FString& PresetPath) const
{
	if (PresetPath.IsEmpty())
	{
		return nullptr;
	}

	// The presets are *instances* of UDS_Weather_Settings_C, not Blueprint classes: the asset
	// imports UDS_Weather_Settings_C as its class and stores the values on the object itself. So the
	// object path is ".../Clear_Skies.Clear_Skies", with no "_C". Both spellings are accepted here
	// anyway, because getting this wrong fails silently into the fallback sweep and the only
	// symptom is "the weather looks like it did before" - which is nearly impossible to read.
	auto Resolve = [](const FString& Path) -> UObject*
	{
		UObject* Loaded = LoadObject<UObject>(nullptr, *Path);
		if (!Loaded)
		{
			return nullptr;
		}
		// Tolerate a path that really does point at a class: use its defaults.
		if (UClass* AsClass = Cast<UClass>(Loaded))
		{
			return AsClass->GetDefaultObject();
		}
		return Loaded;
	};

	if (UObject* Direct = Resolve(PresetPath))
	{
		return Direct;
	}

	// Retry with the "_C" suffix added or removed, whichever the caller did not use.
	FString Alternate;
	if (PresetPath.EndsWith(TEXT("_C")))
	{
		Alternate = PresetPath.LeftChop(2);
	}
	else
	{
		Alternate = PresetPath + TEXT("_C");
	}

	if (UObject* ViaAlternate = Resolve(Alternate))
	{
		return ViaAlternate;
	}

	UE_LOG(LogTemp, Warning,
		TEXT("Weather preset '%s' could not be loaded (also tried '%s'). Falling back to the hand-authored sweep."),
		*PresetPath, *Alternate);
	return nullptr;
}

namespace
{
	// One preset's weather state. Only the channels the pipeline drives; anything a given preset
	// leaves unset simply stays zero (Foggy has no rain, Snow_Blizzard no wetness, and so on).
	struct FPresetState
	{
		float Rain = 0.0f, Snow = 0.0f, Fog = 0.0f, Cloud = 0.0f;
		float Wind = 0.0f, Thunder = 0.0f, Wetness = 0.0f, SnowCover = 0.0f;
	};

	FPresetState LerpPresetState(const FPresetState& A, const FPresetState& B, float T)
	{
		FPresetState R;
		R.Rain      = FMath::Lerp(A.Rain,      B.Rain,      T);
		R.Snow      = FMath::Lerp(A.Snow,      B.Snow,      T);
		R.Fog       = FMath::Lerp(A.Fog,       B.Fog,       T);
		R.Cloud     = FMath::Lerp(A.Cloud,     B.Cloud,     T);
		R.Wind      = FMath::Lerp(A.Wind,      B.Wind,      T);
		R.Thunder   = FMath::Lerp(A.Thunder,   B.Thunder,   T);
		R.Wetness   = FMath::Lerp(A.Wetness,   B.Wetness,   T);
		R.SnowCover = FMath::Lerp(A.SnowCover, B.SnowCover, T);
		return R;
	}
}

bool AMyActor_weather::BuildStepsFromPresetChain(EWeatherKind Kind, const TArray<FString>& Chain,
                                                 TArray<FUDWWeatherStep>& OutSteps) const
{
	// Load every keyframe up front. A chain with a bad entry still works as long as two survive -
	// the progression just has one fewer waypoint.
	TArray<FPresetState> Keys;
	TArray<FString>      KeyNames;

	for (const FString& Path : Chain)
	{
		UObject* Preset = LoadPresetDefaults(Path);
		if (!Preset)
		{
			continue;
		}

		FPresetState K;
		UDWBridge::GetFloat(Preset, UDWNames::Rain, K.Rain);
		UDWBridge::GetFloat(Preset, UDWNames::Snow, K.Snow);
		UDWBridge::GetFloat(Preset, UDWNames::Fog, K.Fog);
		UDWBridge::GetFloat(Preset, UDWNames::CloudCoverage, K.Cloud);
		UDWBridge::GetFloat(Preset, UDWNames::WindIntensity, K.Wind);
		UDWBridge::GetFloat(Preset, UDWNames::ThunderLightning, K.Thunder);
		UDWBridge::GetFloat(Preset, UDWNames::MaterialWetness, K.Wetness);
		UDWBridge::GetFloat(Preset, UDWNames::MaterialSnowCoverage, K.SnowCover);

		Keys.Add(K);
		KeyNames.Add(FPaths::GetBaseFilename(Path));

		UE_LOG(LogTemp, Log,
			TEXT("  preset '%s': rain=%.2f snow=%.2f fog=%.2f cloud=%.2f wind=%.2f thunder=%.2f wet=%.2f snowCov=%.2f"),
			*KeyNames.Last(), K.Rain, K.Snow, K.Fog, K.Cloud, K.Wind, K.Thunder, K.Wetness, K.SnowCover);
	}

	if (Keys.Num() < 2)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("Weather preset chain for %s has fewer than two loadable presets - falling back to the hand-authored sweep."),
			*WeatherKindToString(Kind));
		return false;
	}

	const int32 NumSteps = FMath::Max(2, StepsPerPhase);

	UE_LOG(LogTemp, Log, TEXT("Weather sweep [%s]: %d steps across %d presets (%s -> %s)."),
		*WeatherKindToString(Kind), NumSteps, Keys.Num(), *KeyNames[0], *KeyNames.Last());

	OutSteps.Reset();
	for (int32 i = 0; i < NumSteps; ++i)
	{
		// T walks 0..1 across the whole chain; the last step lands exactly on the final preset, which
		// is what makes the litmus (heaviest step) show that preset unaltered.
		const float T = static_cast<float>(i) / static_cast<float>(NumSteps - 1);

		const float  ChainPos = T * (Keys.Num() - 1);
		const int32  Lower    = FMath::Clamp(FMath::FloorToInt(ChainPos), 0, Keys.Num() - 2);
		const float  Alpha    = ChainPos - static_cast<float>(Lower);
		const FPresetState S0 = LerpPresetState(Keys[Lower], Keys[Lower + 1], Alpha);

		FUDWWeatherStep S;
		S.Rain                 = S0.Rain;
		S.Snow                 = S0.Snow;
		S.Fog                  = S0.Fog;
		S.CloudCoverage        = S0.Cloud;
		S.WindIntensity        = S0.Wind;
		S.ThunderLightning     = S0.Thunder;
		S.MaterialWetness      = S0.Wetness;
		S.MaterialSnowCoverage = S0.SnowCover;

		// Wind direction is swept independently of intensity so the dataset covers several streak
		// angles. Full turn across the phase, offset per kind so the three phases do not coincide.
		const float WindOffset = (Kind == EWeatherKind::Snow) ? 30.0f : 0.0f;
		S.WindDirectionDeg = FMath::Fmod(WindOffset + T * 360.0f, 360.0f);

		// Label by position along the chain, so filenames stay readable at any step count.
		S.Label = FString::Printf(TEXT("%03d"), FMath::RoundToInt(T * 100.0f));

		// Snow accumulation is a 0..1 channel, so it is used directly.
		S.SnowDepth     = (Kind == EWeatherKind::Snow) ? FMath::Lerp(0.0f, 30.0f, S0.SnowCover) : 0.0f;
		S.PuddleCoverage = (Kind == EWeatherKind::Rain) ? S0.Wetness : 0.0f;

		// Settle time has to outlast the ramp, or the shutter opens while the weather is still on its
		// way to the target that the metadata already claims it reached.
		const float BaseSettle =
			(Kind == EWeatherKind::Snow) ? 8.0f :   // ground accumulation lags the sky
			(Kind == EWeatherKind::Fog)  ? 4.0f : 3.0f;
		S.SettleSeconds = BaseSettle + (bInterpolateBetweenSteps ? TransitionSeconds : 0.0f);

		// Everything below overrides what UDW/UDS derive from the state - i.e. it overrides the
		// preset's own look - so it is off unless explicitly asked for.
		if (bOverrideLookForAerialView)
		{
			// The state channels are 0..10; normalise before using one as a lerp alpha. Feeding the
			// raw value in clamps alpha to 1 and hands every step the maximum, which is what made
			// each sweep render identically at full strength.
			const float NRain = FMath::Clamp(S0.Rain / WeatherConstants::UDSStateScale, 0.0f, 1.0f);
			const float NSnow = FMath::Clamp(S0.Snow / WeatherConstants::UDSStateScale, 0.0f, 1.0f);
			const float NFog  = FMath::Clamp(S0.Fog  / WeatherConstants::UDSStateScale, 0.0f, 1.0f);

			S.SpawnRadius             = FMath::Lerp(9000.0f, 22000.0f, T);
			S.MinimumParticleDistance = 150.0f;

			switch (Kind)
			{
			case EWeatherKind::Rain:
				S.RainParticleSpawnCount = FMath::RoundToInt(FMath::Lerp(2000.0f, 34000.0f, NRain));
				S.RainDropsScale         = FMath::Lerp(1.6f, 3.2f, NRain);
				S.RainDropsAlpha         = FMath::Lerp(0.70f, 1.0f, NRain);
				break;

			case EWeatherKind::Snow:
				S.SnowParticleSpawnCount = FMath::RoundToInt(FMath::Lerp(2000.0f, 34000.0f, NSnow));
				S.SnowFlakesScale        = FMath::Lerp(1.6f, 3.2f, NSnow);
				S.SnowFlakesAlpha        = FMath::Lerp(0.75f, 1.0f, NSnow);
				break;

			case EWeatherKind::Fog:
				S.FogDensityScale         = FMath::Lerp(1.0f, 9.0f, NFog);
				S.FogStartDistance        = FMath::Lerp(20000.0f, 0.0f, NFog);
				S.VolumetricFogDistance   = FMath::Lerp(40000.0f, 120000.0f, NFog);
				S.VolumetricFogExtinction = FMath::Lerp(0.4f, 2.5f, NFog);
				break;
			}
		}

		OutSteps.Add(S);
	}

	return true;
}

void AMyActor_weather::BuildDefaultWeatherSteps()
{
	// Presets first: they define rain, cloud, fog and wetness as a matched set, so the sky and
	// lighting are coherent by construction instead of by hand-tuned coincidence. The hand-authored
	// table below is the fallback for when a preset path is wrong or the assets are missing.
	if (bBuildStepsFromPresets)
	{
		if (RainSteps.Num() == 0) { BuildStepsFromPresetChain(EWeatherKind::Rain, RainPresetChain, RainSteps); }
		if (SnowSteps.Num() == 0) { BuildStepsFromPresetChain(EWeatherKind::Snow, SnowPresetChain, SnowSteps); }
		if (FogSteps.Num()  == 0) { BuildStepsFromPresetChain(EWeatherKind::Fog,  FogPresetChain,  FogSteps);  }
	}

	BuildHandAuthoredWeatherSteps();
}

void AMyActor_weather::BuildHandAuthoredWeatherSteps()
{
	// ---- The tuning table for the whole dataset. Edit these numbers, rebuild, rerun. ----
	//
	// Each phase sweeps light -> extreme across CapturesPerWeatherType steps, and the sky is swept
	// with it: cloud coverage rises alongside precipitation so heavy rain never renders under a
	// clear sky. Wind direction rotates so the dataset covers several particle-streak angles.
	//
	// Arrays are only filled when empty, so anything set in the Details panel or DefaultGame.ini
	// wins over these defaults.

	if (RainSteps.Num() == 0)
	{
		// Spawn count and radius have to move together: raising the radius alone spreads the same
		// number of particles over a bigger volume and reads as *less* rain, not more.
		auto MakeRain = [](float RainAmt, float Cloud, float Wind, float WindDeg, float DropScale,
		                   float Radius, int32 Count, float Alpha,
		                   float Wetness, float Puddles, const TCHAR* Label)
		{
			FUDWWeatherStep S;
			// Authored as 0..1 for readability; UDS grades these channels 0..10.
			S.Rain            = RainAmt * WeatherConstants::UDSStateScale;
			S.CloudCoverage   = Cloud   * WeatherConstants::UDSStateScale;
			S.WindIntensity   = Wind    * WeatherConstants::UDSStateScale;
			S.WindDirectionDeg = WindDeg;
			S.RainDropsScale  = DropScale;
			S.SpawnRadius     = Radius;
			S.RainParticleSpawnCount = Count;
			S.RainDropsAlpha  = Alpha;
			S.MaterialWetness = Wetness;
			S.PuddleCoverage  = Puddles;
			S.MinimumParticleDistance = 150.0f;
			S.SettleSeconds   = 3.0f;
			S.Label           = Label;
			return S;
		};

		//                      rain  cloud  wind  deg    scale   radius    count  alpha  wet  puddle
		RainSteps.Add(MakeRain(0.15f, 0.45f, 0.10f,   0.0f, 1.6f,  9000.0f,  4000, 0.70f, 0.15f, 0.0f,  TEXT("drizzle")));
		RainSteps.Add(MakeRain(0.35f, 0.65f, 0.25f,  45.0f, 2.0f, 12000.0f,  9000, 0.80f, 0.40f, 0.2f,  TEXT("light")));
		RainSteps.Add(MakeRain(0.60f, 0.80f, 0.45f, 135.0f, 2.4f, 15000.0f, 16000, 0.90f, 0.65f, 0.45f, TEXT("moderate")));
		RainSteps.Add(MakeRain(0.85f, 0.92f, 0.70f, 225.0f, 2.8f, 18000.0f, 24000, 1.00f, 0.85f, 0.7f,  TEXT("heavy")));

		FUDWWeatherStep Storm = MakeRain(1.0f, 1.0f, 1.0f, 315.0f, 3.2f, 22000.0f, 34000, 1.0f, 1.0f, 1.0f, TEXT("storm"));
		Storm.ThunderLightning = 0.8f * WeatherConstants::UDSStateScale;
		RainSteps.Add(Storm);
	}

	if (SnowSteps.Num() == 0)
	{
		// Snow also ramps the ground: MaterialSnowCoverage is the accumulation target and SnowDepth
		// is how deep DLWE builds it. Accumulation is stateful and lags the sky, so SettleSeconds is
		// longer here than for rain — otherwise the image would show less snow than the metadata says.
		auto MakeSnow = [](float SnowAmt, float Cloud, float Wind, float WindDeg, float FlakeScale,
		                   float Radius, int32 Count, float Alpha,
		                   float Coverage, float Depth, const TCHAR* Label)
		{
			FUDWWeatherStep S;
			// Authored as 0..1 for readability; UDS grades these channels 0..10.
			S.Snow             = SnowAmt * WeatherConstants::UDSStateScale;
			S.CloudCoverage    = Cloud   * WeatherConstants::UDSStateScale;
			S.WindIntensity    = Wind    * WeatherConstants::UDSStateScale;
			S.WindDirectionDeg = WindDeg;
			S.SnowFlakesScale  = FlakeScale;
			S.SpawnRadius      = Radius;
			S.SnowParticleSpawnCount = Count;
			S.SnowFlakesAlpha  = Alpha;
			S.MaterialSnowCoverage = Coverage;
			S.SnowDepth        = Depth;
			S.MinimumParticleDistance = 150.0f;
			S.SettleSeconds    = 8.0f;
			S.Label            = Label;
			return S;
		};

		//                      snow  cloud  wind  deg    scale   radius    count  alpha  cover depth
		SnowSteps.Add(MakeSnow(0.15f, 0.50f, 0.05f,   0.0f, 1.6f,  9000.0f,  4000, 0.75f, 0.10f,  2.0f, TEXT("flurry")));
		SnowSteps.Add(MakeSnow(0.35f, 0.70f, 0.20f,  60.0f, 2.0f, 12000.0f,  9000, 0.85f, 0.35f,  8.0f, TEXT("light")));
		SnowSteps.Add(MakeSnow(0.60f, 0.85f, 0.40f, 150.0f, 2.4f, 15000.0f, 16000, 0.95f, 0.60f, 15.0f, TEXT("moderate")));
		SnowSteps.Add(MakeSnow(0.85f, 0.95f, 0.65f, 240.0f, 2.8f, 18000.0f, 24000, 1.00f, 0.85f, 23.0f, TEXT("heavy")));
		SnowSteps.Add(MakeSnow(1.00f, 1.00f, 1.00f, 330.0f, 3.2f, 22000.0f, 34000, 1.00f, 1.00f, 30.0f, TEXT("blizzard")));
	}

	if (FogSteps.Num() == 0)
	{
		// Fog is a whole-sky condition rather than a particle effect, so it sweeps density against
		// an increasingly overcast sky and keeps wind low (drifting fog reads as artificial).
		// The Fog state value on its own barely reads from altitude: UDS pushes the fog start distance
		// out as fog thickens (so a ground viewer keeps some visibility), and UE computes volumetric
		// fog over a short range by default. Pulling the start distance in and extending the
		// volumetric range is what turns these into visible fog over a city.
		auto MakeFog = [](float FogAmt, float Cloud, float Wind, float WindDeg, float DensityScale,
		                  float StartDist, float VolDist, float VolExtinction, const TCHAR* Label)
		{
			FUDWWeatherStep S;
			// Authored as 0..1 for readability; UDS grades these channels 0..10.
			S.Fog                     = FogAmt * WeatherConstants::UDSStateScale;
			S.CloudCoverage           = Cloud  * WeatherConstants::UDSStateScale;
			S.WindIntensity           = Wind   * WeatherConstants::UDSStateScale;
			S.WindDirectionDeg        = WindDeg;
			S.FogDensityScale         = DensityScale;
			S.FogStartDistance        = StartDist;
			S.VolumetricFogDistance   = VolDist;
			S.VolumetricFogExtinction = VolExtinction;
			S.SettleSeconds           = 4.0f;
			S.Label                   = Label;
			return S;
		};

		// Wind direction is swept here as it is for rain and snow, so the dataset covers several
		// drift angles per fog density. Intensity stays low - fast-moving fog reads as artificial.
		//                     fog   cloud  wind   deg    density  start     volDist  volExt
		FogSteps.Add(MakeFog(0.20f, 0.50f, 0.02f,   0.0f,  1.5f, 20000.0f,  40000.0f, 0.4f, TEXT("haze")));
		FogSteps.Add(MakeFog(0.40f, 0.65f, 0.05f,  45.0f,  2.5f, 12000.0f,  60000.0f, 0.7f, TEXT("light")));
		FogSteps.Add(MakeFog(0.60f, 0.80f, 0.10f, 135.0f,  4.0f,  6000.0f,  80000.0f, 1.0f, TEXT("moderate")));
		FogSteps.Add(MakeFog(0.80f, 0.90f, 0.15f, 225.0f,  6.0f,  2500.0f, 100000.0f, 1.4f, TEXT("thick")));
		FogSteps.Add(MakeFog(1.00f, 1.00f, 0.20f, 315.0f,  9.0f,     0.0f, 120000.0f, 2.0f, TEXT("pea_soup")));
	}
}

ACharacter* AMyActor_weather::TargetCharacter() {
	return TargetThirdPersonCharacter;
}

void AMyActor_weather::MoveForwardForDistance(float DistanceInCm, float Speed)
{
	if (!TargetThirdPersonCharacter) return;

	StartLocation = TargetThirdPersonCharacter->GetActorLocation();
	FVector ForwardVector = TargetThirdPersonCharacter->GetActorForwardVector();
	TargetLocation = StartLocation + ForwardVector * DistanceInCm;

	MoveSpeed = Speed;
	bMoveCamera = true;

	UE_LOG(LogTemp, Log, TEXT("Started moving %.1f cm forward at %.1f cm/s"), DistanceInCm, Speed);
}

void AMyActor_weather::RotateCCW(FRotator angle, float angularSpeed)
{
	if (!TargetThirdPersonCharacter) return;

	StartRotator = TargetThirdPersonCharacter->GetActorRotation();
	TargetRotator = StartRotator + angle;

	AngularSpeed = angularSpeed;
	bRotateCamera = true;

	UE_LOG(LogTemp, Log, TEXT("Started Rotating Camera by (%.1f, %.1f, %.1f) at %.1f cm/s"), angle.Pitch, angle.Yaw, angle.Roll, angularSpeed);
	UE_LOG(LogTemp, Log, TEXT("Started Rotating Camera to (%.1f, %.1f, %.1f) at %.1f cm/s"), TargetRotator.Pitch, TargetRotator.Yaw, TargetRotator.Roll, angularSpeed);
}

void AMyActor_weather::MoveObj(float DeltaTime)
{
	FVector CurrentLocation = TargetThirdPersonCharacter->GetActorLocation();
	FVector Direction = (TargetLocation - CurrentLocation).GetSafeNormal();

	float Step = MoveSpeed * DeltaTime;
	FVector NewLocation = CurrentLocation + Direction * Step;

	// Stop if we�re close enough or overshot
	if (FVector::DistSquared(NewLocation, StartLocation) >= FVector::DistSquared(TargetLocation, StartLocation))
	{
		NewLocation = TargetLocation;
		bMoveCamera = false;
		TargetDestination = true; // Set flag to indicate we reached the target
		UE_LOG(LogTemp, Log, TEXT("Reached target location."));
	}

	TargetThirdPersonCharacter->SetActorLocation(NewLocation);
}

void AMyActor_weather::RotateObj(float DeltaTime)
{
	//UE_LOG(LogTemp, Log, TEXT("Rotate camera!"));
	FRotator CurrentRotator = TargetThirdPersonCharacter->GetActorRotation();

	float AngularStep = AngularSpeed * DeltaTime;
	FRotator NewRotator = CurrentRotator;

	if (CurrentRotator.Pitch < TargetRotator.Pitch) {
		NewRotator.Pitch = CurrentRotator.Pitch + AngularStep;
	}

	if (CurrentRotator.Roll < TargetRotator.Roll) {
		NewRotator.Roll = CurrentRotator.Roll + AngularStep;
	}

	if (CurrentRotator.Yaw < TargetRotator.Yaw) {
		NewRotator.Yaw = CurrentRotator.Yaw + AngularStep;
	}

	// Stop if we�re close enough or overshot
	if (NewRotator.GetManhattanDistance(StartRotator) >= TargetRotator.GetManhattanDistance(StartRotator))
	{
		NewRotator = TargetRotator;
		bRotateCamera = false;
		TargetAngle = true; // Set flag to indicate we reached the target
		UE_LOG(LogTemp, Log, TEXT("Reached target Rotation."));
	}

	TargetThirdPersonCharacter->SetActorRotation(NewRotator.Quaternion());
}

void AMyActor_weather::SetupCaptureComponent()
{
	if (!TargetThirdPersonCharacter) return;

	Capture = TargetThirdPersonCharacter->FindComponentByClass<USceneCaptureComponent2D>();

	if (!Capture)
	{
		UE_LOG(LogTemp, Error, TEXT("No SceneCaptureComponent2D found on character."));
		return;
	}
	if (!bHaveCaptureAttachTransform && !Capture->IsUsingAbsoluteLocation())
	{
		CaptureAttachTransform      = Capture->GetRelativeTransform();
		bHaveCaptureAttachTransform = true;
	}

	// Fixed exposure on the capture, independent of whatever the viewport is doing.
	//
	// Two reasons, and the second is the important one. First, it removes the colour wash seen when
	// a condition changes: UDS blends its exposure bias by weather state (Exposure Bias Cloudy /
	// Foggy / Day), so an abrupt transition ramps eye adaptation, and against a warm sky that reads
	// as a yellow flash.
	//
	// Second, auto-exposure is actively wrong for this dataset. It normalises brightness, so an
	// overcast frame and a clear frame come out at similar exposure and the difference between them
	// - real, physical, and exactly what a weather model should key on - is erased before the image
	// is written. Pinning exposure keeps clear genuinely brighter than overcast.
	//
	// Set on the capture component only, so the operator's viewport keeps auto-exposure and stays
	// comfortable to fly.
	if (bCaptureAutoExposure)
	{
		// Exactly the game window's exposure: Ultra Dynamic Sky's metering, range and bias, from the
		// level's post process - so the saved image is what is seen on screen. Measured before this: an
		// own metering setup (percentiles, physical camera off) exposed ~2.5 stops darker than the
		// window at the same moment. Only the speed is ours, so it settles within the 2 s wait, and a
		// compensation when one is asked for.
		// The game window renders through the drone's camera, whose own post process (exposure
		// compensation, colour, bloom...) applies there but not to a capture component. Start from it,
		// so the capture is processed exactly as the screen is.
		if (const UCameraComponent* ViewCamera = TargetThirdPersonCharacter->FindComponentByClass<UCameraComponent>())
		{
			Capture->PostProcessSettings    = ViewCamera->PostProcessSettings;
			Capture->PostProcessBlendWeight = ViewCamera->PostProcessBlendWeight;
		}
		FPostProcessSettings& PP = Capture->PostProcessSettings;
		if (CaptureExposureBias != 0.0f)
		{
			PP.bOverride_AutoExposureBias = true;
			PP.AutoExposureBias          += CaptureExposureBias;               // on top of the camera's, + = brighter
		}
		PP.bOverride_AutoExposureSpeedUp        = true;
		PP.AutoExposureSpeedUp                  = AutoExposureSpeed;
		PP.bOverride_AutoExposureSpeedDown      = true;
		PP.AutoExposureSpeedDown                = AutoExposureSpeed;

		// Adaptation needs a history: keep the capture's view state and render it every frame, so by
		// the shutter the exposure has converged on what is in front of the camera.
		Capture->bAlwaysPersistRenderingState = true;

		// Render with the game window's own feature switches. A capture component starts from its own
		// defaults, which differ from the game view's - and the saved image should be what is seen.
		if (GEngine && GEngine->GameViewport)
		{
			Capture->ShowFlags = GEngine->GameViewport->EngineShowFlags;
		}

		UE_LOG(LogTemp, Log, TEXT("Capture exposure: auto, as the game window (compensation %+.1f)%s."),
			CaptureExposureBias, (GEngine && GEngine->GameViewport) ? TEXT(", game window show flags") : TEXT(""));
	}
	else if (bLockCaptureExposure)
	{
		Capture->PostProcessSettings.bOverride_AutoExposureMethod       = true;
		Capture->PostProcessSettings.AutoExposureMethod                 = EAutoExposureMethod::AEM_Manual;

		// Manual mode reads the bias as an exposure value: higher is DARKER (measured: bias 11 gave a
		// sky of 5, bias -1 a sky of 103). Negated so CaptureExposureBias means "+1 = twice as bright"
		// in both modes. With min == max the histogram cannot drift.
		Capture->PostProcessSettings.bOverride_AutoExposureBias         = true;
		Capture->PostProcessSettings.AutoExposureBias                   = -CaptureExposureBias;

		Capture->PostProcessSettings.bOverride_AutoExposureMinBrightness = true;
		Capture->PostProcessSettings.AutoExposureMinBrightness           = 1.0f;
		Capture->PostProcessSettings.bOverride_AutoExposureMaxBrightness = true;
		Capture->PostProcessSettings.AutoExposureMaxBrightness           = 1.0f;

		// Physical camera exposure would reintroduce a second, aperture-driven source of variation.
		Capture->PostProcessSettings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
		Capture->PostProcessSettings.AutoExposureApplyPhysicalCameraExposure           = 0;

		UE_LOG(LogTemp, Log,
			TEXT("Capture exposure locked: manual, bias %.2f. Brightness differences between "
			     "conditions are preserved rather than normalised away."), CaptureExposureBias);
	}

	// Stills from a hovering camera: no motion blur. The game window's show flags (copied above) and
	// the camera's post process both have it on, and it smeared every picture along the last camera
	// move - with smooth flight, along the flight itself.
	Capture->ShowFlags.SetMotionBlur(false);
	Capture->PostProcessSettings.bOverride_MotionBlurAmount = true;
	Capture->PostProcessSettings.MotionBlurAmount           = 0.0f;

	// The engine's lens flare, when the sun's flare is off (Ultra Dynamic Sky's is switched with it) - in
	// the capture and in the drone's own camera, so the window shows what is saved.
	if (!bSunLensFlare)
	{
		Capture->PostProcessSettings.bOverride_LensFlareIntensity = true;
		Capture->PostProcessSettings.LensFlareIntensity           = 0.0f;
		if (UCameraComponent* ViewCamera = TargetThirdPersonCharacter->FindComponentByClass<UCameraComponent>())
		{
			ViewCamera->PostProcessSettings.bOverride_LensFlareIntensity = true;
			ViewCamera->PostProcessSettings.LensFlareIntensity           = 0.0f;
		}
	}

	// The finished image - exposure, tone mapping and display colour applied - not raw scene light.
	//
	// The component was authored with SceneColorHDR into a 16-bit float target. That is linear light
	// before post-processing, so the exposure above never reached the image, and converting it to an
	// 8-bit PNG scaled each frame by its brightest pixel: every picture came out dark, and one with
	// the sun or moon in view (even a sliver at the edge) was scaled to that and went near-black with
	// a green cast. Final colour into an 8-bit target is what a camera records: one fixed exposure.
	Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;

	const int32 W = FMath::Max(16, CaptureResolutionX);
	const int32 H = FMath::Max(16, CaptureResolutionY);

	// Fish-eye: a wider normal view, rendered larger, remapped to W x H on read-back.
	FisheyeRenderFovDeg = CaptureFOV;
	FisheyeRenderScale  = 1.0f;
	if (bFisheyeLens)
	{
		FisheyeRender(CaptureFOV, W, H, FisheyeRenderFovDeg, FisheyeRenderScale);
	}
	const int32 RenderW = FMath::CeilToInt(W * FisheyeRenderScale);
	const int32 RenderH = FMath::CeilToInt(H * FisheyeRenderScale);

	// Always our own target at the configured resolution, whatever the editor assigned: every image in
	// the dataset has identical dimensions. Linear 16-bit: final colour arrives in linear light, and
	// ReadCaptureAsDisplayed applies the display curve on read-back - an 8-bit linear target lost the
	// curve (shadows ~5x too dark), an sRGB one came out ~1 stop too bright; this matches the screen.
	if (!Capture->TextureTarget || Capture->TextureTarget->SizeX != RenderW || Capture->TextureTarget->SizeY != RenderH ||
		Capture->TextureTarget->GetFormat() != PF_FloatRGBA)
	{
		UTextureRenderTarget2D* NewRT = NewObject<UTextureRenderTarget2D>(this);
		NewRT->InitCustomFormat(RenderW, RenderH, PF_FloatRGBA, /*bInForceLinearGamma=*/true);
		NewRT->UpdateResource();
		Capture->TextureTarget = NewRT;
	}

	Capture->FOVAngle = FisheyeRenderFovDeg;
	if (bFisheyeLens)
	{
		UE_LOG(LogTemp, Log, TEXT("[Lens] Fish-eye %.0f deg at %dx%d: rendered as a %.1f deg view at %dx%d, remapped."),
			CaptureFOV, W, H, FisheyeRenderFovDeg, RenderW, RenderH);
	}

	// The component was authored with bCaptureEveryFrame on, so it was rendering a second full view
	// of the scene on every tick of the entire run and throwing all of it away - the pipeline only
	// ever wants the one frame it explicitly asks for. Calling CaptureScene() on top of that is what
	// UE was warning about ("told to update - major inefficiency"), once per capture.
	// Auto exposure is the exception: it adapts from frame to frame, so the capture has to keep
	// rendering for its exposure to follow the camera and settle before the shutter.
	Capture->bCaptureEveryFrame = bCaptureAutoExposure;
	Capture->bCaptureOnMovement = false;

	UE_LOG(LogTemp, Log, TEXT("Capture camera: %dx%d at %.1f deg FOV, final colour into 8-bit (fixed capture %s)."),
		W, H, CaptureFOV, bUseFixedCameraCapture ? TEXT("on") : TEXT("off - using the PIE viewport"));
}

void AMyActor_weather::SetupCameraDebugLog() {

	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	if (PC && PC->PlayerCameraManager)
	{
		FVector CamLoc = PC->PlayerCameraManager->GetCameraLocation();
		FRotator CamRot = PC->PlayerCameraManager->GetCameraRotation();

		UE_LOG(LogTemp, Log, TEXT("Active Camera Location: %s, Rotation: %s"),
			*CamLoc.ToString(), *CamRot.ToString());
	}

}


void AMyActor_weather::StartCapture(float InFrameRate)
{
	if (!TargetThirdPersonCharacter) return;

	if (!Capture || !Capture->TextureTarget) {
		bCapturePending = true;
		bCaptureFrameRate = InFrameRate;
		return;
	}

	if (ResolvedRecordingsDir.IsEmpty()) ResolveOutputDirs();
	ScreenshotPath = ResolvedRecordingsDir;
	ScreenshotPath_camera = ResolvedSingleCamDir;

	IFileManager::Get().MakeDirectory(*ScreenshotPath, true);
	IFileManager::Get().MakeDirectory(*ScreenshotPath_camera, true);

	CaptureInterval = 1.0f / InFrameRate;
	bCaptureActive = true;
	TimeSinceLastCapture = 0.0f;

	UE_LOG(LogTemp, Log, TEXT("Capturing to: %s"), *ScreenshotPath);
}

void AMyActor_weather::StartStreamRTSP(float InFPS, FString ServerURL)
{
	if (!TargetThirdPersonCharacter) return;

	// Apply config-driven streaming settings (ffmpeg path, endpoint) before any socket work.
	Streamer.Configure(FFmpegExePath, StreamTargetIP, StreamVideoPort, StreamDataPort);
	if (!Capture || !Capture->TextureTarget) {
		UE_LOG(LogTemp, Error, TEXT("Capture or TextureTarget is null"));
		if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, TEXT("Capture or TextureTarget is null!"));
		bStreamPending = true;
		streamRate = InFPS;
		return;
	}

	// Start streaming at a targeted frame rate
	StreamInterval = 1 / InFPS;
	streamRate = InFPS;
	FViewport* Viewport = GEngine->GameViewport->Viewport;
	if (!Viewport) return;
	int32 Width = Viewport->GetSizeXY().X;
	int32 Height = Viewport->GetSizeXY().Y;
	FString MyWidth = FString::FromInt(Width);
	FString MyHeight = FString::FromInt(Height);
	FString msg = TEXT("Width and Height are ") + MyWidth + TEXT(" and ") + MyHeight;
	if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Green, msg);
	bStreamActive = Streamer.StartTCPServer(ViewportWidth, ViewportHeight) && Streamer.StartMetadataServer();

	if (bStreamActive) {
		bStreamActive = Streamer.StartStream(Width, Height, InFPS, ServerURL);
	}
	TimeSinceLastImgStream = 0.0f; // Reset timer
	UE_LOG(LogTemp, Log, TEXT("Started stream with image of size %dx%d"), Width, Height);
	FString Msg = FString::Printf(TEXT("Started stream with image of size %dx%d"), Width, Height);
	GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Green, Msg);
	UE_LOG(LogTemp, Log, TEXT("Started stream at frequency of %0.2f fps and saving in %s"), 1 / StreamInterval, *ServerURL);
	if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Green, TEXT("Started Stream "));
	bStreamPending = false;
}


void AMyActor_weather::StreamRTSP()
{
	if (!GEngine || !GEngine->GameViewport) return;

	FViewport* Viewport = GEngine->GameViewport->Viewport;
	if (!Viewport) return;

	TArray<FColor> Bitmap, NewBitmap;
	// This is the heavy operation. Since you prefer this over SceneCapture,
	// expect a frame drop here, but the visual quality will be perfect.
	if (Viewport->ReadPixels(Bitmap))
	{
		
		int32 Width = Viewport->GetSizeXY().X;
		int32 Height = Viewport->GetSizeXY().Y;
		AMyActor_weather::ResizeBitmap(Bitmap, Width, Height, NewBitmap);
		Streamer.SendFrameTCP(NewBitmap);

		// If you still need the file saved to disk occasionally, 
		// you can keep your old PNG saving logic here, but don't do it every frame.
	}
}



void AMyActor_weather::StopCapture()
{
	// Stop capturing images
	bCaptureActive = false;
	TimeSinceLastCapture = 0.0f; // Reset timer
	UE_LOG(LogTemp, Log, TEXT("Stopped capturing images."));
	if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Green, TEXT("Stopped capturing images."));
}

// Reads the current frame. Prefers the PIE game viewport (full post-processed image); when there
// is no game viewport (Simulate In Editor), captures via the character's SceneCaptureComponent2D
// render target instead, so the pipeline still produces images in SIE.
namespace
{
	/**
	 * Forces every pixel opaque.
	 *
	 * Scene-capture render targets and viewport reads routinely come back with alpha 0, because
	 * nothing in the scene wrote coverage into that channel. The old thumbnail-based PNG encoder hid
	 * this by discarding alpha entirely; PNGCompressImageArray writes BGRA straight through, so an
	 * unfixed buffer produces a fully transparent PNG - which most viewers render as solid black.
	 *
	 * The dataset wants opaque RGB, so the alpha channel is not information here, it is noise. Done
	 * once at acquisition rather than at each encode, so every consumer of a captured frame gets the
	 * same guarantee.
	 */
	void ForceOpaque(TArray<FColor>& Bitmap)
	{
		for (FColor& Pixel : Bitmap)
		{
			Pixel.A = 255;
		}
	}
}

void AMyActor_weather::FisheyeRender(float FisheyeFovDeg, int32 Width, int32 Height, float& OutRenderFovDeg, float& OutRenderScale)
{
	// Equidistant fish-eye: radius r (px) = f * angle, f = half width / half FOV. The image corner is
	// the widest angle it shows; the normal render must reach that angle at its own corner, and render
	// at least as many pixels per radian at the centre as the fish-eye has there.
	const double HalfW   = Width * 0.5, HalfH = Height * 0.5;
	const double Corner  = FMath::Sqrt(HalfW * HalfW + HalfH * HalfH);
	const double FFish   = HalfW / FMath::DegreesToRadians(FisheyeFovDeg * 0.5);
	const double AngleC  = FMath::Min(Corner / FFish, FMath::DegreesToRadians(89.0));
	const double TanC    = FMath::Tan(AngleC);
	OutRenderFovDeg = static_cast<float>(FMath::RadiansToDegrees(2.0 * FMath::Atan(HalfW * TanC / Corner)));
	OutRenderScale  = static_cast<float>(FMath::Clamp(FFish * TanC / Corner, 1.0, 3.0));
}

bool AMyActor_weather::ReadCaptureAsDisplayed(TArray<FColor>& OutBitmap) const
{
	// The capture holds the finished image in linear light, 16-bit. Read it as it is - no per-image
	// scaling (the 8-bit ReadPixels path stretched each frame to its brightest pixel) - and apply the
	// sRGB display curve, as the screen does. Measured: this matches the game window.
	FTextureRenderTargetResource* Resource = Capture && Capture->TextureTarget
		? Capture->TextureTarget->GameThread_GetRenderTargetResource() : nullptr;
	TArray<FLinearColor> Linear;
	if (!Resource || !Resource->ReadLinearColorPixels(Linear) || Linear.Num() == 0)
	{
		return false;
	}
	if (!bFisheyeLens)
	{
		OutBitmap.SetNumUninitialized(Linear.Num());
		for (int32 Index = 0; Index < Linear.Num(); ++Index)
		{
			OutBitmap[Index] = Linear[Index].ToFColor(/*bSRGB=*/true);
		}
		return true;
	}

	// Fish-eye: each output pixel looks along the angle its distance from the centre stands for; find
	// where that direction landed in the wider normal render and sample it (bilinear, in linear light).
	const int32 W = FMath::Max(16, CaptureResolutionX), H = FMath::Max(16, CaptureResolutionY);
	const int32 SW = Capture->TextureTarget->SizeX, SH = Capture->TextureTarget->SizeY;
	const double FFish = (W * 0.5) / FMath::DegreesToRadians(CaptureFOV * 0.5);
	const double FRect = (SW * 0.5) / FMath::Tan(FMath::DegreesToRadians(FisheyeRenderFovDeg * 0.5));
	auto Sample = [&](double X, double Y) -> FLinearColor
	{
		X = FMath::Clamp(X, 0.0, SW - 1.001); Y = FMath::Clamp(Y, 0.0, SH - 1.001);
		const int32 X0 = static_cast<int32>(X), Y0 = static_cast<int32>(Y);
		const float TX = static_cast<float>(X - X0), TY = static_cast<float>(Y - Y0);
		const FLinearColor Top = FMath::Lerp(Linear[Y0 * SW + X0], Linear[Y0 * SW + X0 + 1], TX);
		const FLinearColor Bot = FMath::Lerp(Linear[(Y0 + 1) * SW + X0], Linear[(Y0 + 1) * SW + X0 + 1], TX);
		return FMath::Lerp(Top, Bot, TY);
	};
	OutBitmap.SetNumUninitialized(W * H);
	for (int32 V = 0; V < H; ++V)
	{
		for (int32 U = 0; U < W; ++U)
		{
			const double DX = U + 0.5 - W * 0.5, DY = V + 0.5 - H * 0.5;
			const double R = FMath::Sqrt(DX * DX + DY * DY);
			const double Scale = R > 1e-9 ? FRect * FMath::Tan(R / FFish) / R : FRect / FFish;
			OutBitmap[V * W + U] = Sample(SW * 0.5 + DX * Scale - 0.5, SH * 0.5 + DY * Scale - 0.5).ToFColor(true);
		}
	}
	return true;
}

bool AMyActor_weather::AcquireSceneImage(TArray<FColor>& OutBitmap, int32& OutWidth, int32& OutHeight)
{
	OutBitmap.Reset();

	// Path 0: the SceneCapture component, at a fixed resolution and an explicitly set pose.
	//
	// Preferred over the viewport because the viewport is not ours: its size follows the editor
	// window and its direction follows the player camera, so images changed dimensions between
	// sessions and framing moved if anyone looked around during a run.
	if (bUseFixedCameraCapture && Capture && Capture->TextureTarget)
	{
		// Re-assert the pose and lens every time. The component is attached to the pawn, so without
		// this it inherits any rotation the pawn or controller has picked up since the waypoint was
		// set - which is exactly the drift being eliminated.
		if (bHaveActiveCapturePose)
		{
			Capture->SetWorldLocationAndRotation(
				ActiveCapturePose.GetLocation(), ActiveCapturePose.GetRotation());
		}
		Capture->FOVAngle = FisheyeRenderFovDeg;           // CaptureFOV, or the wider view a fish-eye needs

		Capture->CaptureScene();
		FlushRenderingCommands();

		if (ReadCaptureAsDisplayed(OutBitmap))
		{
			ForceOpaque(OutBitmap);
			OutWidth  = FMath::Max(16, CaptureResolutionX); // the image, whatever size was rendered
			OutHeight = FMath::Max(16, CaptureResolutionY);
			return true;
		}
		OutBitmap.Reset();
	}

	// Path 1: PIE game viewport.
	if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		FViewport* Viewport = GEngine->GameViewport->Viewport;
		FlushRenderingCommands();
		if (Viewport->ReadPixels(OutBitmap) && OutBitmap.Num() > 0)
		{
			ForceOpaque(OutBitmap);
			OutWidth  = Viewport->GetSizeXY().X;
			OutHeight = Viewport->GetSizeXY().Y;
			return true;
		}
		OutBitmap.Reset();
	}

	// Path 2: SIE / no game viewport — render the SceneCapture and read its render target.
	if (Capture && Capture->TextureTarget)
	{
		Capture->CaptureScene();
		FlushRenderingCommands();
		if (ReadCaptureAsDisplayed(OutBitmap))
		{
			ForceOpaque(OutBitmap);
			OutWidth  = FMath::Max(16, CaptureResolutionX);
			OutHeight = FMath::Max(16, CaptureResolutionY);
			return true;
		}
		OutBitmap.Reset();
	}

	return false;
}

void AMyActor_weather::CaptureWeatherSnapshot(const FString& WeatherType)
{
	// No gating here. Waiting for the scene and the weather to settle is the *run's* job
	// (CaptureCurrentWaypoint), because deferring inside this function let the run advance to the
	// next condition while a capture was still pending - which silently dropped most of the images
	// and mislabelled the rest. This function now always captures, immediately.
	TArray<FColor> Bitmap;
	int32 Width = 0;
	int32 Height = 0;

	if (!AcquireSceneImage(Bitmap, Width, Height))
	{
		UE_LOG(LogTemp, Error, TEXT("CaptureWeatherSnapshot [%s]: could not read a frame (no game viewport and no SceneCapture target). No image saved."), *WeatherType);
		return;
	}

	if (ResolvedSnapshotDir.IsEmpty()) ResolveOutputDirs();
	const FString WeatherFolderPath = ResolvedSnapshotDir;
	IFileManager::Get().MakeDirectory(*WeatherFolderPath, true);

	const FUDWWeatherStep& Step = ActiveCaptureStep;

	// Read UDW's live state back and compare it to what this step asked for. SettleSeconds has
	// elapsed by now, so a mismatch here means the image about to be written does not show the
	// weather its metadata claims - worth a Warning even though the capture still proceeds.
	VerifyWeatherApplied(Step, TEXT("CaptureWeatherSnapshot"));

	// Buffered when the step was applied — the live sequence index has already advanced by now.
	// Included in the filename so every image of a run has a unique, sortable name even when two
	// steps happen to share a label.

	// <condition>_p<NN>.png - the waypoint number is zero-padded and shared across conditions, so
	// the matched counterpart of any image is found by swapping the condition prefix.
	const FString ImageName = FString::Printf(TEXT("%s_p%02d.png"),
		*WeatherType.ToLower(), CurrentWaypointIndex);
	const FString ImagePath = WeatherFolderPath + ImageName;

	// PNGCompressImageArray rather than the deprecated CompressImageArray, which forwards to
	// ThumbnailCompressImageArray - a thumbnail encoder is the wrong intent for dataset images even
	// though PNG is lossless either way, so the pixels are unchanged by this.
	TArray64<uint8> PNGData;
	FImageUtils::PNGCompressImageArray(Width, Height, Bitmap, PNGData);
	if (PNGData.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("CaptureWeatherSnapshot [%s]: PNG compression produced 0 bytes for %s. Skipping."), *WeatherType, *ImageName);
		return;
	}
	if (!FFileHelper::SaveArrayToFile(PNGData, *ImagePath))
	{
		UE_LOG(LogTemp, Error, TEXT("CaptureWeatherSnapshot [%s]: Failed to write %s - check folder permissions or disk space."), *WeatherType, *ImagePath);
	}

	// Diagnostic: the game window's own view of the same moment, next to the capture, to compare what
	// is seen on screen with what is saved. Written on the next rendered frame.
	if (bSaveViewportComparison)
	{
		if (APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0))
		{
			UE_LOG(LogTemp, Log, TEXT("[CaptureDiag] Game window FOV %.1f, capture FOV %.1f."),
				PC->PlayerCameraManager ? PC->PlayerCameraManager->GetFOVAngle() : -1.0f, CaptureFOV);
		}
		FScreenshotRequest::RequestScreenshot(FPaths::ChangeExtension(ImagePath, TEXT("")) + TEXT("_viewport.png"),
			/*bShowUI=*/false, /*bAddFilenameSuffix=*/false);
	}

	// Record the entire applied step, so the dataset is self-describing and a consumer never has to
	// look at this source file to know what a frame depicts.
	// Camera pose goes in the metadata because it is what makes a pair verifiable: two entries with
	// the same waypoint_index must carry identical camera_location and camera_rotation, or the
	// images are not actually matched and the pairing cannot be trusted.
	// The pose the SceneCapture actually rendered from, which is not necessarily the pawn's - the
	// pawn can be rotated by input while the capture stays locked to the waypoint. Recording the
	// pawn's transform here would quietly claim a framing the image does not have.
	FVector  CamLoc = FVector::ZeroVector;
	FRotator CamRot = FRotator::ZeroRotator;
	if (bHaveActiveCapturePose)
	{
		CamLoc = ActiveCapturePose.GetLocation();
		CamRot = ActiveCapturePose.Rotator();
	}
	else if (TargetThirdPersonCharacter)
	{
		CamLoc = TargetThirdPersonCharacter->GetActorLocation();
		CamRot = TargetThirdPersonCharacter->GetActorRotation();
	}

	// EVERY FIELD BELOW IS READ BACK FROM THE ENGINE AT THE MOMENT OF CAPTURE.
	//
	// Not from config and not from the step that was requested - from the objects that rendered the
	// frame. Anything that cannot be read is written as null. A null says "not measured"; a fallback
	// number would say "measured, and it was this", which a dataset consumer cannot tell from the truth.
	// That is how wind_direction_deg and thunder_lightning once carried the REQUESTED values whenever
	// the live read missed.
	//
	// Not recorded: thunder_lightning. UDW holds it (the heavy rain preset carries 10), but lightning
	// flashes are switched off for capture - a random flash would blow out a frame - so no image ever
	// shows lightning, and the value would describe something that is not there.
	//
	// Not recorded: material_wetness. It is a real UDW channel, but nothing on the Cesium tiles responds
	// to it - a wetness 1.0 frame and a 0.0 frame were pixel-identical - so it describes nothing visible.
	//
	// Removed for not being measurements:
	//   animate_time_of_day   - a config flag, false on every frame of every run
	//   moving_fog_intensity  - this actor's own bookkeeping, 0 on every frame since stock presets
	//   rain_drops_scale, snow_flakes_scale, rain/snow alpha, particle spawn counts, snow_depth,
	//   puddle_coverage, cloud_density, cloud_direction_deg, fog_density_scale,
	//   volumetric_fog_distance, volumetric_fog_extinction - step fields nothing populates.
	//
	// Tools/weather_pipeline.py holds the same field list (METADATA_FIELDS) and a test parses this
	// function, so the two cannot drift apart; validate_metadata() then checks a real run's values.

	auto NumOrNull = [](bool bValid, double Value, int32 Decimals) -> FString
	{
		if (!bValid)
		{
			return FString(TEXT("null"));
		}
		switch (Decimals)
		{
		case 1:  return FString::Printf(TEXT("%.1f"), Value);
		case 2:  return FString::Printf(TEXT("%.2f"), Value);
		case 3:  return FString::Printf(TEXT("%.3f"), Value);
		default: return FString::Printf(TEXT("%.8f"), Value);
		}
	};

	// Live weather: UDW's Local Weather State is what it generated from the preset and is rendering.
	UObject* LocalState = UDWBridge::GetLocalWeatherState(UDWActor.Get());
	auto ReadLive = [LocalState](FName Name, float& Out) -> bool
	{
		Out = 0.0f;
		return LocalState != nullptr && UDWBridge::GetFloat(LocalState, Name, Out);
	};
	float Rain = 0, Snow = 0, Fog = 0, Cloud = 0, Wind = 0, WindDir = 0, SnowCov = 0;
	const bool bRain    = ReadLive(UDWNames::Rain,                 Rain);
	const bool bSnow    = ReadLive(UDWNames::Snow,                 Snow);
	const bool bFog     = ReadLive(UDWNames::Fog,                  Fog);
	const bool bCloud   = ReadLive(UDWNames::CloudCoverage,        Cloud);
	const bool bWind    = ReadLive(UDWNames::WindIntensity,        Wind);
	const bool bWindDir = ReadLive(UDWNames::WindDirection,        WindDir);
	const bool bSnowCov = ReadLive(UDWNames::MaterialSnowCoverage, SnowCov);

	// Particle size as UDW holds it.
	float RainScale = 0.0f, SnowScale = 0.0f;
	const bool bRainScale = UDWActor.IsValid() && UDWBridge::GetFloat(UDWActor.Get(), UDWNames::RainDropsScale, RainScale);
	const bool bSnowScale = UDWActor.IsValid() && UDWBridge::GetFloat(UDWActor.Get(), UDWNames::SnowFlakesScale, SnowScale);

	// The clock as UDS holds it, not as it was configured.
	float TimeOfDayNow = 0.0f;
	const bool bTimeOfDay = UDSActor.IsValid() && UDWBridge::GetFloat(UDSActor.Get(), UDWNames::TimeOfDay, TimeOfDayNow);

	// The lens actually used: the SceneCapture's when it produced the frame, otherwise the player
	// camera's, since the viewport path renders through that and not through CaptureFOV.
	float FovNow = 0.0f;
	bool  bFov   = false;
	if (bUseFixedCameraCapture && Capture && Capture->TextureTarget)
	{
		// Fish-eye: the image's own horizontal field of view - the one the remap was built from - not
		// the wider normal view rendered underneath it.
		FovNow = bFisheyeLens ? CaptureFOV : Capture->FOVAngle;
		bFov   = true;
	}
	else if (APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0))
	{
		FovNow = Cam->GetFOVAngle();
		bFov   = true;
	}

	// Real-world position through the Cesium georeference. Cesium returns height above the WGS84
	// ellipsoid; subtracting the EGM96 undulation gives height above mean sea level - what an
	// altimeter, a map or EXIF GPSAltitude reports.
	FVector      LonLatHeight = FVector::ZeroVector;
	const bool   bGeo         = GetGeographicPosition(CamLoc, LonLatHeight);
	const double GeoLonDeg    = LonLatHeight.X;   // Cesium packs longitude in X,
	const double GeoLatDeg    = LonLatHeight.Y;   // latitude in Y,
	const double Undulation   = bGeo ? GetGeoidSeparationAt(GeoLatDeg, GeoLonDeg) : 0.0;
	const double AltitudeMSL  = LonLatHeight.Z - Undulation;   // and ellipsoid height in Z.

	// Ground under the camera and the clearance over it: from the probe that placed the waypoint, or,
	// for a free (manual) frame, probed under the camera. Null when the probe hit nothing.
	bool   bGround           = false;
	double GroundMSL         = 0.0;
	double HeightAboveGround = 0.0;
	double FreeGroundZ = 0.0;
	if (!bUseFlightPath && bGeo && ProbeGroundZ(CamLoc.X, CamLoc.Y, CamLoc.Z, FreeGroundZ))
	{
		// No waypoint probe for a free frame: measure the ground under the camera directly.
		FVector GroundGeo = FVector::ZeroVector;
		if (GetGeographicPosition(FVector(CamLoc.X, CamLoc.Y, FreeGroundZ), GroundGeo))
		{
			bGround           = true;
			GroundMSL         = GroundGeo.Z - Undulation;
			HeightAboveGround = (CamLoc.Z - FreeGroundZ) / 100.0;
		}
	}
	else if (bUseFlightPath && bFollowTerrain && FlightPath.IsValidIndex(CurrentWaypointIndex))
	{
		const FCaptureWaypoint& Waypoint = FlightPath[CurrentWaypointIndex];
		FVector GroundGeo = FVector::ZeroVector;
		if (Waypoint.bGroundMeasured && bGeo &&
			GetGeographicPosition(FVector(CamLoc.X, CamLoc.Y, Waypoint.GroundZ), GroundGeo))
		{
			bGround           = true;
			GroundMSL         = GroundGeo.Z - Undulation;
			HeightAboveGround = (CamLoc.Z - Waypoint.GroundZ) / 100.0;
		}
	}

	FString E;
	E += TEXT("    {\n");
	E += FString::Printf(TEXT("      \"image_file\": \"%s\",\n"), *ImageName);
	E += FString::Printf(TEXT("      \"condition\": \"%s\",\n"), *WeatherType);
	E += FString::Printf(TEXT("      \"waypoint_index\": %d,\n"), CurrentWaypointIndex);
	// Unreal centimetres. Meaningless outside this project, but it is what proves a pair: two entries
	// with the same waypoint_index must carry identical camera_location and camera_rotation.
	E += FString::Printf(TEXT("      \"camera_location\": [%.3f, %.3f, %.3f],\n"), CamLoc.X, CamLoc.Y, CamLoc.Z);
	E += FString::Printf(TEXT("      \"camera_rotation\": [%.3f, %.3f, %.3f],\n"), CamRot.Pitch, CamRot.Yaw, CamRot.Roll);
	E += FString::Printf(TEXT("      \"camera_fov_deg\": %s,\n"), *NumOrNull(bFov, FovNow, 2));
	// The projection the saved image is in: equidistant fish-eye, or the normal (rectilinear) one.
	const bool bLensKnown = bUseFixedCameraCapture && Capture != nullptr;
	E += FString::Printf(TEXT("      \"lens\": %s,\n"), bLensKnown
		? (bFisheyeLens ? TEXT("\"fisheye\"") : TEXT("\"rectilinear\"")) : *NumOrNull(false, 0.0f, 1));
	// From the pixels that were written, not the configured resolution.
	E += FString::Printf(TEXT("      \"image_width\": %d,\n"), Width);
	E += FString::Printf(TEXT("      \"image_height\": %d,\n"), Height);
	E += FString::Printf(TEXT("      \"latitude_deg\": %s,\n"), *NumOrNull(bGeo, GeoLatDeg, 8));
	E += FString::Printf(TEXT("      \"longitude_deg\": %s,\n"), *NumOrNull(bGeo, GeoLonDeg, 8));
	// Altitudes are metres above mean sea level (EGM96), so altitude_m - ground_altitude_m =
	// height_above_ground_m, which the validator checks.
	E += FString::Printf(TEXT("      \"altitude_m\": %s,\n"), *NumOrNull(bGeo, AltitudeMSL, 2));
	E += FString::Printf(TEXT("      \"ground_altitude_m\": %s,\n"), *NumOrNull(bGround, GroundMSL, 2));
	E += FString::Printf(TEXT("      \"height_above_ground_m\": %s,\n"), *NumOrNull(bGround, HeightAboveGround, 2));
	E += FString::Printf(TEXT("      \"rain\": %s,\n"), *NumOrNull(bRain, Rain, 3));
	E += FString::Printf(TEXT("      \"snow\": %s,\n"), *NumOrNull(bSnow, Snow, 3));
	E += FString::Printf(TEXT("      \"fog\": %s,\n"), *NumOrNull(bFog, Fog, 3));
	// How far one can see horizontally at street level - the reference ground the sky is anchored to -
	// to 5% contrast, from the live height fog (both layers). Unreal's fog (SceneCore.cpp,
	// HeightFogCommon.ush): transmittance 2^-(density/1000 per cm of path), each layer's density
	// halving every 1000/falloff cm above its plane.
	float VisibilityKm = 0.0f;
	const UExponentialHeightFogComponent* FogNow = GetSkyHeightFogComponent();
	if (FogNow && bHaveSpawnGround)
	{
		const double PlaneZ = FogNow->GetComponentLocation().Z;
		auto LayerDensityPerCm = [this, PlaneZ](float Density, float Falloff, float OffsetCm)
		{
			const double AbovePlaneCm = SpawnGroundZ - (PlaneZ + OffsetCm);
			return Density / 1000.0 * FMath::Pow(2.0, -Falloff / 1000.0 * AbovePlaneCm);
		};
		const double DensityPerCm =
			LayerDensityPerCm(FogNow->FogDensity, FogNow->FogHeightFalloff, 0.0f) +
			LayerDensityPerCm(FogNow->SecondFogData.FogDensity, FogNow->SecondFogData.FogHeightFalloff,
			                  FogNow->SecondFogData.FogHeightOffset);
		VisibilityKm = DensityPerCm > 0.0
			? static_cast<float>(VisibilityCalibration * FMath::Log2(20.0) / DensityPerCm / 100000.0) : 0.0f;
	}
	E += FString::Printf(TEXT("      \"visibility_km\": %s,\n"), *NumOrNull(VisibilityKm > 0.0f, VisibilityKm, 2));
	E += FString::Printf(TEXT("      \"cloud_coverage\": %s,\n"), *NumOrNull(bCloud, Cloud, 3));
	E += FString::Printf(TEXT("      \"wind_intensity\": %s,\n"), *NumOrNull(bWind, Wind, 3));
	// Compass direction the wind comes FROM (0 = north), converted from UDW's internal yaw.
	E += FString::Printf(TEXT("      \"wind_from_deg\": %s,\n"), *NumOrNull(bWindDir, WindFromForUdwYaw(WindDir), 1));
	E += FString::Printf(TEXT("      \"material_snow_coverage\": %s,\n"), *NumOrNull(bSnowCov, SnowCov, 3));
	E += FString::Printf(TEXT("      \"rain_drop_scale\": %s,\n"), *NumOrNull(bRainScale, RainScale, 3));
	E += FString::Printf(TEXT("      \"snow_flake_scale\": %s,\n"), *NumOrNull(bSnowScale, SnowScale, 3));
	float SunElevationDeg = 0.0f, SunFromCompassDeg = 0.0f;
	const bool bSun = GetSunDirection(SunElevationDeg, SunFromCompassDeg);
	E += FString::Printf(TEXT("      \"sun_elevation_deg\": %s,\n"), *NumOrNull(bSun, SunElevationDeg, 2));
	E += FString::Printf(TEXT("      \"time_of_day\": %s\n"), *NumOrNull(bTimeOfDay, TimeOfDayNow, 1));
	E += TEXT("    }");
	const FString NewEntry = E;

	const FString MasterFilePath = WeatherFolderPath + TEXT("weather_metadata.json");
	FString FullJsonContent;

	// Start a fresh file for each run rather than appending forever — otherwise a single JSON would
	// accumulate entries from every run ever made, including ones with a different schema.
	if (bMetadataFileStarted && IFileManager::Get().FileExists(*MasterFilePath))
	{
		FFileHelper::LoadFileToString(FullJsonContent, *MasterFilePath);
		FullJsonContent.RemoveFromEnd(TEXT("\n]"));
		FullJsonContent.RemoveFromEnd(TEXT("]"));
		FullJsonContent += TEXT(",\n") + NewEntry + TEXT("\n]");
	}
	else
	{
		FullJsonContent = TEXT("[\n") + NewEntry + TEXT("\n]");
		bMetadataFileStarted = true;
	}

	++CapturedImageCount;

	if (FFileHelper::SaveStringToFile(FullJsonContent, *MasterFilePath))
	{
		UE_LOG(LogTemp, Log, TEXT("CaptureWeatherSnapshot [%s]: saved %s (%d bytes PNG)."), *WeatherType, *ImageName, PNGData.Num());
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("CaptureWeatherSnapshot [%s]: image saved but JSON write failed at %s."), *WeatherType, *MasterFilePath);
	}
}

void AMyActor_weather::TriggerRainSnapshot()
{
	CaptureWeatherSnapshot(TEXT("Rain"));
}

void AMyActor_weather::TriggerSnowSnapshot()
{
	// Simplified: Just trigger the snapshot. 
	// The ActiveCapture buffers already hold the correct data.
	CaptureWeatherSnapshot(TEXT("Snow"));
}

// --- RAIN LOGIC ---
void AMyActor_weather::StartRain()
{
	CurrentRainSequenceIndex = 0;
	rainState = true;

	GetWorldTimerManager().ClearTimer(RainSequenceTimerHandle);
	UpdateRainInterval();
}

void AMyActor_weather::UpdateRainInterval()
{
	if (!RainSteps.IsValidIndex(CurrentRainSequenceIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("UpdateRainInterval: RainSteps is empty - skipping the rain phase."));
		ChainAfterRain();
		return;
	}

	const FUDWWeatherStep& Step = RainSteps[CurrentRainSequenceIndex];
	ActiveCaptureStepIndex = CurrentRainSequenceIndex;
	ApplyWeatherStep(EWeatherKind::Rain, Step);

	// Snapshot fires at SettleSeconds. ALL other timers must fire AFTER it so they don't
	// overwrite SnapshotTimerHandle (shared handle) before the snapshot is taken.
	const float SnapshotDelay = FMath::Max(0.1f, Step.SettleSeconds);
	const float AfterSnapshot = SnapshotDelay + 0.3f; // 0.3s buffer for file I/O

	// Per-step dwell time so the whole rain phase fills RainPhaseDurationSeconds. The snapshot
	// still fires early (at SettleSeconds) once the weather has visibly settled; the loop only
	// advances after StepDuration, leaving rain falling continuously between captures. Clamped to
	// AfterSnapshot so a (mis)configured tiny duration can never advance before the snapshot.
	const int32 TotalSteps   = FMath::Max(1, RainSteps.Num());
	const float StepDuration = FMath::Max(RainPhaseDurationSeconds / TotalSteps, AfterSnapshot);

	GetWorldTimerManager().SetTimer(SnapshotTimerHandle, this, &AMyActor_weather::TriggerRainSnapshot, SnapshotDelay, false);

	if (CurrentRainSequenceIndex == RainSteps.Num() - 1)
	{
		CurrentRainSequenceIndex = 0;
		UE_LOG(LogTemp, Log, TEXT("Rain Sequence Finished. Routing to next enabled weather..."));
		// Chain after the final step's full dwell (so the phase spans RainPhaseDurationSeconds)
		GetWorldTimerManager().SetTimer(RainSequenceTimerHandle, this, &AMyActor_weather::ChainAfterRain,
			FMath::Max(WeatherConstants::WeatherChainDelaySeconds, StepDuration + 0.3f), false);
		return;
	}

	CurrentRainSequenceIndex++;
	// Advance after this step's full dwell — also keeps the snapshot timer from colliding
	GetWorldTimerManager().SetTimer(RainSequenceTimerHandle, this, &AMyActor_weather::UpdateRainInterval, StepDuration, false);
}

void AMyActor_weather::StartSnow()
{
	if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		FIntPoint CurrentSize = GEngine->GameViewport->Viewport->GetSizeXY();
		ViewportWidth = CurrentSize.X;
		ViewportHeight = CurrentSize.Y;
	}
	CurrentSnowSequenceIndex = 0;
	snowState = true;
	GetWorldTimerManager().ClearTimer(SnowSequenceTimerHandle);
	UpdateSnowInterval();
}

void AMyActor_weather::UpdateSnowInterval()
{
	if (!SnowSteps.IsValidIndex(CurrentSnowSequenceIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("UpdateSnowInterval: SnowSteps is empty - skipping the snow phase."));
		ChainAfterSnow();
		return;
	}

	const FUDWWeatherStep& Step = SnowSteps[CurrentSnowSequenceIndex];
	ActiveCaptureStepIndex = CurrentSnowSequenceIndex;
	ApplyWeatherStep(EWeatherKind::Snow, Step);

	// Snow steps use a longer SettleSeconds than rain because DLWE ground accumulation is stateful:
	// MaterialSnowCoverage climbs toward its target over time rather than snapping, so a short delay
	// would capture less snow on the ground than the metadata claims.
	const float SnapshotDelay = FMath::Max(0.1f, Step.SettleSeconds);
	const float AfterSnapshot = SnapshotDelay + 0.3f;

	// Per-step dwell so the whole snow phase fills SnowPhaseDurationSeconds (see UpdateRainInterval).
	const int32 TotalSteps   = FMath::Max(1, SnowSteps.Num());
	const float StepDuration = FMath::Max(SnowPhaseDurationSeconds / TotalSteps, AfterSnapshot);

	GetWorldTimerManager().SetTimer(SnapshotTimerHandle, this, &AMyActor_weather::TriggerSnowSnapshot, SnapshotDelay, false);

	if (CurrentSnowSequenceIndex == SnowSteps.Num() - 1)
	{
		CurrentSnowSequenceIndex = 0;
		UE_LOG(LogTemp, Log, TEXT("Snow Sequence Finished. Routing to next enabled weather..."));
		GetWorldTimerManager().SetTimer(SnowSequenceTimerHandle, this, &AMyActor_weather::ChainAfterSnow,
			FMath::Max(WeatherConstants::WeatherChainDelaySeconds, StepDuration + 0.3f), false);
		return;
	}

	CurrentSnowSequenceIndex++;
	GetWorldTimerManager().SetTimer(SnowSequenceTimerHandle, this, &AMyActor_weather::UpdateSnowInterval, StepDuration, false);
}

void AMyActor_weather::StopRain()
{
	GetWorldTimerManager().ClearTimer(RainSequenceTimerHandle);
	GetWorldTimerManager().ClearTimer(SnapshotTimerHandle);
	ClearUDWWeather();
	rainState = false;
}

void AMyActor_weather::StopSnow()
{
	GetWorldTimerManager().ClearTimer(SnowSequenceTimerHandle);
	GetWorldTimerManager().ClearTimer(SnapshotTimerHandle);
	ClearUDWWeather();
	snowState = false;
}

void AMyActor_weather::SaveActiveCameraImage(bool keep_size)
{
	if (!GEngine || !GEngine->GameViewport) return;

	FViewport* Viewport = GEngine->GameViewport->Viewport;
	if (!Viewport) return;

	if (ScreenshotPath_camera.IsEmpty()) ScreenshotPath_camera = FPaths::ProjectSavedDir() + "/single_cam_rec" +  "/";

	TArray<FColor> Bitmap, NewBitmap;
	if (Viewport->ReadPixels(Bitmap))
	{
		// This path reads the viewport directly rather than going through AcquireSceneImage, so it
		// needs the same opaque-alpha guarantee - otherwise the PNG encodes as fully transparent.
		ForceOpaque(Bitmap);

		int32 Width = Viewport->GetSizeXY().X;
		int32 Height = Viewport->GetSizeXY().Y;

		FString Filename = ScreenshotPath_camera + "Capture_" + FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")) + ".png";

		TArray64<uint8> PNGData;
		int32 FinalW, FinalH;

		if (keep_size) {
			FinalW = Viewport->GetSizeXY().X;
			FinalH = Viewport->GetSizeXY().Y;
			FImageUtils::PNGCompressImageArray(FinalW, FinalH, Bitmap, PNGData);
		}
		else {
			FinalW = ViewportWidth;
			FinalH = ViewportHeight;
			AMyActor_weather::ResizeBitmap(Bitmap, Viewport->GetSizeXY().X, Viewport->GetSizeXY().Y, NewBitmap);
			FImageUtils::PNGCompressImageArray(FinalW, FinalH, NewBitmap, PNGData);
		}

		FFileHelper::SaveArrayToFile(PNGData, *Filename); // Moved outside the else

	}
}


void AMyActor_weather::ResizeBitmap(const TArray<FColor>& Src, int SrcW, int SrcH, TArray<FColor>& Dst)
{
	Dst.SetNumUninitialized(ViewportWidth * ViewportHeight);

	for (int y = 0; y < ViewportHeight; y++)
	{
		float SrcY = (float(y) / float(ViewportHeight)) * SrcH;

		for (int x = 0; x < ViewportWidth; x++)
		{
			float SrcX = (float(x) / float(ViewportWidth)) * SrcW;

			int X0 = FMath::Clamp(int(SrcX), 0, SrcW - 1);
			int Y0 = FMath::Clamp(int(SrcY), 0, SrcH - 1);

			Dst[y * ViewportWidth + x] = Src[Y0 * SrcW + X0];
		}
	}
}

void AMyActor_weather::ShareRate(float DeltaTime)
{
	UE_LOG(LogTemp, Log, TEXT("In Share Metadata"));
	if (TargetThirdPersonCharacter)
	{
		AMyCharacterBase* Char =
			Cast<AMyCharacterBase>(TargetThirdPersonCharacter);

		float FakeMouseX = Streamer.YawRate * DeltaTime;
		float FakeMouseY = Streamer.PitchRate * DeltaTime;
		FVector2D camRot = FVector2D(FakeMouseX, FakeMouseY);
		if (Char)
		{
			Char->scriptRot = true;
			Char->CamRot = camRot;
			Char->ApplyVirtualLook(camRot);
			FString Msg = FString::Printf(TEXT("Recieved camRot: %.2f, %.2f"), FakeMouseX, FakeMouseY);
			if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Green, Msg);
		}
		else{ GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, TEXT("No Char")); }
	}
}


void AMyActor_weather::StartFog()
{
	if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		FIntPoint Size = GEngine->GameViewport->Viewport->GetSizeXY();
		ViewportWidth  = Size.X;
		ViewportHeight = Size.Y;
	}

	CurrentFogSequenceIndex = 0;
	fogState = true;

	GetWorldTimerManager().ClearTimer(FogSequenceTimerHandle);
	UpdateFogInterval();

	UE_LOG(LogTemp, Log, TEXT("Fog Sequence Started."));
}

void AMyActor_weather::StopFog()
{
	GetWorldTimerManager().ClearTimer(FogSequenceTimerHandle);
	GetWorldTimerManager().ClearTimer(SnapshotTimerHandle);

	ClearUDWWeather();

	fogState = false;
	UE_LOG(LogTemp, Log, TEXT("Fog Sequence Stopped."));
}

void AMyActor_weather::TriggerFogSnapshot()
{
	CaptureWeatherSnapshot(TEXT("Fog"));
}

// Fog is a whole-sky condition in UDW: the Fog state value drives volumetric fog, the post-process
// wind fog and the sky's aerial perspective together. So this loop is structurally identical to the
// rain and snow loops, rather than the old bespoke fog-box path it replaces.
void AMyActor_weather::UpdateFogInterval()
{
	if (!FogSteps.IsValidIndex(CurrentFogSequenceIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("UpdateFogInterval: FogSteps is empty - skipping the fog phase and exiting."));
		GetWorldTimerManager().SetTimer(ExitTimerHandle, this, &AMyActor_weather::RequestGameExit,
			WeatherConstants::ExitDelaySeconds, false);
		return;
	}

	const FUDWWeatherStep& Step = FogSteps[CurrentFogSequenceIndex];
	ActiveCaptureStepIndex = CurrentFogSequenceIndex;
	ApplyWeatherStep(EWeatherKind::Fog, Step);

	const float SnapshotDelay = FMath::Max(0.1f, Step.SettleSeconds);
	const float AfterSnapshot = SnapshotDelay + 0.3f;

	// Fog now gets the same phase-duration treatment as rain and snow instead of the old fixed
	// 3s-per-scenario walk, so all three phases are the same length.
	const int32 TotalSteps   = FMath::Max(1, FogSteps.Num());
	const float StepDuration = FMath::Max(FogPhaseDurationSeconds / TotalSteps, AfterSnapshot);

	GetWorldTimerManager().SetTimer(SnapshotTimerHandle, this, &AMyActor_weather::TriggerFogSnapshot, SnapshotDelay, false);

	if (CurrentFogSequenceIndex == FogSteps.Num() - 1)
	{
		UE_LOG(LogTemp, Log, TEXT("All fog steps captured. Exiting."));

		// Both timers must fire AFTER the snapshot so nothing tears down the weather — or the PIE
		// session — before the final image and its JSON entry have been written.
		GetWorldTimerManager().SetTimer(FogSequenceTimerHandle, this, &AMyActor_weather::StopFog,
			StepDuration, false);

		GetWorldTimerManager().SetTimer(ExitTimerHandle, this, &AMyActor_weather::RequestGameExit,
			StepDuration + WeatherConstants::ExitDelaySeconds, false);
		return;
	}

	CurrentFogSequenceIndex++;
	GetWorldTimerManager().SetTimer(FogSequenceTimerHandle, this, &AMyActor_weather::UpdateFogInterval,
		StepDuration, false);
}

// ---------------------------------------------------------------------------
// Game exit
// ---------------------------------------------------------------------------

bool AMyActor_weather::AdvanceScreenSpaceErrorBenchmark()
{
	// Local copy so an empty array still gives a usable default pair without the operator having to
	// populate it by hand in the editor.
	TArray<float> Values = ScreenSpaceErrorBenchmarkValues;
	if (Values.Num() == 0)
	{
		Values = { 8.0f, 16.0f };
	}

	const double Elapsed     = FPlatformTime::Seconds() - CaptureRunStartTime;
	const int32  ValueIndex  = BenchmarkPassIndex / 2;
	const bool   bWasWarmRun = (BenchmarkPassIndex % 2) == 1;

	if (Values.IsValidIndex(ValueIndex))
	{
		UE_LOG(LogTemp, Log,
			TEXT("[SSE bench] pass %d complete: SSE %.1f, %s run, %d image(s) in %.1fs (%.2fs/image)."),
			BenchmarkPassIndex + 1, Values[ValueIndex],
			bWasWarmRun ? TEXT("MEASURED warm") : TEXT("discarded warm-up"),
			CapturedImageCount, Elapsed,
			CapturedImageCount > 0 ? Elapsed / CapturedImageCount : 0.0);

		if (bWasWarmRun)
		{
			BenchmarkWarmSeconds.Add(Elapsed);
			BenchmarkWarmImages.Add(CapturedImageCount);
		}
	}

	++BenchmarkPassIndex;

	const int32 NextValueIndex = BenchmarkPassIndex / 2;
	if (!Values.IsValidIndex(NextValueIndex))
	{
		// Done - report the warm runs side by side, relative to the first, so the trade-off is
		// readable without arithmetic.
		UE_LOG(LogTemp, Log, TEXT("[SSE bench] ===== results (warm runs only) ====="));

		const double Baseline = BenchmarkWarmSeconds.Num() > 0 ? BenchmarkWarmSeconds[0] : 0.0;
		for (int32 i = 0; i < BenchmarkWarmSeconds.Num() && i < Values.Num(); ++i)
		{
			UE_LOG(LogTemp, Log,
				TEXT("[SSE bench]   SSE %5.1f : %6.1fs total, %5.2fs/image, %d images  (%.2fx vs SSE %.1f)"),
				Values[i], BenchmarkWarmSeconds[i],
				BenchmarkWarmImages[i] > 0 ? BenchmarkWarmSeconds[i] / BenchmarkWarmImages[i] : 0.0,
				BenchmarkWarmImages[i],
				Baseline > 0.0 ? BenchmarkWarmSeconds[i] / Baseline : 0.0,
				Values.IsValidIndex(0) ? Values[0] : 0.0);
		}

		UE_LOG(LogTemp, Log,
			TEXT("[SSE bench] Compare image sharpness in the last pass's PNGs. Time alone does not "
			     "decide it - a slower setting is only worth it if the imagery is visibly better."));
		return false;   // let the caller exit
	}

	// Next pass. ConfigureCesiumForCapture pushes the new value onto every tileset; the delay lets
	// that take effect and lets the tilesets begin re-streaming before the run starts timing.
	CesiumMaximumScreenSpaceError = Values[NextValueIndex];
	ConfigureCesiumForCapture();

	UE_LOG(LogTemp, Log,
		TEXT("[SSE bench] starting pass %d: SSE %.1f (%s)."),
		BenchmarkPassIndex + 1, CesiumMaximumScreenSpaceError,
		(BenchmarkPassIndex % 2) == 0 ? TEXT("warm-up, discarded") : TEXT("measured"));

	// Reuse LitmusHandle - no sequence is running at this point, so it is free.
	GetWorldTimerManager().SetTimer(LitmusHandle, this,
		&AMyActor_weather::StartMatchedCaptureRun, 2.0f, false);

	return true;
}

void AMyActor_weather::RequestGameExit()
{
	// The benchmark reuses the normal capture run, so completion is where a pass ends. Intercepted
	// before bPipelineActive is cleared, so the next pass starts from the same state the first did.
	if (bRunScreenSpaceErrorBenchmark && AdvanceScreenSpaceErrorBenchmark())
	{
		return;
	}

	bPipelineActive = false;
	UE_LOG(LogTemp, Log, TEXT("All weather data captured. Requesting game exit."));

#if WITH_EDITOR
	// In PIE, QuitGame()'s console "quit" command does not always take the same teardown path
	// as pressing Stop, which can leave World Partition's RuntimeHashSet streaming data
	// uncleared and crash the *next* PIE session in GenerateStreaming() (check(RuntimeStreamingData.IsEmpty())).
	// RequestEndPlayMap() is the supported way for in-game code to end PIE with the full teardown.
	if (GEditor && GEditor->IsPlayingSessionInEditor())
	{
		GEditor->RequestEndPlayMap();
		return;
	}
#endif

	UKismetSystemLibrary::QuitGame(GetWorld(), nullptr, EQuitPreference::Quit, false);
}

// ---------------------------------------------------------------------------
// Pipeline routing — respects bEnableRain / bEnableSnow / bEnableFog flags
// ---------------------------------------------------------------------------

void AMyActor_weather::StartWeatherPipeline()
{
	// Hold at the starting line until Cesium has finished its initial load. Pressing P moments after
	// PIE begins would otherwise capture the opening conditions against half-streamed geometry, and
	// the per-capture gate cannot recover those frames - by the time it runs, the run has committed
	// to a condition and the tiles that were missing are simply missing from that image.
	if (bWaitForCesiumBeforeRun)
	{
		float LowestProgress = 100.0f;
		if (!AreCesiumTilesetsLoaded(LowestProgress))
		{
			if (PreRunWaitElapsed < CesiumPreRunTimeoutSeconds)
			{
				if (!bLoggedPreRunWait)
				{
					bLoggedPreRunWait = true;
					UE_LOG(LogTemp, Log,
						TEXT("Cesium is still loading (%.1f%%). Holding the capture run until it finishes."),
						LowestProgress);
				}

				PreRunWaitElapsed += CesiumTileCheckIntervalSeconds;
				GetWorldTimerManager().SetTimer(RunTimerHandle, this,
					&AMyActor_weather::StartWeatherPipeline,
					CesiumTileCheckIntervalSeconds, false);
				return;
			}

			// Bounded: a dropped connection never reaches 100%, and Cesium reports that as a
			// ConnectionError rather than as progress, so an unbounded wait would hang for good.
			UE_LOG(LogTemp, Warning,
				TEXT("Cesium still at %.1f%% after %.0fs - starting the run anyway. Early frames may contain unloaded tiles."),
				LowestProgress, PreRunWaitElapsed);
		}
		else if (PreRunWaitElapsed > 0.0f)
		{
			UE_LOG(LogTemp, Log, TEXT("Cesium finished loading after %.1fs. Starting the capture run."),
				PreRunWaitElapsed);
		}

		PreRunWaitElapsed = 0.0f;
		bLoggedPreRunWait = false;
	}

	UE_LOG(LogTemp, Log, TEXT("Pipeline flags: Rain=%d Snow=%d Fog=%d"),
		bEnableRain ? 1 : 0, bEnableSnow ? 1 : 0, bEnableFog ? 1 : 0);

	// (Drone/camera visuals are already hidden at BeginPlay in InitializeCharacterReference.)

	// Each run writes its own metadata file rather than appending to the previous run's, so a
	// dataset folder never mixes entries from different runs or schema versions.
	bMetadataFileStarted = false;

	// Claim the shared timers/state for the rest of the run (released in RequestGameExit).
	bPipelineActive = true;

	// The sweep is a gradual build-up, so let UDW ease between steps. Each capture still waits out
	// SettleSeconds and is verified against the live state, so no frame is taken mid-blend.
	bUseInstantWeatherNow = !bSmoothWithinPhase;

	// Multi-site runs: the current site (its folder, clock and conditions) was made current at Play
	// for the first site, and by AdvanceToNextSite for each later one.

	// Matched capture: every condition flown over every waypoint, so waypoint k under one condition
	// is the same pose as waypoint k under every other. That pairing is the point of the dataset.
	StartMatchedCaptureRun();
}

void AMyActor_weather::ChainAfterRain()
{
	if (bEnableSnow) { UE_LOG(LogTemp, Log, TEXT("Rain done. Chaining to Snow.")); StartSnow(); return; }
	if (bEnableFog)  { UE_LOG(LogTemp, Log, TEXT("Rain done. Snow disabled - chaining to Fog.")); StartFog(); return; }
	UE_LOG(LogTemp, Log, TEXT("Rain done. Snow and Fog disabled - exiting."));
	GetWorldTimerManager().SetTimer(ExitTimerHandle, this, &AMyActor_weather::RequestGameExit,
		WeatherConstants::ExitDelaySeconds, false);
}

void AMyActor_weather::ChainAfterSnow()
{
	if (bEnableFog) { UE_LOG(LogTemp, Log, TEXT("Snow done. Chaining to Fog.")); StartFog(); return; }
	UE_LOG(LogTemp, Log, TEXT("Snow done. Fog disabled - exiting."));
	GetWorldTimerManager().SetTimer(ExitTimerHandle, this, &AMyActor_weather::RequestGameExit,
		WeatherConstants::ExitDelaySeconds, false);
}

// ---------------------------------------------------------------------------
// Litmus test
// ---------------------------------------------------------------------------

// Quick visual sanity check. Shows each weather type at its heaviest configured step for
// LitmusStepDurationSecs without capturing anything, then chains into the real pipeline. Run this
// after any UDS update to confirm the reflection names still resolve before committing to a
// multi-minute capture run.
void AMyActor_weather::RunWeatherLitmusTest()
{
	// The litmusVar Blueprint toggle is polled every Tick. If it is re-asserted while a preview is
	// already running, restarting here would reset LitmusStep to 0 on every re-trigger, so the
	// sequence would never reach its final step — and the capture pipeline it chains into would
	// never start at all.
	if (bLitmusActive)
	{
		UE_LOG(LogTemp, Verbose, TEXT("[Litmus] Already running - ignoring re-trigger."));
		return;
	}

	// The condition tables are built lazily, and until P started the litmus directly the only route
	// to them was StartWeatherPipeline. Without this the preview runs against an empty
	// LitmusConditions and completes instantly, which is exactly what it did. Idempotent - each
	// table is only filled when empty.
	BuildDefaultCaptureRun();

	bLitmusActive = true;
	LitmusStep = 0;
	// Clear the phase flags too, or a litmus started after a cancelled run would resume mid-sequence.
	bLitmusAppliedAStep = false;
	bLitmusClearing = false;
	// The litmus is three hard cuts by definition - never eased, whatever the pipeline prefers.
	bUseInstantWeatherNow = true;
	LitmusTestStep();
}

// Skips the visual-only litmus preview and goes straight into the real capture run —
// StartWeatherPipeline() already respects bEnableRain/bEnableSnow/bEnableFog and exits
// the game once every enabled type has been captured.
void AMyActor_weather::SetPlayerInputLocked(bool bLocked)
{
	APlayerController* Controller = UGameplayStatics::GetPlayerController(this, 0);
	if (!Controller)
	{
		return;
	}
	if (bLocked)
	{
		Controller->SetIgnoreMoveInput(true);
		Controller->SetIgnoreLookInput(true);
	}
	else
	{
		Controller->ResetIgnoreInputFlags();
	}
}

bool AMyActor_weather::IsHeadlessAutoStart()
{
	return FParse::Param(FCommandLine::Get(), TEXT("WeatherAutoStart"));
}

void AMyActor_weather::BeginSceneSetupThenRun()
{
	if (bManualFrame)
	{
		// The path starts at the user's view. The ground is measured under it - not under the spawn
		// point, which the user may have flown far from - for its height and the cloud base.
		SetPlayerInputLocked(true);
		bSceneSetupRequested = true;
		if (TargetThirdPersonCharacter)
		{
			const FVector Drone = TargetThirdPersonCharacter->GetActorLocation();
			bHaveSpawnGround = ProbeGroundZ(Drone.X, Drone.Y, Drone.Z, SpawnGroundZ);
			StartPathFromUserView();
		}
		WaitForSceneSetupThenRun();
		return;
	}

	if (!bSceneSetupRequested)
	{
		// Before any world position is read: an offset tileset makes the georeference and the drawn
		// terrain disagree, and every altitude computed from them inherits the difference.
		bSceneSetupRequested = true;
		ResetTilesetOffsets();
		StartGroundHeightCalculation();
		UE_LOG(LogTemp, Log, TEXT("[Setup] Calculating the ground height and placing the drone before the run."));
	}
	WaitForSceneSetupThenRun();
}

void AMyActor_weather::StartPathFromUserView()
{
	if (!TargetThirdPersonCharacter || !bHaveSpawnGround)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Manual] No ground under your view - the path starts at the spawn point."));
		return;
	}
	// What is on screen: the player camera, or the pawn without one (Simulate In Editor).
	FVector  Location = TargetThirdPersonCharacter->GetActorLocation();
	FRotator View     = TargetThirdPersonCharacter->GetActorRotation();
	if (const APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0))
	{
		Location = Cam->GetCameraLocation();
		View     = Cam->GetCameraRotation();
	}
	StartPathAt(Location, View, static_cast<float>((Location.Z - SpawnGroundZ) / 100.0));
}

void AMyActor_weather::StartPathAt(const FVector& Location, const FRotator& View, float HeightAboveGroundM)
{
	// The user's view is spot 1, taken as it is, and the capture area is centred on it.
	ManualStartSpot           = FSpotDraw();
	ManualStartSpot.HeightCm  = HeightAboveGroundM * 100.0f;
	ManualStartSpot.View      = View;
	ManualStartSpot.TimeOfDay = TimeOfDay;
	bHaveManualStartSpot      = true;

	CharacterStartTransform = FTransform(View, Location);
	bHaveCharacterStart     = true;
	FlightPath.Reset();                                   // drawn around this start as the run begins

	UE_LOG(LogTemp, Log, TEXT("[Manual] The spots start at your view %s: heading %.0f, pitch %.0f, %.1f m above "
		"the ground; the area is centred there."), *Location.ToCompactString(), View.Yaw, View.Pitch, HeightAboveGroundM);
}

void AMyActor_weather::WaitForSceneSetupThenRun()
{
	// ApplySpawnHeight (on Tick) calibrates the origin, places the drone and anchors the sky; it has
	// its own bounded fallback, so this wait always ends.
	const bool bPlaced = (!bSpawnAtHeightAboveGround || bSpawnHeightApplied || !TargetThirdPersonCharacter)
		&& !bGroundCalculationPending;
	if (!bPlaced)
	{
		GetWorldTimerManager().SetTimer(SceneSetupHandle, this,
			&AMyActor_weather::WaitForSceneSetupThenRun, 0.25f, false);
		return;
	}
	StartWeatherPipeline();
}

void AMyActor_weather::StartWeatherCapture()
{
	// P starts whichever mode is configured. Both set a sequence-running flag, so a second press
	// cancels either one through the same StopWeatherCapture path.
	if (bStartWithLitmus)
	{
		bLitmusPreviewOnly = false;
		RunWeatherLitmusTest();
		return;
	}

	BeginSceneSetupThenRun();
}

// The litmus alternates between two phases per weather type: hold the type at maximum for its
// duration, then clear it and stay blank for LitmusClearGapSeconds before the next one starts. The
// blank gap is the point of the exercise — it proves each effect switches off cleanly and stops the
// tail of the rain from still falling through the first frames of the snow.
void AMyActor_weather::LitmusTestStep()
{
	if (bLitmusClearing)
	{
		// The blank gap has elapsed. Move on to the next type.
		bLitmusClearing = false;
		LitmusStep++;
	}
	else if (bLitmusAppliedAStep)
	{
		// A type has been held for its full duration. It has had time to settle, so this is the
		// honest point to confirm it reached UDW - then drop it and let the sky empty out.
		VerifyWeatherApplied(ActiveCaptureStep, TEXT("[Litmus]"));

		// Between conditions, not end of run - so the weather is released but UDS's sky and fog
		// settings are left as the pipeline has them. The only caller of the seven that means this.
		ClearUDWWeather(/*bRestoreSkyDefaults=*/false);

		// A gap of zero means "cut straight to the next condition". It has to bypass the timer
		// entirely rather than pass 0: SetTimer with a non-positive delay *clears* the timer instead
		// of firing immediately, which would strand the sequence here with no error.
		if (LitmusGapSeconds > 0.0f)
		{
			bLitmusClearing = true;

			GetWorldTimerManager().SetTimer(LitmusHandle, this,
				&AMyActor_weather::LitmusTestStep,
				LitmusGapSeconds, false);
			return;
		}

		LitmusStep++;
	}

	// Skip conditions whose weather type is disabled. Matched on the label rather than the index,
	// because LitmusConditions is configurable and index 1 is only "snow" in the default ordering.
	auto IsConditionEnabled = [this](const FWeatherCondition& C) -> bool
	{
		if (C.Label.Contains(TEXT("snow")) || C.Label.Contains(TEXT("blizzard"))) { return bEnableSnow; }
		if (C.Label.Contains(TEXT("fog")))                                        { return bEnableFog;  }
		if (C.Label.Contains(TEXT("rain")) || C.Label.Contains(TEXT("thunder")))  { return bEnableRain; }
		return true;
	};

	while (LitmusConditions.IsValidIndex(LitmusStep) && !IsConditionEnabled(LitmusConditions[LitmusStep]))
	{
		++LitmusStep;
	}

	// Each step is just the heaviest configured entry of that type. Because a step sets every state
	// value (including the ones it leaves at zero), applying one type implicitly clears the others —
	// no separate deactivation is needed the way the old per-system Niagara path required.
	if (LitmusConditions.IsValidIndex(LitmusStep))
	{
		const FWeatherCondition& Condition = LitmusConditions[LitmusStep];

		// Zero transition: the litmus is three hard cuts, and each is preceded by a blank gap. With no
		// transition to recompute them, the look overrides can be applied straight away.
		ApplyConditionPreset(Condition, 0.0f);
		ApplyConditionLookOverrides(Condition);
		UE_LOG(LogTemp, Log, TEXT("[Litmus] %s."), *Condition.Label);

		bLitmusAppliedAStep = true;

		// Guarded against a non-positive configured value for the same reason as the gap above.
		const float HoldSeconds = FMath::Max(LitmusStepSeconds, 0.05f);
		GetWorldTimerManager().SetTimer(LitmusHandle, this,
			&AMyActor_weather::LitmusTestStep, HoldSeconds, false);
		return;
	}

	// Every litmus condition has been shown - chain straight into the capture run, which opens on
	// Overcast and breaks into rain. The weather was already cleared on the way into this final gap,
	// so there is nothing left to drop here.
	bLitmusActive       = false;
	bLitmusAppliedAStep = false;
	bLitmusClearing     = false;

	if (!bLitmusChainsToCapture || bLitmusPreviewOnly)
	{
		bLitmusPreviewOnly = false;
		// Preview only. Stopping here is the point when the litmus is being used to eyeball a tuning
		// change across every condition - chaining into a capture run would spend minutes writing
		// images nobody asked for, and would have to be cancelled by hand every time.
		UE_LOG(LogTemp, Log,
			TEXT("[Litmus] Complete - preview only. Press P again to run it back, or set ")
			TEXT("bLitmusChainsToCapture to continue into the capture run."));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("[Litmus] Complete. Starting capture run..."));

	// Reuse LitmusHandle for the startup delay so no new timer handle is needed.
	GetWorldTimerManager().SetTimer(LitmusHandle, this,
		&AMyActor_weather::BeginSceneSetupThenRun,
		WeatherConstants::WeatherChainDelaySeconds, false);
}
