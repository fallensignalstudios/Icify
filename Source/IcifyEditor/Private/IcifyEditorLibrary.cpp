// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifyEditorLibrary.h"

#include "Editor.h"
#include "IcifyContentBuilder.h"
#include "IcifyFreezeZone.h"
#include "IcifyLibraryBuilder.h"
#include "IcifyPreset.h"
#include "IcifyQuickStart.h"

bool UIcifyEditorLibrary::BuildPluginContent(bool bForceRebuild, FString& OutReport)
{
	const FIcifyContentReport Report = FIcifyContentBuilder::Run(bForceRebuild);
	OutReport = Report.ToString();
	return !Report.HasErrors();
}

bool UIcifyEditorLibrary::BuildPresetLibrary(bool bOverwrite, FString& OutMessage)
{
	return FIcifyLibraryBuilder::Build(bOverwrite, OutMessage);
}

TArray<FString> UIcifyEditorLibrary::GetLookNames()
{
	return FIcifyLibraryBuilder::LookNames();
}

bool UIcifyEditorLibrary::ConfigurePresetLook(UIcifyPreset* Preset, const FString& LookName)
{
	if (!Preset)
	{
		return false;
	}
	Preset->Modify();
	if (!FIcifyLibraryBuilder::ConfigureLook(LookName, *Preset))
	{
		return false;
	}
	Preset->MarkPackageDirty();
	return true;
}

AIcifyFreezeZone* UIcifyEditorLibrary::FreezeActors(const TArray<AActor*>& Actors, UIcifyPreset* Preset, bool bPreview, FString& OutMessage)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	return FIcifyQuickStart::FreezeActors(World, Actors, Preset, bPreview, OutMessage);
}
