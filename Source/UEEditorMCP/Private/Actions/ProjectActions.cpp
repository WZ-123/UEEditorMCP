// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/ProjectActions.h"
#include "MCPCommonUtils.h"
#include "GameFramework/InputSettings.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "InputActionValue.h"
#include "EnhancedActionKeyMapping.h"
#include "PlayerMappableKeySettings.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "EditorAssetLibrary.h"

namespace
{
bool ParseInputActionValueType(const FString& InValueType, EInputActionValueType& OutValueType)
{
	if (InValueType.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase) || InValueType.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
	{
		OutValueType = EInputActionValueType::Boolean;
		return true;
	}
	if (InValueType.Equals(TEXT("Axis1D"), ESearchCase::IgnoreCase) || InValueType.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
	{
		OutValueType = EInputActionValueType::Axis1D;
		return true;
	}
	if (InValueType.Equals(TEXT("Axis2D"), ESearchCase::IgnoreCase) || InValueType.Equals(TEXT("Vector2D"), ESearchCase::IgnoreCase))
	{
		OutValueType = EInputActionValueType::Axis2D;
		return true;
	}
	if (InValueType.Equals(TEXT("Axis3D"), ESearchCase::IgnoreCase) || InValueType.Equals(TEXT("Vector"), ESearchCase::IgnoreCase) || InValueType.Equals(TEXT("Vector3D"), ESearchCase::IgnoreCase))
	{
		OutValueType = EInputActionValueType::Axis3D;
		return true;
	}
	return false;
}

FString InputActionValueTypeToString(const EInputActionValueType ValueType)
{
	switch (ValueType)
	{
	case EInputActionValueType::Boolean:
		return TEXT("Boolean");
	case EInputActionValueType::Axis1D:
		return TEXT("Axis1D");
	case EInputActionValueType::Axis2D:
		return TEXT("Axis2D");
	case EInputActionValueType::Axis3D:
		return TEXT("Axis3D");
	default:
		return TEXT("Unknown");
	}
}

bool ParseInputActionAccumulationBehavior(const FString& InBehavior, EInputActionAccumulationBehavior& OutBehavior)
{
	if (InBehavior.Equals(TEXT("TakeHighestAbsoluteValue"), ESearchCase::IgnoreCase) ||
		InBehavior.Equals(TEXT("HighestAbsoluteValue"), ESearchCase::IgnoreCase) ||
		InBehavior.Equals(TEXT("TakeHighest"), ESearchCase::IgnoreCase) ||
		InBehavior.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
	{
		OutBehavior = EInputActionAccumulationBehavior::TakeHighestAbsoluteValue;
		return true;
	}
	if (InBehavior.Equals(TEXT("Cumulative"), ESearchCase::IgnoreCase) ||
		InBehavior.Equals(TEXT("CumulativeAdd"), ESearchCase::IgnoreCase) ||
		InBehavior.Equals(TEXT("Additive"), ESearchCase::IgnoreCase))
	{
		OutBehavior = EInputActionAccumulationBehavior::Cumulative;
		return true;
	}
	return false;
}

FString InputActionAccumulationBehaviorToString(const EInputActionAccumulationBehavior Behavior)
{
	switch (Behavior)
	{
	case EInputActionAccumulationBehavior::TakeHighestAbsoluteValue:
		return TEXT("TakeHighestAbsoluteValue");
	case EInputActionAccumulationBehavior::Cumulative:
		return TEXT("Cumulative");
	default:
		return TEXT("Unknown");
	}
}

bool ResolveInputActionObjectPath(const TSharedPtr<FJsonObject>& Params, FString& OutObjectPath, FString& OutPackagePath, FString& OutActionName, FString& OutError)
{
	if (Params->HasField(TEXT("asset_path")))
	{
		const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("Parameter 'asset_path' cannot be empty");
			return false;
		}

		if (AssetPath.Contains(TEXT(".")))
		{
			FString Left;
			FString Right;
			if (!AssetPath.Split(TEXT("."), &Left, &Right, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || Left.IsEmpty() || Right.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Invalid asset_path: %s"), *AssetPath);
				return false;
			}

			OutPackagePath = Left;
			OutActionName = Right;
			OutObjectPath = AssetPath;
			return true;
		}

		OutPackagePath = AssetPath;
		OutActionName = FPackageName::GetShortName(AssetPath);
		OutObjectPath = AssetPath + TEXT(".") + OutActionName;
		return true;
	}

	if (!Params->HasField(TEXT("name")))
	{
		OutError = TEXT("Missing 'name' or 'asset_path' parameter");
		return false;
	}

	OutActionName = Params->GetStringField(TEXT("name"));
	FString Path = TEXT("/Game/Input");
	Params->TryGetStringField(TEXT("path"), Path);
	OutPackagePath = Path / OutActionName;
	OutObjectPath = OutPackagePath + TEXT(".") + OutActionName;
	return true;
}

UInputAction* LoadInputActionFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutPackagePath, FString& OutActionName, FString& OutError)
{
	FString ObjectPath;
	if (!ResolveInputActionObjectPath(Params, ObjectPath, OutPackagePath, OutActionName, OutError))
	{
		return nullptr;
	}

	UInputAction* Action = LoadObject<UInputAction>(nullptr, *ObjectPath);
	if (!Action)
	{
		OutError = FString::Printf(TEXT("Input Action not found: %s"), *ObjectPath);
	}
	return Action;
}

bool ResolveInputMappingContextObjectPath(const TSharedPtr<FJsonObject>& Params, FString& OutObjectPath, FString& OutPackagePath, FString& OutContextName, FString& OutError)
{
	if (Params->HasField(TEXT("asset_path")))
	{
		const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("Parameter 'asset_path' cannot be empty");
			return false;
		}

		if (AssetPath.Contains(TEXT(".")))
		{
			FString Left;
			FString Right;
			if (!AssetPath.Split(TEXT("."), &Left, &Right, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || Left.IsEmpty() || Right.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Invalid asset_path: %s"), *AssetPath);
				return false;
			}

			OutPackagePath = Left;
			OutContextName = Right;
			OutObjectPath = AssetPath;
			return true;
		}

		OutPackagePath = AssetPath;
		OutContextName = FPackageName::GetShortName(AssetPath);
		OutObjectPath = AssetPath + TEXT(".") + OutContextName;
		return true;
	}

	if (!Params->HasField(TEXT("name")))
	{
		OutError = TEXT("Missing 'name' or 'asset_path' parameter");
		return false;
	}

	OutContextName = Params->GetStringField(TEXT("name"));
	FString Path = TEXT("/Game/Input");
	Params->TryGetStringField(TEXT("path"), Path);
	OutPackagePath = Path / OutContextName;
	OutObjectPath = OutPackagePath + TEXT(".") + OutContextName;
	return true;
}

UInputMappingContext* LoadInputMappingContextFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutPackagePath, FString& OutContextName, FString& OutError)
{
	FString ObjectPath;
	if (!ResolveInputMappingContextObjectPath(Params, ObjectPath, OutPackagePath, OutContextName, OutError))
	{
		return nullptr;
	}

	UInputMappingContext* MappingContext = LoadObject<UInputMappingContext>(nullptr, *ObjectPath);
	if (!MappingContext)
	{
		OutError = FString::Printf(TEXT("Input Mapping Context not found: %s"), *ObjectPath);
	}
	return MappingContext;
}

void AddInputActionDetailsToResult(const UInputAction* Action, const FString& PackagePath, TSharedPtr<FJsonObject>& ResultObj)
{
	ResultObj->SetStringField(TEXT("name"), Action->GetName());
	ResultObj->SetStringField(TEXT("path"), PackagePath);
	ResultObj->SetStringField(TEXT("asset_path"), PackagePath + TEXT(".") + Action->GetName());
	ResultObj->SetStringField(TEXT("value_type"), InputActionValueTypeToString(Action->ValueType));
	ResultObj->SetStringField(TEXT("accumulation_behavior"), InputActionAccumulationBehaviorToString(Action->AccumulationBehavior));
	ResultObj->SetBoolField(TEXT("consume_input"), Action->bConsumeInput);
	ResultObj->SetBoolField(TEXT("consume_legacy_mappings"), Action->bConsumesActionAndAxisMappings);
	ResultObj->SetBoolField(TEXT("reserve_all_mappings"), Action->bReserveAllMappings);
	ResultObj->SetBoolField(TEXT("trigger_when_paused"), Action->bTriggerWhenPaused);
	ResultObj->SetNumberField(TEXT("trigger_events_that_consume_legacy_keys"), Action->TriggerEventsThatConsumeLegacyKeys);
	ResultObj->SetStringField(TEXT("description"), Action->ActionDescription.ToString());
}

FString InputAxisSwizzleToString(const EInputAxisSwizzle Order)
{
	switch (Order)
	{
	case EInputAxisSwizzle::YXZ:
		return TEXT("YXZ");
	case EInputAxisSwizzle::ZYX:
		return TEXT("ZYX");
	case EInputAxisSwizzle::XZY:
		return TEXT("XZY");
	case EInputAxisSwizzle::YZX:
		return TEXT("YZX");
	case EInputAxisSwizzle::ZXY:
		return TEXT("ZXY");
	default:
		return TEXT("Unknown");
	}
}

FString StripInputTypePrefix(const UObject* Object, const TCHAR* Prefix)
{
	if (!Object)
	{
		return TEXT("");
	}

	FString Name = Object->GetClass()->GetName();
	Name.RemoveFromStart(Prefix);
	return Name;
}

TSharedPtr<FJsonObject> SerializeInputModifier(const UInputModifier* Modifier)
{
	TSharedPtr<FJsonObject> ModifierObj = MakeShared<FJsonObject>();
	if (!Modifier)
	{
		ModifierObj->SetStringField(TEXT("type"), TEXT("Unknown"));
		return ModifierObj;
	}

	ModifierObj->SetStringField(TEXT("class"), Modifier->GetClass()->GetName());
	ModifierObj->SetStringField(TEXT("type"), StripInputTypePrefix(Modifier, TEXT("InputModifier")));

	if (const UInputModifierNegate* Negate = Cast<UInputModifierNegate>(Modifier))
	{
		ModifierObj->SetBoolField(TEXT("x"), Negate->bX);
		ModifierObj->SetBoolField(TEXT("y"), Negate->bY);
		ModifierObj->SetBoolField(TEXT("z"), Negate->bZ);
	}
	else if (const UInputModifierSwizzleAxis* Swizzle = Cast<UInputModifierSwizzleAxis>(Modifier))
	{
		ModifierObj->SetStringField(TEXT("order"), InputAxisSwizzleToString(Swizzle->Order));
	}

	return ModifierObj;
}

TSharedPtr<FJsonObject> SerializeInputTrigger(const UInputTrigger* Trigger)
{
	TSharedPtr<FJsonObject> TriggerObj = MakeShared<FJsonObject>();
	if (!Trigger)
	{
		TriggerObj->SetStringField(TEXT("type"), TEXT("Unknown"));
		return TriggerObj;
	}

	TriggerObj->SetStringField(TEXT("class"), Trigger->GetClass()->GetName());
	TriggerObj->SetStringField(TEXT("type"), StripInputTypePrefix(Trigger, TEXT("InputTrigger")));
	TriggerObj->SetNumberField(TEXT("actuation_threshold"), Trigger->ActuationThreshold);

	if (const UInputTriggerHold* Hold = Cast<UInputTriggerHold>(Trigger))
	{
		TriggerObj->SetNumberField(TEXT("hold_time_threshold"), Hold->HoldTimeThreshold);
		TriggerObj->SetBoolField(TEXT("is_one_shot"), Hold->bIsOneShot);
	}
	else if (const UInputTriggerHoldAndRelease* HoldAndRelease = Cast<UInputTriggerHoldAndRelease>(Trigger))
	{
		TriggerObj->SetNumberField(TEXT("hold_time_threshold"), HoldAndRelease->HoldTimeThreshold);
	}
	else if (const UInputTriggerTap* Tap = Cast<UInputTriggerTap>(Trigger))
	{
		TriggerObj->SetNumberField(TEXT("tap_release_time_threshold"), Tap->TapReleaseTimeThreshold);
	}

	return TriggerObj;
}

TSharedPtr<FJsonObject> SerializeInputMapping(const FEnhancedActionKeyMapping& Mapping, const int32 MappingIndex)
{
	TSharedPtr<FJsonObject> MappingObj = MakeShared<FJsonObject>();
	MappingObj->SetNumberField(TEXT("index"), MappingIndex);
	MappingObj->SetStringField(TEXT("key"), Mapping.Key.ToString());

	if (Mapping.Action)
	{
		MappingObj->SetStringField(TEXT("action_name"), Mapping.Action->GetName());
		const FString ActionPackagePath = Mapping.Action->GetOutermost()->GetName();
		MappingObj->SetStringField(TEXT("action_path"), ActionPackagePath);
		MappingObj->SetStringField(TEXT("action_asset_path"), ActionPackagePath + TEXT(".") + Mapping.Action->GetName());
	}
	else
	{
		MappingObj->SetStringField(TEXT("action_name"), TEXT(""));
		MappingObj->SetStringField(TEXT("action_path"), TEXT(""));
		MappingObj->SetStringField(TEXT("action_asset_path"), TEXT(""));
	}

	MappingObj->SetBoolField(TEXT("is_player_mappable"), Mapping.IsPlayerMappable());

	const UPlayerMappableKeySettings* Settings = Mapping.GetPlayerMappableKeySettings();
	if (Settings)
	{
		MappingObj->SetStringField(TEXT("setting_behavior"), TEXT("OverrideSettings"));
		MappingObj->SetStringField(TEXT("player_mappable_name"), Settings->GetMappingName().ToString());
		MappingObj->SetStringField(TEXT("player_mappable_display_name"), Settings->DisplayName.ToString());
		MappingObj->SetStringField(TEXT("player_mappable_display_category"), Settings->DisplayCategory.ToString());
	}
#if WITH_EDITORONLY_DATA
	else
	{
		MappingObj->SetStringField(TEXT("setting_behavior"), Mapping.IsPlayerMappable() ? TEXT("InheritedOrDeprecatedMappable") : TEXT("InheritOrIgnore"));
PRAGMA_DISABLE_DEPRECATION_WARNINGS
		MappingObj->SetStringField(TEXT("player_mappable_name"), Mapping.PlayerMappableOptions.Name.ToString());
		MappingObj->SetStringField(TEXT("player_mappable_display_name"), Mapping.PlayerMappableOptions.DisplayName.ToString());
		MappingObj->SetStringField(TEXT("player_mappable_display_category"), Mapping.PlayerMappableOptions.DisplayCategory.ToString());
PRAGMA_ENABLE_DEPRECATION_WARNINGS
	}
#else
	else
	{
		MappingObj->SetStringField(TEXT("setting_behavior"), Mapping.IsPlayerMappable() ? TEXT("InheritedMappable") : TEXT("Unknown"));
		MappingObj->SetStringField(TEXT("player_mappable_name"), TEXT(""));
		MappingObj->SetStringField(TEXT("player_mappable_display_name"), TEXT(""));
		MappingObj->SetStringField(TEXT("player_mappable_display_category"), TEXT(""));
	}
#endif

	TArray<TSharedPtr<FJsonValue>> ModifierValues;
	for (const UInputModifier* Modifier : Mapping.Modifiers)
	{
		ModifierValues.Add(MakeShared<FJsonValueObject>(SerializeInputModifier(Modifier)));
	}
	MappingObj->SetArrayField(TEXT("modifiers"), ModifierValues);

	TArray<TSharedPtr<FJsonValue>> TriggerValues;
	for (const UInputTrigger* Trigger : Mapping.Triggers)
	{
		TriggerValues.Add(MakeShared<FJsonValueObject>(SerializeInputTrigger(Trigger)));
	}
	MappingObj->SetArrayField(TEXT("triggers"), TriggerValues);

	return MappingObj;
}

void AddInputMappingContextDetailsToResult(const UInputMappingContext* MappingContext, const FString& PackagePath, TSharedPtr<FJsonObject>& ResultObj)
{
	ResultObj->SetStringField(TEXT("name"), MappingContext->GetName());
	ResultObj->SetStringField(TEXT("path"), PackagePath);
	ResultObj->SetStringField(TEXT("asset_path"), PackagePath + TEXT(".") + MappingContext->GetName());

	TArray<TSharedPtr<FJsonValue>> MappingValues;
	const TArray<FEnhancedActionKeyMapping>& Mappings = MappingContext->GetMappings();
	for (int32 MappingIndex = 0; MappingIndex < Mappings.Num(); ++MappingIndex)
	{
		MappingValues.Add(MakeShared<FJsonValueObject>(SerializeInputMapping(Mappings[MappingIndex], MappingIndex)));
	}

	ResultObj->SetNumberField(TEXT("mapping_count"), Mappings.Num());
	ResultObj->SetArrayField(TEXT("mappings"), MappingValues);
}

void NotifyInputActionPropertyChanged(UInputAction* Action, const FName PropertyName)
{
#if WITH_EDITOR
	if (!Action)
	{
		return;
	}

	if (FProperty* Property = FindFProperty<FProperty>(UInputAction::StaticClass(), PropertyName))
	{
		FPropertyChangedEvent PropertyChangedEvent(Property);
		Action->PostEditChangeProperty(PropertyChangedEvent);
	}
#endif
}
}

