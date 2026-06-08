// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/EditorDiffActions.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"

// Source Control
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "ISourceControlRevision.h"
#include "SourceControlOperations.h"
#include "SourceControlHelpers.h"

// Diff
#include "DiffResults.h"
#include "DiffUtils.h"
#include "GraphDiffControl.h"

// Asset loading
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackageName.h"

// Asset tools (duplicate / delete)
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ObjectTools.h"

// Process (git command execution)
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

bool FDiffAgainstDepotAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!GetRequiredString(Params, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}

	// Ensure source control is available
	ISourceControlModule& SCModule = ISourceControlModule::Get();
	if (!SCModule.IsEnabled())
	{
		OutError = TEXT("Source control is not enabled in this editor session.");
		return false;
	}

	ISourceControlProvider& Provider = SCModule.GetProvider();
	if (!Provider.IsAvailable())
	{
		OutError = TEXT("Source control provider is not available / not connected.");
		return false;
	}

	return true;
}

TSharedPtr<FJsonObject> FDiffAgainstDepotAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	// ------------------------------------------------------------------
	// 1. Load the local asset
	// ------------------------------------------------------------------
	UObject* LocalObject = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!LocalObject)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));
	}

	const FString PackagePath = LocalObject->GetOutermost()->GetName();
	const FString PackageName = LocalObject->GetName();

	// ------------------------------------------------------------------
	// 2. Update source control history
	// ------------------------------------------------------------------
	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> UpdateOp = ISourceControlOperation::Create<FUpdateStatus>();
	UpdateOp->SetUpdateHistory(true);

	const FString DiskFilename = USourceControlHelpers::PackageFilename(PackagePath);
	ECommandResult::Type UpdateResult = Provider.Execute(UpdateOp, DiskFilename);

	if (UpdateResult != ECommandResult::Succeeded)
	{
		return CreateErrorResponse(FString::Printf(
			TEXT("Source control UpdateStatus failed for %s (result=%d)"), *AssetPath, static_cast<int32>(UpdateResult)));
	}

	// ------------------------------------------------------------------
	// 3. Get file state & history
	// ------------------------------------------------------------------
	FSourceControlStatePtr State = Provider.GetState(DiskFilename, EStateCacheUsage::Use);
	if (!State.IsValid())
	{
		return CreateErrorResponse(FString::Printf(TEXT("Cannot get source control state for %s"), *AssetPath));
	}

	if (!State->IsSourceControlled())
	{
		return CreateErrorResponse(FString::Printf(TEXT("Asset is not under source control: %s"), *AssetPath));
	}

	if (State->GetHistorySize() == 0)
	{
		return CreateErrorResponse(FString::Printf(TEXT("No source control history for %s"), *AssetPath));
	}

	// Determine which revision to use
	int32 RequestedRevision = static_cast<int32>(GetOptionalNumber(Params, TEXT("revision"), -1));
	TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Revision;

	if (RequestedRevision > 0)
	{
		Revision = State->FindHistoryRevision(RequestedRevision);
		if (!Revision.IsValid())
		{
			return CreateErrorResponse(FString::Printf(
				TEXT("Revision %d not found in history for %s"), RequestedRevision, *AssetPath));
		}
	}
	else
	{
		Revision = State->GetHistoryItem(0); // latest
	}

	if (!Revision.IsValid())
	{
		return CreateErrorResponse(FString::Printf(TEXT("Failed to get revision for %s"), *AssetPath));
	}

	// ------------------------------------------------------------------
	// 4. Load the depot version
	// ------------------------------------------------------------------
	UPackage* DepotPackage = DiffUtils::LoadPackageForDiff(Revision);
	if (!DepotPackage)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Failed to load depot version for %s"), *AssetPath));
	}

	UObject* DepotObject = FindObject<UObject>(DepotPackage, *PackageName);
	if (!DepotObject)
	{
		DepotObject = DepotPackage->FindAssetInPackage();
	}

	if (!DepotObject)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Failed to find asset in depot package for %s"), *AssetPath));
	}

	// ------------------------------------------------------------------
	// 5. Build revision info
	// ------------------------------------------------------------------
	TSharedPtr<FJsonObject> RevisionInfo = MakeShared<FJsonObject>();
	RevisionInfo->SetStringField(TEXT("revision"), Revision->GetRevision());
	RevisionInfo->SetStringField(TEXT("date"), Revision->GetDate().ToString());
	RevisionInfo->SetStringField(TEXT("userName"), Revision->GetUserName());
	RevisionInfo->SetStringField(TEXT("description"), Revision->GetDescription());
	RevisionInfo->SetNumberField(TEXT("changelistId"), Revision->GetCheckInIdentifier());

	// ------------------------------------------------------------------
	// 6. Compute diffs
	// ------------------------------------------------------------------
	TArray<FDiffSingleResult> AllDiffs;
	bool bIsBlueprintDiff = false;

	UBlueprint* LocalBP = Cast<UBlueprint>(LocalObject);
	UBlueprint* DepotBP = Cast<UBlueprint>(DepotObject);

	if (LocalBP && DepotBP)
	{
		bIsBlueprintDiff = true;

		// 6a. Graph-level diff: match graphs by name and diff each pair
		TMap<FString, UEdGraph*> LocalGraphMap;
		for (UEdGraph* Graph : LocalBP->UbergraphPages)
		{
			if (Graph)
			{
				LocalGraphMap.Add(Graph->GetName(), Graph);
			}
		}
		for (UEdGraph* Graph : LocalBP->FunctionGraphs)
		{
			if (Graph)
			{
				LocalGraphMap.Add(Graph->GetName(), Graph);
			}
		}
		for (UEdGraph* Graph : LocalBP->MacroGraphs)
		{
			if (Graph)
			{
				LocalGraphMap.Add(Graph->GetName(), Graph);
			}
		}

		TMap<FString, UEdGraph*> DepotGraphMap;
		for (UEdGraph* Graph : DepotBP->UbergraphPages)
		{
			if (Graph)
			{
				DepotGraphMap.Add(Graph->GetName(), Graph);
			}
		}
		for (UEdGraph* Graph : DepotBP->FunctionGraphs)
		{
			if (Graph)
			{
				DepotGraphMap.Add(Graph->GetName(), Graph);
			}
		}
		for (UEdGraph* Graph : DepotBP->MacroGraphs)
		{
			if (Graph)
			{
				DepotGraphMap.Add(Graph->GetName(), Graph);
			}
		}

		// Diff graphs that exist in both
		TSet<FString> ProcessedGraphs;
		for (auto& Pair : LocalGraphMap)
		{
			const FString& GraphName = Pair.Key;
			UEdGraph* LocalGraph = Pair.Value;
			ProcessedGraphs.Add(GraphName);

			UEdGraph** DepotGraphPtr = DepotGraphMap.Find(GraphName);
			if (DepotGraphPtr && *DepotGraphPtr)
			{
				TArray<FDiffSingleResult> GraphDiffs;
				FGraphDiffControl::DiffGraphs(*DepotGraphPtr, LocalGraph, GraphDiffs);

				for (FDiffSingleResult& Diff : GraphDiffs)
				{
					Diff.OwningObjectPath = GraphName;
					AllDiffs.Add(MoveTemp(Diff));
				}
			}
			else
			{
				// Graph added locally
				FDiffSingleResult AddedResult;
				AddedResult.Diff = EDiffType::OBJECT_ADDED;
				AddedResult.Category = EDiffType::ADDITION;
				AddedResult.DisplayString = FText::FromString(FString::Printf(TEXT("Graph added: %s"), *GraphName));
				AddedResult.OwningObjectPath = GraphName;
				AllDiffs.Add(AddedResult);
			}
		}

		// Graphs only in depot (removed locally)
		for (auto& Pair : DepotGraphMap)
		{
			if (!ProcessedGraphs.Contains(Pair.Key))
			{
				FDiffSingleResult RemovedResult;
				RemovedResult.Diff = EDiffType::OBJECT_REMOVED;
				RemovedResult.Category = EDiffType::SUBTRACTION;
				RemovedResult.DisplayString = FText::FromString(FString::Printf(TEXT("Graph removed: %s"), *Pair.Key));
				RemovedResult.OwningObjectPath = Pair.Key;
				AllDiffs.Add(RemovedResult);
			}
		}
	}
	else
	{
		// 6b. Generic object property diff
		TArray<FSingleObjectDiffEntry> PropertyDiffs;
		DiffUtils::CompareUnrelatedObjects(DepotObject, LocalObject, PropertyDiffs);

		for (const FSingleObjectDiffEntry& PropDiff : PropertyDiffs)
		{
			FDiffSingleResult Result;
			Result.Diff = EDiffType::OBJECT_PROPERTY;
			Result.Object1 = DepotObject;
			Result.Object2 = LocalObject;

			FString DiffTypeName;
			switch (PropDiff.DiffType)
			{
			case EPropertyDiffType::PropertyAddedToA:
				Result.Category = EDiffType::SUBTRACTION;
				DiffTypeName = TEXT("PropertyRemovedLocally");
				break;
			case EPropertyDiffType::PropertyAddedToB:
				Result.Category = EDiffType::ADDITION;
				DiffTypeName = TEXT("PropertyAddedLocally");
				break;
			case EPropertyDiffType::PropertyValueChanged:
				Result.Category = EDiffType::MODIFICATION;
				DiffTypeName = TEXT("PropertyValueChanged");
				break;
			default:
				Result.Category = EDiffType::MINOR;
				DiffTypeName = TEXT("Unknown");
				break;
			}

			Result.DisplayString = FText::FromString(FString::Printf(
				TEXT("%s: %s"), *DiffTypeName, *PropDiff.Identifier.ToDisplayName()));
			AllDiffs.Add(Result);
		}
	}

	// ------------------------------------------------------------------
	// 7. Serialize results to JSON
	// ------------------------------------------------------------------
	int32 AddedCount = 0, RemovedCount = 0, ModifiedCount = 0, MinorCount = 0;
	TArray<TSharedPtr<FJsonValue>> DiffsArray;

	for (const FDiffSingleResult& Diff : AllDiffs)
	{
		if (!Diff.IsRealDifference())
		{
			continue;
		}

		switch (Diff.Category)
		{
		case EDiffType::ADDITION:    ++AddedCount; break;
		case EDiffType::SUBTRACTION: ++RemovedCount; break;
		case EDiffType::MODIFICATION:++ModifiedCount; break;
		case EDiffType::MINOR:       ++MinorCount; break;
		default: break;
		}

		TSharedPtr<FJsonObject> DiffObj = MakeShared<FJsonObject>();
		DiffObj->SetStringField(TEXT("type"), DiffTypeToString(Diff.Diff));
		DiffObj->SetStringField(TEXT("category"), DiffCategoryToString(Diff.Category));
		DiffObj->SetStringField(TEXT("displayString"), Diff.DisplayString.ToString());

		if (!Diff.OwningObjectPath.IsEmpty())
		{
			DiffObj->SetStringField(TEXT("owningGraph"), Diff.OwningObjectPath);
		}

		if (!Diff.ToolTip.IsEmpty())
		{
			DiffObj->SetStringField(TEXT("tooltip"), Diff.ToolTip.ToString());
		}

		if (Diff.Node1)
		{
			DiffObj->SetStringField(TEXT("node1Name"), Diff.Node1->GetNodeTitle(ENodeTitleType::ListView).ToString());
			DiffObj->SetStringField(TEXT("node1Id"), Diff.Node1->NodeGuid.ToString());
		}
		if (Diff.Node2)
		{
			DiffObj->SetStringField(TEXT("node2Name"), Diff.Node2->GetNodeTitle(ENodeTitleType::ListView).ToString());
			DiffObj->SetStringField(TEXT("node2Id"), Diff.Node2->NodeGuid.ToString());
		}
		if (Diff.Pin1)
		{
			DiffObj->SetStringField(TEXT("pin1Name"), Diff.Pin1->GetName());
		}
		if (Diff.Pin2)
		{
			DiffObj->SetStringField(TEXT("pin2Name"), Diff.Pin2->GetName());
		}

		DiffsArray.Add(MakeShared<FJsonValueObject>(DiffObj));
	}

	// ------------------------------------------------------------------
	// 8. Build final response
	// ------------------------------------------------------------------
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("assetClass"), LocalObject->GetClass()->GetName());
	Result->SetBoolField(TEXT("isBlueprintDiff"), bIsBlueprintDiff);
	Result->SetObjectField(TEXT("revisionInfo"), RevisionInfo);
	Result->SetBoolField(TEXT("hasDifferences"), DiffsArray.Num() > 0);

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("totalDiffs"), DiffsArray.Num());
	Summary->SetNumberField(TEXT("added"), AddedCount);
	Summary->SetNumberField(TEXT("removed"), RemovedCount);
	Summary->SetNumberField(TEXT("modified"), ModifiedCount);
	Summary->SetNumberField(TEXT("minor"), MinorCount);
	Result->SetObjectField(TEXT("summary"), Summary);

	Result->SetArrayField(TEXT("diffs"), DiffsArray);

	return CreateSuccessResponse(Result);
}

