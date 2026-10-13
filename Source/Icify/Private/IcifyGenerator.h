// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IcifyTypes.h"

#if WITH_EDITOR

#include "IcifyIcicleBuilder.h"
#include "StaticMeshResources.h"

class AActor;
class AIcifyFreezeZone;
class UActorComponent;
class UMaterialInterface;
class UMaterialInstanceConstant;
class UMeshComponent;
class UPrimitiveComponent;
class UStaticMeshComponent;
class UTexture2D;
class UWorld;

/** Floor heights sampled on a grid over the zone. Each cell can hold several floors (storeys, stairs). */
class FIcifyFloorField
{
public:
	void BuildFlat(double InFloorZ);
	/** Returns false if ShouldCancel asked to stop (the field is then flat at the zone bottom). */
	bool BuildTraced(UWorld* World, const FBox& InBounds, double Spacing, const AActor* IgnoreActor,
		const TFunction<bool()>& ShouldCancel = TFunction<bool()>());

	int32 NumCells() const { return NumX * NumY; }

	/** Highest floor at or just below P. */
	double FloorBelow(const FVector& P) const;

	uint32 Hash() const;

private:
	FBox Bounds = FBox(ForceInit);
	double FlatZ = 0.0;
	double CellX = 1.0;
	double CellY = 1.0;
	int32 NumX = 0;
	int32 NumY = 0;
	/** Per cell, highest first. */
	TArray<TArray<double, TInlineAllocator<4>>> Heights;
};

/**
 * The sky as a shadow map. A grid of traces starts far above the zone and runs along the snowfall direction, recording
 * where each first hits something. A point is open to the sky (snow and freezing rain reach it, and it loses heat to
 * the sky) when it lies no deeper along the snowfall direction than the first hit of its cell. Roofs, canopies,
 * overhangs and trees therefore keep what's under them clear.
 */
class FIcifySkyCoverField
{
public:
	/** Returns false if ShouldCancel asked to stop (the field is then invalid, and everything reads as open). */
	bool Build(UWorld* World, const FBox& ZoneBox, const FVector& FallDirection, double CellSize, double Reach,
		const AActor* IgnoreActor, const TFunction<bool()>& ShouldCancel = TFunction<bool()>());

	/** Tests: a field with explicit first-hit depths (row-major, NumU x NumV; a negative depth means no hit). */
	void BuildFromDepths(const FVector& InOrigin, const FVector& FallDirection, double CellSize, int32 InNumU, int32 InNumV, const TArray<float>& InDepths);

	bool IsValid() const { return NumU > 0 && NumV > 0; }
	int32 NumCells() const { return NumU * NumV; }

	/** 0..1, how open the air point P is to the sky (bilinear over the four nearest cells). Points outside the field are open. */
	float OpenAt(const FVector& P, double Bias) const;
	float OpenAt(const FVector& P) const;

	/**
	 * Openness of a surface point with normal N. Faces turned away from the falling snow (undersides, leeward walls)
	 * read as closed; the rest are read just off the surface with a slope-scaled bias, so faces don't shadow themselves.
	 */
	float SurfaceOpenAt(const FVector& P, const FVector& N) const;

	/**
	 * The first thing the falling snow hits in the column through P, if it lies above P: how far above (along the fall
	 * direction) and which actor it belongs to. False when nothing in the column is above P (P is under open sky).
	 * The actor is for identity comparison only, during the run that built the field; never dereference it.
	 */
	bool FirstHitAbove(const FVector& P, double& OutHeight, const AActor*& OutActor) const;

	const FVector& GetFallDirection() const { return Fall; }
	double GetCellSize() const { return Cell; }
	uint32 Hash() const;

private:
	FVector Origin = FVector::ZeroVector;
	FVector Fall = FVector(0.0, 0.0, -1.0);
	FVector AxisU = FVector(1.0, 0.0, 0.0);
	FVector AxisV = FVector(0.0, 1.0, 0.0);
	double Cell = 20.0;
	int32 NumU = 0;
	int32 NumV = 0;
	/** Distance along Fall from the start plane to the first hit, per cell; NoHit when the ray hit nothing. */
	TArray<float> Depths;
	/** The actor each cell's first hit belongs to (null for no hit, or a field built from depths). Identity only. */
	TArray<const AActor*> HitActors;

