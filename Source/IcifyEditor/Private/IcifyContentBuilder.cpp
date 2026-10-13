// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifyContentBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "IcifyCompat.h"
#include "IcifyFreezeZone.h"
#include "IcifySettings.h"
#include "IcifyTypes.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "RHIGlobals.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#define LOCTEXT_NAMESPACE "IcifyContentBuilder"

FString FIcifyContentReport::ToString() const
{
	TArray<FString> Lines;
	for (const FString& Item : Created)
	{
		Lines.Add(TEXT("Created  ") + Item);
	}
	for (const FString& Item : Rebuilt)
	{
		Lines.Add(TEXT("Rebuilt  ") + Item);
	}
	for (const FString& Item : Notes)
	{
		Lines.Add(TEXT("Note     ") + Item);
	}
	for (const FString& Item : Errors)
	{
		Lines.Add(TEXT("ERROR    ") + Item);
	}
	Lines.Add(bSaveAttempted
		? (bSaved ? TEXT("Saved    ok") : TEXT("ERROR    one or more packages failed to save"))
		: TEXT("Saved    not attempted"));
	Lines.Add(bReady ? TEXT("Ready    validated saved content") : TEXT("Ready    false; generated content requires repair"));
	return FString::Join(Lines, TEXT("\n"));
}

namespace
{
	const TCHAR* ZoneBlueprintPath = TEXT("/Icify/BP_FreezeZone");
	const TCHAR* FunctionPath = TEXT("/Icify/Materials/MF_IcifyFrost");
	const TCHAR* DebugMaterialPath = TEXT("/Icify/Materials/M_IcifyMaskDebug");
	const TCHAR* ExampleMaterialPath = TEXT("/Icify/Materials/M_IcifyExample");
	const TCHAR* IcicleMaterialPath = TEXT("/Icify/Materials/M_IcifyIcicle");
	const TCHAR* NoMaskTexturePath = TEXT("/Icify/Materials/T_IcifyNoMask");
	const TCHAR* IncludePath = TEXT("/Plugin/Icify/Private/Icify.ush");

	constexpr EObjectFlags AssetFlags = RF_Public | RF_Standalone | RF_Transactional;

	/** Changes whenever the generated graphs would change. */
	FString CurrentMarker()
	{
		const UIcifySettings* Settings = UIcifySettings::Get();
		return FString::Printf(TEXT("[IcifyContent v%d cpd=%d uv=%d]"), Icify::ContentVersion, Settings->CustomPrimitiveDataIndex, Settings->MaskUVChannel);
	}

	FString PendingDescription()
	{
		return TEXT("Applies Icify ice (frost, snow, glaze, black ice) baked by an Icify Freeze Zone. ")
			TEXT("Inactive owned-material gate passes artist inputs through unchanged. [IcifyContent pending validation]");
	}