FString FDiffAgainstDepotAction::DiffTypeToString(int32 DiffType)
{
	switch (static_cast<EDiffType::Type>(DiffType))
	{
	case EDiffType::NO_DIFFERENCE:             return TEXT("NO_DIFFERENCE");
	case EDiffType::OBJECT_REMOVED:            return TEXT("OBJECT_REMOVED");
	case EDiffType::OBJECT_ADDED:              return TEXT("OBJECT_ADDED");
	case EDiffType::OBJECT_PROPERTY:           return TEXT("OBJECT_PROPERTY");
	case EDiffType::OBJECT_REQUEST_DIFF:       return TEXT("OBJECT_REQUEST_DIFF");
	case EDiffType::NODE_REMOVED:              return TEXT("NODE_REMOVED");
	case EDiffType::NODE_ADDED:                return TEXT("NODE_ADDED");
	case EDiffType::PIN_LINKEDTO_NUM_DEC:      return TEXT("PIN_LINKEDTO_NUM_DEC");
	case EDiffType::PIN_LINKEDTO_NUM_INC:      return TEXT("PIN_LINKEDTO_NUM_INC");
	case EDiffType::PIN_DEFAULT_VALUE:         return TEXT("PIN_DEFAULT_VALUE");
	case EDiffType::PIN_TYPE_CATEGORY:         return TEXT("PIN_TYPE_CATEGORY");
	case EDiffType::PIN_TYPE_SUBCATEGORY:      return TEXT("PIN_TYPE_SUBCATEGORY");
	case EDiffType::PIN_TYPE_SUBCATEGORY_OBJECT: return TEXT("PIN_TYPE_SUBCATEGORY_OBJECT");
	case EDiffType::PIN_TYPE_IS_ARRAY:         return TEXT("PIN_TYPE_IS_ARRAY");
	case EDiffType::PIN_TYPE_IS_REF:           return TEXT("PIN_TYPE_IS_REF");
	case EDiffType::PIN_LINKEDTO_NODE:         return TEXT("PIN_LINKEDTO_NODE");
	case EDiffType::PIN_LINKEDTO_PIN:          return TEXT("PIN_LINKEDTO_PIN");
	case EDiffType::NODE_MOVED:                return TEXT("NODE_MOVED");
	case EDiffType::TIMELINE_LENGTH:           return TEXT("TIMELINE_LENGTH");
	case EDiffType::TIMELINE_AUTOPLAY:         return TEXT("TIMELINE_AUTOPLAY");
	case EDiffType::TIMELINE_LOOP:             return TEXT("TIMELINE_LOOP");
	case EDiffType::TIMELINE_IGNOREDILATION:   return TEXT("TIMELINE_IGNOREDILATION");
	case EDiffType::TIMELINE_NUM_TRACKS:       return TEXT("TIMELINE_NUM_TRACKS");
	case EDiffType::TIMELINE_TRACK_MODIFIED:   return TEXT("TIMELINE_TRACK_MODIFIED");
	case EDiffType::NODE_PIN_COUNT:            return TEXT("NODE_PIN_COUNT");
	case EDiffType::NODE_COMMENT:              return TEXT("NODE_COMMENT");
	case EDiffType::NODE_PROPERTY:             return TEXT("NODE_PROPERTY");
	case EDiffType::INFO_MESSAGE:              return TEXT("INFO_MESSAGE");
	default:                                    return TEXT("UNKNOWN");
	}
}

