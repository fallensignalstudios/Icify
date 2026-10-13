// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifyMaterialHookup.h"

#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "IContentBrowserSingleton.h"
#include "IcifyCompat.h"
#include "IcifyFreezeZone.h"
#include "IcifySettings.h"
#include "IcifyTypes.h"
#include "IcifyZoneComponent.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionGetMaterialAttributes.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "RHIGlobals.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "IcifyMaterialHookup"

namespace
{
	const TCHAR* FrostFunctionPath = TEXT("/Icify/Materials/MF_IcifyFrost.MF_IcifyFrost");
	const TCHAR* BackupRoot = TEXT("/Game/Icify/Backups");

	// Tags in the Desc of every node the tool inserts; Remove only ever touches tagged nodes.
	const TCHAR* CallTag = TEXT("IcifyHookup call");
	const TCHAR* ConstantTag = TEXT("IcifyHookup constant");
	const TCHAR* UsedConstantTag = TEXT("IcifyHookup constant (was Use Constant)");
	const TCHAR* AttributesTag = TEXT("IcifyHookup attributes");

	enum class EHookupPath : uint8 { Legacy, Attributes };

	const TCHAR* PathName(EHookupPath Path)
	{
		return Path == EHookupPath::Legacy ? TEXT("legacy") : TEXT("attributes");
	}

	UMaterialFunction* LoadFunction(const TCHAR* Path)
	{
		return LoadObject<UMaterialFunction>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	bool IsIcifyFunction(const UMaterialFunctionInterface* Function)
	{
		return Function && Function->GetPathName() == FrostFunctionPath;
	}

	bool IsTagged(const UMaterialExpression* Expression, const TCHAR* Tag)
	{
		return Expression && Expression->Desc.StartsWith(Tag);
	}

	UMaterialExpressionMaterialFunctionCall* FindInsertedCall(UMaterial* Material)
	{
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
			if (Call && IsTagged(Call, CallTag) && IsIcifyFunction(Call->MaterialFunction))
			{
				return Call;
			}
		}
		return nullptr;
	}

	FExpressionInput* FindCallInput(UMaterialExpressionMaterialFunctionCall* Call, const TCHAR* Name)
	{
		for (FFunctionExpressionInput& Input : Call->FunctionInputs)
		{
			if (Input.ExpressionInput && Input.ExpressionInput->InputName == FName(Name))
			{
				return &Input.Input;
			}
		}
		return nullptr;
	}

	int32 FindCallOutput(const UMaterialExpressionMaterialFunctionCall* Call, const TCHAR* Name)
	{
		for (int32 Index = 0; Index < Call->FunctionOutputs.Num(); ++Index)
		{
			const FFunctionExpressionOutput& Output = Call->FunctionOutputs[Index];
			if (Output.ExpressionOutput && Output.ExpressionOutput->OutputName == FName(Name))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/** Copies only the connection (expression, output index and component mask), never the property's own type. */
	void CopyConnection(FExpressionInput& To, const FExpressionInput& From)
	{
		To.Expression = From.Expression;
		To.OutputIndex = From.OutputIndex;
		To.Mask = From.Mask;
		To.MaskR = From.MaskR;
		To.MaskG = From.MaskG;
		To.MaskB = From.MaskB;
		To.MaskA = From.MaskA;
	}

	void Disconnect(FExpressionInput& Input)
	{
		Input.Expression = nullptr;
		Input.OutputIndex = 0;
		Input.Mask = 0;
		Input.MaskR = Input.MaskG = Input.MaskB = Input.MaskA = 0;
	}

	/** The current legacy adapter cannot retain dormant connections or unconnected pin metadata. */
	template <typename TInput>
	bool HasUnsupportedConnection(const TInput& Input)
	{
		return (Input.UseConstant && Input.Expression)
			|| (!Input.Expression && (Input.OutputIndex != 0 || Input.Mask || Input.MaskR || Input.MaskG || Input.MaskB || Input.MaskA));
	}

	/**
	 * What feeds a legacy property: its connection, or a new tagged constant holding the value the material actually
	 * used (Use Constant, or the engine default when unconnected), so routing through the function changes nothing.
	 * Use Constant itself is only cleared once the property is connected to the function.
	 */
	template <typename TInput>
	FExpressionInput SourceFor(UMaterial* Material, const TInput& Property, EMaterialProperty Which, const FVector4f& ConstantValue, bool bVector, int32 Y)
	{
		FExpressionInput Source;
		if (Property.Expression && !Property.UseConstant)
		{
			CopyConnection(Source, Property);
			return Source;
		}

		const bool bWasConstant = Property.UseConstant != 0;
		const FVector4f Value = bWasConstant ? ConstantValue : FVector4f(FMaterialAttributeDefinitionMap::GetDefaultValue(Which));
		UMaterialExpression* Constant = nullptr;
		if (bVector)
		{
			UMaterialExpressionConstant3Vector* Vector = Cast<UMaterialExpressionConstant3Vector>(
				UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant3Vector::StaticClass(), Material->EditorX - 700, Y));
			if (Vector)
			{
				Vector->Constant = FLinearColor(Value.X, Value.Y, Value.Z);
				Constant = Vector;
			}
		}
		else
		{
			UMaterialExpressionConstant* Scalar = Cast<UMaterialExpressionConstant>(
				UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant::StaticClass(), Material->EditorX - 700, Y));
			if (Scalar)
			{
				Scalar->R = Value.X;
				Constant = Scalar;
			}
		}
		if (Constant)
		{
			Constant->Desc = bWasConstant ? UsedConstantTag : ConstantTag;
			Source.Expression = Constant;
		}
		return Source;
	}

