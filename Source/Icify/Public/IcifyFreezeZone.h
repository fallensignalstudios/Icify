// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "IcifyTypes.h"
#include "IcifyFreezeZone.generated.h"

class UArrowComponent;
class UDataLayerAsset;
class UIcifyPreset;
class UIcifyZoneComponent;
class UMaterialInterface;
class UMeshComponent;
class UProceduralMeshComponent;
class UTexture2D;
class FIcifyGateSampler;
struct FIcifyCacheEntry;
struct FIcifyComponentContext;
struct FIcifyComponentResult;
struct FIcifyDripSource;
struct FIcifyGenerationJob;
struct FIcifyGenerationParams;
struct FIcifyHeatSource;
struct FIcifyIcicleBuild;

/**
 * Drop into a level, size the box, then Preview / Apply / Clear.
 *
 * The zone freezes every static mesh it overlaps. It bakes four ice masks into each one, as vertex colours or as a
 * per-instance mask texture (Nanite and sparse geometry): R = frost, G = snow, B = glaze, A = black ice.
 * MF_IcifyFrost turns those masks into shading; meshes the zone hasn't touched keep their original look.
 * It also grows real icicle geometry along the meshes' drip edges, wherever meltwater can reach them.
 * Everything is driven by Seed, so the same settings always regenerate the same result.
 */
UCLASS(Blueprintable, BlueprintType, meta = (DisplayName = "Icify Freeze Zone"),
	hidecategories = (Collision, Physics, Replication, Networking, Input, Cooking, HLOD, LOD))
class ICIFY_API AIcifyFreezeZone : public AActor
{
	GENERATED_BODY()

public:
	AIcifyFreezeZone();

	/** Generate without committing. Other zones' applied meshes are skipped; Take Over happens only on Apply. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Icify")
	void Preview();

	/** Commit the ice and icicles to the level as one undoable step. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Icify")
	void Apply();

	/** Put every mesh this zone touched back the way it was and remove its icicles (undoable). */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Icify")
	void Clear();

	/** Drop the preview and go back to the last applied state. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Icify")
	void CancelPreview();

	/** Pick a new random seed (and refresh the preview if live preview is on). */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Icify")
	void NewSeed();

	/** Generate and report what would happen, without touching any mesh. See Last Report and Saved/Icify/Reports. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Icify")
	void DryRun();

	/** Copy the look (layer settings, icicles, profiles, palette, optionally the seed) from Preset into this zone. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Icify|Preset")
	void LoadPreset();

	/** Write this zone's look into Preset, creating a new preset asset in Preset Folder if none is assigned. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Icify|Preset")
	void SaveToPreset();

	/** Forget cached per-mesh results (they are also invalidated automatically when anything relevant changes). */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Icify|Performance")
	void ClearGenerationCache();

	UFUNCTION(BlueprintPure, Category = "Icify")
	int32 GetAppliedCount() const { return AppliedRecords.Num(); }

	UFUNCTION(BlueprintPure, Category = "Icify")
	int32 GetPreviewCount() const { return PreviewRecords.Num(); }

	/** Icicles currently applied (committed) by this zone. */
	UFUNCTION(BlueprintPure, Category = "Icify")
	int32 GetAppliedIcicleCount() const { return AppliedIcicleCount; }

	UFUNCTION(BlueprintPure, Category = "Icify")
	const FString& GetLastReportFile() const { return LastReportFile; }

	UFUNCTION(BlueprintPure, Category = "Icify")
	const FIcifyRunReport& GetLastReport() const { return LastReport; }

	// ---------------------------------------------------------------- General

