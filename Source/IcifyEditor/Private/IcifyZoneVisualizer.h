// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "ComponentVisualizer.h"

/** Draws a selected freeze zone's melt and heat sources, ground frost band and snowfall direction in the viewport. */
class FIcifyZoneVisualizer : public FComponentVisualizer
{
public:
	virtual void DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
};
