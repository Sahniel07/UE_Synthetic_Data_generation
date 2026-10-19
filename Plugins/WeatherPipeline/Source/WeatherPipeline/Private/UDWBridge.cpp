#include "UDWBridge.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY(LogUDWBridge);

namespace UDWBridge
{
	const TCHAR* UDWClassPath =
		TEXT("/Game/UltraDynamicSky/Blueprints/Ultra_Dynamic_Weather.Ultra_Dynamic_Weather_C");
	const TCHAR* UDSClassPath =
		TEXT("/Game/UltraDynamicSky/Blueprints/Ultra_Dynamic_Sky.Ultra_Dynamic_Sky_C");

	namespace
	{
		AActor* FindActorOfBlueprintClass(UWorld* World, const TCHAR* ClassPath, const TCHAR* Label)
		{
			if (!World)
			{
				return nullptr;
			}

			UClass* Class = LoadObject<UClass>(nullptr, ClassPath);
			if (!Class)
			{
				UE_LOG(LogUDWBridge, Warning,
					TEXT("%s class not found at '%s'. Is Ultra Dynamic Sky installed under Content/UltraDynamicSky?"),
					Label, ClassPath);
				return nullptr;
			}

			AActor* Found = UGameplayStatics::GetActorOfClass(World, Class);
			if (!Found)
			{
				UE_LOG(LogUDWBridge, Warning,
					TEXT("No %s actor is placed in the level. Weather cannot be driven without it."), Label);
			}
			return Found;
		}
	}

	AActor* FindWeatherActor(UWorld* World)
	{
		return FindActorOfBlueprintClass(World, UDWClassPath, TEXT("Ultra_Dynamic_Weather"));
	}

	AActor* FindSkyActor(UWorld* World)
	{
		return FindActorOfBlueprintClass(World, UDSClassPath, TEXT("Ultra_Dynamic_Sky"));
	}

	UObject* GetLocalWeatherState(AActor* WeatherActor)
	{
		if (!WeatherActor)
		{
			return nullptr;
		}

		// Where UDW keeps the weather it has GENERATED, as opposed to the channels it was asked for.
		// The actor's own Rain/Snow/Fog properties are inputs and read 0 whatever the sky is doing, so
		// reading them made every metadata row zero and made the settle check compare 0 against 0 -
		// which "settled" instantly and proved nothing.
		static const FName LocalWeatherStateName(TEXT("Local Weather State"));

		const FObjectProperty* Prop = CastField<FObjectProperty>(
			WeatherActor->GetClass()->FindPropertyByName(LocalWeatherStateName));

		if (!Prop)
		{
			UE_LOG(LogUDWBridge, Warning,
				TEXT("property 'Local Weather State' not found on %s. Live weather cannot be read, so "
				     "metadata will record what was requested rather than what rendered."),
				*WeatherActor->GetClass()->GetName());
			return nullptr;
		}

		return Prop->GetObjectPropertyValue_InContainer(WeatherActor);
	}

	UObject* GetManualWeatherState(AActor* WeatherActor)
	{
		if (!WeatherActor)
		{
			return nullptr;
		}

		static const FName ManualStateName(TEXT("Manual Weather State"));
		FObjectProperty* Prop = FindFProperty<FObjectProperty>(WeatherActor->GetClass(), ManualStateName);
		if (!Prop)
		{
			UE_LOG(LogUDWBridge, Warning,
				TEXT("property 'Manual Weather State' not found on %s. Manual weather overrides will not work."),
				*WeatherActor->GetClass()->GetName());
			return nullptr;
		}

		UObject* State = Prop->GetObjectPropertyValue_InContainer(WeatherActor);
		if (!State)
		{
			UE_LOG(LogUDWBridge, Warning,
				TEXT("'Manual Weather State' is null on %s - UDW may not have finished BeginPlay yet."),
				*WeatherActor->GetName());
		}
		return State;
	}

	/**
	 * Every write that CHANGES a value is logged as "[Write] Object.'Property': old -> new".
	 *
	 * This is the audit trail of what the pipeline overrides at Play. Writes that leave the value as it
	 * was are silent, so the list is exactly the set of settings that differ from the level, the UDS
	 * defaults or the preset. grep "[Write]" in the run log to see it.
	 */
	static void LogWrite(const UObject* Obj, FName PropertyName, const FString& Old, const FString& New)
	{
		if (Old != New)
		{
			UE_LOG(LogUDWBridge, Log, TEXT("[Write] %s.'%s': %s -> %s"),
				*Obj->GetName(), *PropertyName.ToString(), *Old, *New);
		}
	}