	/** Same seed + same settings = same result, every time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify")
	int32 Seed = 1337;

	/** Overall strength of every ice layer. Icicles have their own controls. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (ClampMin = "0", UIMax = "2"))
	float Intensity = 1.f;

	/** Physical size of the ice features (frost patches, snow drifts, drip spacing). Also drives the shader detail. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (ClampMin = "1", UIMin = "5", UIMax = "200", Units = "cm"))
	float FeatureScaleCm = 20.f;

	/** Auto reads tags, physical materials and names per material slot. Anything else forces every mesh in the zone to that type. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify")
	EIcifySurfaceType SurfaceType = EIcifySurfaceType::Auto;

	/** Regenerate the preview whenever a setting changes or the zone is moved. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify")
	bool bLivePreview = true;

	/** What the meshes show while previewing. Icicles always show. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify")
	EIcifyPreviewDisplay PreviewDisplay = EIcifyPreviewDisplay::FinalMaterial;

	/** Vertex colours, or a per-instance mask texture that works on Nanite and sparse geometry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify")
	EIcifyMaskOutput MaskOutput = EIcifyMaskOutput::Auto;

	/** Physical size of one mask texel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (ClampMin = "0.25", UIMin = "1", UIMax = "20", Units = "cm"))
	float MaskTexelSizeCm = 4.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify", meta = (ClampMin = "32", ClampMax = "4096"))
	int32 MaxMaskResolution = 2048;

	/** Write every run's report to Saved/Icify/Reports as JSON. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify")
	bool bWriteReportFiles = true;

	UPROPERTY(VisibleInstanceOnly, Transient, Category = "Icify")
	FString Status;

	UPROPERTY(VisibleInstanceOnly, Category = "Icify")
	bool bPreviewActive = false;

	/** Where the last report was written. */
	UPROPERTY(VisibleInstanceOnly, Transient, Category = "Icify")
	FString LastReportFile;

	UPROPERTY(VisibleInstanceOnly, Transient, BlueprintReadOnly, Category = "Icify")
	FIcifyRunReport LastReport;

	// ---------------------------------------------------------------- Weather

