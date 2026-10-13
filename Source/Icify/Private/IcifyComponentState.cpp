// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifyGenerator.h"

#if WITH_EDITOR

#include "ComponentReregisterContext.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "IcifySettings.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/ColorVertexBuffer.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshResources.h"
#include "UObject/UnrealType.h"

namespace Icify
{
	namespace
	{
		/**
		 * Locate either reflected CPD struct by its public getter's address. Per-element setters cannot
		 * truncate arrays; default setters also reset live-only values throughout the other array.
		 */
		FCustomPrimitiveData* FindCustomData(UPrimitiveComponent* Component, const FCustomPrimitiveData& Data)
		{
			const void* Target = &Data;
			for (TFieldIterator<FStructProperty> It(UPrimitiveComponent::StaticClass()); It; ++It)
			{
				if (It->Struct && It->Struct->GetFName() == TEXT("CustomPrimitiveData") && It->ContainerPtrToValuePtr<void>(Component) == Target)
				{
					return It->ContainerPtrToValuePtr<FCustomPrimitiveData>(Component);
				}
			}
			return nullptr;
		}

		void SetLODColors(UStaticMeshComponent* Component, int32 LODIndex, const TArray<FColor>& Colors)
		{
			FStaticMeshComponentLODInfo& Info = Component->LODData[LODIndex];
			if (Info.OverrideVertexColors)
			{
				Info.ReleaseOverrideVertexColorsAndBlock();
			}
			Info.OverrideVertexColors = new FColorVertexBuffer;
			Info.OverrideVertexColors->InitFromColorArray(Colors);
			BeginInitResource(Info.OverrideVertexColors);
		}

		void ReleaseLODColors(UStaticMeshComponent* Component, int32 LODIndex)
		{
			if (Component->LODData.IsValidIndex(LODIndex) && Component->LODData[LODIndex].OverrideVertexColors)
			{
				Component->LODData[LODIndex].ReleaseOverrideVertexColorsAndBlock();
			}
		}

		void EnsureLODData(UStaticMeshComponent* Component, int32 NumLODs)
		{
			if (Component->LODData.Num() < NumLODs)
			{
				Component->SetLODDataCount(uint32(NumLODs), uint32(NumLODs));
			}
		}

		/** What a slot shows with no override: the static mesh asset's own material. */
		UMaterialInterface* DefaultMaterialOf(const UMeshComponent* Component, int32 MaterialIndex)
		{
			if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
			{
				const UStaticMesh* Mesh = Static->GetStaticMesh();
				return Mesh ? Mesh->GetMaterial(MaterialIndex) : nullptr;
			}
			return nullptr;
		}

		void RestoreStaticState(UStaticMeshComponent* Component, const FIcifyComponentRecord& Record, const FStaticMeshRenderData* RenderData)
		{
			// Release anything we added beyond the original LOD count, then truncate back to it.
			for (int32 LODIndex = Record.LODDataCount; LODIndex < Component->LODData.Num(); ++LODIndex)
			{
				ReleaseLODColors(Component, LODIndex);
			}
			EnsureLODData(Component, Record.LODDataCount);
			if (Component->LODData.Num() > Record.LODDataCount)
			{
				Component->SetLODDataCount(uint32(Record.LODDataCount), uint32(Record.LODDataCount));
			}

			for (int32 LODIndex = 0; LODIndex < Record.LODDataCount && LODIndex < Record.LODs.Num(); ++LODIndex)
			{
				const FIcifyLODBackup& Backup = Record.LODs[LODIndex];
				const bool bMatchesMesh = RenderData && RenderData->LODResources.IsValidIndex(LODIndex)
					&& Backup.Colors.Num() == int32(RenderData->LODResources[LODIndex].GetNumVertices());

				if (Backup.bHadOverride && bMatchesMesh)
				{
					SetLODColors(Component, LODIndex, Backup.Colors);
				}
				else
				{
					if (Backup.bHadOverride)
					{
						UE_LOG(LogIcify, Warning, TEXT("%s LOD%d: the mesh changed since its colours were backed up; removing them instead of restoring."), *Component->GetPathName(), LODIndex);
					}
					ReleaseLODColors(Component, LODIndex);
				}

#if WITH_EDITORONLY_DATA
				TArray<FPaintedVertex>& Painted = Component->LODData[LODIndex].PaintedVertices;
				Painted.Reset(Backup.PaintedVertices.Num());
				for (const FIcifyPaintedVertex& Copy : Backup.PaintedVertices)
				{
					FPaintedVertex& Restored = Painted.AddDefaulted_GetRef();
					Restored.Position.X = Copy.Position.X;
					Restored.Position.Y = Copy.Position.Y;
					Restored.Position.Z = Copy.Position.Z;
					Restored.Normal.X = Copy.Normal.X;
					Restored.Normal.Y = Copy.Normal.Y;
					Restored.Normal.Z = Copy.Normal.Z;
					Restored.Normal.W = Copy.Normal.W;
					Restored.Color = Copy.Color;
				}
#endif
			}

#if WITH_EDITORONLY_DATA
			Component->StaticMeshDerivedDataKey = Record.StaticMeshDerivedDataKey;
#endif
		}
	}

