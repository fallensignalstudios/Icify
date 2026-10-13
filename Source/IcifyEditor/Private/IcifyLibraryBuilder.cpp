// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifyLibraryBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "FileHelpers.h"
#include "IcifyPreset.h"
#include "IcifyTypes.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
	const TCHAR* LibraryFolder = TEXT("/Game/Icify/Library");
	const TCHAR* ExampleMaterialObjectPath = TEXT("/Icify/Materials/M_IcifyExample.M_IcifyExample");

	/** One tuned look: notes and the settings that differ from the zone defaults. */
	struct FLook
	{
		const TCHAR* Name;
		const TCHAR* Notes;
		TFunction<void(UIcifyPreset&)> Configure;
	};

	template <typename T>
	T* FindOrCreate(const FString& Name, bool bOverwrite, bool& bOutCreated)
	{
		const FString PackageName = FString(LibraryFolder) / Name;
		const FString ObjectPath = PackageName + TEXT(".") + Name;
		bOutCreated = false;
		if (T* Existing = LoadObject<T>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return bOverwrite ? Existing : nullptr;
		}
		UPackage* Package = CreatePackage(*PackageName);
		T* Asset = NewObject<T>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
		bOutCreated = true;
		return Asset;
	}

	TArray<FLook> Looks()
	{
		TArray<FLook> Result;

		Result.Add({ TEXT("LightFrost"),
			TEXT("A clear, still, freezing morning: hoarfrost on every exposed edge and face, frost-ferns on glass, a dusting of snow and ")
			TEXT("patches of black ice on the ground. No icicles yet."),
			[](UIcifyPreset& P)
			{
				P.Intensity = 0.9f;
				P.WindInfluence = 0.15f;
				P.SnowfallSlantDegrees = 5.f;
				P.FrostIntensity = 1.3f;
				P.FrostFilm = 0.55f;
				P.EdgeFrost = 1.f;
				P.Rime = 0.2f;
				P.GroundFrost = 0.6f;
				P.SnowIntensity = 0.25f;
				P.CreviceSnow = 0.3f;
				P.GlazeIntensity = 0.3f;
				P.GlazeCoat = 0.08f;
				P.AmbientDrips = 0.f;
				P.BlackIceIntensity = 0.8f;
				P.FloorIce = 0.45f;
				P.bGrowIcicles = false;
				P.Palette.bOverrideSparkle = true;
				P.Palette.Sparkle = 0.8f;
				P.Palette.bOverrideFernFrost = true;
				P.Palette.FernFrost = 1.f;
			} });

		Result.Add({ TEXT("DeepWinter"),
			TEXT("Weeks of snow: deep cover on every ledge and roof, drifts in the corners, long clumped icicles along the eaves, ")
			TEXT("frozen meltwater on the walls below and black ice where it pooled."),
			[](UIcifyPreset& P)
			{
				P.Intensity = 1.1f;
				P.FrostIntensity = 0.8f;
				P.SnowIntensity = 1.4f;
				P.SnowMaxSlopeDegrees = 55.f;
				P.CreviceSnow = 0.9f;
				P.WallPlaster = 0.2f;
				P.EdgeSnowLoss = 0.15f;
				P.GlazeIntensity = 0.7f;
				P.AmbientDrips = 0.4f;
				P.BlackIceIntensity = 0.9f;
				P.FloorIce = 0.3f;
				P.IcicleDensity = 10.f;
				P.IcicleMaxLengthCm = 70.f;
				P.IcicleThickness = 1.15f;
				P.IcicleClumping = 0.75f;
				P.Palette.bOverrideSnowBreakup = true;
				P.Palette.SnowBreakup = 0.25f;
			} });

		Result.Add({ TEXT("IceStorm"),
			TEXT("Freezing rain: a thick clear glaze on everything the rain hit, dense short icicles on every drip edge whether or not ")
			TEXT("snow lay above it, frozen drips down the walls and very little snow."),
			[](UIcifyPreset& P)
			{
				P.Intensity = 1.f;
				P.WindInfluence = 0.6f;
				P.SnowfallSlantDegrees = 25.f;
				P.FrostIntensity = 0.5f;
				P.SnowIntensity = 0.3f;
				P.GlazeIntensity = 1.6f;
				P.GlazeCoat = 0.8f;
				P.AmbientDrips = 0.7f;
				P.BlackIceIntensity = 1.2f;
				P.FloorIce = 0.6f;
				P.bGrowIcicles = true;
				P.IcicleDensity = 14.f;
				P.IcicleMaxLengthCm = 35.f;
				P.IcicleThickness = 0.85f;
				P.IcicleRippleCm = 0.8f;
				P.IcicleClumping = 0.35f;
				P.IcicleLean = 0.35f;
				P.DripOverhangDegrees = 10.f;
				P.bIciclesNeedMeltwater = false;
				P.Palette.bOverrideGlazeStrength = true;
				P.Palette.GlazeStrength = 1.2f;
				P.Palette.bOverrideIceTint = true;
				P.Palette.IceTint = FLinearColor(0.5f, 0.74f, 0.95f);
			} });

		Result.Add({ TEXT("Blizzard"),
			TEXT("Hard wind: snow plastered onto every windward wall, heavy rime feathers into the wind, bare leeward faces, ")
			TEXT("short icicles leaning downwind. Set the zone's Wind Direction (the blue arrow) to aim it."),
			[](UIcifyPreset& P)
			{
				P.Intensity = 1.1f;
				P.WindInfluence = 1.f;
				P.SnowfallSlantDegrees = 35.f;
				P.FrostIntensity = 1.2f;
				P.EdgeFrost = 1.f;
				P.Rime = 1.f;
				P.SnowIntensity = 1.2f;
				P.WallPlaster = 0.8f;
				P.EdgeSnowLoss = 0.5f;
				P.GlazeIntensity = 0.5f;
				P.AmbientDrips = 0.1f;
				P.IcicleDensity = 6.f;
				P.IcicleMaxLengthCm = 30.f;
				P.IcicleLean = 0.6f;
				P.Palette.bOverrideSnowBreakup = true;
				P.Palette.SnowBreakup = 0.6f;
				P.Palette.bOverrideSparkle = true;
				P.Palette.Sparkle = 0.3f;
			} });

		Result.Add({ TEXT("Thaw"),
			TEXT("Late winter, melting by day and refreezing by night: patchy snow, wet glazed walls, long thin dripping icicles, ")
			TEXT("and wide frozen puddles of black ice. Turn on Trace Meltwater to follow the water down the geometry."),
			[](UIcifyPreset& P)
			{
				P.Intensity = 1.f;
				P.FrostIntensity = 0.4f;
				P.SnowIntensity = 0.6f;
				P.EdgeSnowLoss = 0.6f;
				P.GlazeIntensity = 1.2f;
				P.GlazeCoat = 0.4f;
				P.AmbientDrips = 0.6f;
				P.DripLengthCm = 250.f;
				P.BlackIceIntensity = 1.3f;
				P.FloorIce = 0.5f;
				P.CreviceIce = 0.8f;
				P.PuddleRadiusCm = 90.f;
				P.IcicleDensity = 6.f;
				P.IcicleMaxLengthCm = 55.f;
				P.IcicleThickness = 0.7f;
				P.IcicleClumping = 0.4f;
				P.Palette.bOverrideSnowBreakup = true;
				P.Palette.SnowBreakup = 0.85f;
				P.Palette.bOverrideSparkle = true;
				P.Palette.Sparkle = 0.2f;
			} });

		return Result;
	}

	/** Shared by Build and ConfigureLook so a preset made elsewhere matches the library's IP_<Look> field for field. */
	void WriteLook(const FLook& Look, UIcifyPreset& Preset)
	{
		// Start from defaults so overwriting a preset doesn't keep stale tweaks.
		const UIcifyPreset* Defaults = GetDefault<UIcifyPreset>();
#define ICIFY_RESET_FIELD(Field) Preset.Field = Defaults->Field;
		ICIFY_PRESET_FIELDS(ICIFY_RESET_FIELD)
#undef ICIFY_RESET_FIELD
		Preset.Notes = Look.Notes;
		Preset.bIncludeSeed = false;
		Look.Configure(Preset);
	}
}

