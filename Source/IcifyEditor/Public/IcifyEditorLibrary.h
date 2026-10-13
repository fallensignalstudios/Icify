// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IcifyTypes.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "IcifyEditorLibrary.generated.h"

class AActor;
class AIcifyFreezeZone;
class UIcifyPreset;

/**
 * Editor-only entry points for the same explicit authoring actions the Tools > Icify menu offers, for Editor Utility
 * Blueprints and Python:
 *
 *     import unreal
 *     lib = unreal.IcifyEditorLibrary
 *     ok, report = lib.build_plugin_content(False)
 *     actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_selected_level_actors()
 *     zone, message = lib.freeze_actors(actors, None, True)
 */
UCLASS()
class ICIFYEDITOR_API UIcifyEditorLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Creates missing plugin assets and rebuilds outdated ones (all of them when bForceRebuild). True when the content is ready. */
	UFUNCTION(BlueprintCallable, Category = "Icify|Editor")
	static bool BuildPluginContent(bool bForceRebuild, FString& OutReport);

	/** Builds the starter presets and material instances in /Game/Icify/Library. */
	UFUNCTION(BlueprintCallable, Category = "Icify|Editor")
	static bool BuildPresetLibrary(bool bOverwrite, FString& OutMessage);

	/** The starter looks Build Preset Library creates (LightFrost, DeepWinter, IceStorm, Blizzard, Thaw). */
	UFUNCTION(BlueprintPure, Category = "Icify|Editor")
	static TArray<FString> GetLookNames();

	/** Writes one of the starter looks into Preset (resetting everything else to the defaults). False if the name is unknown. */
	UFUNCTION(BlueprintCallable, Category = "Icify|Editor")
	static bool ConfigurePresetLook(UIcifyPreset* Preset, const FString& LookName);

	/**
	 * Freeze Selected Actors without the dialogs: spawns a freeze zone fitted to Actors (and limited to them) in the
	 * editor world, loads Preset if set, and previews it when bPreview is set. Doesn't hook up materials (use
	 * UIcifyMaterialHookup for that). Null, with the reason, when none of the actors has a static mesh.
	 */
	UFUNCTION(BlueprintCallable, Category = "Icify|Editor")
	static AIcifyFreezeZone* FreezeActors(const TArray<AActor*>& Actors, UIcifyPreset* Preset, bool bPreview, FString& OutMessage);
};
