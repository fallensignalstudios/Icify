// Copyright Fallen Signal Studios. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * One icicle to grow, in world centimetres. The shape is deterministic for a given spec: the same seed always grows
 * the same icicle.
 */
struct FIcifyIcicleSpec
{
	/** Where it hangs from (just under the drip edge). */
	FVector Root = FVector::ZeroVector;
	/** Unit growth direction: gravity, leaned by the wind. */
	FVector Down = FVector(0.0, 0.0, -1.0);
	/** The drip edge's direction: the root flares along it into a bead of ice. Zero = a round root. */
	FVector Along = FVector::ZeroVector;
	float Length = 20.f;
	/** Radius at the root, before the flare. */
	float Radius = 1.5f;
	/** Spacing of the bulges each drip leaves as it freezes. 0 = smooth. Fine ripples are left to the material. */
	float RippleCm = 1.2f;
	/** 0..1: rime on the icicle, carried to the material in vertex colour R. */
	float Frost = 0.f;
	/** 0..1: how much it sags and kinks. */
	float Bend = 0.25f;
	uint32 Seed = 0;
	/** Sides around the icicle (thin icicles get fewer). */
	int32 Sides = 8;
};

/**
 * Geometry for one procedural mesh section, positions relative to an origin.
 * Unreal winding: a front face's (B - A) x (C - A) points into the surface.
 * Vertex colour: R = frost, G = thickness (local radius / 2.5 cm), B = 0 at the root to 1 at the tip, A = per-icicle random.
 * UV0: X = around (0..1), Y = distance down the icicle in cm.
 */
struct ICIFY_API FIcifyIcicleMesh
{
	TArray<FVector> Positions;
	TArray<FVector> Normals;
	TArray<FVector> Tangents;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> Colors;
	TArray<int32> Triangles;

	void Reset();
	int32 NumVertices() const { return Positions.Num(); }
	int32 NumTriangles() const { return Triangles.Num() / 3; }
};

namespace Icify
{
	/** Appends one icicle to Mesh, positions relative to Origin. Returns the vertices added (0 for a degenerate spec). */
	ICIFY_API int32 AppendIcicle(const FIcifyIcicleSpec& Spec, const FVector& Origin, FIcifyIcicleMesh& Mesh);

	/** How many vertices AppendIcicle will add for Spec, for budgeting sections. */
	ICIFY_API int32 IcicleVertexCount(const FIcifyIcicleSpec& Spec);
}