	FString ReadinessPath()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Icify"), TEXT("ContentReady.txt"));
	}

	FString ShaderPath()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Icify"));
		return Plugin ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"), TEXT("Private"), TEXT("Icify.ush")) : FString();
	}

	TArray<const TCHAR*> GeneratedPaths()
	{
		return { ZoneBlueprintPath, FunctionPath, DebugMaterialPath, ExampleMaterialPath, IcicleMaterialPath, NoMaskTexturePath };
	}

	// The label alone is not readiness: a partial SavePackages failure may have written some packages.
	// Authenticate only the shader and complete generated package set from a successful build.
	bool MakeReadinessToken(FString& Token)
	{
		Token = CurrentMarker();
		TArray<FString> Files{ ShaderPath() };
		for (const TCHAR* Path : GeneratedPaths())
		{
			Files.Add(FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension()));
		}
		for (const FString& File : Files)
		{
			TArray<uint8> Bytes;
			if (File.IsEmpty() || !FFileHelper::LoadFileToArray(Bytes, *File) || Bytes.IsEmpty())
			{
				return false;
			}
			Token += TEXT("\n") + File + TEXT(" ") + FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num()).ToString();
		}
		return true;
	}

	FString AssetName(const TCHAR* PackagePath)
	{
		return FPackageName::GetShortName(PackagePath);
	}

	template <typename T>
	T* FindExisting(const TCHAR* PackagePath)
	{
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), PackagePath, *AssetName(PackagePath));
		if (T* Loaded = FindObject<T>(nullptr, *ObjectPath))
		{
			return Loaded;
		}
		if (!FPackageName::DoesPackageExist(PackagePath))
		{
			return nullptr;
		}
		return LoadObject<T>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** Collects connection failures so a build can't silently produce a half-wired graph. */
	struct FBuildContext
	{
		FIcifyContentReport& Report;
		FString Asset;

		void Connect(UMaterialExpression* From, const TCHAR* FromOutput, UMaterialExpression* To, const TCHAR* ToInput)
		{
			if (!From || !To || !UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOutput, To, ToInput))
			{
				Report.Errors.Add(FString::Printf(TEXT("%s: could not connect %s.%s -> %s.%s"), *Asset,
					From ? *From->GetName() : TEXT("null"), FromOutput, To ? *To->GetName() : TEXT("null"), ToInput));
			}
		}

		void ConnectProperty(UMaterialExpression* From, const TCHAR* FromOutput, EMaterialProperty Property, const TCHAR* PropertyName)
		{
			if (!From || !UMaterialEditingLibrary::ConnectMaterialProperty(From, FromOutput, Property))
			{
				Report.Errors.Add(FString::Printf(TEXT("%s: could not connect %s to %s"), *Asset, FromOutput, PropertyName));
			}
		}
	};

	template <typename T>
	T* AddToFunction(UMaterialFunction* Function, int32 X, int32 Y)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, T::StaticClass(), X, Y));
	}

	template <typename T>
	T* AddToMaterial(UMaterial* Material, int32 X, int32 Y)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), X, Y));
	}

	// UE 5.8's DeleteAll helpers iterate the live collection while each deletion compacts it.
	// Iterate a stable snapshot instead, otherwise old parameters/outputs can survive a rebuild.
	bool ClearFunctionExpressions(UMaterialFunction* Function, FIcifyContentReport& Report)
	{
		TArray<UMaterialExpression*> Expressions;
		for (UMaterialExpression* Expression : Function->GetExpressions()) { Expressions.Add(Expression); }
		for (UMaterialExpression* Expression : Expressions)
		{
			UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Function, Expression);
		}
		if (Function->GetExpressions().Num() != 0)
		{
			Report.Errors.Add(FString::Printf(TEXT("%s: expression deletion was incomplete; graph was not repopulated"), *Function->GetName()));
			return false;
		}
		return true;
	}

	bool ClearMaterialExpressions(UMaterial* Material, FIcifyContentReport& Report)
	{
		TArray<UMaterialExpression*> Expressions;
		for (UMaterialExpression* Expression : Material->GetExpressions()) { Expressions.Add(Expression); }
		for (UMaterialExpression* Expression : Expressions)
		{
			UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
		}
		if (Material->GetExpressions().Num() != 0)
		{
			Report.Errors.Add(FString::Printf(TEXT("%s: expression deletion was incomplete; graph was not repopulated"), *Material->GetName()));
			return false;
		}
		return true;
	}

	void SetCustomInputs(UMaterialExpressionCustom* Custom, const TArray<const TCHAR*>& Names)
	{
		Custom->Inputs.Reset();
		for (const TCHAR* Name : Names)
		{
			FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
			Input.InputName = FName(Name);
		}
	}

	void SetMask(UMaterialExpressionComponentMask* Mask, bool bR, bool bG, bool bB, bool bA)
	{
		// The defaults are not all-off, so always set every channel.
		Mask->R = bR;
		Mask->G = bG;
		Mask->B = bB;
		Mask->A = bA;
	}

	int32 CustomDataIndex(int32 Offset)
	{
		return UIcifySettings::Get()->CustomPrimitiveDataIndex + Offset;
	}

	/** Expressions feeding the custom nodes, by custom input name. */
	struct FNodeSources
	{
		TMap<FString, TPair<UMaterialExpression*, FString>> ByInput; // custom input -> (expression, output name)

		void Add(const TCHAR* Input, UMaterialExpression* Expression, const TCHAR* Output = TEXT(""))
		{
			ByInput.Add(Input, TPair<UMaterialExpression*, FString>(Expression, Output));
		}
	};

	/** Connects every input of a custom node to its source, reporting any input without one. */
	void ConnectInputs(UMaterialExpressionCustom* Custom, FBuildContext& Ctx, const FNodeSources& Sources, const TArray<const TCHAR*>& InputNames)
	{
		for (const TCHAR* Input : InputNames)
		{
			if (const TPair<UMaterialExpression*, FString>* Source = Sources.ByInput.Find(Input))
			{
				Ctx.Connect(Source->Key, *Source->Value, Custom, Input);
			}
			else
			{
				Ctx.Report.Errors.Add(FString::Printf(TEXT("%s: no source for custom input %s"), *Ctx.Asset, Input));
			}
		}
	}

	// ------------------------------------------------------------ MF_IcifyFrost

	const TArray<const TCHAR*> FrostInputNames = {
		TEXT("BaseColor"), TEXT("Roughness"), TEXT("Metallic"), TEXT("NormalTS"),
		TEXT("MaskR"), TEXT("MaskG"), TEXT("MaskB"), TEXT("MaskA"),
		TEXT("TexR"), TEXT("TexG"), TEXT("TexB"), TEXT("TexA"), TEXT("MaskMode"),
		TEXT("WorldPos"), TEXT("WorldNormal"), TEXT("CameraPos"),
		TEXT("SurfaceType"), TEXT("SurfaceOverride"), TEXT("FeatureScale"), TEXT("SeedOffset"),
		TEXT("FrostColor"), TEXT("SnowColor"), TEXT("IceTint"),
		TEXT("Strength"), TEXT("FrostStrength"), TEXT("SnowStrength"), TEXT("GlazeStrength"), TEXT("BlackIceStrength"),
		TEXT("Relief"), TEXT("Contrast"),
		TEXT("CrystalSize"), TEXT("FernFrost"), TEXT("SnowBreakup"), TEXT("Sparkle") };

	const TCHAR* CommonCode =
		// Contrast reshapes how boldly faint ice reads (exactly unchanged at 1).
		TEXT("const float4 VM = IcifyContrast(float4(MaskR, MaskG, MaskB, MaskA), Contrast);\n")
		TEXT("const float4 TM = IcifyContrast(float4(TexR, TexG, TexB, TexA), Contrast);\n")
		TEXT("const float4 LS = float4(FrostStrength, SnowStrength, GlazeStrength, BlackIceStrength);\n")
		// Art direction (crystal size, fern frost on glass, snow breakup, sparkle); glints fade with camera distance.
		TEXT("const FIcifyArt Art = IcifyMakeArt(FeatureScale, CrystalSize, FernFrost, SnowBreakup, Sparkle, length(WorldPos - CameraPos));\n");

	const TCHAR* EvaluateCode =
		TEXT("FIcifyResult R = IcifyEvaluate(BaseColor, Roughness, Metallic, MaskMode, VM, TM, WorldPos, WorldNormal, ")
		TEXT("SurfaceType, SurfaceOverride, FeatureScale, SeedOffset, FrostColor.rgb, SnowColor.rgb, IceTint.rgb, Strength, LS, Art);\n");

	UMaterialExpressionCustom* AddFrostCustom(UMaterialFunction* Function, FBuildContext& Ctx, const FNodeSources& Sources,
		const TCHAR* Description, ECustomMaterialOutputType OutputType, const FString& Body, int32 X, int32 Y)
	{
		UMaterialExpressionCustom* Custom = AddToFunction<UMaterialExpressionCustom>(Function, X, Y);
		Custom->Description = Description;
		Custom->OutputType = OutputType;
		Custom->IncludeFilePaths.Add(IncludePath);
		Custom->Code = FString(CommonCode) + Body;
		SetCustomInputs(Custom, FrostInputNames);
		ConnectInputs(Custom, Ctx, Sources, FrostInputNames);
		return Custom;
	}

	UMaterialExpressionFunctionInput* AddFunctionInput(UMaterialFunction* Function, const TCHAR* Name, EFunctionInputType Type, int32 Sort, const FVector4f& Default, const TCHAR* Description, int32 Y)
	{
		UMaterialExpressionFunctionInput* Input = AddToFunction<UMaterialExpressionFunctionInput>(Function, -1400, Y);
		Input->InputName = FName(Name);
		Input->InputType = Type;
		Input->SortPriority = Sort;
		Input->Description = Description;
		Input->bUsePreviewValueAsDefault = true;
		Input->PreviewValue = Default;
		return Input;
	}

	UMaterialExpressionScalarParameter* AddScalar(UMaterialFunction* Function, const TCHAR* Name, float Default, int32 Y)
	{
		UMaterialExpressionScalarParameter* Param = AddToFunction<UMaterialExpressionScalarParameter>(Function, -1400, Y);
		Param->ParameterName = FName(Name);
		Param->Group = TEXT("Icify");
		Param->DefaultValue = Default;
		return Param;
	}

	UMaterialExpressionScalarParameter* AddCustomDataParameter(UMaterialFunction* Function, const TCHAR* Name, int32 Offset, int32 Y)
	{
		UMaterialExpressionScalarParameter* Param = AddScalar(Function, Name, 0.f, Y);
		Param->bUseCustomPrimitiveData = true;
		Param->PrimitiveDataIndex = uint8(CustomDataIndex(Offset));
		return Param;
	}

	UMaterialExpressionVectorParameter* AddColor(UMaterialFunction* Function, const TCHAR* Name, const FLinearColor& Default, int32 Y)
	{
		UMaterialExpressionVectorParameter* Param = AddToFunction<UMaterialExpressionVectorParameter>(Function, -1400, Y);
		Param->ParameterName = FName(Name);
		Param->Group = TEXT("Icify");
		Param->DefaultValue = Default;
		return Param;
	}

	/**
	 * The mask UV: the mask channel, or UV0 on meshes the zone had to bake there (Icify Use UV0 on their owned instance).
	 * Add(Class, X, Y) creates an expression in the function or material being built.
	 */
	template <typename TAdd>
	UMaterialExpression* AddMaskUV(TAdd&& Add, FBuildContext& Ctx, UMaterialExpressionScalarParameter* UseFirst)
	{
		UMaterialExpressionTextureCoordinate* MaskChannel = Cast<UMaterialExpressionTextureCoordinate>(Add(UMaterialExpressionTextureCoordinate::StaticClass(), -2000, 700));
		UMaterialExpressionTextureCoordinate* FirstChannel = Cast<UMaterialExpressionTextureCoordinate>(Add(UMaterialExpressionTextureCoordinate::StaticClass(), -2000, 820));
		UMaterialExpressionLinearInterpolate* PickUV = Cast<UMaterialExpressionLinearInterpolate>(Add(UMaterialExpressionLinearInterpolate::StaticClass(), -1700, 760));
		if (!MaskChannel || !FirstChannel || !PickUV)
		{
			Ctx.Report.Errors.Add(FString::Printf(TEXT("%s: could not create the mask UV nodes"), *Ctx.Asset));
			return nullptr;
		}
		MaskChannel->CoordinateIndex = UIcifySettings::Get()->MaskUVChannel;
		FirstChannel->CoordinateIndex = 0;
		Ctx.Connect(MaskChannel, TEXT(""), PickUV, TEXT("A"));
		Ctx.Connect(FirstChannel, TEXT(""), PickUV, TEXT("B"));
		Ctx.Connect(UseFirst, TEXT(""), PickUV, TEXT("Alpha"));
		return PickUV;
	}

	/** Everything the frost custom nodes read besides the artist's own inputs. */
	void AddSharedSources(UMaterialFunction* Function, FBuildContext& Ctx, UTexture2D* NoMask, FNodeSources& S)
	{
		UMaterialExpressionFunctionInput* SurfaceInput = AddFunctionInput(Function, TEXT("SurfaceOverride"), FunctionInput_Scalar, 4, FVector4f(-1.f, 0.f, 0.f, 0.f),
			TEXT("-1 = use the type the zone detected for this mesh. 0 = stone, 1 = metal, 2 = glass. Unconnected: use the owned material instance's Icify Surface Override."), 480);
		UMaterialExpressionScalarParameter* SurfaceFallback = AddScalar(Function, *Icify::SurfaceOverrideParameterName().ToString(), -1.f, 520);
		Ctx.Connect(SurfaceFallback, TEXT(""), SurfaceInput, TEXT("Preview"));
		S.Add(TEXT("SurfaceOverride"), SurfaceInput);

		UMaterialExpressionVertexColor* VertexColor = AddToFunction<UMaterialExpressionVertexColor>(Function, -1400, 600);
		S.Add(TEXT("MaskR"), VertexColor, TEXT("R"));
		S.Add(TEXT("MaskG"), VertexColor, TEXT("G"));
		S.Add(TEXT("MaskB"), VertexColor, TEXT("B"));
		S.Add(TEXT("MaskA"), VertexColor, TEXT("A"));

		UMaterialExpressionScalarParameter* UseFirst = AddScalar(Function, *Icify::UseUV0ParameterName().ToString(), 0.f, 900);
		UMaterialExpression* MaskUV = AddMaskUV([Function](UClass* Class, int32 X, int32 Y)
		{
			return UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, Class, X, Y);
		}, Ctx, UseFirst);
		UMaterialExpressionTextureSampleParameter2D* MaskTexture = AddToFunction<UMaterialExpressionTextureSampleParameter2D>(Function, -1400, 760);
		MaskTexture->ParameterName = Icify::MaskTextureParameterName();
		MaskTexture->Group = TEXT("Icify");
		MaskTexture->Texture = NoMask;
		MaskTexture->SamplerType = SAMPLERTYPE_LinearColor;
		Ctx.Connect(MaskUV, TEXT(""), MaskTexture, TEXT("UVs"));
		S.Add(TEXT("TexR"), MaskTexture, TEXT("R"));
		S.Add(TEXT("TexG"), MaskTexture, TEXT("G"));
		S.Add(TEXT("TexB"), MaskTexture, TEXT("B"));
		S.Add(TEXT("TexA"), MaskTexture, TEXT("A"));

		S.Add(TEXT("WorldPos"), AddToFunction<UMaterialExpressionWorldPosition>(Function, -1400, 1000));
		S.Add(TEXT("WorldNormal"), AddToFunction<UMaterialExpressionVertexNormalWS>(Function, -1400, 1100));
		S.Add(TEXT("CameraPos"), AddToFunction<UMaterialExpressionCameraPositionWS>(Function, -1400, 1150));

		UMaterialExpressionScalarParameter* Mode = AddCustomDataParameter(Function, TEXT("Icify Mask Mode"), Icify::CPD_MaskMode, 1200);
		UMaterialExpressionScalarParameter* Active = AddScalar(Function, *Icify::ActiveParameterName().ToString(), 0.f, 1160);
		UMaterialExpressionMultiply* OwnedMode = AddToFunction<UMaterialExpressionMultiply>(Function, -1050, 1200);
		Ctx.Connect(Mode, TEXT(""), OwnedMode, TEXT("A"));
		Ctx.Connect(Active, TEXT(""), OwnedMode, TEXT("B"));
		S.Add(TEXT("MaskMode"), OwnedMode);
		S.Add(TEXT("SurfaceType"), AddCustomDataParameter(Function, TEXT("Icify Surface Type"), Icify::CPD_SurfaceType, 1300));
		S.Add(TEXT("FeatureScale"), AddCustomDataParameter(Function, TEXT("Icify Feature Scale"), Icify::CPD_FeatureScale, 1400));
		S.Add(TEXT("SeedOffset"), AddCustomDataParameter(Function, TEXT("Icify Seed Offset"), Icify::CPD_SeedOffset, 1500));

		S.Add(TEXT("FrostColor"), AddColor(Function, *Icify::FrostColorParameterName().ToString(), FLinearColor(0.86f, 0.92f, 1.f), 1600));
		S.Add(TEXT("SnowColor"), AddColor(Function, TEXT("Icify Snow Color"), FLinearColor(0.93f, 0.95f, 1.f), 1800));
		S.Add(TEXT("IceTint"), AddColor(Function, *Icify::IceTintParameterName().ToString(), FLinearColor(0.55f, 0.78f, 0.95f), 2000));

		S.Add(TEXT("Strength"), AddScalar(Function, TEXT("Icify Strength"), 1.f, 2200));
		S.Add(TEXT("FrostStrength"), AddScalar(Function, TEXT("Icify Frost Strength"), 1.f, 2300));
		S.Add(TEXT("SnowStrength"), AddScalar(Function, TEXT("Icify Snow Strength"), 1.f, 2400));
		S.Add(TEXT("GlazeStrength"), AddScalar(Function, TEXT("Icify Glaze Strength"), 1.f, 2500));
		S.Add(TEXT("BlackIceStrength"), AddScalar(Function, TEXT("Icify Black Ice Strength"), 1.f, 2600));
		S.Add(TEXT("Relief"), AddScalar(Function, TEXT("Icify Relief Strength"), 1.f, 2700));
		S.Add(TEXT("Contrast"), AddScalar(Function, TEXT("Icify Contrast"), 1.f, 2800));
		// Art direction; the defaults give the standard look.
		S.Add(TEXT("CrystalSize"), AddScalar(Function, TEXT("Icify Frost Crystal Size"), 0.f, 2900));
		S.Add(TEXT("FernFrost"), AddScalar(Function, TEXT("Icify Fern Frost"), 1.f, 3000));
		S.Add(TEXT("SnowBreakup"), AddScalar(Function, TEXT("Icify Snow Breakup"), 0.5f, 3100));
		S.Add(TEXT("Sparkle"), AddScalar(Function, TEXT("Icify Sparkle"), 0.5f, 3200));
	}

	void PopulateFunction(UMaterialFunction* Function, UTexture2D* NoMask, FIcifyContentReport& Report)
	{
		FBuildContext Ctx{ Report, TEXT("MF_IcifyFrost") };

		Function->Description = PendingDescription();
		Function->bExposeToLibrary = true;
		Function->LibraryCategoriesText.Reset();
		Function->LibraryCategoriesText.Add(LOCTEXT("LibraryCategory", "Icify"));

		FNodeSources S;
		S.Add(TEXT("BaseColor"), AddFunctionInput(Function, TEXT("BaseColor"), FunctionInput_Vector3, 0, FVector4f(0.5f, 0.5f, 0.5f, 0.f), TEXT("Base colour without ice."), 0));
		S.Add(TEXT("Roughness"), AddFunctionInput(Function, TEXT("Roughness"), FunctionInput_Scalar, 1, FVector4f(0.6f, 0.f, 0.f, 0.f), TEXT("Roughness without ice."), 120));
		S.Add(TEXT("Metallic"), AddFunctionInput(Function, TEXT("Metallic"), FunctionInput_Scalar, 2, FVector4f(0.f, 0.f, 0.f, 0.f), TEXT("Metallic without ice."), 240));
		S.Add(TEXT("NormalTS"), AddFunctionInput(Function, TEXT("Normal"), FunctionInput_Vector3, 3, FVector4f(0.f, 0.f, 1.f, 0.f),
			TEXT("Your tangent-space normal. Snow and ice flatten it where they cover the surface, and add their own relief (Icify Relief Strength)."), 360));
		AddSharedSources(Function, Ctx, NoMask, S);

		UMaterialExpressionCustom* ColorNode = AddFrostCustom(Function, Ctx, S, TEXT("Icify Base Color"), CMOT_Float3,
			FString(EvaluateCode) + TEXT("return R.BaseColor;"), -600, 0);
		UMaterialExpressionCustom* SurfaceNode = AddFrostCustom(Function, Ctx, S, TEXT("Icify Roughness Metallic"), CMOT_Float2,
			FString(EvaluateCode) + TEXT("return float2(R.Roughness, R.Metallic);"), -600, 500);
		UMaterialExpressionCustom* NormalNode = AddFrostCustom(Function, Ctx, S, TEXT("Icify Relief Normal"), CMOT_Float3,
			TEXT("return IcifyReliefNormal(NormalTS, Relief, MaskMode, VM, TM, WorldPos, WorldNormal, ")
			TEXT("Parameters.TangentToWorld[0], Parameters.TangentToWorld[1], FeatureScale, SeedOffset, SurfaceType, SurfaceOverride, Strength, LS, Art);"), -600, 1000);

		UMaterialExpressionComponentMask* RoughnessMask = AddToFunction<UMaterialExpressionComponentMask>(Function, -200, 450);
		SetMask(RoughnessMask, true, false, false, false);
		UMaterialExpressionComponentMask* MetallicMask = AddToFunction<UMaterialExpressionComponentMask>(Function, -200, 600);
		SetMask(MetallicMask, false, true, false, false);
		Ctx.Connect(SurfaceNode, TEXT(""), RoughnessMask, TEXT(""));
		Ctx.Connect(SurfaceNode, TEXT(""), MetallicMask, TEXT(""));

		auto AddOutput = [Function](const TCHAR* Name, int32 Sort, int32 Y)
		{
			UMaterialExpressionFunctionOutput* Output = AddToFunction<UMaterialExpressionFunctionOutput>(Function, 200, Y);
			Output->OutputName = FName(Name);
			Output->SortPriority = Sort;
			return Output;
		};
		Ctx.Connect(ColorNode, TEXT(""), AddOutput(TEXT("BaseColor"), 0, 0), TEXT(""));
		Ctx.Connect(RoughnessMask, TEXT(""), AddOutput(TEXT("Roughness"), 1, 450), TEXT(""));
		Ctx.Connect(MetallicMask, TEXT(""), AddOutput(TEXT("Metallic"), 2, 600), TEXT(""));
		Ctx.Connect(NormalNode, TEXT(""), AddOutput(TEXT("Normal"), 3, 1000), TEXT(""));

		UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
	}

	// ------------------------------------------------------------ Materials

	void PopulateDebugMaterial(UMaterial* Material, UTexture2D* NoMask, FIcifyContentReport& Report)
	{
		FBuildContext Ctx{ Report, TEXT("M_IcifyMaskDebug") };
		Material->SetShadingModel(MSM_Unlit);

		UMaterialExpressionVertexColor* VertexColor = AddToMaterial<UMaterialExpressionVertexColor>(Material, -900, 0);

		UMaterialExpressionScalarParameter* UseFirst = AddToMaterial<UMaterialExpressionScalarParameter>(Material, -1500, 900);
		UseFirst->ParameterName = Icify::UseUV0ParameterName();
		UseFirst->Group = TEXT("Icify");
		UseFirst->DefaultValue = 0.f;
		UMaterialExpression* MaskUV = AddMaskUV([Material](UClass* Class, int32 X, int32 Y)
		{
			return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
		}, Ctx, UseFirst);
		UMaterialExpressionTextureSampleParameter2D* MaskTexture = AddToMaterial<UMaterialExpressionTextureSampleParameter2D>(Material, -900, 250);
		MaskTexture->ParameterName = Icify::MaskTextureParameterName();
		MaskTexture->Texture = NoMask;
		MaskTexture->SamplerType = SAMPLERTYPE_LinearColor;
		Ctx.Connect(MaskUV, TEXT(""), MaskTexture, TEXT("UVs"));

		auto AddCustomData = [Material](const TCHAR* Name, int32 Offset, int32 Y)
		{
			UMaterialExpressionScalarParameter* Param = AddToMaterial<UMaterialExpressionScalarParameter>(Material, -900, Y);
			Param->ParameterName = FName(Name);
			Param->bUseCustomPrimitiveData = true;
			Param->PrimitiveDataIndex = uint8(CustomDataIndex(Offset));
			return Param;
		};
		UMaterialExpressionScalarParameter* Mode = AddCustomData(TEXT("Icify Mask Mode"), Icify::CPD_MaskMode, 550);
		UMaterialExpressionScalarParameter* Channel = AddCustomData(TEXT("Icify Debug Channel"), Icify::CPD_DebugChannel, 650);
		UMaterialExpressionScalarParameter* Active = AddToMaterial<UMaterialExpressionScalarParameter>(Material, -900, 750);
		Active->ParameterName = Icify::ActiveParameterName();
		Active->Group = TEXT("Icify");
		Active->DefaultValue = 0.f;
		UMaterialExpressionMultiply* OwnedMode = AddToMaterial<UMaterialExpressionMultiply>(Material, -650, 550);
		Ctx.Connect(Mode, TEXT(""), OwnedMode, TEXT("A"));
		Ctx.Connect(Active, TEXT(""), OwnedMode, TEXT("B"));

		UMaterialExpressionCustom* Custom = AddToMaterial<UMaterialExpressionCustom>(Material, -400, 0);
		Custom->Description = TEXT("Icify Mask Debug");
		Custom->OutputType = CMOT_Float3;
		Custom->IncludeFilePaths.Add(IncludePath);
		Custom->Code = TEXT("return IcifyMaskDebug(MaskMode, float4(MaskR, MaskG, MaskB, MaskA), float4(TexR, TexG, TexB, TexA), Channel);");
		SetCustomInputs(Custom, { TEXT("MaskR"), TEXT("MaskG"), TEXT("MaskB"), TEXT("MaskA"), TEXT("TexR"), TEXT("TexG"), TEXT("TexB"), TEXT("TexA"),
			TEXT("MaskMode"), TEXT("Channel") });
		Ctx.Connect(VertexColor, TEXT("R"), Custom, TEXT("MaskR"));
		Ctx.Connect(VertexColor, TEXT("G"), Custom, TEXT("MaskG"));
		Ctx.Connect(VertexColor, TEXT("B"), Custom, TEXT("MaskB"));
		Ctx.Connect(VertexColor, TEXT("A"), Custom, TEXT("MaskA"));
		Ctx.Connect(MaskTexture, TEXT("R"), Custom, TEXT("TexR"));
		Ctx.Connect(MaskTexture, TEXT("G"), Custom, TEXT("TexG"));
		Ctx.Connect(MaskTexture, TEXT("B"), Custom, TEXT("TexB"));
		Ctx.Connect(MaskTexture, TEXT("A"), Custom, TEXT("TexA"));
		Ctx.Connect(OwnedMode, TEXT(""), Custom, TEXT("MaskMode"));
		Ctx.Connect(Channel, TEXT(""), Custom, TEXT("Channel"));

		Ctx.ConnectProperty(Custom, TEXT(""), MP_EmissiveColor, TEXT("Emissive Color"));
	}

	void PopulateExampleMaterial(UMaterial* Material, UMaterialFunction* Function, FIcifyContentReport& Report)
	{
		FBuildContext Ctx{ Report, TEXT("M_IcifyExample") };

		UMaterialExpressionVectorParameter* BaseColor = AddToMaterial<UMaterialExpressionVectorParameter>(Material, -800, 0);
		BaseColor->ParameterName = TEXT("Base Color");
		BaseColor->DefaultValue = FLinearColor(0.5f, 0.5f, 0.5f);

		UMaterialExpressionScalarParameter* Roughness = AddToMaterial<UMaterialExpressionScalarParameter>(Material, -800, 200);
		Roughness->ParameterName = TEXT("Roughness");
		Roughness->DefaultValue = 0.6f;

		UMaterialExpressionScalarParameter* Metallic = AddToMaterial<UMaterialExpressionScalarParameter>(Material, -800, 300);
		Metallic->ParameterName = TEXT("Metallic");
		Metallic->DefaultValue = 0.f;

		UMaterialExpressionScalarParameter* SurfaceOverride = AddToMaterial<UMaterialExpressionScalarParameter>(Material, -800, 400);
		SurfaceOverride->ParameterName = Icify::SurfaceOverrideParameterName();
		SurfaceOverride->DefaultValue = -1.f;

		UMaterialExpressionMaterialFunctionCall* Call = AddToMaterial<UMaterialExpressionMaterialFunctionCall>(Material, -400, 0);
		if (!Call->SetMaterialFunction(Function))
		{
			Report.Errors.Add(TEXT("M_IcifyExample: could not set MF_IcifyFrost on the function call"));
		}

		Ctx.Connect(BaseColor, TEXT(""), Call, TEXT("BaseColor"));
		Ctx.Connect(Roughness, TEXT(""), Call, TEXT("Roughness"));
		Ctx.Connect(Metallic, TEXT(""), Call, TEXT("Metallic"));
		Ctx.Connect(SurfaceOverride, TEXT(""), Call, TEXT("SurfaceOverride"));

		Ctx.ConnectProperty(Call, TEXT("BaseColor"), MP_BaseColor, TEXT("Base Color"));
		Ctx.ConnectProperty(Call, TEXT("Roughness"), MP_Roughness, TEXT("Roughness"));
		Ctx.ConnectProperty(Call, TEXT("Metallic"), MP_Metallic, TEXT("Metallic"));
		Ctx.ConnectProperty(Call, TEXT("Normal"), MP_Normal, TEXT("Normal"));
	}

	// ------------------------------------------------------------ M_IcifyIcicle

	const TArray<const TCHAR*> IcicleInputNames = {
		TEXT("VertexR"), TEXT("VertexG"), TEXT("VertexB"), TEXT("VertexA"), TEXT("UV"), TEXT("WorldPos"),
		TEXT("IceTint"), TEXT("FrostColor"), TEXT("Clarity"), TEXT("Ripples"), TEXT("RippleSize") };

	const TCHAR* IcicleCommonCode =
		TEXT("const float4 VC = float4(VertexR, VertexG, VertexB, VertexA);\n")
		TEXT("const FIcifyIcicle I = IcifyIcicleShade(VC, UV, WorldPos, IceTint.rgb, FrostColor.rgb, Clarity);\n");

	UMaterialExpressionCustom* AddIcicleCustom(UMaterial* Material, FBuildContext& Ctx, const FNodeSources& Sources,
		const TCHAR* Description, ECustomMaterialOutputType OutputType, const FString& Body, int32 X, int32 Y)
	{
		UMaterialExpressionCustom* Custom = AddToMaterial<UMaterialExpressionCustom>(Material, X, Y);
		Custom->Description = Description;
		Custom->OutputType = OutputType;
		Custom->IncludeFilePaths.Add(IncludePath);
		Custom->Code = FString(IcicleCommonCode) + Body;
		SetCustomInputs(Custom, IcicleInputNames);
		ConnectInputs(Custom, Ctx, Sources, IcicleInputNames);
		return Custom;
	}

	/**
	 * Icicles are clear ice: subsurface shading so light glows through thin tips, with the shape's vertex colour
	 * (frost, thickness, root-to-tip, random) and its UVs (around, centimetres down) driving rime, depth and ripples.
	 */
	void PopulateIcicleMaterial(UMaterial* Material, FIcifyContentReport& Report)
	{
		FBuildContext Ctx{ Report, TEXT("M_IcifyIcicle") };
		Material->SetShadingModel(MSM_Subsurface);
		Material->BlendMode = BLEND_Opaque;

		FNodeSources S;
		UMaterialExpressionVertexColor* VertexColor = AddToMaterial<UMaterialExpressionVertexColor>(Material, -1200, 0);
		S.Add(TEXT("VertexR"), VertexColor, TEXT("R"));
		S.Add(TEXT("VertexG"), VertexColor, TEXT("G"));
		S.Add(TEXT("VertexB"), VertexColor, TEXT("B"));
		S.Add(TEXT("VertexA"), VertexColor, TEXT("A"));
		UMaterialExpressionTextureCoordinate* UV = AddToMaterial<UMaterialExpressionTextureCoordinate>(Material, -1200, 200);
		UV->CoordinateIndex = 0;
		S.Add(TEXT("UV"), UV);
		S.Add(TEXT("WorldPos"), AddToMaterial<UMaterialExpressionWorldPosition>(Material, -1200, 300));

		auto AddVector = [Material](const FName& Name, const FLinearColor& Default, int32 Y)
		{
			UMaterialExpressionVectorParameter* Param = AddToMaterial<UMaterialExpressionVectorParameter>(Material, -1200, Y);
			Param->ParameterName = Name;
			Param->Group = TEXT("Icify");
			Param->DefaultValue = Default;
			return Param;
		};
		auto AddParameter = [Material](const TCHAR* Name, float Default, int32 Y)
		{
			UMaterialExpressionScalarParameter* Param = AddToMaterial<UMaterialExpressionScalarParameter>(Material, -1200, Y);
			Param->ParameterName = FName(Name);
			Param->Group = TEXT("Icify");
			Param->DefaultValue = Default;
			return Param;
		};
		S.Add(TEXT("IceTint"), AddVector(Icify::IceTintParameterName(), FLinearColor(0.55f, 0.78f, 0.95f), 400));
		S.Add(TEXT("FrostColor"), AddVector(Icify::FrostColorParameterName(), FLinearColor(0.86f, 0.92f, 1.f), 600));
		S.Add(TEXT("Clarity"), AddParameter(TEXT("Icify Icicle Clarity"), 0.6f, 800));
		S.Add(TEXT("Ripples"), AddParameter(TEXT("Icify Icicle Ripples"), 1.f, 900));
		S.Add(TEXT("RippleSize"), AddParameter(TEXT("Icify Icicle Ripple Size"), 0.7f, 1000));

		UMaterialExpressionCustom* ColorNode = AddIcicleCustom(Material, Ctx, S, TEXT("Icify Icicle Base Color"), CMOT_Float3,
			TEXT("return I.BaseColor;"), -500, 0);
		UMaterialExpressionCustom* SurfaceNode = AddIcicleCustom(Material, Ctx, S, TEXT("Icify Icicle Surface"), CMOT_Float3,
			TEXT("return float3(I.Roughness, I.Specular, I.Opacity);"), -500, 300);
		UMaterialExpressionCustom* SubsurfaceNode = AddIcicleCustom(Material, Ctx, S, TEXT("Icify Icicle Subsurface"), CMOT_Float3,
			TEXT("return I.SubsurfaceColor;"), -500, 600);
		UMaterialExpressionCustom* NormalNode = AddIcicleCustom(Material, Ctx, S, TEXT("Icify Icicle Normal"), CMOT_Float3,
			TEXT("return IcifyIcicleNormal(VC, UV, RippleSize, Ripples);"), -500, 900);

		auto AddChannel = [Material, &Ctx, SurfaceNode](bool bR, bool bG, bool bB, int32 Y)
		{
			UMaterialExpressionComponentMask* Mask = AddToMaterial<UMaterialExpressionComponentMask>(Material, -200, Y);
			SetMask(Mask, bR, bG, bB, false);
			Ctx.Connect(SurfaceNode, TEXT(""), Mask, TEXT(""));
			return Mask;
		};
		UMaterialExpressionComponentMask* RoughnessMask = AddChannel(true, false, false, 250);
		UMaterialExpressionComponentMask* SpecularMask = AddChannel(false, true, false, 350);
		UMaterialExpressionComponentMask* OpacityMask = AddChannel(false, false, true, 450);

		Ctx.ConnectProperty(ColorNode, TEXT(""), MP_BaseColor, TEXT("Base Color"));
		Ctx.ConnectProperty(RoughnessMask, TEXT(""), MP_Roughness, TEXT("Roughness"));
		Ctx.ConnectProperty(SpecularMask, TEXT(""), MP_Specular, TEXT("Specular"));
		Ctx.ConnectProperty(OpacityMask, TEXT(""), MP_Opacity, TEXT("Opacity"));
		Ctx.ConnectProperty(SubsurfaceNode, TEXT(""), MP_SubsurfaceColor, TEXT("Subsurface Color"));
		Ctx.ConnectProperty(NormalNode, TEXT(""), MP_Normal, TEXT("Normal"));
	}

	// ------------------------------------------------------------ Validation

	void CompileAndCheck(UMaterial* Material, FIcifyContentReport& Report)
	{
		if (!IsValid(Material))
		{
			Report.Errors.Add(TEXT("Cannot validate an invalid material"));
			return;
		}
		// RecompileMaterial builds streaming data and can collect garbage. Keep the material alive until both the
		// compile and the resource/error inspection have finished.
		const TStrongObjectPtr<UMaterial> KeepAlive(Material);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		if (FMaterialResource* Resource = ICIFY_GET_MAX_RHI_MATERIAL_RESOURCE(Material))
		{
			Resource->FinishCompilation();
			for (const FString& Error : Resource->GetCompileErrors())
			{
				Report.Errors.Add(FString::Printf(TEXT("%s: %s"), *Material->GetName(), *Error));
			}
		}
		else
		{
			Report.Errors.Add(FString::Printf(TEXT("%s: no material resource; compilation could not be validated"), *Material->GetName()));
		}
	}

	/** Every custom node reads the mask mode through the default-inactive ownership gate, and includes the shader. */
	bool ValidateOwnedMode(TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions, const TCHAR* Asset, FIcifyContentReport& Report)
	{
		UMaterialExpressionScalarParameter* Active = nullptr;
		UMaterialExpressionScalarParameter* Mode = nullptr;
		int32 ActiveCount = 0;
		int32 ModeCount = 0;
		for (UMaterialExpression* Expression : Expressions)
		{
			if (UMaterialExpressionScalarParameter* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression))
			{
				if (Parameter->ParameterName == Icify::ActiveParameterName()) { Active = Parameter; ++ActiveCount; }
				if (Parameter->ParameterName == TEXT("Icify Mask Mode")) { Mode = Parameter; ++ModeCount; }
			}
		}
		if (ActiveCount != 1 || ModeCount != 1 || Active->bUseCustomPrimitiveData || Active->DefaultValue != 0.f || Mode->DefaultValue != 0.f
			|| !Mode->bUseCustomPrimitiveData || Mode->PrimitiveDataIndex != CustomDataIndex(Icify::CPD_MaskMode))
		{
			Report.Errors.Add(FString::Printf(TEXT("%s: missing or invalid default-inactive ownership/mode parameters"), Asset));
			return false;
		}
		UMaterialExpressionMultiply* Gate = nullptr;
		for (UMaterialExpression* Expression : Expressions)
		{
			if (UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
			{
				if (Multiply->A.Expression == Mode && Multiply->B.Expression == Active) { Gate = Multiply; }
			}
		}
		int32 CustomCount = 0;
		for (UMaterialExpression* Expression : Expressions)
		{
			if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression))
			{
				++CustomCount;
				bool bHasOwnedMode = false;
				for (const FCustomInput& Input : Custom->Inputs)
				{
					if (!Input.Input.Expression)
					{
						Report.Errors.Add(FString::Printf(TEXT("%s: %s has an unconnected custom input %s"), Asset, *Custom->Description, *Input.InputName.ToString()));
					}
					if (Input.InputName == TEXT("MaskMode")) { bHasOwnedMode = Gate && Input.Input.Expression == Gate; }
				}
				if (!bHasOwnedMode || !Custom->IncludeFilePaths.Contains(IncludePath))
				{
					Report.Errors.Add(FString::Printf(TEXT("%s: %s bypasses the ownership gate or shader include"), Asset, *Custom->Description));
				}
			}
		}
		if (!Gate || CustomCount == 0)
		{
			Report.Errors.Add(FString::Printf(TEXT("%s: incomplete ownership gate/custom graph"), Asset));
			return false;
		}
		return true;
	}

	void ValidateGeneratedGraphs(UMaterialFunction* Function, UMaterial* Debug, UMaterial* Example, UTexture2D* NoMask, FIcifyContentReport& Report)
	{
		if (!Function || !Debug || !Example || !NoMask)
		{
			Report.Errors.Add(TEXT("One or more generated material/function/texture assets could not be loaded"));
			return;
		}
		ValidateOwnedMode(Function->GetExpressions(), TEXT("MF_IcifyFrost"), Report);
		ValidateOwnedMode(Debug->GetExpressions(), TEXT("M_IcifyMaskDebug"), Report);
		TSet<FName> Outputs;
		UMaterialExpressionFunctionOutput* RoughnessOutput = nullptr;
		UMaterialExpressionFunctionOutput* MetallicOutput = nullptr;
		int32 ValidChannelMasks = 0;
		UMaterialExpressionFunctionInput* SurfaceInput = nullptr;
		UMaterialExpressionScalarParameter* SurfaceFallback = nullptr;
		for (UMaterialExpression* Expression : Function->GetExpressions())
		{
			if (UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
			{
				if (Output->A.Expression) { Outputs.Add(Output->OutputName); }
				if (Output->OutputName == TEXT("Roughness")) { RoughnessOutput = Output; }
				if (Output->OutputName == TEXT("Metallic")) { MetallicOutput = Output; }
			}
			if (UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Expression))
			{
				if (Mask->Input.Expression && !Mask->B && !Mask->A && ((Mask->R && !Mask->G) || (!Mask->R && Mask->G))) { ++ValidChannelMasks; }
			}
			if (UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(Expression))
			{
				if (Input->InputName == TEXT("SurfaceOverride")) { SurfaceInput = Input; }
			}
			if (UMaterialExpressionScalarParameter* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression))
			{
				if (Parameter->ParameterName == Icify::SurfaceOverrideParameterName()) { SurfaceFallback = Parameter; }
			}
		}
		if (Outputs.Num() != 4 || !Outputs.Contains(TEXT("BaseColor")) || !Outputs.Contains(TEXT("Roughness"))
			|| !Outputs.Contains(TEXT("Metallic")) || !Outputs.Contains(TEXT("Normal")) || ValidChannelMasks != 2)
		{
			Report.Errors.Add(TEXT("MF_IcifyFrost: invalid connected outputs or R/G-only channel masks"));
		}
		const UMaterialExpressionComponentMask* RoughnessMask = RoughnessOutput ? Cast<UMaterialExpressionComponentMask>(RoughnessOutput->A.Expression) : nullptr;
		const UMaterialExpressionComponentMask* MetallicMask = MetallicOutput ? Cast<UMaterialExpressionComponentMask>(MetallicOutput->A.Expression) : nullptr;
		const UMaterialExpressionCustom* SurfaceResult = RoughnessMask ? Cast<UMaterialExpressionCustom>(RoughnessMask->Input.Expression) : nullptr;
		if (!RoughnessMask || !MetallicMask || !SurfaceResult || SurfaceResult->Description != TEXT("Icify Roughness Metallic")
			|| SurfaceResult->OutputType != CMOT_Float2 || RoughnessMask->Input.OutputIndex != 0
			|| MetallicMask->Input.Expression != SurfaceResult || MetallicMask->Input.OutputIndex != 0
			|| RoughnessOutput->A.OutputIndex != 0 || MetallicOutput->A.OutputIndex != 0
			|| !RoughnessMask->R || RoughnessMask->G || RoughnessMask->B || RoughnessMask->A
			|| MetallicMask->R || !MetallicMask->G || MetallicMask->B || MetallicMask->A)
		{
			Report.Errors.Add(TEXT("MF_IcifyFrost: Roughness must use R-only and Metallic G-only from the same float2 surface result"));
		}
		if (!SurfaceInput || !SurfaceFallback || SurfaceFallback->bUseCustomPrimitiveData || SurfaceFallback->DefaultValue != -1.f
			|| !SurfaceInput->bUsePreviewValueAsDefault || SurfaceInput->Preview.Expression != SurfaceFallback)
		{
			Report.Errors.Add(TEXT("MF_IcifyFrost: invalid per-material surface override fallback"));
		}
		TArray64<uint8> NoMaskPixels;
		if (NoMask->SRGB || NoMask->CompressionSettings != TC_Default || NoMask->Source.GetSizeX() != 4 || NoMask->Source.GetSizeY() != 4
			|| NoMask->Source.GetFormat() != TSF_BGRA8 || !NoMask->Source.GetMipData(NoMaskPixels, 0) || NoMaskPixels.Num() != 64)
		{
			Report.Errors.Add(TEXT("T_IcifyNoMask: invalid linear RGBA fallback texture"));
		}
		else
		{
			for (uint8 Byte : NoMaskPixels)
			{
				if (Byte != 0) { Report.Errors.Add(TEXT("T_IcifyNoMask: fallback mask is not transparent black")); break; }
			}
		}
	}

	/** Subsurface, with every shading input fed by the shared custom nodes. */
	void ValidateIcicleMaterial(UMaterial* Material, FIcifyContentReport& Report)
	{
		if (!Material)
		{
			Report.Errors.Add(TEXT("M_IcifyIcicle could not be loaded"));
			return;
		}
		if (!Material->GetShadingModels().HasShadingModel(MSM_Subsurface) || Material->BlendMode != BLEND_Opaque)
		{
			Report.Errors.Add(TEXT("M_IcifyIcicle: expected the opaque Subsurface shading model"));
		}
		int32 Customs = 0;
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (const UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression))
			{
				++Customs;
				if (!Custom->IncludeFilePaths.Contains(IncludePath))
				{
					Report.Errors.Add(FString::Printf(TEXT("M_IcifyIcicle: %s doesn't include the shader"), *Custom->Description));
				}
				for (const FCustomInput& Input : Custom->Inputs)
				{
					if (!Input.Input.Expression)
					{
						Report.Errors.Add(FString::Printf(TEXT("M_IcifyIcicle: %s has an unconnected custom input %s"), *Custom->Description, *Input.InputName.ToString()));
					}
				}
			}
		}
		if (Customs != 4)
		{
			Report.Errors.Add(TEXT("M_IcifyIcicle: expected four icicle custom nodes"));
		}
		for (const EMaterialProperty Property : { MP_BaseColor, MP_Roughness, MP_Specular, MP_Opacity, MP_SubsurfaceColor, MP_Normal })
		{
			const FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
			if (!Input || !Input->Expression)
			{
				Report.Errors.Add(FString::Printf(TEXT("M_IcifyIcicle: material input %d is not connected"), int32(Property)));
			}
		}
	}

	/**
	 * Engine plugins are shared: validate their prebuilt content without rebuilding it in place.
	 * BuildPlugin also sets Installed on packaged project plugins; that metadata is not a filesystem write policy.
	 */
	bool CanWritePluginContent(FString& OutWhy)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Icify"));
		if (!Plugin)
		{
			OutWhy = TEXT("plugin not found");
			return false;
		}
		if (Plugin->GetType() == EPluginType::Engine)
		{
			OutWhy = TEXT("Icify is installed in the engine (for example from Fab), so its content is read-only");
			return false;
		}
		for (const TCHAR* Path : GeneratedPaths())
		{
			const FString File = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
			if (FPaths::FileExists(File) && IFileManager::Get().IsReadOnly(*File))
			{
				OutWhy = FString::Printf(TEXT("the plugin's content file is read-only: %s (check it out of source control to rebuild)"), *File);
				return false;
			}
		}
		return true;
	}

	void FillNoMask(UTexture2D* Texture)
	{
		TArray<FColor> Pixels;
		Pixels.Init(FColor(0, 0, 0, 0), 16);
		Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		// Must match the baked mask textures so the Linear Color sampler accepts both.
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_Default;
		Texture->PostEditChange();
	}

	void Register(UObject* Asset, TArray<UPackage*>& Packages)
	{
		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
		Packages.AddUnique(Asset->GetPackage());
	}

	void Touch(UObject* Asset, TArray<UPackage*>& Packages)
	{
		Asset->MarkPackageDirty();
		Packages.AddUnique(Asset->GetPackage());
	}

	bool IsZoneBlueprint(const UBlueprint* Blueprint)
	{
		return Blueprint && Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(AIcifyFreezeZone::StaticClass());
	}
}