FString FDiffAgainstDepotAction::DiffCategoryToString(int32 Category)
{
	switch (static_cast<EDiffType::Category>(Category))
	{
	case EDiffType::ADDITION:     return TEXT("ADDITION");
	case EDiffType::SUBTRACTION:  return TEXT("SUBTRACTION");
	case EDiffType::MODIFICATION: return TEXT("MODIFICATION");
	case EDiffType::MINOR:        return TEXT("MINOR");
	case EDiffType::CONTROL:      return TEXT("CONTROL");
	default:                       return TEXT("UNKNOWN");
	}
}

// =====================================================================
// FGetAssetHistoryAction
// =====================================================================

bool FGetAssetHistoryAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!GetRequiredString(Params, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}

	ISourceControlModule& SCModule = ISourceControlModule::Get();
	if (!SCModule.IsEnabled())
	{
		OutError = TEXT("Source control is not enabled in this editor session.");
		return false;
	}

	ISourceControlProvider& Provider = SCModule.GetProvider();
	if (!Provider.IsAvailable())
	{
		OutError = TEXT("Source control provider is not available / not connected.");
		return false;
	}

	return true;
}

TSharedPtr<FJsonObject> FGetAssetHistoryAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	// ------------------------------------------------------------------
	// 1. Resolve package path to disk file
	// ------------------------------------------------------------------
	UObject* LocalObject = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!LocalObject)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));
	}

	const FString PackagePath = LocalObject->GetOutermost()->GetName();
	const FString DiskFilename = USourceControlHelpers::PackageFilename(PackagePath);

	// ------------------------------------------------------------------
	// 2. Update source control history
	// ------------------------------------------------------------------
	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> UpdateOp = ISourceControlOperation::Create<FUpdateStatus>();
	UpdateOp->SetUpdateHistory(true);

	ECommandResult::Type UpdateResult = Provider.Execute(UpdateOp, DiskFilename);
	if (UpdateResult != ECommandResult::Succeeded)
	{
		return CreateErrorResponse(FString::Printf(
			TEXT("Source control UpdateStatus failed for %s (result=%d)"), *AssetPath, static_cast<int32>(UpdateResult)));
	}

	// ------------------------------------------------------------------
	// 3. Get file state & history
	// ------------------------------------------------------------------
	FSourceControlStatePtr State = Provider.GetState(DiskFilename, EStateCacheUsage::Use);
	if (!State.IsValid())
	{
		return CreateErrorResponse(FString::Printf(TEXT("Cannot get source control state for %s"), *AssetPath));
	}

	if (!State->IsSourceControlled())
	{
		return CreateErrorResponse(FString::Printf(TEXT("Asset is not under source control: %s"), *AssetPath));
	}

	const int32 HistorySize = State->GetHistorySize();
	if (HistorySize == 0)
	{
		return CreateErrorResponse(FString::Printf(TEXT("No source control history for %s"), *AssetPath));
	}

	// ------------------------------------------------------------------
	// 4. Build revision list
	// ------------------------------------------------------------------
	int32 MaxCount = static_cast<int32>(GetOptionalNumber(Params, TEXT("max_count"), 0));
	if (MaxCount <= 0)
	{
		MaxCount = HistorySize;
	}
	else
	{
		MaxCount = FMath::Min(MaxCount, HistorySize);
	}

	TArray<TSharedPtr<FJsonValue>> RevisionsArray;
	for (int32 i = 0; i < MaxCount; ++i)
	{
		TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Revision = State->GetHistoryItem(i);
		if (!Revision.IsValid())
		{
			continue;
		}

		TSharedPtr<FJsonObject> RevObj = MakeShared<FJsonObject>();
		RevObj->SetStringField(TEXT("revision"), Revision->GetRevision());
		RevObj->SetNumberField(TEXT("revisionNumber"), Revision->GetCheckInIdentifier());
		RevObj->SetStringField(TEXT("date"), Revision->GetDate().ToString());
		RevObj->SetStringField(TEXT("userName"), Revision->GetUserName());
		RevObj->SetStringField(TEXT("description"), Revision->GetDescription());
		RevObj->SetStringField(TEXT("action"), Revision->GetAction());

		RevisionsArray.Add(MakeShared<FJsonValueObject>(RevObj));
	}

	// ------------------------------------------------------------------
	// 5. Build response
	// ------------------------------------------------------------------
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetNumberField(TEXT("totalRevisions"), RevisionsArray.Num());
	Result->SetNumberField(TEXT("historyAvailable"), HistorySize);
	Result->SetArrayField(TEXT("revisions"), RevisionsArray);

	return CreateSuccessResponse(Result);
}


