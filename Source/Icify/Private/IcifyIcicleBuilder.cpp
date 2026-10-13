// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifyIcicleBuilder.h"

void FIcifyIcicleMesh::Reset()
{
	Positions.Reset();
	Normals.Reset();
	Tangents.Reset();
	UVs.Reset();
	Colors.Reset();
	Triangles.Reset();
}

namespace Icify
{
	namespace
	{
		constexpr float TwoPi = 6.28318530718f;
		constexpr float Pi = 3.14159265359f;

		uint32 Mix(uint32 X)
		{
			X ^= X >> 16;
			X *= 0x7feb352du;
			X ^= X >> 15;
			X *= 0x846ca68bu;
			X ^= X >> 16;
			return X;
		}

		/** A deterministic stream of 0..1 values. */
		struct FStream
		{
			uint32 State;

			explicit FStream(uint32 Seed) : State(Mix(Seed ^ 0x51ed270bu)) {}

			float Next()
			{
				State = Mix(State + 0x9e3779b9u);
				return float(State & 0x00FFFFFFu) / 16777216.f;
			}

			float Range(float Min, float Max)
			{
				return Min + (Max - Min) * Next();
			}
		};

		/** Smooth 1D value noise in 0..1. */
		float Noise1(float X, uint32 Seed)
		{
			const float Cell = FMath::FloorToFloat(X);
			const float F = X - Cell;
			const float U = F * F * (3.f - 2.f * F);
			const int32 I = int32(Cell);
			const float A = float(Mix(uint32(I) * 0x8da6b343u ^ Seed) & 0xFFFFu) / 65535.f;
			const float B = float(Mix(uint32(I + 1) * 0x8da6b343u ^ Seed) & 0xFFFFu) / 65535.f;
			return FMath::Lerp(A, B, U);
		}

		/** Icicles are long and thin: a radius beyond a third of the length would fold the flared root over itself. */
		float RadiusFor(const FIcifyIcicleSpec& Spec)
		{
			return FMath::Min(Spec.Radius, Spec.Length * 0.3f);
		}

		int32 SidesFor(const FIcifyIcicleSpec& Spec)
		{
			const int32 Max = FMath::Clamp(Spec.Sides, 4, 16);
			// Thin icicles don't need as many sides to read as round.
			const float Scale = FMath::Clamp(RadiusFor(Spec) / 1.2f, 0.5f, 1.f);
			return FMath::Clamp(FMath::RoundToInt(float(Max) * Scale), 4, Max);
		}

		int32 RingsFor(const FIcifyIcicleSpec& Spec)
		{
			// The silhouette only: about a ring per centimetre on short icicles, at most 24 on long ones.
			// The fine freeze ripples are left to the material.
			const float Step = FMath::Max(Spec.Length / 24.f, 0.8f);
			return FMath::Clamp(FMath::CeilToInt(Spec.Length / Step), 5, 24);
		}

		/** A unit vector perpendicular to Axis, as close to Hint as possible. */
		FVector Perpendicular(const FVector& Axis, const FVector& Hint)
		{
			FVector Result = Hint - Axis * FVector::DotProduct(Hint, Axis);
			if (Result.SizeSquared() < 1e-6)
			{
				const FVector Fallback = FMath::Abs(Axis.Z) < 0.9 ? FVector(0.0, 0.0, 1.0) : FVector(1.0, 0.0, 0.0);
				Result = Fallback - Axis * FVector::DotProduct(Fallback, Axis);
			}
			return Result.GetSafeNormal();
		}
	}

	int32 IcicleVertexCount(const FIcifyIcicleSpec& Spec)
	{
		if (!(Spec.Length > 0.1f) || !(RadiusFor(Spec) > 0.01f))
		{
			return 0;
		}
		// The cap shoulder ring, the root ring and the body rings, each with a split seam; plus the cap centre and the tip.
		return (RingsFor(Spec) + 2) * (SidesFor(Spec) + 1) + 2;
	}