// =============================================================================
// FCreateInputMappingAction - Legacy input mapping
// =============================================================================

bool FCreateInputMappingAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("action_name")))
	{
		OutError = TEXT("Missing 'action_name' parameter");
		return false;
	}
	if (!Params->HasField(TEXT("key")))
	{
		OutError = TEXT("Missing 'key' parameter");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FCreateInputMappingAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString ActionName = Params->GetStringField(TEXT("action_name"));
	FString Key = Params->GetStringField(TEXT("key"));

	FString InputType = TEXT("Action");
	Params->TryGetStringField(TEXT("input_type"), InputType);

	UInputSettings* InputSettings = GetMutableDefault<UInputSettings>();
	if (!InputSettings)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get input settings"));
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();

	if (InputType == TEXT("Axis"))
	{
		FInputAxisKeyMapping AxisMapping;
		AxisMapping.AxisName = FName(*ActionName);
		AxisMapping.Key = FKey(*Key);
		AxisMapping.Scale = 1.0f;
		if (Params->HasField(TEXT("scale")))
		{
			AxisMapping.Scale = Params->GetNumberField(TEXT("scale"));
		}

		InputSettings->AddAxisMapping(AxisMapping);
		InputSettings->SaveConfig();
		InputSettings->ForceRebuildKeymaps();

		ResultObj->SetBoolField(TEXT("success"), true);
		ResultObj->SetStringField(TEXT("action_name"), ActionName);
		ResultObj->SetStringField(TEXT("key"), Key);
		ResultObj->SetStringField(TEXT("input_type"), TEXT("Axis"));
		ResultObj->SetNumberField(TEXT("scale"), AxisMapping.Scale);
	}
	else
	{
		FInputActionKeyMapping ActionMapping;
		ActionMapping.ActionName = FName(*ActionName);
		ActionMapping.Key = FKey(*Key);

		if (Params->HasField(TEXT("shift")))
		{
			ActionMapping.bShift = Params->GetBoolField(TEXT("shift"));
		}
		if (Params->HasField(TEXT("ctrl")))
		{
			ActionMapping.bCtrl = Params->GetBoolField(TEXT("ctrl"));
		}
		if (Params->HasField(TEXT("alt")))
		{
			ActionMapping.bAlt = Params->GetBoolField(TEXT("alt"));
		}
		if (Params->HasField(TEXT("cmd")))
		{
			ActionMapping.bCmd = Params->GetBoolField(TEXT("cmd"));
		}

		InputSettings->AddActionMapping(ActionMapping);
		InputSettings->SaveConfig();
		InputSettings->ForceRebuildKeymaps();

		ResultObj->SetBoolField(TEXT("success"), true);
		ResultObj->SetStringField(TEXT("action_name"), ActionName);
		ResultObj->SetStringField(TEXT("key"), Key);
		ResultObj->SetStringField(TEXT("input_type"), TEXT("Action"));
	}

	return ResultObj;
}

