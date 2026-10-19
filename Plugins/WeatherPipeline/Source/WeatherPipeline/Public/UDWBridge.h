// Reflection bridge to the Ultra Dynamic Sky / Ultra Dynamic Weather Blueprint actors.
//
// UDS and UDW are Blueprint-only marketplace assets, so this module cannot reference their types.
// Everything is reached by FName instead. That is inherently fragile across UDS versions, so every
// helper here logs a Warning naming the property or function it could not resolve — a version bump
// then shows up as a readable log line instead of silently missing weather.
//
// DumpTunableProperties() prints the authoritative, version-correct list of settable names; run it
// once at BeginPlay and reconcile the names used by the pipeline against it.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UWorld;

DECLARE_LOG_CATEGORY_EXTERN(LogUDWBridge, Log, All);

namespace UDWBridge
{
	/** Blueprint class paths for the two marketplace actors. */
	extern WEATHERPIPELINE_API const TCHAR* UDWClassPath;
	extern WEATHERPIPELINE_API const TCHAR* UDSClassPath;

	/** Finds the placed Ultra_Dynamic_Weather actor, or null (logs a Warning) if the level has none. */
	WEATHERPIPELINE_API AActor* FindWeatherActor(UWorld* World);

	/** Finds the placed Ultra_Dynamic_Sky actor, or null (logs a Warning) if the level has none. */
	WEATHERPIPELINE_API AActor* FindSkyActor(UWorld* World);

	/**
	 * Reads UDW's "Manual Weather State" object property — a UDS_Weather_Settings instance holding
	 * the Rain / Snow / Fog / Cloud Coverage / Wind Intensity values that the per-value
	 * "<X> - Manual Override" booleans force onto the local weather.
	 */
	/**
	 * Reads UDW's "Local Weather State" — the weather it has GENERATED, as opposed to the channels it
	 * was asked for.
	 *
	 * The actor's own Rain/Snow/Fog properties are inputs: they read 0 whatever the sky is doing. Any
	 * code that wants to know what is actually rendering — metadata, the settle check — has to come
	 * here instead.
	 */
	WEATHERPIPELINE_API UObject* GetLocalWeatherState(AActor* WeatherActor);

	WEATHERPIPELINE_API UObject* GetManualWeatherState(AActor* WeatherActor);

	// Blueprint floats are FDoubleProperty in UE5, but a few authored ones are still FFloatProperty,
	// so both are handled. All setters return false and log a Warning when the name does not resolve.
	WEATHERPIPELINE_API bool SetFloat(UObject* Obj, FName PropertyName, float Value);
	WEATHERPIPELINE_API bool SetBool(UObject* Obj, FName PropertyName, bool bValue);
	WEATHERPIPELINE_API bool SetInt(UObject* Obj, FName PropertyName, int32 Value);

	/**
	 * Sets a Blueprint enum property by its *display* name (e.g. "Custom Location"), handling both
	 * FEnumProperty and FByteProperty. Logs the available entries when the name does not match.
	 */
	WEATHERPIPELINE_API bool SetEnumByDisplayName(UObject* Obj, FName PropertyName, const FString& DisplayName);

	/** Reads a float back; used to verify a write actually took. Logs a Warning if the name misses. */
	WEATHERPIPELINE_API bool GetFloat(UObject* Obj, FName PropertyName, float& OutValue);

	/**
	 * Returns the first name in Candidates that exists on Obj as a callable UFunction taking no
	 * parameters, or NAME_None. Lets the caller name several plausible spellings of an operation and
	 * bind to whichever one this UDS version actually ships, instead of hard-coding one and silently
	 * doing nothing when it has been renamed.
	 */
	WEATHERPIPELINE_API FName ResolveNoArgFunction(UObject* Obj, const TArray<FName>& Candidates);

	/**
	 * Calls a Blueprint function by name via ProcessEvent.
	 * Params must match the function's parameter struct layout exactly, or be null for no-arg
	 * functions. Returns false (and logs) if the function does not exist.
	 */
	WEATHERPIPELINE_API bool CallFunction(UObject* Obj, FName FunctionName, void* Params = nullptr);

	/**
	 * Calls UDW's own "Change Weather(New Weather Type, Time To Transition)" function.
	 *
	 * This is the API the presets were built for: UDW transitions its whole system - clouds, sun,
	 * particles, material wetness - coherently, instead of us pinning individual state channels and
	 * forcing the update chain. Preset is a UDS_Weather_Settings instance from Weather_Presets.
	 */
	WEATHERPIPELINE_API bool CallChangeWeather(UObject* Obj, UObject* Preset, float TransitionSeconds);

	/**
	 * Calls a function of the shape FVector Fn(const FVector&) and returns its result.
	 *
	 * Written for Cesium's TransformUnrealPositionToLongitudeLatitudeHeight, which is how an Unreal
	 * position becomes a real-world coordinate. The parameter block is built from the function's own
	 * layout rather than a hand-declared struct, so it does not depend on assumptions about padding.
	 */
	WEATHERPIPELINE_API bool CallVectorToVector(UObject* Obj, FName FunctionName, const FVector& In, FVector& Out);

	/** Calls a function taking a single bool parameter (e.g. Cesium's SetGenerateSmoothNormals).
	 *  Going through the setter rather than writing the property matters when the setter has side
	 *  effects - Cesium rebuilds its tiles from it. */
	WEATHERPIPELINE_API bool CallFunctionOneBool(UObject* Obj, FName FunctionName, bool bValue);

	/** Convenience: calls a function taking a single float parameter (e.g. UDS "Set Time of Day"). */
	WEATHERPIPELINE_API bool CallFunctionOneFloat(UObject* Obj, FName FunctionName, float Value);

	/**
	 * Returns the first name in Candidates that exists on Obj as a float/double property, or
	 * NAME_None. Silent - unlike SetFloat it does not warn per miss, so a caller can probe several
	 * plausible spellings without filling the log with expected failures.
	 */
	WEATHERPIPELINE_API FName ResolveFloatProperty(UObject* Obj, const TArray<FName>& Candidates);

	/**
	 * Logs the settable properties whose name contains Substring. A targeted alternative to
	 * DumpTunableProperties, which prints ~1500 lines - when only one name is unknown, this answers
	 * it in a handful of lines instead of burying the rest of the run.
	 */
	WEATHERPIPELINE_API void DumpMatchingProperties(UObject* Obj, const FString& Substring, const TCHAR* Context);

	/** Logs every float/double/bool/int/struct/enum property name on Obj. The list to tune against. */
	WEATHERPIPELINE_API void DumpTunableProperties(UObject* Obj, const TCHAR* Context);

	/**
	 * Logs every callable UFunction name on Obj. Functions do not appear in the property dump, so
	 * without this there is no way to tell a renamed Blueprint function from an absent one — which
	 * is exactly how "Force Update" and "Set Time of Day" were found not to exist in UDS 9.6.
	 */
	WEATHERPIPELINE_API void DumpCallableFunctions(UObject* Obj, const TCHAR* Context);
}
