// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "IcifyTypes.h"
#include "IcifyPreset.generated.h"

class UMaterialInterface;

/**
 * A saved ice look: layer settings, icicle settings, surface profiles, named material profiles and the palette.
 * Lives in the project's content (not the plugin's generated content), so it survives plugin-content rebuilds,
 * regeneration and Clear. Zones copy it in with Load Preset (or by assigning it) and write it back with Save To Preset.
 *
 * Scene-specific settings (wind direction, melt and heat sources, actors, slot rules, filtering, budgets) stay on the zone.
 * Field names match AIcifyFreezeZone's; see ICIFY_PRESET_FIELDS.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Icify Preset"))
class ICIFY_API UIcifyPreset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preset", meta = (MultiLine = true))
	FString Notes;

	/** Loading also sets the zone's seed. Turn off to keep each zone's own seed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preset")
	bool bIncludeSeed = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preset", meta = (EditCondition = "bIncludeSeed"))
	int32 Seed = 1337;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General", meta = (ClampMin = "0", UIMax = "2"))
	float Intensity = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General", meta = (ClampMin = "1", UIMax = "200", Units = "cm"))
	float FeatureScaleCm = 20.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General", meta = (ClampMin = "0.25", UIMax = "20", Units = "cm"))
	float MaskTexelSizeCm = 4.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather", meta = (ClampMin = "0", ClampMax = "1"))
	float WindInfluence = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather", meta = (ClampMin = "0", ClampMax = "60", Units = "deg"))
	float SnowfallSlantDegrees = 15.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weather")
	bool bUseSkyCover = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frost", meta = (ClampMin = "0", UIMax = "2"))
	float FrostIntensity = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frost", meta = (ClampMin = "0", ClampMax = "1"))
	float FrostFilm = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frost", meta = (ClampMin = "0", ClampMax = "1"))
	float EdgeFrost = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frost", meta = (ClampMin = "0", ClampMax = "1"))
	float Rime = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frost", meta = (ClampMin = "0", ClampMax = "1"))
	float GroundFrost = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frost", meta = (ClampMin = "0", UIMax = "300", Units = "cm"))
	float GroundFrostHeightCm = 40.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frost", meta = (ClampMin = "5", ClampMax = "170", Units = "deg"))
	float EdgeAngleDegrees = 45.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frost", meta = (ClampMin = "0.5", UIMax = "50", Units = "cm"))
	float FrostEdgeWidthCm = 6.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow", meta = (ClampMin = "0", UIMax = "2"))
	float SnowIntensity = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow", meta = (ClampMin = "0", ClampMax = "89", Units = "deg"))
	float SnowMaxSlopeDegrees = 50.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow", meta = (ClampMin = "0", ClampMax = "1"))
	float CreviceSnow = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow", meta = (ClampMin = "0", ClampMax = "1"))
	float WallPlaster = 0.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow", meta = (ClampMin = "0", ClampMax = "1"))
	float EdgeSnowLoss = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Glaze & Drips", meta = (ClampMin = "0", UIMax = "2"))
	float GlazeIntensity = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Glaze & Drips", meta = (ClampMin = "0", ClampMax = "1"))
	float GlazeCoat = 0.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Glaze & Drips", meta = (ClampMin = "0", ClampMax = "1"))
	float AmbientDrips = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Glaze & Drips", meta = (ClampMin = "10", UIMax = "1000", Units = "cm"))
	float DripLengthCm = 150.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Glaze & Drips", meta = (ClampMin = "0", UIMax = "2"))
	float DripSpread = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Glaze & Drips")
	bool bTraceMeltwater = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Black Ice", meta = (ClampMin = "0", UIMax = "2"))
	float BlackIceIntensity = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Black Ice", meta = (ClampMin = "0", ClampMax = "1"))
	float FloorIce = 0.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Black Ice", meta = (ClampMin = "0", ClampMax = "1"))
	float CreviceIce = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Black Ice", meta = (ClampMin = "0", UIMax = "300", Units = "cm"))
	float PuddleRadiusCm = 60.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icicles")
	bool bGrowIcicles = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icicles", meta = (ClampMin = "0", UIMax = "40"))
	float IcicleDensity = 8.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icicles", meta = (ClampMin = "1", UIMax = "300", Units = "cm"))
	float IcicleMaxLengthCm = 45.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icicles", meta = (ClampMin = "0.5", UIMax = "50", Units = "cm"))
	float IcicleMinLengthCm = 3.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icicles", meta = (ClampMin = "0.1", UIMax = "3"))
	float IcicleThickness = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icicles", meta = (ClampMin = "0", UIMax = "5", Units = "cm"))
	float IcicleRippleCm = 1.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icicles", meta = (ClampMin = "0", ClampMax = "1"))
	float IcicleClumping = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icicles", meta = (ClampMin = "0", ClampMax = "1"))
	float IcicleLean = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icicles", meta = (ClampMin = "0", ClampMax = "80", Units = "deg"))
	float DripOverhangDegrees = 20.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icicles")
	bool bIciclesNeedMeltwater = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icicles", meta = (ClampMin = "4", ClampMax = "16"))
	int32 IcicleSides = 8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icicles")
	TObjectPtr<UMaterialInterface> IcicleMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thaw", meta = (ClampMin = "0", ClampMax = "1"))
	float ThawIntensity = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thaw", meta = (ClampMin = "10", UIMax = "1000", Units = "cm"))
	float ThawRadiusCm = 150.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfaces")
	FIcifySurfaceProfile StoneProfile = FIcifySurfaceProfile(1.0f, 1.0f, 1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfaces")
	FIcifySurfaceProfile MetalProfile = FIcifySurfaceProfile(1.3f, 0.9f, 1.2f, 1.0f, 1.2f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfaces")
	FIcifySurfaceProfile GlassProfile = FIcifySurfaceProfile(1.1f, 0.8f, 1.0f, 1.0f, 0.6f);

	/** Named profiles slot rules can refer to. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfaces")
	TMap<FName, FIcifySurfaceProfile> MaterialProfiles;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	FIcifyPalette Palette;
};

/** Every field shared by UIcifyPreset and AIcifyFreezeZone (Seed is handled separately). */
#define ICIFY_PRESET_FIELDS(X) \
	X(Intensity) X(FeatureScaleCm) X(MaskTexelSizeCm) X(WindInfluence) X(SnowfallSlantDegrees) X(bUseSkyCover) \
	X(FrostIntensity) X(FrostFilm) X(EdgeFrost) X(Rime) X(GroundFrost) X(GroundFrostHeightCm) X(EdgeAngleDegrees) X(FrostEdgeWidthCm) \
	X(SnowIntensity) X(SnowMaxSlopeDegrees) X(CreviceSnow) X(WallPlaster) X(EdgeSnowLoss) \
	X(GlazeIntensity) X(GlazeCoat) X(AmbientDrips) X(DripLengthCm) X(DripSpread) X(bTraceMeltwater) \
	X(BlackIceIntensity) X(FloorIce) X(CreviceIce) X(PuddleRadiusCm) \
	X(bGrowIcicles) X(IcicleDensity) X(IcicleMaxLengthCm) X(IcicleMinLengthCm) X(IcicleThickness) X(IcicleRippleCm) \
	X(IcicleClumping) X(IcicleLean) X(DripOverhangDegrees) X(bIciclesNeedMeltwater) X(IcicleSides) X(IcicleMaterial) \
	X(ThawIntensity) X(ThawRadiusCm) \
	X(StoneProfile) X(MetalProfile) X(GlassProfile) X(MaterialProfiles) X(Palette)