	/** Puts a routed property back from the call input that held its original source. */
	template <typename TInput>
	void RestoreProperty(UMaterial* Material, TInput& Property, const FExpressionInput* Source)
	{
		UMaterialExpression* Feed = Source ? Source->Expression : nullptr;
		if (IsTagged(Feed, ConstantTag))
		{
			Property.UseConstant = IsTagged(Feed, UsedConstantTag) ? 1 : 0;
			Disconnect(Property);
			UMaterialEditingLibrary::DeleteMaterialExpression(Material, Feed);
		}
		else if (Source)
		{
			CopyConnection(Property, *Source);
		}
		else
		{
			Disconnect(Property);
		}
	}

	bool CompileErrors(UMaterial* Material, FString& OutErrors)
	{
		// RecompileMaterial builds streaming data, which runs CollectGarbage. Retain even transient callers.
		TStrongObjectPtr<UMaterial> KeepMaterialAlive(Material);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		FMaterialResource* Resource = ICIFY_GET_MAX_RHI_MATERIAL_RESOURCE(Material);
		if (!Resource)
		{
			OutErrors = TEXT("no material resource was available to verify compilation");
			return true;
		}
		Resource->FinishCompilation();
		OutErrors = FString::Join(Resource->GetCompileErrors(), TEXT("; "));
		return !OutErrors.IsEmpty();
	}

	bool IsWritable(const UMaterial* Material, FString& OutWhy)
	{
		const FString Package = Material->GetPackage()->GetName();
		if (Package.StartsWith(TEXT("/Engine/")) || Package.StartsWith(TEXT("/Script/")))
		{
			OutWhy = TEXT("engine content; duplicate it into your project and use the copy");
			return false;
		}
		if (Package.StartsWith(TEXT("/Icify/")))
		{
			OutWhy = TEXT("Icify's own content already handles ice");
			return false;
		}
		if (Material->GetPackage()->HasAnyPackageFlags(PKG_Cooked))
		{
			OutWhy = TEXT("cooked content can't be edited");
			return false;
		}
		return true;
	}

	/** Landscape materials use Landscape Layer nodes (Landscape module classes, matched by name to avoid the dependency). */
	bool IsLandscapeMaterial(const UMaterial* Material)
	{
		for (const UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (Expression && Expression->GetClass()->GetName().StartsWith(TEXT("MaterialExpressionLandscape")))
			{
				return true;
			}
		}
		return false;
	}

	/** First-time copy of the untouched material; later Adds keep that original backup. */
	UIcifyHookupRecord* FindRecord(UMaterialInterface* Material)
	{
		return Material ? Cast<UIcifyHookupRecord>(Material->GetAssetUserDataOfClass(UIcifyHookupRecord::StaticClass())) : nullptr;
	}

	FString BackUp(UMaterial* Material, FGuid& OutStateId)
	{
		FString Relative = Material->GetPackage()->GetName();
		Relative.RemoveFromStart(TEXT("/Game"));
		const FString PackagePath = FString(BackupRoot) + FPackageName::GetLongPackagePath(Relative);
		const FString Name = Material->GetName() + TEXT("_PreIcify");
		const FString PackageName = PackagePath / Name;
		if (FPackageName::DoesPackageExist(PackageName))
		{
			// Never overwrite a first backup, and do not mistake an unrelated/corrupt asset for one.
			const FString ObjectPath = PackageName + TEXT(".") + Name;
			UMaterial* Existing = LoadObject<UMaterial>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			const UIcifyHookupRecord* Marker = FindRecord(Existing);
			if (!Existing || !Marker || Marker->BackupOf != Material->GetPathName())
			{
				return FString();
			}
			OutStateId = Existing->StateId;
			return PackageName;
		}
		// A previous failed save may have left an unsaved duplicate in memory. It is not a disk backup.
		if (FindPackage(nullptr, *PackageName))
		{
			return FString();
		}
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UObject* Copy = AssetTools.DuplicateAsset(Name, PackagePath, Material);
		if (!Copy)
		{
			return FString();
		}
		TStrongObjectPtr<UObject> KeepCopyAlive(Copy);
		if (UMaterial* CopyMaterial = Cast<UMaterial>(Copy))
		{
			// Mark the copy as this material's backup (and never as an installation of its own).
			CopyMaterial->RemoveUserDataOfClass(UIcifyHookupRecord::StaticClass());
			UIcifyHookupRecord* Marker = NewObject<UIcifyHookupRecord>(CopyMaterial, NAME_None, RF_Transactional);
			Marker->BackupOf = Material->GetPathName();
			Marker->InstalledAt = FDateTime::UtcNow().ToIso8601();
			CopyMaterial->AddAssetUserData(Marker);
			OutStateId = CopyMaterial->StateId;
		}
		return UEditorLoadingAndSavingUtils::SavePackages({ Copy->GetPackage() }, /*bOnlyDirty*/ false)
			? PackageName : FString();
	}

