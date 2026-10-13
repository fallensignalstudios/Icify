# Engine compatibility: UE 5.5 to 5.8

Icify targets UE 5.5 to 5.8. **It has not been compiled against any engine yet**: it was written without an engine
install. This page lists every engine API it relies on, split into the ones it shares with Grungify (whose code was
verified on UE 5.8.3) and the ones that are new to Icify, which need their first compile.

How to read the confidence column:

- **high**: a stable API that has been in place since 5.3 or earlier, used the same way in Grungify's verified code.
- **medium**: I believe it is right, but it sits in an area that changes, or Icify uses it in a new way.
- **low**: a guess. Believe the compiler, not this table.

The compile switches in `Source/Icify/Public/IcifyCompat.h` are Grungify's, with the same defaults. Flip one from
`Icify.Build.cs` (`PublicDefinitions.Add("ICIFY_...=0")`) if a version disagrees.

---

## 1. Top risks (read first)

| # | Risk | Versions | Confidence | What to do |
|---|---|---|---|---|
| 1 | **Content saved by a newer editor can't be opened by an older one.** `/Icify` content is generated on first launch, so this only matters for a packaged copy that ships prebuilt content. | 5.5 to 5.7 | **high** | Generate and save `Content/` with UE 5.5 (the oldest target) for a packaged release, or per version with `--content-root`. `CheckFabReadiness.py --max-content-engine` checks it. |
| 2 | **Procedural mesh sections and undo.** The icicles are `UProceduralMeshComponent` sections. Their `ProcMeshSections` array is a `UPROPERTY`, so a transaction records it, and the zone marks both icicle components' render state dirty in `PostEditUndo`. Collision cooked for icicles (only with **Icicle Collision** on) isn't rebuilt on undo. | all | **medium** | On first run: Apply, Undo and Redo, then check the icicles in the viewport. Keep collision off unless needed. |
| 3 | **Camera position in a custom node.** `MF_IcifyFrost` feeds `CameraPositionWS` into a custom node (to fade the sparkle and the fine frost crystals with distance), next to `WorldPosition`. Both are LWC values, demoted to `float3` at the node input. Grungify does the same with `WorldPosition` on 5.8.3. | all | **medium** | If a version refuses the LWC input, put a `Transform Position` or `LWC To Float` node in front, or drop the fade (pass 0: full detail at every distance, which can shimmer far away). |
| 4 | **Subsurface opacity.** `M_IcifyIcicle` uses `MSM_Subsurface` with Opacity connected in opaque blend mode. That's long-standing engine behaviour, but Lumen and Substrate projects shade subsurface differently. | all | **medium** | Look at the icicles under Lumen; in a Substrate project, they are converted automatically. |
| 5 | **Strict includes and Clang warnings.** The same story as Grungify: headers stop including each other between versions, and Clang warns where MSVC doesn't (shadowing, unused variables). | all | **medium** | The build scripts pass `-StrictIncludes` and fail on any warning. Add the include the compiler names. |
| 6 | **Saving.** Previews are discarded from `UPackage::PreSavePackageWithContextEvent` (Grungify's verified hook). As a last resort, the zone's own `PreSave` empties preview icicle sections and shows hidden applied ones, which modifies a subobject during the save. | all | **medium** | Save a level with a preview up and check the saved level has no preview icicles. |

---

## 2. Shared with Grungify (verified on UE 5.8.3)

| Area | APIs | Conf. |
|---|---|---|
| Component state | `FStaticMeshComponentLODInfo::OverrideVertexColors`, `ReleaseOverrideVertexColorsAndBlock`, `PaintedVertices`, `SetLODDataCount`, `CachePaintedDataIfNecessary`, `StaticMeshDerivedDataKey`, reflected `FCustomPrimitiveData` lookup, `FComponentReregisterContext`, `UMeshComponent::OverrideMaterials`, `SetMaterial` | high |
| Mask textures | `UTexture::Source.Init(…, TSF_BGRA8, …)`, `SRGB`, `CompressionSettings`, `LODGroup`, `MipGenSettings`, `AddressX/Y` | high |
| Material instances | `UMaterialInstanceConstant::SetParentEditorOnly`, `Set*ParameterValueEditorOnly`, `PostEditChange`, `GetScalarParameterDefaultValue(FHashedMaterialParameterInfo, …)` | high |
| Content builder | `UMaterialEditingLibrary` (create, delete, connect, recompile, update function), `UMaterialExpressionCustom` (`Code`, `Inputs`, `IncludeFilePaths`, `OutputType`), function inputs and outputs, `bUseCustomPrimitiveData`/`PrimitiveDataIndex` parameters, `FKismetEditorUtilities::CreateBlueprint`/`CompileBlueprint`, `UMaterial::GetMaterialResource` via `ICIFY_GET_MAX_RHI_MATERIAL_RESOURCE`, `FMaterialResource::FinishCompilation`/`GetCompileErrors`, readiness token over saved package bytes | high |
| Material hookup | `UMaterialEditorOnlyData` inputs (`BaseColor`, `Roughness`, `Metallic`, `Normal`, `MaterialAttributes`, `FrontMaterial`), Get/Set Material Attributes, `FMaterialAttributeDefinitionMap`, `UAssetUserData` ownership records, `IAssetTools::DuplicateAsset`, `UAssetEditorSubsystem::CloseAllEditorsForAsset` | high |
| Zone | `FScopedTransaction`, `FScopedSlowTask` with Cancel, `TActorIterator`, `AActor::GetDataLayerAssets()`, `GetComponentsBoundingBox`, `UEditorLoadingAndSavingUtils::SavePackages`, `FJsonObjectConverter`, `UWorld::LineTraceSingleByChannel` with complex traces | high |
| Editor module | `UToolMenus` (`LevelEditor.MainMenu.Tools`, `LevelEditor.ActorContextMenu`, `ContentBrowser.AssetContextMenu.Material[InstanceConstant]`), `FComponentVisualizer`, `UPackage::PreSavePackageWithContextEvent`, `ULevel::OnLoadedActorRemovedFromLevelEvent`, `FWorldDelegates`, `UDeveloperSettings::OnSettingChanged` | high |
| Shaders module | `AddShaderSourceDirectoryMapping` at `PostConfigInit` | high |

## 3. New in Icify (verify on first compile)

| API (where used) | 5.5 | 5.6 | 5.7 | 5.8 | Conf. | Notes |
|---|---|---|---|---|---|---|
| `UProceduralMeshComponent` as a default subobject; `CreateMeshSection_LinearColor(int32, Vertices, Triangles, Normals, UV0, VertexColors, TArray<FProcMeshTangent>, bool)`, `ClearAllMeshSections`, `GetNumSections`, `GetProcMeshSection`, `bUseComplexAsSimpleCollision` (`IcifyFreezeZone.cpp`) | OK | OK | OK | OK | medium-high | The plugin has been stable for years. Newer versions add an optional `bSRGBConversion` parameter after `bCreateCollision`; Icify leaves it at its default (linear), which is what the icicle vertex colours need. |
| `FProcMeshTangent(FVector, bool)`, `FProcMeshVertex::Position`, `FProcMeshSection::ProcVertexBuffer` (tests) | OK | OK | OK | OK | high | |
| `"Plugins": [{ "Name": "ProceduralMeshComponent" }]` in the descriptor, module dependency `ProceduralMeshComponent` | OK | OK | OK | OK | high | |
| `UMeshComponent::EmptyOverrideMaterials()` | OK | OK | OK | OK | medium-high | |
| `USceneComponent::GetVisibleFlag()`, `SetVisibility`, `SetUsingAbsoluteLocation/Rotation/Scale`, `SetWorldLocationAndRotation` | OK | OK | OK | OK | high | |
| `UPrimitiveComponent::SetCastShadow`, `SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName / NoCollision_ProfileName)`, `SetCanEverAffectNavigation`, `SetGenerateOverlapEvents` | OK | OK | OK | OK | high | |
| `AActor::PreSave(FObjectPreSaveContext)`, `FObjectPreSaveContext::IsCooking()` | OK | OK | OK | OK | medium-high | |
| `AActor::PostEditUndo()` override | OK | OK | OK | OK | high | |
| `FHitResult::GetActor()`, `GetComponent()`, `FCollisionQueryParams::AddIgnoredActor/AddIgnoredComponent` | OK | OK | OK | OK | high | |
| `UMaterialExpressionCameraPositionWS` into a custom node (`IcifyContentBuilder.cpp`) | ? | ? | ? | ? | medium | Risk 3. |
| `MSM_Subsurface`, `MP_Specular`, `MP_Opacity`, `MP_SubsurfaceColor`, `UMaterial::BlendMode`, `GetShadingModels().HasShadingModel` (`IcifyContentBuilder.cpp`) | OK | OK | OK | OK | high | Risk 4 is about shading, not compilation. |
| `UMaterial::GetExpressionInputForProperty` | OK | OK | OK | OK | high | Grungify's hookup uses it too. |
| `UInstancedStaticMeshComponent`, `USplineMeshComponent` (class checks only) | OK | OK | OK | OK | high | Both are skipped with a report note. |
| `ULocalLightComponent` (thaw from lights) | OK | OK | OK | OK | high | |
| `UMaterialInstanceConstant` created inside a procedural mesh component (icicle palette tint) | OK | OK | OK | OK | high | The same pattern as the owned mesh instances. |

## 4. Shaders

`Scripts/CheckShaders.py` compiles `Icify.ush` and every generated custom node with glslang's HLSL front end. It also
lints for constructs that HLSL 2021 (DXC) and FXC reject but glslang accepts: vector conditions in `?:`/`&&`/`||`,
and scalar-to-vector constructors like `float3(1.0)`. Integer hashing uses `asuint` and `uint` arithmetic only.
What it can't prove is how each RHI compiles it (DXC for D3D12, SPIR-V for Vulkan on Linux, Metal on Mac). Open
`M_IcifyExample`, `M_IcifyIcicle` and `M_IcifyMaskDebug` once on each platform and check the log is clean.