	bool SetFloat(UObject* Obj, FName PropertyName, float Value)
	{
		if (!Obj)
		{
			return false;
		}

		// UE5 Blueprint "float" variables are doubles; a few authored ones are still real floats.
		if (FDoubleProperty* AsDouble = FindFProperty<FDoubleProperty>(Obj->GetClass(), PropertyName))
		{
			LogWrite(Obj, PropertyName,
				FString::Printf(TEXT("%.4f"), AsDouble->GetPropertyValue_InContainer(Obj)),
				FString::Printf(TEXT("%.4f"), static_cast<double>(Value)));
			AsDouble->SetPropertyValue_InContainer(Obj, static_cast<double>(Value));
			return true;
		}
		if (FFloatProperty* AsFloat = FindFProperty<FFloatProperty>(Obj->GetClass(), PropertyName))
		{
			LogWrite(Obj, PropertyName,
				FString::Printf(TEXT("%.4f"), AsFloat->GetPropertyValue_InContainer(Obj)),
				FString::Printf(TEXT("%.4f"), Value));
			AsFloat->SetPropertyValue_InContainer(Obj, Value);
			return true;
		}

		UE_LOG(LogUDWBridge, Warning, TEXT("property '%s' not found (float) on %s."),
			*PropertyName.ToString(), *Obj->GetClass()->GetName());
		return false;
	}

	bool GetFloat(UObject* Obj, FName PropertyName, float& OutValue)
	{
		if (!Obj)
		{
			return false;
		}

		if (FDoubleProperty* AsDouble = FindFProperty<FDoubleProperty>(Obj->GetClass(), PropertyName))
		{
			OutValue = static_cast<float>(AsDouble->GetPropertyValue_InContainer(Obj));
			return true;
		}
		if (FFloatProperty* AsFloat = FindFProperty<FFloatProperty>(Obj->GetClass(), PropertyName))
		{
			OutValue = AsFloat->GetPropertyValue_InContainer(Obj);
			return true;
		}

		// Log, like the setters do. A silent false here leaves OutValue at whatever the caller
		// initialised it to - almost always 0.0f - which reads in the log exactly like a real value of
		// zero. That ambiguity sent one debugging pass chasing the wrong cause; never again.
		UE_LOG(LogUDWBridge, Warning, TEXT("property '%s' not found (float, reading) on %s."),
			*PropertyName.ToString(), *Obj->GetClass()->GetName());
		return false;
	}

	bool SetEnumByDisplayName(UObject* Obj, FName PropertyName, const FString& DisplayName)
	{
		if (!Obj)
		{
			return false;
		}

		// Blueprint enums come through as either FEnumProperty (with an inner numeric property) or a
		// plain FByteProperty carrying a UEnum, depending on how the asset was authored.
		UEnum*     Enum      = nullptr;
		FNumericProperty* Underlying = nullptr;
		void*      ValuePtr  = nullptr;

		if (FEnumProperty* AsEnum = FindFProperty<FEnumProperty>(Obj->GetClass(), PropertyName))
		{
			Enum       = AsEnum->GetEnum();
			Underlying = AsEnum->GetUnderlyingProperty();
			ValuePtr   = AsEnum->ContainerPtrToValuePtr<void>(Obj);
		}
		else if (FByteProperty* AsByte = FindFProperty<FByteProperty>(Obj->GetClass(), PropertyName))
		{
			Enum       = AsByte->Enum;
			Underlying = AsByte;
			ValuePtr   = AsByte->ContainerPtrToValuePtr<void>(Obj);
		}

		if (!Enum || !Underlying || !ValuePtr)
		{
			UE_LOG(LogUDWBridge, Warning, TEXT("property '%s' not found (enum) on %s."),
				*PropertyName.ToString(), *Obj->GetClass()->GetName());
			return false;
		}

		// Match on display name rather than a hard-coded index. The underlying entries are named
		// NewEnumerator0..N, so an index would silently point at the wrong mode if UDS ever reorders
		// them - and the display name is the only part a reader of this code can verify against the
		// UDS docs anyway.
		const int32 NumEntries = Enum->NumEnums();
		for (int32 i = 0; i < NumEntries; ++i)
		{
			if (Enum->GetDisplayNameTextByIndex(i).ToString().Equals(DisplayName, ESearchCase::IgnoreCase))
			{
				const int64 OldValue = Underlying->GetSignedIntPropertyValue(ValuePtr);
				LogWrite(Obj, PropertyName,
					Enum->GetDisplayNameTextByValue(OldValue).ToString(), DisplayName);
				Underlying->SetIntPropertyValue(ValuePtr, static_cast<int64>(Enum->GetValueByIndex(i)));
				return true;
			}
		}

		// Name the options, so a rename is one log line away from being fixed.
		FString Available;
		for (int32 i = 0; i < NumEntries; ++i)
		{
			if (i > 0) { Available += TEXT(", "); }
			Available += Enum->GetDisplayNameTextByIndex(i).ToString();
		}
		UE_LOG(LogUDWBridge, Warning,
			TEXT("enum '%s' on %s has no entry called '%s'. Available: %s"),
			*PropertyName.ToString(), *Obj->GetClass()->GetName(), *DisplayName, *Available);
		return false;
	}

