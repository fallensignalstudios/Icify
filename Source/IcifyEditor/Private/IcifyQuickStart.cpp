// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifyQuickStart.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IcifyFreezeZone.h"
#include "IcifyMaterialHookup.h"
#include "IcifyPreset.h"
#include "IcifySettings.h"
#include "IcifyZoneComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "IcifyQuickStart"

namespace
{
	const TCHAR* ZoneBlueprintObjectPath = TEXT("/Icify/BP_FreezeZone.BP_FreezeZone");
	/** Past the zone's default 50 cm falloff, so the selection itself is fully frozen. */
	constexpr double MinPaddingCm = 60.;
	constexpr double PaddingFraction = 0.1;
	/** Extra reach below the selection, so Trace Floors finds the floor it stands on. */
	constexpr double FloorReachCm = 100.;
	/** Where Freeze Selected Actors puts private material copies. */
	const TCHAR* PrivateFolder = TEXT("/Game/Icify/Private");

	bool HasFreezableMesh(const AActor* Actor)
	{
		TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
		return Components.ContainsByPredicate([](const UStaticMeshComponent* Component)
		{
			return Component && Component->IsRegistered() && Component->GetStaticMesh();
		});
	}

	UClass* ZoneClass()
	{
		// The plugin's Blueprint when it exists (it is what users place by hand), the native class otherwise.
		if (const UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, ZoneBlueprintObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			if (Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(AIcifyFreezeZone::StaticClass()))
			{
				return Blueprint->GeneratedClass;
			}
		}
		return AIcifyFreezeZone::StaticClass();
	}

	void NotifyQuickStart(const FText& Message, bool bError)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = bError ? 8.f : 5.f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	/** A unique package for a private copy of Source under Folder: <Folder>/<Source>_Private, _Private_2, ... */
	FString UniquePrivatePackage(const FString& Folder, const UObject* Source)
	{
		const FString Base = FString::Printf(TEXT("%s/%s_Private"), *Folder, *Source->GetName());
		FString Candidate = Base;
		for (int32 Index = 2; FPackageName::DoesPackageExist(Candidate) || FindPackage(nullptr, *Candidate); ++Index)
		{
			Candidate = FString::Printf(TEXT("%s_%d"), *Base, Index);
		}
		return Candidate;
	}

	template <typename T>
	T* DuplicateAsPrivate(T* Source, const FString& Folder, TArray<FString>& Created)
	{
		const FString PackageName = UniquePrivatePackage(Folder, Source);
		UPackage* Package = CreatePackage(*PackageName);
		T* Copy = DuplicateObject<T>(Source, Package, *FPackageName::GetShortName(PackageName));
		if (!Copy)
		{
			return nullptr;
		}
		Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		Copy->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Copy);
		Created.Add(Copy->GetPathName());
		return Copy;
	}

	/**
	 * Clones each slot's master material (and its material-instance chain) under Folder, hooks Icify into the clone,
	 * and assigns the clone to only those slots of Component. Shared assets are never modified.
	 */
	void ClonePrivateMaterials(UMeshComponent* Component, const TArray<int32>& Slots, const FString& Folder,
		TMap<UMaterialInterface*, UMaterialInterface*>& CloneOf, TArray<FString>& Created, TArray<FString>& Notes)
	{
		for (int32 Slot : Slots)
		{
			UMaterialInterface* Leaf = Component->GetMaterial(Slot);
			if (!Leaf)
			{
				continue;
			}
			UMaterialInterface** Existing = CloneOf.Find(Leaf);
			UMaterialInterface* Clone = Existing ? *Existing : nullptr;
			if (!Existing)
			{
				// Leaf -> ... -> base material. Only constant instances can be cloned; a dynamic one belongs to gameplay.
				TArray<UMaterialInstanceConstant*> Chain;
				UMaterialInterface* It = Leaf;
				bool bClonable = true;
				while (UMaterialInstance* Instance = Cast<UMaterialInstance>(It))
				{
					UMaterialInstanceConstant* Constant = Cast<UMaterialInstanceConstant>(Instance);
					if (!Constant)
					{
						bClonable = false;
						break;
					}
					Chain.Add(Constant);
					It = Instance->Parent;
				}
				UMaterial* Base = Cast<UMaterial>(It);
				if (!bClonable || !Base)
				{
					Notes.Add(FString::Printf(TEXT("%s isn't a material or a chain of material instance constants; it can't be cloned privately and is left alone."),
						*Leaf->GetPathName()));
					CloneOf.Add(Leaf, nullptr);
					continue;
				}

				UMaterial* PrivateBase = DuplicateAsPrivate(Base, Folder, Created);
				if (!PrivateBase)
				{
					Notes.Add(FString::Printf(TEXT("Could not duplicate %s."), *Base->GetPathName()));
					CloneOf.Add(Leaf, nullptr);
					continue;
				}
				// The clone is ours alone, so no backup: the original master is the backup.
				const FIcifyHookupResult Hookup = UIcifyMaterialHookup::AddIcifyToMaterial(PrivateBase, /*bBackup*/ false);
				Notes.Add(FString::Printf(TEXT("Private master %s: hookup %s%s"), *PrivateBase->GetPathName(), *Hookup.Outcome,
					Hookup.Notes.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Hookup.Notes)));
				if (Hookup.Outcome != TEXT("added") && Hookup.Outcome != TEXT("already"))
				{
					CloneOf.Add(Leaf, nullptr);
					continue;
				}

				// Re-create the instance chain on top of the private master, base first, keeping every parameter.
				UMaterialInterface* Parent = PrivateBase;
				for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
				{
					UMaterialInstanceConstant* Copy = DuplicateAsPrivate(Chain[Index], Folder, Created);
					if (!Copy)
					{
						Parent = nullptr;
						break;
					}
					Copy->SetParentEditorOnly(Parent);
					Copy->PostEditChange();
					Parent = Copy;
				}
				Clone = Parent;
				CloneOf.Add(Leaf, Clone);
			}
			if (Clone)
			{
				Component->Modify();
				Component->SetMaterial(Slot, Clone);
			}
		}
	}
}