// =============================================================================
// FCreateInputActionAction - Enhanced Input Action asset
// =============================================================================

bool FCreateInputActionAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("name")))
	{
		OutError = TEXT("Missing 'name' parameter");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FCreateInputActionAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString Name = Params->GetStringField(TEXT("name"));

	FString ValueTypeStr = TEXT("Boolean");
	Params->TryGetStringField(TEXT("value_type"), ValueTypeStr);

	EInputActionValueType ValueType = EInputActionValueType::Boolean;
	if (!ParseInputActionValueType(ValueTypeStr, ValueType))
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid value_type: %s"), *ValueTypeStr));
	}

	// Create package path
	FString Path = TEXT("/Game/Input");
	Params->TryGetStringField(TEXT("path"), Path);
	FString PackagePath = Path / Name;

	// Check if asset already exists and clean up safely
	UPackage* ExistingPackage = FindPackage(nullptr, *PackagePath);
	if (ExistingPackage)
	{
		UInputAction* ExistingAction = FindObject<UInputAction>(ExistingPackage, *Name);
		if (ExistingAction)
		{
			UE_LOG(LogMCP, Log, TEXT("Input Action '%s' already exists, cleaning up for recreation"), *Name);
			FString TempName = FString::Printf(TEXT("%s_TEMP_%d"), *Name, FMath::Rand());
			ExistingAction->Rename(*TempName, GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional);
			ExistingAction->MarkAsGarbage();
			ExistingPackage->MarkAsGarbage();
		}
	}

	// Delete from disk if exists
	if (UEditorAssetLibrary::DoesAssetExist(PackagePath))
	{
		UE_LOG(LogMCP, Log, TEXT("Input Action '%s' exists on disk, deleting"), *Name);
		UEditorAssetLibrary::DeleteAsset(PackagePath);
	}

	// Create the package
	UPackage* Package = CreatePackage(*PackagePath);
	Package->FullyLoad();

	// Create the Input Action
	UInputAction* NewAction = NewObject<UInputAction>(Package, *Name, RF_Public | RF_Standalone);
	if (!NewAction)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Input Action"));
	}

	NewAction->ValueType = ValueType;

	// Register with asset registry and save
	FAssetRegistryModule::AssetCreated(NewAction);
	NewAction->MarkPackageDirty();

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	FString PackageFilename = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
	UPackage::SavePackage(Package, NewAction, *PackageFilename, SaveArgs);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("name"), Name);
	ResultObj->SetStringField(TEXT("path"), PackagePath);
	ResultObj->SetStringField(TEXT("value_type"), ValueTypeStr);
	return ResultObj;
}