	FName ResolveNoArgFunction(UObject* Obj, const TArray<FName>& Candidates)
	{
		if (!Obj)
		{
			return NAME_None;
		}

		for (const FName& Candidate : Candidates)
		{
			UFunction* Function = Obj->FindFunction(Candidate);
			if (!Function)
			{
				continue;
			}

			// Only no-parameter functions are safe to call blind: anything with a parameter block
			// would need arguments we cannot synthesise correctly from here.
			if (Function->ParmsSize > 0)
			{
				UE_LOG(LogUDWBridge, Verbose,
					TEXT("candidate '%s' exists on %s but takes %d bytes of parameters - skipping."),
					*Candidate.ToString(), *Obj->GetClass()->GetName(), Function->ParmsSize);
				continue;
			}

			return Candidate;
		}

		return NAME_None;
	}

	bool SetBool(UObject* Obj, FName PropertyName, bool bValue)
	{
		if (!Obj)
		{
			return false;
		}

		if (FBoolProperty* Prop = FindFProperty<FBoolProperty>(Obj->GetClass(), PropertyName))
		{
			LogWrite(Obj, PropertyName,
				Prop->GetPropertyValue_InContainer(Obj) ? TEXT("true") : TEXT("false"),
				bValue ? TEXT("true") : TEXT("false"));
			Prop->SetPropertyValue_InContainer(Obj, bValue);
			return true;
		}

		UE_LOG(LogUDWBridge, Warning, TEXT("property '%s' not found (bool) on %s."),
			*PropertyName.ToString(), *Obj->GetClass()->GetName());
		return false;
	}

	bool SetInt(UObject* Obj, FName PropertyName, int32 Value)
	{
		if (!Obj)
		{
			return false;
		}

		if (FIntProperty* Prop = FindFProperty<FIntProperty>(Obj->GetClass(), PropertyName))
		{
			LogWrite(Obj, PropertyName,
				FString::FromInt(Prop->GetPropertyValue_InContainer(Obj)), FString::FromInt(Value));
			Prop->SetPropertyValue_InContainer(Obj, Value);
			return true;
		}
		// Some UDS counts are authored as floats even though they read like integers.
		if (FDoubleProperty* AsDouble = FindFProperty<FDoubleProperty>(Obj->GetClass(), PropertyName))
		{
			LogWrite(Obj, PropertyName,
				FString::Printf(TEXT("%.4f"), AsDouble->GetPropertyValue_InContainer(Obj)),
				FString::Printf(TEXT("%.4f"), static_cast<double>(Value)));
			AsDouble->SetPropertyValue_InContainer(Obj, static_cast<double>(Value));
			return true;
		}
		if (FFloatProperty* AsFloat = FindFProperty<FFloatProperty>(Obj->GetClass(), PropertyName))
		{
			LogWrite(Obj, PropertyName,
				FString::Printf(TEXT("%.4f"), AsFloat->GetPropertyValue_InContainer(Obj)),
				FString::Printf(TEXT("%.4f"), static_cast<float>(Value)));
			AsFloat->SetPropertyValue_InContainer(Obj, static_cast<float>(Value));
			return true;
		}

		UE_LOG(LogUDWBridge, Warning, TEXT("property '%s' not found (int) on %s."),
			*PropertyName.ToString(), *Obj->GetClass()->GetName());
		return false;
	}

