// Copyright Fallen Signal Studios. All Rights Reserved.
// Regression coverage uses isolated transient editor worlds, never the editor's current map.
#include "IcifyGenerator.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "IcifyFreezeZone.h"
#include "IcifySettings.h"
#include "IcifyZoneComponent.h"
#include "MaterialDomain.h"
#include "ProceduralMeshComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshResources.h"

namespace
{
	struct FIcifyTestWorld
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
		UStaticMeshComponent* Component = nullptr;
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));

		FIcifyTestWorld()
		{
			if (World && Mesh)
			{
				AActor* Actor = World->SpawnActor<AActor>();
				Component = NewObject<UStaticMeshComponent>(Actor);
				Actor->AddInstanceComponent(Component);
				Actor->SetRootComponent(Component);
				Component->SetMobility(EComponentMobility::Static);
				Component->SetStaticMesh(Mesh);
				Component->RegisterComponent();
			}
		}

		~FIcifyTestWorld()
		{
			if (World)
			{
				World->DestroyWorld(false);
			}
		}

		AIcifyFreezeZone* Zone()
		{
			AIcifyFreezeZone* Result = World->SpawnActor<AIcifyFreezeZone>();
			Result->bLivePreview = false;
			Result->bWriteReportFiles = false;
			Result->bAutoDetectDripSources = false;
			Result->bUseSkyCover = false;
			Result->FloorMode = EIcifyFloorMode::ZoneBottom;
			Result->MaskOutput = EIcifyMaskOutput::VertexColor;
			Result->GetZoneComponent()->SetBoxExtent(FVector(500.));
			Result->IncludeOnlyActors.Add(Component->GetOwner());
			return Result;
		}
	};

	TArray<TArray<FColor>> ColorsFor(UStaticMesh* Mesh, FColor Value)
	{
		TArray<TArray<FColor>> Result;
		if (const FStaticMeshRenderData* Data = Mesh->GetRenderData())
		{
			Result.SetNum(Data->LODResources.Num());
			for (int32 I = 0; I < Result.Num(); ++I)
			{
				Result[I].Init(Value, Data->LODResources[I].GetNumVertices());
			}
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIcifyExactCPDTest, "Icify.State.ExactCPDRestoration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIcifyExactCPDTest::RunTest(const FString& Parameters)
{
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>();
	UIcifySettings* Settings = GetMutableDefault<UIcifySettings>();
	TGuardValue<int32> BaseGuard(Settings->CustomPrimitiveDataIndex, 0);
	const float Values[Icify::CPD_Count] = { Icify::MaskModeVertex, 0.f, 20.f, 12.f, 0.f };
	FIcifyComponentRecord Empty;
	Icify::CaptureRecord(Component, Empty);
	TestTrue(TEXT("Write empty arrays"), Icify::WriteCustomData(Component, Values));
	Settings->CustomPrimitiveDataIndex = 8;
	TestTrue(TEXT("Restore after changing base"), Icify::RestoreRecord(Empty, false));
	TestTrue(TEXT("Both arrays return to exact empty shape"), Component->GetDefaultCustomPrimitiveData().Data.IsEmpty()
		&& Component->GetCustomPrimitiveData().Data.IsEmpty());
	TestEqual(TEXT("Capture remembers original base"), Empty.CustomDataBase, 0);

	Component->SetDefaultCustomPrimitiveDataFloat(12, .2f);
	Component->SetCustomPrimitiveDataFloat(14, .37f);
	FIcifyComponentRecord Original;
	Icify::CaptureRecord(Component, Original);
	Settings->CustomPrimitiveDataIndex = 0;
	TestTrue(TEXT("Write mixed saved/live arrays"), Icify::WriteCustomData(Component, Values));
	TestEqual(TEXT("Unrelated live-only value survives write"), Component->GetCustomPrimitiveData().Data[14], .37f);
	Settings->CustomPrimitiveDataIndex = 8;
	TestTrue(TEXT("Restore mixed arrays after changing base"), Icify::RestoreRecord(Original, false));
	TestTrue(TEXT("Defaults restored exactly"), Component->GetDefaultCustomPrimitiveData().Data == Original.CustomData);
	TestTrue(TEXT("Live array restored exactly"), Component->GetCustomPrimitiveData().Data == Original.LiveCustomData);

	Component->SetCustomPrimitiveDataFloat(2, .5f);
	TestFalse(TEXT("Live-only reservation is occupied"), Icify::CustomDataSlotsFree(
		Component->GetDefaultCustomPrimitiveData().Data, Component->GetCustomPrimitiveData().Data, 0));
	TestFalse(TEXT("Out of range reservation rejected"), Icify::CustomDataSlotsFree({}, {}, FCustomPrimitiveData::NumCustomPrimitiveDataFloats - 1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIcifyPaintAndMaterialsTest, "Icify.State.PaintCacheAndMaterialArrays",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIcifyPaintAndMaterialsTest::RunTest(const FString& Parameters)
{
	FIcifyTestWorld Fixture;
	if (!TestNotNull(TEXT("Cube fixture"), Fixture.Component) || !TestNotNull(TEXT("Cube render data"), Fixture.Mesh->GetRenderData()))
	{
		return false;
	}
	const TArray<TArray<FColor>> Red = ColorsFor(Fixture.Mesh, FColor::Red);
	const TArray<TArray<FColor>> Blue = ColorsFor(Fixture.Mesh, FColor::Blue);
	TestTrue(TEXT("Original painted colors"), Icify::WriteVertexColors(Fixture.Component, Red));
	FIcifyComponentRecord Original;
	Icify::CaptureRecord(Fixture.Component, Original);
	TestTrue(TEXT("Preexisting paint cache populated"), Original.LODs[0].PaintedVertices.Num() > 1);
	TestTrue(TEXT("New painted colors"), Icify::WriteVertexColors(Fixture.Component, Blue));
	TestTrue(TEXT("New cache uses new colors"), Fixture.Component->LODData[0].PaintedVertices.ContainsByPredicate(
		[](const FPaintedVertex& V) { return V.Color == FColor::Blue; }));
	TestFalse(TEXT("Old colors absent from new remap cache"), Fixture.Component->LODData[0].PaintedVertices.ContainsByPredicate(
		[](const FPaintedVertex& V) { return V.Color == FColor::Red; }));
	TestTrue(TEXT("Restore original paint"), Icify::RestoreRecord(Original, false));
	FIcifyComponentRecord Restored;
	Icify::CaptureRecord(Fixture.Component, Restored);
	TestTrue(TEXT("Override buffer restored"), Restored.LODs[0].Colors == Original.LODs[0].Colors);
	TestEqual(TEXT("LOD count restored"), Restored.LODDataCount, Original.LODDataCount);
	TestEqual(TEXT("DDC key restored"), Restored.StaticMeshDerivedDataKey, Original.StaticMeshDerivedDataKey);
	TestEqual(TEXT("Paint cache count restored"), Restored.LODs[0].PaintedVertices.Num(), Original.LODs[0].PaintedVertices.Num());
	for (int32 I = 0; I < Original.LODs[0].PaintedVertices.Num(); ++I)
	{
		TestTrue(TEXT("Painted vertex restored"), Restored.LODs[0].PaintedVertices[I].Position == Original.LODs[0].PaintedVertices[I].Position
			&& Restored.LODs[0].PaintedVertices[I].Normal == Original.LODs[0].PaintedVertices[I].Normal
			&& Restored.LODs[0].PaintedVertices[I].Color == Original.LODs[0].PaintedVertices[I].Color);
	}

	for (bool bShortArray : { false, true })
	{
		Fixture.Component->OverrideMaterials.Reset();
		if (bShortArray)
		{
			Fixture.Component->OverrideMaterials.Add(nullptr);
		}
		FIcifyComponentRecord Before;
		Icify::CaptureRecord(Fixture.Component, Before);
		Fixture.Component->SetMaterial(2, UMaterial::GetDefaultMaterial(MD_Surface));
		TestTrue(TEXT("Restore override material array"), Icify::RestoreRecord(Before, false));
		TestTrue(TEXT("Empty/short/null raw array shape restored"), Fixture.Component->OverrideMaterials == Before.OverrideMaterials);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIcifyOverlapTest, "Icify.State.OverlapOwnershipAndOccupiedLiveData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIcifyOverlapTest::RunTest(const FString& Parameters)
{
	FIcifyTestWorld Fixture;
	if (!TestNotNull(TEXT("Cube fixture"), Fixture.Component))
	{
		return false;
	}
	TGuardValue<int32> BaseGuard(GetMutableDefault<UIcifySettings>()->CustomPrimitiveDataIndex, 0);
	FIcifyComponentRecord Before;
	Icify::CaptureRecord(Fixture.Component, Before);
	AIcifyFreezeZone* A = Fixture.Zone();
	AIcifyFreezeZone* B = Fixture.Zone();
	B->OverlapPolicy = EIcifyOverlapPolicy::TakeOver;
	A->Apply();
	TestNotNull(TEXT("A owns applied component"), A->FindAppliedRecord(Fixture.Component));
	B->Preview();
	TestFalse(TEXT("B cannot preview another applied owner"), B->IsPreviewing(Fixture.Component));
	A->Clear();
	B->CancelPreview();
	TestTrue(TEXT("Cancel cannot resurrect cleared owner"), Fixture.Component->GetDefaultCustomPrimitiveData().Data == Before.CustomData
		&& Fixture.Component->GetCustomPrimitiveData().Data == Before.LiveCustomData
		&& Fixture.Component->OverrideMaterials == Before.OverrideMaterials);
	A->Apply();
	B->Preview();
	A->Apply();
	B->CancelPreview();
	TestNotNull(TEXT("A keeps ownership during blocked B preview"), A->FindAppliedRecord(Fixture.Component));
	B->Apply();
	TestNull(TEXT("Apply transfers ownership from A"), A->FindAppliedRecord(Fixture.Component));
	TestNotNull(TEXT("B receives original backup"), B->FindAppliedRecord(Fixture.Component));
	B->Clear();
	TestTrue(TEXT("Takeover Clear restores original arrays"), Fixture.Component->GetDefaultCustomPrimitiveData().Data == Before.CustomData
		&& Fixture.Component->GetCustomPrimitiveData().Data == Before.LiveCustomData
		&& Fixture.Component->OverrideMaterials == Before.OverrideMaterials);

	Fixture.Component->SetCustomPrimitiveDataFloat(0, .5f);
	FIcifyComponentRecord Occupied;
	Icify::CaptureRecord(Fixture.Component, Occupied);
	A->Apply();
	TestNull(TEXT("Live-only occupied slot gives no ownership"), A->FindAppliedRecord(Fixture.Component));
	TestTrue(TEXT("Conflict leaves full state untouched"), Fixture.Component->GetCustomPrimitiveData().Data == Occupied.LiveCustomData
		&& Fixture.Component->GetDefaultCustomPrimitiveData().Data == Occupied.CustomData
		&& Fixture.Component->OverrideMaterials == Occupied.OverrideMaterials);
	TestTrue(TEXT("Conflict is reported"), A->GetLastReport().Components.ContainsByPredicate(
		[](const FIcifyComponentReport& Entry) { return Entry.bCustomDataConflict; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIcifyMaterialOwnershipTest, "Icify.State.AdmittedMaterialOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIcifyMaterialOwnershipTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Mesh = NewObject<UStaticMesh>();
	UMaterialInterface* Original = UMaterial::GetDefaultMaterial(MD_Surface);
	Mesh->GetStaticMaterials().Add(FStaticMaterial(Original));
	Mesh->GetStaticMaterials().Add(FStaticMaterial(Original));
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>();
	Component->SetStaticMesh(Mesh);
	FIcifyComponentRecord Before;
	Icify::CaptureRecord(Component, Before);
	const TArray<EIcifySurfaceType> Surfaces = { EIcifySurfaceType::Glass, EIcifySurfaceType::Auto };
	TestTrue(TEXT("Own admitted vertex slot"), Icify::BindOwnedMaterials(Component, nullptr, Surfaces, nullptr));
	UMaterialInstanceConstant* Owned = Cast<UMaterialInstanceConstant>(Component->GetMaterial(0));
	TestNotNull(TEXT("Admitted slot has owned MIC"), Owned);
	TestTrue(TEXT("MIC belongs to component"), Owned && Owned->GetOuter() == Component);
	TestTrue(TEXT("Owned MIC carries the Icify prefix"), Owned && Owned->GetName().StartsWith(Icify::OwnedInstancePrefix()));
	TestTrue(TEXT("Excluded effective material unchanged"), Component->GetMaterial(1) == Original);
	TestEqual(TEXT("Excluded raw slot remains absent"), Component->OverrideMaterials.Num(), 1);
	if (Owned)
	{
		float Active = 0.f;
		float Surface = -1.f;
		TestTrue(TEXT("Owned activation parameter"), Owned->GetScalarParameterValue(FMaterialParameterInfo(Icify::ActiveParameterName()), Active));
		TestEqual(TEXT("Owned activation enabled"), Active, 1.f);
		TestTrue(TEXT("Per-slot surface parameter"), Owned->GetScalarParameterValue(FMaterialParameterInfo(Icify::SurfaceOverrideParameterName()), Surface));
		TestEqual(TEXT("Glass slot uses the glass family"), Surface, 2.f);
	}
	TestTrue(TEXT("Clear raw materials"), Icify::RestoreRecord(Before, false));
	TestTrue(TEXT("Original raw array shape"), Component->OverrideMaterials == Before.OverrideMaterials);

	// An unrelated component-owned MIC is not ours merely because it has the same Outer.
	UMaterialInstanceConstant* Excluded = NewObject<UMaterialInstanceConstant>(Component);
	Excluded->SetParentEditorOnly(Original);
	Excluded->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Icify::UseUV0ParameterName()), .25f);
	Component->SetMaterial(1, Excluded);
	Icify::CaptureRecord(Component, Before);
	const TArray<FScalarParameterValue> ExcludedScalars = Excluded->ScalarParameterValues;
	const TArray<FTextureParameterValue> ExcludedTextures = Excluded->TextureParameterValues;
	TestTrue(TEXT("Bind UV0 fallback only to admitted slot"), Icify::BindOwnedMaterials(Component, nullptr, Surfaces, nullptr, nullptr, /*bUseUV0*/ true));
	Owned = Cast<UMaterialInstanceConstant>(Component->GetMaterial(0));
	float UseUV0 = 0.f;
	TestTrue(TEXT("New admitted MIC selects actual UV0 bake"), Owned
		&& Owned->GetScalarParameterValue(FMaterialParameterInfo(Icify::UseUV0ParameterName()), UseUV0) && UseUV0 == 1.f);
	TestTrue(TEXT("Excluded MIC identity and all scalar/texture overrides held"), Component->GetMaterial(1) == Excluded
		&& Excluded->ScalarParameterValues == ExcludedScalars && Excluded->TextureParameterValues == ExcludedTextures);
	TestTrue(TEXT("Restore fallback bindings"), Icify::RestoreRecord(Before, false));
	TestTrue(TEXT("Excluded preexisting MIC remains exact after restoration"), Component->OverrideMaterials == Before.OverrideMaterials
		&& Excluded->ScalarParameterValues == ExcludedScalars && Excluded->TextureParameterValues == ExcludedTextures);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIcifyLevelRemovalTest, "Icify.State.EditorLevelRemovalRestoresPreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIcifyLevelRemovalTest::RunTest(const FString& Parameters)
{
	FIcifyTestWorld Fixture;
	if (!TestNotNull(TEXT("Cube fixture"), Fixture.Component))
	{
		return false;
	}
	TGuardValue<int32> BaseGuard(GetMutableDefault<UIcifySettings>()->CustomPrimitiveDataIndex, 0);
	FIcifyComponentRecord Before;
	Icify::CaptureRecord(Fixture.Component, Before);
	AIcifyFreezeZone* Zone = Fixture.Zone();
	Zone->Preview();
	TestTrue(TEXT("Preview active before level event"), Zone->IsPreviewing(Fixture.Component));
	Fixture.World->PersistentLevel->OnLoadedActorRemovedFromLevelEvent.Broadcast(*Zone);
	TestFalse(TEXT("Real editor module level event retires preview"), Zone->bPreviewActive);
	TestTrue(TEXT("Level removal restores both arrays/materials"), Fixture.Component->GetDefaultCustomPrimitiveData().Data == Before.CustomData
		&& Fixture.Component->GetCustomPrimitiveData().Data == Before.LiveCustomData
		&& Fixture.Component->OverrideMaterials == Before.OverrideMaterials);
	TestEqual(TEXT("Level removal drops the preview icicles"), Zone->GetPreviewIcicleComponent()->GetNumSections(), 0);
	return true;
}

#endif