// =============================================================================
// FGetInputActionAction - Read Input Action asset properties
// =============================================================================

bool FGetInputActionAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("name")) && !Params->HasField(TEXT("asset_path")))
	{
		OutError = TEXT("Missing 'name' or 'asset_path' parameter");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FGetInputActionAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString PackagePath;
	FString ActionName;
	FString Error;
	UInputAction* Action = LoadInputActionFromParams(Params, PackagePath, ActionName, Error);
	if (!Action)
	{
		return FMCPCommonUtils::CreateErrorResponse(Error);
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	AddInputActionDetailsToResult(Action, PackagePath, ResultObj);
	return ResultObj;
}

// =============================================================================
// FUpdateInputActionAction - Update Input Action asset properties
// =============================================================================

bool FUpdateInputActionAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("name")) && !Params->HasField(TEXT("asset_path")))
	{
		OutError = TEXT("Missing 'name' or 'asset_path' parameter");
		return false;
	}

	const bool bHasAnyUpdateField =
		Params->HasField(TEXT("value_type")) ||
		Params->HasField(TEXT("accumulation_behavior")) ||
		Params->HasField(TEXT("consume_input")) ||
		Params->HasField(TEXT("consume_legacy_mappings")) ||
		Params->HasField(TEXT("reserve_all_mappings")) ||
		Params->HasField(TEXT("trigger_when_paused")) ||
		Params->HasField(TEXT("trigger_events_that_consume_legacy_keys")) ||
		Params->HasField(TEXT("description"));

	if (!bHasAnyUpdateField)
	{
		OutError = TEXT("No updatable fields provided");
		return false;
	}

	return true;
}

