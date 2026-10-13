// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifyGenerator.h"

#if WITH_EDITOR

#include "CollisionQueryParams.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "IcifySettings.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Crc.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "StaticMeshResources.h"

namespace Icify
{
	// ------------------------------------------------------------------ Noise

	uint32 HashU(uint32 X)
	{
		X ^= X >> 16;
		X *= 0x7feb352du;
		X ^= X >> 15;
		X *= 0x846ca68bu;
		X ^= X >> 16;
		return X;
	}

	float Hash3(int32 X, int32 Y, int32 Z, uint32 Seed)
	{
		uint32 H = HashU(uint32(Z) * 0xcb1ab31fu ^ Seed);
		H = HashU(uint32(Y) * 0xd8163841u ^ H);
		H = HashU(uint32(X) * 0x8da6b343u ^ H);
		return float(H & 0x00FFFFFFu) / 16777216.f;
	}

	float ValueNoise(const FVector& P, uint32 Seed)
	{
		const double FX = FMath::FloorToDouble(P.X);
		const double FY = FMath::FloorToDouble(P.Y);
		const double FZ = FMath::FloorToDouble(P.Z);
		const int32 X = int32(FX);
		const int32 Y = int32(FY);
		const int32 Z = int32(FZ);

		auto Fade = [](double T) { return float(T * T * (3.0 - 2.0 * T)); };
		const float UX = Fade(P.X - FX);
		const float UY = Fade(P.Y - FY);
		const float UZ = Fade(P.Z - FZ);

		const float N000 = Hash3(X, Y, Z, Seed);
		const float N100 = Hash3(X + 1, Y, Z, Seed);
		const float N010 = Hash3(X, Y + 1, Z, Seed);
		const float N110 = Hash3(X + 1, Y + 1, Z, Seed);
		const float N001 = Hash3(X, Y, Z + 1, Seed);
		const float N101 = Hash3(X + 1, Y, Z + 1, Seed);
		const float N011 = Hash3(X, Y + 1, Z + 1, Seed);
		const float N111 = Hash3(X + 1, Y + 1, Z + 1, Seed);

		const float A = FMath::Lerp(FMath::Lerp(N000, N100, UX), FMath::Lerp(N010, N110, UX), UY);
		const float B = FMath::Lerp(FMath::Lerp(N001, N101, UX), FMath::Lerp(N011, N111, UX), UY);
		return FMath::Lerp(A, B, UZ);
	}

	float Fbm(const FVector& P, uint32 Seed, int32 Octaves)
	{
		float Sum = 0.f;
		float Amp = 0.5f;
		float Norm = 0.f;
		FVector Q = P;
		for (int32 Octave = 0; Octave < Octaves; ++Octave)
		{
			Sum += ValueNoise(Q, Seed + uint32(Octave) * 101u) * Amp;
			Norm += Amp;
			Q = Q * 2.03 + FVector(17.17);
			Amp *= 0.5f;
		}
		return Norm > 0.f ? Sum / Norm : 0.f;
	}

	// --------------------------------------------------------- Classification

	void TokenizeName(const FString& Name, TArray<FString>& OutTokens)
	{
		FString Current;
		auto Flush = [&]()
		{
			if (!Current.IsEmpty())
			{
				OutTokens.Add(Current.ToLower());
				Current.Reset();
			}
		};

		TCHAR Prev = 0;
		for (const TCHAR Ch : Name)
		{
			if (!FChar::IsAlnum(Ch))
			{
				Flush();
				Prev = 0;
				continue;
			}

			if (Prev != 0)
			{
				const bool bLowerToUpper = FChar::IsLower(Prev) && FChar::IsUpper(Ch);
				const bool bDigitChange = FChar::IsDigit(Prev) != FChar::IsDigit(Ch);
				if (bLowerToUpper || bDigitChange)
				{
					Flush();
				}
				else if (FChar::IsUpper(Prev) && FChar::IsLower(Ch) && Current.Len() > 1)
				{
					// "HVACVent": the last capital of an acronym starts the next word.
					const TCHAR Carry = Current[Current.Len() - 1];
					Current.LeftChopInline(1);
					Flush();
					Current.AppendChar(Carry);
				}
			}

			Current.AppendChar(Ch);
			Prev = Ch;
		}
		Flush();
	}

	bool NameMatches(const FString& Name, const TArray<FString>& Keywords, FString* OutKeyword)
	{
		if (Name.IsEmpty() || Keywords.Num() == 0)
		{
			return false;
		}

		TArray<FString> Tokens;
		TokenizeName(Name, Tokens);
		for (const FString& Token : Tokens)
		{
			for (const FString& Keyword : Keywords)
			{
				if (!Keyword.IsEmpty() && Token.StartsWith(Keyword, ESearchCase::IgnoreCase))
				{
					if (OutKeyword)
					{
						*OutKeyword = Keyword;
					}
					return true;
				}
			}
		}
		return false;
	}

	static EIcifySurfaceType SurfaceFromTags(const TArray<FName>& Tags, const UIcifySettings* Settings, FName* OutTag = nullptr)
	{
		for (const FName& Tag : Tags)
		{
			EIcifySurfaceType Found = EIcifySurfaceType::Auto;
			if (Tag == Settings->GlassTag)
			{
				Found = EIcifySurfaceType::Glass;
			}
			else if (Tag == Settings->MetalTag)
			{
				Found = EIcifySurfaceType::Metal;
			}
			else if (Tag == Settings->StoneTag)
			{
				Found = EIcifySurfaceType::Stone;
			}
			if (Found != EIcifySurfaceType::Auto)
			{
				if (OutTag)
				{
					*OutTag = Tag;
				}
				return Found;
			}
		}
		return EIcifySurfaceType::Auto;
	}