	/** Where the wind blows towards (the blue arrow), relative to the zone. Rime grows into it; snow sticks to the faces it hits. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Weather")
	FRotator WindDirection = FRotator(0.f, 0.f, 0.f);

	/** 0 = still air: ice forms evenly all round. 1 = a hard wind: rime and plastered snow only on windward faces, icicles lean. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Weather", meta = (ClampMin = "0", ClampMax = "1"))
	float WindInfluence = 0.5f;

	/** How far from vertical snow and freezing rain fall, tilted along the wind. 0 = straight down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Weather", meta = (ClampMin = "0", ClampMax = "60", Units = "deg"))
	float SnowfallSlantDegrees = 15.f;

	/**
	 * Snow, frost and icicles need open sky: trace the zone from above, along the snowfall direction, so anything under a
	 * roof, canopy or overhang stays clear. Off = every surface counts as open to the sky.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Weather")
	bool bUseSkyCover = true;

	/** Spacing of the sky traces. Smaller catches thin cover (rails, wires) more precisely, but is slower. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Weather", meta = (ClampMin = "5", UIMax = "100", Units = "cm", EditCondition = "bUseSkyCover"))
	float SkyCellSizeCm = 20.f;

	/** How far above the zone to look for cover (roofs and canopies above the box). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Weather", meta = (ClampMin = "0", UIMax = "20000", Units = "cm", EditCondition = "bUseSkyCover"))
	float SkyReachCm = 3000.f;

	// ------------------------------------------------------------------ Frost

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Frost", meta = (ClampMin = "0", UIMax = "2"))
	float FrostIntensity = 1.f;

	/** A light hoarfrost film on faces open to the sky (they lose heat to it). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Frost", meta = (ClampMin = "0", ClampMax = "1"))
	float FrostFilm = 0.35f;

	/** Frost on exposed convex edges, which cool fastest. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Frost", meta = (ClampMin = "0", ClampMax = "1"))
	float EdgeFrost = 0.8f;

	/** Rime feathers on faces and edges into the wind (scaled by Wind Influence). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Frost", meta = (ClampMin = "0", ClampMax = "1"))
	float Rime = 0.6f;

	/** Heavier frost close to the ground, where the coldest air settles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Frost", meta = (ClampMin = "0", ClampMax = "1"))
	float GroundFrost = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Frost", meta = (ClampMin = "0", UIMax = "300", Units = "cm"))
	float GroundFrostHeightCm = 40.f;

	/** Edges this sharp or sharper count fully as exposed edges. Lower it to catch bevelled edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Frost", meta = (ClampMin = "5", ClampMax = "170", Units = "deg"))
	float EdgeAngleDegrees = 45.f;

	/** Width of edge frost in mask textures (vertex colours follow the mesh's own edge spacing). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Frost", meta = (ClampMin = "0.5", UIMax = "50", Units = "cm"))
	float FrostEdgeWidthCm = 6.f;

	// ------------------------------------------------------------------- Snow

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Snow", meta = (ClampMin = "0", UIMax = "2"))
	float SnowIntensity = 1.f;

	/** Surfaces steeper than this shed their snow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Snow", meta = (ClampMin = "0", ClampMax = "89", Units = "deg"))
	float SnowMaxSlopeDegrees = 50.f;

	/** Snow drifted into inside corners and creases. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Snow", meta = (ClampMin = "0", ClampMax = "1"))
	float CreviceSnow = 0.6f;

	/** Wet snow plastered onto walls facing the wind (scaled by Wind Influence). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Snow", meta = (ClampMin = "0", ClampMax = "1"))
	float WallPlaster = 0.25f;

	/** How much snow the wind strips off convex edges. 0 keeps it right to the edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Snow", meta = (ClampMin = "0", ClampMax = "1"))
	float EdgeSnowLoss = 0.3f;

	// ----------------------------------------------------------- Glaze, drips

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Glaze & Drips", meta = (ClampMin = "0", UIMax = "2"))
	float GlazeIntensity = 1.f;

	/** A clear freezing-rain coat on faces open to the sky, thicker into the wind. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Glaze & Drips", meta = (ClampMin = "0", ClampMax = "1"))
	float GlazeCoat = 0.25f;

	/** Faint frozen drips down every exposed wall, from snow melting above. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Glaze & Drips", meta = (ClampMin = "0", ClampMax = "1"))
	float AmbientDrips = 0.3f;

	/** How far frozen drips run down from a melt source. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Glaze & Drips", meta = (ClampMin = "10", UIMax = "1000", Units = "cm"))
	float DripLengthCm = 150.f;

	/** How much drips fan out as they run down (0 = straight, 1 = twice the source width at the bottom). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Glaze & Drips", meta = (ClampMin = "0", UIMax = "2"))
	float DripSpread = 0.4f;

	/** Hand-placed meltwater sources. Drag the diamonds in the viewport. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Glaze & Drips")
	TArray<FIcifyMeltSource> MeltSources;

	/** Also treat actors tagged Icify.Drip or named like a gutter, downpipe or spout as melt sources. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Glaze & Drips")
	bool bAutoDetectDripSources = true;

	/** Actors whose footprint is a melt source. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Icify|Glaze & Drips")
	TArray<TSoftObjectPtr<AActor>> DripSourceActors;

	/**
	 * Trace how meltwater actually travels from each source: down walls, along ledges, over edges, and into puddles on
	 * flat ground, which freeze into black ice. Off = straight drips below each source and a puddle under it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Glaze & Drips")
	bool bTraceMeltwater = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Glaze & Drips", meta = (ClampMin = "2", UIMax = "50", Units = "cm", EditCondition = "bTraceMeltwater"))
	float FlowStepCm = 10.f;

	// -------------------------------------------------------------- Black ice

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Black Ice", meta = (ClampMin = "0", UIMax = "2"))
	float BlackIceIntensity = 1.f;

	/** Thin patchy ice on open floors. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Black Ice", meta = (ClampMin = "0", ClampMax = "1"))
	float FloorIce = 0.25f;

	/** Ice where floors meet walls and in floor creases, where meltwater collects. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Black Ice", meta = (ClampMin = "0", ClampMax = "1"))
	float CreviceIce = 0.5f;

	/** Size of the frozen puddles below melt sources and where traced meltwater pools. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Black Ice", meta = (ClampMin = "0", UIMax = "300", Units = "cm"))
	float PuddleRadiusCm = 60.f;

	// ---------------------------------------------------------------- Icicles

	/** Grow icicle geometry along drip edges: eaves, ledges, sills, gutters, pipes, rails and the undersides of anything round. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Icicles")
	bool bGrowIcicles = true;

	/** Icicles per metre of fully fed drip edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Icicles", meta = (ClampMin = "0", UIMax = "40", EditCondition = "bGrowIcicles"))
	float IcicleDensity = 8.f;

	/** Length of the longest icicles, on edges with plenty of meltwater and room below. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Icicles", meta = (ClampMin = "1", UIMax = "300", Units = "cm", EditCondition = "bGrowIcicles"))
	float IcicleMaxLengthCm = 45.f;

	/** Icicles shorter than this aren't grown (there isn't room below the edge, or not enough meltwater). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Icicles", meta = (ClampMin = "0.5", UIMax = "50", Units = "cm", EditCondition = "bGrowIcicles"))
	float IcicleMinLengthCm = 3.f;

	/** Multiplies every icicle's thickness. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Icicles", meta = (ClampMin = "0.1", UIMax = "3", EditCondition = "bGrowIcicles"))
	float IcicleThickness = 1.f;

	/** Spacing of the ripples each drip leaves as it freezes. 0 = smooth icicles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Icicles", meta = (ClampMin = "0", UIMax = "5", Units = "cm", EditCondition = "bGrowIcicles"))
	float IcicleRippleCm = 1.2f;

	/** 0 = even rows of similar icicles; 1 = long clumps and bare gaps, like real eaves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Icicles", meta = (ClampMin = "0", ClampMax = "1", EditCondition = "bGrowIcicles"))
	float IcicleClumping = 0.6f;

	/** How far icicles lean with the wind (scaled by Wind Influence). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Icicles", meta = (ClampMin = "0", ClampMax = "1", EditCondition = "bGrowIcicles"))
	float IcicleLean = 0.2f;

	/**
	 * How far past vertical a surface must turn under before meltwater drips from it rather than running on. Raise it to
	 * keep icicles to clear overhangs (the undersides of ledges and eaves); lower it to let steep faces drip too.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Icicles", meta = (ClampMin = "0", ClampMax = "80", Units = "deg", EditCondition = "bGrowIcicles"))
	float DripOverhangDegrees = 20.f;

	/** Only edges with snow above them (open to the sky) or a melt source nearby grow icicles. Off = every drip edge does. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Icicles", meta = (EditCondition = "bGrowIcicles"))
	bool bIciclesNeedMeltwater = true;

	/** Upper limit per zone, so a huge zone can't stall the editor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Icicles", meta = (ClampMin = "0", UIMax = "20000", EditCondition = "bGrowIcicles"))
	int32 MaxIcicles = 1500;

	/** Sides around each icicle. More is smoother and costs more vertices. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Icicles", meta = (ClampMin = "4", ClampMax = "16", EditCondition = "bGrowIcicles"))
	int32 IcicleSides = 8;

	/** Give the icicles collision (complex as simple). Off by default: they're decoration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Icicles", meta = (EditCondition = "bGrowIcicles"))
	bool bIcicleCollision = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Icicles", meta = (EditCondition = "bGrowIcicles"))
	bool bIcicleShadows = true;

	/** Material for this zone's icicles. Empty = Project Settings > Icify > Icicle Material (M_IcifyIcicle). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Icicles", meta = (EditCondition = "bGrowIcicles"))
	TObjectPtr<UMaterialInterface> IcicleMaterial;

	// ------------------------------------------------------------------- Thaw

	/** How completely heat melts ice near heat sources (actors tagged Icify.Heat or named like a fire, heater or chimney). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Thaw", meta = (ClampMin = "0", ClampMax = "1"))
	float ThawIntensity = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Thaw", meta = (ClampMin = "10", UIMax = "1000", Units = "cm"))
	float ThawRadiusCm = 150.f;

	/** Also thaw around point and spot lights (lamps). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Thaw")
	bool bThawFromLights = false;

	/** Actors that are heat sources, whatever their names and tags. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Icify|Thaw")
	TArray<TSoftObjectPtr<AActor>> HeatSourceActors;

	// --------------------------------------------------------------- Surfaces

	/** Stone, concrete, brick, wood: takes every layer evenly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Surfaces")
	FIcifySurfaceProfile StoneProfile = FIcifySurfaceProfile(1.0f, 1.0f, 1.0f, 1.0f, 1.0f);

	/** Metal frosts and glazes first, and its gutters and rails grow icicles readily. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Surfaces")
	FIcifySurfaceProfile MetalProfile = FIcifySurfaceProfile(1.3f, 0.9f, 1.2f, 1.0f, 1.2f);

	/** Glass grows fern frost and sheds snow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Surfaces")
	FIcifySurfaceProfile GlassProfile = FIcifySurfaceProfile(1.1f, 0.8f, 1.0f, 1.0f, 0.6f);

	/** Named profiles slot rules can use via Profile Name. Saved in presets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Surfaces")
	TMap<FName, FIcifySurfaceProfile> MaterialProfiles;

	// ---------------------------------------------------------------- Palette

	/** Material look written onto the zone's own material instances on every Preview and Apply, so it survives regeneration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Palette")
	FIcifyPalette Palette;

	// ----------------------------------------------------------------- Stamps

	/** Apply Icify Freeze Stamp actors that overlap the zone (hand-placed add/erase touch-ups). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Stamps")
	bool bApplyFreezeStamps = true;

	// ------------------------------------------------------------ Performance

	/** Reuse per-mesh results when nothing affecting that mesh changed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Performance")
	bool bUseGenerationCache = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Performance", meta = (ClampMin = "16", UIMax = "4096", Units = "MB"))
	int32 CacheBudgetMB = 512;

	// ----------------------------------------------------------------- Preset

	/** Assigning a preset loads it into the zone. Use Save To Preset to store changes back. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Preset")
	TObjectPtr<UIcifyPreset> Preset;

	/** Where Save To Preset creates a new preset asset when none is assigned. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Preset", meta = (ContentDir))
	FDirectoryPath PresetFolder;

	// -------------------------------------------------------------- Filtering

	/** Ice fades out over this distance inside the zone's faces. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Filtering", meta = (ClampMin = "0", UIMax = "500", Units = "cm"))
	float ZoneFalloffCm = 50.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Filtering")
	EIcifyFloorMode FloorMode = EIcifyFloorMode::Trace;

	/** Spacing of the floor traces. Smaller is more accurate on stairs and uneven ground, but slower. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Filtering", meta = (ClampMin = "10", UIMax = "200", Units = "cm", EditCondition = "FloorMode == EIcifyFloorMode::Trace"))
	float FloorProbeSpacingCm = 50.f;

	/** Also freeze Movable static meshes (off by default, since they usually move out of the zone). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Filtering")
	bool bIncludeMovable = false;

	/** Actors this zone never touches (the Icify.Ignore tag does the same). */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Icify|Filtering")
	TArray<TSoftObjectPtr<AActor>> ExcludedActors;