TSharedPtr<FJsonObject> FUpdateInputActionAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString PackagePath;
	FString ActionName;
	FString Error;
	UInputAction* Action = LoadInputActionFromParams(Params, PackagePath, ActionName, Error);
	if (!Action)
	{
		return FMCPCommonUtils::CreateErrorResponse(Error);
	}

	Action->Modify();

	bool bChanged = false;
	TArray<FString> UpdatedFields;

	if (Params->HasField(TEXT("value_type")))
	{
		const FString ValueTypeStr = Params->GetStringField(TEXT("value_type"));
		EInputActionValueType NewValueType = EInputActionValueType::Boolean;
		if (!ParseInputActionValueType(ValueTypeStr, NewValueType))
		{
			return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid value_type: %s"), *ValueTypeStr));
		}

		if (Action->ValueType != NewValueType)
		{
			Action->ValueType = NewValueType;
			NotifyInputActionPropertyChanged(Action, GET_MEMBER_NAME_CHECKED(UInputAction, ValueType));
			bChanged = true;
			UpdatedFields.Add(TEXT("value_type"));
		}
	}

	if (Params->HasField(TEXT("accumulation_behavior")))
	{
		const FString BehaviorStr = Params->GetStringField(TEXT("accumulation_behavior"));
		EInputActionAccumulationBehavior NewBehavior = EInputActionAccumulationBehavior::TakeHighestAbsoluteValue;
		if (!ParseInputActionAccumulationBehavior(BehaviorStr, NewBehavior))
		{
			return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid accumulation_behavior: %s"), *BehaviorStr));
		}

		if (Action->AccumulationBehavior != NewBehavior)
		{
			Action->AccumulationBehavior = NewBehavior;
			NotifyInputActionPropertyChanged(Action, GET_MEMBER_NAME_CHECKED(UInputAction, AccumulationBehavior));
			bChanged = true;
			UpdatedFields.Add(TEXT("accumulation_behavior"));
		}
	}

	if (Params->HasField(TEXT("consume_input")))
	{
		const bool bConsumeInput = Params->GetBoolField(TEXT("consume_input"));
		if (Action->bConsumeInput != bConsumeInput)
		{
			Action->bConsumeInput = bConsumeInput;
			NotifyInputActionPropertyChanged(Action, GET_MEMBER_NAME_CHECKED(UInputAction, bConsumeInput));
			bChanged = true;
			UpdatedFields.Add(TEXT("consume_input"));
		}
	}

	if (Params->HasField(TEXT("consume_legacy_mappings")))
	{
		const bool bConsumeLegacyMappings = Params->GetBoolField(TEXT("consume_legacy_mappings"));
		if (Action->bConsumesActionAndAxisMappings != bConsumeLegacyMappings)
		{
			Action->bConsumesActionAndAxisMappings = bConsumeLegacyMappings;
			NotifyInputActionPropertyChanged(Action, GET_MEMBER_NAME_CHECKED(UInputAction, bConsumesActionAndAxisMappings));
			bChanged = true;
			UpdatedFields.Add(TEXT("consume_legacy_mappings"));
		}
	}

	if (Params->HasField(TEXT("reserve_all_mappings")))
	{
		const bool bReserveAllMappings = Params->GetBoolField(TEXT("reserve_all_mappings"));
		if (Action->bReserveAllMappings != bReserveAllMappings)
		{
			Action->bReserveAllMappings = bReserveAllMappings;
			NotifyInputActionPropertyChanged(Action, GET_MEMBER_NAME_CHECKED(UInputAction, bReserveAllMappings));
			bChanged = true;
			UpdatedFields.Add(TEXT("reserve_all_mappings"));
		}
	}

	if (Params->HasField(TEXT("trigger_when_paused")))
	{
		const bool bTriggerWhenPaused = Params->GetBoolField(TEXT("trigger_when_paused"));
		if (Action->bTriggerWhenPaused != bTriggerWhenPaused)
		{
			Action->bTriggerWhenPaused = bTriggerWhenPaused;
			NotifyInputActionPropertyChanged(Action, GET_MEMBER_NAME_CHECKED(UInputAction, bTriggerWhenPaused));
			bChanged = true;
			UpdatedFields.Add(TEXT("trigger_when_paused"));
		}
	}

	if (Params->HasField(TEXT("trigger_events_that_consume_legacy_keys")))
	{
		const int32 NewMask = static_cast<int32>(Params->GetIntegerField(TEXT("trigger_events_that_consume_legacy_keys")));
		if (Action->TriggerEventsThatConsumeLegacyKeys != NewMask)
		{
			Action->TriggerEventsThatConsumeLegacyKeys = NewMask;
			NotifyInputActionPropertyChanged(Action, GET_MEMBER_NAME_CHECKED(UInputAction, TriggerEventsThatConsumeLegacyKeys));
			bChanged = true;
			UpdatedFields.Add(TEXT("trigger_events_that_consume_legacy_keys"));
		}
	}

	if (Params->HasField(TEXT("description")))
	{
		const FString Description = Params->GetStringField(TEXT("description"));
		if (!Action->ActionDescription.EqualTo(FText::FromString(Description)))
		{
			Action->ActionDescription = FText::FromString(Description);
			NotifyInputActionPropertyChanged(Action, GET_MEMBER_NAME_CHECKED(UInputAction, ActionDescription));
			bChanged = true;
			UpdatedFields.Add(TEXT("description"));
		}
	}

	if (bChanged)
	{
		Action->MarkPackageDirty();
		UPackage* Package = Action->GetOutermost();
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		UPackage::SavePackage(Package, Action, *PackageFilename, SaveArgs);
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetBoolField(TEXT("changed"), bChanged);

	TArray<TSharedPtr<FJsonValue>> UpdatedFieldValues;
	for (const FString& UpdatedField : UpdatedFields)
	{
		UpdatedFieldValues.Add(MakeShared<FJsonValueString>(UpdatedField));
	}
	ResultObj->SetArrayField(TEXT("updated_fields"), UpdatedFieldValues);

	AddInputActionDetailsToResult(Action, PackagePath, ResultObj);
	return ResultObj;
}