	static constexpr float NoHit = 3.0e38f;
	void MakeAxes();
	bool CellOpen(int32 X, int32 Y, double Depth, double Bias) const;
};

/** Capsules (segments with a radius) in a uniform grid, for fast "near a path" queries over many samples. */
class FIcifySegmentField
{
public:
	struct FSegment
	{
		FVector A;
		FVector B;
		float Radius = 10.f;
		/** Caller-defined strength (water amount). */
		float Value = 1.f;
		/** 0 at the path start, 1 at its end. */
		float AlongA = 0.f;
		float AlongB = 1.f;
	};

	void Reset(double InCellSize);
	void Add(const FSegment& Segment);
	bool IsEmpty() const { return Segments.Num() == 0; }
	uint32 Hash() const;

	/** Calls Fn(Segment, Distance, AlongFraction) for every segment whose capsule contains P. */
	template <typename FnType>
	void ForEachNear(const FVector& P, FnType&& Fn) const
	{
		const TArray<int32>* List = Cells.Find(CellOf(P));
		if (!List)
		{
			return;
		}
		for (const int32 Index : *List)
		{
			const FSegment& Seg = Segments[Index];
			const FVector Closest = FMath::ClosestPointOnSegment(P, Seg.A, Seg.B);
			const double Distance = FVector::Dist(P, Closest);
			if (Distance < Seg.Radius)
			{
				const double Length = FVector::Dist(Seg.A, Seg.B);
				const float T = Length > UE_KINDA_SMALL_NUMBER ? float(FVector::Dist(Seg.A, Closest) / Length) : 0.f;
				Fn(Seg, float(Distance), FMath::Lerp(Seg.AlongA, Seg.AlongB, T));
			}
		}
	}

private:
	TArray<FSegment> Segments;
	TMap<FIntVector, TArray<int32>> Cells;
	double CellSize = 50.0;

	FIntVector CellOf(const FVector& P) const
	{
		return FIntVector(FMath::FloorToInt(P.X / CellSize), FMath::FloorToInt(P.Y / CellSize), FMath::FloorToInt(P.Z / CellSize));
	}
};

/** A fire, heater or (optionally) lamp: ice thaws within Radius. */
struct FIcifyHeatSource
{
	FVector Location = FVector::ZeroVector;
	float Radius = 150.f;
};

/** Copy of a freeze stamp, safe to read anywhere. */
struct FIcifyStampData
{
	FTransform Transform;
	float Radius = 50.f;
	float Falloff = 0.6f;
	float EdgeNoise = 0.35f;
	FIcifyStampLayers Layers;
};

/** Where meltwater collects and freezes into a puddle (below a melt source, or the end of a traced path on flat ground). */
struct FIcifyPool
{
	FVector Center = FVector::ZeroVector;
	float Radius = 40.f;
};

/** A point meltwater runs down from (gutter, downpipe, spout or hand-placed source). */
struct FIcifyDripSource
{
	/** Traced meltwater path (world points from the source down), when Trace Meltwater is on. Empty = straight down. */
	TArray<FVector> Path;
	FVector Origin = FVector::ZeroVector;
	float Radius = 10.f;
	/** 0 = the zone's drip length. */
	float Length = 0.f;
	float Strength = 1.f;
	bool bManual = false;
};

