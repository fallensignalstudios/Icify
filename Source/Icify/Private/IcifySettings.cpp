// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifySettings.h"

#include "Materials/MaterialInterface.h"

UIcifySettings::UIcifySettings()
{
	StoneKeywords = { TEXT("stone"), TEXT("rock"), TEXT("brick"), TEXT("concrete"), TEXT("cement"), TEXT("marble"), TEXT("granite"),
		TEXT("cobble"), TEXT("masonry"), TEXT("plaster"), TEXT("stucco"), TEXT("slate"), TEXT("tile"), TEXT("wood"), TEXT("timber"),
		TEXT("plank"), TEXT("bark"), TEXT("log"), TEXT("shingle"), TEXT("thatch") };

	MetalKeywords = { TEXT("metal"), TEXT("steel"), TEXT("iron"), TEXT("copper"), TEXT("brass"), TEXT("bronze"), TEXT("alum"),
		TEXT("chrome"), TEXT("zinc"), TEXT("rust"), TEXT("galvan"), TEXT("corrugated"), TEXT("pipe"), TEXT("rail") };

	// Prefix matches: "pane" would also catch "panel", so only whole-word-safe keywords are listed.
	GlassKeywords = { TEXT("glass"), TEXT("window"), TEXT("windshield"), TEXT("windscreen"), TEXT("glazing"), TEXT("mirror"),
		TEXT("skylight") };

	NotGlassKeywords = { TEXT("frame"), TEXT("sill"), TEXT("ledge"), TEXT("lintel"), TEXT("shutter"), TEXT("sash"), TEXT("mullion"),
		TEXT("trim"), TEXT("casing"), TEXT("surround"), TEXT("grille") };

	DripKeywords = { TEXT("gutter"), TEXT("downpipe"), TEXT("downspout"), TEXT("drainpipe"), TEXT("spout"), TEXT("drain"),
		TEXT("overflow"), TEXT("scupper"), TEXT("vent"), TEXT("faucet") };

	// Lanterns and candles are left out on purpose: winter scenes are full of them, and they barely melt anything. So is a
	// bare "fire", which would also catch FireHydrant, FireEscape and Firewood; tag other fires with Icify.Heat.
	HeatKeywords = { TEXT("bonfire"), TEXT("campfire"), TEXT("fireplace"), TEXT("firepit"), TEXT("torch"), TEXT("brazier"),
		TEXT("furnace"), TEXT("forge"), TEXT("hearth"), TEXT("stove"), TEXT("heater"), TEXT("radiator"), TEXT("boiler"), TEXT("chimney"),
		TEXT("exhaust") };

	MaskDebugMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Icify/Materials/M_IcifyMaskDebug.M_IcifyMaskDebug")));
	IcicleMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Icify/Materials/M_IcifyIcicle.M_IcifyIcicle")));
}
