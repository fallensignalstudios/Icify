// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IcifyTypes.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "IcifyBlueprintLibrary.generated.h"

class AIcifyFreezeZone;
class UIcifyPreset;
class UPrimitiveComponent;

/**
 * Scripting access to freeze zones from Blueprint (Editor Utility Widgets / Blueprints) and Python.
 *
 * Every function is safe to call in any build: the zone actions are editor-only, so outside the editor they do
 * nothing and the run functions return the zone's existing (usually empty) report.
 *
 * Blueprint: Get Zones -> For Each -> Run Zone (Operation = Dry Run) -> Report To Json -> Print String.
 *
 * Python (editor):
 *
 *     import unreal
 *     world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
 *     lib = unreal.IcifyBlueprintLibrary
 *     for report in lib.run_all_zones(world, unreal.IcifyOperation.DRY_RUN):
 *         unreal.log("{}: {} frozen, {} icicles".format(report.zone, report.frozen, report.icicle_stats.icicles))
 *
 *     zone = lib.find_zone_by_label(world, "Courtyard")
 *     lib.set_zone_seed(zone, 42, True)
 *     report = lib.run_zone(zone, unreal.IcifyOperation.APPLY)
 *     lib.save_report(report, unreal.Paths.project_saved_dir() + "Icify/Courtyard.json")
 */

UENUM(BlueprintType)
enum class EIcifyOperation : uint8
{
	DryRun,
	Preview,
	Apply,
	Clear,
	CancelPreview
};

UCLASS()
class ICIFY_API UIcifyBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Every freeze zone in the world, including Blueprint subclasses (only loaded actors in World Partition levels). */
	UFUNCTION(BlueprintCallable, Category = "Icify", meta = (WorldContext = "WorldContextObject"))
	static TArray<AIcifyFreezeZone*> GetZones(const UObject* WorldContextObject);

	/** First zone whose label (or object name outside the editor) matches, ignoring case. Null if none. */
	UFUNCTION(BlueprintCallable, Category = "Icify", meta = (WorldContext = "WorldContextObject"))
	static AIcifyFreezeZone* FindZoneByLabel(const UObject* WorldContextObject, const FString& Label);

	/** Runs one action on the zone and returns its Last Report. Clear and Cancel Preview don't make a report, so the previous one is returned. */
	UFUNCTION(BlueprintCallable, Category = "Icify")
	static FIcifyRunReport RunZone(AIcifyFreezeZone* Zone, EIcifyOperation Operation);

	/** Runs the action on every zone (one undo step in the editor) and returns one report per zone. */
	UFUNCTION(BlueprintCallable, Category = "Icify", meta = (WorldContext = "WorldContextObject"))
	static TArray<FIcifyRunReport> RunAllZones(const UObject* WorldContextObject, EIcifyOperation Operation);

	/** Spawns a native freeze zone. If Preset is set it is assigned and loaded (editor only). */
	UFUNCTION(BlueprintCallable, Category = "Icify", meta = (WorldContext = "WorldContextObject"))
	static AIcifyFreezeZone* SpawnZone(const UObject* WorldContextObject, const FTransform& Transform, FVector HalfExtent, UIcifyPreset* Preset);

	/** The report as JSON, in the same format as the files in Saved/Icify/Reports. Empty on failure. */
	UFUNCTION(BlueprintPure, Category = "Icify")
	static FString ReportToJson(const FIcifyRunReport& Report);

	/** Writes the report as JSON to FilePath (relative paths are relative to the project directory), creating folders as needed. */
	UFUNCTION(BlueprintCallable, Category = "Icify")
	static bool SaveReport(const FIcifyRunReport& Report, const FString& FilePath);

	/** Zones whose box bounds intersect the component's bounds. */
	UFUNCTION(BlueprintCallable, Category = "Icify")
	static TArray<AIcifyFreezeZone*> GetZonesAffecting(const UPrimitiveComponent* Component);

	/** Sets the zone's seed, optionally refreshing its preview (editor only). */
	UFUNCTION(BlueprintCallable, Category = "Icify")
	static void SetZoneSeed(AIcifyFreezeZone* Zone, int32 Seed, bool bPreview);
};
