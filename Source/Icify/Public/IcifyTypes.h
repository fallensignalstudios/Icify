// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/SoftObjectPtr.h"
#include "IcifyTypes.generated.h"

class AActor;
class UMaterialInterface;
class UMeshComponent;
class UStaticMesh;
class UTexture2D;

ICIFY_API DECLARE_LOG_CATEGORY_EXTERN(LogIcify, Log, All);

/** What a surface is made of. Picks the shading family in MF_IcifyFrost and the default ice profile. */
UENUM(BlueprintType)
enum class EIcifySurfaceType : uint8
{
	Auto UMETA(ToolTip = "Classify each material slot from rules, tags, physical material and asset names."),
	Stone UMETA(ToolTip = "Stone, concrete, brick, wood, plaster and anything else ordinary: frost, snow and ice evenly."),
	Metal UMETA(ToolTip = "Metal conducts the cold: frost and rime form first and glaze sheets form on it."),
	Glass UMETA(ToolTip = "Glass grows fern frost, and snow slides off anything steep.")
};

/** What the meshes show while a preview is active. */
UENUM(BlueprintType)
enum class EIcifyPreviewDisplay : uint8
{
	FinalMaterial UMETA(DisplayName = "Final Material", ToolTip = "Show the result through the meshes' own materials (they must use MF_IcifyFrost)."),
	AllMasks UMETA(DisplayName = "All Masks", ToolTip = "Swap to the mask debug material: red = frost, green = snow, blue = glaze, yellow = black ice."),
	Frost UMETA(DisplayName = "Frost Mask"),
	Snow UMETA(DisplayName = "Snow Mask"),
	Glaze UMETA(DisplayName = "Glaze Mask"),
	BlackIce UMETA(DisplayName = "Black Ice Mask")
};

/** How the zone finds the floor that ground frost and black ice are measured from. */
UENUM(BlueprintType)
enum class EIcifyFloorMode : uint8
{
	Trace UMETA(DisplayName = "Trace Floors", ToolTip = "Trace down through the zone to find walkable floors, including multiple storeys and stairs."),
	ZoneBottom UMETA(DisplayName = "Zone Bottom", ToolTip = "Treat the bottom face of the zone box as the floor.")
};

/** Where the masks are stored. */
UENUM(BlueprintType)
enum class EIcifyMaskOutput : uint8
{
	Auto UMETA(ToolTip = "Texture for Nanite meshes and sparse geometry (when the mesh has the mask UV channel), vertex colours otherwise."),
	VertexColor UMETA(DisplayName = "Vertex Color", ToolTip = "Per-instance override vertex colours. Detail limited by vertex density; may not render on Nanite."),
	Texture UMETA(ToolTip = "Per-instance mask texture in the mask UV channel, bound through a material instance. Works on Nanite and sparse geometry.")
};

/** What a zone does with meshes another zone has already frozen. */
UENUM(BlueprintType)
enum class EIcifyOverlapPolicy : uint8
{
	SkipOwned UMETA(ToolTip = "Leave meshes owned by another zone alone and report them."),
	TakeOver UMETA(ToolTip = "Apply takes ownership (and the original backup) from the other zone.")
};

/** Whether a slot's drip edges may grow icicles. */
UENUM(BlueprintType)
enum class EIcifyIcicleMode : uint8
{
	Inherit UMETA(ToolTip = "Grow icicles where the zone finds drip edges, unless the mesh or actor carries the Icify.NoIcicles tag."),
	Grow UMETA(ToolTip = "Consider this slot's drip edges even when the mesh carries the Icify.NoIcicles tag."),
	Never UMETA(ToolTip = "Never grow icicles from this slot.")
};

UENUM(BlueprintType)
enum class EIcifyChannel : uint8
{
	R,
	G,
	B,
	A
};