	int32 AppendIcicle(const FIcifyIcicleSpec& Spec, const FVector& Origin, FIcifyIcicleMesh& Mesh)
	{
		const int32 Count = IcicleVertexCount(Spec);
		if (Count == 0)
		{
			return 0;
		}

		const FVector Down = Spec.Down.IsNearlyZero() ? FVector(0.0, 0.0, -1.0) : Spec.Down.GetSafeNormal();
		const float Length = Spec.Length;
		const float Radius = RadiusFor(Spec);
		const int32 Sides = SidesFor(Spec);
		const int32 BodyRings = RingsFor(Spec);
		FStream Rand(Spec.Seed);

		// The shape, all from the seed: how it tapers, how wide the root flares, the bulges, the sag and the lumps.
		const float Taper = Rand.Range(0.7f, 1.f);
		const float Flare = Rand.Range(0.25f, 0.55f);
		const float Wave = Spec.RippleCm > 0.f ? FMath::Max(Spec.RippleCm, Length / 12.f) : 0.f;
		const float RippleAmount = Wave > 0.f ? Rand.Range(0.06f, 0.12f) : 0.f;
		const float RipplePhase = Rand.Range(0.f, TwoPi);
		const float BendAmount = FMath::Clamp(Spec.Bend, 0.f, 1.f) * Rand.Range(0.4f, 1.f);
		const float SagAngle = Rand.Range(0.f, TwoPi);
		const float WobbleFrequency = Rand.Range(0.8f, 1.6f);
		const float Lump2 = Rand.Range(0.02f, 0.05f);
		const float Lump3 = Rand.Range(0.015f, 0.035f);
		const float Phase2 = Rand.Range(0.f, TwoPi);
		const float Phase3 = Rand.Range(0.f, TwoPi);
		const float Random = Rand.Next();
		const uint32 LumpSeed = Mix(Spec.Seed ^ 0xa511e9b3u);

		const bool bBead = !Spec.Along.IsNearlyZero();
		const FVector AlongAxis = Perpendicular(Down, bBead ? Spec.Along : FVector(1.0, 0.0, 0.0));
		const FVector Side = FVector::CrossProduct(Down, AlongAxis).GetSafeNormal();
		const FVector SagDirection = (AlongAxis * FMath::Cos(SagAngle) + Side * FMath::Sin(SagAngle)).GetSafeNormal();
		const FVector WobbleDirection = FVector::CrossProduct(Down, SagDirection).GetSafeNormal();

		auto Center = [&](float T) -> FVector
		{
			const FVector Sag = SagDirection * (BendAmount * 0.09f * Length * T * T)
				+ WobbleDirection * (BendAmount * 0.025f * Length * FMath::Sin(Pi * T * WobbleFrequency));
			return Spec.Root + Down * (T * Length) + Sag;
		};
		auto RadiusAt = [&](float T) -> float
		{
			const float Tc = FMath::Clamp(T, 0.f, 1.f);
			const float Along = Tc * Length;
			float Value = Radius * FMath::Pow(1.f - Tc, Taper);
			// A wide root where it's fused to the edge.
			Value *= 1.f + Flare * (1.f - FMath::SmoothStep(0.f, 0.12f, Tc));
			if (Wave > 0.f)
			{
				Value *= 1.f + RippleAmount * FMath::Sin(TwoPi * Along / Wave + RipplePhase) * (0.35f + 0.65f * FMath::SmoothStep(0.f, 0.3f, Tc));
			}
			Value *= 1.f + 0.16f * (Noise1(Along / 3.f, LumpSeed) - 0.5f);
			return FMath::Max(Value, 0.04f);
		};
		// Along the edge the root stretches into a bead that merges with its neighbours'.
		auto StretchAt = [&](float T) -> float
		{
			return bBead ? 1.f + 0.7f * (1.f - FMath::SmoothStep(0.f, 0.15f, FMath::Max(T, 0.f))) : 1.f;
		};

		const int32 Base = Mesh.Positions.Num();
		const int32 Stride = Sides + 1;
		const int32 NumRings = BodyRings + 2;
		auto Vertex = [Base, Stride](int32 Ring, int32 J) { return Base + Ring * Stride + J; };

		auto AddRing = [&](float T, float RingRadius, float Stretch)
		{
			const FVector C = Center(T);
			const FVector Tangent = (Center(T + 0.01f) - Center(T - 0.01f)).GetSafeNormal();
			const FVector X = Perpendicular(Tangent, AlongAxis);
			const FVector Y = FVector::CrossProduct(Tangent, X);
			const float Tc = FMath::Clamp(T, 0.f, 1.f);
			const FLinearColor Color(Spec.Frost * (1.f - 0.6f * Tc), FMath::Clamp(RingRadius / 2.5f, 0.f, 1.f), Tc, Random);
			for (int32 J = 0; J <= Sides; ++J)
			{
				const float Theta = TwoPi * float(J % Sides) / float(Sides);
				const float Cos = FMath::Cos(Theta);
				const float Sin = FMath::Sin(Theta);
				const float Irregular = 1.f + Lump2 * FMath::Sin(2.f * Theta + Phase2) + Lump3 * FMath::Sin(3.f * Theta + Phase3);
				const FVector Radial = X * (Cos * RingRadius * Stretch * Irregular) + Y * (Sin * RingRadius * Irregular);
				Mesh.Positions.Add(C + Radial - Origin);
				Mesh.Normals.Add(FVector::ZeroVector);
				Mesh.Tangents.Add((Y * Cos - X * Sin).GetSafeNormal());
				Mesh.UVs.Add(FVector2D(float(J) / float(Sides), T * Length));
				Mesh.Colors.Add(Color);
			}
		};

		// Ring 0 is the shoulder of the cap buried in the edge, ring 1 the root, then the body down to just above the tip.
		const float Shoulder = -0.6f * Radius / Length;
		AddRing(Shoulder, Radius * (1.f + Flare) * 0.7f, StretchAt(0.f));
		AddRing(0.f, RadiusAt(0.f), StretchAt(0.f));
		for (int32 Ring = 1; Ring <= BodyRings; ++Ring)
		{
			const float T = 0.97f * float(Ring) / float(BodyRings);
			AddRing(T, RadiusAt(T), StretchAt(T));
		}

		// Cap centre (inside the edge) and tip.
		const int32 Cap = Mesh.Positions.Num();
		Mesh.Positions.Add(Center(-1.1f * Radius / Length) - Origin);
		Mesh.Normals.Add(FVector::ZeroVector);
		Mesh.Tangents.Add(AlongAxis);
		Mesh.UVs.Add(FVector2D(0.5f, -1.1f * Radius));
		Mesh.Colors.Add(FLinearColor(Spec.Frost, FMath::Clamp(Radius / 2.5f, 0.f, 1.f), 0.f, Random));
		const int32 Tip = Mesh.Positions.Num();
		Mesh.Positions.Add(Center(1.f) - Origin);
		Mesh.Normals.Add(FVector::ZeroVector);
		Mesh.Tangents.Add(AlongAxis);
		Mesh.UVs.Add(FVector2D(0.5f, Length));
		Mesh.Colors.Add(FLinearColor(Spec.Frost * 0.4f, 0.f, 1.f, Random));

		// Unreal winding (front faces have (B - A) x (C - A) pointing inwards): ring to ring, then the two fans.
		const int32 FirstIndex = Mesh.Triangles.Num();
		for (int32 Ring = 0; Ring + 1 < NumRings; ++Ring)
		{
			for (int32 J = 0; J < Sides; ++J)
			{
				const int32 A0 = Vertex(Ring, J);
				const int32 A1 = Vertex(Ring, J + 1);
				const int32 B0 = Vertex(Ring + 1, J);
				const int32 B1 = Vertex(Ring + 1, J + 1);
				Mesh.Triangles.Append({ A0, B0, A1, A1, B0, B1 });
			}
		}
		for (int32 J = 0; J < Sides; ++J)
		{
			Mesh.Triangles.Append({ Cap, Vertex(0, J), Vertex(0, J + 1) });
			Mesh.Triangles.Append({ Tip, Vertex(NumRings - 1, J + 1), Vertex(NumRings - 1, J) });
		}

		// Smooth normals from the faces (area weighted). The cross product points inwards, so its negative is outward.
		for (int32 Index = FirstIndex; Index + 2 < Mesh.Triangles.Num(); Index += 3)
		{
			const int32 A = Mesh.Triangles[Index];
			const int32 B = Mesh.Triangles[Index + 1];
			const int32 C = Mesh.Triangles[Index + 2];
			const FVector Face = -FVector::CrossProduct(Mesh.Positions[B] - Mesh.Positions[A], Mesh.Positions[C] - Mesh.Positions[A]);
			Mesh.Normals[A] += Face;
			Mesh.Normals[B] += Face;
			Mesh.Normals[C] += Face;
		}
		// The seam is split for the UVs; it shares one normal so it never shows.
		for (int32 Ring = 0; Ring < NumRings; ++Ring)
		{
			const FVector Sum = Mesh.Normals[Vertex(Ring, 0)] + Mesh.Normals[Vertex(Ring, Sides)];
			Mesh.Normals[Vertex(Ring, 0)] = Sum;
			Mesh.Normals[Vertex(Ring, Sides)] = Sum;
		}
		for (int32 V = Base; V < Mesh.Normals.Num(); ++V)
		{
			const FVector Normal = Mesh.Normals[V].GetSafeNormal();
			Mesh.Normals[V] = Normal.IsZero() ? -Down : Normal;
		}
		return Mesh.Positions.Num() - Base;
	}
}
