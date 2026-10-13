// Copyright Fallen Signal Studios. All Rights Reserved.

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

/**
 * Maps the plugin's Shaders folder to /Plugin/Icify so the custom nodes in MF_IcifyFrost, M_IcifyIcicle and
 * M_IcifyMaskDebug can include /Plugin/Icify/Private/Icify.ush. Lives in its own module because it has to load at
 * PostConfigInit, before any material is compiled.
 */
class FIcifyShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Icify"));
		if (!Plugin.IsValid())
		{
			return;
		}

		const FString VirtualPath = TEXT("/Plugin/Icify");
		if (!AllShaderSourceDirectoryMappings().Contains(VirtualPath))
		{
			AddShaderSourceDirectoryMapping(VirtualPath, FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		}
	}
};

IMPLEMENT_MODULE(FIcifyShadersModule, IcifyShaders)
