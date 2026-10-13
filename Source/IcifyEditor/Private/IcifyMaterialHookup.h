// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "IcifyMaterialHookup.generated.h"

class AActor;
class UMaterial;
class UMaterialExpression;
class UMaterialInterface;

/** What happened to one material. */
USTRUCT(BlueprintType)
struct FIcifyHookupResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Icify")
	FString Material;

	/** "added", "removed", "already", "skipped" or "failed". */
	UPROPERTY(BlueprintReadOnly, Category = "Icify")
	FString Outcome;

	/** "legacy" or "attributes" when the material was changed. */
	UPROPERTY(BlueprintReadOnly, Category = "Icify")
	FString Path;

	UPROPERTY(BlueprintReadOnly, Category = "Icify")
	FString Notes;

	UPROPERTY(BlueprintReadOnly, Category = "Icify")
	FString Backup;
};

/**
 * Editor-only ownership record of one hookup installation, stored on the material (and, marking it as a backup, on
 * the backup copy). Remove uses it to touch exactly the nodes this installation inserted (not copies an artist pasted,
 * not nodes found by their description) and to refuse when artist edits have come to depend on those nodes.
 */
UCLASS()
class UIcifyHookupRecord : public UAssetUserData
{
	GENERATED_BODY()

public:
	virtual bool IsEditorOnly() const override { return true; }

	/** Unique per installation, so a re-add after Remove is a new installation. */
	UPROPERTY()
	FGuid InstallId;

	/** "legacy" or "attributes". */
	UPROPERTY()
	FString Path;

	/** Every node this installation inserted (function calls, constants, attribute nodes). */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialExpression>> Nodes;

	/** Material properties the installation routed through its nodes, e.g. BaseColor, MaterialAttributes. */
	UPROPERTY()
	TArray<FString> RoutedProperties;

	/** Backup package made before the first change, and the backup material's state id when it was made. */
	UPROPERTY()
	FString Backup;

	UPROPERTY()
	FGuid BackupStateId;

	/** Set only on backup copies: the material this is a backup of (so an unrelated asset at the path is never trusted). */
	UPROPERTY()
	FString BackupOf;

	UPROPERTY()
	FString InstalledAt;
};

/**
 * Wires Icify into existing master materials, so nobody has to edit material graphs by hand:
 *  - legacy materials: Base Color, Roughness, Metallic and Normal are routed through MF_IcifyFrost;
 *  - "Use Material Attributes" materials: Get/Set Material Attributes nodes route the same four attributes.
 * Substrate and landscape materials aren't supported yet and are reported as skipped. A material that already uses
 * Grungify (or any other function) keeps it: Icify goes after it, so the ice sits on top of the weathering.
 * Inserted nodes are tagged for removal. Legacy dormant constant wiring/unconnected pin metadata and world-space
 * attribute normals are refused before changing the graph. Add/Remove are editor undo transactions. A backup copy
 * must be saved to /Game/Icify/Backups before a first backed-up change; an unsaved duplicate is insufficient.
 * Material instances resolve to their parent material, which changes every instance of it, as intended.
 */
UCLASS()
class UIcifyMaterialHookup : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Icify|Materials", meta = (DevelopmentOnly))
	static FIcifyHookupResult AddIcifyToMaterial(UMaterialInterface* Material, bool bBackup = true);

	UFUNCTION(BlueprintCallable, Category = "Icify|Materials", meta = (DevelopmentOnly))
	static FIcifyHookupResult RemoveIcifyFromMaterial(UMaterialInterface* Material);

	/** True if the material (or its parent) already reads Icify's masks, by hand or through this tool. */
	UFUNCTION(BlueprintPure, Category = "Icify|Materials", meta = (DevelopmentOnly))
	static bool UsesIcify(const UMaterialInterface* Material);

	/** The base materials used by static meshes in the editor level (inside zones, if there are any) that don't use Icify yet. */
	UFUNCTION(BlueprintCallable, Category = "Icify|Materials", meta = (DevelopmentOnly))
	static TArray<UMaterial*> FindUnhookedMaterialsInLevel();

	/** The base materials used by these actors' static meshes that don't use Icify yet. */
	static TArray<UMaterial*> FindUnhookedMaterialsOn(const TArray<AActor*>& Actors);

	/**
	 * The base materials these actors' static meshes use that don't use Icify and can't be hooked up in place, but whose
	 * private copy could be: engine content, such as the basic shapes' material.
	 */
	static TArray<UMaterial*> FindCopyOnlyMaterialsOn(const TArray<AActor*>& Actors);

	/** Whether Icify can be hooked into this base material (editable surface material, not Substrate or landscape). */
	static bool CanHookUp(const UMaterial* Material);

	/** Whether this material can't be edited in place (engine content) but a copy of it could take Icify. */
	static bool NeedsPrivateCopy(const UMaterial* Material);

	/** Hooks up each material (with a backup) behind a progress dialog, then reports the outcome like Hook Up Materials In Level. */
	static void HookUpMaterials(const TArray<UMaterial*>& Materials);

	// Menu actions: run on the Content Browser selection or the level, with a progress bar, notification and report file.
	static void AddToSelectedAssets();
	static void RemoveFromSelectedAssets();
	static void HookUpLevel();
};
