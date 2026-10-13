// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/BoxComponent.h"
#include "IcifyZoneComponent.generated.h"

/** The box that bounds a freeze zone. Editor-only visual, no collision. */
UCLASS(ClassGroup = Icify, meta = (BlueprintSpawnableComponent))
class ICIFY_API UIcifyZoneComponent : public UBoxComponent
{
	GENERATED_BODY()

public:
	UIcifyZoneComponent(const FObjectInitializer& ObjectInitializer);

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