	/** If set, only actors in one of these Data Layers are frozen. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Targeting")
	TArray<TObjectPtr<const UDataLayerAsset>> IncludeDataLayers;

	/** If set, only these actors are frozen. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Icify|Targeting")
	TArray<TSoftObjectPtr<AActor>> IncludeOnlyActors;

	/** If set, only components carrying one of these tags are frozen. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Targeting")
	TArray<FName> RequiredComponentTags;

	/** Per-slot targeting: exclude slots, give them their own profile, turn icicles on or off, gate by facing, height or a painted texture. First match wins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Targeting")
	TArray<FIcifySlotRule> SlotRules;

	/** Freeze only slots an enabled, non-excluding Slot Rule matches; every other slot is left untouched. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Targeting")
	bool bOnlySlotsMatchedByRules = false;

	/** What to do with meshes another zone has already frozen. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify|Targeting")
	EIcifyOverlapPolicy OverlapPolicy = EIcifyOverlapPolicy::SkipOwned;

	UIcifyZoneComponent* GetZoneComponent() const { return ZoneComponent; }
	UProceduralMeshComponent* GetIcicleComponent() const { return IcicleMesh; }
	UProceduralMeshComponent* GetPreviewIcicleComponent() const { return PreviewIcicleMesh; }

	/** Melt and heat sources found by the last run or refresh, for the viewport visualizer. */
	struct FSourceView
	{
		FVector Origin = FVector::ZeroVector;
		float Radius = 0.f;
		float Length = 0.f;
		bool bManual = false;
		TArray<FVector> Path;
	};
	const TArray<FSourceView>& GetMeltSourceViews() const { return MeltSourceViews; }
	const TArray<FVector>& GetHeatSourceViews() const { return HeatSourceViews; }

