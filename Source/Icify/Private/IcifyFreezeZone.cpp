// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifyFreezeZone.h"

#include "Components/ArrowComponent.h"
#include "Engine/CollisionProfile.h"
#include "IcifyZoneComponent.h"
#include "ProceduralMeshComponent.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "IcifyFreezeStamp.h"
#include "IcifyGenerator.h"
#include "IcifyPreset.h"
#include "IcifySettings.h"
#include "JsonObjectConverter.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopedSlowTask.h"
#include "ScopedTransaction.h"
#include "UObject/ObjectSaveContext.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#endif

#define LOCTEXT_NAMESPACE "IcifyFreezeZone"

#if WITH_EDITOR
namespace
{
	/** Records how long a phase took, and how many items it handled, in the run report. */
	struct FIcifyPhaseScope
	{
		FIcifyRunReport& Report;
		FString Name;
		double Start;
		int32 Items = 0;
		bool bFinished = false;

		FIcifyPhaseScope(FIcifyRunReport& InReport, const TCHAR* InName)
			: Report(InReport), Name(InName), Start(FPlatformTime::Seconds())
		{
		}

		/** Records now (before the report is written); otherwise the destructor records. */
		void Finish()
		{
			if (!bFinished)
			{
				bFinished = true;
				FIcifyPhaseTiming& Timing = Report.Phases.AddDefaulted_GetRef();
				Timing.Phase = Name;
				Timing.Seconds = float(FPlatformTime::Seconds() - Start);
				Timing.Items = Items;
			}
		}

		~FIcifyPhaseScope()
		{
			Finish();
		}
	};
}
#endif

AIcifyFreezeZone::AIcifyFreezeZone()
{
	PrimaryActorTick.bCanEverTick = false;

	ZoneComponent = CreateDefaultSubobject<UIcifyZoneComponent>(TEXT("Zone"));
	RootComponent = ZoneComponent;
	PresetFolder.Path = TEXT("/Game/Icify/Presets");

	IcicleMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Icicles"));
	IcicleMesh->SetupAttachment(ZoneComponent);
	ConfigureIcicleComponent(IcicleMesh);

	PreviewIcicleMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PreviewIcicles"));
	PreviewIcicleMesh->SetupAttachment(ZoneComponent);
	ConfigureIcicleComponent(PreviewIcicleMesh);

#if WITH_EDITORONLY_DATA
	WindArrow = CreateEditorOnlyDefaultSubobject<UArrowComponent>(TEXT("WindDirectionArrow"));
	if (WindArrow)
	{
		WindArrow->SetupAttachment(ZoneComponent);
		WindArrow->ArrowColor = FColor(90, 200, 255);
		WindArrow->ArrowSize = 2.f;
		WindArrow->bIsScreenSizeScaled = true;
		WindArrow->SetRelativeRotation(WindDirection);
	}
#endif
}

void AIcifyFreezeZone::ConfigureIcicleComponent(UProceduralMeshComponent* Mesh) const
{
	if (!Mesh)
	{
		return;
	}
	// Icicles belong to the meshes they hang from, not to the box: they stay put when the zone moves.
	Mesh->SetUsingAbsoluteLocation(true);
	Mesh->SetUsingAbsoluteRotation(true);
	Mesh->SetUsingAbsoluteScale(true);
	Mesh->SetCollisionProfileName(bIcicleCollision ? UCollisionProfile::BlockAll_ProfileName : UCollisionProfile::NoCollision_ProfileName);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCanEverAffectNavigation(false);
	Mesh->bUseComplexAsSimpleCollision = true;
	Mesh->SetCastShadow(bIcicleShadows);
}

void AIcifyFreezeZone::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UpdateWindArrow();
	ParkEmptyIcicleComponents();
}

void AIcifyFreezeZone::ParkEmptyIcicleComponents()
{
	// The icicle components keep absolute transforms, and an empty one still reports a point bound at its location. Every
	// path that empties one inside a transaction has already called Modify on it, so undo puts it back with its sections.
	for (UProceduralMeshComponent* Mesh : { IcicleMesh.Get(), PreviewIcicleMesh.Get() })
	{
		if (Mesh && Mesh->GetNumSections() == 0 && !Mesh->GetComponentLocation().Equals(GetActorLocation()))
		{
			Mesh->SetWorldLocationAndRotation(GetActorLocation(), FQuat::Identity);
		}
	}
}

void AIcifyFreezeZone::UpdateWindArrow()
{
#if WITH_EDITORONLY_DATA
	if (WindArrow)
	{
		WindArrow->SetRelativeRotation(WindDirection);
	}
#endif
}

FVector AIcifyFreezeZone::GetWindDirection() const
{
	FVector Wind = GetActorTransform().TransformVectorNoScale(WindDirection.Vector());
	Wind.Z = 0.0;
	return Wind.Normalize() ? Wind : FVector::ForwardVector;
}

FVector AIcifyFreezeZone::GetSnowfallDirection() const
{
	const float Slant = FMath::DegreesToRadians(FMath::Clamp(SnowfallSlantDegrees, 0.f, 60.f));
	return (FVector(0.0, 0.0, -FMath::Cos(Slant)) + GetWindDirection() * FMath::Sin(Slant)).GetSafeNormal();
}

const FIcifyComponentRecord* AIcifyFreezeZone::FindAppliedRecord(const UMeshComponent* Component) const
{
	return AppliedRecords.FindByPredicate([Component](const FIcifyComponentRecord& Record) { return Record.Component.Get() == Component; });
}

bool AIcifyFreezeZone::IsPreviewing(const UMeshComponent* Component) const
{
	return PreviewRecords.ContainsByPredicate([Component](const FIcifyComponentRecord& Record) { return Record.Component.Get() == Component; });
}

// ------------------------------------------------------------------ Actions

void AIcifyFreezeZone::Preview()
{
#if WITH_EDITOR
	if (!CanRunInEditorWorld())
	{
		return;
	}

	const double Start = FPlatformTime::Seconds();
	Modify();
	if (!RestorePreview())
	{
		return;
	}

	FIcifyGenerationJob Job;
	TArray<FIcifyComponentResult> Results;
	TMap<UMeshComponent*, AIcifyFreezeZone*> TakeOvers;
	if (!GenerateResults(TEXT("Preview"), Job, Results, TakeOvers))
	{
		FinishReport(Start);
		return;
	}

	TSet<int32> Frozen;
	{
		// Writing is quick and must not stop halfway, so this dialog has no Cancel button.
		FIcifyPhaseScope WritePhase(LastReport, TEXT("Write"));
		FScopedSlowTask WriteTask(float(FMath::Max(Results.Num(), 1)), LOCTEXT("WritingPreview", "Icify: writing preview..."));
		WriteTask.MakeDialogDelayed(0.3f, /*bShowCancelButton*/ false);
		for (const FIcifyComponentResult& Result : Results)
		{
			WriteTask.EnterProgressFrame(1.f);
			if (!IsValid(Result.Component))
			{
				continue;
			}
			++WritePhase.Items;
			FIcifyComponentRecord Record;
			Icify::CaptureRecord(Result.Component, Record);
			const FIcifyComponentRecord* Original = FindOriginalRecord(Result.Component);
			if (WriteResult(Result, PreviewDisplay, Original ? *Original : Record))
			{
				PreviewRecords.Add(MoveTemp(Record));
				Frozen.Add(Result.ReportIndex);
			}
			else if (!LastReport.Components[Result.ReportIndex].bCustomDataConflict && !Icify::RestoreRecord(Record))
			{
				// Keep the rollback snapshot if the failure could not be restored.
				PreviewRecords.Add(MoveTemp(Record));
			}
		}
	}

	{
		FIcifyPhaseScope IciclePhase(LastReport, TEXT("Icicles"));
		FIcifyIcicleBuild Build;
		BuildIcicles(Job, Frozen, Build);
		WriteIcicles(PreviewIcicleMesh, Build, /*bTransactional*/ true);
		// The preview stands in for the applied icicles until it ends.
		if (IcicleMesh && IcicleMesh->GetNumSections() > 0 && IcicleMesh->GetVisibleFlag())
		{
			IcicleMesh->SetFlags(RF_Transactional);
			IcicleMesh->Modify();
			IcicleMesh->SetVisibility(false);
		}
		IciclePhase.Items = Build.Stats.Icicles;
	}

	bPreviewActive = HasPreviewState();
	FinishReport(Start);
#endif
}

void AIcifyFreezeZone::Apply()
{
#if WITH_EDITOR
	if (!CanRunInEditorWorld())
	{
		return;
	}

	const double Start = FPlatformTime::Seconds();
	const FScopedTransaction Transaction(LOCTEXT("ApplyIce", "Icify: Apply Ice"));
	Modify();
	if (!RestorePreview() || !CancelDependentPreviews())
	{
		return;
	}

	FIcifyGenerationJob Job;
	TArray<FIcifyComponentResult> Results;
	TMap<UMeshComponent*, AIcifyFreezeZone*> TakeOvers;
	if (!GenerateResults(TEXT("Apply"), Job, Results, TakeOvers))
	{
		FinishReport(Start);
		return;
	}

	TSet<int32> Frozen;
	TSet<FString> TakenFrom;
	{
		// Writing is quick and must not stop halfway, so this dialog has no Cancel button.
		FIcifyPhaseScope WritePhase(LastReport, TEXT("Write"));
		FScopedSlowTask WriteTask(float(FMath::Max(Results.Num(), 1)), LOCTEXT("WritingApply", "Icify: applying ice..."));
		WriteTask.MakeDialogDelayed(0.3f, /*bShowCancelButton*/ false);

		// Meshes this zone applied to before but no longer covers go back to their originals.
		for (int32 Index = AppliedRecords.Num() - 1; Index >= 0; --Index)
		{
			const UMeshComponent* Previous = AppliedRecords[Index].Component.Get();
			if (!Previous)
			{
				// Not loaded (e.g. an unloaded World Partition cell): keep its backup for a later Clear.
				continue;
			}
			const bool bStillCovered = Results.ContainsByPredicate([Previous](const FIcifyComponentResult& Result) { return Result.Component == Previous; });
			if (!bStillCovered && Icify::RestoreRecord(AppliedRecords[Index]))
			{
				AppliedRecords.RemoveAt(Index);
			}
		}

		for (FIcifyComponentResult& Result : Results)
		{
			WriteTask.EnterProgressFrame(1.f);
			if (!IsValid(Result.Component))
			{
				continue;
			}
			++WritePhase.Items;
			FIcifyComponentReport& Entry = LastReport.Components[Result.ReportIndex];

			// Check before restoring an existing owner or transferring its backup. A conflict must touch nothing.
			const FIcifyComponentRecord* Original = FindOriginalRecord(Result.Component);
			const TArray<float>& Defaults = Original ? Original->CustomData : Result.Component->GetDefaultCustomPrimitiveData().Data;
			const TArray<float>& Live = Original ? Original->LiveCustomData : Result.Component->GetCustomPrimitiveData().Data;
			if (!Icify::CustomDataSlotsFree(Defaults, Live, UIcifySettings::Get()->CustomPrimitiveDataIndex))
			{
				Entry.bCustomDataConflict = true;
				Entry.Result = TEXT("skipped");
				continue;
			}

			int32 RecordIndex = AppliedRecords.IndexOfByPredicate([&Result](const FIcifyComponentRecord& Record) { return Record.Component.Get() == Result.Component; });
			bool bNewRecord = false;
			if (RecordIndex != INDEX_NONE)
			{
				// Start again from the original so nothing from the previous Apply lingers.
				if (!Icify::RestoreRecord(AppliedRecords[RecordIndex]))
				{
					Entry.Result = TEXT("failed");
					Entry.Notes += TEXT(" original state could not be restored; backup retained");
					continue;
				}
			}
			else
			{
				FIcifyComponentRecord Record;
				AIcifyFreezeZone* const* PreviousOwner = TakeOvers.Find(Result.Component);
				if (PreviousOwner && *PreviousOwner)
				{
					if (!(*PreviousOwner)->ReleaseAppliedRecord(Result.Component, Record))
					{
						Entry.Result = TEXT("failed");
						Entry.Notes += TEXT(" previous owner's backup could not be restored; ownership retained");
						continue;
					}
					TakenFrom.Add((*PreviousOwner)->GetActorNameOrLabel());
				}
				else
				{
					Icify::CaptureRecord(Result.Component, Record);
				}
				RecordIndex = AppliedRecords.Add(MoveTemp(Record));
				bNewRecord = true;
			}

			if (WriteResult(Result, EIcifyPreviewDisplay::FinalMaterial, AppliedRecords[RecordIndex]))
			{
				Frozen.Add(Result.ReportIndex);
			}
			else
			{
				const bool bRolledBack = Entry.bCustomDataConflict || Icify::RestoreRecord(AppliedRecords[RecordIndex]);
				if (bNewRecord && bRolledBack)
				{
					AppliedRecords.RemoveAt(RecordIndex);
				}
			}
		}
	}

	{
		// The icicles are rebuilt from scratch on every Apply, from the meshes that froze.
		FIcifyPhaseScope IciclePhase(LastReport, TEXT("Icicles"));
		FIcifyIcicleBuild Build;
		BuildIcicles(Job, Frozen, Build);
		WriteIcicles(IcicleMesh, Build, /*bTransactional*/ true);
		AppliedIcicleCount = Build.Stats.Icicles;
		IciclePhase.Items = Build.Stats.Icicles;
	}
	// Meshes that keep an older record (not loaded, or not restorable) keep their ice but not their icicles.
	int32 NumWithoutIcicles = 0;
	for (const FIcifyComponentRecord& Record : AppliedRecords)
	{
		const UMeshComponent* Component = Record.Component.Get();
		const bool bFrozenNow = Component && Results.ContainsByPredicate([Component, &Frozen](const FIcifyComponentResult& Result)
		{
			return Result.Component == Component && Frozen.Contains(Result.ReportIndex);
		});
		NumWithoutIcicles += bFrozenNow ? 0 : 1;
	}
	if (NumWithoutIcicles > 0)
	{
		LastReport.Warnings.Add(FString::Printf(TEXT("%d mesh(es) frozen earlier weren't frozen in this run (not loaded, or their backup was retained). They keep their ice but have no icicles until they're loaded and the zone is applied again."), NumWithoutIcicles));
	}
	for (const FString& Zone : TakenFrom)
	{
		LastReport.Warnings.Add(FString::Printf(TEXT("Took over meshes from %s. Its own icicles on them stay until that zone is applied again or cleared."), *Zone));
	}

	FinishReport(Start);
#endif
}

