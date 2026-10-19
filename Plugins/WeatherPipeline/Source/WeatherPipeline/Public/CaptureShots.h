#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CaptureShots.generated.h"

/**
 * Capture shots: what is photographed at each random camera spot.
 *
 * A shot is given the spot - where the camera was drawn and how it looks - and returns the poses to
 * photograph there, in order. Everything else the pipeline does for every shot alike: it places each
 * pose its spot's height above the ground under it, draws the spot again if any pose is in or against
 * a building, sets the spot's time of day, captures every pose in every weather and writes the
 * metadata. A shot is pure geometry.
 *
 * Adding your own takes two steps:
 *   1. A class deriving from UCaptureShot - in this file or any other of the module - overriding
 *      BuildPoses. Its name is its name in the settings: USpiralCaptureShot is "spiral". Return the
 *      spot's own pose first: that view is what the location's limits are checked against.
 *   2. An entry for it in weather_pipeline.py SHOT_TYPES: its parameters and how many pictures it
 *      takes. Then "shot": {"type": "spiral", ...} in a LOCATION entry uses it.
 * Parameters arrive by name, as numbers; read them with Parameter(). The test
 * WeatherPipeline.FlightPath.EveryShotStartsAtItsSpot runs every shot class, yours included.
 */

/** One random camera spot: where it is (world cm; the height is set from the ground later) and its view. */
struct FCaptureSpot
{
	FVector  Location = FVector::ZeroVector;
	FRotator View     = FRotator::ZeroRotator;
};

/** One picture of a shot: where the camera stands and how it looks. */
struct FCapturePose
{
	FVector  Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
};

UCLASS(Abstract)
class WEATHERPIPELINE_API UCaptureShot : public UObject
{
	GENERATED_BODY()

public:
	/** The poses to photograph at Spot, the spot's own first. */
	virtual TArray<FCapturePose> BuildPoses(const FCaptureSpot& Spot, const TMap<FName, float>& Parameters) const
		PURE_VIRTUAL(UCaptureShot::BuildPoses, return TArray<FCapturePose>(););

	/** The shot called Name ("orbit" is UOrbitCaptureShot), or null when there is no such class. */
	static const UCaptureShot* Find(const FString& Name);

protected:
	/** A parameter by name, or Default when the settings do not give it. */
	static float Parameter(const TMap<FName, float>& Parameters, FName Name, float Default);
};

/** "frame": one picture, from the spot. */
UCLASS()
class WEATHERPIPELINE_API UFrameCaptureShot : public UCaptureShot
{
	GENERATED_BODY()

public:
	virtual TArray<FCapturePose> BuildPoses(const FCaptureSpot& Spot, const TMap<FName, float>& Parameters) const override;
};

/**
 * "orbit": `pictures` pictures round a circle. The spot is the first; the circle's centre is
 * `radius_m` straight ahead of the spot's view, level; every picture faces that centre at the spot's
 * pitch and roll.
 */
UCLASS()
class WEATHERPIPELINE_API UOrbitCaptureShot : public UCaptureShot
{
	GENERATED_BODY()

public:
	virtual TArray<FCapturePose> BuildPoses(const FCaptureSpot& Spot, const TMap<FName, float>& Parameters) const override;
};