	/** bAllowGlass turns off for good once a name says it's a window's frame or sill, so the mesh name ("SM_Window")
	 * can't make that slot glass later on. */
	static EIcifySurfaceType SurfaceFromName(const FString& Name, const UIcifySettings* Settings, bool& bAllowGlass, FString* OutKeyword = nullptr)
	{
		if (bAllowGlass && NameMatches(Name, Settings->NotGlassKeywords, nullptr))
		{
			bAllowGlass = false;
		}
		if (bAllowGlass && NameMatches(Name, Settings->GlassKeywords, OutKeyword))
		{
			return EIcifySurfaceType::Glass;
		}
		if (NameMatches(Name, Settings->MetalKeywords, OutKeyword))
		{
			return EIcifySurfaceType::Metal;
		}
		if (NameMatches(Name, Settings->StoneKeywords, OutKeyword))
		{
			return EIcifySurfaceType::Stone;
		}
		return EIcifySurfaceType::Auto;
	}

	const UObject* MeshAssetOf(const UMeshComponent* Component)
	{
		if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
		{
			return Static->GetStaticMesh();
		}
		return nullptr;
	}

	FString SlotNameOf(const UMeshComponent* Component, int32 MaterialIndex)
	{
		const TArray<FName> Names = Component ? Component->GetMaterialSlotNames() : TArray<FName>();
		return Names.IsValidIndex(MaterialIndex) ? Names[MaterialIndex].ToString() : FString();
	}

	int32 NumUVChannelsOf(const UMeshComponent* Component)
	{
		if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
		{
			const UStaticMesh* Mesh = Static->GetStaticMesh();
			const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
			return RenderData && RenderData->LODResources.Num() > 0 ? int32(RenderData->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()) : 0;
		}
		return 0;
	}

	uint32 MeshBuildHash(const UMeshComponent* Component)
	{
		if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
		{
			const UStaticMesh* Mesh = Static->GetStaticMesh();
			const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
			return RenderData ? GetTypeHash(RenderData->DerivedDataKey) : 0;
		}
		return 0;
	}

	EIcifySurfaceType ClassifySlot(const UMeshComponent* Component, int32 MaterialIndex, EIcifySurfaceType Override, FString* OutSource)
	{
		auto Explain = [OutSource](const FString& Why)
		{
			if (OutSource)
			{
				*OutSource = Why;
			}
		};

		if (Override != EIcifySurfaceType::Auto)
		{
			Explain(TEXT("zone Surface Type override"));
			return Override;
		}

		const UIcifySettings* Settings = UIcifySettings::Get();
		const AActor* Owner = Component->GetOwner();
		FName Tag;
		FString Keyword;

		// 1. Explicit tags win.
		EIcifySurfaceType Result = SurfaceFromTags(Component->ComponentTags, Settings, &Tag);
		if (Result != EIcifySurfaceType::Auto)
		{
			Explain(FString::Printf(TEXT("component tag '%s'"), *Tag.ToString()));
			return Result;
		}
		if (Owner)
		{
			Result = SurfaceFromTags(Owner->Tags, Settings, &Tag);
			if (Result != EIcifySurfaceType::Auto)
			{
				Explain(FString::Printf(TEXT("actor tag '%s'"), *Tag.ToString()));
				return Result;
			}
		}

		// 2. Physical material, then material names up the instance chain.
		bool bAllowGlass = true;
		if (UMaterialInterface* Material = Component->GetMaterial(MaterialIndex))
		{
			if (const UPhysicalMaterial* PhysMat = Material->GetPhysicalMaterial())
			{
				if (const EIcifySurfaceType* Mapped = Settings->PhysicalSurfaceMap.Find(PhysMat->SurfaceType))
				{
					if (*Mapped != EIcifySurfaceType::Auto)
					{
						Explain(FString::Printf(TEXT("physical material %s (surface type %d) in Physical Surface Map"), *PhysMat->GetName(), int32(PhysMat->SurfaceType.GetValue())));
						return *Mapped;
					}
				}
			}

			for (const UMaterialInterface* It = Material; It; )
			{
				Result = SurfaceFromName(It->GetName(), Settings, bAllowGlass, &Keyword);
				if (Result != EIcifySurfaceType::Auto)
				{
					Explain(FString::Printf(TEXT("keyword '%s' in material %s%s"), *Keyword, *It->GetName(), It == Material ? TEXT("") : TEXT(" (parent)")));
					return Result;
				}
				const UMaterialInstance* Instance = Cast<UMaterialInstance>(It);
				It = Instance ? Instance->Parent.Get() : nullptr;
			}
		}

		// 3. Slot name, mesh name, actor label.
		if (const UObject* Mesh = MeshAssetOf(Component))
		{
			const FString SlotName = SlotNameOf(Component, MaterialIndex);
			if (!SlotName.IsEmpty())
			{
				Result = SurfaceFromName(SlotName, Settings, bAllowGlass, &Keyword);
				if (Result != EIcifySurfaceType::Auto)
				{
					Explain(FString::Printf(TEXT("keyword '%s' in slot name %s"), *Keyword, *SlotName));
					return Result;
				}
			}

			Result = SurfaceFromName(Mesh->GetName(), Settings, bAllowGlass, &Keyword);
			if (Result != EIcifySurfaceType::Auto)
			{
				Explain(FString::Printf(TEXT("keyword '%s' in mesh %s"), *Keyword, *Mesh->GetName()));
				return Result;
			}
		}

		if (Owner)
		{
			Result = SurfaceFromName(Owner->GetActorLabel(), Settings, bAllowGlass, &Keyword);
			if (Result != EIcifySurfaceType::Auto)
			{
				Explain(FString::Printf(TEXT("keyword '%s' in actor label %s"), *Keyword, *Owner->GetActorLabel()));
				return Result;
			}
		}

		Explain(TEXT("no tag, physical material or keyword matched; Fallback Surface Type"));
		return Settings->FallbackSurfaceType == EIcifySurfaceType::Auto ? EIcifySurfaceType::Stone : Settings->FallbackSurfaceType;
	}