FBox FIcifyQuickStart::ZoneBoxFor(const FBox& Bounds)
{
	const FVector Padding = FVector::Max(Bounds.GetExtent() * PaddingFraction, FVector(MinPaddingCm));
	FBox Box(Bounds.Min - Padding, Bounds.Max + Padding);
	Box.Min.Z -= FloorReachCm;
	return Box;
}

AIcifyFreezeZone* FIcifyQuickStart::FreezeActors(UWorld* World, const TArray<AActor*>& Actors, UIcifyPreset* Preset, bool bPreview, FString& OutMessage)
{
	TArray<AActor*> Targets;
	FBox Bounds(ForceInit);
	for (AActor* Actor : Actors)
	{
		if (IsValid(Actor) && !Actor->IsA<AIcifyFreezeZone>() && Actor->GetWorld() == World && HasFreezableMesh(Actor))
		{
			Targets.AddUnique(Actor);
			Bounds += Actor->GetComponentsBoundingBox(/*bNonColliding*/ true);
		}
	}
	if (!World || Targets.Num() == 0 || !Bounds.IsValid)
	{
		OutMessage = TEXT("Select one or more actors with static meshes first.");
		return nullptr;
	}

	const FScopedTransaction Transaction(LOCTEXT("FreezeSelected", "Icify: Freeze Selected Actors"));
	const FBox Box = ZoneBoxFor(Bounds);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AIcifyFreezeZone* Zone = World->SpawnActor<AIcifyFreezeZone>(ZoneClass(), Box.GetCenter(), FRotator::ZeroRotator, Params);
	if (!Zone || !Zone->GetZoneComponent())
	{
		OutMessage = TEXT("Could not spawn a freeze zone.");
		return nullptr;
	}

	{
		// SetActorLabel and LoadPreset send property changes: never generate before the selection filter is set.
		// The live-preview flag comes back on return, so later artist edits behave as usual.
		TGuardValue<bool> LivePreviewGuard(Zone->bLivePreview, false);

		const FString First = Targets[0]->GetActorLabel();
		Zone->SetActorLabel(Targets.Num() == 1 ? FString::Printf(TEXT("Icify_%s"), *First)
			: FString::Printf(TEXT("Icify_%s_and_%d_more"), *First, Targets.Num() - 1));
		Zone->GetZoneComponent()->SetBoxExtent(Box.GetExtent());
		if (Preset)
		{
			Zone->Preset = Preset;
			Zone->LoadPreset();
		}
		// Only the selection: neighbours that happen to overlap the padded box are left alone.
		for (AActor* Target : Targets)
		{
			Zone->IncludeOnlyActors.Add(Target);
		}
		if (bPreview)
		{
			Zone->Preview();
		}
	}
	OutMessage = FString::Printf(TEXT("%s %d actor(s) with %s. Adjust it in the Details panel, then Apply to keep it."),
		bPreview ? TEXT("Previewing") : TEXT("Created a freeze zone for"), Targets.Num(), *Zone->GetActorLabel());
	return Zone;
}