void AIcifyFreezeZone::Clear()
{
#if WITH_EDITOR
	if (!CanRunInEditorWorld())
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("ClearIce", "Icify: Clear Ice"));
	Modify();
	if (!RestorePreview() || !CancelDependentPreviews())
	{
		return;
	}

	int32 NumCleared = 0;
	for (int32 Index = AppliedRecords.Num() - 1; Index >= 0; --Index)
	{
		// Meshes that aren't loaded keep their record so a later Clear can still restore them.
		if (AppliedRecords[Index].Component.Get() && Icify::RestoreRecord(AppliedRecords[Index]))
		{
			AppliedRecords.RemoveAt(Index);
			++NumCleared;
		}
	}

	const int32 NumIcicles = AppliedIcicleCount;
	if (IcicleMesh && IcicleMesh->GetNumSections() > 0)
	{
		IcicleMesh->SetFlags(RF_Transactional);
		IcicleMesh->Modify();
		IcicleMesh->ClearAllMeshSections();
		IcicleMesh->EmptyOverrideMaterials();
	}
	AppliedIcicleCount = 0;
	ParkEmptyIcicleComponents();

	Status = AppliedRecords.Num() > 0
		? FString::Printf(TEXT("Cleared %d mesh(es) and %d icicle(s). %d backups retained (unloaded or restoration failed); see Output Log."), NumCleared, NumIcicles, AppliedRecords.Num())
		: FString::Printf(TEXT("Cleared %d mesh(es) and %d icicle(s)."), NumCleared, NumIcicles);
	UE_LOG(LogIcify, Log, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
#endif
}

void AIcifyFreezeZone::CancelPreview()
{
#if WITH_EDITOR
	if (!CanRunInEditorWorld())
	{
		return;
	}

	Modify();
	if (!RestorePreview())
	{
		return;
	}
	Status = AppliedRecords.Num() > 0 || AppliedIcicleCount > 0
		? FString::Printf(TEXT("Preview cancelled. %d mesh(es) and %d icicle(s) applied."), AppliedRecords.Num(), AppliedIcicleCount)
		: FString(TEXT("Preview cancelled."));
#endif
}

void AIcifyFreezeZone::NewSeed()
{
#if WITH_EDITOR
	Modify();
	Seed = FMath::RandRange(1, 999999);
	RefreshPreviewIfLive();
#endif
}

void AIcifyFreezeZone::ClearGenerationCache()
{
#if WITH_EDITOR
	const int32 Count = GenerationCache.Num();
	GenerationCache.Reset();
	Status = FString::Printf(TEXT("Forgot %d cached mesh result(s)."), Count);
#endif
}

void AIcifyFreezeZone::DryRun()
{
#if WITH_EDITOR
	if (!CanRunInEditorWorld())
	{
		return;
	}

	const double Start = FPlatformTime::Seconds();
	FIcifyGenerationJob Job;
	TArray<FIcifyComponentResult> Results;
	TMap<UMeshComponent*, AIcifyFreezeZone*> TakeOvers;
	if (!GenerateResults(TEXT("DryRun"), Job, Results, TakeOvers))
	{
		FinishReport(Start);
		return;
	}

	// Flag the custom data conflicts a real run would hit; the rest would freeze and grow their icicles.
	TSet<int32> Frozen;
	for (const FIcifyComponentResult& Result : Results)
	{
		const FIcifyComponentRecord* Original = FindOriginalRecord(Result.Component);
		const TArray<float>& Defaults = Original ? Original->CustomData : Result.Component->GetDefaultCustomPrimitiveData().Data;
		const TArray<float>& Live = Original ? Original->LiveCustomData : Result.Component->GetCustomPrimitiveData().Data;
		if (!Icify::CustomDataSlotsFree(Defaults, Live, UIcifySettings::Get()->CustomPrimitiveDataIndex))
		{
			LastReport.Components[Result.ReportIndex].bCustomDataConflict = true;
			LastReport.Components[Result.ReportIndex].Result = TEXT("skipped");
		}
		else
		{
			Frozen.Add(Result.ReportIndex);
		}
	}

	// Built only to count them; nothing is written.
	FIcifyIcicleBuild Build;
	BuildIcicles(Job, Frozen, Build);
	FinishReport(Start);
#endif
}

// ------------------------------------------------------------------ Presets

void AIcifyFreezeZone::LoadPreset()
{
#if WITH_EDITOR
	if (!Preset)
	{
		Status = TEXT("No preset assigned. Pick one in Preset, or use Save To Preset to create one from this zone.");
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("LoadPreset", "Icify: Load Preset"));
	Modify();
	CopyFromPreset(*Preset);
	UpdateWindArrow();
	Status = FString::Printf(TEXT("Loaded preset %s%s."), *Preset->GetName(), Preset->bIncludeSeed ? TEXT(" (including seed)") : TEXT(""));
	UE_LOG(LogIcify, Log, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
	RefreshPreviewIfLive();
#endif
}

void AIcifyFreezeZone::SaveToPreset()
{
#if WITH_EDITOR
	UIcifyPreset* Target = Preset;
	bool bCreated = false;

	if (!Target)
	{
		// New asset in the project's content, never in the plugin's generated content.
		FString Folder = PresetFolder.Path.IsEmpty() ? FString(TEXT("/Game/Icify/Presets")) : PresetFolder.Path;
		Folder.RemoveFromEnd(TEXT("/"));
		FString BaseName;
		for (const TCHAR Ch : GetActorNameOrLabel())
		{
			BaseName.AppendChar(FChar::IsAlnum(Ch) ? Ch : TEXT('_'));
		}
		BaseName = TEXT("IP_") + BaseName;

		FString AssetName = BaseName;
		FString PackageName = Folder / AssetName;
		for (int32 Suffix = 2; FPackageName::DoesPackageExist(PackageName) || FindPackage(nullptr, *PackageName); ++Suffix)
		{
			AssetName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix);
			PackageName = Folder / AssetName;
		}

		FText Error;
		if (!FPackageName::IsValidLongPackageName(PackageName, /*bIncludeReadOnlyRoots*/ false, &Error))
		{
			Status = FString::Printf(TEXT("Preset not saved: '%s' is not a valid content path (%s)."), *PackageName, *Error.ToString());
			UE_LOG(LogIcify, Warning, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
			return;
		}

		UPackage* Package = CreatePackage(*PackageName);
		Target = NewObject<UIcifyPreset>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Target);
		bCreated = true;
	}

	Target->Modify();
#define ICIFY_COPY_TO_PRESET(Field) Target->Field = Field;
	ICIFY_PRESET_FIELDS(ICIFY_COPY_TO_PRESET)
#undef ICIFY_COPY_TO_PRESET
	Target->Seed = Seed;
	Target->MarkPackageDirty();

	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages({ Target->GetPackage() }, /*bOnlyDirty*/ false);

	if (bCreated)
	{
		const FScopedTransaction Transaction(LOCTEXT("AssignPreset", "Icify: Assign New Preset"));
		Modify();
		Preset = Target;
	}

	Status = bSaved
		? FString::Printf(TEXT("%s preset %s."), bCreated ? TEXT("Created") : TEXT("Saved"), *Target->GetPathName())
		: FString::Printf(TEXT("Preset %s was updated in memory but could not be saved to disk; save it from the Content Browser."), *Target->GetPathName());
	UE_LOG(LogIcify, Log, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
#endif
}

#if WITH_EDITOR

void AIcifyFreezeZone::CopyFromPreset(const UIcifyPreset& Source)
{
#define ICIFY_COPY_FROM_PRESET(Field) Field = Source.Field;
	ICIFY_PRESET_FIELDS(ICIFY_COPY_FROM_PRESET)
#undef ICIFY_COPY_FROM_PRESET
	if (Source.bIncludeSeed)
	{
		Seed = Source.Seed;
	}
}

// ------------------------------------------------------------ Editor hooks

void AIcifyFreezeZone::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	UpdateWindArrow();

	// Undo/redo arrives with no property; never regenerate then.
	if (!PropertyChangedEvent.Property || GIsTransacting)
	{
		return;
	}

	const FName MemberName = PropertyChangedEvent.GetMemberPropertyName();

	// Assigning a preset copies it in (one undo step), then regenerates like any other change.
	if (MemberName == GET_MEMBER_NAME_CHECKED(AIcifyFreezeZone, Preset))
	{
		if (Preset)
		{
			LoadPreset();
		}
		return;
	}
	if (MemberName == GET_MEMBER_NAME_CHECKED(AIcifyFreezeZone, PresetFolder)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AIcifyFreezeZone, Status)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AIcifyFreezeZone, bPreviewActive)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AIcifyFreezeZone, LastReport)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AIcifyFreezeZone, LastReportFile)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AIcifyFreezeZone, bWriteReportFiles)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AIcifyFreezeZone, CacheBudgetMB)
		|| MemberName == GET_MEMBER_NAME_CHECKED(AIcifyFreezeZone, bUseGenerationCache))
	{
		if (MemberName == GET_MEMBER_NAME_CHECKED(AIcifyFreezeZone, bUseGenerationCache) && !bUseGenerationCache)
		{
			GenerationCache.Reset();
		}
		else if (MemberName == GET_MEMBER_NAME_CHECKED(AIcifyFreezeZone, CacheBudgetMB))
		{
			TrimCache();
		}
		return;
	}

	// Dragging a slider or a melt source widget fires Interactive changes every frame; wait for the release.
	if (PropertyChangedEvent.ChangeType == EPropertyChangeType::Interactive)
	{
		return;
	}

	RefreshPreviewIfLive();
}

void AIcifyFreezeZone::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);

	if (bFinished && !GIsTransacting)
	{
		RefreshPreviewIfLive();
	}
}

void AIcifyFreezeZone::PostEditImport()
{
	Super::PostEditImport();
	ForgetRecords();
}

void AIcifyFreezeZone::PostDuplicate(EDuplicateMode::Type DuplicateMode)
{
	Super::PostDuplicate(DuplicateMode);
	if (DuplicateMode == EDuplicateMode::Normal)
	{
		ForgetRecords();
	}
}

void AIcifyFreezeZone::PreSave(FObjectPreSaveContext SaveContext)
{
	Super::PreSave(SaveContext);
	// The editor module discards previews before a level is saved; this only catches saves that bypass it. Autosaves and
	// procedural saves keep the whole preview, as the module does, so a recovery copy never holds half of one.
	const bool bKeepsPreview = SaveContext.IsCooking() || SaveContext.IsProceduralSave() || (SaveContext.GetSaveFlags() & SAVE_FromAutosave) != 0;
	if (!IsTemplate() && !bKeepsPreview)
	{
		ClearPreviewIcicles(/*bTransactional*/ false);
	}
}

void AIcifyFreezeZone::PostEditUndo()
{
	Super::PostEditUndo();
	for (UProceduralMeshComponent* Mesh : { IcicleMesh.Get(), PreviewIcicleMesh.Get() })
	{
		if (!Mesh)
		{
			continue;
		}
		// Undo restores the sections but not the collision cooked from them. Clearing an emptied component drops stale
		// collision; setting one section again recooks it for all of them and refreshes the bounds.
		if (Mesh->GetNumSections() == 0)
		{
			Mesh->ClearAllMeshSections();
		}
		for (int32 Index = 0; Index < Mesh->GetNumSections(); ++Index)
		{
			const FProcMeshSection* Section = Mesh->GetProcMeshSection(Index);
			if (Section && Section->bEnableCollision)
			{
				const FProcMeshSection Copy = *Section;
				Mesh->SetProcMeshSection(Index, Copy);
				break;
			}
		}
		Mesh->MarkRenderStateDirty();
	}
}

void AIcifyFreezeZone::ForgetRecords()
{
	// A pasted or duplicated zone starts fresh: the meshes belong to the zone that froze them, and so do its icicles.
	PreviewRecords.Reset();
	AppliedRecords.Reset();
	AppliedIcicleCount = 0;
	for (UProceduralMeshComponent* Mesh : { IcicleMesh.Get(), PreviewIcicleMesh.Get() })
	{
		if (Mesh)
		{
			Mesh->ClearAllMeshSections();
			Mesh->EmptyOverrideMaterials();
			Mesh->SetVisibility(true);
		}
	}
	ParkEmptyIcicleComponents();
	bPreviewActive = false;
	Status.Reset();
}

