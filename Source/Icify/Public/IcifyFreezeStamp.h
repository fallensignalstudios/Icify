// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "IcifyTypes.h"
#include "IcifyFreezeStamp.generated.h"

class USphereComponent;

/**
 * Hand-placed, non-destructive touch-up on top of the procedural result: adds (positive) or erases (negative) ice inside a
 * soft ellipsoid, and grows or removes icicles there. Scale the actor to stretch it. Stamps are ordinary level actors, so
 * they survive regeneration, seed changes, Clear -> Apply and presets; every zone that overlaps a stamp applies it.
 * Duplicate (Alt+drag) to paint a stroke.
 */
UCLASS(meta = (DisplayName = "Icify Freeze Stamp"), hidecategories = (Collision, Physics, Replication, Networking, Input, Cooking, HLOD, LOD, Rendering))
class ICIFY_API AIcifyFreezeStamp : public AActor
{
	GENERATED_BODY()

public:
	AIcifyFreezeStamp();

	/** Radius before actor scale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify Stamp", meta = (ClampMin = "1", UIMax = "500", Units = "cm"))
	float RadiusCm = 50.f;

	/** 0 = hard edge, 1 = fades from the centre. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify Stamp", meta = (ClampMin = "0", ClampMax = "1"))
	float Falloff = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify Stamp", meta = (DisplayName = "Layers"))
	FIcifyStampLayers IceLayers;

	/** Break the edge up with noise at the zone's feature scale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icify Stamp", meta = (ClampMin = "0", ClampMax = "1"))
	float EdgeNoise = 0.35f;

	/** Weight (0..1) at a world point, before the per-layer amounts. */
	float WeightAt(const FVector& WorldPoint, float FeatureScaleCm) const;

	/** World-space bounds of the stamp. */
	FBox GetStampBounds() const;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
#endif

protected:
	UPROPERTY(VisibleAnywhere, Category = "Icify Stamp")
	TObjectPtr<USphereComponent> Shape;

private:
	void SyncShape();
	void NotifyZones();
};