// =====================================================================
// FDuplicateAssetAction
// =====================================================================

bool FDuplicateAssetAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!GetRequiredString(Params, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	FString NewName;
	if (!GetRequiredString(Params, TEXT("new_name"), NewName, OutError))
	{
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FDuplicateAssetAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	UObject* SourceObject = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!SourceObject)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Failed to load source asset: %s"), *AssetPath));
	}

	FString NewName;
	GetRequiredString(Params, TEXT("new_name"), NewName, NewName);
	FString NewPackagePath = GetOptionalString(Params, TEXT("new_package_path"));
	if (NewPackagePath.IsEmpty())
	{
		NewPackagePath = FPackageName::GetLongPackagePath(SourceObject->GetOutermost()->GetName());
	}

	IAssetTools& AssetToolsRef = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* NewAsset = AssetToolsRef.DuplicateAsset(NewName, NewPackagePath, SourceObject);
	if (!NewAsset)
	{
		return CreateErrorResponse(FString::Printf(TEXT("DuplicateAsset failed for %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("sourceAssetPath"), AssetPath);
	Result->SetStringField(TEXT("newAssetPath"), NewAsset->GetPathName());
	Result->SetStringField(TEXT("newPackagePath"), NewAsset->GetOutermost()->GetName());
	Result->SetStringField(TEXT("newName"), NewAsset->GetName());

	return CreateSuccessResponse(Result);
}


// =====================================================================
// FDeleteAssetAction
// =====================================================================

bool FDeleteAssetAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!GetRequiredString(Params, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FDeleteAssetAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	UObject* ObjectToDelete = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!ObjectToDelete)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));
	}

	bool bForce = GetOptionalBool(Params, TEXT("force"), false);

	// Check source control status before deleting
	ISourceControlModule& SCModule = ISourceControlModule::Get();
	if (SCModule.IsEnabled() && SCModule.GetProvider().IsAvailable())
	{
		const FString PackagePath = ObjectToDelete->GetOutermost()->GetName();
		const FString DiskFilename = USourceControlHelpers::PackageFilename(PackagePath);
		ISourceControlProvider& Provider = SCModule.GetProvider();

		TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> UpdateOp = ISourceControlOperation::Create<FUpdateStatus>();
		Provider.Execute(UpdateOp, DiskFilename);

		FSourceControlStatePtr State = Provider.GetState(DiskFilename, EStateCacheUsage::Use);
		if (State.IsValid() && State->IsSourceControlled())
		{
			if (!bForce)
			{
				if (State->IsCheckedOutOther())
				{
					return CreateErrorResponse(FString::Printf(
						TEXT("Asset is checked out by another user. Use force=true to override. Asset: %s"), *AssetPath));
				}
				if (State->IsModified() || State->IsCheckedOut())
				{
					return CreateErrorResponse(
						FString::Printf(TEXT("Asset has local SC modifications. Use force=true to confirm deletion. Asset: %s"), *AssetPath),
						TEXT("sc_modified_warning"));
				}
			}

			TSharedRef<FDelete, ESPMode::ThreadSafe> DeleteOp = ISourceControlOperation::Create<FDelete>();
			Provider.Execute(DeleteOp, DiskFilename);
		}
	}

	TArray<UObject*> ObjectsToDelete;
	ObjectsToDelete.Add(ObjectToDelete);
	int32 DeletedCount = ObjectTools::DeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/false);

	if (DeletedCount == 0)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Failed to delete asset (may still be referenced): %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("deletedAssetPath"), AssetPath);
	Result->SetNumberField(TEXT("deletedCount"), DeletedCount);

	return CreateSuccessResponse(Result);
}