void AIcifyFreezeZone::NotifyZoneShapeChanged(const FPropertyChangedEvent& PropertyChangedEvent)
{
	if (PropertyChangedEvent.Property && !GIsTransacting && PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
	{
		RefreshPreviewIfLive();
	}
}

void AIcifyFreezeZone::RefreshPreviewIfLive()
{
	if (!CanRunInEditorWorld())
	{
		return;
	}
	if (bLivePreview)
	{
		RequestLivePreview();
	}
	else
	{
		// Keep the viewport's melt and heat source gizmos current even when not previewing.
		RefreshSourceViews();
	}
}

void AIcifyFreezeZone::RequestLivePreview()
{
	if (bLivePreview && CanRunInEditorWorld())
	{
		Preview();
	}
}

bool AIcifyFreezeZone::CanRunInEditorWorld() const
{
	const UWorld* World = GetWorld();
	return World && World->WorldType == EWorldType::Editor && !IsTemplate() && ZoneComponent && IcicleMesh && PreviewIcicleMesh;
}

#endif // WITH_EDITOR

// --------------------------------------------------------------- Lifecycle

void AIcifyFreezeZone::Destroyed()
{
#if WITH_EDITOR
	// Deleting the zone drops its preview inside the delete transaction, so undo brings both back.
	if ((bPreviewActive || HasPreviewState()) && !GIsTransacting && CanRunInEditorWorld())
	{
		Modify();
		RestorePreview();
	}
#endif
	Super::Destroyed();
}

void AIcifyFreezeZone::PostUnregisterAllComponents()
{
	Super::PostUnregisterAllComponents();

#if WITH_EDITOR
	// Level unload or streaming out: put previewed meshes back so no un-owned preview is left behind.
	const ULevel* Level = GetLevel();
	const bool bLeaving = IsActorBeingDestroyed() || (Level && Level->bIsBeingRemoved);
	if (bPreviewActive && bLeaving && !GIsTransacting && !IsEngineExitRequested() && CanRunInEditorWorld())
	{
		DiscardPreview(TEXT("zone unloaded"));
	}
#endif
}

#if WITH_EDITOR

void AIcifyFreezeZone::DiscardPreview(const TCHAR* Reason)
{
	if (!bPreviewActive && !HasPreviewState())
	{
		return;
	}

	for (int32 Index = PreviewRecords.Num() - 1; Index >= 0; --Index)
	{
		if (Icify::RestoreRecord(PreviewRecords[Index], /*bTransactional*/ false))
		{
			PreviewRecords.RemoveAt(Index);
		}
	}
	ClearPreviewIcicles(/*bTransactional*/ false);
	bPreviewActive = PreviewRecords.Num() > 0;

	Status = bPreviewActive
		? FString(TEXT("Preview restoration failed; backups retained. See Output Log."))
		: FString::Printf(TEXT("Preview discarded (%s). Press Preview to see it again."), Reason);
	UE_LOG(LogIcify, Log, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
}

bool AIcifyFreezeZone::ReleaseAppliedRecord(const UMeshComponent* Component, FIcifyComponentRecord& OutRecord)
{
	const int32 Index = AppliedRecords.IndexOfByPredicate([Component](const FIcifyComponentRecord& Record) { return Record.Component.Get() == Component; });
	if (Index == INDEX_NONE)
	{
		return false;
	}
	Modify();
	if (!Icify::RestoreRecord(AppliedRecords[Index]))
	{
		return false;
	}
	OutRecord = MoveTemp(AppliedRecords[Index]);
	AppliedRecords.RemoveAt(Index);
	Status = FString::Printf(TEXT("%s was taken over by another zone."), *Component->GetName());
	return true;
}

// ---------------------------------------------------------------- Internals

bool AIcifyFreezeZone::HasPreviewState() const
{
	return PreviewRecords.Num() > 0
		|| (PreviewIcicleMesh && PreviewIcicleMesh->GetNumSections() > 0)
		|| (IcicleMesh && !IcicleMesh->GetVisibleFlag());
}

void AIcifyFreezeZone::ClearPreviewIcicles(bool bTransactional)
{
	if (PreviewIcicleMesh && PreviewIcicleMesh->GetNumSections() > 0)
	{
		if (bTransactional)
		{
			PreviewIcicleMesh->SetFlags(RF_Transactional);
			PreviewIcicleMesh->Modify();
		}
		PreviewIcicleMesh->ClearAllMeshSections();
		PreviewIcicleMesh->EmptyOverrideMaterials();
	}
	if (IcicleMesh && !IcicleMesh->GetVisibleFlag())
	{
		if (bTransactional)
		{
			IcicleMesh->SetFlags(RF_Transactional);
			IcicleMesh->Modify();
		}
		IcicleMesh->SetVisibility(true);
	}
	ParkEmptyIcicleComponents();
}

bool AIcifyFreezeZone::RestorePreview()
{
	for (int32 Index = PreviewRecords.Num() - 1; Index >= 0; --Index)
	{
		if (Icify::RestoreRecord(PreviewRecords[Index]))
		{
			PreviewRecords.RemoveAt(Index);
		}
	}
	ClearPreviewIcicles(/*bTransactional*/ true);
	bPreviewActive = PreviewRecords.Num() > 0;
	if (bPreviewActive)
	{
		Status = TEXT("Some preview state could not be restored; backups retained. See Output Log.");
	}
	return !bPreviewActive;
}

bool AIcifyFreezeZone::CancelDependentPreviews()
{
	// Older saved records can contain previews of this zone's applied layer. Retire those snapshots
	// before changing the owner, so a later Cancel cannot resurrect an owner that has already cleared.
	for (TActorIterator<AIcifyFreezeZone> It(GetWorld()); It; ++It)
	{
		AIcifyFreezeZone* Other = *It;
		if (Other == this)
		{
			continue;
		}
		auto DependsOnUs = [this, Other]()
		{
			return AppliedRecords.ContainsByPredicate([Other](const FIcifyComponentRecord& Record)
				{ return Record.Component.Get() && Other->IsPreviewing(Record.Component.Get()); });
		};
		if (DependsOnUs())
		{
			Other->CancelPreview();
			if (DependsOnUs())
			{
				Status = TEXT("Another zone's dependent preview could not be restored; owner state retained.");
				return false;
			}
		}
	}
	return true;
}

void AIcifyFreezeZone::CollectSources(TArray<FIcifyDripSource>& OutDrips, TArray<FIcifyHeatSource>& OutHeat) const
{
	OutDrips.Reset();
	OutHeat.Reset();

	UWorld* World = GetWorld();
	if (!World || !ZoneComponent)
	{
		return;
	}
	const FBox ZoneBox = ZoneComponent->Bounds.GetBox();

	for (const FIcifyMeltSource& Melt : MeltSources)
	{
		FIcifyDripSource& Source = OutDrips.AddDefaulted_GetRef();
		Source.Origin = GetActorTransform().TransformPosition(Melt.Location);
		Source.Radius = FMath::Max(Melt.RadiusCm, 1.f);
		Source.Length = Melt.LengthCm;
		Source.Strength = Melt.Strength;
		Source.bManual = true;
	}

	// Meltwater leaves an actor at the bottom of its footprint.
	auto AddDrip = [&OutDrips](const FBox& Box)
	{
		if (!Box.IsValid)
		{
			return;
		}
		const FVector Center = Box.GetCenter();
		const FVector Extent = Box.GetExtent();
		FIcifyDripSource& Source = OutDrips.AddDefaulted_GetRef();
		Source.Origin = FVector(Center.X, Center.Y, Box.Min.Z);
		Source.Radius = FMath::Clamp(float(FMath::Max(Extent.X, Extent.Y)), 5.f, 300.f);
	};
	const float ThawRadius = FMath::Max(ThawRadiusCm, 10.f);
	auto AddHeat = [&OutHeat](const FVector& Location, float Radius)
	{
		FIcifyHeatSource& Heat = OutHeat.AddDefaulted_GetRef();
		Heat.Location = Location;
		Heat.Radius = Radius;
	};

	for (const TSoftObjectPtr<AActor>& Listed : DripSourceActors)
	{
		if (const AActor* Actor = Listed.Get())
		{
			AddDrip(Actor->GetComponentsBoundingBox(/*bNonColliding*/ true));
		}
	}
	for (const TSoftObjectPtr<AActor>& Listed : HeatSourceActors)
	{
		if (const AActor* Actor = Listed.Get())
		{
			const FBox Box = Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
			if (Box.IsValid)
			{
				AddHeat(Box.GetCenter(), ThawRadius);
			}
		}
	}

	// Melt sources above the zone still drip into it; heat reaches into the zone from around it, furthest from below.
	FBox DripBox = ZoneBox;
	DripBox.Max.Z += FMath::Max(DripLengthCm, 10.f);
	FBox HeatBox = ZoneBox.ExpandBy(FVector(ThawRadius));
	HeatBox.Min.Z -= ThawRadius * 0.25;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		if (!Actor || Actor == this || Actor->IsA<AIcifyFreezeZone>() || Actor->IsEditorOnly() || Icify::HasIgnoreTag(Actor, nullptr))
		{
			continue;
		}
		const FBox ActorBox = Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
		if (!ActorBox.IsValid)
		{
			continue;
		}

		if (ActorBox.Intersect(HeatBox) && !HeatSourceActors.ContainsByPredicate([Actor](const TSoftObjectPtr<AActor>& Listed) { return Listed.Get() == Actor; }))
		{
			if (Icify::IsHeatSource(Actor))
			{
				AddHeat(ActorBox.GetCenter(), ThawRadius);
			}
			else if (bThawFromLights)
			{
				// Lamps are gentler than fires.
				TInlineComponentArray<ULocalLightComponent*> Lights(Actor);
				for (const ULocalLightComponent* Light : Lights)
				{
					if (Light && Light->IsVisible())
					{
						AddHeat(Light->GetComponentLocation(), ThawRadius * 0.5f);
					}
				}
			}
		}

		if (bAutoDetectDripSources && ActorBox.Intersect(DripBox)
			&& !DripSourceActors.ContainsByPredicate([Actor](const TSoftObjectPtr<AActor>& Listed) { return Listed.Get() == Actor; }))
		{
			if (Icify::IsDripSource(Actor, nullptr))
			{
				AddDrip(ActorBox);
				continue;
			}
			// Gutters and spouts can also be parts of a larger actor.
			TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
			for (const UPrimitiveComponent* Primitive : Primitives)
			{
				if (Primitive && Primitive->IsRegistered() && !Icify::HasIgnoreTag(nullptr, Primitive) && Icify::IsDripSource(nullptr, Primitive))
				{
					const FBox Box = Primitive->Bounds.GetBox();
					if (Box.Intersect(DripBox))
					{
						AddDrip(Box);
					}
				}
			}
		}
	}
}

void AIcifyFreezeZone::RefreshSourceViews()
{
	TArray<FIcifyDripSource> Drips;
	TArray<FIcifyHeatSource> Heat;
	CollectSources(Drips, Heat);

	MeltSourceViews.Reset();
	for (const FIcifyDripSource& Drip : Drips)
	{
		FSourceView& View = MeltSourceViews.AddDefaulted_GetRef();
		View.Origin = Drip.Origin;
		View.Radius = Drip.Radius;
		View.Length = Drip.Length > 0.f ? Drip.Length : DripLengthCm;
		View.bManual = Drip.bManual;
	}
	HeatSourceViews.Reset();
	for (const FIcifyHeatSource& Source : Heat)
	{
		HeatSourceViews.Add(Source.Location);
	}
}

void AIcifyFreezeZone::BuildParams(FIcifyGenerationParams& P) const
{
	P.Seed = uint32(Seed);
	P.Intensity = Intensity;
	P.FeatureScale = FeatureScaleCm;

	P.ZoneTransform = ZoneComponent->GetComponentTransform();
	P.ZoneTransform.SetScale3D(FVector::OneVector);
	P.ZoneExtent = ZoneComponent->GetScaledBoxExtent();
	P.ZoneFalloff = ZoneFalloffCm;

	P.WindDir = GetWindDirection();
	P.WindInfluence = WindInfluence;

	P.FrostIntensity = FrostIntensity;
	P.FrostFilm = FrostFilm;
	P.EdgeFrost = EdgeFrost;
	P.Rime = Rime;
	P.GroundFrost = GroundFrost;
	P.GroundFrostHeight = GroundFrostHeightCm;
	P.EdgeAngleDeg = EdgeAngleDegrees;
	P.FrostEdgeWidth = FrostEdgeWidthCm;

	P.SnowIntensity = SnowIntensity;
	P.SnowMinUp = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(SnowMaxSlopeDegrees, 0.f, 89.f)));
	P.CreviceSnow = CreviceSnow;
	P.WallPlaster = WallPlaster;
	P.EdgeSnowLoss = EdgeSnowLoss;

	P.GlazeIntensity = GlazeIntensity;
	P.GlazeCoat = GlazeCoat;
	P.AmbientDrips = AmbientDrips;
	P.DripLength = FMath::Max(DripLengthCm, 10.f);
	P.DripSpread = DripSpread;

	P.BlackIceIntensity = BlackIceIntensity;
	P.FloorIce = FloorIce;
	P.CreviceIce = CreviceIce;
	P.PuddleRadius = PuddleRadiusCm;

	P.ThawIntensity = ThawIntensity;

	// Candidates at half the icicle spacing, so the density draw has room to place them unevenly.
	P.bDripPoints = bGrowIcicles && IcicleDensity > 0.f && MaxIcicles > 0;
	P.DripOverhangDeg = DripOverhangDegrees;
	P.DripSpacing = FMath::Clamp(100.f / FMath::Max(IcicleDensity, 0.1f) * 0.5f, 1.f, 200.f);

	P.MaskOutput = MaskOutput;
	P.MaskTexelSize = MaskTexelSizeCm;
	P.MaxMaskResolution = MaxMaskResolution;
	P.MaskUVChannel = UIcifySettings::Get()->MaskUVChannel;
}