	bool IsDripSource(const AActor* Actor, const UPrimitiveComponent* Component)
	{
		const UIcifySettings* Settings = UIcifySettings::Get();

		if (Actor)
		{
			return Actor->ActorHasTag(Settings->DripTag) || NameMatches(Actor->GetActorLabel(), Settings->DripKeywords);
		}

		if (Component)
		{
			if (Component->ComponentHasTag(Settings->DripTag))
			{
				return true;
			}
			if (const UMeshComponent* MeshComponent = Cast<UMeshComponent>(Component))
			{
				const UObject* Mesh = MeshAssetOf(MeshComponent);
				return Mesh && NameMatches(Mesh->GetName(), Settings->DripKeywords);
			}
		}
		return false;
	}

	bool IsHeatSource(const AActor* Actor)
	{
		if (!Actor)
		{
			return false;
		}
		const UIcifySettings* Settings = UIcifySettings::Get();
		if (Actor->ActorHasTag(Settings->HeatTag) || NameMatches(Actor->GetActorLabel(), Settings->HeatKeywords))
		{
			return true;
		}
		TInlineComponentArray<UMeshComponent*> Meshes(Actor);
		for (const UMeshComponent* Mesh : Meshes)
		{
			if (!Mesh)
			{
				continue;
			}
			const UObject* Asset = MeshAssetOf(Mesh);
			if (Mesh->ComponentHasTag(Settings->HeatTag) || (Asset && NameMatches(Asset->GetName(), Settings->HeatKeywords)))
			{
				return true;
			}
		}
		return false;
	}

	bool HasIgnoreTag(const AActor* Actor, const UActorComponent* Component)
	{
		const FName IgnoreTag = UIcifySettings::Get()->IgnoreTag;
		return (Actor && Actor->ActorHasTag(IgnoreTag)) || (Component && Component->ComponentHasTag(IgnoreTag));
	}

	bool HasNoIciclesTag(const AActor* Actor, const UActorComponent* Component)
	{
		const FName Tag = UIcifySettings::Get()->NoIciclesTag;
		return (Actor && Actor->ActorHasTag(Tag)) || (Component && Component->ComponentHasTag(Tag));
	}
}

// --------------------------------------------------------------- Floor field

void FIcifyFloorField::BuildFlat(double InFloorZ)
{
	FlatZ = InFloorZ;
	NumX = NumY = 0;
	Heights.Reset();
}

bool FIcifyFloorField::BuildTraced(UWorld* World, const FBox& InBounds, double Spacing, const AActor* IgnoreActor, const TFunction<bool()>& ShouldCancel)
{
	BuildFlat(InBounds.Min.Z);
	if (!World || !InBounds.IsValid)
	{
		return true;
	}

	const UIcifySettings* Settings = UIcifySettings::Get();
	constexpr int32 MaxCellsPerAxis = 128;
	constexpr int32 MaxHitsPerCell = 16;

	Bounds = InBounds;
	const FVector Size = Bounds.GetSize();
	const double Step = FMath::Max(Spacing, 10.0);
	NumX = FMath::Clamp(FMath::CeilToInt(Size.X / Step), 1, MaxCellsPerAxis);
	NumY = FMath::Clamp(FMath::CeilToInt(Size.Y / Step), 1, MaxCellsPerAxis);
	CellX = Size.X / NumX;
	CellY = Size.Y / NumY;
	Heights.SetNum(NumX * NumY);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(IcifyFloorTrace), /*bTraceComplex*/ true);
	if (IgnoreActor)
	{
		QueryParams.AddIgnoredActor(IgnoreActor);
	}

	auto IsFloor = [Settings](const FHitResult& Hit)
	{
		if (Hit.ImpactNormal.Z < 0.65)
		{
			return false;
		}
		const UPrimitiveComponent* HitComponent = Hit.GetComponent();
		const AActor* HitActor = Hit.GetActor();
		if (Icify::HasIgnoreTag(HitActor, HitComponent))
		{
			return false;
		}
		if ((HitComponent && HitComponent->ComponentHasTag(Settings->FloorTag)) || (HitActor && HitActor->ActorHasTag(Settings->FloorTag)))
		{
			return true;
		}
		if (!HitComponent)
		{
			return true;
		}
		const FVector Extent = HitComponent->Bounds.BoxExtent;
		return FMath::Min(Extent.X, Extent.Y) * 2.0 >= Settings->MinFloorSizeCm;
	};

	const double TopZ = Bounds.Max.Z + 50.0;
	const double BottomZ = Bounds.Min.Z - 500.0;

	for (int32 Y = 0; Y < NumY; ++Y)
	{
		// One poll per row of cells keeps the dialog responsive without slowing the traces.
		if (ShouldCancel && ShouldCancel())
		{
			BuildFlat(InBounds.Min.Z);
			return false;
		}
		for (int32 X = 0; X < NumX; ++X)
		{
			TArray<double, TInlineAllocator<4>>& Cell = Heights[Y * NumX + X];
			FVector Start(Bounds.Min.X + (X + 0.5) * CellX, Bounds.Min.Y + (Y + 0.5) * CellY, TopZ);
			const FVector End(Start.X, Start.Y, BottomZ);

			for (int32 HitIndex = 0; HitIndex < MaxHitsPerCell; ++HitIndex)
			{
				FHitResult Hit;
				if (!World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, QueryParams))
				{
					break;
				}
				if (IsFloor(Hit))
				{
					Cell.Add(Hit.ImpactPoint.Z);
				}
				// Step just past the hit and keep going to find the storeys below.
				Start.Z = Hit.ImpactPoint.Z - 1.0;
				if (Start.Z <= End.Z)
				{
					break;
				}
			}

			Cell.Sort([](double A, double B) { return A > B; });
		}
	}
	return true;
}