/** The four ice layers. They live in vertex colour or the mask texture: R = frost, G = snow, B = glaze, A = black ice. */
UENUM(BlueprintType)
enum class EIcifyLayer : uint8
{
	Frost UMETA(ToolTip = "Hoarfrost and rime: exposed faces, convex edges, and faces and edges into the wind."),
	Snow UMETA(ToolTip = "Snow wherever it can fall and settle: up-facing, open to the sky, below the slope limit."),
	Glaze UMETA(ToolTip = "Clear ice: a freezing-rain coat on exposed faces and frozen meltwater drips."),
	BlackIce UMETA(DisplayName = "Black Ice", ToolTip = "Smooth dark ice on floors: thin sheets and frozen puddles.")
};

/** Per-surface multipliers for each ice layer and for icicles. */
USTRUCT(BlueprintType)
struct ICIFY_API FIcifySurfaceProfile
{
	GENERATED_BODY()

	FIcifySurfaceProfile() = default;
	FIcifySurfaceProfile(float InFrost, float InSnow, float InGlaze, float InBlackIce, float InIcicles)
		: Frost(InFrost), Snow(InSnow), Glaze(InGlaze), BlackIce(InBlackIce), Icicles(InIcicles)
	{
	}

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (ClampMin = "0", UIMax = "2"))
	float Frost = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (ClampMin = "0", UIMax = "2"))
	float Snow = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (ClampMin = "0", UIMax = "2"))
	float Glaze = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (ClampMin = "0", UIMax = "2", DisplayName = "Black Ice"))
	float BlackIce = 1.f;

	/** How readily this surface's drip edges grow icicles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (ClampMin = "0", UIMax = "2"))
	float Icicles = 1.f;
};

/** Optional limits on where a slot rule lets ice appear. All enabled gates multiply together. */
USTRUCT(BlueprintType)
struct ICIFY_API FIcifyGate
{
	GENERATED_BODY()

	/** Only faces whose world normal Z is within the range (1 = facing up, -1 = facing down). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify")
	bool bUseFacingGate = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (EditCondition = "bUseFacingGate", ClampMin = "-1", ClampMax = "1"))
	float MinUpFacing = -1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (EditCondition = "bUseFacingGate", ClampMin = "-1", ClampMax = "1"))
	float MaxUpFacing = 1.f;

	/** Only between these heights above the floor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify")
	bool bUseHeightGate = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (EditCondition = "bUseHeightGate", Units = "cm"))
	float MinHeightCm = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (EditCondition = "bUseHeightGate", Units = "cm"))
	float MaxHeightCm = 10000.f;

	/** Painted gate in mesh UV space: white = ice allowed, black = none. Source must be 8-bit (BGRA8 or G8). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify")
	TObjectPtr<UTexture2D> GateTexture;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (ClampMin = "0", ClampMax = "7"))
	int32 GateUVChannel = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify")
	EIcifyChannel GateChannel = EIcifyChannel::R;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify")
	bool bInvertGate = false;
};

/**
 * Targets material slots precisely. The first enabled rule that matches a slot decides it.
 * A rule needs at least one of Actor, Name Pattern, Material, Component Name or Slot Index.
 */
USTRUCT(BlueprintType)
struct ICIFY_API FIcifySlotRule
{
	GENERATED_BODY()