void AIcifyFreezeZone::BuildContext(UMeshComponent* Component, TMap<UTexture2D*, TUniquePtr<FIcifyGateSampler>>& Samplers,
	FIcifyComponentContext& OutContext, FIcifyComponentReport& Report)
{
	const AActor* ComponentOwner = Component->GetOwner();
	const UEnum* SurfaceEnum = StaticEnum<EIcifySurfaceType>();
	const int32 NumUVChannels = Icify::NumUVChannelsOf(Component);
	const int32 NumSlots = FMath::Max(Component->GetNumMaterials(), 1);
	OutContext.Slots.SetNum(NumSlots);

	// A gutter or downpipe is itself a melt source: every drip edge it has is fed.
	OutContext.bDripSourceMesh = (ComponentOwner && DripSourceActors.ContainsByPredicate([ComponentOwner](const TSoftObjectPtr<AActor>& Listed) { return Listed.Get() == ComponentOwner; }))
		|| (bAutoDetectDripSources && (Icify::IsDripSource(ComponentOwner, nullptr) || Icify::IsDripSource(nullptr, Component)));
	const bool bNoIciclesTag = Icify::HasNoIciclesTag(ComponentOwner, Component);
	const FString NoIciclesTag = UIcifySettings::Get()->NoIciclesTag.ToString();

	for (int32 Slot = 0; Slot < NumSlots; ++Slot)
	{
		FIcifySlotInfo& Info = OutContext.Slots[Slot];
		FIcifySlotReport& Detail = Report.SlotDetails.AddDefaulted_GetRef();
		Detail.Slot = Slot;
		Detail.SlotName = Icify::SlotNameOf(Component, Slot);
		const UMaterialInterface* Material = Component->GetMaterial(Slot);
		Detail.Material = Material ? Material->GetName() : TEXT("(none)");

		FString RuleWhy;
		const FIcifySlotRule* Rule = nullptr;
		for (const FIcifySlotRule& Candidate : SlotRules)
		{
			if (Icify::RuleMatchesSlot(Candidate, Component, Slot, &RuleWhy))
			{
				Rule = &Candidate;
				break;
			}
		}
		const FString RuleName = Rule ? FString::Printf(TEXT("'%s'"), Rule->Label.IsEmpty() ? TEXT("(unnamed)") : *Rule->Label) : FString();
		if (Rule)
		{
			Detail.Rule = FString::Printf(TEXT("%s, matched by %s"), *RuleName, *RuleWhy);
		}

		// Surface family.
		if (Rule && Rule->Surface != EIcifySurfaceType::Auto)
		{
			Info.Surface = Rule->Surface;
			Detail.SurfaceSource = FString::Printf(TEXT("rule %s"), *RuleName);
		}
		else
		{
			Info.Surface = Icify::ClassifySlot(Component, Slot, SurfaceType, &Detail.SurfaceSource);
		}
		Detail.Surface = SurfaceEnum->GetNameStringByValue(int64(Info.Surface));

		// Profile: rule's own, then a named material profile, then the surface's.
		switch (Info.Surface)
		{
		case EIcifySurfaceType::Metal: Info.Profile = MetalProfile; Detail.Profile = TEXT("Metal Profile"); break;
		case EIcifySurfaceType::Glass: Info.Profile = GlassProfile; Detail.Profile = TEXT("Glass Profile"); break;
		default:                       Info.Profile = StoneProfile; Detail.Profile = TEXT("Stone Profile"); break;
		}

		Info.bIcicles = !bNoIciclesTag;
		Detail.IcicleSource = bNoIciclesTag ? FString::Printf(TEXT("'%s' tag"), *NoIciclesTag) : TEXT("drip edges");

		bool bGateUnreadable = false;
		if (Rule)
		{
			Info.bExcluded = Rule->bExclude;
			if (Rule->bOverrideProfile)
			{
				Info.Profile = Rule->Profile;
				Detail.Profile = FString::Printf(TEXT("rule %s profile"), *RuleName);
			}
			else if (!Rule->ProfileName.IsNone())
			{
				if (const FIcifySurfaceProfile* Named = MaterialProfiles.Find(Rule->ProfileName))
				{
					Info.Profile = *Named;
					Detail.Profile = FString::Printf(TEXT("material profile '%s'"), *Rule->ProfileName.ToString());
				}
				else
				{
					Detail.Warnings.Add(FString::Printf(TEXT("rule %s names profile '%s', which isn't in Material Profiles; used %s instead"),
						*RuleName, *Rule->ProfileName.ToString(), *Detail.Profile));
				}
			}
			if (Rule->Icicles != EIcifyIcicleMode::Inherit)
			{
				Info.bIcicles = Rule->Icicles == EIcifyIcicleMode::Grow;
				Detail.IcicleSource = FString::Printf(TEXT("rule %s (%s)"), *RuleName, Info.bIcicles ? TEXT("forced on") : TEXT("forced off"));
			}

			const FIcifyGate& Gate = Rule->Gate;
			Info.Gate = &Gate;
			TArray<FString> Gates;
			if (Gate.bUseFacingGate)
			{
				Gates.Add(FString::Printf(TEXT("facing %.2f..%.2f"), Gate.MinUpFacing, Gate.MaxUpFacing));
			}
			if (Gate.bUseHeightGate)
			{
				Gates.Add(FString::Printf(TEXT("height %.0f..%.0f cm"), Gate.MinHeightCm, Gate.MaxHeightCm));
			}
			if (UTexture2D* GateTexture = Gate.GateTexture)
			{
				Gates.Add(FString::Printf(TEXT("texture %s %s on UV%d%s"), *GateTexture->GetName(),
					*StaticEnum<EIcifyChannel>()->GetNameStringByValue(int64(Gate.GateChannel)), Gate.GateUVChannel, Gate.bInvertGate ? TEXT(" (inverted)") : TEXT("")));
				if (NumUVChannels > 0 && Gate.GateUVChannel >= NumUVChannels)
				{
					Detail.Warnings.Add(FString::Printf(TEXT("gate UV channel %d doesn't exist on this mesh (%d channel(s)); UV%d is used instead"),
						Gate.GateUVChannel, NumUVChannels, NumUVChannels - 1));
				}

				TUniquePtr<FIcifyGateSampler>& Sampler = Samplers.FindOrAdd(GateTexture);
				FString Error;
				if (!Sampler.IsValid())
				{
					Sampler = MakeUnique<FIcifyGateSampler>();
					if (!Sampler->Init(GateTexture, Error))
					{
						LastReport.Warnings.AddUnique(FString::Printf(TEXT("Rule %s: %s. Slots using it are left untouched."), *RuleName, *Error));
						Sampler.Reset();
					}
				}
				if (Sampler.IsValid())
				{
					Info.GateSampler = Sampler.Get();
				}
				else
				{
					// Fail safe: an unreadable gate must not let ice through everywhere.
					Info.bExcluded = true;
					bGateUnreadable = true;
					Detail.Warnings.Add(FString::Printf(TEXT("gate texture %s could not be read%s"), *GateTexture->GetName(), Error.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(": %s"), *Error)));
				}
			}
			Detail.Gates = FString::Join(Gates, TEXT(", "));
		}
		else if (bOnlySlotsMatchedByRules)
		{
			// Selected-slot scope: nothing a rule didn't explicitly pick is touched.
			Info.bExcluded = true;
		}

		Detail.bIcicles = Info.bIcicles && !Info.bExcluded;
		Detail.bIncluded = !Info.bExcluded;
		if (Rule && Rule->bExclude)
		{
			Detail.Reason = FString::Printf(TEXT("excluded by rule %s (Exclude is on; matched by %s)"), *RuleName, *RuleWhy);
		}
		else if (bGateUnreadable)
		{
			Detail.Reason = FString::Printf(TEXT("excluded: rule %s has a gate texture that couldn't be read, so no ice is allowed (fail-safe)"), *RuleName);
		}
		else if (Rule)
		{
			Detail.Reason = FString::Printf(TEXT("included; rule %s applies (matched by %s)"), *RuleName, *RuleWhy);
		}
		else if (bOnlySlotsMatchedByRules)
		{
			Detail.Reason = TEXT("excluded: no slot rule matched and Only Slots Matched By Rules is on");
		}
		else
		{
			Detail.Reason = SlotRules.Num() > 0 ? TEXT("included; no slot rule matched") : TEXT("included; zone has no slot rules");
		}
		if (!Info.bExcluded && !Material)
		{
			Detail.Warnings.Add(TEXT("slot has no material, so there is nothing to bind the ice to"));
		}
		float ActiveDefault = 0.f;
		if (!Info.bExcluded && Material && !Material->GetScalarParameterDefaultValue(FHashedMaterialParameterInfo(Icify::ActiveParameterName()), ActiveDefault))
		{
			// Masks are still written (the debug display shows them), but the final material can't show them.
			Detail.Warnings.Add(FString::Printf(TEXT("material %s doesn't use Icify, so the ice won't show in Final Material (icicles still grow); ")
				TEXT("use Tools > Icify > Hook Up Materials In Level, or right-click the material > Add Icify"), *Material->GetName()));
		}

		Info.Description = FString::Printf(TEXT("%d %s -> %s%s%s%s%s"),
			Slot,
			*Detail.Material,
			*Detail.Surface,
			Rule ? *FString::Printf(TEXT(" [rule %s]"), *RuleName) : TEXT(""),
			Info.bIcicles ? TEXT("") : TEXT(" [no icicles]"),
			OutContext.bDripSourceMesh ? TEXT(" [melt source]") : TEXT(""),
			Info.bExcluded ? TEXT(" [excluded]") : TEXT(""));
		Report.Slots.Add(Info.Description);
	}
}

bool AIcifyFreezeZone::ShouldConsiderActor(const AActor* Actor, const FBox& ZoneBox, FString* OutReason) const
{
	if (!Actor || Actor == this || Actor->IsA<AIcifyFreezeZone>() || Actor->IsEditorOnly() || Actor->IsHiddenEd())
	{
		return false;
	}
	const FBox ActorBox = Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
	if (!ActorBox.IsValid || !ActorBox.Intersect(ZoneBox))
	{
		return false;
	}

	// Inside the zone but filtered by a setting: say which one.
	auto Filtered = [OutReason](const FString& Reason)
	{
		if (OutReason)
		{
			*OutReason = Reason;
		}
		return false;
	};
	if (Icify::HasIgnoreTag(Actor, nullptr))
	{
		return Filtered(FString::Printf(TEXT("actor has the '%s' tag"), *UIcifySettings::Get()->IgnoreTag.ToString()));
	}
	if (ExcludedActors.ContainsByPredicate([Actor](const TSoftObjectPtr<AActor>& Excluded) { return Excluded.Get() == Actor; }))
	{
		return Filtered(TEXT("actor is in the zone's Excluded Actors"));
	}
	if (IncludeOnlyActors.Num() > 0 && !IncludeOnlyActors.ContainsByPredicate([Actor](const TSoftObjectPtr<AActor>& Included) { return Included.Get() == Actor; }))
	{
		return Filtered(TEXT("actor is not in the zone's Include Only Actors"));
	}
	if (IncludeDataLayers.Num() > 0)
	{
		const TArray<const UDataLayerAsset*> ActorLayers = Actor->GetDataLayerAssets();
		if (!IncludeDataLayers.ContainsByPredicate([&ActorLayers](const TObjectPtr<const UDataLayerAsset>& Layer) { return Layer && ActorLayers.Contains(Layer.Get()); }))
		{
			return Filtered(TEXT("actor is in none of the zone's Include Data Layers"));
		}
	}
	return true;
}

const FIcifyComponentRecord* AIcifyFreezeZone::FindOriginalRecord(const UMeshComponent* Component) const
{
	// Whether the slots are free is decided by the pre-Icify values, not by values this or another zone wrote.
	if (const FIcifyComponentRecord* Own = FindAppliedRecord(Component))
	{
		return Own;
	}
	if (const UWorld* World = GetWorld())
	{
		for (TActorIterator<AIcifyFreezeZone> It(World); It; ++It)
		{
			if (*It != this)
			{
				if (const FIcifyComponentRecord* Other = It->FindAppliedRecord(Component))
				{
					return Other;
				}
			}
		}
	}
	return nullptr;
}