	bool CallFunction(UObject* Obj, FName FunctionName, void* Params)
	{
		if (!Obj)
		{
			return false;
		}

		UFunction* Function = Obj->FindFunction(FunctionName);
		if (!Function)
		{
			UE_LOG(LogUDWBridge, Warning, TEXT("function '%s' not found on %s."),
				*FunctionName.ToString(), *Obj->GetClass()->GetName());
			return false;
		}

		// ProcessEvent reads the parameter block straight out of Params. Handing it null for a
		// function that declares parameters would have it read arbitrary memory as arguments, so
		// refuse rather than risk a crash or a garbage call.
		if (Function->ParmsSize > 0 && Params == nullptr)
		{
			UE_LOG(LogUDWBridge, Warning,
				TEXT("function '%s' on %s expects %d bytes of parameters but none were supplied - not calling it."),
				*FunctionName.ToString(), *Obj->GetClass()->GetName(), Function->ParmsSize);
			return false;
		}

		Obj->ProcessEvent(Function, Params);
		return true;
	}

	bool CallVectorToVector(UObject* Obj, FName FunctionName, const FVector& In, FVector& Out)
	{
		if (!Obj)
		{
			return false;
		}

		UFunction* Function = Obj->FindFunction(FunctionName);
		if (!Function)
		{
			UE_LOG(LogUDWBridge, Warning, TEXT("function '%s' not found on %s."),
				*FunctionName.ToString(), *Obj->GetClass()->GetName());
			return false;
		}

		TArray<uint8> ParamBuffer;
		ParamBuffer.SetNumZeroed(FMath::Max<int32>(Function->ParmsSize, 1));
		uint8* Params = ParamBuffer.GetData();

		FStructProperty* ReturnProp = nullptr;
		bool bSetInput = false;

		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			FStructProperty* AsStruct = CastField<FStructProperty>(*It);
			if (!AsStruct || AsStruct->Struct != TBaseStructure<FVector>::Get())
			{
				continue;
			}

			if (It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ReturnProp = AsStruct;
			}
			else if (!bSetInput)
			{
				*AsStruct->ContainerPtrToValuePtr<FVector>(Params) = In;
				bSetInput = true;
			}
		}

		if (!bSetInput || !ReturnProp)
		{
			UE_LOG(LogUDWBridge, Warning,
				TEXT("function '%s' on %s is not of the form FVector Fn(FVector) - not calling it."),
				*FunctionName.ToString(), *Obj->GetClass()->GetName());
			return false;
		}