uint32 FIcifyFloorField::Hash() const
{
	uint32 Hash = HashCombine(GetTypeHash(FlatZ), GetTypeHash(NumX * 131 + NumY));
	Hash = HashCombine(Hash, GetTypeHash(Bounds.Min));
	Hash = HashCombine(Hash, GetTypeHash(Bounds.Max));
	Hash = HashCombine(Hash, GetTypeHash(CellX));
	Hash = HashCombine(Hash, GetTypeHash(CellY));
	for (const TArray<double, TInlineAllocator<4>>& Cell : Heights)
	{
		Hash = HashCombine(Hash, GetTypeHash(Cell.Num()));
		for (const double Height : Cell)
		{
			Hash = HashCombine(Hash, GetTypeHash(Height));
		}
	}
	return Hash;
}

double FIcifyFloorField::FloorBelow(const FVector& P) const
{
	if (NumX == 0 || NumY == 0)
	{
		return FlatZ;
	}

	const int32 X = FMath::Clamp(FMath::FloorToInt((P.X - Bounds.Min.X) / CellX), 0, NumX - 1);
	const int32 Y = FMath::Clamp(FMath::FloorToInt((P.Y - Bounds.Min.Y) / CellY), 0, NumY - 1);
	for (const double Height : Heights[Y * NumX + X])
	{
		// Small tolerance so vertices sitting on the floor (or sunk into it) still find it.
		if (Height <= P.Z + 2.0)
		{
			return Height;
		}
	}
	return FMath::Min(FlatZ, P.Z);
}

// ----------------------------------------------------------- Sky cover field

void FIcifySkyCoverField::MakeAxes()
{
	Fall = Fall.GetSafeNormal();
	if (Fall.IsNearlyZero())
	{
		Fall = FVector(0.0, 0.0, -1.0);
	}
	// Any reference that isn't parallel to the fall direction gives a stable frame.
	const FVector Reference = FMath::Abs(Fall.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector;
	AxisU = FVector::CrossProduct(Reference, Fall).GetSafeNormal();
	AxisV = FVector::CrossProduct(Fall, AxisU).GetSafeNormal();
}

bool FIcifySkyCoverField::Build(UWorld* World, const FBox& ZoneBox, const FVector& FallDirection, double CellSize, double Reach,
	const AActor* IgnoreActor, const TFunction<bool()>& ShouldCancel)
{
	NumU = NumV = 0;
	Depths.Reset();
	HitActors.Reset();
	if (!World || !ZoneBox.IsValid)
	{
		return true;
	}
	Fall = FallDirection;
	MakeAxes();
	Cell = FMath::Max(CellSize, 5.0);

	// The zone box as seen along the fall direction: its extent in the start plane and along the fall.
	const FVector Center = ZoneBox.GetCenter();
	const FVector Extent = ZoneBox.GetExtent();
	double MinU = TNumericLimits<double>::Max(), MaxU = -TNumericLimits<double>::Max();
	double MinV = TNumericLimits<double>::Max(), MaxV = -TNumericLimits<double>::Max();
	double MinD = TNumericLimits<double>::Max(), MaxD = -TNumericLimits<double>::Max();
	for (int32 Corner = 0; Corner < 8; ++Corner)
	{
		const FVector Rel((Corner & 1) ? Extent.X : -Extent.X, (Corner & 2) ? Extent.Y : -Extent.Y, (Corner & 4) ? Extent.Z : -Extent.Z);
		MinU = FMath::Min(MinU, FVector::DotProduct(Rel, AxisU));
		MaxU = FMath::Max(MaxU, FVector::DotProduct(Rel, AxisU));
		MinV = FMath::Min(MinV, FVector::DotProduct(Rel, AxisV));
		MaxV = FMath::Max(MaxV, FVector::DotProduct(Rel, AxisV));
		MinD = FMath::Min(MinD, FVector::DotProduct(Rel, Fall));
		MaxD = FMath::Max(MaxD, FVector::DotProduct(Rel, Fall));
	}

	// Two cells of margin all round, so lookups at the zone's faces have neighbours.
	const double Margin = 2.0 * Cell;
	MinU -= Margin;
	MaxU += Margin;
	MinV -= Margin;
	MaxV += Margin;

	// Keep the trace count bounded on huge zones by coarsening the grid.
	constexpr int32 MaxCellsPerAxis = 512;
	constexpr int64 MaxCells = 160000;
	auto CountFor = [this](double Length) { return FMath::Max(1, FMath::CeilToInt(Length / Cell)); };
	while (CountFor(MaxU - MinU) > MaxCellsPerAxis || CountFor(MaxV - MinV) > MaxCellsPerAxis
		|| int64(CountFor(MaxU - MinU)) * int64(CountFor(MaxV - MinV)) > MaxCells)
	{
		Cell *= 1.25;
	}
	NumU = CountFor(MaxU - MinU);
	NumV = CountFor(MaxV - MinV);

	// The start plane sits above the zone (against the fall) by Reach, so roofs and canopies above the box still count.
	const double StartD = MinD - FMath::Max(Reach, 0.0) - 10.0;
	Origin = Center + AxisU * MinU + AxisV * MinV + Fall * StartD;
	const double Length = (MaxD - StartD) + Margin + 50.0;

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(IcifySkyTrace), /*bTraceComplex*/ true);
	if (IgnoreActor)
	{
		QueryParams.AddIgnoredActor(IgnoreActor);
	}

	Depths.SetNumUninitialized(NumU * NumV);
	HitActors.SetNumZeroed(NumU * NumV);
	for (int32 Y = 0; Y < NumV; ++Y)
	{
		if (ShouldCancel && ShouldCancel())
		{
			NumU = NumV = 0;
			Depths.Reset();
			HitActors.Reset();
			return false;
		}
		for (int32 X = 0; X < NumU; ++X)
		{
			const FVector Start = Origin + AxisU * ((X + 0.5) * Cell) + AxisV * ((Y + 0.5) * Cell);
			const FVector End = Start + Fall * Length;
			float Depth = NoHit;
			for (int32 Attempt = 0; Attempt < 8; ++Attempt)
			{
				FHitResult Hit;
				if (!World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, QueryParams))
				{
					break;
				}
				const AActor* HitActor = Hit.GetActor();
				const UPrimitiveComponent* HitComponent = Hit.GetComponent();
				if (Icify::HasIgnoreTag(HitActor, HitComponent))
				{
					// Ignored actors shelter nothing: leave them out of every later trace too, and look again.
					if (HitActor && HitActor->ActorHasTag(UIcifySettings::Get()->IgnoreTag))
					{
						QueryParams.AddIgnoredActor(HitActor);
					}
					else if (HitComponent)
					{
						QueryParams.AddIgnoredComponent(HitComponent);
					}
					continue;
				}
				Depth = float(FVector::DotProduct(Hit.ImpactPoint - Origin, Fall));
				HitActors[Y * NumU + X] = HitActor;
				break;
			}
			Depths[Y * NumU + X] = Depth;
		}
	}
	return true;
}

