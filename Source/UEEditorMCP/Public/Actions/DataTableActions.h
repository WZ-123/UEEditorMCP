// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actions/EditorAction.h"

/**
 * export_data_table_to_csv
 * Params: data_table_path, csv_path (absolute or relative to project dir)
 */
class UEEDITORMCP_API FExportDataTableToCsvAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("export_data_table_to_csv"); }
	virtual bool RequiresSave() const override { return false; }
};

/**
 * import_data_table_from_csv
 * Fills an existing DataTable from CSV using an explicit row struct (no import dialog).
 *
 * Params:
 *   data_table_path  - e.g. /DestructibleCaveGenerator/.../DT_Demo
 *   csv_path         - absolute or project-relative path to .csv
 *   row_struct_path  - e.g. /Script/DestructibleCaveGenerator.TerrainData
 *   preserve_existing_values (bool, default false)
 *   try_asset_import_fallback (bool, default true)
 *   verify_row_name (optional) - e.g. Grass
 *   verify_substring (optional) - e.g. 0.400000
 */
class UEEDITORMCP_API FImportDataTableFromCsvAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("import_data_table_from_csv"); }
	virtual bool RequiresSave() const override { return true; }
};