	const TCHAR* OwnedInstancePrefix()
	{
		return TEXT("IcifyMI_");
	}

	void CaptureRecord(UMeshComponent* Component, FIcifyComponentRecord& OutRecord)
	{
		OutRecord = FIcifyComponentRecord();
		OutRecord.Component = Component;

		// Vertex colours, mesh paint and the derived-data key only exist on static meshes.
		if (UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
		{
			OutRecord.StaticMesh = Static->GetStaticMesh();
			OutRecord.LODDataCount = Static->LODData.Num();
			OutRecord.LODs.SetNum(OutRecord.LODDataCount);

			for (int32 LODIndex = 0; LODIndex < OutRecord.LODDataCount; ++LODIndex)
			{
				const FStaticMeshComponentLODInfo& Info = Static->LODData[LODIndex];
				FIcifyLODBackup& Backup = OutRecord.LODs[LODIndex];

				if (const FColorVertexBuffer* Existing = Info.OverrideVertexColors)
				{
					Backup.bHadOverride = true;
					Backup.Colors.SetNumUninitialized(Existing->GetNumVertices());
					for (uint32 V = 0; V < Existing->GetNumVertices(); ++V)
					{
						Backup.Colors[V] = Existing->VertexColor(V);
					}
				}

#if WITH_EDITORONLY_DATA
				Backup.PaintedVertices.Reserve(Info.PaintedVertices.Num());
				for (const FPaintedVertex& Painted : Info.PaintedVertices)
				{
					FIcifyPaintedVertex& Copy = Backup.PaintedVertices.AddDefaulted_GetRef();
					Copy.Position = FVector(Painted.Position.X, Painted.Position.Y, Painted.Position.Z);
					Copy.Normal = FVector4(Painted.Normal.X, Painted.Normal.Y, Painted.Normal.Z, Painted.Normal.W);
					Copy.Color = Painted.Color;
				}
#endif
			}

#if WITH_EDITORONLY_DATA
			OutRecord.StaticMeshDerivedDataKey = Static->StaticMeshDerivedDataKey;
#endif
		}

		OutRecord.CustomData = Component->GetDefaultCustomPrimitiveData().Data;
		OutRecord.LiveCustomData = Component->GetCustomPrimitiveData().Data;
		OutRecord.CustomDataBase = UIcifySettings::Get()->CustomPrimitiveDataIndex;
		OutRecord.OverrideMaterials = Component->OverrideMaterials;
	}

	bool RestoreRecord(const FIcifyComponentRecord& Record, bool bTransactional)
	{
		UMeshComponent* Component = Record.Component.Get();
		if (!IsValid(Component))
		{
			return false;
		}
		UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component);

		FCustomPrimitiveData* Defaults = FindCustomData(Component, Component->GetDefaultCustomPrimitiveData());
		FCustomPrimitiveData* Live = FindCustomData(Component, Component->GetCustomPrimitiveData());
		if (!Defaults || !Live)
		{
			UE_LOG(LogIcify, Error, TEXT("%s: could not locate both custom primitive data arrays; state and backup retained."), *Component->GetPathName());
			return false;
		}

		const UStaticMesh* Mesh = Static ? Static->GetStaticMesh() : nullptr;
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		if ((Static && !Record.StaticMesh.IsNull() && Record.StaticMesh.Get() != Mesh)
			|| Record.LODs.ContainsByPredicate([RenderData](const FIcifyLODBackup& Backup) { return Backup.bHadOverride && !RenderData; }))
		{
			UE_LOG(LogIcify, Error, TEXT("%s: mesh changed since capture; state and original backup retained."), *Component->GetPathName());
			return false;
		}
		for (int32 LODIndex = 0; LODIndex < Record.LODs.Num(); ++LODIndex)
		{
			const FIcifyLODBackup& Backup = Record.LODs[LODIndex];
			if (Backup.bHadOverride && (!RenderData->LODResources.IsValidIndex(LODIndex)
				|| Backup.Colors.Num() != int32(RenderData->LODResources[LODIndex].GetNumVertices())))
			{
				UE_LOG(LogIcify, Error, TEXT("%s LOD%d: geometry changed since capture; state and original backup retained."), *Component->GetPathName(), LODIndex);
				return false;
			}
		}

		if (bTransactional)
		{
			Component->SetFlags(RF_Transactional);
			Component->Modify();
		}

		{
			// Render state must be torn down while buffers and the material array are swapped.
			FComponentReregisterContext Reregister(Component);

			if (Static)
			{
				RestoreStaticState(Static, Record, RenderData);
			}

			// Use the public material setter for renderer/physics/parameter-cache notifications.
			// Null or absent original overrides resolve to the mesh default, then the raw assignment
			// restores their exact null entries and array length.
			const int32 MaterialCount = FMath::Max(Component->OverrideMaterials.Num(), Record.OverrideMaterials.Num());
			for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
			{
				UMaterialInterface* OriginalMaterial = Record.OverrideMaterials.IsValidIndex(MaterialIndex)
					? Record.OverrideMaterials[MaterialIndex].Get() : nullptr;
				if (!OriginalMaterial)
				{
					OriginalMaterial = DefaultMaterialOf(Component, MaterialIndex);
				}
				Component->SetMaterial(MaterialIndex, OriginalMaterial);
			}
			Component->OverrideMaterials = Record.OverrideMaterials;

			Defaults->Data = Record.CustomData;
			Live->Data = Record.LiveCustomData;
		}
		Component->MarkRenderStateDirty();
		return true;
	}