void FIcifySkyCoverField::BuildFromDepths(const FVector& InOrigin, const FVector& FallDirection, double CellSize, int32 InNumU, int32 InNumV, const TArray<float>& InDepths)
{
	Fall = FallDirection;
	MakeAxes();
	Origin = InOrigin;
	Cell = FMath::Max(CellSize, 1.0);
	NumU = InNumU;
	NumV = InNumV;
	Depths = InDepths;
	HitActors.Reset();
	if (NumU <= 0 || NumV <= 0 || Depths.Num() != NumU * NumV)
	{
		NumU = NumV = 0;
		Depths.Reset();
		return;
	}
	HitActors.SetNumZeroed(NumU * NumV);
	for (float& Depth : Depths)
	{
		if (Depth < 0.f)
		{
			Depth = NoHit;
		}
	}
}

bool FIcifySkyCoverField::CellOpen(int32 X, int32 Y, double Depth, double Bias) const
{
	if (X < 0 || Y < 0 || X >= NumU || Y >= NumV)
	{
		return true;
	}
	const float FirstHit = Depths[Y * NumU + X];
	return FirstHit >= NoHit * 0.5f || Depth <= double(FirstHit) + Bias;
}

float FIcifySkyCoverField::OpenAt(const FVector& P, double Bias) const
{
	if (!IsValid())
	{
		return 1.f;
	}
	const FVector Rel = P - Origin;
	// Cell centres sit at half-cell offsets, so subtract half a cell to interpolate between them.
	const double FU = FVector::DotProduct(Rel, AxisU) / Cell - 0.5;
	const double FV = FVector::DotProduct(Rel, AxisV) / Cell - 0.5;
	const double Depth = FVector::DotProduct(Rel, Fall);
	const int32 X0 = FMath::FloorToInt(FU);
	const int32 Y0 = FMath::FloorToInt(FV);
	const float TX = float(FU - X0);
	const float TY = float(FV - Y0);
	auto Open = [&](int32 X, int32 Y) { return CellOpen(X, Y, Depth, Bias) ? 1.f : 0.f; };
	return FMath::Lerp(FMath::Lerp(Open(X0, Y0), Open(X0 + 1, Y0), TX), FMath::Lerp(Open(X0, Y0 + 1), Open(X0 + 1, Y0 + 1), TX), TY);
}

float FIcifySkyCoverField::OpenAt(const FVector& P) const
{
	return OpenAt(P, Cell * 0.5 + 2.0);
}

