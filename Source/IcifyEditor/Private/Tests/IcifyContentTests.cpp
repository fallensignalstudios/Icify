// Copyright Fallen Signal Studios. All Rights Reserved.
// This regression deliberately rebuilds generated plugin content in an isolated BuildPlugin HostProject.
#include "IcifyContentBuilder.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "IcifySettings.h"
#include "IcifyTypes.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialFunction.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIcifyContentRegenerationTest, "Icify.Content.InPlaceRegeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIcifyContentRegenerationTest::RunTest(const FString& Parameters)
{
	if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("HostProject"))
	{
		AddError(TEXT("Generated-content regeneration must run in the isolated BuildPlugin HostProject, not a production project."));
		return false;
	}

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Icify"));
	if (!TestTrue(TEXT("Isolated Icify plugin exists"), Plugin.IsValid())) { return false; }
	if (!TestTrue(TEXT("Regeneration fixture is a project plugin"), Plugin->GetType() == EPluginType::Project)) { return false; }

	UIcifySettings* Settings = GetMutableDefault<UIcifySettings>();
	const int32 OriginalBase = Settings->CustomPrimitiveDataIndex;
	TGuardValue<int32> RestoreBase(Settings->CustomPrimitiveDataIndex, OriginalBase);
	TArray<int32> OriginalExpressionCounts;
	for (const int32 Base : { 8, 12, 8 })
	{
		Settings->CustomPrimitiveDataIndex = Base;
		const FIcifyContentReport Report = FIcifyContentBuilder::Run(true);
		TestTrue(TEXT("Rebuilt content is ready"), Report.bReady);
		TestTrue(TEXT("All generated packages saved"), Report.bSaveAttempted && Report.bSaved);
		TestEqual(TEXT("No rebuild errors"), Report.Errors.Num(), 0);
		for (const FString& Error : Report.Errors) { AddError(Error); }
		if (Report.HasErrors()) { return false; }

		UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, TEXT("/Icify/Materials/MF_IcifyFrost.MF_IcifyFrost"));
		UMaterial* Debug = LoadObject<UMaterial>(nullptr, TEXT("/Icify/Materials/M_IcifyMaskDebug.M_IcifyMaskDebug"));
		UMaterial* Example = LoadObject<UMaterial>(nullptr, TEXT("/Icify/Materials/M_IcifyExample.M_IcifyExample"));
		UMaterial* Icicle = LoadObject<UMaterial>(nullptr, TEXT("/Icify/Materials/M_IcifyIcicle.M_IcifyIcicle"));
		if (!TestNotNull(TEXT("Generated function"), Function) || !TestNotNull(TEXT("Generated debug material"), Debug)
			|| !TestNotNull(TEXT("Generated example material"), Example) || !TestNotNull(TEXT("Generated icicle material"), Icicle)) { return false; }

		const TArray<int32> Counts{ Function->GetExpressions().Num(), Debug->GetExpressions().Num(), Example->GetExpressions().Num(), Icicle->GetExpressions().Num() };
		if (OriginalExpressionCounts.Num() == 0) { OriginalExpressionCounts = Counts; }
		for (int32 Index = 0; Index < Counts.Num(); ++Index)
		{
			TestEqual(TEXT("Rebuild retains exact expression count; no stale nodes"), Counts[Index], OriginalExpressionCounts[Index]);
		}

		auto CheckOwnership = [&](TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions)
		{
			int32 ActiveCount = 0;
			int32 ModeCount = 0;
			for (UMaterialExpression* Expression : Expressions)
			{
				if (const UMaterialExpressionScalarParameter* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression))
				{
					if (Parameter->ParameterName == Icify::ActiveParameterName())
					{
						++ActiveCount;
						TestFalse(TEXT("Active is not CPD"), Parameter->bUseCustomPrimitiveData);
						TestEqual(TEXT("Active remains inactive by default"), Parameter->DefaultValue, 0.f);
					}
					if (Parameter->ParameterName == TEXT("Icify Mask Mode"))
					{
						++ModeCount;
						TestTrue(TEXT("Mode reads custom primitive data"), Parameter->bUseCustomPrimitiveData);
						TestEqual(TEXT("Mode defaults to zero"), Parameter->DefaultValue, 0.f);
						TestEqual(TEXT("Mode follows current CPD base"), int32(Parameter->PrimitiveDataIndex), Base + Icify::CPD_MaskMode);
					}
				}
			}
			TestEqual(TEXT("Exactly one Active parameter"), ActiveCount, 1);
			TestEqual(TEXT("Exactly one Mode parameter"), ModeCount, 1);
		};
		CheckOwnership(Function->GetExpressions());
		CheckOwnership(Debug->GetExpressions());

		TSet<FName> OutputNames;
		int32 OutputCount = 0;
		const UMaterialExpressionCustom* RoughnessResult = nullptr;
		const UMaterialExpressionCustom* MetallicResult = nullptr;
		for (UMaterialExpression* Expression : Function->GetExpressions())
		{
			if (const UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
			{
				++OutputCount;
				OutputNames.Add(Output->OutputName);
				TestNotNull(TEXT("Every function output is connected"), Output->A.Expression);
				if (Output->OutputName == TEXT("Roughness") || Output->OutputName == TEXT("Metallic"))
				{
					const bool bRoughness = Output->OutputName == TEXT("Roughness");
					const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Output->A.Expression);
					if (!TestNotNull(TEXT("Surface output uses component mask"), Mask)) { return false; }
					TestTrue(TEXT("Roughness R-only; Metallic G-only"), Mask->R == bRoughness && Mask->G != bRoughness && !Mask->B && !Mask->A);
					const UMaterialExpressionCustom* Result = Cast<UMaterialExpressionCustom>(Mask->Input.Expression);
					if (!TestNotNull(TEXT("Surface mask reads custom result"), Result)) { return false; }
					TestTrue(TEXT("Surface custom returns float2"), Result->OutputType == CMOT_Float2);
					(bRoughness ? RoughnessResult : MetallicResult) = Result;
				}
			}
		}
		TestEqual(TEXT("Exactly four function outputs"), OutputCount, 4);
		TestTrue(TEXT("Exact named function outputs"), OutputNames.Num() == 4 && OutputNames.Contains(TEXT("BaseColor"))
			&& OutputNames.Contains(TEXT("Roughness")) && OutputNames.Contains(TEXT("Metallic")) && OutputNames.Contains(TEXT("Normal")));
		TestTrue(TEXT("Roughness and metallic share the surface result"), RoughnessResult && RoughnessResult == MetallicResult);

		TestTrue(TEXT("Icicles use the subsurface shading model"), Icicle->GetShadingModels().HasShadingModel(MSM_Subsurface));
		for (const EMaterialProperty Property : { MP_BaseColor, MP_Roughness, MP_Specular, MP_Opacity, MP_SubsurfaceColor, MP_Normal })
		{
			const FExpressionInput* Input = Icicle->GetExpressionInputForProperty(Property);
			TestTrue(TEXT("Every icicle shading input is connected"), Input && Input->Expression);
		}
		TestFalse(TEXT("Saved graph and token need no further rebuild"), FIcifyContentBuilder::NeedsWork());

		// Readiness must authenticate the real saved bytes of all six outputs, not merely their in-memory graphs.
		FString Token;
		const FString TokenPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Icify"), TEXT("ContentReady.txt"));
		if (!TestTrue(TEXT("Saved readiness token is readable"), FFileHelper::LoadFileToString(Token, *TokenPath))) { return false; }
		TArray<FString> TokenLines;
		Token.ParseIntoArrayLines(TokenLines);
		TestEqual(TEXT("Token has marker plus shader and six generated files"), TokenLines.Num(), 8);
		const FString Marker = FString::Printf(TEXT("[IcifyContent v%d cpd=%d uv=%d]"), Icify::ContentVersion, Base, Settings->MaskUVChannel);
		if (TokenLines.Num() == 0) { return false; }
		TestEqual(TEXT("Token uses the current generated-content settings"), TokenLines[0], Marker);
		TArray<FString> SavedFiles{ FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"), TEXT("Private"), TEXT("Icify.ush")) };
		for (const TCHAR* Package : { TEXT("/Icify/BP_FreezeZone"), TEXT("/Icify/Materials/MF_IcifyFrost"), TEXT("/Icify/Materials/M_IcifyMaskDebug"),
			TEXT("/Icify/Materials/M_IcifyExample"), TEXT("/Icify/Materials/M_IcifyIcicle"), TEXT("/Icify/Materials/T_IcifyNoMask") })
		{
			SavedFiles.Add(FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension()));
		}
		for (const FString& File : SavedFiles)
		{
			TArray<uint8> Bytes;
			if (!TestTrue(TEXT("Every readiness file is saved and readable"), FFileHelper::LoadFileToArray(Bytes, *File) && Bytes.Num() > 0)) { return false; }
			const FString ExpectedLine = File + TEXT(" ") + FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num()).ToString();
			TestTrue(TEXT("Readiness hash matches each actual saved file"), TokenLines.Contains(ExpectedLine));
		}
	}

	// Restore both the setting and generated graphs if the caller began with a different CPD base.
	Settings->CustomPrimitiveDataIndex = OriginalBase;
	if (OriginalBase != 8)
	{
		const FIcifyContentReport RestoreReport = FIcifyContentBuilder::Run(true);
		TestFalse(TEXT("Original generated-content setting restored"), RestoreReport.HasErrors());
	}
	return !HasAnyErrors();
}

#endif