	void CloseEditors(UMaterial* Material)
	{
		// Material editors keep their own copy of the graph; editing underneath them would be lost or clash.
		if (GEditor)
		{
			if (UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				Editors->CloseAllEditorsForAsset(Material);
			}
		}
	}

	TArray<FGuid> RoutedAttributeIds()
	{
		return { FMaterialAttributeDefinitionMap::GetID(MP_BaseColor), FMaterialAttributeDefinitionMap::GetID(MP_Roughness),
			FMaterialAttributeDefinitionMap::GetID(MP_Metallic), FMaterialAttributeDefinitionMap::GetID(MP_Normal) };
	}

	bool InsertLegacy(UMaterial* Material, UMaterialEditorOnlyData* Data, UMaterialFunction* Function, FIcifyHookupResult& Result)
	{
		const int32 X = Material->EditorX - 400;
		const int32 Y = Material->EditorY;
		UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMaterialFunctionCall::StaticClass(), X, Y));
		if (!Call || !Call->SetMaterialFunction(Function))
		{
			Result.Notes = TEXT("could not create the MF_IcifyFrost call");
			return false;
		}

		const bool bNormal = Material->bTangentSpaceNormal;
		Call->Desc = FString::Printf(TEXT("%s legacy%s"), CallTag, bNormal ? TEXT(" normal") : TEXT(""));

		// UE 5.8 stores this input constant in linear colour space already.
		const FLinearColor ColorConstant = Data->BaseColor.Constant;
		const FExpressionInput BaseColor = SourceFor(Material, Data->BaseColor, MP_BaseColor, FVector4f(ColorConstant.R, ColorConstant.G, ColorConstant.B, 1.f), true, Y);
		const FExpressionInput Roughness = SourceFor(Material, Data->Roughness, MP_Roughness, FVector4f(Data->Roughness.Constant, 0.f, 0.f, 0.f), false, Y + 150);
		const FExpressionInput Metallic = SourceFor(Material, Data->Metallic, MP_Metallic, FVector4f(Data->Metallic.Constant, 0.f, 0.f, 0.f), false, Y + 250);
		FExpressionInput* CallBaseColor = FindCallInput(Call, TEXT("BaseColor"));
		FExpressionInput* CallRoughness = FindCallInput(Call, TEXT("Roughness"));
		FExpressionInput* CallMetallic = FindCallInput(Call, TEXT("Metallic"));
		FExpressionInput* CallNormal = FindCallInput(Call, TEXT("Normal"));
		if (!CallBaseColor || !CallRoughness || !CallMetallic || !CallNormal)
		{
			Result.Notes = TEXT("MF_IcifyFrost is missing an input; run Tools > Icify > Update Plugin Content");
			return false;
		}
		CopyConnection(*CallBaseColor, BaseColor);
		CopyConnection(*CallRoughness, Roughness);
		CopyConnection(*CallMetallic, Metallic);

		// A connected property ignores nothing while Use Constant is on, so clear it as each one is routed.
		bool bOk = true;
		if (UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("BaseColor"), MP_BaseColor)) { Data->BaseColor.UseConstant = 0; } else { bOk = false; }
		if (UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("Roughness"), MP_Roughness)) { Data->Roughness.UseConstant = 0; } else { bOk = false; }
		if (UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("Metallic"), MP_Metallic)) { Data->Metallic.UseConstant = 0; } else { bOk = false; }
		if (bNormal)
		{
			// Unconnected normals stay unconnected on the call, whose default is the flat tangent-space normal.
			if (Data->Normal.Expression && !Data->Normal.UseConstant)
			{
				CopyConnection(*CallNormal, Data->Normal);
				bOk = UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("Normal"), MP_Normal) && bOk;
			}
			else if (!Data->Normal.Expression && !Data->Normal.UseConstant)
			{
				bOk = UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("Normal"), MP_Normal) && bOk;
			}
			else
			{
				Result.Notes += TEXT("the normal uses a constant, so it was left alone and the ice has no relief");
			}
		}
		else
		{
			Result.Notes += TEXT("world-space normal material: the normal was left alone, so the ice has no relief");
		}
		return bOk;
	}

	bool InsertAttributes(UMaterial* Material, UMaterialEditorOnlyData* Data, UMaterialFunction* Function, FIcifyHookupResult& Result)
	{
		const int32 X = Material->EditorX;
		const int32 Y = Material->EditorY;
		UMaterialExpressionGetMaterialAttributes* Get = Cast<UMaterialExpressionGetMaterialAttributes>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionGetMaterialAttributes::StaticClass(), X - 900, Y));
		UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMaterialFunctionCall::StaticClass(), X - 600, Y));
		UMaterialExpressionSetMaterialAttributes* Set = Cast<UMaterialExpressionSetMaterialAttributes>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionSetMaterialAttributes::StaticClass(), X - 300, Y));
		if (!Get || !Call || !Set || !Call->SetMaterialFunction(Function))
		{
			Result.Notes = TEXT("could not create the attribute nodes");
			return false;
		}
		Call->Desc = FString::Printf(TEXT("%s attributes-path"), CallTag);
		Get->Desc = AttributesTag;
		Set->Desc = AttributesTag;

		const TArray<FGuid> Ids = RoutedAttributeIds();
		Get->AttributeGetTypes = Ids;
		if (FProperty* Property = FindFProperty<FProperty>(UMaterialExpressionGetMaterialAttributes::StaticClass(), GET_MEMBER_NAME_CHECKED(UMaterialExpressionGetMaterialAttributes, AttributeGetTypes)))
		{
			FPropertyChangedEvent Event(Property);
			Get->PostEditChangeProperty(Event);
		}
		if (Get->GetOutputs().Num() != Ids.Num() + 1)
		{
			Result.Notes = TEXT("Get Material Attributes did not expose the routed attributes");
			return false;
		}

		// Set node: input 0 is the attribute stream, then one input per attribute type.
		Set->Inputs.SetNum(1);
		for (int32 Index = 0; Index < Ids.Num(); ++Index)
		{
			Set->Inputs.AddDefaulted();
		}
		Set->AttributeSetTypes = Ids;

		CopyConnection(Get->MaterialAttributes, Data->MaterialAttributes);
		CopyConnection(Set->Inputs[0], Data->MaterialAttributes);

		const TCHAR* Names[] = { TEXT("BaseColor"), TEXT("Roughness"), TEXT("Metallic"), TEXT("Normal") };
		for (int32 Index = 0; Index < Ids.Num(); ++Index)
		{
			FExpressionInput* CallInput = FindCallInput(Call, Names[Index]);
			const int32 CallOutput = FindCallOutput(Call, Names[Index]);
			if (!CallInput || CallOutput == INDEX_NONE)
			{
				Result.Notes = TEXT("MF_IcifyFrost is missing an input or output; run Tools > Icify > Update Plugin Content");
				return false;
			}
			CallInput->Connect(Index + 1, Get);
			Set->Inputs[Index + 1].Connect(CallOutput, Call);
		}
		Material->Modify();
		Data->MaterialAttributes.Connect(0, Set);
		return true;
	}

	/** The properties an installation may legitimately drive from its own nodes. */
	bool IsRoutedProperty(EMaterialProperty Property)
	{
		return Property == MP_BaseColor || Property == MP_Roughness || Property == MP_Metallic || Property == MP_Normal
			|| Property == MP_MaterialAttributes;
	}

	/**
	 * Artist edits that now depend on the installation's nodes: another node reading one of them, or a property other
	 * than the routed ones connected to one. Removing would silently cut those edits, so Remove refuses instead.
	 */
	TArray<FString> DependentsOn(UMaterial* Material, const TSet<UMaterialExpression*>& Nodes)
	{
		TArray<FString> Out;
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (!Expression || Nodes.Contains(Expression))
			{
				continue;
			}
			for (FExpressionInputIterator It{ Expression }; It; ++It)
			{
				if (It->Expression && Nodes.Contains(It->Expression))
				{
					Out.Add(FString::Printf(TEXT("node %s reads %s"), *Expression->GetName(), *It->Expression->GetName()));
				}
			}
		}
		for (int32 Index = 0; Index < MP_MAX; ++Index)
		{
			const EMaterialProperty Property = EMaterialProperty(Index);
			const FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
			if (Input && Input->Expression && Nodes.Contains(Input->Expression) && !IsRoutedProperty(Property))
			{
				Out.Add(FString::Printf(TEXT("material output %s reads %s"), *FMaterialAttributeDefinitionMap::GetAttributeName(Property), *Input->Expression->GetName()));
			}
		}
		return Out;
	}

	TSet<UMaterialExpression*> LiveNodes(UMaterial* Material, const UIcifyHookupRecord& Record)
	{
		TSet<UMaterialExpression*> Out;
		for (UMaterialExpression* Node : Record.Nodes)
		{
			if (IsValid(Node) && Material->GetExpressions().Contains(Node))
			{
				Out.Add(Node);
			}
		}
		return Out;
	}

	/**
	 * Removes exactly the recorded installation. Each routed property is restored from what currently feeds the
	 * installation's input (so upstream edits made after Add are kept); copies of Icify nodes are left alone.
	 */
	void RemoveRecorded(UMaterial* Material, UMaterialEditorOnlyData* Data, UIcifyHookupRecord& Record, FIcifyHookupResult& Result)
	{
		const TSet<UMaterialExpression*> Nodes = LiveNodes(Material, Record);
		UMaterialExpressionMaterialFunctionCall* Call = nullptr;
		UMaterialExpressionSetMaterialAttributes* Set = nullptr;
		for (UMaterialExpression* Node : Nodes)
		{
			Call = Call ? Call : Cast<UMaterialExpressionMaterialFunctionCall>(Node);
			Set = Set ? Set : Cast<UMaterialExpressionSetMaterialAttributes>(Node);
		}
		Result.Path = Record.Path;

		if (Record.Path.StartsWith(TEXT("attributes")))
		{
			if (Set && Data->MaterialAttributes.Expression == Set && Set->Inputs.Num() > 0)
			{
				CopyConnection(Data->MaterialAttributes, Set->Inputs[0]);
			}
		}
		else if (Call)
		{
			if (Data->BaseColor.Expression == Call) { RestoreProperty(Material, Data->BaseColor, FindCallInput(Call, TEXT("BaseColor"))); }
			if (Data->Roughness.Expression == Call) { RestoreProperty(Material, Data->Roughness, FindCallInput(Call, TEXT("Roughness"))); }
			if (Data->Metallic.Expression == Call) { RestoreProperty(Material, Data->Metallic, FindCallInput(Call, TEXT("Metallic"))); }
			if (Data->Normal.Expression == Call) { RestoreProperty(Material, Data->Normal, FindCallInput(Call, TEXT("Normal"))); }
		}

		// RestoreProperty may already have deleted a recorded constant; delete only what is still there.
		for (UMaterialExpression* Node : Nodes)
		{
			if (IsValid(Node) && Material->GetExpressions().Contains(Node))
			{
				UMaterialEditingLibrary::DeleteMaterialExpression(Material, Node);
			}
		}
		Material->RemoveUserDataOfClass(UIcifyHookupRecord::StaticClass());
		if (!Record.Backup.IsEmpty())
		{
			Result.Backup = Record.Backup;
		}
	}

	/** Undoes whatever the tool inserted, found by its tags (installations without a record). False if nothing was found. */
	bool RemoveInserted(UMaterial* Material, FIcifyHookupResult& Result)
	{
		UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
		UMaterialExpressionMaterialFunctionCall* Call = FindInsertedCall(Material);
		if (!Data || !Call)
		{
			return false;
		}

		if (Call->Desc.Contains(TEXT("attributes-path")))
		{
			Result.Path = PathName(EHookupPath::Attributes);
			TArray<UMaterialExpression*> Tagged;
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (IsTagged(Expression, AttributesTag))
				{
					Tagged.Add(Expression);
				}
			}
			for (UMaterialExpression* Expression : Tagged)
			{
				UMaterialExpressionSetMaterialAttributes* Set = Cast<UMaterialExpressionSetMaterialAttributes>(Expression);
				if (Set && Data->MaterialAttributes.Expression == Set && Set->Inputs.Num() > 0)
				{
					CopyConnection(Data->MaterialAttributes, Set->Inputs[0]);
				}
			}
			for (UMaterialExpression* Expression : Tagged)
			{
				UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
			}
		}
		else
		{
			Result.Path = PathName(EHookupPath::Legacy);
			if (Data->BaseColor.Expression == Call) { RestoreProperty(Material, Data->BaseColor, FindCallInput(Call, TEXT("BaseColor"))); }
			if (Data->Roughness.Expression == Call) { RestoreProperty(Material, Data->Roughness, FindCallInput(Call, TEXT("Roughness"))); }
			if (Data->Metallic.Expression == Call) { RestoreProperty(Material, Data->Metallic, FindCallInput(Call, TEXT("Metallic"))); }
			if (Data->Normal.Expression == Call) { RestoreProperty(Material, Data->Normal, FindCallInput(Call, TEXT("Normal"))); }
			// Constants the tool made for properties that weren't routed after all.
			TArray<UMaterialExpression*> Leftovers;
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (IsTagged(Expression, ConstantTag)) { Leftovers.Add(Expression); }
			}
			for (UMaterialExpression* Expression : Leftovers)
			{
				UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
			}
		}
		UMaterialEditingLibrary::DeleteMaterialExpression(Material, Call);
		return true;
	}

	UMaterial* ResolveBase(UMaterialInterface* Material)
	{
		return Material ? Material->GetMaterial() : nullptr;
	}

	void NotifyHookup(const FText& Message, bool bError)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = bError ? 8.f : 5.f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	/** Runs Add or Remove over a set of materials with progress, then reports. */
	void RunBatch(const TArray<UMaterial*>& Materials, bool bAdd)
	{
		if (Materials.Num() == 0)
		{
			NotifyHookup(LOCTEXT("NothingSelected", "Icify: no materials to change. Select materials or material instances in the Content Browser."), false);
			return;
		}

		TArray<FIcifyHookupResult> Results;
		{
			FScopedSlowTask Task(float(Materials.Num()), bAdd ? LOCTEXT("Adding", "Icify: hooking up materials...") : LOCTEXT("Removing", "Icify: removing from materials..."));
			Task.MakeDialogDelayed(0.3f);
			for (UMaterial* Material : Materials)
			{
				Task.EnterProgressFrame(1.f, FText::FromString(Material->GetName()));
				Results.Add(bAdd ? UIcifyMaterialHookup::AddIcifyToMaterial(Material, true) : UIcifyMaterialHookup::RemoveIcifyFromMaterial(Material));
			}
		}

		TMap<FString, int32> Counts;
		TArray<FString> Lines;
		for (const FIcifyHookupResult& Result : Results)
		{
			Counts.FindOrAdd(Result.Outcome)++;
			Lines.Add(FString::Printf(TEXT("%-8s %-10s %s%s%s"), *Result.Outcome, *Result.Path, *Result.Material,
				Result.Notes.IsEmpty() ? TEXT("") : *(TEXT("  - ") + Result.Notes),
				Result.Backup.IsEmpty() ? TEXT("") : *(TEXT("  (backup ") + Result.Backup + TEXT(")"))));
			UE_LOG(LogIcify, Log, TEXT("Material hookup: %s"), *Lines.Last());
		}
		const FString File = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Icify"), TEXT("MaterialHookup.txt"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
		FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")), *File);

		const int32 Failed = Counts.FindRef(TEXT("failed"));
		const FString Summary = bAdd
			? FString::Printf(TEXT("Icify: hooked up %d material(s), %d already used it, %d skipped, %d failed. Save them to keep the change. Details: Saved/Icify/MaterialHookup.txt"),
				Counts.FindRef(TEXT("added")), Counts.FindRef(TEXT("already")), Counts.FindRef(TEXT("skipped")), Failed)
			: FString::Printf(TEXT("Icify: removed from %d material(s), %d skipped, %d failed. Save them to keep the change. Details: Saved/Icify/MaterialHookup.txt"),
				Counts.FindRef(TEXT("removed")), Counts.FindRef(TEXT("skipped")), Failed);
		NotifyHookup(FText::FromString(Summary), Failed > 0);
	}

	TArray<UMaterial*> SelectedBaseMaterials()
	{
		TArray<FAssetData> Assets;
		FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().GetSelectedAssets(Assets);
		TArray<UMaterial*> Materials;
		for (const FAssetData& Asset : Assets)
		{
			if (UMaterial* Base = ResolveBase(Cast<UMaterialInterface>(Asset.GetAsset())))
			{
				Materials.AddUnique(Base);
			}
		}
		return Materials;
	}
}