	/** World direction snow falls along (unit, pointing down). */
	FVector GetSnowfallDirection() const;
	/** World direction the wind blows towards (unit, horizontal). */
	FVector GetWindDirection() const;

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void Destroyed() override;
	virtual void PostUnregisterAllComponents() override;

	/** Read-only views of the zone's records (for tools and tests; never modify through these). */
	const TArray<FIcifyComponentRecord>& GetPreviewRecords() const { return PreviewRecords; }
	const TArray<FIcifyComponentRecord>& GetAppliedRecords() const { return AppliedRecords; }

	/** The record this zone holds for a component it has applied to, or null. */
	const FIcifyComponentRecord* FindAppliedRecord(const UMeshComponent* Component) const;
	bool IsPreviewing(const UMeshComponent* Component) const;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditImport() override;
	virtual void PostDuplicate(EDuplicateMode::Type DuplicateMode) override;
	/** Last line of defence: preview icicles are never saved, and applied icicles are never saved hidden. */
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;
	/** Undo/redo restores the icicle sections; make sure both components redraw from them. */
	virtual void PostEditUndo() override;

	/** Called by the zone box when its extent is edited. */
	void NotifyZoneShapeChanged(const FPropertyChangedEvent& PropertyChangedEvent);

	/** Puts previewed meshes back and drops preview icicles without an undo entry. Used before saves and when the zone is unloaded or destroyed. */
	void DiscardPreview(const TCHAR* Reason);