	bool WriteVertexColors(UStaticMeshComponent* Component, const TArray<TArray<FColor>>& LODColors, const TArray<EIcifySurfaceType>* MaterialSurfaces)
	{
		const UStaticMesh* Mesh = Component->GetStaticMesh();
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		if (!RenderData)
		{
			return false;
		}

		const int32 NumLODs = FMath::Min(LODColors.Num(), RenderData->LODResources.Num());
		if (NumLODs == 0)
		{
			return false;
		}
		TArray<TArray<FColor>> WrittenColors = LODColors;
		for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
		{
			const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];
			if (WrittenColors[LODIndex].Num() != int32(LOD.GetNumVertices()))
			{
				return false;
			}
			if (MaterialSurfaces)
			{
				TArray<uint8> Usage;
				Usage.SetNumZeroed(LOD.GetNumVertices());
				const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
				for (const FStaticMeshSection& Section : LOD.Sections)
				{
					const bool bAdmitted = MaterialSurfaces->IsValidIndex(Section.MaterialIndex)
						&& (*MaterialSurfaces)[Section.MaterialIndex] != EIcifySurfaceType::Auto;
					for (uint32 I = Section.FirstIndex; I < Section.FirstIndex + Section.NumTriangles * 3; ++I)
					{
						if (I >= uint32(Indices.Num()) || !Usage.IsValidIndex(Indices[I]))
						{
							return false;
						}
						Usage[Indices[I]] |= bAdmitted ? 1 : 2;
					}
				}
				const FColorVertexBuffer* Previous = Component->LODData.IsValidIndex(LODIndex)
					? Component->LODData[LODIndex].OverrideVertexColors : nullptr;
				for (int32 V = 0; V < Usage.Num(); ++V)
				{
					if (Usage[V] == 3)
					{
						UE_LOG(LogIcify, Warning, TEXT("%s: admitted/excluded slots share render vertices; use texture masks to preserve excluded paint."), *Component->GetPathName());
						return false;
					}
					if ((Usage[V] & 1) == 0)
					{
						WrittenColors[LODIndex][V] = Previous && Previous->GetNumVertices() == LOD.GetNumVertices()
							? Previous->VertexColor(V)
							: LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices() == LOD.GetNumVertices()
								? LOD.VertexBuffers.ColorVertexBuffer.VertexColor(V) : FColor::White;
					}
				}
			}
		}
		FComponentReregisterContext Reregister(Component);
		EnsureLODData(Component, NumLODs);
		for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
		{
			const TArray<FColor>& Colors = WrittenColors[LODIndex];
			if (Colors.Num() > 0 && Colors.Num() == int32(RenderData->LODResources[LODIndex].GetNumVertices()))
			{
				SetLODColors(Component, LODIndex, Colors);
#if WITH_EDITORONLY_DATA
				// CachePaintedDataIfNecessary deliberately keeps existing caches. These colours need a new cache.
				Component->LODData[LODIndex].PaintedVertices.Reset();
#endif
			}
		}

