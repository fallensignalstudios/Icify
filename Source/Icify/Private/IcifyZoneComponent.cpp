// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifyZoneComponent.h"

#include "Engine/CollisionProfile.h"
#include "IcifyFreezeZone.h"

UIcifyZoneComponent::UIcifyZoneComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	InitBoxExtent(FVector(400.f, 400.f, 250.f));
	ShapeColor = FColor(90, 200, 255);
	LineThickness = 2.f;
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetGenerateOverlapEvents(false);
	SetCanEverAffectNavigation(false);
	bHiddenInGame = true;
}

#if WITH_EDITOR
void UIcifyZoneComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (AIcifyFreezeZone* Zone = Cast<AIcifyFreezeZone>(GetOwner()))
	{
		Zone->NotifyZoneShapeChanged(PropertyChangedEvent);
	}
}
#endif