// =====================================================================
// FGetAssetSCStatusAction
// =====================================================================

bool FGetAssetSCStatusAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!GetRequiredString(Params, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}

	ISourceControlModule& SCModule = ISourceControlModule::Get();
	if (!SCModule.IsEnabled())
	{
		OutError = TEXT("Source control is not enabled in this editor session.");
		return false;
	}

	ISourceControlProvider& Provider = SCModule.GetProvider();
	if (!Provider.IsAvailable())
	{
		OutError = TEXT("Source control provider is not available / not connected.");
		return false;
	}

	return true;
}

/**
 * Helper: run a git command in the project directory and capture stdout.
 * Returns true if the command succeeded (exit code 0).
 */
static bool RunGitCommand(const FString& Args, FString& OutStdout)
{
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	int32 ReturnCode = -1;
	FString StdErr;

	FPlatformProcess::ExecProcess(
		TEXT("git"), *Args,
		&ReturnCode, &OutStdout, &StdErr,
		*ProjectDir);

	OutStdout.TrimStartAndEndInline();
	return ReturnCode == 0;
}

TSharedPtr<FJsonObject> FGetAssetSCStatusAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	UObject* LocalObject = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!LocalObject)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));
	}

	const FString PackagePath = LocalObject->GetOutermost()->GetName();
	const FString DiskFilename = USourceControlHelpers::PackageFilename(PackagePath);

	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> UpdateOp = ISourceControlOperation::Create<FUpdateStatus>();
	UpdateOp->SetUpdateHistory(true);

	ECommandResult::Type UpdateResult = Provider.Execute(UpdateOp, DiskFilename);
	if (UpdateResult != ECommandResult::Succeeded)
	{
		return CreateErrorResponse(FString::Printf(
			TEXT("Source control UpdateStatus failed for %s"), *AssetPath));
	}

	FSourceControlStatePtr State = Provider.GetState(DiskFilename, EStateCacheUsage::Use);
	if (!State.IsValid())
	{
		return CreateErrorResponse(FString::Printf(TEXT("Cannot get SC state for %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("diskFilename"), DiskFilename);
	Result->SetBoolField(TEXT("isSourceControlled"), State->IsSourceControlled());
	Result->SetBoolField(TEXT("isAdded"), State->IsAdded());
	Result->SetBoolField(TEXT("isDeleted"), State->IsDeleted());
	Result->SetBoolField(TEXT("isModified"), State->IsModified());
	Result->SetBoolField(TEXT("isCheckedOut"), State->IsCheckedOut());
	Result->SetBoolField(TEXT("isCheckedOutOther"), State->IsCheckedOutOther());
	Result->SetBoolField(TEXT("isCurrent"), State->IsCurrent());
	Result->SetBoolField(TEXT("isConflicted"), State->IsConflicted());
	Result->SetBoolField(TEXT("canCheckout"), State->CanCheckout());
	Result->SetBoolField(TEXT("canEdit"), State->CanEdit());

	TOptional<FText> StatusTextOpt = State->GetStatusText();
	FString StatusText = StatusTextOpt.IsSet() ? StatusTextOpt.GetValue().ToString() : TEXT("");
	Result->SetStringField(TEXT("statusText"), StatusText);

	const int32 HistorySize = State->GetHistorySize();
	Result->SetNumberField(TEXT("historySize"), HistorySize);

	// ------------------------------------------------------------------
	// Determine the TRUE base revision via git.
	//
	// UE's SC history (GetHistoryItem) comes from the provider which may
	// include remote commits after a fetch.  The actual base the user's
	// working copy is built on is the last commit that touched this file
	// on the CURRENT local branch — obtained via:
	//   git log -1 --format=%H -- <file>
	//
	// We also query the remote tracking branch to get the remote latest:
	//   git log -1 --format=%H origin/HEAD -- <file>
	// ------------------------------------------------------------------

	// Make the path relative to the project root for git
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FString RelativePath = DiskFilename;
	FPaths::MakePathRelativeTo(RelativePath, *ProjectDir);

	// localBaseRevision: the last commit on the current branch for this file
	{
		FString GitOut;
		FString GitArgs = FString::Printf(TEXT("log -1 --format=%%H -- \"%s\""), *RelativePath);
		if (RunGitCommand(GitArgs, GitOut) && !GitOut.IsEmpty())
		{
			TSharedPtr<FJsonObject> LocalBase = MakeShared<FJsonObject>();
			LocalBase->SetStringField(TEXT("revision"), GitOut);

			// Also get the commit message and date
			FString DetailOut;
			FString DetailArgs = FString::Printf(TEXT("log -1 --format=%%ai%%n%%an%%n%%s %s -- \"%s\""), *GitOut, *RelativePath);
			if (RunGitCommand(DetailArgs, DetailOut))
			{
				TArray<FString> Lines;
				DetailOut.ParseIntoArrayLines(Lines);
				if (Lines.Num() >= 1) LocalBase->SetStringField(TEXT("date"), Lines[0]);
				if (Lines.Num() >= 2) LocalBase->SetStringField(TEXT("userName"), Lines[1]);
				if (Lines.Num() >= 3) LocalBase->SetStringField(TEXT("description"), Lines[2]);
			}

			Result->SetObjectField(TEXT("localBaseRevision"), LocalBase);
		}
	}

	// remoteLatestRevision: the last commit on the remote tracking branch for this file
	{
		// Detect the remote tracking branch name
		FString RemoteBranch;
		FString TrackOut;
		if (RunGitCommand(TEXT("rev-parse --abbrev-ref --symbolic-full-name @{u}"), TrackOut) && !TrackOut.IsEmpty())
		{
			RemoteBranch = TrackOut;
		}
		else
		{
			// Fallback: try origin/master then origin/main
			FString CheckOut;
			if (RunGitCommand(TEXT("rev-parse --verify origin/master"), CheckOut))
			{
				RemoteBranch = TEXT("origin/master");
			}
			else if (RunGitCommand(TEXT("rev-parse --verify origin/main"), CheckOut))
			{
				RemoteBranch = TEXT("origin/main");
			}
		}

		if (!RemoteBranch.IsEmpty())
		{
			FString GitOut;
			FString GitArgs = FString::Printf(TEXT("log -1 --format=%%H %s -- \"%s\""), *RemoteBranch, *RelativePath);
			if (RunGitCommand(GitArgs, GitOut) && !GitOut.IsEmpty())
			{
				TSharedPtr<FJsonObject> RemoteLatest = MakeShared<FJsonObject>();
				RemoteLatest->SetStringField(TEXT("revision"), GitOut);
				RemoteLatest->SetStringField(TEXT("remoteBranch"), RemoteBranch);

				FString DetailOut;
				FString DetailArgs = FString::Printf(TEXT("log -1 --format=%%ai%%n%%an%%n%%s %s -- \"%s\""), *GitOut, *RelativePath);
				if (RunGitCommand(DetailArgs, DetailOut))
				{
					TArray<FString> Lines;
					DetailOut.ParseIntoArrayLines(Lines);
					if (Lines.Num() >= 1) RemoteLatest->SetStringField(TEXT("date"), Lines[0]);
					if (Lines.Num() >= 2) RemoteLatest->SetStringField(TEXT("userName"), Lines[1]);
					if (Lines.Num() >= 3) RemoteLatest->SetStringField(TEXT("description"), Lines[2]);
				}

				Result->SetObjectField(TEXT("remoteLatestRevision"), RemoteLatest);
			}
		}
	}

	// isAheadOfBase: local base != remote latest → someone pushed new changes
	{
		FString LocalRev, RemoteRev;
		const TSharedPtr<FJsonObject>* LocalObj = nullptr;
		const TSharedPtr<FJsonObject>* RemoteObj = nullptr;
		if (Result->TryGetObjectField(TEXT("localBaseRevision"), LocalObj) && LocalObj)
		{
			LocalRev = (*LocalObj)->GetStringField(TEXT("revision"));
		}
		if (Result->TryGetObjectField(TEXT("remoteLatestRevision"), RemoteObj) && RemoteObj)
		{
			RemoteRev = (*RemoteObj)->GetStringField(TEXT("revision"));
		}
		if (!LocalRev.IsEmpty() && !RemoteRev.IsEmpty())
		{
			Result->SetBoolField(TEXT("remoteHasNewerVersion"), LocalRev != RemoteRev);
		}
	}

	UPackage* Package = LocalObject->GetOutermost();
	Result->SetBoolField(TEXT("isPackageDirty"), Package ? Package->IsDirty() : false);

	return CreateSuccessResponse(Result);
}


// =====================================================================
// FDiffBlueprintRevisionsAction
// =====================================================================

bool FDiffBlueprintRevisionsAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!GetRequiredString(Params, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}

	FString RevisionA;
	if (!GetRequiredString(Params, TEXT("revision_a"), RevisionA, OutError))
	{
		return false;
	}

	ISourceControlModule& SCModule = ISourceControlModule::Get();
	if (!SCModule.IsEnabled())
	{
		OutError = TEXT("Source control is not enabled in this editor session.");
		return false;
	}

	ISourceControlProvider& Provider = SCModule.GetProvider();
	if (!Provider.IsAvailable())
	{
		OutError = TEXT("Source control provider is not available / not connected.");
		return false;
	}

	return true;
}

TSharedPtr<FJsonObject> FDiffBlueprintRevisionsAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString RevisionAStr;
	GetRequiredString(Params, TEXT("revision_a"), RevisionAStr, RevisionAStr);
	FString RevisionBStr = GetOptionalString(Params, TEXT("revision_b"));

	UObject* LocalObject = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!LocalObject)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));
	}

	const FString PackagePath = LocalObject->GetOutermost()->GetName();
	const FString PackageName = LocalObject->GetName();
	const FString DiskFilename = USourceControlHelpers::PackageFilename(PackagePath);

	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> UpdateOp = ISourceControlOperation::Create<FUpdateStatus>();
	UpdateOp->SetUpdateHistory(true);
	Provider.Execute(UpdateOp, DiskFilename);

	FSourceControlStatePtr State = Provider.GetState(DiskFilename, EStateCacheUsage::Use);
	if (!State.IsValid() || !State->IsSourceControlled())
	{
		return CreateErrorResponse(FString::Printf(TEXT("Asset not under source control: %s"), *AssetPath));
	}

	// Find revision A by string prefix matching (supports short commit hashes)
	TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> RevisionA;
	for (int32 i = 0; i < State->GetHistorySize(); ++i)
	{
		TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Rev = State->GetHistoryItem(i);
		if (Rev.IsValid() && (Rev->GetRevision() == RevisionAStr || Rev->GetRevision().StartsWith(RevisionAStr)))
		{
			RevisionA = Rev;
			break;
		}
	}
	if (!RevisionA.IsValid())
	{
		return CreateErrorResponse(FString::Printf(
			TEXT("Revision '%s' not found in history for %s. Use editor.get_asset_history to list available revisions."),
			*RevisionAStr, *AssetPath));
	}

	UPackage* PackageA = DiffUtils::LoadPackageForDiff(RevisionA);
	if (!PackageA)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Failed to load depot package for revision %s"), *RevisionAStr));
	}

	UObject* ObjectA = FindObject<UObject>(PackageA, *PackageName);
	if (!ObjectA)
	{
		ObjectA = PackageA->FindAssetInPackage();
	}
	if (!ObjectA)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Failed to find asset in package for revision %s"), *RevisionAStr));
	}

	UBlueprint* BlueprintA = Cast<UBlueprint>(ObjectA);
	if (!BlueprintA)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Asset at revision %s is not a Blueprint"), *RevisionAStr));
	}

	UBlueprint* BlueprintB = nullptr;
	FString RevBLabel;

	if (RevisionBStr.IsEmpty())
	{
		BlueprintB = Cast<UBlueprint>(LocalObject);
		if (!BlueprintB)
		{
			return CreateErrorResponse(TEXT("Local asset is not a Blueprint"));
		}
		RevBLabel = TEXT("local");
	}
	else
	{
		TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> RevisionB;
		for (int32 i = 0; i < State->GetHistorySize(); ++i)
		{
			TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Rev = State->GetHistoryItem(i);
			if (Rev.IsValid() && (Rev->GetRevision() == RevisionBStr || Rev->GetRevision().StartsWith(RevisionBStr)))
			{
				RevisionB = Rev;
				break;
			}
		}
		if (!RevisionB.IsValid())
		{
			return CreateErrorResponse(FString::Printf(
				TEXT("Revision '%s' not found in history for %s"), *RevisionBStr, *AssetPath));
		}

		UPackage* PackageB = DiffUtils::LoadPackageForDiff(RevisionB);
		if (!PackageB)
		{
			return CreateErrorResponse(FString::Printf(TEXT("Failed to load depot package for revision %s"), *RevisionBStr));
		}

		UObject* ObjectB = FindObject<UObject>(PackageB, *PackageName);
		if (!ObjectB)
		{
			ObjectB = PackageB->FindAssetInPackage();
		}
		if (!ObjectB)
		{
			return CreateErrorResponse(FString::Printf(TEXT("Failed to find asset in package for revision %s"), *RevisionBStr));
		}

		BlueprintB = Cast<UBlueprint>(ObjectB);
		if (!BlueprintB)
		{
			return CreateErrorResponse(FString::Printf(TEXT("Asset at revision %s is not a Blueprint"), *RevisionBStr));
		}
		RevBLabel = RevisionBStr;
	}

	TSharedPtr<FJsonObject> DiffResult = DiffBlueprints(BlueprintA, BlueprintB, AssetPath);
	DiffResult->SetStringField(TEXT("revisionA"), RevisionAStr);
	DiffResult->SetStringField(TEXT("revisionB"), RevBLabel);

	return CreateSuccessResponse(DiffResult);
}