/** CPU copy of an 8-bit gate texture, sampled nearest with wrap. */
class FIcifyGateSampler
{
public:
	bool Init(UTexture2D* Texture, FString& OutError);
	float Sample(const FVector2f& UV, EIcifyChannel Channel) const;
	/** Identity of the copied source pixels and layout, computed once on Init. */
	uint32 Hash() const { return ContentHash; }

private:
	int32 Width = 0;
	int32 Height = 0;
	int32 BytesPerPixel = 0;
	TArray64<uint8> Pixels;
	uint32 ContentHash = 0;
};

struct FIcifyGenerationParams
{
	uint32 Seed = 0;
	float Intensity = 1.f;
	float FeatureScale = 20.f;

	FTransform ZoneTransform;
	FVector ZoneExtent = FVector::ZeroVector;
	float ZoneFalloff = 0.f;

	/** Horizontal unit direction the wind blows towards. */
	FVector WindDir = FVector::ForwardVector;
	float WindInfluence = 0.f;

	float FrostIntensity = 1.f;
	float FrostFilm = 0.f;
	float EdgeFrost = 0.f;
	float Rime = 0.f;
	float GroundFrost = 0.f;
	float GroundFrostHeight = 40.f;
	float EdgeAngleDeg = 45.f;
	float FrostEdgeWidth = 6.f;

	float SnowIntensity = 1.f;
	/** Cosine of the steepest slope that holds snow. */
	float SnowMinUp = 0.64f;
	float CreviceSnow = 0.f;
	float WallPlaster = 0.f;
	float EdgeSnowLoss = 0.f;

	float GlazeIntensity = 1.f;
	float GlazeCoat = 0.f;
	float AmbientDrips = 0.f;
	float DripLength = 150.f;
	float DripSpread = 0.f;

	float BlackIceIntensity = 1.f;
	float FloorIce = 0.f;
	float CreviceIce = 0.f;
	float PuddleRadius = 60.f;

	float ThawIntensity = 0.f;

	/** Collect icicle candidates along drip edges. */
	bool bDripPoints = false;
	/** How far past vertical a surface must turn under before water drips from it. */
	float DripOverhangDeg = 20.f;
	/** Distance between candidate points along a drip edge (cm). */
	float DripSpacing = 6.f;

	EIcifyMaskOutput MaskOutput = EIcifyMaskOutput::Auto;
	float MaskTexelSize = 4.f;
	int32 MaxMaskResolution = 2048;
	int32 MaskUVChannel = 1;

	const FIcifyFloorField* Floor = nullptr;
	/** Openness to the sky; null = everything is open. */
	const FIcifySkyCoverField* Sky = nullptr;

	/** Polled between work batches. Returning true stops generation of the current mesh. */
	TFunction<bool()> ShouldCancel;

	bool CancelRequested() const { return ShouldCancel && ShouldCancel(); }

	TArray<FIcifyHeatSource> HeatSources;
	TArray<FIcifyStampData> Stamps;
	TArray<FIcifyPool> Pools;
	TArray<FIcifyDripSource> Sources;
	/** Traced meltwater paths (replace straight drips for their sources). */
	const FIcifySegmentField* Flow = nullptr;
};

/** Everything resolved for one material slot. */
struct FIcifySlotInfo
{
	EIcifySurfaceType Surface = EIcifySurfaceType::Stone;
	FIcifySurfaceProfile Profile;
	bool bIcicles = true;
	bool bExcluded = false;
	const FIcifyGate* Gate = nullptr;
	const FIcifyGateSampler* GateSampler = nullptr;
	FString Description;
};

struct FIcifyComponentContext
{
	TArray<FIcifySlotInfo> Slots;
	/** The mesh is itself a melt source (gutter, downpipe): its drip edges are always fed. */
	bool bDripSourceMesh = false;
};

