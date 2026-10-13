// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifyFreezeStamp.h"

#include "Components/SphereComponent.h"
#include "IcifyFreezeZone.h"

#if WITH_EDITOR
#include "EngineUtils.h"
#include "IcifyGenerator.h"
#include "IcifyZoneComponent.h"
#endif

AIcifyFreezeStamp::AIcifyFreezeStamp()
{
	PrimaryActorTick.bCanEverTick = false;

	Shape = CreateDefaultSubobject<USphereComponent>(TEXT("Shape"));
	Shape->SetCollisionProfileName(TEXT("NoCollision"));
	Shape->SetGenerateOverlapEvents(false);
	Shape->SetCanEverAffectNavigation(false);
	Shape->bHiddenInGame = true;
	Shape->ShapeColor = FColor(140, 220, 255);
	RootComponent = Shape;
	SyncShape();
}

void AIcifyFreezeStamp::SyncShape()
{
	if (Shape)
	{
		Shape->SetSphereRadius(RadiusCm);
	}
}

FBox AIcifyFreezeStamp::GetStampBounds() const
{
	const FVector Extent = GetActorScale3D().GetAbs() * RadiusCm;
	return FBox(GetActorLocation() - Extent, GetActorLocation() + Extent);
}

float AIcifyFreezeStamp::WeightAt(const FVector& WorldPoint, float FeatureScaleCm) const
{
#if WITH_EDITOR
	FIcifyStampData Data;
	Data.Transform = GetActorTransform();
	Data.Radius = RadiusCm;
	Data.Falloff = Falloff;
	Data.EdgeNoise = EdgeNoise;
	return Icify::StampWeight(Data, WorldPoint, FeatureScaleCm);
#else
	return 0.f;
#endif
}

#if WITH_EDITOR
void AIcifyFreezeStamp::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	SyncShape();
	if (PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive && !GIsTransacting)
	{
		NotifyZones();
	}
}

void AIcifyFreezeStamp::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	if (bFinished && !GIsTransacting)
	{
		NotifyZones();
	}
}

void AIcifyFreezeStamp::NotifyZones()
{
	// Live-previewing zones that overlap the stamp refresh, just as if their own settings changed.
	UWorld* World = GetWorld();
	if (!World || World->WorldType != EWorldType::Editor)
	{
		return;
	}
	const FBox Bounds = GetStampBounds();
	for (TActorIterator<AIcifyFreezeZone> It(World); It; ++It)
	{
		if (It->bLivePreview && It->GetZoneComponent() && It->GetZoneComponent()->Bounds.GetBox().Intersect(Bounds))
		{
			It->RequestLivePreview();
		}
	}
}
#endif
#if !WITH_EDITOR
void AIcifyFreezeStamp::NotifyZones() {}
#endif