namespace
{
	/** Zone-owned instances wrap the original material; hash against the original. */
	const UMaterialInterface* UnwrapOwnedMaterial(const UMaterialInterface* Material, const UObject* Owner)
	{
		while (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Material))
		{
			if (Instance->GetOuter() != Owner)
			{
				break;
			}
			Material = Instance->Parent;
		}
		return Material;
	}

	struct FHasher
	{
		uint32 Hash = 0;
		void Add(uint32 Value) { Hash = HashCombine(Hash, Value); }
		void Add(float Value) { Add(GetTypeHash(Value)); }
		void Add(const FVector& Value) { Add(GetTypeHash(Value)); }
		void Add(const FString& Value) { Add(GetTypeHash(Value)); }
	};

	/** Everything run-wide that changes the masks or the drip candidates. Icicle shape settings are left out: they only re-plan. */
	uint32 HashParams(const FIcifyGenerationParams& P)
	{
		FHasher H;
		H.Add(P.Seed);
		for (const float Value : { P.Intensity, P.FeatureScale, P.ZoneFalloff, P.WindInfluence,
			P.FrostIntensity, P.FrostFilm, P.EdgeFrost, P.Rime, P.GroundFrost, P.GroundFrostHeight, P.EdgeAngleDeg, P.FrostEdgeWidth,
			P.SnowIntensity, P.SnowMinUp, P.CreviceSnow, P.WallPlaster, P.EdgeSnowLoss,
			P.GlazeIntensity, P.GlazeCoat, P.AmbientDrips, P.DripLength, P.DripSpread,
			P.BlackIceIntensity, P.FloorIce, P.CreviceIce, P.PuddleRadius, P.ThawIntensity,
			P.DripOverhangDeg, P.DripSpacing, P.MaskTexelSize })
		{
			H.Add(Value);
		}
		H.Add(P.ZoneTransform.GetLocation());
		H.Add(P.ZoneTransform.GetRotation().Euler());
		H.Add(P.ZoneTransform.GetScale3D());
		H.Add(P.ZoneExtent);
		H.Add(P.WindDir);
		H.Add(uint32(P.MaskOutput));
		H.Add(uint32(P.MaxMaskResolution));
		H.Add(uint32(P.MaskUVChannel));
		H.Add(uint32(P.bDripPoints));
		for (const FIcifyDripSource& Source : P.Sources)
		{
			H.Add(Source.Origin);
			H.Add(Source.Radius);
			H.Add(Source.Length);
			H.Add(Source.Strength);
			H.Add(uint32(Source.Path.Num()));
		}
		for (const FIcifyHeatSource& Heat : P.HeatSources)
		{
			H.Add(Heat.Location);
			H.Add(Heat.Radius);
		}
		for (const FIcifyPool& Pool : P.Pools)
		{
			H.Add(Pool.Center);
			H.Add(Pool.Radius);
		}
		for (const FIcifyStampData& Stamp : P.Stamps)
		{
			H.Add(Stamp.Transform.GetLocation());
			H.Add(Stamp.Transform.GetRotation().Euler());
			H.Add(Stamp.Transform.GetScale3D());
			const FIcifyStampLayers& L = Stamp.Layers;
			// The stamp's icicle layer only affects the plan, which is never cached.
			for (const float Value : { Stamp.Radius, Stamp.Falloff, Stamp.EdgeNoise, L.Frost, L.Snow, L.Glaze, L.BlackIce })
			{
				H.Add(Value);
			}
		}
		return H.Hash;
	}

	/** Everything about one component that changes its masks, beyond the run-wide inputs. */
	uint32 HashComponent(UMeshComponent* Component, const FIcifyComponentContext& Context)
	{
		FHasher H;
		H.Add(Component->GetPathName());
		const UObject* Mesh = Icify::MeshAssetOf(Component);
		H.Add(Mesh ? Mesh->GetPathName() : FString());
		H.Add(Icify::MeshBuildHash(Component));
		const FTransform& Transform = Component->GetComponentTransform();
		H.Add(Transform.GetLocation());
		H.Add(Transform.GetRotation().Euler());
		H.Add(Transform.GetScale3D());
		for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
		{
			const UMaterialInterface* Material = UnwrapOwnedMaterial(Component->GetMaterial(Slot), Component);
			H.Add(Material ? Material->GetPathName() : FString());
		}
		H.Add(Icify::HashSlotContext(Context));
		return H.Hash;
	}

	/** Physical coverage per slot and channel: global means hide sparse, deliberately local permissions. */
	void FillCoverage(const FIcifyComponentResult& Result, FIcifyComponentReport& Report)
	{
		static const TCHAR* ChannelNames[4] = { TEXT("Frost"), TEXT("Snow"), TEXT("Glaze"), TEXT("Black Ice") };
		for (int32 Slot = 0; Slot < Result.SlotCoverage.Num() && Slot < Report.SlotDetails.Num(); ++Slot)
		{
			const FIcifyComponentResult::FSlotCoverage& Measured = Result.SlotCoverage[Slot];
			FIcifySlotReport& Detail = Report.SlotDetails[Slot];
			Detail.AreaM2 = float(Measured.Area);
			Detail.Coverage.Reset();
			for (int32 Channel = 0; Channel < 4 && Measured.Area > 0.0; ++Channel)
			{
				FIcifyChannelCoverage& Row = Detail.Coverage.AddDefaulted_GetRef();
				Row.Channel = ChannelNames[Channel];
				Row.CoveredM2 = float(Measured.Covered[Channel]);
				Row.CoveredFraction = float(Measured.Covered[Channel] / Measured.Area);
				Row.MeanWhereCovered = Measured.Covered[Channel] > 0.0 ? float(Measured.Sum[Channel] / Measured.Covered[Channel]) : 0.f;
				Row.MaxValue = Measured.Max[Channel];
			}
		}
	}

	int64 ResultBytes(const FIcifyComponentResult& Result)
	{
		int64 Bytes = int64(Result.TexturePixels.Num()) * sizeof(FColor);
		for (const TArray<FColor>& LOD : Result.LODColors)
		{
			Bytes += int64(LOD.Num()) * sizeof(FColor);
		}
		Bytes += int64(Result.DripPoints.Num()) * sizeof(FIcifyDripPoint);
		return Bytes + 1024;
	}

	/** Copies the generated rows of a run that stopped before writing, without counting them as frozen. */
	void ReportDiscarded(FIcifyComponentReport& Report, const FIcifyComponentResult& Result, float Seconds, bool bFromCache)
	{
		const FString Kind = Result.Output == EIcifyMaskOutput::Texture ? TEXT("texture") : TEXT("vertex");
		Report.Result = TEXT("cancelled_before_write");
		Report.Notes = Result.Notes.TrimEnd() + FString::Printf(TEXT(" Generated %s masks were discarded before writing."), *Kind);
		Report.Vertices = Result.Vertices;
		Report.TextureSize = Result.TextureSize;
		Report.MeanMasks = Result.MeanMasks;
		Report.MaxMasks = Result.MaxMasks;
		Report.bNanite = Result.bNanite;
		Report.MaskUV = Result.MaskUV;
		Report.Seconds = Seconds;
		Report.bFromCache = bFromCache;
		Report.DripPoints = Result.DripPoints.Num();
		FillCoverage(Result, Report);
	}

	/** 0..1 from a drip point's hash: a fixed random per point and purpose. */
	float Rand01(uint32 Hash, uint32 Salt)
	{
		return float(Icify::HashU(Hash ^ (Salt * 0x9e3779b9u)) & 0x00FFFFFFu) / 16777216.f;
	}

	/** 0..1: snow lies above the edge, so meltwater from it reaches the edge. */
	float SkySupply(const FIcifySkyCoverField* Sky, const FIcifyDripPoint& Point, const AActor* Owner)
	{
		if (!Sky)
		{
			return 1.f;
		}
		// Thin ledges, eaves and rails: the air just above and behind the edge is open to the sky.
		const float Open = Sky->OpenAt(Point.P + FVector(0.0, 0.0, 25.0) - Point.Outward * 15.0);
		if (Open >= 0.99f)
		{
			return 1.f;
		}
		// Thick ledges and round things: what the snow lands on first, above the edge, is the edge's own actor or
		// something resting just on top of it (a coping stone, roof tiles over a fascia). A roof further up keeps it dry.
		double Height = 0.0;
		const AActor* Cover = nullptr;
		// Its own top (a thick ledge, a globe) doesn't shelter a drip edge, but the same actor's balcony two metres up does.
		if (!Sky->FirstHitAbove(Point.P, Height, Cover) || (Owner && Cover == Owner && Height <= 150.0) || Height <= 60.0)
		{
			return 1.f;
		}
		return Open;
	}

	/** 0..1: meltwater from a melt source runs over this point (straight down from the source, or along its traced path). */
	float MeltSupply(const FIcifyGenerationParams& P, const FVector& At)
	{
		float Supply = 0.f;
		for (const FIcifyDripSource& Source : P.Sources)
		{
			// Sources with a traced meltwater path are handled by the flow field below.
			if (P.Flow && Source.Path.Num() > 1)
			{
				continue;
			}
			const double Length = FMath::Max<double>(Source.Length > 0.f ? Source.Length : P.DripLength, 10.0);
			const FVector Rel = At - Source.Origin;
			const double Below = -Rel.Z;
			if (Below < -Source.Radius * 0.5 - 5.0 || Below > Length)
			{
				continue;
			}
			const double Width = Source.Radius * (1.0 + P.DripSpread * FMath::Max(Below, 0.0) / Length) + 5.0;
			const double Lateral = FVector(Rel.X, Rel.Y, 0.0).Size();
			if (Lateral > Width)
			{
				continue;
			}
			const float Along = 1.f - FMath::SmoothStep(float(Length * 0.6), float(Length), float(FMath::Max(Below, 0.0)));
			const float Across = 1.f - FMath::SmoothStep(float(Width * 0.6), float(Width), float(Lateral));
			Supply = FMath::Max(Supply, Source.Strength * Along * Across);
		}
		if (P.Flow)
		{
			P.Flow->ForEachNear(At, [&Supply](const FIcifySegmentField::FSegment& Segment, float Distance, float)
			{
				Supply = FMath::Max(Supply, Segment.Value * (1.f - FMath::SmoothStep(Segment.Radius * 0.5f, Segment.Radius, Distance)));
			});
		}
		return FMath::Clamp(Supply, 0.f, 1.f);
	}
}

void FIcifyGenerationJob::GenerateItem(FItem& Item)
{
	if (Item.bGenerated)
	{
		return;
	}
	const double Start = FPlatformTime::Seconds();
	// Only the snapshot taken at scan time is read here, never the live component.
	const bool bOk = Item.Snapshot.IsValid() && Icify::GenerateFromSnapshot(*Item.Snapshot, Item.Context, Params, Item.Result);
	Item.Snapshot.Reset();
	Item.Seconds = float(FPlatformTime::Seconds() - Start);
	Item.bGenerated = !Item.Result.bCancelled;
	Item.bFailed = !bOk && !Item.Result.bCancelled;
}

void AIcifyFreezeZone::GatherScene(FIcifyGenerationJob& Job, const FBox& ZoneBox)
{
	UWorld* World = GetWorld();
	FIcifyGenerationParams& P = Job.Params;

	CollectSources(P.Sources, P.HeatSources);
	// Every surface sample visits every source; keep a busy level from multiplying the bake time.
	constexpr int32 MaxSources = 128;
	if (P.Sources.Num() > MaxSources)
	{
		LastReport.Warnings.Add(FString::Printf(TEXT("%d melt sources found; only the first %d are used. Exclude some, or turn off Auto Detect Drip Sources."), P.Sources.Num(), MaxSources));
		P.Sources.SetNum(MaxSources);
	}
	if (P.HeatSources.Num() > MaxSources)
	{
		LastReport.Warnings.Add(FString::Printf(TEXT("%d heat sources found; only the first %d are used."), P.HeatSources.Num(), MaxSources));
		P.HeatSources.SetNum(MaxSources);
	}

	// Hand-placed stamps.
	if (bApplyFreezeStamps)
	{
		for (TActorIterator<AIcifyFreezeStamp> It(World); It; ++It)
		{
			if (It->GetStampBounds().Intersect(ZoneBox))
			{
				FIcifyStampData& Stamp = P.Stamps.AddDefaulted_GetRef();
				Stamp.Transform = It->GetActorTransform();
				Stamp.Radius = It->RadiusCm;
				Stamp.Falloff = It->Falloff;
				Stamp.EdgeNoise = It->EdgeNoise;
				Stamp.Layers = It->IceLayers;
			}
		}
	}

	// Meltwater: follow each source down the actual geometry into puddles, or drop straight down into one.
	Job.Flow.Reset(40.0);
	if (bTraceMeltwater)
	{
		const int32 MaxSteps = FMath::Clamp(FMath::CeilToInt((ZoneBox.GetSize().Z + 500.0) / FMath::Max(FlowStepCm, 2.f)) * 2, 10, 2000);
		for (FIcifyDripSource& Source : P.Sources)
		{
			Icify::TraceMeltwaterPath(World, Source.Origin, FlowStepCm, MaxSteps, this, Source.Path, P.Pools);
			const float Length = FMath::Max(Source.Length > 0.f ? Source.Length : DripLengthCm, 10.f);
			float Travelled = 0.f;
			for (int32 Index = 1; Index < Source.Path.Num(); ++Index)
			{
				FIcifySegmentField::FSegment Segment;
				Segment.A = Source.Path[Index - 1];
				Segment.B = Source.Path[Index];
				const float Step = float(FVector::Dist(Segment.A, Segment.B));
				Segment.AlongA = FMath::Clamp(Travelled / Length, 0.f, 1.f);
				Travelled += Step;
				Segment.AlongB = FMath::Clamp(Travelled / Length, 0.f, 1.f);
				Segment.Radius = Source.Radius * (1.f + DripSpread * Segment.AlongB);
				Segment.Value = Source.Strength;
				if (Segment.AlongA < 1.f)
				{
					Job.Flow.Add(Segment);
				}
			}
		}
	}
	else
	{
		for (const FIcifyDripSource& Source : P.Sources)
		{
			FIcifyPool& Pool = P.Pools.AddDefaulted_GetRef();
			Pool.Center = FVector(Source.Origin.X, Source.Origin.Y, Job.Floor.FloorBelow(Source.Origin));
			Pool.Radius = FMath::Max(Source.Radius, 10.f);
		}
	}
	P.Flow = Job.Flow.IsEmpty() ? nullptr : &Job.Flow;

	// What the viewport shows: the sources this run used, with their traced paths.
	MeltSourceViews.Reset();
	for (const FIcifyDripSource& Source : P.Sources)
	{
		FSourceView& View = MeltSourceViews.AddDefaulted_GetRef();
		View.Origin = Source.Origin;
		View.Radius = Source.Radius;
		View.Length = Source.Length > 0.f ? Source.Length : DripLengthCm;
		View.bManual = Source.bManual;
		View.Path = Source.Path;
	}
	HeatSourceViews.Reset();
	for (const FIcifyHeatSource& Heat : P.HeatSources)
	{
		HeatSourceViews.Add(Heat.Location);
	}
}