/** A candidate icicle position on a drip edge. */
struct FIcifyDripPoint
{
	/** On the drip edge (world). */
	FVector P = FVector::ZeroVector;
	/** Along the edge (unit); zero for a single drip point (the bottom of a dome). */
	FVector Tangent = FVector::ZeroVector;
	/** Horizontal unit vector off the edge into open air; zero below round undersides, where there's no wall side. */
	FVector Outward = FVector::ZeroVector;
	/** 0..1: how readily water drips here (edge sharpness and level) times the slot's profile, gate and zone weight. */
	float Strength = 0.f;
	/** Frost on the edge, for rimed icicles. */
	float Frost = 0.f;
	/** Per-point random, from the position and seed: independent of triangulation order. */
	uint32 Hash = 0;
	/** On a melt-source mesh: always fed. */
	bool bDripSource = false;
};

struct FIcifyComponentResult
{
	UMeshComponent* Component = nullptr;

	/** VertexColor or Texture (never Auto). */
	EIcifyMaskOutput Output = EIcifyMaskOutput::VertexColor;

	/** One colour per render vertex, per LOD (vertex output). */
	TArray<TArray<FColor>> LODColors;

	/** Per material slot: world area, and per channel (frost, snow, glaze, black ice) the area above 0.1, the summed value there and the peak. */
	struct FSlotCoverage
	{
		double Area = 0.0;
		double Covered[4] = { 0, 0, 0, 0 };
		double Sum[4] = { 0, 0, 0, 0 };
		float Max[4] = { 0, 0, 0, 0 };
	};
	TArray<FSlotCoverage> SlotCoverage;

	/** UV channel the texture was baked in. */
	int32 MaskUVChannelUsed = INDEX_NONE;

	/** Square BGRA mask texture in the mask UV channel (texture output). */
	int32 TextureSize = 0;
	TArray<FColor> TexturePixels;

	EIcifySurfaceType DominantSurface = EIcifySurfaceType::Stone;
	bool bNanite = false;

	/** Generation stopped because ShouldCancel returned true; the result must not be written. */
	bool bCancelled = false;

	/** Mask UV channel checks (texture output candidates). */
	FIcifyUVReport MaskUV;
	int32 Vertices = 0;
	FLinearColor MeanMasks = FLinearColor(0.f, 0.f, 0.f, 0.f);
	FLinearColor MaxMasks = FLinearColor(0.f, 0.f, 0.f, 0.f);
	FString Notes;

	/** Index of this component's entry in the zone's run report. */
	int32 ReportIndex = INDEX_NONE;

	/** Auto marks an excluded slot; admitted slots carry their resolved material family. */
	TArray<EIcifySurfaceType> MaterialSurfaces;

	/** World centimetres per mask texel. */
	float TexelSizeCm = 0.f;

	/** Icicle candidates along this mesh's drip edges. */
	TArray<FIcifyDripPoint> DripPoints;
	/** Spacing the candidates were placed at (wider than asked on huge meshes, to stay within the candidate budget). */
	float DripSpacingCm = 0.f;
};

/**
 * Immutable inputs for generating one component, copied on the game thread before generation: CPU copies of the mesh
 * LODs and the transform. Edits, reimports, deletion or unloading after the copy can't affect a running generation.
 */
struct FIcifyMeshSnapshot
{
	enum class EKind : uint8 { None, Static };
	EKind Kind = EKind::None;
	FString Path;
	FTransform Transform;
	bool bNanite = false;
	FBox LocalBounds = FBox(ForceInit);
	TArray<TUniquePtr<FStaticMeshLODResources>> LODs;
};

/** A cached per-component result and the inputs it was made from. */
struct FIcifyCacheEntry
{
	uint32 Key = 0;
	TSharedPtr<FIcifyComponentResult> Result;
	FIcifyComponentReport Report;
	int64 Bytes = 0;
	double LastUsed = 0.0;
};

/** One run's inputs and outputs, prepared and collected on the game thread. */
struct FIcifyGenerationJob
{
	FString Operation;
	FIcifyGenerationParams Params;
	FIcifyFloorField Floor;
	FIcifySkyCoverField Sky;
	FIcifySegmentField Flow;
	TMap<UTexture2D*, TUniquePtr<FIcifyGateSampler>> Samplers;
	/** Gate copies, so generation never reads the zone's (editable) rule array. */
	TArray<TUniquePtr<FIcifyGate>> GateCopies;
	uint32 EnvironmentHash = 0;

