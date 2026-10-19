#include "CaptureShots.h"

#include "UObject/UObjectIterator.h"

const UCaptureShot* UCaptureShot::Find(const FString& Name)
{
	const FString ClassName = Name + TEXT("CaptureShot");
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->IsChildOf(UCaptureShot::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract) &&
			It->GetName().Equals(ClassName, ESearchCase::IgnoreCase))
		{
			return It->GetDefaultObject<UCaptureShot>();
		}
	}
	return nullptr;
}

float UCaptureShot::Parameter(const TMap<FName, float>& Parameters, FName Name, float Default)
{
	const float* Value = Parameters.Find(Name);
	return Value ? *Value : Default;
}

TArray<FCapturePose> UFrameCaptureShot::BuildPoses(const FCaptureSpot& Spot, const TMap<FName, float>& Parameters) const
{
	return { FCapturePose{ Spot.Location, Spot.View } };
}

TArray<FCapturePose> UOrbitCaptureShot::BuildPoses(const FCaptureSpot& Spot, const TMap<FName, float>& Parameters) const
{
	constexpr float DefaultRadiusM  = 100.0f;
	constexpr float DefaultPictures = 8.0f;
	const double RadiusCm = FMath::Max(1.0f, Parameter(Parameters, TEXT("radius_m"), DefaultRadiusM)) * 100.0;
	const int32  Pictures = FMath::Max(2, FMath::RoundToInt(Parameter(Parameters, TEXT("pictures"), DefaultPictures)));

	FVector Ahead = Spot.View.Vector();
	Ahead.Z = 0.0;
	if (!Ahead.Normalize())
	{
		Ahead = FVector::ForwardVector;   // looking straight down or up: no heading to go by
	}
	const FVector Centre   = Spot.Location + Ahead * RadiusCm;
	const double  StartDeg = FMath::RadiansToDegrees(FMath::Atan2(Spot.Location.Y - Centre.Y, Spot.Location.X - Centre.X));

	TArray<FCapturePose> Poses;
	for (int32 Picture = 0; Picture < Pictures; ++Picture)
	{
		const double AngleDeg = StartDeg + 360.0 / Pictures * Picture;
		const double AngleRad = FMath::DegreesToRadians(AngleDeg);
		const FVector Location = Centre + FVector(RadiusCm * FMath::Cos(AngleRad), RadiusCm * FMath::Sin(AngleRad), 0.0);
		Poses.Add(FCapturePose{ Location, FRotator(Spot.View.Pitch, AngleDeg + 180.0, Spot.View.Roll) });   // faces the centre
	}
	Poses[0] = FCapturePose{ Spot.Location, Spot.View };   // exactly the spot, not a recomputed one
	return Poses;
}