bool FIcifyContentBuilder::NeedsWork()
{
	UMaterialFunction* Function = FindExisting<UMaterialFunction>(FunctionPath);
	UMaterial* Debug = FindExisting<UMaterial>(DebugMaterialPath);
	UMaterial* Example = FindExisting<UMaterial>(ExampleMaterialPath);
	UMaterial* Icicle = FindExisting<UMaterial>(IcicleMaterialPath);
	UTexture2D* NoMask = FindExisting<UTexture2D>(NoMaskTexturePath);
	UBlueprint* Blueprint = FindExisting<UBlueprint>(ZoneBlueprintPath);
	if (!Function || !Function->Description.Contains(CurrentMarker()) || !Debug || !Example || !Icicle || !NoMask
		|| !IsZoneBlueprint(Blueprint) || !Blueprint->GeneratedClass || Blueprint->Status == BS_Error)
	{
		return true;
	}
	for (UObject* Asset : TArray<UObject*>{ Function, Debug, Example, Icicle, NoMask, Blueprint })
	{
		if (Asset->GetPackage()->IsDirty()) { return true; }
	}
	FIcifyContentReport Validation;
	ValidateGeneratedGraphs(Function, Debug, Example, NoMask, Validation);
	ValidateIcicleMaterial(Icicle, Validation);
	FString ExpectedToken;
	FString SavedToken;
	return Validation.Errors.Num() > 0 || !MakeReadinessToken(ExpectedToken)
		|| !FFileHelper::LoadFileToString(SavedToken, *ReadinessPath()) || SavedToken != ExpectedToken;
}