void FIcifyQuickStart::FreezeSelected()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TArray<AActor*> Actors;
	if (GEditor)
	{
		for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
		{
			if (AActor* Actor = Cast<AActor>(*It))
			{
				Actors.Add(Actor);
			}
		}
	}

	// One undo step for everything below that touches the level (private slot assignments, zone, masks, icicles).
	const FScopedTransaction Transaction(LOCTEXT("FreezeSelectedAll", "Icify: Freeze Selected Actors"));

	// Masks only show through materials that call Icify, so offer to hook up what the selection uses. Engine materials (the
	// basic shapes' material, for one) can't be changed in place, so they always get private copies.
	TArray<FString> Created;
	TArray<FString> Notes;
	const TArray<UMaterial*> Unhooked = UIcifyMaterialHookup::FindUnhookedMaterialsOn(Actors);
	const TArray<UMaterial*> CopyOnly = UIcifyMaterialHookup::FindCopyOnlyMaterialsOn(Actors);
	if (Unhooked.Num() > 0 || CopyOnly.Num() > 0)
	{
		const int32 Total = Unhooked.Num() + CopyOnly.Num();
		TArray<FString> Names;
		for (const UMaterial* Material : Unhooked)
		{
			if (Names.Num() < 10)
			{
				Names.Add(TEXT("  ") + Material->GetPathName());
			}
		}
		for (const UMaterial* Material : CopyOnly)
		{
			if (Names.Num() < 10)
			{
				Names.Add(TEXT("  ") + Material->GetPathName() + TEXT("  (engine content: private copy only)"));
			}
		}
		if (Total > 10)
		{
			Names.Add(FString::Printf(TEXT("  ...and %d more"), Total - 10));
		}
		const FText List = FText::FromString(FString::Join(Names, TEXT("\n")));

		bool bPrivate = true;
		if (Unhooked.Num() > 0)
		{
			const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNoCancel, FText::Format(LOCTEXT("ConfirmHookupPrivate",
				"The selection uses {0} master material(s) that don't show Icify ice yet:\n\n{1}\n\n"
				"Yes (recommended): make private copies for the selected actors only, in {2}. Shared masters and every other "
				"actor are left unchanged; Undo puts the selection's original materials back (the copies stay on disk to delete or keep).\n\n"
				"No: hook up the shared masters themselves. Each is backed up first, but every instance of them in the project is affected. "
				"Engine materials can't be changed, so they get private copies either way.\n\n"
				"Cancel: do nothing.\n\nIcicles grow either way: they have their own material."),
				Total, List, FText::FromString(PrivateFolder)));
			if (Answer == EAppReturnType::Cancel)
			{
				return;
			}
			bPrivate = Answer == EAppReturnType::Yes;
		}
		else if (FMessageDialog::Open(EAppMsgType::OkCancel, FText::Format(LOCTEXT("ConfirmPrivateCopies",
			"The selection uses {0} engine material(s) that can't show Icify ice:\n\n{1}\n\n"
			"OK: make private copies for the selected actors only, in {2}. The engine materials and every other actor are left "
			"unchanged; Undo puts the selection's original materials back (the copies stay on disk to delete or keep).\n\n"
			"Cancel: do nothing.\n\nIcicles grow either way: they have their own material."),
			Total, List, FText::FromString(PrivateFolder))) == EAppReturnType::Cancel)
		{
			return;
		}

		if (!bPrivate)
		{
			UIcifyMaterialHookup::HookUpMaterials(Unhooked);
		}
		// Private copies of everything (Yes), or only of the engine materials (No).
		TArray<UMaterial*> ToCopy = CopyOnly;
		if (bPrivate)
		{
			ToCopy.Append(Unhooked);
		}
		if (ToCopy.Num() > 0)
		{
			TMap<UMaterialInterface*, UMaterialInterface*> CloneOf;
			for (AActor* Actor : Actors)
			{
				TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
				for (UStaticMeshComponent* Component : Components)
				{
					if (!Component || !Component->IsRegistered())
					{
						continue;
					}
					TArray<int32> Slots;
					for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
					{
						const UMaterialInterface* Material = Component->GetMaterial(Slot);
						if (Material && ToCopy.Contains(Material->GetMaterial()))
						{
							Slots.Add(Slot);
						}
					}
					ClonePrivateMaterials(Component, Slots, PrivateFolder, CloneOf, Created, Notes);
				}
			}
		}
	}

	FString Message;
	AIcifyFreezeZone* Zone = FreezeActors(World, Actors, /*Preset*/ nullptr, /*bPreview*/ true, Message);
	if (Zone)
	{
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
		GEditor->SelectActor(Zone, /*bInSelected*/ true, /*bNotify*/ true);
	}
	for (const FString& Note : Notes)
	{
		UE_LOG(LogIcify, Log, TEXT("Freeze Selected Actors: %s"), *Note);
	}
	if (Created.Num() > 0)
	{
		Message += FString::Printf(TEXT(" Created %d private material asset(s) in %s; save or delete them."), Created.Num(), PrivateFolder);
	}
	NotifyQuickStart(FText::FromString(TEXT("Icify: ") + Message), Zone == nullptr);
}

#undef LOCTEXT_NAMESPACE
