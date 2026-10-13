// Copyright Fallen Signal Studios. All Rights Reserved.
// Icicle growth through the real zone: traces, the sky cover field, the procedural mesh components and undo-safe state.
// Runs in an isolated transient editor world, never the editor's current map.
#include "IcifyGenerator.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "IcifyFreezeZone.h"
#include "IcifySettings.h"
#include "IcifyZoneComponent.h"
#include "Misc/AutomationTest.h"
#include "ProceduralMeshComponent.h"

namespace
{
	struct FIcicleWorld
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		AActor* Slab = nullptr;
		AActor* Floor = nullptr;

		FIcicleWorld()
		{
			if (World && Cube)
			{
				// A 200 x 40 x 20 cm slab floating with its underside at 190 cm, over a floor whose top is at 0.
				Slab = SpawnCube(FVector(0.0, 0.0, 200.0), FVector(2.0, 0.4, 0.2));
				Floor = SpawnCube(FVector(0.0, 0.0, -5.0), FVector(10.0, 10.0, 0.1));
			}
		}

		~FIcicleWorld()
		{
			if (World)
			{
				World->DestroyWorld(false);
			}
		}

		AActor* SpawnCube(const FVector& Location, const FVector& Scale)
		{
			AActor* Actor = World->SpawnActor<AActor>();
			UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor);
			Actor->AddInstanceComponent(Component);
			Actor->SetRootComponent(Component);
			Component->SetMobility(EComponentMobility::Static);
			Component->SetStaticMesh(Cube);
			Component->SetWorldLocationAndRotation(Location, FQuat::Identity);
			Component->SetWorldScale3D(Scale);
			Component->RegisterComponent();
			return Actor;
		}

		AIcifyFreezeZone* Zone()
		{
			AIcifyFreezeZone* Result = World->SpawnActor<AIcifyFreezeZone>();
			Result->bLivePreview = false;
			Result->bWriteReportFiles = false;
			Result->bAutoDetectDripSources = false;
			Result->FloorMode = EIcifyFloorMode::ZoneBottom;
			Result->MaskOutput = EIcifyMaskOutput::VertexColor;
			Result->GetZoneComponent()->SetBoxExtent(FVector(400.0, 400.0, 400.0));
			Result->IncludeOnlyActors.Add(Slab);
			// Plenty of candidates and no clumping, so every edge is sure to grow some.
			Result->IcicleDensity = 20.f;
			Result->IcicleClumping = 0.f;
			Result->IcicleMaxLengthCm = 40.f;
			return Result;
		}
	};

	int32 SectionVertices(const UProceduralMeshComponent* Mesh)
	{
		int32 Count = 0;
		for (int32 Index = 0; Index < Mesh->GetNumSections(); ++Index)
		{
			Count += Mesh->GetProcMeshSection(Index)->ProcVertexBuffer.Num();
		}
		return Count;
	}

	/** Every vertex of every section, in world space, folded into one hash. */
	uint32 GeometryHash(UProceduralMeshComponent* Mesh)
	{
		uint32 Hash = 0;
		const FTransform& Transform = Mesh->GetComponentTransform();
		for (int32 Index = 0; Index < Mesh->GetNumSections(); ++Index)
		{
			for (const FProcMeshVertex& Vertex : Mesh->GetProcMeshSection(Index)->ProcVertexBuffer)
			{
				const FVector P = Transform.TransformPosition(Vertex.Position);
				Hash = HashCombine(Hash, GetTypeHash(FIntVector(FMath::RoundToInt(P.X * 100.0), FMath::RoundToInt(P.Y * 100.0), FMath::RoundToInt(P.Z * 100.0))));
			}
		}
		return Hash;
	}

	/** World-space bounds of all sections. */
	FBox GeometryBounds(UProceduralMeshComponent* Mesh)
	{
		FBox Box(ForceInit);
		const FTransform& Transform = Mesh->GetComponentTransform();
		for (int32 Index = 0; Index < Mesh->GetNumSections(); ++Index)
		{
			for (const FProcMeshVertex& Vertex : Mesh->GetProcMeshSection(Index)->ProcVertexBuffer)
			{
				Box += Transform.TransformPosition(Vertex.Position);
			}
		}
		return Box;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIcifyIcicleRoundTripTest, "Icify.Icicles.PreviewApplyClearRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIcifyIcicleRoundTripTest::RunTest(const FString& Parameters)
{
	FIcicleWorld Fixture;
	if (!TestNotNull(TEXT("Slab fixture"), Fixture.Slab))
	{
		return false;
	}
	TGuardValue<int32> BaseGuard(GetMutableDefault<UIcifySettings>()->CustomPrimitiveDataIndex, 0);
	AIcifyFreezeZone* Zone = Fixture.Zone();
	UProceduralMeshComponent* Applied = Zone->GetIcicleComponent();
	UProceduralMeshComponent* Previewed = Zone->GetPreviewIcicleComponent();
	if (!TestNotNull(TEXT("Applied icicle component"), Applied) || !TestNotNull(TEXT("Preview icicle component"), Previewed))
	{
		return false;
	}

	// Preview: icicles grow only in the preview component, under the slab and above the floor.
	Zone->Preview();
	const FIcifyIcicleStats Stats = Zone->GetLastReport().IcicleStats;
	TestTrue(TEXT("Drip edges found on the slab"), Stats.Candidates > 0);
	TestTrue(TEXT("Icicles grew"), Stats.Icicles > 0);
	TestTrue(TEXT("Preview icicles are in the preview component"), Previewed->GetNumSections() > 0);
	TestEqual(TEXT("Nothing applied yet"), Applied->GetNumSections(), 0);
	TestEqual(TEXT("Report counts the built vertices"), Stats.Vertices, SectionVertices(Previewed));
	TestTrue(TEXT("Icicles are counted on the slab's report row"), Zone->GetLastReport().Components.ContainsByPredicate(
		[](const FIcifyComponentReport& Row) { return Row.Icicles > 0; }));
	const FBox Bounds = GeometryBounds(Previewed);
	TestTrue(TEXT("Icicles hang from the slab's underside"), Bounds.IsValid && Bounds.Max.Z <= 193.0 && Bounds.Max.Z >= 189.0);
	TestTrue(TEXT("Icicles stop above the floor"), Bounds.Min.Z > 0.0);
	TestTrue(TEXT("Icicles stay along the slab's edges"), Bounds.Min.X > -110.0 && Bounds.Max.X < 110.0 && Bounds.Min.Y > -30.0 && Bounds.Max.Y < 30.0);
	TestTrue(TEXT("No icicle is longer than the maximum (plus the embedded root)"), Stats.LongestCm <= Zone->IcicleMaxLengthCm + 0.01f);

	// Determinism: the same seed grows exactly the same geometry; another seed grows different icicles.
	const uint32 First = GeometryHash(Previewed);
	Zone->Preview();
	TestEqual(TEXT("Same seed, same icicles"), GeometryHash(Previewed), First);
	const int32 OriginalSeed = Zone->Seed;
	Zone->Seed = OriginalSeed + 7919;
	Zone->Preview();
	TestNotEqual(TEXT("New seed, new icicles"), GeometryHash(Previewed), First);
	Zone->Seed = OriginalSeed;

	// Apply: the applied component takes the icicles; the preview empties.
	Zone->Apply();
	const int32 AppliedCount = Zone->GetAppliedIcicleCount();
	TestTrue(TEXT("Applied icicles"), AppliedCount > 0 && Applied->GetNumSections() > 0);
	TestEqual(TEXT("Applied count matches the report"), AppliedCount, Zone->GetLastReport().IcicleStats.Icicles);
	TestEqual(TEXT("Preview emptied by Apply"), Previewed->GetNumSections(), 0);
	TestTrue(TEXT("Applied icicles visible"), Applied->GetVisibleFlag());
	TestEqual(TEXT("Apply grows the same icicles the preview showed"), GeometryHash(Applied), First);

	// Preview over Apply hides the applied icicles; Cancel brings them back.
	Zone->IcicleMaxLengthCm = 20.f;
	Zone->Preview();
	TestFalse(TEXT("Applied icicles hidden behind the preview"), Applied->GetVisibleFlag());
	TestTrue(TEXT("Preview shows its own icicles"), Previewed->GetNumSections() > 0);
	TestTrue(TEXT("Preview is active"), Zone->bPreviewActive);
	Zone->CancelPreview();
	TestTrue(TEXT("Cancel shows the applied icicles again"), Applied->GetVisibleFlag());
	TestEqual(TEXT("Cancel empties the preview"), Previewed->GetNumSections(), 0);
	TestEqual(TEXT("Cancel leaves the applied icicles alone"), GeometryHash(Applied), First);
	TestFalse(TEXT("No preview left"), Zone->bPreviewActive);
	Zone->IcicleMaxLengthCm = 40.f;

	// A roof overhead keeps the snow, and so the meltwater, off the slab.
	AActor* Roof = Fixture.SpawnCube(FVector(0.0, 0.0, 380.0), FVector(4.0, 4.0, 0.1));
	Zone->DryRun();
	TestEqual(TEXT("No icicles under a roof"), Zone->GetLastReport().IcicleStats.Icicles, 0);
	TestTrue(TEXT("Covered edges report no meltwater"), Zone->GetLastReport().IcicleStats.NoMeltwater > 0);
	Zone->bIciclesNeedMeltwater = false;
	Zone->DryRun();
	TestTrue(TEXT("Without the meltwater rule they grow anyway"), Zone->GetLastReport().IcicleStats.Icicles > 0);
	Zone->bIciclesNeedMeltwater = true;
	Roof->Destroy();

	// The NoIcicles tag turns them off on that mesh.
	UStaticMeshComponent* SlabMesh = Fixture.Slab->FindComponentByClass<UStaticMeshComponent>();
	SlabMesh->ComponentTags.Add(UIcifySettings::Get()->NoIciclesTag);
	Zone->DryRun();
	TestEqual(TEXT("NoIcicles tag: no candidates"), Zone->GetLastReport().IcicleStats.Candidates, 0);
	SlabMesh->ComponentTags.Remove(UIcifySettings::Get()->NoIciclesTag);

	// Clear removes them.
	Zone->Clear();
	TestEqual(TEXT("Clear removes the icicles"), Applied->GetNumSections(), 0);
	TestEqual(TEXT("Clear resets the count"), Zone->GetAppliedIcicleCount(), 0);
	TestNull(TEXT("Clear releases the slab"), Zone->FindAppliedRecord(SlabMesh));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIcifySkyCoverTraceTest, "Icify.Icicles.SkyCoverFirstHit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIcifySkyCoverTraceTest::RunTest(const FString& Parameters)
{
	FIcicleWorld Fixture;
	if (!TestNotNull(TEXT("Slab fixture"), Fixture.Slab))
	{
		return false;
	}
	FIcifySkyCoverField Sky;
	const FBox ZoneBox(FVector(-300.0, -300.0, -50.0), FVector(300.0, 300.0, 300.0));
	TestTrue(TEXT("Traced"), Sky.Build(Fixture.World, ZoneBox, FVector(0.0, 0.0, -1.0), 10.0, 1000.0, nullptr));
	TestTrue(TEXT("Field built"), Sky.IsValid());

	double Height = 0.0;
	const AActor* Cover = nullptr;
	TestTrue(TEXT("Under the slab, the slab is the first hit"), Sky.FirstHitAbove(FVector(0.0, 0.0, 150.0), Height, Cover));
	TestTrue(TEXT("It is the slab actor"), Cover == Fixture.Slab);
	TestTrue(TEXT("Its top is 60 cm above"), FMath::IsNearlyEqual(Height, 60.0, 6.0));
	TestTrue(TEXT("Under the slab is covered"), Sky.OpenAt(FVector(0.0, 0.0, 150.0)) < 0.01f);
	TestTrue(TEXT("Beside the slab is open"), Sky.OpenAt(FVector(200.0, 200.0, 150.0)) > 0.99f);
	TestFalse(TEXT("Above the slab nothing covers"), Sky.FirstHitAbove(FVector(0.0, 0.0, 250.0), Height, Cover));
	return !HasAnyErrors();
}

#endif
