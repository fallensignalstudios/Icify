// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifyBlueprintLibrary.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "IcifyFreezeZone.h"
#include "IcifyPreset.h"
#include "IcifyZoneComponent.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_EDITOR
#include "ScopedTransaction.h"
#endif

#define LOCTEXT_NAMESPACE "IcifyBlueprintLibrary"

namespace
{
	UWorld* WorldFrom(const UObject* WorldContextObject)
	{
		return GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull) : nullptr;
	}

	void CollectZones(UWorld* World, TArray<AIcifyFreezeZone*>& OutZones)
	{
		if (!World)
		{
			return;
		}
		for (TActorIterator<AIcifyFreezeZone> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				OutZones.Add(*It);
			}
		}
	}
}

TArray<AIcifyFreezeZone*> UIcifyBlueprintLibrary::GetZones(const UObject* WorldContextObject)
{
	TArray<AIcifyFreezeZone*> Zones;
	CollectZones(WorldFrom(WorldContextObject), Zones);
	return Zones;
}

AIcifyFreezeZone* UIcifyBlueprintLibrary::FindZoneByLabel(const UObject* WorldContextObject, const FString& Label)
{
	TArray<AIcifyFreezeZone*> Zones;
	CollectZones(WorldFrom(WorldContextObject), Zones);
	for (AIcifyFreezeZone* Zone : Zones)
	{
		// GetActorNameOrLabel is the label in the editor and the object name in game builds (labels are editor-only).
		if (Zone->GetActorNameOrLabel().Equals(Label, ESearchCase::IgnoreCase) || Zone->GetName().Equals(Label, ESearchCase::IgnoreCase))
		{
			return Zone;
		}
	}
	return nullptr;
}

FIcifyRunReport UIcifyBlueprintLibrary::RunZone(AIcifyFreezeZone* Zone, EIcifyOperation Operation)
{
	if (!IsValid(Zone))
	{
		return FIcifyRunReport();
	}

	switch (Operation)
	{
	case EIcifyOperation::DryRun: Zone->DryRun(); break;
	case EIcifyOperation::Preview: Zone->Preview(); break;
	case EIcifyOperation::Apply: Zone->Apply(); break;
	case EIcifyOperation::Clear: Zone->Clear(); break;
	case EIcifyOperation::CancelPreview: Zone->CancelPreview(); break;
	}
	return Zone->GetLastReport();
}

TArray<FIcifyRunReport> UIcifyBlueprintLibrary::RunAllZones(const UObject* WorldContextObject, EIcifyOperation Operation)
{
	TArray<AIcifyFreezeZone*> Zones;
	CollectZones(WorldFrom(WorldContextObject), Zones);

	TArray<FIcifyRunReport> Reports;
	Reports.Reserve(Zones.Num());

#if WITH_EDITOR
	// Groups the zones' own transactions so one undo reverts the whole batch, like the toolbar's All Zones actions.
	const FScopedTransaction Transaction(LOCTEXT("AllZones", "Icify: All Zones"), Zones.Num() > 0);
#endif
	for (AIcifyFreezeZone* Zone : Zones)
	{
		Reports.Add(RunZone(Zone, Operation));
	}
	return Reports;
}

AIcifyFreezeZone* UIcifyBlueprintLibrary::SpawnZone(const UObject* WorldContextObject, const FTransform& Transform, FVector HalfExtent, UIcifyPreset* Preset)
{
	UWorld* World = WorldFrom(WorldContextObject);
	if (!World)
	{
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AIcifyFreezeZone* Zone = World->SpawnActor<AIcifyFreezeZone>(AIcifyFreezeZone::StaticClass(), Transform, Params);
	if (!Zone)
	{
		return nullptr;
	}

	if (UIcifyZoneComponent* Box = Zone->GetZoneComponent())
	{
		Box->SetBoxExtent(HalfExtent.ComponentMax(FVector(1.0)));
	}

	if (Preset)
	{
		Zone->Preset = Preset;
#if WITH_EDITOR
		// Loading a preset refreshes a live preview; a script spawning zones should decide itself when to generate.
		const bool bWasLive = Zone->bLivePreview;
		Zone->bLivePreview = false;
		Zone->LoadPreset();
		Zone->bLivePreview = bWasLive;
#endif
	}
	return Zone;
}

FString UIcifyBlueprintLibrary::ReportToJson(const FIcifyRunReport& Report)
{
	FString Json;
	if (!FJsonObjectConverter::UStructToJsonObjectString(Report, Json))
	{
		return FString();
	}
	return Json;
}

bool UIcifyBlueprintLibrary::SaveReport(const FIcifyRunReport& Report, const FString& FilePath)
{
	if (FilePath.IsEmpty())
	{
		return false;
	}

	const FString FullPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), FilePath);
	const FString Json = ReportToJson(Report);
	if (Json.IsEmpty() || !IFileManager::Get().MakeDirectory(*FPaths::GetPath(FullPath), true) || !FFileHelper::SaveStringToFile(Json, *FullPath))
	{
		UE_LOG(LogIcify, Warning, TEXT("Could not write Icify report to %s"), *FullPath);
		return false;
	}
	return true;
}

TArray<AIcifyFreezeZone*> UIcifyBlueprintLibrary::GetZonesAffecting(const UPrimitiveComponent* Component)
{
	TArray<AIcifyFreezeZone*> Result;
	if (!IsValid(Component))
	{
		return Result;
	}

	const FBox ComponentBox = Component->Bounds.GetBox();
	TArray<AIcifyFreezeZone*> Zones;
	CollectZones(Component->GetWorld(), Zones);
	for (AIcifyFreezeZone* Zone : Zones)
	{
		// Same box test the zone uses to pick candidates: axis-aligned bounds, so a rotated zone can over-report slightly.
		const UIcifyZoneComponent* Box = Zone->GetZoneComponent();
		if (Box && Box->Bounds.GetBox().Intersect(ComponentBox))
		{
			Result.Add(Zone);
		}
	}
	return Result;
}

void UIcifyBlueprintLibrary::SetZoneSeed(AIcifyFreezeZone* Zone, int32 Seed, bool bPreview)
{
	if (!IsValid(Zone))
	{
		return;
	}

	Zone->Modify();
	Zone->Seed = Seed;
	if (bPreview)
	{
		Zone->Preview();
	}
}

#undef LOCTEXT_NAMESPACE
