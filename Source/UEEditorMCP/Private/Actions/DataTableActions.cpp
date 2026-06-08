// Copyright (c) 2025 zolnoor. All Rights Reserved.

#include "Actions/DataTableActions.h"

#include "Actions/EditorAction.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/DataTable.h"
#include "Factories/DataTableFactory.h"
#include "IAssetTools.h"
#include "Kismet/DataTableFunctionLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UEEditorMCPDataTable
{
	static FString ResolveExistingFilePath(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return InPath;
		}

		if (FPaths::FileExists(InPath))
		{
			return FPaths::ConvertRelativePathToFull(InPath);
		}

		const FString RelativeToProject = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), InPath);
		if (FPaths::FileExists(RelativeToProject))
		{
			return RelativeToProject;
		}

		return InPath;
	}

	static UDataTable* LoadDataTableAsset(const FString& DataTablePath, FString& OutError)
	{
		UDataTable* DataTable = LoadObject<UDataTable>(nullptr, *DataTablePath);
		if (!DataTable)
		{
			DataTable = Cast<UDataTable>(UEditorAssetLibrary::LoadAsset(DataTablePath));
		}

		if (!DataTable)
		{
			OutError = FString::Printf(TEXT("Failed to load DataTable: %s"), *DataTablePath);
		}

		return DataTable;
	}

	static UScriptStruct* LoadRowStruct(const FString& RowStructPath, FString& OutError)
	{
		UScriptStruct* RowStruct = LoadObject<UScriptStruct>(nullptr, *RowStructPath);
		if (!RowStruct)
		{
			RowStruct = FindObject<UScriptStruct>(nullptr, *RowStructPath);
		}

		if (!RowStruct)
		{
			OutError = FString::Printf(
				TEXT("Failed to load row struct: %s (compile the module that defines it first)"),
				*RowStructPath);
		}

		return RowStruct;
	}

	static void SetPreserveExistingValues(UDataTable* DataTable, bool bPreserve)
	{
		if (!DataTable)
		{
			return;
		}

		FProperty* Prop = DataTable->GetClass()->FindPropertyByName(TEXT("bPreserveExistingValues"));
		if (!Prop)
		{
			return;
		}

		if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			BoolProp->SetPropertyValue_InContainer(DataTable, bPreserve);
		}
	}

	static bool ImportViaFillCsv(UDataTable* DataTable, UScriptStruct* RowStruct, const FString& CsvPath, bool bPreserveExisting)
	{
		SetPreserveExistingValues(DataTable, bPreserveExisting);
		return UDataTableFunctionLibrary::FillDataTableFromCSVFile(DataTable, CsvPath, RowStruct);
	}

	static bool ImportViaAssetTools(
		UScriptStruct* RowStruct,
		const FString& CsvPath,
		const FString& DestinationPath,
		const FString& DestinationName)
	{
		UDataTableFactory* Factory = NewObject<UDataTableFactory>();
		Factory->Struct = RowStruct;

		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = CsvPath;
		Task->DestinationPath = DestinationPath;
		Task->DestinationName = DestinationName;
		Task->bReplaceExisting = true;
		Task->bAutomated = true;
		Task->bSave = false;
		Task->Factory = Factory;

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		TArray<UAssetImportTask*> Tasks;
		Tasks.Add(Task);
		AssetToolsModule.Get().ImportAssetTasks(Tasks);
		return Task->ImportedObjectPaths.Num() > 0 || Task->GetObjects().Num() > 0;
	}

	static bool VerifyRowContains(const UDataTable* DataTable, const FString& RowName, const FString& Substring)
	{
		if (!DataTable || RowName.IsEmpty() || Substring.IsEmpty())
		{
			return true;
		}

		FString Exported;
		if (!UDataTableFunctionLibrary::ExportDataTableToCSVString(DataTable, Exported))
		{
			return false;
		}
		TArray<FString> Lines;
		Exported.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(RowName) && Line.Contains(Substring))
			{
				return true;
			}
		}

		return false;
	}
}

// ============================================================================
// FExportDataTableToCsvAction
// ============================================================================

bool FExportDataTableToCsvAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString Unused;
	if (!GetRequiredString(Params, TEXT("data_table_path"), Unused, OutError))
	{
		return false;
	}
	if (!GetRequiredString(Params, TEXT("csv_path"), Unused, OutError))
	{
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FExportDataTableToCsvAction::ExecuteInternal(
	const TSharedPtr<FJsonObject>& Params,
	FMCPEditorContext& Context)
{
	FString DataTablePath;
	FString CsvPath;
	FString Error;
	GetRequiredString(Params, TEXT("data_table_path"), DataTablePath, Error);
	GetRequiredString(Params, TEXT("csv_path"), CsvPath, Error);

	UDataTable* DataTable = UEEditorMCPDataTable::LoadDataTableAsset(DataTablePath, Error);
	if (!DataTable)
	{
		return CreateErrorResponse(Error, TEXT("data_table_not_found"));
	}

	const FString ResolvedCsv = UEEditorMCPDataTable::ResolveExistingFilePath(CsvPath);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ResolvedCsv), true);

	const bool bExported = UDataTableFunctionLibrary::ExportDataTableToCSVFile(DataTable, ResolvedCsv);
	if (!bExported || !FPaths::FileExists(ResolvedCsv))
	{
		return CreateErrorResponse(
			FString::Printf(TEXT("ExportDataTableToCSVFile failed for %s"), *DataTablePath),
			TEXT("export_failed"));
	}

	const int32 RowCount = DataTable->GetRowNames().Num();
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("data_table_path"), DataTablePath);
	Result->SetStringField(TEXT("csv_path"), ResolvedCsv);
	Result->SetNumberField(TEXT("row_count"), RowCount);
	Result->SetStringField(
		TEXT("row_struct"),
		DataTable->GetRowStruct() ? DataTable->GetRowStruct()->GetPathName() : TEXT(""));

	return CreateSuccessResponse(Result);
}