// =============================================================================
// FGetInputMappingContextAction - Read Input Mapping Context mappings
// =============================================================================

bool FGetInputMappingContextAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("name")) && !Params->HasField(TEXT("asset_path")))
	{
		OutError = TEXT("Missing 'name' or 'asset_path' parameter");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FGetInputMappingContextAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString PackagePath;
	FString ContextName;
	FString Error;
	UInputMappingContext* MappingContext = LoadInputMappingContextFromParams(Params, PackagePath, ContextName, Error);
	if (!MappingContext)
	{
		return FMCPCommonUtils::CreateErrorResponse(Error);
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	AddInputMappingContextDetailsToResult(MappingContext, PackagePath, ResultObj);
	return ResultObj;
}

// =============================================================================
// FCreateInputMappingContextAction - Input Mapping Context asset
// =============================================================================

bool FCreateInputMappingContextAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("name")))
	{
		OutError = TEXT("Missing 'name' parameter");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FCreateInputMappingContextAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString Name = Params->GetStringField(TEXT("name"));

	// Create package path
	FString Path = TEXT("/Game/Input");
	Params->TryGetStringField(TEXT("path"), Path);
	FString PackagePath = Path / Name;

	// Check if asset already exists and clean up safely
	UPackage* ExistingPackage = FindPackage(nullptr, *PackagePath);
	if (ExistingPackage)
	{
		UInputMappingContext* ExistingIMC = FindObject<UInputMappingContext>(ExistingPackage, *Name);
		if (ExistingIMC)
		{
			UE_LOG(LogMCP, Log, TEXT("Input Mapping Context '%s' already exists, cleaning up for recreation"), *Name);
			FString TempName = FString::Printf(TEXT("%s_TEMP_%d"), *Name, FMath::Rand());
			ExistingIMC->Rename(*TempName, GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional);
			ExistingIMC->MarkAsGarbage();
			ExistingPackage->MarkAsGarbage();
		}
	}

	// Delete from disk if exists
	if (UEditorAssetLibrary::DoesAssetExist(PackagePath))
	{
		UE_LOG(LogMCP, Log, TEXT("Input Mapping Context '%s' exists on disk, deleting"), *Name);
		UEditorAssetLibrary::DeleteAsset(PackagePath);
	}

	// Create the package
	UPackage* Package = CreatePackage(*PackagePath);
	Package->FullyLoad();

	// Create the Input Mapping Context
	UInputMappingContext* NewIMC = NewObject<UInputMappingContext>(Package, *Name, RF_Public | RF_Standalone);
	if (!NewIMC)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Input Mapping Context"));
	}

	// Register with asset registry and save
	FAssetRegistryModule::AssetCreated(NewIMC);
	NewIMC->MarkPackageDirty();

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	FString PackageFilename = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
	UPackage::SavePackage(Package, NewIMC, *PackageFilename, SaveArgs);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("name"), Name);
	ResultObj->SetStringField(TEXT("path"), PackagePath);
	return ResultObj;
}