float FIcifySkyCoverField::SurfaceOpenAt(const FVector& P, const FVector& N) const
{
	if (!IsValid())
	{
		return 1.f;
	}
	// 1 = facing straight into the falling snow. Turned away: undersides and leeward walls get none directly.
	const double Facing = FVector::DotProduct(N, -Fall);
	if (Facing < -0.05)
	{
		return 0.f;
	}
	// Slope-scaled bias: the steeper the face is to the fall direction, the further apart its cells' first hits are.
	const double Sine = FMath::Sqrt(FMath::Max(0.0, 1.0 - Facing * Facing));
	const double Slope = Sine / FMath::Max(Facing, 0.25);
	const double Bias = Cell * (0.5 + 0.75 * Slope) + 2.0;
	return OpenAt(P + N * (0.25 * Cell + 1.0), Bias);
}

bool FIcifySkyCoverField::FirstHitAbove(const FVector& P, double& OutHeight, const AActor*& OutActor) const
{
	OutHeight = 0.0;
	OutActor = nullptr;
	if (!IsValid())
	{
		return false;
	}
	const FVector Rel = P - Origin;
	const int32 X = FMath::FloorToInt(FVector::DotProduct(Rel, AxisU) / Cell);
	const int32 Y = FMath::FloorToInt(FVector::DotProduct(Rel, AxisV) / Cell);
	if (X < 0 || Y < 0 || X >= NumU || Y >= NumV)
	{
		return false;
	}
	const float FirstHit = Depths[Y * NumU + X];
	const double Depth = FVector::DotProduct(Rel, Fall);
	if (FirstHit >= NoHit * 0.5f || double(FirstHit) >= Depth)
	{
		return false;
	}
	OutHeight = Depth - double(FirstHit);
	OutActor = HitActors.IsValidIndex(Y * NumU + X) ? HitActors[Y * NumU + X] : nullptr;
	return true;
}

uint32 FIcifySkyCoverField::Hash() const
{
	uint32 Hash = Depths.Num() > 0 ? FCrc::MemCrc32(Depths.GetData(), Depths.Num() * sizeof(float)) : 0u;
	Hash = HashCombine(Hash, GetTypeHash(Origin));
	Hash = HashCombine(Hash, GetTypeHash(Fall));
	Hash = HashCombine(Hash, GetTypeHash(Cell));
	return HashCombine(Hash, GetTypeHash(NumU * 7919 + NumV));
}

// -------------------------------------------------------------- Slot rules

namespace Icify
{
	bool RuleMatchesSlot(const FIcifySlotRule& Rule, const UMeshComponent* Component, int32 MaterialIndex, FString* OutWhy)
	{
		const bool bHasActor = !Rule.Actor.IsNull();
		const bool bHasPattern = !Rule.NamePattern.IsEmpty();
		const bool bHasMaterial = !Rule.Material.IsNull();
		const bool bHasSlot = Rule.SlotIndex >= 0;
		const bool bHasComponent = !Rule.ComponentName.IsNone();
		if (!Rule.bEnabled || (!bHasActor && !bHasPattern && !bHasMaterial && !bHasSlot && !bHasComponent))
		{
			return false;
		}
		if ((bHasSlot && Rule.SlotIndex != MaterialIndex) || (bHasComponent && Component->GetFName() != Rule.ComponentName))
		{
			return false;
		}

		TArray<FString> Why;
		if (bHasComponent)
		{
			Why.Add(FString::Printf(TEXT("component %s"), *Component->GetName()));
		}
		if (bHasSlot)
		{
			Why.Add(FString::Printf(TEXT("slot %d"), MaterialIndex));
		}
		const AActor* Owner = Component->GetOwner();
		if (bHasActor)
		{
			if (Rule.Actor.Get() != Owner)
			{
				return false;
			}
			Why.Add(FString::Printf(TEXT("actor %s"), Owner ? *Owner->GetActorLabel() : TEXT("?")));
		}

		const UMaterialInterface* SlotMaterial = Component->GetMaterial(MaterialIndex);

		if (bHasMaterial)
		{
			const UMaterialInterface* Wanted = Rule.Material.Get();
			const UMaterialInterface* Matched = nullptr;
			for (const UMaterialInterface* It = SlotMaterial; It && !Matched; )
			{
				if (Wanted ? It == Wanted : It->GetPathName() == Rule.Material.ToSoftObjectPath().ToString())
				{
					Matched = It;
				}
				const UMaterialInstance* Instance = Cast<UMaterialInstance>(It);
				It = Instance ? Instance->Parent.Get() : nullptr;
			}
			if (!Matched)
			{
				return false;
			}
			Why.Add(FString::Printf(TEXT("material %s%s"), *Matched->GetName(), Matched == SlotMaterial ? TEXT("") : TEXT(" (parent)")));
		}

		if (bHasPattern)
		{
			TArray<TPair<const TCHAR*, FString>> Names;
			if (const UObject* Mesh = MeshAssetOf(Component))
			{
				const FString SlotName = SlotNameOf(Component, MaterialIndex);
				if (!SlotName.IsEmpty())
				{
					Names.Emplace(TEXT("slot name"), SlotName);
				}
				Names.Emplace(TEXT("mesh"), Mesh->GetName());
			}
			for (const UMaterialInterface* It = SlotMaterial; It; )
			{
				Names.Emplace(It == SlotMaterial ? TEXT("material") : TEXT("parent material"), It->GetName());
				const UMaterialInstance* Instance = Cast<UMaterialInstance>(It);
				It = Instance ? Instance->Parent.Get() : nullptr;
			}
			Names.Emplace(TEXT("component"), Component->GetName());
			if (Owner)
			{
				Names.Emplace(TEXT("actor label"), Owner->GetActorLabel());
			}

			const TPair<const TCHAR*, FString>* Hit = Names.FindByPredicate([&Rule](const TPair<const TCHAR*, FString>& Name)
			{
				return Name.Value.MatchesWildcard(Rule.NamePattern, ESearchCase::IgnoreCase);
			});
			if (!Hit)
			{
				return false;
			}
			Why.Add(FString::Printf(TEXT("%s '%s' matches '%s'"), Hit->Key, *Hit->Value, *Rule.NamePattern));
		}

		if (OutWhy)
		{
			*OutWhy = FString::Join(Why, TEXT(", "));
		}
		return true;
	}
}

