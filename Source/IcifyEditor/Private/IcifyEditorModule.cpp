// Copyright Fallen Signal Studios. All Rights Reserved.

#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IcifyContentBuilder.h"
#include "IcifyFreezeZone.h"
#include "IcifyLibraryBuilder.h"
#include "IcifyMaterialHookup.h"
#include "IcifyQuickStart.h"
#include "IcifySettings.h"
#include "IcifyZoneComponent.h"
#include "IcifyZoneVisualizer.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "ToolMenus.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UnrealEdGlobals.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "IcifyEditor"

namespace
{
	enum class EZoneAction : uint8
	{
		Preview,
		Apply,
		Clear,
		CancelPreview,
		DryRun
	};

	void Notify(const FText& Message, bool bError = false)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = bError ? 8.f : 4.f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	void RunOnAllZones(EZoneAction Action)
	{
		UWorld* World = EditorWorld();
		if (!World)
		{
			return;
		}

		TArray<AIcifyFreezeZone*> Zones;
		for (TActorIterator<AIcifyFreezeZone> It(World); It; ++It)
		{
			Zones.Add(*It);
		}
		if (Zones.Num() == 0)
		{
			Notify(LOCTEXT("NoZones", "No Icify freeze zones in this level."));
			return;
		}

		const FScopedTransaction Transaction(LOCTEXT("AllZones", "Icify: All Freeze Zones"));
		int32 Icicles = 0;
		for (AIcifyFreezeZone* Zone : Zones)
		{
			switch (Action)
			{
			case EZoneAction::Preview: Zone->Preview(); break;
			case EZoneAction::Apply: Zone->Apply(); break;
			case EZoneAction::Clear: Zone->Clear(); break;
			case EZoneAction::CancelPreview: Zone->CancelPreview(); break;
			case EZoneAction::DryRun: Zone->DryRun(); break;
			}
			Icicles += Zone->GetLastReport().IcicleStats.Icicles;
		}
		const bool bReports = Action == EZoneAction::Preview || Action == EZoneAction::Apply || Action == EZoneAction::DryRun;
		Notify(bReports
			? FText::Format(LOCTEXT("ZonesDone", "Icify: processed {0} zone(s), {1} icicle(s). Reports are in Saved/Icify/Reports."), Zones.Num(), Icicles)
			: FText::Format(LOCTEXT("ZonesDoneNoReport", "Icify: processed {0} zone(s)."), Zones.Num()));
	}

	void RunContentBuild(bool bForce)
	{
		const FIcifyContentReport Report = FIcifyContentBuilder::Run(bForce);
		if (Report.HasErrors())
		{
			Notify(FText::Format(LOCTEXT("ContentErrors", "Icify content build had {0} error(s). See the Output Log or Saved/Icify/ContentReport.txt."),
				Report.Errors.Num() + (Report.bSaved ? 0 : 1)), true);
		}
		else if (Report.Created.Num() + Report.Rebuilt.Num() > 0)
		{
			Notify(FText::Format(LOCTEXT("ContentBuilt", "Icify: created {0}, rebuilt {1} asset(s); connections, compile and save OK."), Report.Created.Num(), Report.Rebuilt.Num()));
		}
		else
		{
			Notify(LOCTEXT("ContentCurrent", "Icify: plugin content is up to date."));
		}
	}

	void BuildLibrary(bool bOverwrite)
	{
		FString Message;
		const bool bOk = FIcifyLibraryBuilder::Build(bOverwrite, Message);
		UE_LOG(LogIcify, Log, TEXT("%s"), *Message);
		Notify(FText::FromString(TEXT("Icify: ") + Message), !bOk);
	}
}

class FIcifyEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (GUnrealEd)
		{
			const TSharedPtr<FComponentVisualizer> Visualizer = MakeShared<FIcifyZoneVisualizer>();
			GUnrealEd->RegisterComponentVisualizer(UIcifyZoneComponent::StaticClass()->GetFName(), Visualizer);
			Visualizer->OnRegister();
		}

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FIcifyEditorModule::RegisterMenus));

		// Create missing assets and migrate outdated ones once the editor is fully up.
		EngineInitHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([this]()
		{
			BindExistingEditorLevels();
			if (UIcifySettings::Get()->bAutoBuildContent && FIcifyContentBuilder::NeedsWork())
			{
				RunContentBuild(/*bForce*/ false);
			}
		});

		// Previews must never be saved: drop them before any package save.
		PreSaveHandle = UPackage::PreSavePackageWithContextEvent.AddRaw(this, &FIcifyEditorModule::OnPreSavePackage);

		// World Partition unloads actors without destroying them; drop a zone's preview when it goes.
		WorldInitializedHandle = FWorldDelegates::OnPostWorldInitialization.AddRaw(this, &FIcifyEditorModule::OnWorldInitialized);
		LevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddRaw(this, &FIcifyEditorModule::OnLevelAdded);
		LevelRemovedHandle = FWorldDelegates::PreLevelRemovedFromWorld.AddRaw(this, &FIcifyEditorModule::OnLevelRemoving);
		WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(this, &FIcifyEditorModule::OnWorldCleanup);
		BindExistingEditorLevels();

		SettingsHandle = GetMutableDefault<UIcifySettings>()->OnSettingChanged().AddRaw(this, &FIcifyEditorModule::OnSettingChanged);
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnFEngineLoopInitComplete.Remove(EngineInitHandle);
		UPackage::PreSavePackageWithContextEvent.Remove(PreSaveHandle);
		FWorldDelegates::OnPostWorldInitialization.Remove(WorldInitializedHandle);
		FWorldDelegates::LevelAddedToWorld.Remove(LevelAddedHandle);
		FWorldDelegates::PreLevelRemovedFromWorld.Remove(LevelRemovedHandle);
		FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
		for (const auto& Binding : ActorRemovedHandles)
		{
			if (ULevel* Level = Binding.Key.Get())
			{
				Level->OnLoadedActorRemovedFromLevelEvent.Remove(Binding.Value);
			}
		}
		ActorRemovedHandles.Empty();

		if (UObjectInitialized())
		{
			if (UIcifySettings* Settings = GetMutableDefault<UIcifySettings>())
			{
				Settings->OnSettingChanged().Remove(SettingsHandle);
			}
			UToolMenus::UnRegisterStartupCallback(this);
			UToolMenus::UnregisterOwner(this);
		}

		if (GUnrealEd)
		{
			GUnrealEd->UnregisterComponentVisualizer(UIcifyZoneComponent::StaticClass()->GetFName());
		}
	}