bool UIcifyMaterialHookup::UsesIcify(const UMaterialInterface* Material)
{
	float Value = 0.f;
	return Material && Material->GetScalarParameterDefaultValue(FHashedMaterialParameterInfo(Icify::ActiveParameterName()), Value);
}

namespace
{
	/** A lit surface material wired the way the hookup knows (not Substrate, not landscape). */
	bool IsHookableSurface(const UMaterial* Material, const UMaterialEditorOnlyData* Data)
	{
		return Material->MaterialDomain == MD_Surface && !Material->GetShadingModels().IsUnlit() && !Data->FrontMaterial.IsConnected()
			&& !IsLandscapeMaterial(Material);
	}
}

bool UIcifyMaterialHookup::CanHookUp(const UMaterial* Material)
{
	FString Why;
	const UMaterialEditorOnlyData* Data = Material ? Material->GetEditorOnlyData() : nullptr;
	return Data && IsWritable(Material, Why) && IsHookableSurface(Material, Data);
}

bool UIcifyMaterialHookup::NeedsPrivateCopy(const UMaterial* Material)
{
	FString Why;
	const UMaterialEditorOnlyData* Data = Material ? Material->GetEditorOnlyData() : nullptr;
	const UPackage* Package = Material ? Material->GetPackage() : nullptr;
	return Data && Package && !IsWritable(Material, Why) && Package->GetName().StartsWith(TEXT("/Engine/"))
		&& !Package->HasAnyPackageFlags(PKG_Cooked) && IsHookableSurface(Material, Data);
}