// =============================================================================
// FAddKeyMappingToContextAction - Add key to IMC with modifiers
// =============================================================================

bool FAddKeyMappingToContextAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("context_name")))
	{
		OutError = TEXT("Missing 'context_name' parameter");
		return false;
	}
	if (!Params->HasField(TEXT("action_name")))
	{
		OutError = TEXT("Missing 'action_name' parameter");
		return false;
	}
	if (!Params->HasField(TEXT("key")))
	{
		OutError = TEXT("Missing 'key' parameter");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FAddKeyMappingToContextAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString ContextName = Params->GetStringField(TEXT("context_name"));
	FString ActionName = Params->GetStringField(TEXT("action_name"));
	FString KeyStr = Params->GetStringField(TEXT("key"));

	// Find the IMC asset
	FString ContextPath = TEXT("/Game/Input");
	Params->TryGetStringField(TEXT("context_path"), ContextPath);
	FString FullContextPath = ContextPath / ContextName + TEXT(".") + ContextName;

	UInputMappingContext* IMC = LoadObject<UInputMappingContext>(nullptr, *FullContextPath);
	if (!IMC)
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Input Mapping Context not found: %s"), *FullContextPath));
	}

	// Find the Input Action asset
	FString ActionPath = TEXT("/Game/Input");
	Params->TryGetStringField(TEXT("action_path"), ActionPath);
	FString FullActionPath = ActionPath / ActionName + TEXT(".") + ActionName;

	UInputAction* Action = LoadObject<UInputAction>(nullptr, *FullActionPath);
	if (!Action)
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Input Action not found: %s"), *FullActionPath));
	}

	// Map the key to the action
	FKey MappedKey{FName{*KeyStr}};
	FEnhancedActionKeyMapping& Mapping = IMC->MapKey(Action, MappedKey);

	// Apply modifiers if specified
	if (Params->HasField(TEXT("modifiers")))
	{
		const TArray<TSharedPtr<FJsonValue>>& ModifiersArray = Params->GetArrayField(TEXT("modifiers"));
		for (const TSharedPtr<FJsonValue>& ModValue : ModifiersArray)
		{
			FString ModName = ModValue->AsString();

			if (ModName == TEXT("Negate"))
			{
				UInputModifierNegate* Mod = NewObject<UInputModifierNegate>(IMC);
				Mapping.Modifiers.Add(Mod);
			}
			else if (ModName == TEXT("SwizzleYXZ") || ModName == TEXT("Swizzle"))
			{
				UInputModifierSwizzleAxis* Mod = NewObject<UInputModifierSwizzleAxis>(IMC);
				Mod->Order = EInputAxisSwizzle::YXZ;
				Mapping.Modifiers.Add(Mod);
			}
			else if (ModName == TEXT("SwizzleZYX"))
			{
				UInputModifierSwizzleAxis* Mod = NewObject<UInputModifierSwizzleAxis>(IMC);
				Mod->Order = EInputAxisSwizzle::ZYX;
				Mapping.Modifiers.Add(Mod);
			}
			else if (ModName == TEXT("SwizzleXZY"))
			{
				UInputModifierSwizzleAxis* Mod = NewObject<UInputModifierSwizzleAxis>(IMC);
				Mod->Order = EInputAxisSwizzle::XZY;
				Mapping.Modifiers.Add(Mod);
			}
			else if (ModName == TEXT("SwizzleYZX"))
			{
				UInputModifierSwizzleAxis* Mod = NewObject<UInputModifierSwizzleAxis>(IMC);
				Mod->Order = EInputAxisSwizzle::YZX;
				Mapping.Modifiers.Add(Mod);
			}
			else if (ModName == TEXT("SwizzleZXY"))
			{
				UInputModifierSwizzleAxis* Mod = NewObject<UInputModifierSwizzleAxis>(IMC);
				Mod->Order = EInputAxisSwizzle::ZXY;
				Mapping.Modifiers.Add(Mod);
			}
		}
	}

	// Save the IMC package
	IMC->MarkPackageDirty();
	UPackage* Package = IMC->GetOutermost();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	UPackage::SavePackage(Package, IMC, *PackageFilename, SaveArgs);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("context"), ContextName);
	ResultObj->SetStringField(TEXT("action"), ActionName);
	ResultObj->SetStringField(TEXT("key"), KeyStr);
	return ResultObj;
}