	/** Live preview, if Live Preview is on. Used by stamps and other actors that affect this zone. */
	void RequestLivePreview();

	/** Re-scan the level for melt and heat sources, for the viewport (also done on every run). */
	void RefreshSourceViews();

	/** Restores then hands over the original record; failed restoration retains ownership. */
	bool ReleaseAppliedRecord(const UMeshComponent* Component, FIcifyComponentRecord& OutRecord);
#endif

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	TObjectPtr<UIcifyZoneComponent> ZoneComponent;

	/** Applied icicles: saved with the level, removed by Clear. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	TObjectPtr<UProceduralMeshComponent> IcicleMesh;

	/** Preview icicles: never saved (emptied before every save). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Icify")
	TObjectPtr<UProceduralMeshComponent> PreviewIcicleMesh;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UArrowComponent> WindArrow;
#endif

	/** Components currently showing a preview, with what they looked like before it. Not copied when the zone is duplicated. */
	UPROPERTY()
	TArray<FIcifyComponentRecord> PreviewRecords;

	/** Components this zone has applied to, with what they looked like before the first Apply. Not copied when the zone is duplicated. */
	UPROPERTY()
	TArray<FIcifyComponentRecord> AppliedRecords;

	UPROPERTY()
	int32 AppliedIcicleCount = 0;

private:
	TArray<FSourceView> MeltSourceViews;
	TArray<FVector> HeatSourceViews;