bool AIcifyFreezeZone::PrepareJob(FIcifyGenerationJob& Job, const TCHAR* Operation, const TFunction<bool()>& ShouldCancel)
{
	UWorld* World = GetWorld();
	const FBox ZoneBox = ZoneComponent->Bounds.GetBox();
	Job.Operation = Operation;

	LastReport = FIcifyRunReport();
	LastReport.Operation = Operation;
	LastReport.Zone = GetActorNameOrLabel();
	LastReport.Seed = Seed;
	LastReport.Time = FDateTime::Now().ToIso8601();
	LastReport.Preset = Preset ? Preset->GetPathName() : FString();

	auto Cancelled = [this, &Job](const TCHAR* Phase)
	{
		Job.bCancelled = true;
		Job.CancelledDuring = Phase;
		LastReport.bCancelled = true;
		LastReport.CancelledDuring = Phase;
		return false;
	};
	auto CancelRequested = [&ShouldCancel]() { return ShouldCancel && ShouldCancel(); };

	{
		FIcifyPhaseScope Phase(LastReport, TEXT("Floor traces"));
		if (FloorMode == EIcifyFloorMode::Trace)
		{
			if (!Job.Floor.BuildTraced(World, ZoneBox, FloorProbeSpacingCm, this, ShouldCancel))
			{
				return Cancelled(TEXT("Floor traces"));
			}
			Phase.Items = Job.Floor.NumCells();
		}
		else
		{
			Job.Floor.BuildFlat(ZoneBox.Min.Z);
		}
	}

	if (bUseSkyCover)
	{
		FIcifyPhaseScope Phase(LastReport, TEXT("Sky cover"));
		if (!Job.Sky.Build(World, ZoneBox, GetSnowfallDirection(), SkyCellSizeCm, SkyReachCm, this, ShouldCancel))
		{
			return Cancelled(TEXT("Sky cover"));
		}
		Phase.Items = Job.Sky.NumCells();
		LastReport.SkyCells = Job.Sky.NumCells();
	}

	BuildParams(Job.Params);
	Job.Params.Floor = &Job.Floor;
	Job.Params.Sky = bUseSkyCover && Job.Sky.IsValid() ? &Job.Sky : nullptr;

	{
		FIcifyPhaseScope Phase(LastReport, TEXT("Scene sources"));
		GatherScene(Job, ZoneBox);
		Phase.Items = Job.Params.Sources.Num() + Job.Params.HeatSources.Num() + Job.Params.Stamps.Num() + Job.Params.Pools.Num();
	}
	LastReport.MeltSources = Job.Params.Sources.Num();
	LastReport.HeatSources = Job.Params.HeatSources.Num();
	if (CancelRequested())
	{
		return Cancelled(TEXT("Scene sources"));
	}

	Job.EnvironmentHash = HashCombine(HashCombine(HashParams(Job.Params), Job.Floor.Hash()), HashCombine(Job.Sky.Hash(), Job.Flow.Hash()));

	auto Skip = [this](FIcifyComponentReport& Report, const FString& Reason)
	{
		Report.Result = TEXT("skipped");
		Report.Notes = Reason;
		LastReport.Components.Add(MoveTemp(Report));
	};

	FIcifyPhaseScope Phase(LastReport, TEXT("Scan"));

	// Which meshes other zones already own (applied) or are previewing.
	TMap<const UMeshComponent*, AIcifyFreezeZone*> AppliedBy;
	TMap<const UMeshComponent*, AIcifyFreezeZone*> PreviewedBy;
	for (TActorIterator<AIcifyFreezeZone> It(World); It; ++It)
	{
		AIcifyFreezeZone* Other = *It;
		if (Other == this)
		{
			continue;
		}
		for (const FIcifyComponentRecord& Record : Other->AppliedRecords)
		{
			if (const UMeshComponent* Owned = Record.Component.Get())
			{
				AppliedBy.Add(Owned, Other);
			}
		}
		for (const FIcifyComponentRecord& Record : Other->PreviewRecords)
		{
			if (const UMeshComponent* Owned = Record.Component.Get())
			{
				PreviewedBy.Add(Owned, Other);
			}
		}
	}

	const bool bWrites = FCString::Strcmp(Operation, TEXT("Apply")) == 0;
	const double Now = FPlatformTime::Seconds();
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		FString ActorReason;
		if (!ShouldConsiderActor(Actor, ZoneBox, &ActorReason))
		{
			if (!ActorReason.IsEmpty())
			{
				FIcifyComponentReport Report;
				Report.Actor = Actor->GetActorNameOrLabel();
				Report.Component = TEXT("(all components)");
				Skip(Report, ActorReason);
			}
			continue;
		}

		TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
		for (UStaticMeshComponent* Component : Components)
		{
			if (!Component || !Component->IsRegistered() || Component->IsEditorOnly() || !Icify::MeshAssetOf(Component)
				|| !Component->Bounds.GetBox().Intersect(ZoneBox))
			{
				continue;
			}

			FIcifyComponentReport Report;
			Report.Actor = Actor->GetActorNameOrLabel();
			Report.Component = Component->GetName();
			Report.Mesh = Icify::MeshAssetOf(Component)->GetName();

			if (Component->IsA<UInstancedStaticMeshComponent>())
			{
				Skip(Report, TEXT("instanced meshes and foliage aren't supported yet"));
				continue;
			}
			if (Component->IsA<USplineMeshComponent>())
			{
				Skip(Report, TEXT("spline meshes aren't supported (they bend in the shader, so their drip edges can't be found)"));
				continue;
			}
			if (Icify::HasIgnoreTag(nullptr, Component))
			{
				Skip(Report, FString::Printf(TEXT("component has the '%s' tag"), *UIcifySettings::Get()->IgnoreTag.ToString()));
				continue;
			}
			if (RequiredComponentTags.Num() > 0 && !RequiredComponentTags.ContainsByPredicate([Component](const FName& Tag) { return Component->ComponentHasTag(Tag); }))
			{
				TArray<FString> TagNames;
				for (const FName& Tag : RequiredComponentTags)
				{
					TagNames.Add(Tag.ToString());
				}
				Skip(Report, FString::Printf(TEXT("component has none of the Required Component Tags (%s)"), *FString::Join(TagNames, TEXT(", "))));
				continue;
			}
			if (!bIncludeMovable && Component->Mobility == EComponentMobility::Movable)
			{
				Skip(Report, TEXT("movable (enable Include Movable)"));
				continue;
			}
			if (AIcifyFreezeZone* const* Previewer = PreviewedBy.Find(Component))
			{
				Skip(Report, FString::Printf(TEXT("being previewed by %s"), *(*Previewer)->GetActorNameOrLabel()));
				continue;
			}
			AIcifyFreezeZone* TakeOverFrom = nullptr;
			if (AIcifyFreezeZone* const* AppliedOwner = AppliedBy.Find(Component))
			{
				Report.PreviousOwner = (*AppliedOwner)->GetActorNameOrLabel();
				// Taking over is an Apply operation, never a temporary snapshot of another owner's layer.
				if (OverlapPolicy == EIcifyOverlapPolicy::SkipOwned || !bWrites)
				{
					Skip(Report, OverlapPolicy == EIcifyOverlapPolicy::TakeOver
						? FString::Printf(TEXT("owned by %s (%s never takes over; use Take Over and Apply)"), *Report.PreviousOwner, Operation)
						: FString::Printf(TEXT("owned by %s (set Overlap Policy to Take Over)"), *Report.PreviousOwner));
					continue;
				}
				TakeOverFrom = *AppliedOwner;
			}

			FIcifyGenerationJob::FItem& Item = Job.Items.AddDefaulted_GetRef();
			Item.Component = Component;
			Item.Path = Component->GetPathName();
			Item.TakeOverFrom = TakeOverFrom;
			Item.Report = MoveTemp(Report);
			BuildContext(Component, Job.Samplers, Item.Context, Item.Report);

			// Generation must never read the zone's rule array; give each slot its own gate copy.
			for (FIcifySlotInfo& Slot : Item.Context.Slots)
			{
				if (Slot.Gate)
				{
					Slot.Gate = Job.GateCopies.Add_GetRef(MakeUnique<FIcifyGate>(*Slot.Gate)).Get();
				}
			}

			if (!Item.Context.Slots.ContainsByPredicate([](const FIcifySlotInfo& Slot) { return !Slot.bExcluded; }))
			{
				Skip(Item.Report, TEXT("every material slot is excluded (see Slot Details for each reason)"));
				Job.Items.Pop();
				continue;
			}

			if (bUseGenerationCache)
			{
				Item.Key = HashCombine(Job.EnvironmentHash, HashComponent(Component, Item.Context));
				if (const TSharedPtr<FIcifyCacheEntry>* Entry = GenerationCache.Find(Item.Path))
				{
					if ((*Entry)->Key == Item.Key && (*Entry)->Result.IsValid())
					{
						Item.Result = *(*Entry)->Result;
						Item.Result.Component = Component;
						Item.bGenerated = true;
						Item.bFromCache = true;
						(*Entry)->LastUsed = Now;
					}
				}
			}

			// Immutable inputs for generation. Cached items don't need them.
			if (!Item.bGenerated)
			{
				Item.Snapshot = MakeShared<FIcifyMeshSnapshot>();
				if (!Icify::SnapshotMesh(Component, *Item.Snapshot))
				{
					Skip(Item.Report, TEXT("mesh data isn't readable on the CPU"));
					Job.Items.Pop();
					continue;
				}
			}
		}
	}
	Phase.Items = Job.Items.Num();
	return true;
}

void AIcifyFreezeZone::CollectJob(FIcifyGenerationJob& Job, TArray<FIcifyComponentResult>& OutResults, TMap<UMeshComponent*, AIcifyFreezeZone*>& OutTakeOvers)
{
	const double Now = FPlatformTime::Seconds();
	for (FIcifyGenerationJob::FItem& Item : Job.Items)
	{
		UMeshComponent* Component = Item.Component.Get();
		FIcifyComponentReport& Report = Item.Report;
		if (!Component || !Item.bGenerated || Item.bFailed)
		{
			Report.Result = TEXT("skipped");
			Report.Notes = !Component ? TEXT("component no longer exists") : TEXT("no readable mesh data");
			LastReport.Components.Add(MoveTemp(Report));
			continue;
		}

		FIcifyComponentResult& Result = Item.Result;
		Result.Component = Component;
		Result.MaterialSurfaces.Reset();
		for (const FIcifySlotInfo& Slot : Item.Context.Slots)
		{
			Result.MaterialSurfaces.Add(Slot.bExcluded ? EIcifySurfaceType::Auto : Slot.Surface);
		}

		if (bUseGenerationCache && !Item.bFromCache && Item.Key != 0)
		{
			TSharedPtr<FIcifyCacheEntry> Entry = MakeShared<FIcifyCacheEntry>();
			Entry->Key = Item.Key;
			Entry->Result = MakeShared<FIcifyComponentResult>(Result);
			Entry->Bytes = ResultBytes(Result);
			Entry->LastUsed = Now;
			GenerationCache.Add(Item.Path, Entry);
		}

		Report.Result = Result.Output == EIcifyMaskOutput::Texture ? TEXT("texture") : TEXT("vertex");
		Report.Notes = Result.Notes.TrimEnd();
		Report.Vertices = Result.Vertices;
		Report.TextureSize = Result.TextureSize;
		Report.MeanMasks = Result.MeanMasks;
		Report.MaxMasks = Result.MaxMasks;
		Report.bNanite = Result.bNanite;
		Report.MaskUV = Result.MaskUV;
		Report.Seconds = Item.Seconds;
		Report.bFromCache = Item.bFromCache;
		Report.DripPoints = Result.DripPoints.Num();
		FillCoverage(Result, Report);
		LastReport.CacheHits += Item.bFromCache ? 1 : 0;

		if (AIcifyFreezeZone* AppliedOwner = Item.TakeOverFrom.Get())
		{
			OutTakeOvers.Add(Component, AppliedOwner);
		}
		Result.ReportIndex = LastReport.Components.Add(MoveTemp(Report));
		OutResults.Add(MoveTemp(Result));
	}
	TrimCache();
}

bool AIcifyFreezeZone::GenerateResults(const TCHAR* Operation, FIcifyGenerationJob& Job, TArray<FIcifyComponentResult>& OutResults,
	TMap<UMeshComponent*, AIcifyFreezeZone*>& OutTakeOvers)
{
	// Progress dialog with a Cancel button, shown only if the run takes noticeable time.
	// Cancellation is honoured at every phase boundary and inside long phases; nothing is written before this returns.
	FScopedSlowTask Task(100.f, FText::Format(LOCTEXT("Generating", "Icify {0}: {1}"), FText::FromString(Operation), FText::FromString(GetActorNameOrLabel())));
	Task.MakeDialogDelayed(0.3f, /*bShowCancelButton*/ true);
	const TFunction<bool()> ShouldCancel = [&Task]() { return Task.ShouldCancel(); };

	Task.EnterProgressFrame(15.f, LOCTEXT("PhasePrepare", "Tracing floors and sky, finding melt and heat sources, scanning meshes..."));
	if (!PrepareJob(Job, Operation, ShouldCancel))
	{
		return false;
	}
	Job.Params.ShouldCancel = ShouldCancel;
	// The dialog dies with this function: nothing may poll it afterwards.
	ON_SCOPE_EXIT
	{
		Job.Params.ShouldCancel = nullptr;
	};

	// A later mesh can cancel the whole run before the write phase. Keep the diagnostics of the masks already
	// generated, without reporting them as frozen in either the rows or the aggregate.
	auto CancelDuringGeneration = [this, &Job]()
	{
		LastReport.bCancelled = true;
		LastReport.CancelledDuring = TEXT("Mask generation");
		for (FIcifyGenerationJob::FItem& Item : Job.Items)
		{
			if (!Item.bGenerated || Item.bFailed || !Item.Component.IsValid())
			{
				continue;
			}
			ReportDiscarded(Item.Report, Item.Result, Item.Seconds, Item.bFromCache);
			LastReport.Components.Add(MoveTemp(Item.Report));
		}
		return false;
	};

	Task.EnterProgressFrame(70.f);
	{
		FIcifyPhaseScope Phase(LastReport, TEXT("Mask generation"));
		FScopedSlowTask MeshTask(float(FMath::Max(Job.Items.Num(), 1)), LOCTEXT("PhaseGenerate", "Generating masks..."));
		for (int32 Index = 0; Index < Job.Items.Num(); ++Index)
		{
			FIcifyGenerationJob::FItem& Item = Job.Items[Index];
			MeshTask.EnterProgressFrame(1.f, FText::Format(LOCTEXT("PhaseGenerateMesh", "Generating masks: {0} / {1}  ({2} of {3}){4}"),
				FText::FromString(Item.Report.Actor), FText::FromString(Item.Report.Component), Index + 1, Job.Items.Num(),
				Item.bFromCache ? LOCTEXT("Cached", "  [cached]") : FText::GetEmpty()));
			if (ShouldCancel())
			{
				return CancelDuringGeneration();
			}
			Job.GenerateItem(Item);
			if (Item.Result.bCancelled)
			{
				return CancelDuringGeneration();
			}
			Phase.Items += Item.bFromCache ? 0 : 1;
		}
	}
	CollectJob(Job, OutResults, OutTakeOvers);

	Task.EnterProgressFrame(15.f, LOCTEXT("PhaseIcicles", "Growing icicles..."));
	if (!PlanIcicles(Job, OutResults, ShouldCancel))
	{
		LastReport.bCancelled = true;
		LastReport.CancelledDuring = TEXT("Icicle planning");
		for (const FIcifyComponentResult& Result : OutResults)
		{
			FIcifyComponentReport& Report = LastReport.Components[Result.ReportIndex];
			ReportDiscarded(Report, Result, Report.Seconds, Report.bFromCache);
		}
		OutResults.Reset();
		OutTakeOvers.Reset();
		return false;
	}
	return true;
}

