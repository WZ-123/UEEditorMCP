// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FDiffAgainstDepotAction
 *
 * Programmatic "Diff Against Depot" — compares a local asset against
 * the latest source-control revision and returns structured diff data
 * as JSON (no UI).
 *
 * For Blueprints the diff walks every graph and produces per-node
 * results (added / removed / modified / moved …).
 * For generic assets it falls back to object-property comparison.
 *
 * Params:
 *   asset_path  (string, required)  — e.g. "/Game/P110_2/Blueprints/BP_Foo"
 *   revision    (int, optional)     — specific revision number (default: latest)
 *
 * Command type: diff_against_depot
 */
class UEEDITORMCP_API FDiffAgainstDepotAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

	/** Convert EDiffType::Type enum to human-readable string */
	static FString DiffTypeToString(int32 DiffType);

	/** Convert EDiffType::Category enum to string */
	static FString DiffCategoryToString(int32 Category);

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("diff_against_depot"); }
	virtual bool RequiresSave() const override { return false; }

private:
	FString AssetPath;
};

/**
 * FGetAssetHistoryAction
 *
 * Lists source-control revision history for a given asset.
 * Returns an array of revisions that actually modified the file
 * (up to the provider limit, typically 100 for SVN).
 *
 * Params:
 *   asset_path  (string, required)  — e.g. "/Game/P110_2/Blueprints/BP_Foo"
 *   max_count   (int, optional)     — max revisions to return (default: all available)
 *
 * Command type: get_asset_history
 */
class UEEDITORMCP_API FGetAssetHistoryAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("get_asset_history"); }
	virtual bool RequiresSave() const override { return false; }

private:
	FString AssetPath;
};


/**
 * FDuplicateAssetAction
 *
 * Duplicates an asset in the Content Browser via IAssetTools.
 * The new asset is immediately visible to the editor — no rescan needed.
 *
 * Params:
 *   asset_path       (string, required) — source asset, e.g. "/Game/Actors/BP_Hero"
 *   new_name         (string, required) — new asset name (no path)
 *   new_package_path (string, optional) — destination folder (default: same folder as source)
 *
 * Command type: duplicate_asset
 */
class UEEDITORMCP_API FDuplicateAssetAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("duplicate_asset"); }

private:
	FString AssetPath;
};


/**
 * FDeleteAssetAction
 *
 * Deletes an asset from the project, checking source-control status first.
 * If the asset is under SC and has been modified/checked-out, the action
 * will return an error unless force=true.
 *
 * Params:
 *   asset_path  (string, required)  — e.g. "/Game/Actors/BP_Temp"
 *   force       (bool, optional, default false) — skip SC safety checks
 *
 * Command type: delete_asset
 */
class UEEDITORMCP_API FDeleteAssetAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("delete_asset"); }

private:
	FString AssetPath;
};


/**
 * FGetAssetSCStatusAction
 *
 * Returns detailed source-control status for an asset, including whether it
 * is locally modified and the base revision it was last synced to.
 * Useful for auto-detecting the base version before a merge.
 *
 * Params:
 *   asset_path  (string, required)
 *
 * Command type: get_asset_sc_status
 */
class UEEDITORMCP_API FGetAssetSCStatusAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("get_asset_sc_status"); }
	virtual bool RequiresSave() const override { return false; }

private:
	FString AssetPath;
};


/**
 * FDiffBlueprintRevisionsAction
 *
 * Compares two depot revisions of the same Blueprint asset and returns
 * structured diff data. Both sides are loaded from source control history.
 * If only revision_a is given, it diffs revision_a against the current
 * local (in-memory) version — i.e. the user's working copy.
 *
 * Params:
 *   asset_path  (string, required)
 *   revision_a  (string, required)  — older revision (commit hash / revision id)
 *   revision_b  (string, optional)  — newer revision; omit to use local working copy
 *
 * Command type: diff_blueprint_revisions
 */
class UEEDITORMCP_API FDiffBlueprintRevisionsAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("diff_blueprint_revisions"); }
	virtual bool RequiresSave() const override { return false; }

private:
	FString AssetPath;

	/** Shared diff logic: compare two blueprint objects and serialize results */
	TSharedPtr<FJsonObject> DiffBlueprints(UBlueprint* OlderBP, UBlueprint* NewerBP, const FString& InAssetPath) const;
};