	/** Shown in reports, e.g. "Windows", "Bridge deck". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify")
	FString Label;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify")
	bool bEnabled = true;

	/** Only slots on this actor. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Icify|Match")
	TSoftObjectPtr<AActor> Actor;

	/** Wildcard (e.g. "*Window*") tested against the slot name, material names (and parents), mesh, component and actor label. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Match")
	FString NamePattern;

	/** Matches the slot's material or any material it derives from. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Match")
	TSoftObjectPtr<UMaterialInterface> Material;

	/** Only the component with this name on the matched actor (None = any component). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Match")
	FName ComponentName;

	/** Only this material slot index (-1 = any). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Match", meta = (ClampMin = "-1"))
	int32 SlotIndex = -1;

	/** Leave matching slots completely untouched (no ice and no icicles). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Effect")
	bool bExclude = false;

	/** Shading family for matching slots (Auto = classify as usual). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Effect", meta = (EditCondition = "!bExclude"))
	EIcifySurfaceType Surface = EIcifySurfaceType::Auto;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Effect", meta = (InlineEditConditionToggle))
	bool bOverrideProfile = false;

	/** Independent ice profile for matching slots. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Effect", meta = (EditCondition = "bOverrideProfile"))
	FIcifySurfaceProfile Profile;

	/** Use a named profile from the zone's Material Profiles (saved in presets). Ignored when Override Profile is on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Effect", meta = (EditCondition = "!bExclude && !bOverrideProfile"))
	FName ProfileName;

	/** Whether matching slots grow icicles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Effect", meta = (EditCondition = "!bExclude"))
	EIcifyIcicleMode Icicles = EIcifyIcicleMode::Inherit;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Effect", meta = (EditCondition = "!bExclude"))
	FIcifyGate Gate;
};

/**
 * Shader-side look written onto every material instance the zone owns, on every Preview and Apply.
 * Only fields whose override is ticked are written; the rest inherit from the mesh's own material.
 * Icicle Tint and Frost Color are also written onto the zone's icicle material instance.
 */
USTRUCT(BlueprintType)
struct ICIFY_API FIcifyPalette
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (InlineEditConditionToggle))
	bool bOverrideFrostColor = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (EditCondition = "bOverrideFrostColor"))
	FLinearColor FrostColor = FLinearColor(0.86f, 0.92f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (InlineEditConditionToggle))
	bool bOverrideSnowColor = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (EditCondition = "bOverrideSnowColor"))
	FLinearColor SnowColor = FLinearColor(0.93f, 0.95f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (InlineEditConditionToggle))
	bool bOverrideIceTint = false;

	/** The colour clear ice and icicles take on with depth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (EditCondition = "bOverrideIceTint"))
	FLinearColor IceTint = FLinearColor(0.55f, 0.78f, 0.95f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (InlineEditConditionToggle))
	bool bOverrideStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (EditCondition = "bOverrideStrength", ClampMin = "0", UIMax = "2"))
	float Strength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (InlineEditConditionToggle))
	bool bOverrideContrast = false;

	/** How boldly faint ice shows. 1 is standard; below 1 is subtler, above 1 lifts light frost so more of it reads. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (EditCondition = "bOverrideContrast", ClampMin = "0.2", UIMax = "3"))
	float Contrast = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (InlineEditConditionToggle))
	bool bOverrideFrostStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (EditCondition = "bOverrideFrostStrength", ClampMin = "0", UIMax = "2"))
	float FrostStrength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (InlineEditConditionToggle))
	bool bOverrideSnowStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (EditCondition = "bOverrideSnowStrength", ClampMin = "0", UIMax = "2"))
	float SnowStrength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (InlineEditConditionToggle))
	bool bOverrideGlazeStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (EditCondition = "bOverrideGlazeStrength", ClampMin = "0", UIMax = "2"))
	float GlazeStrength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (InlineEditConditionToggle))
	bool bOverrideBlackIceStrength = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (EditCondition = "bOverrideBlackIceStrength", ClampMin = "0", UIMax = "2", DisplayName = "Black Ice Strength"))
	float BlackIceStrength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (InlineEditConditionToggle))
	bool bOverrideReliefStrength = false;

	/**
	 * Relief: the bumps snow, frost crystals and frozen drips add to the normal. Snow and ice smooth the surface
	 * under them at any value; 0 adds no bumps of its own.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (EditCondition = "bOverrideReliefStrength", ClampMin = "0", UIMax = "2"))
	float ReliefStrength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Art Direction", meta = (InlineEditConditionToggle))
	bool bOverrideSparkle = false;

	/** Glints in snow and frost: tiny mirror-smooth facets that catch the light. 0 = none. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Art Direction", meta = (EditCondition = "bOverrideSparkle", ClampMin = "0", ClampMax = "1"))
	float Sparkle = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Art Direction", meta = (InlineEditConditionToggle))
	bool bOverrideFrostCrystalCm = false;

	/** Size of frost crystals in cm, independent of Feature Scale. 0 = a sixth of the zone's Feature Scale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Art Direction", meta = (EditCondition = "bOverrideFrostCrystalCm", ClampMin = "0", UIMax = "10", Units = "cm"))
	float FrostCrystalCm = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Art Direction", meta = (InlineEditConditionToggle))
	bool bOverrideFernFrost = false;

	/** Fern-like frost feathers on glass. 0 = plain granular frost like every other surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Art Direction", meta = (EditCondition = "bOverrideFernFrost", ClampMin = "0", ClampMax = "1"))
	float FernFrost = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Art Direction", meta = (InlineEditConditionToggle))
	bool bOverrideSnowBreakup = false;

	/** Breaks snow cover into drifts and bare patches at its edges. 0 = an even blanket. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Art Direction", meta = (EditCondition = "bOverrideSnowBreakup", ClampMin = "0", ClampMax = "1"))
	float SnowBreakup = 0.5f;
};

/** How much a freeze stamp adds (positive) or erases (negative) on each layer, and icicle growth inside it. */
USTRUCT(BlueprintType)
struct ICIFY_API FIcifyStampLayers
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stamp", meta = (UIMin = "-1", UIMax = "1"))
	float Frost = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stamp", meta = (UIMin = "-1", UIMax = "1"))
	float Snow = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stamp", meta = (UIMin = "-1", UIMax = "1"))
	float Glaze = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stamp", meta = (UIMin = "-1", UIMax = "1", DisplayName = "Black Ice"))
	float BlackIce = 0.f;

	/** Positive grows more and longer icicles on drip edges inside the stamp; -1 removes them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stamp", meta = (UIMin = "-1", UIMax = "1"))
	float Icicles = 0.f;
};

/** A hand-placed meltwater source: frozen drips run down from it, icicles hang near it, and black ice forms below. */
USTRUCT(BlueprintType)
struct ICIFY_API FIcifyMeltSource
{
	GENERATED_BODY()