	void UpdateWindArrow();
	void ConfigureIcicleComponent(UProceduralMeshComponent* Mesh) const;

#if WITH_EDITOR
	/** Per-component results keyed by component path (editor only, never saved). */
	TMap<FString, TSharedPtr<FIcifyCacheEntry>> GenerationCache;

	bool CanRunInEditorWorld() const;
	void ForgetRecords();
	void RefreshPreviewIfLive();
	bool RestorePreview();
	bool CancelDependentPreviews();
	void BuildParams(FIcifyGenerationParams& OutParams) const;
	void BuildContext(UMeshComponent* Component, TMap<UTexture2D*, TUniquePtr<FIcifyGateSampler>>& Samplers, FIcifyComponentContext& OutContext, FIcifyComponentReport& Report);

	/** Returns false if the user cancelled; nothing has been written at that point. Shows a progress dialog for long runs. */
	bool GenerateResults(const TCHAR* Operation, FIcifyGenerationJob& Job, TArray<FIcifyComponentResult>& OutResults, TMap<UMeshComponent*, AIcifyFreezeZone*>& OutTakeOvers);
	/** Game-thread part of a run: floors, sky cover, sources, flow, scan, contexts and cache lookups. False if cancelled. */
	bool PrepareJob(FIcifyGenerationJob& Job, const TCHAR* Operation, const TFunction<bool()>& ShouldCancel);
	/** Turns finished job items into results and report entries (and fills the cache). */
	void CollectJob(FIcifyGenerationJob& Job, TArray<FIcifyComponentResult>& OutResults, TMap<UMeshComponent*, AIcifyFreezeZone*>& OutTakeOvers);
	/** Melt sources, heat sources, stamps and traced meltwater for this run. */
	void GatherScene(FIcifyGenerationJob& Job, const FBox& ZoneBox);
	/** Melt sources (hand-placed, listed and detected) and heat sources around the zone. */
	void CollectSources(TArray<FIcifyDripSource>& OutDrips, TArray<FIcifyHeatSource>& OutHeat) const;

	/**
	 * Picks icicles from the results' drip points: meltwater, thaw, stamps, density, spacing, then traces for walls and
	 * room below. Game thread, before anything is written. False if cancelled.
	 */
	bool PlanIcicles(FIcifyGenerationJob& Job, const TArray<FIcifyComponentResult>& Results, const TFunction<bool()>& ShouldCancel);
	/** Builds the planned icicles of the meshes that were frozen (their report rows) into mesh sections. */
	void BuildIcicles(const FIcifyGenerationJob& Job, const TSet<int32>& FrozenReports, FIcifyIcicleBuild& OutBuild);
	/** Replaces Target's sections with the built icicles. */
	void WriteIcicles(UProceduralMeshComponent* Target, const FIcifyIcicleBuild& Build, bool bTransactional);
	UMaterialInterface* ResolveIcicleMaterial(UProceduralMeshComponent* Owner);
	/** Empties the preview icicles and shows the applied ones again. */
	void ClearPreviewIcicles(bool bTransactional);
	/** Previewed meshes, preview icicles, or applied icicles hidden behind a preview. */
	bool HasPreviewState() const;

	void TrimCache();
	/** OutReason is set when an actor inside the zone is filtered out by a zone setting or tag (for the report). */
	bool ShouldConsiderActor(const AActor* Actor, const FBox& ZoneBox, FString* OutReason = nullptr) const;
	void CopyFromPreset(const UIcifyPreset& Source);
	/** Writes masks, custom data and materials only when both original CPD arrays leave our slots free. */
	bool WriteResult(const FIcifyComponentResult& Result, EIcifyPreviewDisplay Display, const FIcifyComponentRecord& OriginalState);
	const FIcifyComponentRecord* FindOriginalRecord(const UMeshComponent* Component) const;
	void FinishReport(double StartSeconds);
#endif
};