TSharedPtr<FJsonObject> FDiffBlueprintRevisionsAction::DiffBlueprints(
	UBlueprint* OlderBP, UBlueprint* NewerBP, const FString& InAssetPath) const
{
	auto CollectGraphs = [](UBlueprint* BP) -> TMap<FString, UEdGraph*>
	{
		TMap<FString, UEdGraph*> GraphMap;
		for (UEdGraph* Graph : BP->UbergraphPages)
		{
			if (Graph) GraphMap.Add(Graph->GetName(), Graph);
		}
		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (Graph) GraphMap.Add(Graph->GetName(), Graph);
		}
		for (UEdGraph* Graph : BP->MacroGraphs)
		{
			if (Graph) GraphMap.Add(Graph->GetName(), Graph);
		}
		return GraphMap;
	};

	TMap<FString, UEdGraph*> OlderGraphs = CollectGraphs(OlderBP);
	TMap<FString, UEdGraph*> NewerGraphs = CollectGraphs(NewerBP);

	TArray<FDiffSingleResult> AllDiffs;
	TSet<FString> Processed;

	for (auto& Pair : NewerGraphs)
	{
		const FString& GraphName = Pair.Key;
		Processed.Add(GraphName);

		UEdGraph** OlderPtr = OlderGraphs.Find(GraphName);
		if (OlderPtr && *OlderPtr)
		{
			TArray<FDiffSingleResult> GraphDiffs;
			FGraphDiffControl::DiffGraphs(*OlderPtr, Pair.Value, GraphDiffs);
			for (FDiffSingleResult& Diff : GraphDiffs)
			{
				Diff.OwningObjectPath = GraphName;
				AllDiffs.Add(MoveTemp(Diff));
			}
		}
		else
		{
			FDiffSingleResult Added;
			Added.Diff = EDiffType::OBJECT_ADDED;
			Added.Category = EDiffType::ADDITION;
			Added.DisplayString = FText::FromString(FString::Printf(TEXT("Graph added: %s"), *GraphName));
			Added.OwningObjectPath = GraphName;
			AllDiffs.Add(Added);
		}
	}

	for (auto& Pair : OlderGraphs)
	{
		if (!Processed.Contains(Pair.Key))
		{
			FDiffSingleResult Removed;
			Removed.Diff = EDiffType::OBJECT_REMOVED;
			Removed.Category = EDiffType::SUBTRACTION;
			Removed.DisplayString = FText::FromString(FString::Printf(TEXT("Graph removed: %s"), *Pair.Key));
			Removed.OwningObjectPath = Pair.Key;
			AllDiffs.Add(Removed);
		}
	}

	int32 AddedCount = 0, RemovedCount = 0, ModifiedCount = 0, MinorCount = 0;
	TArray<TSharedPtr<FJsonValue>> DiffsArray;

	for (const FDiffSingleResult& Diff : AllDiffs)
	{
		if (!Diff.IsRealDifference()) continue;

		switch (Diff.Category)
		{
		case EDiffType::ADDITION:     ++AddedCount; break;
		case EDiffType::SUBTRACTION:  ++RemovedCount; break;
		case EDiffType::MODIFICATION: ++ModifiedCount; break;
		case EDiffType::MINOR:        ++MinorCount; break;
		default: break;
		}

		TSharedPtr<FJsonObject> DiffObj = MakeShared<FJsonObject>();
		DiffObj->SetStringField(TEXT("type"), FDiffAgainstDepotAction::DiffTypeToString(Diff.Diff));
		DiffObj->SetStringField(TEXT("category"), FDiffAgainstDepotAction::DiffCategoryToString(Diff.Category));
		DiffObj->SetStringField(TEXT("displayString"), Diff.DisplayString.ToString());

		if (!Diff.OwningObjectPath.IsEmpty())
		{
			DiffObj->SetStringField(TEXT("owningGraph"), Diff.OwningObjectPath);
		}
		if (Diff.Node1)
		{
			DiffObj->SetStringField(TEXT("node1Name"), Diff.Node1->GetNodeTitle(ENodeTitleType::ListView).ToString());
			DiffObj->SetStringField(TEXT("node1Id"), Diff.Node1->NodeGuid.ToString());
		}
		if (Diff.Node2)
		{
			DiffObj->SetStringField(TEXT("node2Name"), Diff.Node2->GetNodeTitle(ENodeTitleType::ListView).ToString());
			DiffObj->SetStringField(TEXT("node2Id"), Diff.Node2->NodeGuid.ToString());
		}
		if (Diff.Pin1)
		{
			DiffObj->SetStringField(TEXT("pin1Name"), Diff.Pin1->GetName());
		}
		if (Diff.Pin2)
		{
			DiffObj->SetStringField(TEXT("pin2Name"), Diff.Pin2->GetName());
		}

		DiffsArray.Add(MakeShared<FJsonValueObject>(DiffObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("assetPath"), InAssetPath);
	Result->SetBoolField(TEXT("hasDifferences"), DiffsArray.Num() > 0);

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("totalDiffs"), DiffsArray.Num());
	Summary->SetNumberField(TEXT("added"), AddedCount);
	Summary->SetNumberField(TEXT("removed"), RemovedCount);
	Summary->SetNumberField(TEXT("modified"), ModifiedCount);
	Summary->SetNumberField(TEXT("minor"), MinorCount);
	Result->SetObjectField(TEXT("summary"), Summary);
	Result->SetArrayField(TEXT("diffs"), DiffsArray);

	return Result;
}