	/** Relative to the zone. Drag the diamond widget in the viewport. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (MakeEditWidget))
	FVector Location = FVector(0.0, 0.0, 150.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (ClampMin = "1", UIMax = "200", Units = "cm"))
	float RadiusCm = 10.f;

	/** 0 = use the zone's Drip Length. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (ClampMin = "0", UIMax = "1000", Units = "cm"))
	float LengthCm = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (ClampMin = "0", UIMax = "2"))
	float Strength = 1.f;
};

/** Mirror of the engine's mesh-paint cache entry, so it can be backed up and saved. */
USTRUCT()
struct ICIFY_API FIcifyPaintedVertex
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Position = FVector::ZeroVector;

	UPROPERTY()
	FVector4 Normal = FVector4(0.0, 0.0, 0.0, 0.0);

	UPROPERTY()
	FColor Color = FColor(0, 0, 0, 0);
};

/** One LOD's vertex colour state before a zone touched it. */
USTRUCT()
struct ICIFY_API FIcifyLODBackup
{
	GENERATED_BODY()

	UPROPERTY()
	bool bHadOverride = false;

	UPROPERTY()
	TArray<FColor> Colors;

	UPROPERTY()
	TArray<FIcifyPaintedVertex> PaintedVertices;
};

/** Everything needed to put a component back exactly the way it was. */
USTRUCT()
struct ICIFY_API FIcifyComponentRecord
{
	GENERATED_BODY()

	UPROPERTY()
	TSoftObjectPtr<UMeshComponent> Component;

	/** Refuse destructive restoration after the component's mesh has been replaced. */
	UPROPERTY()
	TSoftObjectPtr<UStaticMesh> StaticMesh;

	/** LODData.Num() before the zone touched it. */
	UPROPERTY()
	int32 LODDataCount = 0;

	UPROPERTY()
	TArray<FIcifyLODBackup> LODs;

	UPROPERTY()
	FString StaticMeshDerivedDataKey;

	/** The complete default custom primitive data array. */
	UPROPERTY()
	TArray<float> CustomData;

	/** The complete live array; it can differ from the saved defaults. */
	UPROPERTY()
	TArray<float> LiveCustomData;