FIcifyContentReport FIcifyContentBuilder::Run(bool bForceRebuild)
{
	FIcifyContentReport Report;
	TArray<UPackage*> Packages;
	const bool bRebuild = bForceRebuild || NeedsWork();
	const FString SavedDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Icify"));

	auto Finish = [&]()
	{
		if (!IFileManager::Get().MakeDirectory(*SavedDirectory, true)
			|| !FFileHelper::SaveStringToFile(Report.ToString(), *FPaths::Combine(SavedDirectory, TEXT("ContentReport.txt"))))
		{
			Report.Errors.Add(TEXT("Could not write Saved/Icify/ContentReport.txt"));
		}
		if (Report.HasErrors())
		{
			UE_LOG(LogIcify, Error, TEXT("Icify content build had problems:\n%s"), *Report.ToString());
		}
		else if (Packages.Num() > 0)
		{
			UE_LOG(LogIcify, Log, TEXT("Icify content build:\n%s"), *Report.ToString());
		}
		return Report;
	};

	// Installed (Fab) copies ship prebuilt content and must never be rebuilt in place: validate it and stop.
	FString ReadOnlyReason;
	if (!CanWritePluginContent(ReadOnlyReason))
	{
		Report.Notes.Add(ReadOnlyReason + TEXT("; validating the shipped content instead of rebuilding it"));
		UMaterialFunction* Shipped = FindExisting<UMaterialFunction>(FunctionPath);
		if (!Shipped || !Shipped->Description.Contains(CurrentMarker()))
		{
			Report.Errors.Add(FString::Printf(TEXT("The shipped content was built for different settings than this project's (expected %s). ")
				TEXT("Reset Custom Primitive Data Index and Mask UV Channel in Project Settings > Plugins > Icify to the shipped values, ")
				TEXT("or copy the Icify plugin into this project's Plugins folder so it can be rebuilt."), *CurrentMarker()));
			return Finish();
		}
		ValidateGeneratedGraphs(Shipped, FindExisting<UMaterial>(DebugMaterialPath), FindExisting<UMaterial>(ExampleMaterialPath),
			FindExisting<UTexture2D>(NoMaskTexturePath), Report);
		ValidateIcicleMaterial(FindExisting<UMaterial>(IcicleMaterialPath), Report);
		FString Token;
		if (Report.Errors.Num() == 0 && MakeReadinessToken(Token) && IFileManager::Get().MakeDirectory(*SavedDirectory, true)
			&& FFileHelper::SaveStringToFile(Token, *ReadinessPath()))
		{
			Report.bReady = true;
		}
		return Finish();
	}

	// Invalidate before touching any asset. An interrupted or failed rebuild cannot reuse a prior token.
	if (!IFileManager::Get().MakeDirectory(*SavedDirectory, true)
		|| !FFileHelper::SaveStringToFile(TEXT("unvalidated"), *ReadinessPath()))
	{
		Report.Errors.Add(TEXT("Could not invalidate content readiness; no generated assets were changed"));
		return Finish();
	}

	// The custom nodes include this file; nothing compiles without it.
	const FString Shader = ShaderPath();
	if (Shader.IsEmpty() || !FPaths::FileExists(Shader))
	{
		Report.Errors.Add(FString::Printf(TEXT("Shader include or plugin missing: %s"), *Shader));
		return Finish();
	}

	UTexture2D* NoMask = FindExisting<UTexture2D>(NoMaskTexturePath);
	if (!NoMask)
	{
		NoMask = NewObject<UTexture2D>(CreatePackage(NoMaskTexturePath), *AssetName(NoMaskTexturePath), AssetFlags);
		FillNoMask(NoMask);
		Register(NoMask, Packages);
		Report.Created.Add(AssetName(NoMaskTexturePath));
	}
	else if (bRebuild)
	{
		NoMask->Modify();
		FillNoMask(NoMask);
		Touch(NoMask, Packages);
		Report.Rebuilt.Add(AssetName(NoMaskTexturePath));
	}

	// The generated material assets are rebuilt together whenever the function's marker is stale.
	UMaterialFunction* Function = FindExisting<UMaterialFunction>(FunctionPath);
	if (!Function)
	{
		Function = NewObject<UMaterialFunction>(CreatePackage(FunctionPath), *AssetName(FunctionPath), AssetFlags);
		Register(Function, Packages);
		Report.Created.Add(AssetName(FunctionPath));
	}
	else if (bRebuild)
	{
		Function->Modify();
		if (!ClearFunctionExpressions(Function, Report)) { return Finish(); }
		Touch(Function, Packages);
		Report.Rebuilt.Add(AssetName(FunctionPath));
	}
	if (bRebuild)
	{
		PopulateFunction(Function, NoMask, Report);
	}

	auto PrepareMaterial = [&](const TCHAR* Path) -> UMaterial*
	{
		UMaterial* Material = FindExisting<UMaterial>(Path);
		if (!Material)
		{
			Material = NewObject<UMaterial>(CreatePackage(Path), *AssetName(Path), AssetFlags);
			Register(Material, Packages);
			Report.Created.Add(AssetName(Path));
			return Material;
		}
		if (!bRebuild)
		{
			return nullptr;
		}
		Material->Modify();
		if (!ClearMaterialExpressions(Material, Report)) { return nullptr; }
		Touch(Material, Packages);
		Report.Rebuilt.Add(AssetName(Path));
		return Material;
	};

	if (UMaterial* Debug = PrepareMaterial(DebugMaterialPath))
	{
		PopulateDebugMaterial(Debug, NoMask, Report);
	}
	if (UMaterial* Example = PrepareMaterial(ExampleMaterialPath))
	{
		PopulateExampleMaterial(Example, Function, Report);
	}
	if (UMaterial* Icicle = PrepareMaterial(IcicleMaterialPath))
	{
		PopulateIcicleMaterial(Icicle, Report);
	}

	UBlueprint* Blueprint = FindExisting<UBlueprint>(ZoneBlueprintPath);
	if (Blueprint)
	{
		if (!IsZoneBlueprint(Blueprint))
		{
			Report.Errors.Add(TEXT("BP_FreezeZone exists but does not derive from Icify Freeze Zone"));
		}
	}
	else
	{
		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AIcifyFreezeZone::StaticClass(), CreatePackage(ZoneBlueprintPath), *AssetName(ZoneBlueprintPath), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("IcifyContentBuilder"));
		if (Blueprint)
		{
			Register(Blueprint, Packages);
			Report.Created.Add(AssetName(ZoneBlueprintPath));
		}
		else
		{
			Report.Errors.Add(TEXT("Could not create BP_FreezeZone"));
		}
	}
	if (IsZoneBlueprint(Blueprint))
	{
		if (bRebuild)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			Touch(Blueprint, Packages);
		}
		if (Blueprint->Status == BS_Error || !Blueprint->GeneratedClass)
		{
			Report.Errors.Add(TEXT("BP_FreezeZone did not compile successfully"));
		}
	}

	UMaterial* Debug = FindExisting<UMaterial>(DebugMaterialPath);
	UMaterial* Example = FindExisting<UMaterial>(ExampleMaterialPath);
	UMaterial* Icicle = FindExisting<UMaterial>(IcicleMaterialPath);
	ValidateGeneratedGraphs(Function, Debug, Example, NoMask, Report);
	ValidateIcicleMaterial(Icicle, Report);
	// RecompileMaterial marks the package dirty even if the graph is unchanged. A no-work run already authenticated
	// the saved graphs and shader; keep its packages and readiness genuinely clean.
	if (bRebuild && Debug) { CompileAndCheck(Debug, Report); }
	if (bRebuild && Example) { CompileAndCheck(Example, Report); }
	if (bRebuild && Icicle) { CompileAndCheck(Icicle, Report); }
	if (Report.Errors.Num() > 0)
	{
		Function->Description = PendingDescription();
		Touch(Function, Packages);
		Report.Notes.Add(TEXT("Validation failed: no generated packages were saved and readiness remains false"));
		return Finish();
	}

	// A current marker is granted only to a validated graph. The disk-byte token is granted only after every
	// requested save succeeds, so even a partial save cannot make NeedsWork return false.
	if (bRebuild)
	{
		Function->Description = PendingDescription().Replace(TEXT("[IcifyContent pending validation]"), *CurrentMarker());
		Touch(Function, Packages);
	}
	if (Packages.Num() > 0)
	{
		Report.bSaveAttempted = true;
		Report.bSaved = UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty*/ false);
	}
	if (!Report.bSaved)
	{
		Function->Description = PendingDescription();
		Touch(Function, Packages);
		Report.Errors.Add(TEXT("Generated package save failed; readiness remains false"));
		return Finish();
	}
	FString Token;
	if (!MakeReadinessToken(Token) || !FFileHelper::SaveStringToFile(Token, *ReadinessPath()))
	{
		Report.Errors.Add(TEXT("Could not publish saved content readiness token; readiness remains false"));
		return Finish();
	}
	Report.bReady = true;
	return Finish();
}

#undef LOCTEXT_NAMESPACE
