// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

/**
 * Engine version compatibility helpers for Icify (UE 5.5 - 5.8).
 *
 * Header-only and deliberately light: no engine headers beyond CoreMinimal and Version.h. Wrappers that need an
 * engine type are macros, expanded at the call site, so globals such as GMaxRHIShaderPlatform resolve there.
 *
 * These are the same switches Grungify uses, where they were verified on UE 5.8.3. Each one names the behaviour it
 * selects so it can be flipped from Icify.Build.cs (PublicDefinitions.Add("ICIFY_...=0/1")) if the first compile on
 * another engine version disagrees. See Docs/EngineCompatibility.md.
 */

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"

/** True when compiling against UE Major.Minor or newer. */
#define ICIFY_UE_AT_LEAST(Major, Minor) \
	((ENGINE_MAJOR_VERSION > (Major)) || (ENGINE_MAJOR_VERSION == (Major) && ENGINE_MINOR_VERSION >= (Minor)))

/** True when compiling against an engine older than Major.Minor. */
#define ICIFY_UE_BELOW(Major, Minor) (!ICIFY_UE_AT_LEAST(Major, Minor))

static_assert(ICIFY_UE_AT_LEAST(5, 5), "Icify supports Unreal Engine 5.5 and newer.");

/**
 * 1: UMaterial::GetMaterialResource(EShaderPlatform) (what the 5.8 code uses).
 * 0: UMaterial::GetMaterialResource(ERHIFeatureLevel::Type), the older overload.
 */
#ifndef ICIFY_MATERIAL_RESOURCE_BY_SHADER_PLATFORM
	#define ICIFY_MATERIAL_RESOURCE_BY_SHADER_PLATFORM 1
#endif

#if WITH_EDITOR

/**
 * The material resource for the running RHI's max shader platform (or feature level), or nullptr.
 * A macro so GMaxRHIShaderPlatform / GMaxRHIFeatureLevel resolve at the call site, which includes "RHIGlobals.h".
 */
#if ICIFY_MATERIAL_RESOURCE_BY_SHADER_PLATFORM
	#define ICIFY_GET_MAX_RHI_MATERIAL_RESOURCE(Material) ((Material)->GetMaterialResource(GMaxRHIShaderPlatform))
#else
	#define ICIFY_GET_MAX_RHI_MATERIAL_RESOURCE(Material) ((Material)->GetMaterialResource(GMaxRHIFeatureLevel))
#endif

#endif // WITH_EDITOR
