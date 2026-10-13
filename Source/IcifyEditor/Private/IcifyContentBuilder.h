// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** What a content build did, and whether everything connected, compiled and saved. */
struct FIcifyContentReport
{
	TArray<FString> Created;
	TArray<FString> Rebuilt;
	TArray<FString> Errors;
	TArray<FString> Notes;
	bool bSaveAttempted = false;
	bool bSaved = true;
	bool bReady = false;

	bool HasErrors() const { return Errors.Num() > 0 || !bSaved || !bReady; }
	FString ToString() const;
};

/**
 * Creates and maintains the plugin's assets in /Icify:
 *   BP_FreezeZone, MF_IcifyFrost, M_IcifyMaskDebug, M_IcifyExample, M_IcifyIcicle, T_IcifyNoMask.
 * The generated material assets carry a version marker (content version, custom data index, mask UV channel).
 * Readiness requires graph validation, a compile of every generated material and saving all generated packages.
 * A Saved readiness token binds the marker, shader include and saved package bytes. When it doesn't match, they are
 * rebuilt in place, so every material that already uses MF_IcifyFrost picks up fixes and settings changes without
 * relinking.
 */
struct FIcifyContentBuilder
{
	/** Creates missing assets and rebuilds outdated ones (or all generated ones when bForceRebuild). */
	static FIcifyContentReport Run(bool bForceRebuild);

	static bool NeedsWork();
};
