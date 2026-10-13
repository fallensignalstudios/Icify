// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifyMaterialHookup.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Editor.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIcifyHookupRoundTripTest, "Icify.Content.MaterialHookupRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIcifyHookupRoundTripTest::RunTest(const FString& Parameters)
{
	// An unsaved project-path material, so the tool treats it as editable; nothing is written to disk.
	// The public compile API collects garbage while building streaming data. Raw C++ locals do not retain UObjects.
	TStrongObjectPtr<UPackage> KeepPackageAlive(CreatePackage(TEXT("/Game/IcifyAutomation/M_HookupRoundTrip")));
	UPackage* Package = KeepPackageAlive.Get();
	TStrongObjectPtr<UMaterial> KeepMaterialAlive(NewObject<UMaterial>(Package, TEXT("M_HookupRoundTrip"), RF_Transient | RF_Transactional));
	UMaterial* Material = KeepMaterialAlive.Get();
	UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
	if (!TestNotNull(TEXT("Editor data"), Data)) { return false; }

	// Base colour from a parameter (with a component mask), roughness as Use Constant, metallic and normal unconnected.
	UMaterialExpressionVectorParameter* Color = Cast<UMaterialExpressionVectorParameter>(
		UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -500, 0));
	Color->ParameterName = TEXT("Tint");
	Data->BaseColor.Connect(0, Color);
	Data->BaseColor.SetMask(1, 1, 1, 1, 0);
	Data->Roughness.UseConstant = 1;
	Data->Roughness.Constant = 0.37f;
	if (!TestNotNull(TEXT("Editor undo service"), GEditor)) { return false; }

	// A Use Constant input can still carry dormant wiring. Refuse before any graph or package mutation.
	const FScalarMaterialInput OriginalRoughness = Data->Roughness;
	Data->Roughness.Connect(3, Color);
	Data->Roughness.SetMask(1, 0, 0, 0, 1);
	Package->SetDirtyFlag(false);
	const int32 BeforeRefusalCount = Material->GetExpressions().Num();
	const FIcifyHookupResult Dormant = UIcifyMaterialHookup::AddIcifyToMaterial(Material, false);
	TestEqual(TEXT("Dormant constant wiring refused"), Dormant.Outcome, FString(TEXT("skipped")));
	TestTrue(TEXT("Dormant expression, index and masks conserved"), Data->Roughness.Expression == Color
		&& Data->Roughness.OutputIndex == 3 && Data->Roughness.UseConstant && Data->Roughness.Mask == 1
		&& !Data->Roughness.MaskR && !Data->Roughness.MaskG && !Data->Roughness.MaskB && Data->Roughness.MaskA
		&& Data->Roughness.Constant == 0.37f);
	TestEqual(TEXT("Refusal creates no nodes"), Material->GetExpressions().Num(), BeforeRefusalCount);
	TestFalse(TEXT("Refusal leaves package clean"), Package->IsDirty());
	Data->Roughness = OriginalRoughness;

	// World-space attribute normals are not compatible with the function's tangent-space normal route.
	UMaterialExpressionMakeMaterialAttributes* Attributes = Cast<UMaterialExpressionMakeMaterialAttributes>(
		UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMakeMaterialAttributes::StaticClass(), -700, 300));
	if (!TestNotNull(TEXT("Real attribute source"), Attributes)) { return false; }
	Material->bUseMaterialAttributes = true;
	Material->bTangentSpaceNormal = false;
	Data->MaterialAttributes.Connect(0, Attributes);
	Package->SetDirtyFlag(false);
	const int32 BeforeAttributesCount = Material->GetExpressions().Num();
	const FIcifyHookupResult WorldAttributes = UIcifyMaterialHookup::AddIcifyToMaterial(Material, false);
	TestEqual(TEXT("World-space attribute normal refused"), WorldAttributes.Outcome, FString(TEXT("skipped")));
	TestTrue(TEXT("Attribute source and normal mode conserved"), Data->MaterialAttributes.Expression == Attributes
		&& Material->bUseMaterialAttributes && !Material->bTangentSpaceNormal);
	TestEqual(TEXT("Attribute refusal creates no nodes"), Material->GetExpressions().Num(), BeforeAttributesCount);
	TestFalse(TEXT("Attribute refusal leaves package clean"), Package->IsDirty());
	UMaterialEditingLibrary::DeleteMaterialExpression(Material, Attributes);
	Material->bUseMaterialAttributes = false;
	Material->bTangentSpaceNormal = true;

	// An in-memory duplicate without a saved package is not a usable backup. No disk write is attempted here.
	TStrongObjectPtr<UPackage> UnsavedBackup(CreatePackage(TEXT("/Game/Icify/Backups/IcifyAutomation/M_HookupRoundTrip_PreIcify")));
	Package->SetDirtyFlag(false);
	const FIcifyHookupResult NoDiskBackup = UIcifyMaterialHookup::AddIcifyToMaterial(Material, true);
	TestEqual(TEXT("Unsaved backup refuses original graph change"), NoDiskBackup.Outcome, FString(TEXT("failed")));
	TestTrue(TEXT("Backup refusal preserves original base colour"), Data->BaseColor.Expression == Color);
	TestEqual(TEXT("Backup refusal creates no nodes"), Material->GetExpressions().Num(), BeforeRefusalCount);
	TestFalse(TEXT("Backup refusal leaves package clean"), Package->IsDirty());
	const int32 OriginalCount = Material->GetExpressions().Num();

	const FIcifyHookupResult Added = UIcifyMaterialHookup::AddIcifyToMaterial(Material, /*bBackup*/ false);
	if (Added.Outcome != TEXT("added"))
	{
		AddError(FString::Printf(TEXT("Add failed: %s %s"), *Added.Outcome, *Added.Notes));
		return false;
	}
	TestEqual(TEXT("Legacy path"), Added.Path, FString(TEXT("legacy")));
	TestTrue(TEXT("Material now uses Icify"), UIcifyMaterialHookup::UsesIcify(Material));
	const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Data->BaseColor.Expression);
	TestNotNull(TEXT("Base colour goes through the function"), Call);
	TestTrue(TEXT("Roughness goes through the function"), Data->Roughness.Expression == Call && !Data->Roughness.UseConstant);
	TestTrue(TEXT("Metallic goes through the function"), Data->Metallic.Expression == Call);
	TestEqual(TEXT("Adding twice is a no-op"), UIcifyMaterialHookup::AddIcifyToMaterial(Material, false).Outcome, FString(TEXT("already")));
	TestTrue(TEXT("Add has an actual undo transaction"), GEditor->UndoTransaction());
	TestTrue(TEXT("Undo Add restores base colour and roughness"), Data->BaseColor.Expression == Color
		&& Data->Roughness.UseConstant && !Data->Roughness.Expression && Data->Roughness.Constant == 0.37f);
	TestEqual(TEXT("Undo Add restores original expression count"), Material->GetExpressions().Num(), OriginalCount);
	TestTrue(TEXT("Add can be redone"), GEditor->RedoTransaction());
	TestTrue(TEXT("Redo Add restores function binding"), Data->BaseColor.Expression == Call && !Data->Roughness.UseConstant);

	const FIcifyHookupResult Removed = UIcifyMaterialHookup::RemoveIcifyFromMaterial(Material);
	TestEqual(TEXT("Removed"), Removed.Outcome, FString(TEXT("removed")));
	TestFalse(TEXT("Material no longer uses Icify"), UIcifyMaterialHookup::UsesIcify(Material));
	TestTrue(TEXT("Base colour wiring and mask restored"), Data->BaseColor.Expression == Color && Data->BaseColor.Mask == 1
		&& Data->BaseColor.MaskR && Data->BaseColor.MaskG && Data->BaseColor.MaskB && !Data->BaseColor.MaskA);
	TestTrue(TEXT("Roughness constant restored"), Data->Roughness.UseConstant && !Data->Roughness.Expression && Data->Roughness.Constant == 0.37f);
	TestNull(TEXT("Metallic unconnected again"), Data->Metallic.Expression);
	TestNull(TEXT("Normal unconnected again"), Data->Normal.Expression);
	TestEqual(TEXT("No inserted nodes left behind"), Material->GetExpressions().Num(), OriginalCount);
	TestTrue(TEXT("Remove has an actual undo transaction"), GEditor->UndoTransaction());
	TestTrue(TEXT("Undo Remove restores function binding"), Data->BaseColor.Expression == Call && !Data->Roughness.UseConstant);
	TestTrue(TEXT("Remove can be redone"), GEditor->RedoTransaction());
	TestTrue(TEXT("Redo Remove restores original wiring and constant"), Data->BaseColor.Expression == Color
		&& Data->Roughness.UseConstant && !Data->Roughness.Expression && Data->Roughness.Constant == 0.37f);
	TestEqual(TEXT("Redo Remove restores original expression count"), Material->GetExpressions().Num(), OriginalCount);

	// Undo records may still reference the fixture. Let their lifetime/GC release it normally rather than marking it garbage.
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIcifyHookupArtistEditsTest, "Icify.Content.MaterialHookupPreservesArtistEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIcifyHookupArtistEditsTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UPackage> KeepPackageAlive(CreatePackage(TEXT("/Game/IcifyAutomation/M_HookupArtistEdits")));
	TStrongObjectPtr<UMaterial> KeepMaterialAlive(NewObject<UMaterial>(KeepPackageAlive.Get(), TEXT("M_HookupArtistEdits"), RF_Transient | RF_Transactional));
	UMaterial* Material = KeepMaterialAlive.Get();
	UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
	if (!TestNotNull(TEXT("Editor data"), Data) || !TestNotNull(TEXT("Editor undo service"), GEditor)) { return false; }

	UMaterialExpressionVectorParameter* Color = Cast<UMaterialExpressionVectorParameter>(
		UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -500, 0));
	Color->ParameterName = TEXT("Tint");
	Data->BaseColor.Connect(0, Color);

	const FIcifyHookupResult Added = UIcifyMaterialHookup::AddIcifyToMaterial(Material, /*bBackup*/ false);
	if (!TestEqual(TEXT("Added"), Added.Outcome, FString(TEXT("added")))) { return false; }
	UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Data->BaseColor.Expression);
	const UIcifyHookupRecord* Record = Cast<UIcifyHookupRecord>(Material->GetAssetUserDataOfClass(UIcifyHookupRecord::StaticClass()));
	if (!TestNotNull(TEXT("Function call"), Call) || !TestNotNull(TEXT("Ownership record"), Record)) { return false; }
	TestTrue(TEXT("Record owns the call"), Record->Nodes.Contains(Call));
	TestFalse(TEXT("Record never owns the artist's nodes"), Record->Nodes.Contains(Color));
	TestTrue(TEXT("Record has an install id"), Record->InstallId.IsValid());

	// Artist edit upstream: a new tint replaces the original feeding the function.
	UMaterialExpressionVectorParameter* NewTint = Cast<UMaterialExpressionVectorParameter>(
		UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -700, 0));
	NewTint->ParameterName = TEXT("New Tint");
	UMaterialEditingLibrary::ConnectMaterialExpressions(NewTint, TEXT(""), Call, TEXT("BaseColor"));

	// Artist edit downstream: emissive now reads the iced colour, so removal would silently cut it.
	UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("BaseColor"), MP_EmissiveColor);
	// Artist copy of the call (same description tag), not connected: never the installation's to delete.
	UMaterialExpression* Copy = UMaterialEditingLibrary::DuplicateMaterialExpression(Material, nullptr, Call);
	// A pasted copy keeps its input links, some to the installation's own constants; reading those would itself be a
	// dependency. Here the artist has disconnected it.
	if (Copy)
	{
		for (FExpressionInputIterator It{ Copy }; It; ++It)
		{
			It->Expression = nullptr;
		}
	}

	const FIcifyHookupResult Refused = UIcifyMaterialHookup::RemoveIcifyFromMaterial(Material);
	TestEqual(TEXT("Remove refuses while an artist edit depends on Icify"), Refused.Outcome, FString(TEXT("skipped")));
	TestTrue(TEXT("Refusal names the dependency"), Refused.Notes.Contains(TEXT("depend")));
	TestTrue(TEXT("Refusal changes nothing"), Data->BaseColor.Expression == Call && Data->EmissiveColor.Expression == Call);

	Data->EmissiveColor.Expression = nullptr;
	const FIcifyHookupResult Removed = UIcifyMaterialHookup::RemoveIcifyFromMaterial(Material);
	TestEqual(TEXT("Removed once nothing depends on it"), Removed.Outcome, FString(TEXT("removed")));
	TestTrue(TEXT("The artist's upstream edit is kept"), Data->BaseColor.Expression == NewTint);
	TestTrue(TEXT("The artist's copy is kept"), Copy && Material->GetExpressions().Contains(Copy));
	TestFalse(TEXT("The installed call is gone"), Material->GetExpressions().Contains(Call));
	TestNull(TEXT("The record is gone"), Material->GetAssetUserDataOfClass(UIcifyHookupRecord::StaticClass()));

	TestTrue(TEXT("Remove can be undone"), GEditor->UndoTransaction());
	TestNotNull(TEXT("Undo brings the record back"), Material->GetAssetUserDataOfClass(UIcifyHookupRecord::StaticClass()));
	TestTrue(TEXT("Undo restores the routing"), Data->BaseColor.Expression == Call);
	return !HasAnyErrors();
}

#endif
