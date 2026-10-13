// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class AIcifyFreezeZone;
class UIcifyPreset;
class UWorld;

/** Freeze Selected Actors: one step from a selection to a previewed, correctly sized freeze zone. */
struct FIcifyQuickStart
{
	/**
	 * The zone box for a selection's bounds: padded on every side past the zone's falloff, so the selection freezes
	 * fully, and further down so the floor under it is traced.
	 */
	static FBox ZoneBoxFor(const FBox& Bounds);

	/**
	 * Spawns a freeze zone fitted to Actors, limited to exactly those actors, loads Preset (if any) and optionally
	 * previews it. One undo step. Returns null (and why, in OutMessage) when none of the actors has a static mesh.
	 */
	static AIcifyFreezeZone* FreezeActors(UWorld* World, const TArray<AActor*>& Actors, UIcifyPreset* Preset, bool bPreview, FString& OutMessage);

	/** The menu action: the level selection, an offer to hook up its materials, then FreezeActors and select the zone. */
	static void FreezeSelected();
};