// ------------------------------------------------------------ Gate sampler

bool FIcifyGateSampler::Init(UTexture2D* Texture, FString& OutError)
{
	Width = Height = BytesPerPixel = 0;
	Pixels.Reset();
	ContentHash = 0;
	if (!Texture)
	{
		OutError = TEXT("no gate texture");
		return false;
	}

	FTextureSource& Source = Texture->Source;
	const ETextureSourceFormat Format = Source.GetFormat();
	if (Format == TSF_BGRA8)
	{
		BytesPerPixel = 4;
	}
	else if (Format == TSF_G8)
	{
		BytesPerPixel = 1;
	}
	else
	{
		OutError = FString::Printf(TEXT("gate texture %s must have an 8-bit source (BGRA8 or G8)"), *Texture->GetName());
		return false;
	}

	if (!Source.GetMipData(Pixels, 0))
	{
		OutError = FString::Printf(TEXT("could not read source data of gate texture %s"), *Texture->GetName());
		return false;
	}

	Width = Source.GetSizeX();
	Height = Source.GetSizeY();
	if (Width <= 0 || Height <= 0 || Pixels.Num() < int64(Width) * Height * BytesPerPixel)
	{
		OutError = FString::Printf(TEXT("gate texture %s has unexpected source size"), *Texture->GetName());
		return false;
	}
	ContentHash = HashCombine(HashCombine(GetTypeHash(Width), GetTypeHash(Height)), GetTypeHash(BytesPerPixel));
	for (int64 Offset = 0; Offset < Pixels.Num();)
	{
		const int32 Count = int32(FMath::Min<int64>(Pixels.Num() - Offset, MAX_int32));
		ContentHash = FCrc::MemCrc32(Pixels.GetData() + Offset, Count, ContentHash);
		Offset += Count;
	}
	return true;
}

float FIcifyGateSampler::Sample(const FVector2f& UV, EIcifyChannel Channel) const
{
	if (Width <= 0)
	{
		return 1.f;
	}

	const float U = UV.X - FMath::FloorToFloat(UV.X);
	const float V = UV.Y - FMath::FloorToFloat(UV.Y);
	const int32 X = FMath::Clamp(int32(U * Width), 0, Width - 1);
	const int32 Y = FMath::Clamp(int32(V * Height), 0, Height - 1);
	const uint8* Pixel = Pixels.GetData() + (int64(Y) * Width + X) * BytesPerPixel;

	if (BytesPerPixel == 1)
	{
		return Pixel[0] / 255.f;
	}

	// BGRA byte order.
	switch (Channel)
	{
	case EIcifyChannel::R: return Pixel[2] / 255.f;
	case EIcifyChannel::G: return Pixel[1] / 255.f;
	case EIcifyChannel::B: return Pixel[0] / 255.f;
	default:               return Pixel[3] / 255.f;
	}
}

namespace Icify
{
	uint32 HashSlotContext(const FIcifyComponentContext& Context)
	{
		uint32 Hash = HashCombine(GetTypeHash(Context.Slots.Num()), uint32(Context.bDripSourceMesh));
		auto Add = [&Hash](uint32 Value) { Hash = HashCombine(Hash, Value); };
		for (const FIcifySlotInfo& Slot : Context.Slots)
		{
			Add(uint32(Slot.Surface));
			Add(uint32(Slot.bExcluded));
			Add(uint32(Slot.bIcicles));
			for (float Value : { Slot.Profile.Frost, Slot.Profile.Snow, Slot.Profile.Glaze, Slot.Profile.BlackIce, Slot.Profile.Icicles })
			{
				Add(GetTypeHash(Value));
			}
			Add(uint32(Slot.Gate != nullptr));
			if (const FIcifyGate* Gate = Slot.Gate)
			{
				Add(uint32(Gate->bUseFacingGate));
				Add(GetTypeHash(Gate->MinUpFacing));
				Add(GetTypeHash(Gate->MaxUpFacing));
				Add(uint32(Gate->bUseHeightGate));
				Add(GetTypeHash(Gate->MinHeightCm));
				Add(GetTypeHash(Gate->MaxHeightCm));
				Add(GetTypeHash(Gate->GateUVChannel));
				Add(uint32(Gate->GateChannel));
				Add(uint32(Gate->bInvertGate));
				Add(GetTypeHash(Gate->GateTexture ? Gate->GateTexture->GetPathName() : FString()));
			}
			Add(uint32(Slot.GateSampler != nullptr));
			if (Slot.GateSampler)
			{
				Add(Slot.GateSampler->Hash());
			}
		}
		return Hash;
	}
}

// ------------------------------------------------------------ Segment field

void FIcifySegmentField::Reset(double InCellSize)
{
	Segments.Reset();
	Cells.Reset();
	CellSize = FMath::Max(InCellSize, 5.0);
}