// ----------------------------------------------------------------- Icicles

bool AIcifyFreezeZone::PlanIcicles(FIcifyGenerationJob& Job, const TArray<FIcifyComponentResult>& Results, const TFunction<bool()>& ShouldCancel)
{
	FIcifyPhaseScope Phase(LastReport, TEXT("Icicle planning"));
	Job.Icicles.Reset();
	Job.IcicleStats = FIcifyIcicleStats();
	FIcifyIcicleStats& Stats = Job.IcicleStats;
	const FIcifyGenerationParams& P = Job.Params;
	UWorld* World = GetWorld();
	if (!World || !bGrowIcicles || IcicleDensity <= 0.f || MaxIcicles <= 0)
	{
		return true;
	}

	const float MaxLength = FMath::Max(IcicleMaxLengthCm, 1.f);
	const float MinLength = FMath::Clamp(IcicleMinLengthCm, 0.5f, MaxLength);
	const FVector Up(0.0, 0.0, 1.0);

	struct FCandidate
	{
		FVector P;
		FVector Tangent;
		FVector Outward;
		float Length = 0.f;
		float Radius = 0.f;
		float Frost = 0.f;
		uint32 Hash = 0;
		int32 ReportIndex = INDEX_NONE;
	};
	TArray<FCandidate> Candidates;

	// Meshes a write would skip for a custom data conflict grow nothing, so they can't use up the icicle budget or crowd
	// out their neighbours' icicles. The same test Apply and Dry Run make.
	auto WouldWrite = [this](const FIcifyComponentResult& MeshResult)
	{
		if (!IsValid(MeshResult.Component))
		{
			return false;
		}
		const FIcifyComponentRecord* Original = FindOriginalRecord(MeshResult.Component);
		const TArray<float>& Defaults = Original ? Original->CustomData : MeshResult.Component->GetDefaultCustomPrimitiveData().Data;
		const TArray<float>& Live = Original ? Original->LiveCustomData : MeshResult.Component->GetCustomPrimitiveData().Data;
		return Icify::CustomDataSlotsFree(Defaults, Live, UIcifySettings::Get()->CustomPrimitiveDataIndex);
	};

	// 1. What each drip point would grow, from meltwater, heat, stamps and the density draw.
	for (const FIcifyComponentResult& Result : Results)
	{
		if (Result.DripPoints.Num() == 0 || !WouldWrite(Result))
		{
			continue;
		}
		const AActor* ComponentOwner = Result.Component ? Result.Component->GetOwner() : nullptr;
		// Candidates sit at half the requested spacing (wider on huge meshes); the draw keeps the requested density.
		const float Spacing = Result.DripSpacingCm > 0.f ? Result.DripSpacingCm : P.DripSpacing;
		const float AcceptRate = FMath::Clamp(Spacing * IcicleDensity / 100.f, 0.f, 1.f);
		for (const FIcifyDripPoint& Point : Result.DripPoints)
		{
			++Stats.Candidates;

			// Hand-placed stamps grow icicles (and the meltwater for them) or erase them.
			float StampAdd = 0.f;
			float StampKeep = 1.f;
			for (const FIcifyStampData& Stamp : P.Stamps)
			{
				const float Layer = Stamp.Layers.Icicles;
				const float Weight = Layer != 0.f ? Icify::StampWeight(Stamp, Point.P, P.FeatureScale) : 0.f;
				if (Layer > 0.f)
				{
					StampAdd += Weight * Layer;
				}
				else if (Layer < 0.f)
				{
					StampKeep *= FMath::Max(0.f, 1.f + Weight * Layer);
				}
			}
			if (StampKeep <= 0.01f)
			{
				++Stats.Erased;
				continue;
			}

			// Meltwater: snow above the edge, a melt source upstream, or the edge belongs to a gutter or downpipe.
			float Supply = 1.f;
			if (bIciclesNeedMeltwater && !Point.bDripSource)
			{
				Supply = FMath::Max(SkySupply(P.Sky, Point, ComponentOwner), MeltSupply(P, Point.P));
			}
			Supply = FMath::Max(Supply, FMath::Min(StampAdd, 1.f));
			if (Supply < 0.05f)
			{
				++Stats.NoMeltwater;
				continue;
			}
			const float Thaw = Icify::ThawAt(P, Point.P);
			if (Thaw >= 0.5f)
			{
				++Stats.Thawed;
				continue;
			}

			// Meltwater gathers into a few runs along an edge: clumps of long icicles with bare gaps between.
			const float Clump = FMath::SmoothStep(0.25f, 0.75f, Icify::ValueNoise(Point.P / 60.0, P.Seed + 911u));
			float Feed = Point.Strength * Supply * (1.f - 2.f * Thaw);
			Feed *= 1.f + IcicleClumping * (0.15f + 1.1f * Clump - 1.f);
			Feed = FMath::Clamp(Feed * (1.f + StampAdd) * StampKeep, 0.f, 1.5f);

			if (Rand01(Point.Hash, 1) >= Feed * AcceptRate)
			{
				++Stats.Thinned;
				continue;
			}
			const float Length = MaxLength * FMath::Pow(FMath::Min(Feed, 1.f), 0.6f) * (0.25f + 0.75f * FMath::Pow(Rand01(Point.Hash, 2), 0.7f));
			if (Length < MinLength)
			{
				++Stats.Thinned;
				continue;
			}

			FCandidate& Candidate = Candidates.AddDefaulted_GetRef();
			Candidate.P = Point.P;
			Candidate.Tangent = Point.Tangent;
			Candidate.Outward = Point.Outward;
			Candidate.Length = Length;
			Candidate.Radius = FMath::Clamp(Length * IcicleThickness * (0.055f + 0.045f * Rand01(Point.Hash, 3)), 0.35f, 6.f);
			Candidate.Frost = Point.Frost;
			Candidate.Hash = Point.Hash;
			Candidate.ReportIndex = Result.ReportIndex;
		}
	}
	if (ShouldCancel && ShouldCancel())
	{
		return false;
	}

	// 2. Longest first: the big icicles claim their place, smaller ones fill the gaps between them.
	Candidates.Sort([](const FCandidate& A, const FCandidate& B)
	{
		if (A.Length != B.Length)
		{
			return A.Length > B.Length;
		}
		if (A.Hash != B.Hash)
		{
			return A.Hash < B.Hash;
		}
		if (A.P.X != B.P.X)
		{
			return A.P.X < B.P.X;
		}
		return A.P.Y != B.P.Y ? A.P.Y < B.P.Y : A.P.Z < B.P.Z;
	});

	// Kept icicles in a grid, for spacing checks. The largest spacing (radius 6 + 6) is under one cell.
	constexpr double GridCell = 16.0;
	TMap<FIntVector, TArray<int32, TInlineAllocator<4>>> Grid;
	TArray<FVector> KeptP;
	TArray<float> KeptR;
	auto CellOf = [](const FVector& V)
	{
		return FIntVector(FMath::FloorToInt(V.X / GridCell), FMath::FloorToInt(V.Y / GridCell), FMath::FloorToInt(V.Z / GridCell));
	};
	auto TooClose = [&](const FVector& At, float Radius)
	{
		const FIntVector Cell = CellOf(At);
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 DZ = -1; DZ <= 1; ++DZ)
				{
					if (const TArray<int32, TInlineAllocator<4>>* List = Grid.Find(Cell + FIntVector(DX, DY, DZ)))
					{
						for (const int32 Index : *List)
						{
							if (FVector::DistSquared(At, KeptP[Index]) < FMath::Square(0.9 * (Radius + KeptR[Index]) + 1.0))
							{
								return true;
							}
						}
					}
				}
			}
		}
		return false;
	};

	// Room traces see what the snow sees: ignored actors neither shelter nor block, and the zone's own icicles don't count.
	FCollisionQueryParams Query(SCENE_QUERY_STAT(IcifyIcicleRoom), /*bTraceComplex*/ true);
	Query.AddIgnoredActor(this);
	const FName IgnoreTag = UIcifySettings::Get()->IgnoreTag;
	auto Blocked = [World, &Query, IgnoreTag](const FVector& From, const FVector& To, FHitResult& Hit)
	{
		for (int32 Attempt = 0; Attempt < 4; ++Attempt)
		{
			if (!World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Query))
			{
				return false;
			}
			const AActor* HitActor = Hit.GetActor();
			const UPrimitiveComponent* HitComponent = Hit.GetComponent();
			if (!Icify::HasIgnoreTag(HitActor, HitComponent))
			{
				return true;
			}
			if (HitActor && HitActor->ActorHasTag(IgnoreTag))
			{
				Query.AddIgnoredActor(HitActor);
			}
			else if (HitComponent)
			{
				Query.AddIgnoredComponent(HitComponent);
			}
		}
		return false;
	};

	// 3. Spacing, then room: an edge against a wall can't hang an icicle, and one stops short of whatever is below it.
	const FVector Lean = P.WindDir * (IcicleLean * P.WindInfluence * 0.5f);
	const int32 MaxTraced = MaxIcicles * 6 + 2000;
	int32 Traced = 0;
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		const FCandidate& C = Candidates[Index];
		if (Job.Icicles.Num() >= MaxIcicles || Traced >= MaxTraced)
		{
			Stats.OverBudget += Candidates.Num() - Index;
			break;
		}
		if ((Index & 255) == 0 && ShouldCancel && ShouldCancel())
		{
			return false;
		}
		if (TooClose(C.P, C.Radius))
		{
			++Stats.Thinned;
			continue;
		}

		++Traced;
		FHitResult Hit;
		if (!C.Outward.IsNearlyZero())
		{
			// From just under the edge, out into the air it should hang in.
			const FVector Under = C.P - Up * 3.0 - C.Outward * 1.0;
			if (Blocked(Under, Under + C.Outward * 13.0, Hit))
			{
				++Stats.NoRoom;
				continue;
			}
		}

		FVector Down = FVector(0.0, 0.0, -1.0) + Lean;
		Down.X += (Rand01(C.Hash, 4) - 0.5f) * 0.06f;
		Down.Y += (Rand01(C.Hash, 5) - 0.5f) * 0.06f;
		Down.Normalize();

		// The root tucks slightly back under the edge and up into it, so there's never a gap at the top.
		const FVector Back = C.Outward * (C.Radius * 0.5);
		const FVector Root = C.P - Back + Up * (0.3 + 0.25 * C.Radius);
		const FVector Start = C.P - Back - Up * 1.5;
		float Length = C.Length;
		if (Blocked(Start, Start + Down * (Length + 2.0), Hit))
		{
			Length = FMath::Min(Length, float(Hit.Distance + FVector::Dist(Root, Start)) - 1.f);
		}
		if (Length < MinLength)
		{
			++Stats.NoRoom;
			continue;
		}

		FIcifyGenerationJob::FPlannedIcicle& Planned = Job.Icicles.AddDefaulted_GetRef();
		Planned.ReportIndex = C.ReportIndex;
		FIcifyIcicleSpec& Spec = Planned.Spec;
		Spec.Root = Root;
		Spec.Down = Down;
		Spec.Along = C.Tangent;
		Spec.Length = Length;
		Spec.Radius = C.Radius;
		Spec.RippleCm = IcicleRippleCm;
		Spec.Frost = FMath::Clamp(C.Frost, 0.f, 1.f);
		Spec.Bend = 0.25f * (0.6f + 0.8f * Rand01(C.Hash, 6));
		Spec.Seed = C.Hash;
		Spec.Sides = IcicleSides;

		const int32 Kept = KeptP.Add(C.P);
		KeptR.Add(C.Radius);
		Grid.FindOrAdd(CellOf(C.P)).Add(Kept);
	}
	Phase.Items = Job.Icicles.Num();
	return true;
}

void AIcifyFreezeZone::BuildIcicles(const FIcifyGenerationJob& Job, const TSet<int32>& FrozenReports, FIcifyIcicleBuild& OutBuild)
{
	OutBuild = FIcifyIcicleBuild();
	OutBuild.Origin = GetActorLocation();
	OutBuild.Stats = Job.IcicleStats;
	FIcifyIcicleStats& Stats = OutBuild.Stats;
	Stats.Icicles = 0;
	Stats.Vertices = 0;
	Stats.Triangles = 0;
	Stats.LongestCm = 0.f;
	Stats.MeanLengthCm = 0.f;

	// Sections stay a manageable size; each is one draw.
	constexpr int32 MaxSectionVertices = 60000;
	double TotalLength = 0.0;
	for (const FIcifyGenerationJob::FPlannedIcicle& Planned : Job.Icicles)
	{
		// Icicles only grow on meshes that actually froze (not on custom data conflicts or failed writes).
		if (!FrozenReports.Contains(Planned.ReportIndex))
		{
			continue;
		}
		const int32 Count = Icify::IcicleVertexCount(Planned.Spec);
		if (Count == 0)
		{
			continue;
		}
		if (OutBuild.Sections.Num() == 0 || OutBuild.Sections.Last().NumVertices() + Count > MaxSectionVertices)
		{
			OutBuild.Sections.AddDefaulted();
		}
		Icify::AppendIcicle(Planned.Spec, OutBuild.Origin, OutBuild.Sections.Last());
		++Stats.Icicles;
		++OutBuild.PerReport.FindOrAdd(Planned.ReportIndex);
		TotalLength += Planned.Spec.Length;
		Stats.LongestCm = FMath::Max(Stats.LongestCm, Planned.Spec.Length);
	}
	for (const FIcifyIcicleMesh& Section : OutBuild.Sections)
	{
		Stats.Vertices += Section.NumVertices();
		Stats.Triangles += Section.NumTriangles();
	}
	Stats.MeanLengthCm = Stats.Icicles > 0 ? float(TotalLength / Stats.Icicles) : 0.f;

	LastReport.IcicleStats = Stats;
	for (const TPair<int32, int32>& Pair : OutBuild.PerReport)
	{
		if (LastReport.Components.IsValidIndex(Pair.Key))
		{
			LastReport.Components[Pair.Key].Icicles = Pair.Value;
		}
	}
}