private:
	FDelegateHandle EngineInitHandle;
	FDelegateHandle PreSaveHandle;
	FDelegateHandle WorldInitializedHandle;
	FDelegateHandle LevelAddedHandle;
	FDelegateHandle LevelRemovedHandle;
	FDelegateHandle WorldCleanupHandle;
	TMap<TWeakObjectPtr<ULevel>, FDelegateHandle> ActorRemovedHandles;
	FDelegateHandle SettingsHandle;
	bool bDiscarding = false;

	void BindLevel(ULevel* Level, UWorld* World)
	{
		if (!Level || !World || World->WorldType != EWorldType::Editor || ActorRemovedHandles.Contains(Level))
		{
			return;
		}
		ActorRemovedHandles.Add(Level, Level->OnLoadedActorRemovedFromLevelEvent.AddRaw(this, &FIcifyEditorModule::OnLoadedActorRemoved));
	}

	void BindWorldLevels(UWorld* World)
	{
		if (World && World->WorldType == EWorldType::Editor)
		{
			for (ULevel* Level : World->GetLevels())
			{
				BindLevel(Level, World);
			}
		}
	}

	void BindExistingEditorLevels()
	{
		for (TObjectIterator<UWorld> It; It; ++It)
		{
			BindWorldLevels(*It);
		}
	}

	void OnWorldInitialized(UWorld* World, const UWorld::InitializationValues InitializationValues)
	{
		BindWorldLevels(World);
	}

	void OnLevelAdded(ULevel* Level, UWorld* World)
	{
		BindLevel(Level, World);
	}

	void UnbindLevel(ULevel* Level)
	{
		if (FDelegateHandle* Handle = ActorRemovedHandles.Find(Level))
		{
			Level->OnLoadedActorRemovedFromLevelEvent.Remove(*Handle);
			ActorRemovedHandles.Remove(Level);
		}
	}

	void OnLevelRemoving(ULevel* Level, UWorld* World)
	{
		if (!World || World->WorldType != EWorldType::Editor)
		{
			return;
		}
		if (!Level)
		{
			OnWorldCleanup(World, false, false);
			return;
		}
		for (AActor* Actor : Level->Actors)
		{
			if (Actor)
			{
				OnLoadedActorRemoved(*Actor);
			}
		}
		UnbindLevel(Level);
	}

	void OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
	{
		if (!World || World->WorldType != EWorldType::Editor)
		{
			return;
		}
		for (TActorIterator<AIcifyFreezeZone> It(World); It; ++It)
		{
			OnLoadedActorRemoved(**It);
		}
		for (auto It = ActorRemovedHandles.CreateIterator(); It; ++It)
		{
			ULevel* Level = It.Key().Get();
			if (!Level || Level->GetWorld() == World)
			{
				if (Level)
				{
					Level->OnLoadedActorRemovedFromLevelEvent.Remove(It.Value());
				}
				It.RemoveCurrent();
			}
		}
	}

	void OnPreSavePackage(UPackage* Package, FObjectPreSaveContext Context)
	{
		// Autosaves are recovery copies; keep the preview rather than dropping it every few minutes.
		if (bDiscarding || Context.IsProceduralSave() || Context.IsCooking() || (Context.GetSaveFlags() & SAVE_FromAutosave) != 0)
		{
			return;
		}

		UWorld* World = EditorWorld();
		if (!World)
		{
			return;
		}

		TGuardValue<bool> Guard(bDiscarding, true);
		int32 NumDiscarded = 0;
		int32 NumRetained = 0;
		for (TActorIterator<AIcifyFreezeZone> It(World); It; ++It)
		{
			if (It->bPreviewActive)
			{
				It->DiscardPreview(TEXT("level saved"));
				if (It->bPreviewActive)
				{
					++NumRetained;
				}
				else
				{
					++NumDiscarded;
				}
			}
		}
		if (NumDiscarded > 0)
		{
			Notify(FText::Format(LOCTEXT("PreviewDiscarded", "Icify: discarded {0} preview(s) before saving. Apply to keep the ice, or Preview again."), NumDiscarded));
		}
		if (NumRetained > 0)
		{
			Notify(FText::Format(LOCTEXT("PreviewRestoreFailed", "Icify: could not restore {0} preview(s). Their original backups were retained; see the Output Log and repair the changed meshes before trying again."), NumRetained), true);
		}
	}

	void OnLoadedActorRemoved(AActor& Actor)
	{
		if (AIcifyFreezeZone* Zone = Cast<AIcifyFreezeZone>(&Actor))
		{
			if (Zone->bPreviewActive && !GIsTransacting)
			{
				Zone->DiscardPreview(TEXT("zone unloaded"));
			}
		}
	}

	void OnSettingChanged(UObject* Settings, FPropertyChangedEvent& Event)
	{
		const FName Name = Event.GetPropertyName();
		if (Name == GET_MEMBER_NAME_CHECKED(UIcifySettings, CustomPrimitiveDataIndex) || Name == GET_MEMBER_NAME_CHECKED(UIcifySettings, MaskUVChannel))
		{
			RunContentBuild(/*bForce*/ false);
			Notify(LOCTEXT("ReapplyZones", "Icify: material content updated. Clear and re-apply existing freeze zones so meshes use the new settings."));
		}
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);

		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->FindOrAddSection("Icify", LOCTEXT("Section", "Icify"));

		Section.AddMenuEntry("IcifyFreezeSelected",
			LOCTEXT("FreezeSelected", "Freeze Selected Actors"),
			LOCTEXT("FreezeSelectedTip", "Place a freeze zone fitted to the selected actors (and only them), offer to hook up their materials, and preview the ice and icicles. One undo step."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&FIcifyQuickStart::FreezeSelected)));

		Section.AddMenuEntry("IcifyHookUpLevel",
			LOCTEXT("HookUpLevel", "Hook Up Materials In Level"),
			LOCTEXT("HookUpLevelTip", "Find the master materials used inside freeze zones (or the whole level when there are none) that don't use Icify yet, and wire Icify into them. Each is backed up first."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&UIcifyMaterialHookup::HookUpLevel)));

		Section.AddMenuEntry("IcifyHookUpSelected",
			LOCTEXT("HookUpSelected", "Add Icify to Selected Materials"),
			LOCTEXT("HookUpSelectedTip", "Wire Icify into the materials (or the parents of the material instances) selected in the Content Browser."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&UIcifyMaterialHookup::AddToSelectedAssets)));

		Section.AddMenuEntry("IcifyUnhookSelected",
			LOCTEXT("UnhookSelected", "Remove Icify from Selected Materials"),
			LOCTEXT("UnhookSelectedTip", "Put back the original wiring of materials Icify was added to by Add Icify or Hook Up Materials."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&UIcifyMaterialHookup::RemoveFromSelectedAssets)));

		// Right-click on actors in the viewport or World Outliner.
		{
			UToolMenu* ActorMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.ActorContextMenu");
			FToolMenuSection& ActorSection = ActorMenu->FindOrAddSection("Icify", LOCTEXT("ActorSection", "Icify"));
			ActorSection.AddMenuEntry("IcifyFreezeSelectedContext",
				LOCTEXT("FreezeSelectedContext", "Freeze with Icify"),
				LOCTEXT("FreezeSelectedContextTip", "Place a freeze zone fitted to the selected actors, offer to hook up their materials, and preview."),
				FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&FIcifyQuickStart::FreezeSelected)));
		}

		// Right-click on materials and material instances in the Content Browser.
		for (const TCHAR* MenuName : { TEXT("ContentBrowser.AssetContextMenu.Material"), TEXT("ContentBrowser.AssetContextMenu.MaterialInstanceConstant") })
		{
			UToolMenu* AssetMenu = UToolMenus::Get()->ExtendMenu(MenuName);
			FToolMenuSection& AssetSection = AssetMenu->FindOrAddSection("Icify", LOCTEXT("AssetSection", "Icify"));
			AssetSection.AddMenuEntry("IcifyAddToMaterial",
				LOCTEXT("AddToMaterial", "Add Icify"),
				LOCTEXT("AddToMaterialTip", "Wire Icify ice into this material (for an instance, its parent). Legacy and Material Attributes materials are supported. A backup goes to /Game/Icify/Backups."),
				FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&UIcifyMaterialHookup::AddToSelectedAssets)));
			AssetSection.AddMenuEntry("IcifyRemoveFromMaterial",
				LOCTEXT("RemoveFromMaterial", "Remove Icify"),
				LOCTEXT("RemoveFromMaterialTip", "Put back the wiring this material had before Add Icify."),
				FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&UIcifyMaterialHookup::RemoveFromSelectedAssets)));
		}

		Section.AddMenuEntry("IcifyDryRunAll",
			LOCTEXT("DryRunAll", "Dry Run All Freeze Zones"),
			LOCTEXT("DryRunAllTip", "Report what every zone would do (masks and icicles), without touching any mesh."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&RunOnAllZones, EZoneAction::DryRun)));

		Section.AddMenuEntry("IcifyPreviewAll",
			LOCTEXT("PreviewAll", "Preview All Freeze Zones"),
			LOCTEXT("PreviewAllTip", "Preview every freeze zone in the level."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&RunOnAllZones, EZoneAction::Preview)));

		Section.AddMenuEntry("IcifyApplyAll",
			LOCTEXT("ApplyAll", "Apply All Freeze Zones"),
			LOCTEXT("ApplyAllTip", "Apply every freeze zone in the level (one undo step)."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&RunOnAllZones, EZoneAction::Apply)));

		Section.AddMenuEntry("IcifyCancelAll",
			LOCTEXT("CancelAll", "Cancel All Previews"),
			LOCTEXT("CancelAllTip", "Drop every zone's preview and go back to the applied state."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&RunOnAllZones, EZoneAction::CancelPreview)));

		Section.AddMenuEntry("IcifyClearAll",
			LOCTEXT("ClearAll", "Clear All Freeze Zones"),
			LOCTEXT("ClearAllTip", "Remove the ice and icicles of every zone in the level (one undo step)."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&RunOnAllZones, EZoneAction::Clear)));

		Section.AddMenuEntry("IcifyUpdateContent",
			LOCTEXT("UpdateContent", "Update Plugin Content"),
			LOCTEXT("UpdateContentTip", "Create missing Icify assets and rebuild any generated by an older version or other settings. Validates connections, compile and save."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&RunContentBuild, false)));

		Section.AddMenuEntry("IcifyRebuildContent",
			LOCTEXT("RebuildContent", "Rebuild Plugin Content"),
			LOCTEXT("RebuildContentTip", "Rebuild MF_IcifyFrost, M_IcifyIcicle and the other Icify materials in place, even if they look current."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&RunContentBuild, true)));

		Section.AddMenuEntry("IcifyBuildLibrary",
			LOCTEXT("BuildLibrary", "Create Preset Library"),
			LOCTEXT("BuildLibraryTip", "Create starter presets (Light Frost, Deep Winter, Ice Storm, Blizzard, Thaw) and stone, metal and glass material instances in /Game/Icify/Library. Existing assets are kept."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&BuildLibrary, false)));

		Section.AddMenuEntry("IcifyRebuildLibrary",
			LOCTEXT("RebuildLibrary", "Reset Preset Library"),
			LOCTEXT("RebuildLibraryTip", "Overwrite the starter presets and material instances in /Game/Icify/Library with their defaults."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&BuildLibrary, true)));
	}
};

IMPLEMENT_MODULE(FIcifyEditorModule, IcifyEditor)

#undef LOCTEXT_NAMESPACE