FIcifyHookupResult UIcifyMaterialHookup::AddIcifyToMaterial(UMaterialInterface* MaterialInterface, bool bBackup)
{
	FIcifyHookupResult Result;
	UMaterial* Material = ResolveBase(MaterialInterface);
	if (!Material)
	{
		Result.Outcome = TEXT("failed");
		Result.Notes = TEXT("no material");
		return Result;
	}
	TStrongObjectPtr<UMaterial> KeepMaterialAlive(Material);
	Result.Material = Material->GetPathName();
	if (MaterialInterface != Material)
	{
		Result.Notes = FString::Printf(TEXT("changed the parent of %s, so all its instances get ice; "), *MaterialInterface->GetName());
	}

	auto Skip = [&Result](const FString& Why)
	{
		Result.Outcome = TEXT("skipped");
		Result.Notes += Why;
		return Result;
	};

	if (UsesIcify(Material))
	{
		Result.Outcome = TEXT("already");
		return Result;
	}
	FString Why;
	if (!IsWritable(Material, Why))
	{
		return Skip(Why);
	}
	if (Material->MaterialDomain != MD_Surface)
	{
		return Skip(TEXT("not a surface material (decals, post process and UI don't freeze)"));
	}
	if (Material->GetShadingModels().IsUnlit())
	{
		return Skip(TEXT("unlit materials have no roughness or metallic to ice over"));
	}
	UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
	if (!Data)
	{
		return Skip(TEXT("no editor data (cooked?)"));
	}
	if (Data->FrontMaterial.IsConnected())
	{
		return Skip(TEXT("Substrate materials aren't supported yet; use the legacy pins or Material Attributes"));
	}
	if (IsLandscapeMaterial(Material))
	{
		return Skip(TEXT("landscape materials aren't supported yet"));
	}

	EHookupPath Path = EHookupPath::Legacy;
	if (Material->bUseMaterialAttributes)
	{
		Path = EHookupPath::Attributes;
		if (!Data->MaterialAttributes.IsConnected())
		{
			return Skip(TEXT("Use Material Attributes is on but nothing is connected"));
		}
	}
	Result.Path = PathName(Path);
	if (Path == EHookupPath::Legacy && (HasUnsupportedConnection(Data->BaseColor) || HasUnsupportedConnection(Data->Roughness)
		|| HasUnsupportedConnection(Data->Metallic) || HasUnsupportedConnection(Data->Normal)))
	{
		return Skip(TEXT("dormant Use Constant wiring or unconnected pin metadata cannot be restored exactly; material left unchanged"));
	}
	if (Path == EHookupPath::Attributes && !Material->bTangentSpaceNormal)
	{
		return Skip(TEXT("world-space normal attributes are unsupported by the tangent-space ice function; material left unchanged"));
	}

	UMaterialFunction* Function = LoadFunction(FrostFunctionPath);
	if (!Function)
	{
		Result.Outcome = TEXT("failed");
		Result.Notes += TEXT("MF_IcifyFrost is missing; run Tools > Icify > Update Plugin Content");
		return Result;
	}

	FGuid BackupStateId;
	if (bBackup)
	{
		Result.Backup = BackUp(Material, BackupStateId);
		if (Result.Backup.IsEmpty())
		{
			Result.Outcome = TEXT("failed");
			Result.Notes += TEXT("could not create and save a backup copy, so the material was left alone");
			return Result;
		}
	}

	CloseEditors(Material);
	FScopedTransaction Transaction(LOCTEXT("AddIcifyTransaction", "Add Icify to Material"));
	Material->Modify();
	// Everything that exists now is the artist's; whatever appears during insertion is this installation's.
	TSet<UMaterialExpression*> Existing;
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		Existing.Add(Expression);
	}
	const bool bInserted = Path == EHookupPath::Legacy ? InsertLegacy(Material, Data, Function, Result) : InsertAttributes(Material, Data, Function, Result);

	FString Errors;
	if (!bInserted || CompileErrors(Material, Errors))
	{
		// Never leave a half-wired or broken material behind.
		FIcifyHookupResult Ignored;
		RemoveInserted(Material, Ignored);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		Transaction.Cancel();
		Result.Outcome = TEXT("failed");
		Result.Notes += bInserted ? FString::Printf(TEXT("the hooked-up material didn't compile (%s); change reverted"), *Errors) : TEXT("; change reverted");
		return Result;
	}

	// Ownership record: exactly which nodes this installation owns, which properties it routes, and its backup.
	UIcifyHookupRecord* Record = NewObject<UIcifyHookupRecord>(Material, NAME_None, RF_Transactional);
	Record->InstallId = FGuid::NewGuid();
	Record->Path = Result.Path;
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		if (Expression && !Existing.Contains(Expression))
		{
			Record->Nodes.Add(Expression);
		}
	}
	Record->RoutedProperties = Path == EHookupPath::Attributes ? TArray<FString>{ TEXT("MaterialAttributes") }
		: TArray<FString>{ TEXT("BaseColor"), TEXT("Roughness"), TEXT("Metallic"), TEXT("Normal") };
	Record->Backup = Result.Backup;
	Record->BackupStateId = BackupStateId;
	Record->InstalledAt = FDateTime::UtcNow().ToIso8601();
	Material->RemoveUserDataOfClass(UIcifyHookupRecord::StaticClass());
	Material->AddAssetUserData(Record);

	Material->MarkPackageDirty();
	Result.Outcome = TEXT("added");
	return Result;
}