		Obj->ProcessEvent(Function, Params);
		Out = *ReturnProp->ContainerPtrToValuePtr<FVector>(Params);
		return true;
	}

	bool CallFunctionOneBool(UObject* Obj, FName FunctionName, bool bValue)
	{
		if (!Obj)
		{
			return false;
		}

		UFunction* Function = Obj->FindFunction(FunctionName);
		if (!Function)
		{
			UE_LOG(LogUDWBridge, Warning, TEXT("function '%s' not found on %s."),
				*FunctionName.ToString(), *Obj->GetClass()->GetName());
			return false;
		}

		TArray<uint8> ParamBuffer;
		ParamBuffer.SetNumZeroed(FMath::Max<int32>(Function->ParmsSize, 1));
		uint8* Params = ParamBuffer.GetData();

		bool bAssigned = false;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				continue;
			}
			if (FBoolProperty* AsBool = CastField<FBoolProperty>(*It))
			{
				AsBool->SetPropertyValue_InContainer(Params, bValue);
				bAssigned = true;
			}
			break;
		}

		if (!bAssigned)
		{
			UE_LOG(LogUDWBridge, Warning,
				TEXT("function '%s' on %s has no leading bool parameter - not calling it."),
				*FunctionName.ToString(), *Obj->GetClass()->GetName());
			return false;
		}

		Obj->ProcessEvent(Function, Params);
		return true;
	}

	bool CallFunctionOneFloat(UObject* Obj, FName FunctionName, float Value)
	{
		if (!Obj)
		{
			return false;
		}

		UFunction* Function = Obj->FindFunction(FunctionName);
		if (!Function)
		{
			UE_LOG(LogUDWBridge, Warning, TEXT("function '%s' not found on %s."),
				*FunctionName.ToString(), *Obj->GetClass()->GetName());
			return false;
		}

		// Build the parameter block from the function's own layout rather than assuming a struct,
		// so this works whether the Blueprint author declared the pin as float or double.
		TArray<uint8> ParamBuffer;
		ParamBuffer.SetNumZeroed(FMath::Max<int32>(Function->ParmsSize, 1));
		uint8* Params = ParamBuffer.GetData();

		bool bAssigned = false;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				continue;
			}

			if (FDoubleProperty* AsDouble = CastField<FDoubleProperty>(*It))
			{
				AsDouble->SetPropertyValue_InContainer(Params, static_cast<double>(Value));
				bAssigned = true;
			}
			else if (FFloatProperty* AsFloat = CastField<FFloatProperty>(*It))
			{
				AsFloat->SetPropertyValue_InContainer(Params, Value);
				bAssigned = true;
			}
			break; // only the first input parameter is ours to fill
		}

		if (!bAssigned)
		{
			UE_LOG(LogUDWBridge, Warning,
				TEXT("function '%s' on %s has no leading float parameter - not calling it."),
				*FunctionName.ToString(), *Obj->GetClass()->GetName());
			return false;
		}

		Obj->ProcessEvent(Function, Params);
		return true;
	}

	bool CallChangeWeather(UObject* Obj, UObject* Preset, float TransitionSeconds)
	{
		if (!Obj)
		{
			return false;
		}

		static const FName ChangeWeatherName(TEXT("Change Weather"));
		UFunction* Function = Obj->FindFunction(ChangeWeatherName);
		if (!Function)
		{
			UE_LOG(LogUDWBridge, Warning,
				TEXT("function 'Change Weather' not found on %s - cannot drive weather through UDW's own API."),
				*Obj->GetClass()->GetName());
			return false;
		}

		// Build the parameter block from the function's declared layout rather than assuming the
		// order or the float width. UDS documents the pins as "New Weather Type" (the preset object)
		// and "Time To Transition To New Weather (Seconds)", but assuming that packing and being
		// wrong would hand ProcessEvent a malformed block.
		TArray<uint8> ParamBuffer;
		ParamBuffer.SetNumZeroed(FMath::Max<int32>(Function->ParmsSize, 1));
		uint8* Params = ParamBuffer.GetData();

		bool bSetPreset     = false;
		bool bSetTransition = false;

		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			FProperty* Prop = *It;
			if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				continue;
			}

			if (!bSetPreset)
			{
				if (FObjectPropertyBase* AsObject = CastField<FObjectPropertyBase>(Prop))
				{
					AsObject->SetObjectPropertyValue(Prop->ContainerPtrToValuePtr<void>(Params), Preset);
					bSetPreset = true;
					continue;
				}
			}

			if (!bSetTransition)
			{
				if (FDoubleProperty* AsDouble = CastField<FDoubleProperty>(Prop))
				{
					AsDouble->SetPropertyValue_InContainer(Params, static_cast<double>(TransitionSeconds));
					bSetTransition = true;
					continue;
				}
				if (FFloatProperty* AsFloat = CastField<FFloatProperty>(Prop))
				{
					AsFloat->SetPropertyValue_InContainer(Params, TransitionSeconds);
					bSetTransition = true;
					continue;
				}
			}
		}

		if (!bSetPreset)
		{
			UE_LOG(LogUDWBridge, Warning,
				TEXT("'Change Weather' on %s has no object parameter to receive the preset - not calling it."),
				*Obj->GetClass()->GetName());
			return false;
		}

		if (!bSetTransition)
		{
			UE_LOG(LogUDWBridge, Warning,
				TEXT("'Change Weather' on %s has no numeric transition parameter; calling it with the preset only."),
				*Obj->GetClass()->GetName());
		}

		Obj->ProcessEvent(Function, Params);
		return true;
	}

	FName ResolveFloatProperty(UObject* Obj, const TArray<FName>& Candidates)
	{
		if (!Obj)
		{
			return NAME_None;
		}

		for (const FName& Candidate : Candidates)
		{
			if (FindFProperty<FDoubleProperty>(Obj->GetClass(), Candidate) ||
				FindFProperty<FFloatProperty>(Obj->GetClass(), Candidate))
			{
				return Candidate;
			}
		}
		return NAME_None;
	}

	void DumpMatchingProperties(UObject* Obj, const FString& Substring, const TCHAR* Context)
	{
		if (!Obj)
		{
			return;
		}

		UE_LOG(LogUDWBridge, Log, TEXT("---- properties on %s matching '%s' ----"), Context, *Substring);

		int32 Count = 0;
		for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
		{
			FProperty* Prop = *It;
			const FString Name = Prop->GetName();
			if (!Name.Contains(Substring, ESearchCase::IgnoreCase))
			{
				continue;
			}

			const TCHAR* TypeLabel =
				(CastField<FDoubleProperty>(Prop) || CastField<FFloatProperty>(Prop)) ? TEXT("float") :
				CastField<FBoolProperty>(Prop)   ? TEXT("bool")  :
				CastField<FIntProperty>(Prop)    ? TEXT("int")   :
				CastField<FStructProperty>(Prop) ? TEXT("struct"):
				(CastField<FEnumProperty>(Prop) || CastField<FByteProperty>(Prop)) ? TEXT("enum") : nullptr;

			if (TypeLabel)
			{
				UE_LOG(LogUDWBridge, Log, TEXT("  [%s] %s"), TypeLabel, *Name);
				++Count;
			}
		}

		UE_LOG(LogUDWBridge, Log, TEXT("---- %d match(es) for '%s' on %s ----"), Count, *Substring, Context);
	}

	void DumpTunableProperties(UObject* Obj, const TCHAR* Context)
	{
		if (!Obj)
		{
			UE_LOG(LogUDWBridge, Warning, TEXT("DumpTunableProperties: %s is null."), Context);
			return;
		}

		UE_LOG(LogUDWBridge, Log, TEXT("---- settable properties on %s (%s) ----"),
			Context, *Obj->GetClass()->GetName());

		int32 Count = 0;
		for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
		{
			FProperty* Prop = *It;
			const TCHAR* TypeLabel = nullptr;

			if (CastField<FDoubleProperty>(Prop) || CastField<FFloatProperty>(Prop))
			{
				TypeLabel = TEXT("float");
			}
			else if (CastField<FBoolProperty>(Prop))
			{
				TypeLabel = TEXT("bool");
			}
			else if (CastField<FIntProperty>(Prop))
			{
				TypeLabel = TEXT("int");
			}
			// Structs and enums are not settable through the helpers above, but they still need to
			// appear here: the control-point location (a vector) and its source (an enum) are both
			// written by the pipeline, and without them in the dump there is no way to tell a
			// renamed property from an absent one.
			else if (CastField<FStructProperty>(Prop))
			{
				TypeLabel = TEXT("struct");
			}
			else if (CastField<FEnumProperty>(Prop) || CastField<FByteProperty>(Prop))
			{
				TypeLabel = TEXT("enum");
			}

			if (TypeLabel)
			{
				UE_LOG(LogUDWBridge, Log, TEXT("  [%s] %s"), TypeLabel, *Prop->GetName());
				++Count;
			}
		}

		UE_LOG(LogUDWBridge, Log, TEXT("---- %d tunable properties on %s ----"), Count, Context);
	}

	void DumpCallableFunctions(UObject* Obj, const TCHAR* Context)
	{
		if (!Obj)
		{
			UE_LOG(LogUDWBridge, Warning, TEXT("DumpCallableFunctions: %s is null."), Context);
			return;
		}

		UE_LOG(LogUDWBridge, Log, TEXT("---- callable functions on %s (%s) ----"),
			Context, *Obj->GetClass()->GetName());

		int32 Count = 0;
		for (TFieldIterator<UFunction> It(Obj->GetClass()); It; ++It)
		{
			UFunction* Function = *It;
			UE_LOG(LogUDWBridge, Log, TEXT("  %s (%d param bytes)"), *Function->GetName(), Function->ParmsSize);
			++Count;
		}

		UE_LOG(LogUDWBridge, Log, TEXT("---- %d callable functions on %s ----"), Count, Context);
	}
}