	struct FItem
	{
		TWeakObjectPtr<UMeshComponent> Component;
		FString Path;
		uint32 Key = 0;
		FIcifyComponentContext Context;
		FIcifyComponentReport Report;
		FIcifyComponentResult Result;
		bool bGenerated = false;
		bool bFromCache = false;
		/** Generation found nothing usable (reported as skipped). */
		bool bFailed = false;
		float Seconds = 0.f;
		/** Generation inputs copied on the game thread (released once generated). */
		TSharedPtr<FIcifyMeshSnapshot> Snapshot;
		/** Apply with Take Over: the zone that owns this mesh now. */
		TWeakObjectPtr<AIcifyFreezeZone> TakeOverFrom;
	};
	TArray<FItem> Items;

	/** Icicles the plan chose, longest first, each with the report row of the mesh it grows from. */
	struct FPlannedIcicle
	{
		FIcifyIcicleSpec Spec;
		int32 ReportIndex = INDEX_NONE;
	};
	TArray<FPlannedIcicle> Icicles;
	/** The plan's candidate and rejection counts. */
	FIcifyIcicleStats IcicleStats;

	bool bCancelled = false;
	FString CancelledDuring;

	void GenerateItem(FItem& Item);
};

/** Icicle geometry ready for a procedural mesh component: one mesh per section, positions relative to Origin. */
struct FIcifyIcicleBuild
{
	/** World position the sections are relative to (the component is placed here). */
	FVector Origin = FVector::ZeroVector;
	TArray<FIcifyIcicleMesh> Sections;
	FIcifyIcicleStats Stats;
	/** Icicles per report row. */
	TMap<int32, int32> PerReport;
};

namespace Icify
{
	float Hash3(int32 X, int32 Y, int32 Z, uint32 Seed);
	uint32 HashU(uint32 X);
	float ValueNoise(const FVector& P, uint32 Seed);
	float Fbm(const FVector& P, uint32 Seed, int32 Octaves = 3);

	/** Splits "SM_GutterPipe_02" into {"sm", "gutter", "pipe", "02"}. */
	void TokenizeName(const FString& Name, TArray<FString>& OutTokens);
	/** True if any token starts with any keyword (so "Windows" matches "window", but "SM_Downtown" does not match "town" inside "downtown"). */
	bool NameMatches(const FString& Name, const TArray<FString>& Keywords, FString* OutKeyword = nullptr);

	/** The static mesh asset a component draws (null for other mesh components). */
	const UObject* MeshAssetOf(const UMeshComponent* Component);
	FString SlotNameOf(const UMeshComponent* Component, int32 MaterialIndex);
	/** UV channels of LOD0. */
	int32 NumUVChannelsOf(const UMeshComponent* Component);
	/** Changes when the mesh is rebuilt (the static mesh derived-data key). */
	uint32 MeshBuildHash(const UMeshComponent* Component);

	/** OutSource explains the decision (tag, physical material, keyword and where it was found, fallback...). */
	EIcifySurfaceType ClassifySlot(const UMeshComponent* Component, int32 MaterialIndex, EIcifySurfaceType Override, FString* OutSource = nullptr);
	/** Pass either an actor or a component. */
	bool IsDripSource(const AActor* Actor, const UPrimitiveComponent* Component);
	bool IsHeatSource(const AActor* Actor);
	bool HasIgnoreTag(const AActor* Actor, const UActorComponent* Component);
	bool HasNoIciclesTag(const AActor* Actor, const UActorComponent* Component);

	/** True if the rule targets this slot. */
	bool RuleMatchesSlot(const FIcifySlotRule& Rule, const UMeshComponent* Component, int32 MaterialIndex, FString* OutWhy = nullptr);

	/** Hashes effective generation inputs, not rounded diagnostic labels. */
	uint32 HashSlotContext(const FIcifyComponentContext& Context);