FIcifyHookupResult UIcifyMaterialHookup::RemoveIcifyFromMaterial(UMaterialInterface* MaterialInterface)
{
	FIcifyHookupResult Result;
	UMaterial* Material = ResolveBase(MaterialInterface);
	if (!Material)
	{
		Result.Outcome = TEXT("failed");
		Result.Notes = TEXT("no material");
		return Result;
	}
	TStrongObjectPtr<UMaterial> KeepMaterialAlive(Material);
	Result.Material = Material->GetPathName();
	FString Why;
	if (!IsWritable(Material, Why))
	{
		Result.Outcome = TEXT("skipped");
		Result.Notes = Why;
		return Result;
	}

	CloseEditors(Material);
	FScopedTransaction Transaction(LOCTEXT("RemoveIcifyTransaction", "Remove Icify from Material"));
	Material->Modify();
	// DeleteMaterialExpression also clears incoming links. Snapshot existing nodes for a complete undo.
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		if (Expression) { Expression->Modify(); }
	}
	UIcifyHookupRecord* Record = FindRecord(Material);
	if (Record && Record->BackupOf.IsEmpty())
	{
		UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
		const TArray<FString> Dependents = Data ? DependentsOn(Material, LiveNodes(Material, *Record)) : TArray<FString>();
		if (!Data || Dependents.Num() > 0)
		{
			Transaction.Cancel();
			Result.Outcome = TEXT("skipped");
			Result.Notes = Data
				? FString::Printf(TEXT("artist edits depend on Icify's nodes (%s); disconnect them first, then Remove again"), *FString::Join(Dependents, TEXT("; ")))
				: TEXT("no editor data");
			return Result;
		}
		RemoveRecorded(Material, Data, *Record, Result);
	}
	else if (!RemoveInserted(Material, Result))
	{
		Transaction.Cancel();
		Result.Outcome = TEXT("skipped");
		Result.Notes = UsesIcify(Material)
			? TEXT("Icify was wired in by hand, not by this tool; remove it in the material editor")
			: TEXT("doesn't use Icify");
		return Result;
	}
	FString Errors;
	if (CompileErrors(Material, Errors))
	{
		Result.Notes = FString::Printf(TEXT("nodes removed, but compilation could not be verified (%s); use Undo to restore the hookup"), *Errors);
		Material->MarkPackageDirty();
		Result.Outcome = TEXT("failed");
		return Result;
	}
	Material->MarkPackageDirty();
	Result.Outcome = TEXT("removed");
	return Result;
}