TArray<FString> FIcifyLibraryBuilder::LookNames()
{
	TArray<FString> Names;
	for (const FLook& Look : Looks())
	{
		Names.Add(Look.Name);
	}
	return Names;
}

bool FIcifyLibraryBuilder::ConfigureLook(const FString& LookName, UIcifyPreset& Preset)
{
	for (const FLook& Look : Looks())
	{
		if (LookName.Equals(Look.Name, ESearchCase::IgnoreCase))
		{
			WriteLook(Look, Preset);
			return true;
		}
	}
	return false;
}

bool FIcifyLibraryBuilder::Build(bool bOverwrite, FString& OutMessage)
{
	UMaterialInterface* Example = LoadObject<UMaterialInterface>(nullptr, ExampleMaterialObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Example)
	{
		OutMessage = TEXT("M_IcifyExample is missing; run Tools > Icify > Update Plugin Content first.");
		return false;
	}

	TArray<UPackage*> Packages;
	int32 Created = 0;
	int32 Updated = 0;
	for (const FLook& Look : Looks())
	{
		bool bCreated = false;
		if (UIcifyPreset* Preset = FindOrCreate<UIcifyPreset>(FString::Printf(TEXT("IP_%s"), Look.Name), bOverwrite, bCreated))
		{
			Preset->Modify();
			WriteLook(Look, *Preset);
			Preset->MarkPackageDirty();
			Packages.Add(Preset->GetPackage());
			(bCreated ? Created : Updated) += 1;
		}
	}

	// Surfaces to try the looks on: each family forces its surface type, so a zone on Auto treats it the same way.
	struct FFamily { const TCHAR* Name; FLinearColor Color; float Metallic; float Roughness; EIcifySurfaceType Surface; };
	const FFamily Families[] = {
		{ TEXT("Stone"), FLinearColor(0.42f, 0.40f, 0.37f), 0.f, 0.8f, EIcifySurfaceType::Stone },
		{ TEXT("Metal"), FLinearColor(0.32f, 0.33f, 0.35f), 1.f, 0.4f, EIcifySurfaceType::Metal },
		{ TEXT("Glass"), FLinearColor(0.05f, 0.06f, 0.07f), 0.f, 0.05f, EIcifySurfaceType::Glass } };
	for (const FFamily& Family : Families)
	{
		bool bCreated = false;
		UMaterialInstanceConstant* Instance = FindOrCreate<UMaterialInstanceConstant>(FString::Printf(TEXT("MI_Icify_%s"), Family.Name), bOverwrite, bCreated);
		if (!Instance)
		{
			continue;
		}
		Instance->Modify();
		Instance->SetParentEditorOnly(Example);
		Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Base Color")), Family.Color);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), Family.Roughness);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Metallic")), Family.Metallic);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Icify::SurfaceOverrideParameterName()), float(int32(Family.Surface) - 1));
		Instance->PostEditChange();
		Instance->MarkPackageDirty();
		Packages.Add(Instance->GetPackage());
		(bCreated ? Created : Updated) += 1;
	}

	if (Packages.Num() == 0)
	{
		OutMessage = TEXT("Library already exists in /Game/Icify/Library (use Reset Preset Library to overwrite).");
		return true;
	}
	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty*/ false);
	OutMessage = FString::Printf(TEXT("Preset library: %d created, %d updated in /Game/Icify/Library%s."),
		Created, Updated, bSaved ? TEXT("") : TEXT(" (some packages failed to save)"));
	return bSaved;
}
