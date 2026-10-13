// Copyright Fallen Signal Studios. All Rights Reserved.

using UnrealBuildTool;

public class Icify : ModuleRules
{
	public Icify(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
			"PhysicsCore",
			// The freeze zone owns the icicle geometry as procedural mesh components.
			"ProceduralMeshComponent"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"JsonUtilities",
			"RenderCore",
			"RHI"
		});

		if (Target.bBuildEditor)
		{
			// Transactions (undo/redo) for Preview / Apply / Clear.
			PrivateDependencyModuleNames.Add("UnrealEd");
			// Creating and saving preset assets.
			PrivateDependencyModuleNames.Add("AssetRegistry");
		}
	}
}