namespace
{
	/**
	 * Static mesh materials on Actor (inside one of Zones, when there are any) that don't use Icify yet and could take it:
	 * in place, or (bCopyOnly) only through a private copy.
	 */
	void AddUnhookedMeshMaterials(const AActor* Actor, const TArray<FBox>& Zones, bool bCopyOnly, TArray<UMaterial*>& Materials)
	{
		TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
		for (const UStaticMeshComponent* Component : Components)
		{
			if (!Component || !Component->IsRegistered())
			{
				continue;
			}
			const FBox Box = Component->Bounds.GetBox();
			if (Zones.Num() > 0 && !Zones.ContainsByPredicate([&Box](const FBox& Zone) { return Zone.Intersect(Box); }))
			{
				continue;
			}
			for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
			{
				UMaterial* Base = ResolveBase(Component->GetMaterial(Slot));
				if (Base && !UIcifyMaterialHookup::UsesIcify(Base)
					&& (bCopyOnly ? UIcifyMaterialHookup::NeedsPrivateCopy(Base) : UIcifyMaterialHookup::CanHookUp(Base)))
				{
					Materials.AddUnique(Base);
				}
			}
		}
	}
}

namespace
{
	/** The level's unhooked materials (inside zones, if there are any). */
	TArray<UMaterial*> LevelMaterials(bool bCopyOnly)
	{
		TArray<UMaterial*> Materials;
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return Materials;
		}

