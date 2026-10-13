// Copyright Fallen Signal Studios. All Rights Reserved.

using UnrealBuildTool;

public class IcifyEditor : ModuleRules
{
	public IcifyEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Icify is public: IcifyEditorLibrary.h exposes its types (presets, reports) to Blueprint and Python.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Icify" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"InputCore",
			"UnrealEd",
			"ToolMenus",
			"MaterialEditor",
			"Projects",
			"RHI",
			"AssetRegistry",
			"DeveloperSettings",
			"RenderCore",
			"ContentBrowser",
			"AssetTools",
			"ProceduralMeshComponent"
		});
	}
}