		// Lets the engine remap the colours if the mesh is reimported. The previous cache and key are in the record.
		Component->StaticMeshDerivedDataKey = RenderData->DerivedDataKey;
		Component->CachePaintedDataIfNecessary();
		return true;
	}

	bool CustomDataSlotsFree(const TArray<float>& OriginalData, const TArray<float>& OriginalLiveData, int32 Base)
	{
		if (Base < 0 || Base > FCustomPrimitiveData::NumCustomPrimitiveDataFloats - CPD_Count)
		{
			return false;
		}
		for (int32 Offset = 0; Offset < CPD_Count; ++Offset)
		{
			if ((OriginalData.IsValidIndex(Base + Offset) && OriginalData[Base + Offset] != 0.f)
				|| (OriginalLiveData.IsValidIndex(Base + Offset) && OriginalLiveData[Base + Offset] != 0.f))
			{
				return false;
			}
		}
		return true;
	}

	bool WriteCustomData(UMeshComponent* Component, const float (&Values)[CPD_Count])
	{
		const int32 Base = UIcifySettings::Get()->CustomPrimitiveDataIndex;
		FCustomPrimitiveData* Defaults = FindCustomData(Component, Component->GetDefaultCustomPrimitiveData());
		FCustomPrimitiveData* Live = FindCustomData(Component, Component->GetCustomPrimitiveData());
		if (!Defaults || !Live || Base < 0 || Base > FCustomPrimitiveData::NumCustomPrimitiveDataFloats - CPD_Count)
		{
			return false;
		}
		if (Defaults->Data.Num() < Base + CPD_Count)
		{
			Defaults->Data.SetNumZeroed(Base + CPD_Count);
		}
		if (Live->Data.Num() < Base + CPD_Count)
		{
			Live->Data.SetNumZeroed(Base + CPD_Count);
		}
		for (int32 Offset = 0; Offset < CPD_Count; ++Offset)
		{
			Defaults->Data[Base + Offset] = Values[Offset];
			Live->Data[Base + Offset] = Values[Offset];
		}
		Component->MarkRenderStateDirty();
		return true;
	}

	UTexture2D* CreateMaskTexture(UMeshComponent* Component, int32 Size, const TArray<FColor>& Pixels)
	{
		if (Size <= 0 || Pixels.Num() != Size * Size)
		{
			return nullptr;
		}

		// Lives inside the component, so it is saved with the actor and disappears with it.
		const FName Name = MakeUniqueObjectName(Component, UTexture2D::StaticClass(), TEXT("IcifyMask"));
		UTexture2D* Texture = NewObject<UTexture2D>(Component, Name, RF_Transactional);
		Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_Default;
		Texture->LODGroup = TEXTUREGROUP_World;
		Texture->MipGenSettings = TMGS_FromTextureGroup;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->PostEditChange();
		return Texture;
	}

	void ApplyPalette(UMaterialInstanceConstant* Instance, const FIcifyPalette& Palette)
	{
		auto Vector = [Instance](bool bOverride, const TCHAR* Name, const FLinearColor& Value)
		{
			if (bOverride)
			{
				Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(Name), Value);
			}
		};
		auto Scalar = [Instance](bool bOverride, const TCHAR* Name, float Value)
		{
			if (bOverride)
			{
				Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Name), Value);
			}
		};
		Vector(Palette.bOverrideFrostColor, TEXT("Icify Frost Color"), Palette.FrostColor);
		Vector(Palette.bOverrideSnowColor, TEXT("Icify Snow Color"), Palette.SnowColor);
		Vector(Palette.bOverrideIceTint, TEXT("Icify Ice Tint"), Palette.IceTint);
		Scalar(Palette.bOverrideStrength, TEXT("Icify Strength"), Palette.Strength);
		Scalar(Palette.bOverrideContrast, TEXT("Icify Contrast"), Palette.Contrast);
		Scalar(Palette.bOverrideFrostStrength, TEXT("Icify Frost Strength"), Palette.FrostStrength);
		Scalar(Palette.bOverrideSnowStrength, TEXT("Icify Snow Strength"), Palette.SnowStrength);
		Scalar(Palette.bOverrideGlazeStrength, TEXT("Icify Glaze Strength"), Palette.GlazeStrength);
		Scalar(Palette.bOverrideBlackIceStrength, TEXT("Icify Black Ice Strength"), Palette.BlackIceStrength);
		Scalar(Palette.bOverrideReliefStrength, TEXT("Icify Relief Strength"), Palette.ReliefStrength);
		Scalar(Palette.bOverrideSparkle, TEXT("Icify Sparkle"), Palette.Sparkle);
		Scalar(Palette.bOverrideFrostCrystalCm, TEXT("Icify Frost Crystal Size"), Palette.FrostCrystalCm);
		Scalar(Palette.bOverrideFernFrost, TEXT("Icify Fern Frost"), Palette.FernFrost);
		Scalar(Palette.bOverrideSnowBreakup, TEXT("Icify Snow Breakup"), Palette.SnowBreakup);
	}

	void ConfigureOwnedInstance(UMaterialInstanceConstant* Instance, UMaterialInterface* Parent, UTexture2D* Texture,
		EIcifySurfaceType Surface, const FIcifyPalette* Palette)
	{
		Instance->SetParentEditorOnly(Parent);
		if (Texture)
		{
			Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(MaskTextureParameterName()), Texture);
		}
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(ActiveParameterName()), 1.f);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(SurfaceOverrideParameterName()), float(int32(Surface) - 1));
		if (Palette)
		{
			ApplyPalette(Instance, *Palette);
		}
		Instance->PostEditChange();
	}

	bool BindOwnedMaterials(UMeshComponent* Component, UTexture2D* Texture, const TArray<EIcifySurfaceType>& MaterialSurfaces,
		UMaterialInterface* ParentOverride, const FIcifyPalette* Palette, bool bUseUV0)
	{
		if (MaterialSurfaces.Num() != Component->GetNumMaterials())
		{
			return false;
		}
		// Validate all admitted parents before changing any raw override array.
		for (int32 Slot = 0; Slot < MaterialSurfaces.Num(); ++Slot)
		{
			if (MaterialSurfaces[Slot] != EIcifySurfaceType::Auto && !ParentOverride && !Component->GetMaterial(Slot))
			{
				return false;
			}
		}
		for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
		{
			if (MaterialSurfaces[Slot] == EIcifySurfaceType::Auto)
			{
				continue;
			}
			UMaterialInterface* Parent = ParentOverride ? ParentOverride : Component->GetMaterial(Slot);
			if (!Parent)
			{
				continue;
			}

			const FName Name = MakeUniqueObjectName(Component, UMaterialInstanceConstant::StaticClass(), *FString::Printf(TEXT("%s%d"), OwnedInstancePrefix(), Slot));
			UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(Component, Name, RF_Transactional);
			ConfigureOwnedInstance(Instance, Parent, Texture, MaterialSurfaces[Slot], Palette);
			if (bUseUV0)
			{
				Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(UseUV0ParameterName()), 1.f);
				Instance->PostEditChange();
			}
			Component->SetMaterial(Slot, Instance);
		}
		return true;
	}
}

#endif // WITH_EDITOR
