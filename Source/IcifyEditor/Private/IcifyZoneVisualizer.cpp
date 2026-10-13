// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifyZoneVisualizer.h"

#include "IcifyFreezeZone.h"
#include "IcifyZoneComponent.h"
#include "SceneManagement.h"

namespace
{
	const FLinearColor FrostColor(0.65f, 0.85f, 1.f);
	const FLinearColor SnowfallColor(0.95f, 0.97f, 1.f);
	const FLinearColor MeltColor(0.3f, 0.55f, 1.f);
	const FLinearColor ManualMeltColor(0.2f, 0.9f, 1.f);
	const FLinearColor HeatColor(1.f, 0.45f, 0.1f);

	/** Horizontal rectangle across the zone at a height above its bottom face. */
	void DrawLevel(FPrimitiveDrawInterface* PDI, const FTransform& Zone, const FVector& Extent, float HeightAboveBottom, const FLinearColor& Color)
	{
		const double Z = FMath::Min<double>(-Extent.Z + HeightAboveBottom, Extent.Z);
		const FVector Corners[4] = {
			Zone.TransformPosition(FVector(-Extent.X, -Extent.Y, Z)),
			Zone.TransformPosition(FVector(Extent.X, -Extent.Y, Z)),
			Zone.TransformPosition(FVector(Extent.X, Extent.Y, Z)),
			Zone.TransformPosition(FVector(-Extent.X, Extent.Y, Z)) };
		for (int32 Index = 0; Index < 4; ++Index)
		{
			PDI->DrawLine(Corners[Index], Corners[(Index + 1) % 4], Color, SDPG_World, 1.5f);
		}
	}
}

void FIcifyZoneVisualizer::DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	const UIcifyZoneComponent* ZoneComponent = Cast<UIcifyZoneComponent>(Component);
	const AIcifyFreezeZone* Zone = ZoneComponent ? Cast<AIcifyFreezeZone>(ZoneComponent->GetOwner()) : nullptr;
	if (!Zone)
	{
		return;
	}

	FTransform ZoneTransform = ZoneComponent->GetComponentTransform();
	ZoneTransform.SetScale3D(FVector::OneVector);
	const FVector Extent = ZoneComponent->GetScaledBoxExtent();

	// Measured from the zone bottom here; the real generation measures from traced floors.
	if (Zone->GroundFrost > 0.f)
	{
		DrawLevel(PDI, ZoneTransform, Extent, Zone->GroundFrostHeightCm, FrostColor);
	}

	// Snowfall: a few streaks falling into the zone along the snowfall direction.
	const FVector Fall = Zone->GetSnowfallDirection();
	const double Streak = FMath::Max(Extent.Z * 0.6, 50.0);
	for (const FVector2D& Corner : { FVector2D(-0.5, -0.5), FVector2D(0.5, -0.5), FVector2D(0.5, 0.5), FVector2D(-0.5, 0.5), FVector2D(0.0, 0.0) })
	{
		const FVector Top = ZoneTransform.TransformPosition(FVector(Corner.X * Extent.X, Corner.Y * Extent.Y, Extent.Z));
		PDI->DrawLine(Top - Fall * Streak, Top, SnowfallColor, SDPG_World, 1.f);
	}

	for (const AIcifyFreezeZone::FSourceView& Source : Zone->GetMeltSourceViews())
	{
		const FLinearColor Color = Source.bManual ? ManualMeltColor : MeltColor;
		DrawCircle(PDI, Source.Origin, FVector::ForwardVector, FVector::RightVector, Color, Source.Radius, 16, SDPG_World, 1.5f);
		if (Source.Path.Num() > 1)
		{
			// Traced meltwater: show where it actually runs.
			for (int32 Index = 1; Index < Source.Path.Num(); ++Index)
			{
				PDI->DrawLine(Source.Path[Index - 1], Source.Path[Index], Color, SDPG_World, 2.f);
			}
			continue;
		}
		const float Length = FMath::Max(Source.Length, 10.f);
		const FVector Bottom = Source.Origin - FVector(0.0, 0.0, Length);
		const double BottomRadius = Source.Radius * (1.0 + Zone->DripSpread);
		DrawCircle(PDI, Bottom, FVector::ForwardVector, FVector::RightVector, Color * 0.6f, BottomRadius, 16, SDPG_World, 1.f);
		for (const FVector& Side : { FVector::ForwardVector, -FVector::ForwardVector, FVector::RightVector, -FVector::RightVector })
		{
			PDI->DrawLine(Source.Origin + Side * Source.Radius, Bottom + Side * BottomRadius, Color, SDPG_World, 1.f);
		}
	}

	// Heat sources and how far they thaw (heat rises, so the reach upwards is larger).
	const float Thaw = FMath::Max(Zone->ThawRadiusCm, 10.f);
	for (const FVector& Heat : Zone->GetHeatSourceViews())
	{
		DrawCircle(PDI, Heat, FVector::ForwardVector, FVector::RightVector, HeatColor, Thaw, 24, SDPG_World, 1.5f);
		PDI->DrawLine(Heat - FVector(0.0, 0.0, Thaw / 1.6), Heat + FVector(0.0, 0.0, Thaw / 0.8), HeatColor, SDPG_World, 1.5f);
	}
}