void FIcifySegmentField::Add(const FSegment& Segment)
{
	const int32 Index = Segments.Add(Segment);
	const double Length = FVector::Dist(Segment.A, Segment.B);
	const int32 Steps = FMath::Max(1, FMath::CeilToInt(Length / (CellSize * 0.5)));
	const int32 Reach = FMath::Max(1, FMath::CeilToInt(Segment.Radius / CellSize));
	TSet<FIntVector> Touched;
	for (int32 Step = 0; Step <= Steps; ++Step)
	{
		const FIntVector Center = CellOf(FMath::Lerp(Segment.A, Segment.B, double(Step) / Steps));
		for (int32 Z = -Reach; Z <= Reach; ++Z)
		{
			for (int32 Y = -Reach; Y <= Reach; ++Y)
			{
				for (int32 X = -Reach; X <= Reach; ++X)
				{
					const FIntVector Cell = Center + FIntVector(X, Y, Z);
					if (!Touched.Contains(Cell))
					{
						Touched.Add(Cell);
						Cells.FindOrAdd(Cell).Add(Index);
					}
				}
			}
		}
	}
}

uint32 FIcifySegmentField::Hash() const
{
	uint32 Hash = GetTypeHash(Segments.Num());
	for (const FSegment& Seg : Segments)
	{
		Hash = HashCombine(Hash, GetTypeHash(Seg.A));
		Hash = HashCombine(Hash, GetTypeHash(Seg.B));
		Hash = HashCombine(Hash, GetTypeHash(Seg.Radius));
		Hash = HashCombine(Hash, GetTypeHash(Seg.Value));
		Hash = HashCombine(Hash, GetTypeHash(Seg.AlongA));
		Hash = HashCombine(Hash, GetTypeHash(Seg.AlongB));
	}
	return Hash;
}

namespace Icify
{
	// ------------------------------------------------------- Meltwater paths

	void TraceMeltwaterPath(UWorld* World, const FVector& Start, float StepCm, int32 MaxSteps, const AActor* IgnoreActor,
		TArray<FVector>& OutPath, TArray<FIcifyPool>& OutPools)
	{
		OutPath.Reset();
		OutPath.Add(Start);
		if (!World)
		{
			return;
		}

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(IcifyMeltwaterTrace), /*bTraceComplex*/ true);
		if (IgnoreActor)
		{
			QueryParams.AddIgnoredActor(IgnoreActor);
		}

		const double Step = FMath::Max(StepCm, 2.f);
		const FVector Gravity(0.0, 0.0, -1.0);
		FVector P = Start;
		for (int32 Index = 0; Index < MaxSteps; ++Index)
		{
			FHitResult Hit;
			const FVector Next = P + Gravity * Step;
			if (!World->LineTraceSingleByChannel(Hit, P, Next, ECC_Visibility, QueryParams))
			{
				// Falling, or running down a wall just beside it.
				P = Next;
				OutPath.Add(P);
				continue;
			}

			const FVector N = Hit.ImpactNormal;
			if (N.Z < 0.2)
			{
				// Overhang or steep face: cling to it and keep going down.
				P = Hit.ImpactPoint + N * 1.0 + Gravity * (Step * 0.5);
				OutPath.Add(P);
				continue;
			}

			// On a ledge or slope: run downhill along it; on flat ground, pool (and freeze).
			FVector Downhill = Gravity - N * FVector::DotProduct(Gravity, N);
			if (Downhill.Size() < 0.15)
			{
				OutPath.Add(Hit.ImpactPoint);
				FIcifyPool& Pool = OutPools.AddDefaulted_GetRef();
				Pool.Center = Hit.ImpactPoint;
				Pool.Radius = 30.f;
				return;
			}
			Downhill.Normalize();
			P = Hit.ImpactPoint + N * 1.0 + Downhill * Step;
			OutPath.Add(P);
		}
	}

	float StampWeight(const FIcifyStampData& Stamp, const FVector& P, float FeatureScale)
	{
		// Distance in the stamp's own unit-sphere space, so scaling the actor stretches it into an ellipsoid.
		const FVector Local = Stamp.Transform.InverseTransformPosition(P) / FMath::Max(Stamp.Radius, 1.f);
		float Distance = float(Local.Size());
		if (Stamp.EdgeNoise > 0.f)
		{
			const FVector NP = P / FMath::Max(FeatureScale, 1.f);
			Distance += (Fbm(NP * 1.3, 4242u) - 0.5f) * Stamp.EdgeNoise;
		}
		if (Distance >= 1.f)
		{
			return 0.f;
		}
		const float Inner = 1.f - FMath::Clamp(Stamp.Falloff, 0.f, 1.f);
		return Distance <= Inner ? 1.f : 1.f - FMath::SmoothStep(Inner, 1.f, Distance);
	}

	float ThawAt(const FIcifyGenerationParams& Params, const FVector& P)
	{
		if (Params.ThawIntensity <= 0.f)
		{
			return 0.f;
		}
		float Thaw = 0.f;
		for (const FIcifyHeatSource& Heat : Params.HeatSources)
		{
			// Heat rises: it reaches further above a source than below it.
			FVector Rel = P - Heat.Location;
			Rel.Z = Rel.Z < 0.0 ? Rel.Z * 1.6 : Rel.Z * 0.8;
			const float Radius = FMath::Max(Heat.Radius, 1.f);
			const float Distance = float(Rel.Size());
			if (Distance < Radius)
			{
				Thaw = FMath::Max(Thaw, 1.f - FMath::SmoothStep(Radius * 0.35f, Radius, Distance));
			}
		}
		return FMath::Clamp(Thaw * Params.ThawIntensity, 0.f, 1.f);
	}
}

#endif // WITH_EDITOR