		TArray<FBox> Zones;
		for (TActorIterator<AIcifyFreezeZone> It(World); It; ++It)
		{
			if (const UIcifyZoneComponent* Zone = It->GetZoneComponent())
			{
				Zones.Add(Zone->Bounds.GetBox());
			}
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (!It->IsA<AIcifyFreezeZone>())
			{
				AddUnhookedMeshMaterials(*It, Zones, bCopyOnly, Materials);
			}
		}
		return Materials;
	}

	TArray<UMaterial*> ActorMaterials(const TArray<AActor*>& Actors, bool bCopyOnly)
	{
		TArray<UMaterial*> Materials;
		for (const AActor* Actor : Actors)
		{
			if (Actor && !Actor->IsA<AIcifyFreezeZone>())
			{
				AddUnhookedMeshMaterials(Actor, {}, bCopyOnly, Materials);
			}
		}
		return Materials;
	}

	/** Engine materials can't take Icify in place; says what to do about them instead. Empty when there are none. */
	FString CopyOnlyAdvice(const TArray<UMaterial*>& CopyOnly)
	{
		if (CopyOnly.Num() == 0)
		{
			return FString();
		}
		return FString::Printf(TEXT("%d engine material(s), such as %s, can't be hooked up in place, so meshes using them show no ice. ")
			TEXT("Select those actors and run Freeze Selected Actors: it makes private copies that can."), CopyOnly.Num(), *CopyOnly[0]->GetName());
	}
}

TArray<UMaterial*> UIcifyMaterialHookup::FindUnhookedMaterialsInLevel()
{
	return LevelMaterials(/*bCopyOnly*/ false);
}

TArray<UMaterial*> UIcifyMaterialHookup::FindUnhookedMaterialsOn(const TArray<AActor*>& Actors)
{
	return ActorMaterials(Actors, /*bCopyOnly*/ false);
}

TArray<UMaterial*> UIcifyMaterialHookup::FindCopyOnlyMaterialsOn(const TArray<AActor*>& Actors)
{
	return ActorMaterials(Actors, /*bCopyOnly*/ true);
}

void UIcifyMaterialHookup::HookUpMaterials(const TArray<UMaterial*>& Materials)
{
	RunBatch(Materials, /*bAdd*/ true);
}

void UIcifyMaterialHookup::AddToSelectedAssets()
{
	RunBatch(SelectedBaseMaterials(), /*bAdd*/ true);
}

void UIcifyMaterialHookup::RemoveFromSelectedAssets()
{
	RunBatch(SelectedBaseMaterials(), /*bAdd*/ false);
}

void UIcifyMaterialHookup::HookUpLevel()
{
	const TArray<UMaterial*> Materials = FindUnhookedMaterialsInLevel();
	const FString Advice = CopyOnlyAdvice(LevelMaterials(/*bCopyOnly*/ true));
	if (Materials.Num() == 0)
	{
		NotifyHookup(FText::Format(LOCTEXT("LevelHooked", "Icify: every editable material in the level (inside freeze zones, if any) already uses Icify. {0}"),
			FText::FromString(Advice)), false);
		return;
	}

	TArray<FString> Names;
	for (int32 Index = 0; Index < Materials.Num() && Index < 15; ++Index)
	{
		Names.Add(TEXT("  ") + Materials[Index]->GetPathName());
	}
	if (Materials.Num() > 15)
	{
		Names.Add(FString::Printf(TEXT("  ...and %d more"), Materials.Num() - 15));
	}
	const FText Question = FText::Format(LOCTEXT("ConfirmLevel",
		"Hook up {0} master material(s) so they show Icify ice?\n\n{1}\n\nEach is backed up to {2} first, and every instance of it is affected. "
		"Remove Icify (right-click the material) puts the original wiring back.{3}"),
		Materials.Num(), FText::FromString(FString::Join(Names, TEXT("\n"))), FText::FromString(BackupRoot),
		FText::FromString(Advice.IsEmpty() ? FString() : TEXT("\n\n") + Advice));
	if (FMessageDialog::Open(EAppMsgType::OkCancel, Question) == EAppReturnType::Ok)
	{
		RunBatch(Materials, /*bAdd*/ true);
	}
}

#undef LOCTEXT_NAMESPACE
