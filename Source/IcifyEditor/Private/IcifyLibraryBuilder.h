// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UIcifyPreset;

/**
 * Starter looks in /Game/Icify/Library: a preset per look (Light Frost, Deep Winter, Ice Storm, Blizzard, Thaw) and
 * stone, metal and glass material instances of M_IcifyExample to try them on. Project content, so it is never
 * touched by plugin-content rebuilds. Existing assets are kept unless bOverwrite.
 */
struct FIcifyLibraryBuilder
{
	static bool Build(bool bOverwrite, FString& OutMessage);

	/** The looks Build creates, by name (LightFrost, DeepWinter, IceStorm, Blizzard, Thaw). */
	static TArray<FString> LookNames();

	/**
	 * Resets Preset to the class defaults and writes LookName's settings and notes into it (bIncludeSeed off), exactly as
	 * Build does for IP_<LookName>. Does not mark the package dirty. False (Preset untouched) if the name is unknown.
	 */
	static bool ConfigureLook(const FString& LookName, UIcifyPreset& Preset);
};