	/** Custom data base when this record was captured; restoration never consults current settings. */
	UPROPERTY()
	int32 CustomDataBase = INDEX_NONE;

	/** The complete override material array, including its length. */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> OverrideMaterials;
};

/** How much of a slot's physical surface one mask channel covers (baked masks, before the shader's noise and thresholds). */
USTRUCT(BlueprintType)
struct ICIFY_API FIcifyChannelCoverage
{
	GENERATED_BODY()

	/** Frost, Snow, Glaze or Black Ice. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Channel;

	/** Surface area where the mask is above the threshold (0.1). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	float CoveredM2 = 0.f;

	/** CoveredM2 as a fraction of the slot's area. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	float CoveredFraction = 0.f;

	/** Mean mask value over the covered area only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	float MeanWhereCovered = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	float MaxValue = 0.f;
};

/** Why one material slot was or wasn't frozen, and how it was classified. */
USTRUCT(BlueprintType)
struct ICIFY_API FIcifySlotReport
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Slot = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString SlotName;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Material;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	bool bIncluded = true;

	/** Plain-language reason it was included or excluded. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Reason;

	/** Matched slot rule (label and what matched), or empty. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Rule;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Surface;

	/** Where the surface came from: rule, tag, physical material, keyword (and which), zone override or fallback. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString SurfaceSource;

	/** Which ice profile was used. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Profile;

	/** Whether the slot's drip edges may grow icicles, and why. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	bool bIcicles = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString IcicleSource;

	/** Active gates (facing, height, texture) in readable form. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Gates;

	/** Physical area of this slot (world space, at the component's scale). 0 when coverage wasn't measured. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	float AreaM2 = 0.f;

	/** Per channel, how much of this slot's area the baked masks cover. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	TArray<FIcifyChannelCoverage> Coverage;

	/** Problems specific to this slot (e.g. a gate UV channel the mesh doesn't have). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	TArray<FString> Warnings;
};

/** Checks on the UV channel the mask texture is baked into. */
USTRUCT(BlueprintType)
struct ICIFY_API FIcifyUVReport
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Channel = 0;

	/** False when the check did not run (vertex output requested, no readable mesh). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	bool bChecked = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	bool bPresent = false;

	/** "ok", "warning" or "failed". */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Verdict;

	/** Share of the mesh's UV area that lands outside 0..1. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	float OutOfRange = 0.f;

	/** Share of covered texels claimed by more than one triangle (overlapping islands). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	float Overlap = 0.f;

	/** Share of triangles with zero UV area. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	float Degenerate = 0.f;

	/** Share of the 0..1 square the UVs cover. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	float Coverage = 0.f;

	/** Ratio between the most and least stretched triangles (world area per UV area); 1 = uniform texel density. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	float DensitySpread = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	TArray<FString> Problems;
};

/** Wall-clock time spent in one phase of a run. */
USTRUCT(BlueprintType)
struct ICIFY_API FIcifyPhaseTiming
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Phase;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	float Seconds = 0.f;

	/** Items processed (floor cells, sky cells, meshes, icicles...). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Items = 0;
};

/** What happened to one component in a run. */
USTRUCT(BlueprintType)
struct ICIFY_API FIcifyComponentReport
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Actor;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Component;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Mesh;

	/** "vertex", "texture", "skipped", "failed" or "cancelled_before_write". */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Result;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Notes;

	/** One line per material slot: index, material, surface, rule, flags. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	TArray<FString> Slots;

	/** Structured per-slot decisions: why each slot was included or excluded, and how it was classified. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	TArray<FIcifySlotReport> SlotDetails;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FIcifyUVReport MaskUV;

	/** Time spent generating this mesh's masks and drip edges. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	float Seconds = 0.f;

	/** Reused from the generation cache (nothing that affects this mesh changed). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	bool bFromCache = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Vertices = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 TextureSize = 0;

	/** Mean of each mask over covered samples: R = frost, G = snow, B = glaze, A = black ice. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FLinearColor MeanMasks = FLinearColor(0.f, 0.f, 0.f, 0.f);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FLinearColor MaxMasks = FLinearColor(0.f, 0.f, 0.f, 0.f);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	bool bNanite = false;

	/** The Icify custom primitive data indices were already used by something else, so the material path stays inactive. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	bool bCustomDataConflict = false;

	/** Zone that owned this mesh before this run (empty if none). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString PreviousOwner;

	/** Candidate icicle points found along this mesh's drip edges. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 DripPoints = 0;

	/** Icicles grown from this mesh. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Icicles = 0;
};

/** What the icicle pass did, and why candidate points didn't become icicles. */
USTRUCT(BlueprintType)
struct ICIFY_API FIcifyIcicleStats
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Icicles = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Vertices = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Triangles = 0;