// ============================================================================
// FImportDataTableFromCsvAction
// ============================================================================

bool FImportDataTableFromCsvAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString Unused;
	if (!GetRequiredString(Params, TEXT("data_table_path"), Unused, OutError))
	{
		return false;
	}
	if (!GetRequiredString(Params, TEXT("csv_path"), Unused, OutError))
	{
		return false;
	}
	if (!GetRequiredString(Params, TEXT("row_struct_path"), Unused, OutError))
	{
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FImportDataTableFromCsvAction::ExecuteInternal(
	const TSharedPtr<FJsonObject>& Params,
	FMCPEditorContext& Context)
{
	FString DataTablePath;
	FString CsvPath;
	FString RowStructPath;
	FString Error;
	GetRequiredString(Params, TEXT("data_table_path"), DataTablePath, Error);
	GetRequiredString(Params, TEXT("csv_path"), CsvPath, Error);
	GetRequiredString(Params, TEXT("row_struct_path"), RowStructPath, Error);

	const bool bPreserveExisting = GetOptionalBool(Params, TEXT("preserve_existing_values"), false);
	const bool bTryAssetImportFallback = GetOptionalBool(Params, TEXT("try_asset_import_fallback"), true);
	const FString VerifyRowName = GetOptionalString(Params, TEXT("verify_row_name"));
	const FString VerifySubstring = GetOptionalString(Params, TEXT("verify_substring"));

	const FString ResolvedCsv = UEEditorMCPDataTable::ResolveExistingFilePath(CsvPath);
	if (!FPaths::FileExists(ResolvedCsv))
	{
		return CreateErrorResponse(
			FString::Printf(TEXT("CSV file not found: %s"), *CsvPath),
			TEXT("csv_not_found"));
	}

	UScriptStruct* RowStruct = UEEditorMCPDataTable::LoadRowStruct(RowStructPath, Error);
	if (!RowStruct)
	{
		return CreateErrorResponse(Error, TEXT("row_struct_not_found"));
	}

	UDataTable* DataTable = UEEditorMCPDataTable::LoadDataTableAsset(DataTablePath, Error);
	if (!DataTable)
	{
		return CreateErrorResponse(Error, TEXT("data_table_not_found"));
	}

	const UScriptStruct* LoadedStruct = DataTable->GetRowStruct();
	if (LoadedStruct && LoadedStruct != RowStruct)
	{
		UE_LOG(LogMCP, Warning,
			TEXT("import_data_table_from_csv: DataTable row_struct=%s differs from requested %s; CSV import uses requested struct."),
			*LoadedStruct->GetPathName(),
			*RowStruct->GetPathName());
	}

	FString ImportMethod = TEXT("fill_csv");
	bool bImported = UEEditorMCPDataTable::ImportViaFillCsv(DataTable, RowStruct, ResolvedCsv, bPreserveExisting);

	if (!bImported && bTryAssetImportFallback)
	{
		const FString PackagePath = DataTable->GetOutermost()->GetName();
		const FString DestinationPath = FPackageName::GetLongPackagePath(PackagePath);
		const FString DestinationName = DataTable->GetName();
		bImported = UEEditorMCPDataTable::ImportViaAssetTools(RowStruct, ResolvedCsv, DestinationPath, DestinationName);
		ImportMethod = TEXT("asset_import_task");
		DataTable = UEEditorMCPDataTable::LoadDataTableAsset(DataTablePath, Error);
	}

	if (!bImported || !DataTable)
	{
		return CreateErrorResponse(
			TEXT("Failed to import CSV into DataTable. Ensure row_struct_path matches the table (e.g. /Script/DestructibleCaveGenerator.TerrainData)."),
			TEXT("import_failed"));
	}

	DataTable->Modify();

	const int32 RowCount = DataTable->GetRowNames().Num();
	bool bVerified = true;
	if (!VerifyRowName.IsEmpty() && !VerifySubstring.IsEmpty())
	{
		bVerified = UEEditorMCPDataTable::VerifyRowContains(DataTable, VerifyRowName, VerifySubstring);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("data_table_path"), DataTablePath);
	Result->SetStringField(TEXT("csv_path"), ResolvedCsv);
	Result->SetStringField(TEXT("row_struct_path"), RowStructPath);
	Result->SetStringField(TEXT("import_method"), ImportMethod);
	Result->SetNumberField(TEXT("row_count"), RowCount);
	Result->SetBoolField(TEXT("verified"), bVerified);

	if (!bVerified)
	{
		Result->SetStringField(
			TEXT("warning"),
			TEXT("Import completed but verify_row_name/verify_substring check failed. Rebuild the plugin defining the row struct if new columns were added."));
	}

	return CreateSuccessResponse(Result);
}