	/**
	 * Checks the mask UV channel of a mesh LOD: presence, 0..1 range, overlapping islands, zero-area triangles,
	 * coverage and texel-density spread. Overlap is measured on a coarse raster, independent of the bake.
	 */
	FIcifyUVReport ValidateMaskUVs(const FStaticMeshLODResources& LOD, const FTransform& Transform, int32 Channel,
		const FIcifyGenerationParams* CancelParams = nullptr);

	/** Copies everything generation reads from a component (game thread). False if its mesh data isn't readable. */
	bool SnapshotMesh(UMeshComponent* Component, FIcifyMeshSnapshot& Out);
	/** Generates from a snapshot only: never touches the component or live render data. */
	bool GenerateFromSnapshot(const FIcifyMeshSnapshot& Snapshot, const FIcifyComponentContext& Context, const FIcifyGenerationParams& Params, FIcifyComponentResult& OutResult);
	bool GenerateComponent(UStaticMeshComponent* Component, const FIcifyComponentContext& Context, const FIcifyGenerationParams& Params, FIcifyComponentResult& OutResult);

	/**
	 * Follows meltwater from Start: down walls, along sloped surfaces, over edges, until it pools on flat ground or runs out.
	 * Game thread only (line traces).
	 */
	void TraceMeltwaterPath(UWorld* World, const FVector& Start, float StepCm, int32 MaxSteps, const AActor* IgnoreActor,
		TArray<FVector>& OutPath, TArray<FIcifyPool>& OutPools);

	/** Stamp weight (0..1) at P, matching AIcifyFreezeStamp::WeightAt. */
	float StampWeight(const FIcifyStampData& Stamp, const FVector& P, float FeatureScale);

	/** 0..1, how thawed P is by the run's heat sources (1 = fully melted). */
	float ThawAt(const FIcifyGenerationParams& Params, const FVector& P);

	/** Snapshot of everything Icify can change on a component. */
	void CaptureRecord(UMeshComponent* Component, FIcifyComponentRecord& OutRecord);
	/** Puts the component back exactly as captured. Calls Modify() only when bTransactional. */
	bool RestoreRecord(const FIcifyComponentRecord& Record, bool bTransactional = true);

	bool WriteVertexColors(UStaticMeshComponent* Component, const TArray<TArray<FColor>>& LODColors,
		const TArray<EIcifySurfaceType>* MaterialSurfaces = nullptr);
	/** Both saved and live values must leave the complete requested range free. */
	bool CustomDataSlotsFree(const TArray<float>& OriginalData, const TArray<float>& OriginalLiveData, int32 Base);
	/** Updates only our fields in each array, preserving unrelated live-only values. */
	bool WriteCustomData(UMeshComponent* Component, const float (&Values)[CPD_Count]);
	UTexture2D* CreateMaskTexture(UMeshComponent* Component, int32 Size, const TArray<FColor>& Pixels);
	/** Owns only admitted slots; Texture is null for vertex masks. Excluded material identities are retained. */
	bool BindOwnedMaterials(UMeshComponent* Component, UTexture2D* Texture, const TArray<EIcifySurfaceType>& MaterialSurfaces,
		UMaterialInterface* ParentOverride, const FIcifyPalette* Palette = nullptr, bool bUseUV0 = false);
	/** Shared setup of an owned instance: texture, activation, surface, palette. */
	void ConfigureOwnedInstance(UMaterialInstanceConstant* Instance, UMaterialInterface* Parent, UTexture2D* Texture,
		EIcifySurfaceType Surface, const FIcifyPalette* Palette);
	/** Writes only the ticked palette fields. */
	void ApplyPalette(UMaterialInstanceConstant* Instance, const FIcifyPalette& Palette);
	/** Name prefix of the material instances zones create inside components. */
	const TCHAR* OwnedInstancePrefix();
}

#endif // WITH_EDITOR