	/** Points found along drip edges, before meltwater, spacing and room checks. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Candidates = 0;

	/** No snow or meltwater above the edge (covered from the sky, and no melt source near). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 NoMeltwater = 0;

	/** Inside a heat source's thaw radius. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Thawed = 0;

	/** Removed by a freeze stamp with negative Icicles. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Erased = 0;

	/** Lost the density draw or too close to a bigger neighbour. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Thinned = 0;

	/** The edge sits against a wall, or there's less room below than Min Length. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 NoRoom = 0;

	/** Dropped because the zone reached Max Icicles. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 OverBudget = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	float LongestCm = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	float MeanLengthCm = 0.f;
};

/** Structured result of a Dry Run, Preview or Apply. Also written to Saved/Icify/Reports as JSON. */
USTRUCT(BlueprintType)
struct ICIFY_API FIcifyRunReport
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Operation;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Zone;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Seed = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Time;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	float Seconds = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 MeltSources = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 HeatSources = 0;

	/** Meshes that received ice masks. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Frozen = 0;

	/** Components left alone on purpose: excluded, filtered, custom-data conflicts, owned by another zone. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Skipped = 0;

	/** Components Icify tried to write and couldn't (see each component's Notes). Not counted as Skipped. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Failed = 0;

	/** Components whose masks were generated but not written because the run was cancelled. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 Cancelled = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	TArray<FIcifyComponentReport> Components;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FIcifyIcicleStats IcicleStats;

	/** Time per phase: floor traces, sky cover, scene sources, scan, generation, icicles, writes. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	TArray<FIcifyPhaseTiming> Phases;

	/** The run was cancelled; Cancelled During names the phase. Nothing was written after cancellation. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	bool bCancelled = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString CancelledDuring;

	/** Preset the zone's look was loaded from, if any. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	FString Preset;

	/** Meshes reused from the generation cache. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 CacheHits = 0;

	/** Sky cover cells traced (0 when sky cover is off). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	int32 SkyCells = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	TArray<FString> Warnings;
};

namespace Icify
{
	/** Custom primitive data slots, relative to UIcifySettings::CustomPrimitiveDataIndex. */
	enum ECustomDataOffset : int32
	{
		/** Only signed Icify modes activate an explicitly owned material instance. */
		CPD_MaskMode = 0,
		CPD_SurfaceType = 1,
		CPD_FeatureScale = 2,
		CPD_SeedOffset = 3,
		CPD_DebugChannel = 4,
		CPD_Count = 5
	};

	/** Bumped whenever the generated content changes; older assets are rebuilt in place. */
	constexpr int32 ContentVersion = 1;
	/** Distinct from Grungify's 501/502, so the two plugins never activate each other's material paths. */
	constexpr float MaskModeVertex = 701.f;
	constexpr float MaskModeTexture = 702.f;

	inline FName MaskTextureParameterName() { return FName(TEXT("Icify Mask Texture")); }
	inline FName ActiveParameterName() { return FName(TEXT("Icify Active")); }
	inline FName SurfaceOverrideParameterName() { return FName(TEXT("Icify Surface Override")); }
	inline FName UseUV0ParameterName() { return FName(TEXT("Icify Use UV0")); }
	inline FName IceTintParameterName() { return FName(TEXT("Icify Ice Tint")); }
	inline FName FrostColorParameterName() { return FName(TEXT("Icify Frost Color")); }
}