UMaterialInterface* AIcifyFreezeZone::ResolveIcicleMaterial(UProceduralMeshComponent* IcicleComponent)
{
	UMaterialInterface* Base = IcicleMaterial ? IcicleMaterial.Get() : UIcifySettings::Get()->IcicleMaterial.LoadSynchronous();
	if (!Base)
	{
		LastReport.Warnings.AddUnique(TEXT("Icicle material not found; icicles use the default material. Run Tools > Icify > Rebuild Plugin Content, or set Icicle Material."));
		return UMaterial::GetDefaultMaterial(MD_Surface);
	}
	if (!Palette.bOverrideIceTint && !Palette.bOverrideFrostColor)
	{
		return Base;
	}

	// The zone's palette tints its own icicles through an instance that lives inside the icicle component.
	const FName Name = MakeUniqueObjectName(IcicleComponent, UMaterialInstanceConstant::StaticClass(), TEXT("IcifyIcicleMI"));
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(IcicleComponent, Name, RF_Transactional);
	Instance->SetParentEditorOnly(Base);
	if (Palette.bOverrideIceTint)
	{
		Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(Icify::IceTintParameterName()), Palette.IceTint);
	}
	if (Palette.bOverrideFrostColor)
	{
		Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(Icify::FrostColorParameterName()), Palette.FrostColor);
	}
	Instance->PostEditChange();
	return Instance;
}

void AIcifyFreezeZone::WriteIcicles(UProceduralMeshComponent* Target, const FIcifyIcicleBuild& Build, bool bTransactional)
{
	if (!Target)
	{
		return;
	}
	if (bTransactional)
	{
		Target->SetFlags(RF_Transactional);
		Target->Modify();
	}

	Target->ClearAllMeshSections();
	Target->EmptyOverrideMaterials();
	ConfigureIcicleComponent(Target);
	Target->SetWorldLocationAndRotation(Build.Origin, FQuat::Identity);
	Target->SetWorldScale3D(FVector::OneVector);

	UMaterialInterface* Material = Build.Sections.Num() > 0 ? ResolveIcicleMaterial(Target) : nullptr;
	TArray<FProcMeshTangent> Tangents;
	for (int32 Index = 0; Index < Build.Sections.Num(); ++Index)
	{
		const FIcifyIcicleMesh& Mesh = Build.Sections[Index];
		Tangents.Reset(Mesh.Tangents.Num());
		for (const FVector& Tangent : Mesh.Tangents)
		{
			Tangents.Emplace(Tangent, /*bFlipTangentY*/ false);
		}
		Target->CreateMeshSection_LinearColor(Index, Mesh.Positions, Mesh.Triangles, Mesh.Normals, Mesh.UVs, Mesh.Colors, Tangents, bIcicleCollision);
		Target->SetMaterial(Index, Material);
	}
	Target->MarkRenderStateDirty();
}

// -------------------------------------------------------------------- Cache

void AIcifyFreezeZone::TrimCache()
{
	int64 Total = 0;
	for (const TPair<FString, TSharedPtr<FIcifyCacheEntry>>& Pair : GenerationCache)
	{
		Total += Pair.Value->Bytes;
	}
	const int64 Budget = int64(FMath::Max(CacheBudgetMB, 16)) * 1024 * 1024;
	while (Total > Budget && GenerationCache.Num() > 0)
	{
		FString Oldest;
		double OldestTime = TNumericLimits<double>::Max();
		for (const TPair<FString, TSharedPtr<FIcifyCacheEntry>>& Pair : GenerationCache)
		{
			if (Pair.Value->LastUsed < OldestTime)
			{
				OldestTime = Pair.Value->LastUsed;
				Oldest = Pair.Key;
			}
		}
		Total -= GenerationCache[Oldest]->Bytes;
		GenerationCache.Remove(Oldest);
	}
}

// -------------------------------------------------------------------- Write

bool AIcifyFreezeZone::WriteResult(const FIcifyComponentResult& Result, EIcifyPreviewDisplay Display, const FIcifyComponentRecord& OriginalState)
{
	UMeshComponent* Component = Result.Component;
	FIcifyComponentReport& Entry = LastReport.Components[Result.ReportIndex];
	auto Fail = [&Entry](const TCHAR* Reason)
	{
		Entry.Result = TEXT("failed");
		Entry.Notes += FString(TEXT(" ")) + Reason;
		return false;
	};

	// Never overwrite custom data another system put on this mesh. Without our slots the material path stays
	// inactive, so don't write masks either.
	if (!Icify::CustomDataSlotsFree(OriginalState.CustomData, OriginalState.LiveCustomData, UIcifySettings::Get()->CustomPrimitiveDataIndex))
	{
		Entry.bCustomDataConflict = true;
		Entry.Result = TEXT("skipped");
		return false;
	}

	Component->SetFlags(RF_Transactional);
	Component->Modify();

	UMaterialInterface* DebugMaterial = nullptr;
	if (Display != EIcifyPreviewDisplay::FinalMaterial)
	{
		DebugMaterial = UIcifySettings::Get()->MaskDebugMaterial.LoadSynchronous();
		if (!DebugMaterial)
		{
			UE_LOG(LogIcify, Warning, TEXT("Mask debug material not found. Run Tools > Icify > Rebuild Plugin Content."));
			return Fail(TEXT("requested debug material is unavailable"));
		}
	}

	const bool bTexture = Result.Output == EIcifyMaskOutput::Texture;
	if (bTexture)
	{
		UTexture2D* Texture = Icify::CreateMaskTexture(Component, Result.TextureSize, Result.TexturePixels);
		if (!Texture || !Icify::BindOwnedMaterials(Component, Texture, Result.MaterialSurfaces, DebugMaterial, &Palette,
			Result.MaskUVChannelUsed == 0 && UIcifySettings::Get()->MaskUVChannel != 0))
		{
			return Fail(TEXT("texture or admitted material bindings could not be written"));
		}
	}
	else
	{
		UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component);
		if (!Static || !Icify::WriteVertexColors(Static, Result.LODColors, &Result.MaterialSurfaces)
			|| !Icify::BindOwnedMaterials(Component, nullptr, Result.MaterialSurfaces, DebugMaterial, &Palette))
		{
			return Fail(TEXT("vertex masks or admitted material bindings could not be written"));
		}
	}

	float DebugChannel = 0.f;
	switch (Display)
	{
	case EIcifyPreviewDisplay::Frost:    DebugChannel = 1.f; break;
	case EIcifyPreviewDisplay::Snow:     DebugChannel = 2.f; break;
	case EIcifyPreviewDisplay::Glaze:    DebugChannel = 3.f; break;
	case EIcifyPreviewDisplay::BlackIce: DebugChannel = 4.f; break;
	default: break;
	}

	float CustomData[Icify::CPD_Count];
	CustomData[Icify::CPD_MaskMode] = bTexture ? Icify::MaskModeTexture : Icify::MaskModeVertex;
	CustomData[Icify::CPD_SurfaceType] = float(int32(Result.DominantSurface) - 1);
	CustomData[Icify::CPD_FeatureScale] = FeatureScaleCm;
	// Scramble the seed into a stable 0..1000 offset so the shader detail changes with the seed too.
	CustomData[Icify::CPD_SeedOffset] = float((uint32(Seed) * 2654435761u) % 100000u) * 0.01f;
	CustomData[Icify::CPD_DebugChannel] = DebugChannel;
	if (!Icify::WriteCustomData(Component, CustomData))
	{
		return Fail(TEXT("custom primitive data arrays could not be written"));
	}

	Component->MarkRenderStateDirty();
	return true;
}

// ------------------------------------------------------------------- Report

void AIcifyFreezeZone::FinishReport(double StartSeconds)
{
	LastReport.Seconds = float(FPlatformTime::Seconds() - StartSeconds);

	int32 NumVertex = 0;
	int32 NumTexture = 0;
	int32 NumConflicts = 0;
	int32 NumFailed = 0;
	int32 NumCancelled = 0;
	for (const FIcifyComponentReport& Entry : LastReport.Components)
	{
		NumFailed += Entry.Result == TEXT("failed") ? 1 : 0;
		NumCancelled += Entry.Result == TEXT("cancelled_before_write") ? 1 : 0;
		NumVertex += Entry.Result == TEXT("vertex") ? 1 : 0;
		NumTexture += Entry.Result == TEXT("texture") ? 1 : 0;
		NumConflicts += Entry.bCustomDataConflict ? 1 : 0;
	}
	// Every component lands in exactly one count, taken from its final result (after writes, not before).
	LastReport.Frozen = NumVertex + NumTexture;
	LastReport.Failed = NumFailed;
	LastReport.Cancelled = NumCancelled;
	LastReport.Skipped = LastReport.Components.Num() - LastReport.Frozen - NumFailed - NumCancelled;
	if (NumConflicts > 0)
	{
		const int32 Base = UIcifySettings::Get()->CustomPrimitiveDataIndex;
		LastReport.Warnings.Add(FString::Printf(TEXT("%d mesh(es) already use custom primitive data %d-%d; they were left untouched. Change Project Settings > Plugins > Icify > Custom Primitive Data Index."),
			NumConflicts, Base, Base + Icify::CPD_Count - 1));
	}

	TArray<FString> PhaseText;
	for (const FIcifyPhaseTiming& Timing : LastReport.Phases)
	{
		PhaseText.Add(FString::Printf(TEXT("%s %.2fs"), *Timing.Phase, Timing.Seconds));
	}
	const FString Phases = PhaseText.Num() > 0 ? FString::Printf(TEXT(" [%s]"), *FString::Join(PhaseText, TEXT(", "))) : FString();

	if (LastReport.bCancelled)
	{
		// Cancellation only ever happens before the write phase, so no mesh received new masks.
		const TCHAR* Consequence = LastReport.Operation == TEXT("Apply")
			? TEXT("No meshes were frozen and the applied ice and icicles are unchanged; any active preview was removed.")
			: LastReport.Operation == TEXT("Preview")
				? TEXT("Nothing was written; the previous preview was removed.")
				: TEXT("Nothing was changed.");
		Status = FString::Printf(TEXT("%s cancelled during %s after %.2fs. %s%s"),
			*LastReport.Operation, *LastReport.CancelledDuring, LastReport.Seconds, Consequence, *Phases);
	}
	else
	{
		const TCHAR* Verb = LastReport.Operation == TEXT("Apply") ? TEXT("Applied")
			: LastReport.Operation == TEXT("Preview") ? TEXT("Previewing") : TEXT("Dry run");
		FString Extras;
		if (LastReport.CacheHits > 0)
		{
			Extras += FString::Printf(TEXT(", %d from cache"), LastReport.CacheHits);
		}
		FString Failures;
		if (LastReport.Failed > 0)
		{
			Failures += FString::Printf(TEXT(", %d FAILED"), LastReport.Failed);
		}
		if (LastReport.Cancelled > 0)
		{
			Failures += FString::Printf(TEXT(", %d cancelled before write"), LastReport.Cancelled);
		}
		Status = FString::Printf(TEXT("%s: %d mesh(es) (%d vertex, %d texture%s), %d icicle(s), %d skipped%s, %d melt / %d heat source(s), seed %d, %.2fs%s. See Last Report."),
			Verb, LastReport.Frozen, NumVertex, NumTexture, *Extras, LastReport.IcicleStats.Icicles, LastReport.Skipped, *Failures,
			LastReport.MeltSources, LastReport.HeatSources, Seed, LastReport.Seconds, *Phases);
	}
	UE_LOG(LogIcify, Log, TEXT("%s: %s"), *GetActorNameOrLabel(), *Status);
	for (const FString& Warning : LastReport.Warnings)
	{
		UE_LOG(LogIcify, Warning, TEXT("%s: %s"), *GetActorNameOrLabel(), *Warning);
	}

	if (bWriteReportFiles)
	{
		auto ReportFailure = [this](const FString& Message)
		{
			LastReport.Warnings.Add(Message);
			Status += TEXT(" Report file was not written; see Last Report.");
			UE_LOG(LogIcify, Warning, TEXT("%s: %s"), *GetActorNameOrLabel(), *Message);
		};
		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Icify"), TEXT("Reports"));
		if (!IFileManager::Get().MakeDirectory(*Directory, true))
		{
			ReportFailure(FString::Printf(TEXT("Report directory could not be created: %s"), *Directory));
			return;
		}
		FString Json;
		if (!FJsonObjectConverter::UStructToJsonObjectString(LastReport, Json))
		{
			ReportFailure(TEXT("Report could not be serialized to JSON."));
			return;
		}
		const FString File = FPaths::Combine(Directory,
			FPaths::MakeValidFileName(FString::Printf(TEXT("%s_%s.json"), *GetActorNameOrLabel(), *LastReport.Operation)));
		if (!FFileHelper::SaveStringToFile(Json, *File))
		{
			ReportFailure(FString::Printf(TEXT("Report could not be saved to %s"), *File));
		}
		else
		{
			LastReportFile = File;
		}
	}
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
