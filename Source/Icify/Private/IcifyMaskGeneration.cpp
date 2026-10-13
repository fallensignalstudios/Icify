// Copyright Fallen Signal Studios. All Rights Reserved.

#include "IcifyGenerator.h"

#if WITH_EDITOR

#include "Async/ParallelFor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace Icify
{
	namespace
	{
		FORCEINLINE uint8 Quantize(float Value)
		{
			return uint8(FMath::RoundToInt(FMath::Clamp(Value, 0.f, 1.f) * 255.f));
		}

		FORCEINLINE float Cross2(const FVector2D& A, const FVector2D& B)
		{
			return float(A.X * B.Y - A.Y * B.X);
		}

		float ZoneWeight(const FIcifyGenerationParams& Params, const FVector& P)
		{
			const FVector Local = Params.ZoneTransform.InverseTransformPositionNoScale(P);
			const FVector Inside = Params.ZoneExtent - Local.GetAbs();
			const double MinInside = Inside.GetMin();
			if (MinInside <= 0.0)
			{
				return 0.f;
			}
			return Params.ZoneFalloff > 1.f ? FMath::Clamp(float(MinInside / Params.ZoneFalloff), 0.f, 1.f) : 1.f;
		}

		/** A deterministic random for a world position: the same edge point gets the same value however the mesh is triangulated. */
		uint32 PointHash(const FVector& P, uint32 Seed)
		{
			uint32 H = HashU(uint32(FMath::RoundToInt(P.Z * 4.0)) * 0xcb1ab31fu ^ Seed);
			H = HashU(uint32(FMath::RoundToInt(P.Y * 4.0)) * 0xd8163841u ^ H);
			return HashU(uint32(FMath::RoundToInt(P.X * 4.0)) * 0x8da6b343u ^ H);
		}

		/** Frozen drip streaks below a melt source: even along most of their length, fading out at the end. */
		float DripFromSource(const FIcifyDripSource& Source, const FVector& P, const FVector& N, const FIcifyGenerationParams& Params)
		{
			const double Length = FMath::Max<double>(Source.Length > 0.f ? Source.Length : Params.DripLength, 10.0);

			// Meltwater follows gravity.
			const FVector Rel = P - Source.Origin;
			const double Below = -Rel.Z;
			const double Lead = Source.Radius * 0.25;
			if (Below < -Lead || Below > Length)
			{
				return 0.f;
			}

			// Skip the far side of the wall the source is on.
			const double BackfaceTolerance = Source.Radius * 0.25 + 2.0;
			if (FVector::DotProduct(Source.Origin - P, N) < -BackfaceTolerance)
			{
				return 0.f;
			}

			const double Down = FMath::Max(Below, 0.0);
			const double Lateral = FVector(Rel.X, Rel.Y, 0.0).Size();
			const double Width = Source.Radius * (1.0 + Params.DripSpread * Down / Length);
			if (Lateral > Width)
			{
				return 0.f;
			}

			const float Edge = 1.f - FMath::SmoothStep(float(Width * 0.5), float(Width), float(Lateral));
			const float Along = 1.f - FMath::SmoothStep(float(Length * 0.6), float(Length), float(Down));
			const float Lip = Below < 0.0 ? float(1.0 + Below / Lead) : 1.f;

			// Drips: fine across the flow, long along it.
			const double StreakScale = FMath::Max(Params.FeatureScale * 0.3, 1.0);
			const FVector StreakP(P.X / StreakScale, P.Y / StreakScale, P.Z / (Length * 0.5));
			const float Streak = Fbm(StreakP, Params.Seed + 71u);

			return Edge * Along * Lip * FMath::Lerp(0.4f, 1.f, Streak) * Source.Strength;
		}

		float RangeGate(float Value, float Min, float Max, float Soft, float Lowest, float Highest)
		{
			float Weight = 1.f;
			if (Min > Lowest)
			{
				Weight *= FMath::SmoothStep(Min - Soft, Min + Soft, Value);
			}
			if (Max < Highest)
			{
				Weight *= 1.f - FMath::SmoothStep(Max - Soft, Max + Soft, Value);
			}
			return Weight;
		}

		float GateWeight(const FIcifySlotInfo& Slot, float NZ, float Height, const FVector2f& GateUV)
		{
			if (!Slot.Gate)
			{
				return 1.f;
			}
			const FIcifyGate& Gate = *Slot.Gate;
			float Weight = 1.f;
			if (Gate.bUseFacingGate)
			{
				Weight *= RangeGate(NZ, Gate.MinUpFacing, Gate.MaxUpFacing, 0.05f, -1.f, 1.f);
			}
			if (Gate.bUseHeightGate)
			{
				Weight *= RangeGate(Height, Gate.MinHeightCm, Gate.MaxHeightCm, 5.f, -UE_BIG_NUMBER, UE_BIG_NUMBER);
			}
			if (Slot.GateSampler)
			{
				const float Value = Slot.GateSampler->Sample(GateUV, Gate.GateChannel);
				Weight *= Gate.bInvertGate ? 1.f - Value : Value;
			}
			return Weight;
		}

		struct FPointSample
		{
			FVector P;
			FVector N;
			float Convex = 0.f;
			float Concave = 0.f;
			/** Convex edge proximity (texture output measures it in cm from creased edges); negative = use Convex. */
			float EdgeBand = -1.f;
			const FIcifySlotInfo* Slot = nullptr;
			FVector2f GateUV = FVector2f::ZeroVector;
		};

		void ApplyStamp(FLinearColor& Masks, const FIcifyStampLayers& Layers, float Weight)
		{
			Masks.R = FMath::Clamp(Masks.R + Weight * Layers.Frost, 0.f, 1.f);
			Masks.G = FMath::Clamp(Masks.G + Weight * Layers.Snow, 0.f, 1.f);
			Masks.B = FMath::Clamp(Masks.B + Weight * Layers.Glaze, 0.f, 1.f);
			Masks.A = FMath::Clamp(Masks.A + Weight * Layers.BlackIce, 0.f, 1.f);
		}

		/**
		 * Frost, snow, glaze and black ice in 0..1 (R, G, B, A) at one surface point. The bake carries where ice is permitted
		 * and how strongly; the shader breaks it up into crystals, drifts and drips at the feature scale.
		 */
		FLinearColor EvaluatePoint(const FPointSample& S, const FIcifyGenerationParams& Params)
		{
			const FLinearColor None(0.f, 0.f, 0.f, 0.f);
			if (!S.Slot || S.Slot->bExcluded)
			{
				return None;
			}

			const float ZoneW = ZoneWeight(Params, S.P);
			if (ZoneW <= 0.f)
			{
				return None;
			}

			const FIcifySurfaceProfile& Profile = S.Slot->Profile;
			const FVector& N = S.N;
			const float NZ = float(N.Z);
			const float Height = float(FMath::Max(0.0, S.P.Z - Params.Floor->FloorBelow(S.P)));

			const float Gate = GateWeight(*S.Slot, NZ, Height, S.GateUV);
			if (Gate <= 0.f)
			{
				return None;
			}

			const FVector NP = S.P / FMath::Max<double>(Params.FeatureScale, 1.0);
			// Open to the sky: reached by falling snow and rain, and losing heat to the sky. Without a sky cover field,
			// facing stands in for it (undersides closed, walls half open, tops open).
			const float Open = Params.Sky ? Params.Sky->SurfaceOpenAt(S.P, N) : FMath::Clamp(0.5f + NZ, 0.f, 1.f);
			const float Windward = FMath::Clamp(float(FVector::DotProduct(N, -Params.WindDir)), 0.f, 1.f);
			const float Wind = Params.WindInfluence;
			const float Thaw = ThawAt(Params, S.P);
			const float Warm = 1.f - Thaw;
			const float Edge = S.EdgeBand >= 0.f ? S.EdgeBand : S.Convex;

			// Frost: a film on faces open to the sky, heavier on convex edges (they cool fastest), rime feathers into the wind,
			// and heavier frost low down where the coldest air settles. Sheltered creases lose less heat.
			const float SkyFacing = FMath::Clamp(0.55f + 0.45f * NZ, 0.f, 1.f);
			float Frost = Params.FrostFilm * FMath::Lerp(0.55f, 1.f, Open) * SkyFacing;
			Frost = FMath::Max(Frost, Edge * Params.EdgeFrost * FMath::Lerp(0.7f, 1.f, Open));
			if (Params.Rime > 0.f && Wind > 0.f)
			{
				// Rime builds on top of the frost already there, so it still shows on heavily frosted convex edges.
				const float RimeCover = FMath::Clamp(Params.Rime * Wind * FMath::Pow(Windward, 1.5f) * (0.45f + 0.55f * Edge), 0.f, 1.f);
				Frost = 1.f - (1.f - FMath::Clamp(Frost, 0.f, 1.f)) * (1.f - RimeCover);
			}
			if (Params.GroundFrost > 0.f)
			{
				const float Low = 1.f - FMath::SmoothStep(0.f, FMath::Max(Params.GroundFrostHeight, 1.f), Height);
				Frost = FMath::Max(Frost, Params.GroundFrost * Low * (0.6f + 0.4f * Open));
			}
			Frost *= 1.f - 0.4f * S.Concave;
			Frost *= Params.FrostIntensity * Profile.Frost * Warm;

			// Snow: settles where it can fall and the slope holds it (glass sheds it sooner), drifts into creases, is plastered
			// onto walls facing the wind, and is stripped off convex edges.
			const float MinUp = S.Slot->Surface == EIcifySurfaceType::Glass ? FMath::Min(Params.SnowMinUp + 0.15f, 0.97f) : Params.SnowMinUp;
			float Snow = FMath::SmoothStep(MinUp - 0.06f, MinUp + 0.1f, NZ) * Open;
			if (Params.CreviceSnow > 0.f)
			{
				Snow = FMath::Max(Snow, S.Concave * FMath::SmoothStep(-0.2f, 0.4f, NZ) * Params.CreviceSnow * Open);
			}
			if (Params.WallPlaster > 0.f && Wind > 0.f)
			{
				const float Vertical = 1.f - FMath::Abs(NZ);
				Snow = FMath::Max(Snow, Params.WallPlaster * Wind * FMath::Square(Windward) * Vertical * Open);
			}
			Snow *= 1.f - Params.EdgeSnowLoss * Edge * (0.5f + 0.5f * Wind);
			Snow *= Params.SnowIntensity * Profile.Snow * Warm;

			// Glaze: a freezing-rain coat on open faces (thicker into the wind), frozen drips below melt sources and along
			// traced meltwater, and faint frozen drips down open walls.
			float Glaze = Params.GlazeCoat * Open * FMath::Lerp(1.f, 0.4f + 0.6f * Windward, Wind);
			float Drips = 0.f;
			for (const FIcifyDripSource& Source : Params.Sources)
			{
				// Sources with a traced meltwater path are handled by the flow field below.
				if (Params.Flow && Source.Path.Num() > 1)
				{
					continue;
				}
				Drips = FMath::Max(Drips, DripFromSource(Source, S.P, N, Params));
			}
			float FlowNear = 0.f;
			if (Params.Flow)
			{
				const double StreakScale = FMath::Max(Params.FeatureScale * 0.3, 1.0);
				const float Streak = Fbm(FVector(S.P.X / StreakScale, S.P.Y / StreakScale, S.P.Z / 120.0), Params.Seed + 71u);
				Params.Flow->ForEachNear(S.P, [&](const FIcifySegmentField::FSegment& Seg, float Distance, float Along)
				{
					const float Near = 1.f - FMath::SmoothStep(Seg.Radius * 0.5f, Seg.Radius, Distance);
					FlowNear = FMath::Max(FlowNear, Near * Seg.Value);
					Drips = FMath::Max(Drips, Near * Seg.Value * FMath::Lerp(1.f, 0.6f, Along) * FMath::Lerp(0.4f, 1.f, Streak));
				});
			}
			if (Params.AmbientDrips > 0.f)
			{
				const float Vertical = 1.f - FMath::Abs(NZ);
				const FVector StreakP(NP.X * 3.0, NP.Y * 3.0, NP.Z * 0.2);
				Drips = FMath::Max(Drips, Vertical * Params.AmbientDrips * FMath::Lerp(0.35f, 1.f, Open)
					* FMath::SmoothStep(0.45f, 0.8f, Fbm(StreakP, Params.Seed + 37u)));
			}
			// Meltwater runs over faces, not along undersides.
			Drips *= FMath::Clamp(1.f + NZ * 1.5f, 0.f, 1.f);
			Glaze = FMath::Max(Glaze, Drips);
			// Meltwater refreezes in a ring around a thawed patch.
			const float Ring = FMath::SmoothStep(0.05f, 0.35f, Thaw) * (1.f - FMath::SmoothStep(0.55f, 0.9f, Thaw));
			Glaze = FMath::Max(Glaze * (1.f - 0.85f * Thaw), Ring * 0.7f * (0.3f + 0.7f * Open));
			Glaze *= Params.GlazeIntensity * Profile.Glaze;

			// Black ice: thin patchy sheets on open floors, ice where floors meet walls, frozen puddles and frozen meltwater paths.
			float Black = 0.f;
			if (NZ > 0.75f && Height < 6.f)
			{
				Black = Params.FloorIce * Open;
				Black = FMath::Max(Black, Params.CreviceIce * S.Concave * FMath::Lerp(0.3f, 1.f, Open));
				Black = FMath::Max(Black, FlowNear * 0.6f);
			}
			if (NZ > 0.6f)
			{
				for (const FIcifyPool& Pool : Params.Pools)
				{
					const float Reach = Pool.Radius + Params.PuddleRadius * 0.5f;
					const float Lateral = float(FVector(S.P.X - Pool.Center.X, S.P.Y - Pool.Center.Y, 0.0).Size());
					const float Above = float(S.P.Z - Pool.Center.Z);
					if (Lateral > Reach || Above < -20.f || Above > 30.f)
					{
						continue;
					}
					Black = FMath::Max(Black, 1.f - FMath::SmoothStep(Pool.Radius * 0.5f, Reach, Lateral));
				}
			}
			Black *= Params.BlackIceIntensity * Profile.BlackIce * (1.f - 0.8f * Thaw);

			const float K = Params.Intensity * ZoneW * Gate;
			FLinearColor Masks(
				FMath::Clamp(Frost * K, 0.f, 1.f),
				FMath::Clamp(Snow * K, 0.f, 1.f),
				FMath::Clamp(Glaze * K, 0.f, 1.f),
				FMath::Clamp(Black * K, 0.f, 1.f));

			// Hand-placed stamps add or erase on top of the procedural result.
			for (const FIcifyStampData& Stamp : Params.Stamps)
			{
				const float Weight = StampWeight(Stamp, S.P, Params.FeatureScale);
				if (Weight > 0.f)
				{
					ApplyStamp(Masks, Stamp.Layers, Weight * ZoneW * Gate);
				}
			}
			return Masks;
		}

		FColor ToColor(const FLinearColor& M)
		{
			return FColor(Quantize(M.R), Quantize(M.G), Quantize(M.B), Quantize(M.A));
		}

		struct FMaskStats
		{
			FLinearColor Sum = FLinearColor(0.f, 0.f, 0.f, 0.f);
			FLinearColor Max = FLinearColor(0.f, 0.f, 0.f, 0.f);
			int64 Count = 0;

			void Add(const FColor& C)
			{
				const FLinearColor V(C.R / 255.f, C.G / 255.f, C.B / 255.f, C.A / 255.f);
				Sum += V;
				Max = FLinearColor(FMath::Max(Max.R, V.R), FMath::Max(Max.G, V.G), FMath::Max(Max.B, V.B), FMath::Max(Max.A, V.A));
				++Count;
			}

			void WriteTo(FIcifyComponentResult& Result) const
			{
				Result.MeanMasks = Count > 0 ? Sum / float(Count) : FLinearColor(0.f, 0.f, 0.f, 0.f);
				Result.MaxMasks = Max;
			}
		};

		/** World-space mesh with welded topology, per-vertex edge convexity and per-triangle slot. */
		struct FTopology
		{
			int32 NumVerts = 0;
			TArray<FVector> Pos;
			TArray<FVector> Nrm;
			TArray<int32> Weld;
			TArray<int32> WeldFirst;
			/** Area-weighted average normal per welded vertex. */
			TArray<FVector> WeldNrm;
			TArray<uint32> Indices;
			TArray<FVector> FaceN;
			TArray<int32> TriSlot;
			TArray<float> Convex;
			TArray<float> Concave;
			double Area = 0.0;

			bool Build(const FStaticMeshLODResources& LOD, const FTransform& Transform, float SharpRef)
			{
				const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
				const FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
				NumVerts = int32(LOD.GetNumVertices());
				LOD.IndexBuffer.GetCopy(Indices);
				if (NumVerts == 0 || Indices.Num() < 3 || int32(Positions.GetNumVertices()) != NumVerts || int32(Attributes.GetNumVertices()) != NumVerts)
				{
					return false;
				}

				const FVector Scale = Transform.GetScale3D();
				const FVector InvScale(
					FMath::IsNearlyZero(Scale.X) ? 0.0 : 1.0 / Scale.X,
					FMath::IsNearlyZero(Scale.Y) ? 0.0 : 1.0 / Scale.Y,
					FMath::IsNearlyZero(Scale.Z) ? 0.0 : 1.0 / Scale.Z);

				Pos.SetNumUninitialized(NumVerts);
				Nrm.SetNumUninitialized(NumVerts);
				Weld.SetNumUninitialized(NumVerts);
				TMap<FIntVector, int32> WeldMap;
				WeldMap.Reserve(NumVerts);
				WeldFirst.Reset();

				for (int32 V = 0; V < NumVerts; ++V)
				{
					const FVector3f L = Positions.VertexPosition(V);
					const FVector4f N = Attributes.VertexTangentZ(V);
					Pos[V] = Transform.TransformPosition(FVector(L.X, L.Y, L.Z));
					Nrm[V] = Transform.TransformVectorNoScale(FVector(N.X, N.Y, N.Z) * InvScale).GetSafeNormal();

					// Weld split vertices (hard edges, UV seams) so edge detection sees the real topology.
					const FIntVector Key(FMath::RoundToInt(L.X * 20.f), FMath::RoundToInt(L.Y * 20.f), FMath::RoundToInt(L.Z * 20.f));
					if (const int32* Found = WeldMap.Find(Key))
					{
						Weld[V] = *Found;
					}
					else
					{
						Weld[V] = WeldFirst.Num();
						WeldMap.Add(Key, WeldFirst.Num());
						WeldFirst.Add(V);
					}
				}

				const int32 NumWelded = WeldFirst.Num();
				const int32 NumTris = Indices.Num() / 3;

				TriSlot.Init(0, NumTris);
				for (const FStaticMeshSection& Section : LOD.Sections)
				{
					const int32 FirstTri = int32(Section.FirstIndex / 3);
					const int32 LastTri = FMath::Min(FirstTri + int32(Section.NumTriangles), NumTris);
					for (int32 T = FirstTri; T < LastTri; ++T)
					{
						TriSlot[T] = Section.MaterialIndex;
					}
				}

				FaceN.SetNumZeroed(NumTris);
				WeldNrm.Init(FVector::ZeroVector, NumWelded);
				Area = 0.0;

				for (int32 T = 0; T < NumTris; ++T)
				{
					const uint32 I0 = Indices[T * 3 + 0];
					const uint32 I1 = Indices[T * 3 + 1];
					const uint32 I2 = Indices[T * 3 + 2];
					if (I0 >= uint32(NumVerts) || I1 >= uint32(NumVerts) || I2 >= uint32(NumVerts))
					{
						continue;
					}

					FVector Cross = FVector::CrossProduct(Pos[I1] - Pos[I0], Pos[I2] - Pos[I0]);
					if (Cross.SizeSquared() < UE_SMALL_NUMBER)
					{
						continue;
					}
					// Orient by the authored normals so winding / mirroring doesn't matter.
					if (FVector::DotProduct(Cross, Nrm[I0] + Nrm[I1] + Nrm[I2]) < 0.0)
					{
						Cross = -Cross;
					}
					FaceN[T] = Cross.GetUnsafeNormal();
					Area += Cross.Size() * 0.5;

					const int32 W0 = Weld[I0], W1 = Weld[I1], W2 = Weld[I2];
					WeldNrm[W0] += Cross;
					if (W1 != W0)
					{
						WeldNrm[W1] += Cross;
					}
					if (W2 != W0 && W2 != W1)
					{
						WeldNrm[W2] += Cross;
					}
				}
				for (FVector& N : WeldNrm)
				{
					N = N.GetSafeNormal();
				}

				// Sharpness: how far the faces around a vertex bend away from its average normal.
				// Side: whether the neighbours sit below (convex) or above (concave) the tangent plane.
				TArray<float> Sharp;
				Sharp.Init(0.f, NumWelded);
				TArray<float> SideSum;
				SideSum.Init(0.f, NumWelded);
				TArray<int32> SideCount;
				SideCount.Init(0, NumWelded);

				for (int32 T = 0; T < NumTris; ++T)
				{
					if (FaceN[T].IsZero())
					{
						continue;
					}
					const int32 W[3] = { Weld[Indices[T * 3 + 0]], Weld[Indices[T * 3 + 1]], Weld[Indices[T * 3 + 2]] };
					for (int32 K = 0; K < 3; ++K)
					{
						const int32 Wk = W[K];
						Sharp[Wk] = FMath::Max(Sharp[Wk], float(1.0 - FVector::DotProduct(FaceN[T], WeldNrm[Wk])));

						const FVector& Pk = Pos[WeldFirst[Wk]];
						for (int32 J = 0; J < 3; ++J)
						{
							if (W[J] == Wk)
							{
								continue;
							}
							const FVector Dir = (Pos[WeldFirst[W[J]]] - Pk).GetSafeNormal();
							if (!Dir.IsZero())
							{
								SideSum[Wk] += float(FVector::DotProduct(WeldNrm[Wk], Dir));
								++SideCount[Wk];
							}
						}
					}
				}

				Convex.SetNumUninitialized(NumWelded);
				Concave.SetNumUninitialized(NumWelded);
				for (int32 Wi = 0; Wi < NumWelded; ++Wi)
				{
					const float SharpN = FMath::Clamp(Sharp[Wi] / SharpRef, 0.f, 1.f);
					const float Side = SideCount[Wi] > 0 ? SideSum[Wi] / SideCount[Wi] : 0.f;
					Convex[Wi] = SharpN * FMath::Clamp(-Side * 3.f, 0.f, 1.f);
					Concave[Wi] = SharpN * FMath::Clamp(Side * 3.f, 0.f, 1.f);
				}
				return true;
			}

			FVector Centroid(int32 T) const
			{
				return (Pos[Indices[T * 3]] + Pos[Indices[T * 3 + 1]] + Pos[Indices[T * 3 + 2]]) / 3.0;
			}
		};

		/** Welded edge -> the (up to two) faces sharing it. Y is -1 on an open border and -2 on non-manifold edges. */
		void CollectEdgeFaces(const FTopology& Topo, TMap<uint64, FIntPoint>& OutEdgeFaces)
		{
			const int32 NumTris = Topo.FaceN.Num();
			OutEdgeFaces.Reserve(NumTris * 2);
			for (int32 T = 0; T < NumTris; ++T)
			{
				if (Topo.FaceN[T].IsZero())
				{
					continue;
				}
				for (int32 K = 0; K < 3; ++K)
				{
					const uint32 A = uint32(Topo.Weld[Topo.Indices[T * 3 + K]]);
					const uint32 B = uint32(Topo.Weld[Topo.Indices[T * 3 + (K + 1) % 3]]);
					if (A == B)
					{
						continue;
					}
					const uint64 Key = (uint64(FMath::Min(A, B)) << 32) | uint64(FMath::Max(A, B));
					if (FIntPoint* Faces = OutEdgeFaces.Find(Key))
					{
						Faces->Y = Faces->Y == -1 ? T : -2;
					}
					else
					{
						OutEdgeFaces.Add(Key, FIntPoint(T, -1));
					}
				}
			}
		}

		/** Distance to the mesh's creased edges, so texture masks get crisp edge frost even on sparse geometry. */
		class FEdgeField
		{
		public:
			void Build(const FTopology& Topo, float EdgeAngleDeg, float InWidth)
			{
				Width = FMath::Max(InWidth, 0.5f);
				CellSize = Width;
				const float BendRef = 1.f - FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(EdgeAngleDeg, 5.f, 170.f)));

				TMap<uint64, FIntPoint> EdgeFaces;
				CollectEdgeFaces(Topo, EdgeFaces);
				for (const TPair<uint64, FIntPoint>& Pair : EdgeFaces)
				{
					const int32 F1 = Pair.Value.X;
					const int32 F2 = Pair.Value.Y;
					if (F2 < 0)
					{
						continue; // open border or non-manifold
					}
					const FVector& N1 = Topo.FaceN[F1];
					const FVector& N2 = Topo.FaceN[F2];
					const float Bend = float(1.0 - FVector::DotProduct(N1, N2));
					if (Bend < 0.02f)
					{
						continue;
					}

					FFeatureEdge Edge;
					Edge.A = Topo.Pos[Topo.WeldFirst[int32(Pair.Key >> 32)]];
					Edge.B = Topo.Pos[Topo.WeldFirst[int32(Pair.Key & 0xffffffffu)]];
					const FVector Mid = (Edge.A + Edge.B) * 0.5;
					Edge.bConvex = FVector::DotProduct(N1, Topo.Centroid(F2) - Mid) < 0.0;
					Edge.Strength = FMath::Clamp(Bend / BendRef, 0.f, 1.f);

					const int32 EdgeIndex = Edges.Add(Edge);
					Insert(EdgeIndex);
				}
			}

			void Query(const FVector& P, float& OutConvex, float& OutConcave) const
			{
				OutConvex = 0.f;
				OutConcave = 0.f;
				const TArray<int32>* List = Cells.Find(CellOf(P));
				if (!List)
				{
					return;
				}
				for (const int32 EdgeIndex : *List)
				{
					const FFeatureEdge& Edge = Edges[EdgeIndex];
					const float Dist = float(FMath::PointDistToSegment(P, Edge.A, Edge.B));
					if (Dist >= Width)
					{
						continue;
					}
					const float Value = Edge.Strength * (1.f - FMath::SmoothStep(Width * 0.35f, Width, Dist));
					float& Target = Edge.bConvex ? OutConvex : OutConcave;
					Target = FMath::Max(Target, Value);
				}
			}

		private:
			struct FFeatureEdge
			{
				FVector A;
				FVector B;
				float Strength = 0.f;
				bool bConvex = true;
			};

			TArray<FFeatureEdge> Edges;
			TMap<FIntVector, TArray<int32>> Cells;
			double CellSize = 6.0;
			float Width = 6.f;

			FIntVector CellOf(const FVector& P) const
			{
				return FIntVector(FMath::FloorToInt(P.X / CellSize), FMath::FloorToInt(P.Y / CellSize), FMath::FloorToInt(P.Z / CellSize));
			}

			/** Registers the edge in every cell within one cell of the segment, so a point's own cell lists every edge within Width. */
			void Insert(int32 EdgeIndex)
			{
				const FFeatureEdge& Edge = Edges[EdgeIndex];
				const double Length = FVector::Dist(Edge.A, Edge.B);
				const int32 Steps = FMath::Max(1, FMath::CeilToInt(Length / (CellSize * 0.5)));
				TSet<FIntVector> Touched;
				for (int32 Step = 0; Step <= Steps; ++Step)
				{
					const FIntVector Center = CellOf(FMath::Lerp(Edge.A, Edge.B, double(Step) / Steps));
					for (int32 Z = -1; Z <= 1; ++Z)
					{
						for (int32 Y = -1; Y <= 1; ++Y)
						{
							for (int32 X = -1; X <= 1; ++X)
							{
								const FIntVector Cell = Center + FIntVector(X, Y, Z);
								if (!Touched.Contains(Cell))
								{
									Touched.Add(Cell);
									Cells.FindOrAdd(Cell).Add(EdgeIndex);
								}
							}
						}
					}
				}
			}
		};

		FVector2f GateUVOf(const FIcifySlotInfo& Slot, const FStaticMeshVertexBuffer& Attributes, uint32 Vertex)
		{
			if (!Slot.GateSampler || !Slot.Gate)
			{
				return FVector2f::ZeroVector;
			}
			const uint32 Channel = uint32(FMath::Clamp<int32>(Slot.Gate->GateUVChannel, 0, int32(Attributes.GetNumTexCoords()) - 1));
			return Attributes.GetVertexUV(Vertex, Channel);
		}

		const FIcifySlotInfo* SlotFor(const FIcifyComponentContext& Context, int32 MaterialIndex)
		{
			if (Context.Slots.IsValidIndex(MaterialIndex))
			{
				return &Context.Slots[MaterialIndex];
			}
			return Context.Slots.Num() > 0 ? &Context.Slots[0] : nullptr;
		}

		/**
		 * Icicle candidates where meltwater collects and drips. Water runs downhill over the surface, so it gathers at sink
		 * vertices: ones with no clearly lower neighbour (anything within 2 degrees of level counts as level, so a slightly
		 * tilted ledge still collects water along its edges). A sink drips when water flows into it from above and a face
		 * around it has turned under past the overhang angle (the bottom edge of a ledge, beam, eave or sill, however it's
		 * bevelled or rounded, and the lowest line of a pipe or rail), or when it's on an open border that water spills off (a
		 * single-sided roof plane, the bottom of a sign). Icicles grow along edges joining two drip vertices, and from lone
		 * drip vertices at true low points (the bottom of a dome, the low end of a tilted pipe). Whether water actually
		 * reaches them (snow above, a melt source) and whether there's room below is decided by the zone, with traces, on the
		 * game thread.
		 */
		void CollectDripPoints(const FTopology& Topo, const FStaticMeshVertexBuffer& Attributes, const FIcifyComponentContext& Context,
			const FIcifyGenerationParams& Params, FIcifyComponentResult& Out)
		{
			constexpr int32 MaxPoints = 20000;
			const int32 NumWelded = Topo.WeldFirst.Num();
			const int32 NumTris = Topo.FaceN.Num();
			const float Overhang = FMath::Sin(FMath::DegreesToRadians(FMath::Clamp(Params.DripOverhangDeg, 0.f, 80.f)));
			const float LevelSlope = FMath::Tan(FMath::DegreesToRadians(2.f));
			const float InflowSlope = FMath::Tan(FMath::DegreesToRadians(1.f));

			// Per welded vertex: whether water runs on downhill from it, the steepest climb to a neighbour (water flowing in),
			// whether every neighbour is higher (a true low point), the lowest-facing face around it and one of its triangles.
			TArray<uint8> RunsOn;
			TArray<uint8> LowPoint;
			TArray<float> SteepestClimb;
			TArray<float> LowestFaceZ;
			TArray<int32> AnyTri;
			RunsOn.Init(0, NumWelded);
			LowPoint.Init(1, NumWelded);
			SteepestClimb.Init(0.f, NumWelded);
			LowestFaceZ.Init(1.f, NumWelded);
			AnyTri.Init(INDEX_NONE, NumWelded);
			for (int32 T = 0; T < NumTris; ++T)
			{
				if (Topo.FaceN[T].IsZero())
				{
					continue;
				}
				const int32 W[3] = { Topo.Weld[Topo.Indices[T * 3]], Topo.Weld[Topo.Indices[T * 3 + 1]], Topo.Weld[Topo.Indices[T * 3 + 2]] };
				for (int32 K = 0; K < 3; ++K)
				{
					const int32 Wk = W[K];
					if (AnyTri[Wk] == INDEX_NONE)
					{
						AnyTri[Wk] = T;
					}
					LowestFaceZ[Wk] = FMath::Min(LowestFaceZ[Wk], float(Topo.FaceN[T].Z));
					const FVector& Here = Topo.Pos[Topo.WeldFirst[Wk]];
					for (int32 J = 0; J < 3; ++J)
					{
						if (W[J] == Wk)
						{
							continue;
						}
						const FVector& There = Topo.Pos[Topo.WeldFirst[W[J]]];
						const float Climb = float(There.Z - Here.Z);
						const float Span = float(FVector2D(There.X - Here.X, There.Y - Here.Y).Size());
						if (Climb < -(0.05f + LevelSlope * Span))
						{
							RunsOn[Wk] = 1;
						}
						if (Climb <= 0.05f)
						{
							LowPoint[Wk] = 0;
						}
						SteepestClimb[Wk] = FMath::Max(SteepestClimb[Wk], Climb / FMath::Max(Span, 0.01f));
					}
				}
			}
			TMap<uint64, FIntPoint> EdgeFaces;
			CollectEdgeFaces(Topo, EdgeFaces);
			TArray<uint8> OnBorder;
			OnBorder.Init(0, NumWelded);
			for (const TPair<uint64, FIntPoint>& Pair : EdgeFaces)
			{
				if (Pair.Value.Y == -1)
				{
					OnBorder[int32(Pair.Key >> 32)] = OnBorder[int32(Pair.Key & 0xffffffffu)] = 1;
				}
			}

			// How readily each welded vertex drips (0 = it doesn't).
			TArray<float> Drip;
			Drip.Init(0.f, NumWelded);
			for (int32 Wi = 0; Wi < NumWelded; ++Wi)
			{
				if (AnyTri[Wi] == INDEX_NONE || RunsOn[Wi])
				{
					continue; // nothing here, or water runs on downhill
				}
				float Value = 0.f;
				// Turned under: water flows in from above and hangs here. The lowest-facing face decides, not the vertex normal,
				// which the tall sides of a beam or sign would swamp at its bottom corners.
				if (SteepestClimb[Wi] > InflowSlope)
				{
					Value = FMath::SmoothStep(Overhang, Overhang + 0.4f, -LowestFaceZ[Wi]);
				}
				// Spilling off an open border of a face that isn't an underside.
				if (OnBorder[Wi] && Topo.WeldNrm[Wi].Z > -0.5)
				{
					Value = 1.f;
				}
				Drip[Wi] = Value;
			}

			struct FDripEdge
			{
				FVector A;
				FVector B;
				/** The surface's normal at the edge (between its faces). */
				FVector Normal;
				/** Level, off the edge into open air (zero below round undersides and where water spills straight down). */
				FVector Outward;
				float Strength = 0.f;
				int32 Tri = INDEX_NONE;
				uint32 VA = 0;
				uint32 VB = 0;
			};
			TArray<FDripEdge> DripEdges;
			TArray<uint8> Used;
			Used.Init(0, NumWelded);
			// Water runs along steep edges rather than dripping off them.
			auto Level = [](const FVector& Dir) { return 1.f - FMath::SmoothStep(0.35f, 0.65f, float(FMath::Abs(Dir.Z))); };
			auto LevelUnit = [](FVector V)
			{
				V.Z = 0.0;
				return V.SizeSquared() > 0.04 ? V.GetSafeNormal() : FVector::ZeroVector;
			};
			// The triangle's own vertex at a welded position, so gate UVs come from that triangle's UV island.
			auto CornerOf = [&Topo](int32 Tri, int32 Welded)
			{
				for (int32 K = 0; K < 3; ++K)
				{
					const uint32 Vertex = Topo.Indices[Tri * 3 + K];
					if (Topo.Weld[Vertex] == Welded)
					{
						return Vertex;
					}
				}
				return uint32(Topo.WeldFirst[Welded]);
			};

			for (const TPair<uint64, FIntPoint>& Pair : EdgeFaces)
			{
				const int32 WA = int32(Pair.Key >> 32);
				const int32 WB = int32(Pair.Key & 0xffffffffu);
				if (Drip[WA] <= 0.f || Drip[WB] <= 0.f || Pair.Value.Y == -2)
				{
					continue;
				}
				const FVector A = Topo.Pos[Topo.WeldFirst[WA]];
				const FVector B = Topo.Pos[Topo.WeldFirst[WB]];
				const double Length = FVector::Dist(A, B);
				if (Length < 0.5)
				{
					continue;
				}
				const FVector Dir = (B - A) / Length;
				FVector Normal;
				FVector Outward;
				if (Pair.Value.Y >= 0)
				{
					// A crease (or the faceted lowest line of something round), never the inside of a flat region such as the
					// diagonal of a flat underside, and never a concave fold.
					const FVector& N1 = Topo.FaceN[Pair.Value.X];
					const FVector& N2 = Topo.FaceN[Pair.Value.Y];
					const float Bend = float(1.0 - FVector::DotProduct(N1, N2));
					const bool bLowestLine = Topo.WeldNrm[WA].Z < -0.9 && Topo.WeldNrm[WB].Z < -0.9;
					if (Bend < (bLowestLine ? 1e-4f : 0.02f))
					{
						continue;
					}
					const bool bConvex = Bend > 1.9f || FVector::DotProduct(N1, Topo.Centroid(Pair.Value.Y) - (A + B) * 0.5) < 0.0;
					if (!bConvex)
					{
						continue;
					}
					// The edge's own faces, not the vertex normals, which a large neighbouring face can swamp.
					Normal = (N1 + N2).GetSafeNormal();
					Outward = LevelUnit(N1 + N2);
				}
				else
				{
					// An open border: off it is in the face's plane, away from the face.
					const FVector& Face = Topo.FaceN[Pair.Value.X];
					FVector Off = FVector::CrossProduct(Dir, Face).GetSafeNormal();
					if (FVector::DotProduct(Off, Topo.Centroid(Pair.Value.X) - (A + B) * 0.5) > 0.0)
					{
						Off = -Off;
					}
					Normal = (Face + Off).GetSafeNormal();
					Outward = LevelUnit(Off);
				}
				Used[WA] = Used[WB] = 1;
				FDripEdge& Edge = DripEdges.AddDefaulted_GetRef();
				Edge.A = A;
				Edge.B = B;
				Edge.Normal = Normal.IsZero() ? FVector(0.0, 0.0, -1.0) : Normal;
				Edge.Outward = Outward;
				Edge.Strength = FMath::Min(Drip[WA], Drip[WB]) * Level(Dir);
				Edge.Tri = Pair.Value.X;
				Edge.VA = CornerOf(Pair.Value.X, WA);
				Edge.VB = CornerOf(Pair.Value.X, WB);
			}
			for (int32 Wi = 0; Wi < NumWelded; ++Wi)
			{
				if (Drip[Wi] > 0.f && !Used[Wi] && LowPoint[Wi])
				{
					FDripEdge& Edge = DripEdges.AddDefaulted_GetRef();
					Edge.A = Edge.B = Topo.Pos[Topo.WeldFirst[Wi]];
					Edge.Normal = Topo.WeldNrm[Wi].IsZero() ? FVector(0.0, 0.0, -1.0) : Topo.WeldNrm[Wi];
					Edge.Outward = LevelUnit(Topo.WeldNrm[Wi]);
					Edge.Strength = Drip[Wi];
					Edge.Tri = AnyTri[Wi];
					Edge.VA = Edge.VB = CornerOf(AnyTri[Wi], Wi);
				}
			}

			// Uniform spacing everywhere; widened on huge meshes so the candidate budget still covers every edge.
			double TotalLength = 0.0;
			for (const FDripEdge& Edge : DripEdges)
			{
				TotalLength += FMath::Max(FVector::Dist(Edge.A, Edge.B), 1.0);
			}
			const double Spacing = FMath::Max<double>(FMath::Max(Params.DripSpacing, 0.5f), TotalLength / MaxPoints);
			Out.DripSpacingCm = float(Spacing);

			// A candidate closer than half the spacing to an earlier one is skipped, so near-duplicate drip lines (the bottom
			// of a finely tessellated pipe, doubled edges) don't multiply the icicles.
			const double MinGap = Spacing * 0.5;
			TMap<FIntVector, TArray<int32>> Taken;
			auto GapCell = [MinGap](const FVector& P)
			{
				return FIntVector(FMath::FloorToInt(P.X / MinGap), FMath::FloorToInt(P.Y / MinGap), FMath::FloorToInt(P.Z / MinGap));
			};
			auto IsTaken = [&Taken, &GapCell, &Out, MinGap](const FVector& P)
			{
				const FIntVector Center = GapCell(P);
				for (int32 Z = -1; Z <= 1; ++Z)
				{
					for (int32 Y = -1; Y <= 1; ++Y)
					{
						for (int32 X = -1; X <= 1; ++X)
						{
							if (const TArray<int32>* Indices = Taken.Find(Center + FIntVector(X, Y, Z)))
							{
								for (const int32 Index : *Indices)
								{
									if (FVector::DistSquared(Out.DripPoints[Index].P, P) < MinGap * MinGap)
									{
										return true;
									}
								}
							}
						}
					}
				}
				return false;
			};

			for (const FDripEdge& Edge : DripEdges)
			{
				if (Out.DripPoints.Num() >= MaxPoints)
				{
					break;
				}
				const FIcifySlotInfo* Slot = SlotFor(Context, Topo.TriSlot.IsValidIndex(Edge.Tri) ? Topo.TriSlot[Edge.Tri] : 0);
				if (!Slot || Slot->bExcluded || !Slot->bIcicles || Edge.Strength <= 0.f)
				{
					continue;
				}
				const double Length = FVector::Dist(Edge.A, Edge.B);
				const bool bPoint = Length < 0.5;
				const FVector Dir = bPoint ? FVector::ZeroVector : (Edge.B - Edge.A) / Length;
				// One candidate per spacing on average, however finely the edge is cut up: a fixed phase per edge rounds its
				// count up or down, so short edges sometimes get none. The phase ignores the seed, so reseeding keeps the points.
				int32 Count = 1;
				if (!bPoint)
				{
					const float Phase = float(PointHash((Edge.A + Edge.B) * 0.5, 0x5bd1e995u) & 0x00FFFFFFu) / 16777216.f;
					Count = FMath::Max(0, FMath::FloorToInt(float(Length / Spacing) + Phase));
				}
				for (int32 K = 0; K < Count && Out.DripPoints.Num() < MaxPoints; ++K)
				{
					// Centred samples, so two edges meeting at a corner don't both put a candidate on it.
					const float T = bPoint ? 0.f : (float(K) + 0.5f) / float(Count);
					const FVector P = FMath::Lerp(Edge.A, Edge.B, double(T));
					const float ZoneW = ZoneWeight(Params, P);
					if (ZoneW <= 0.f || IsTaken(P))
					{
						continue;
					}

					FPointSample S;
					S.P = P;
					S.N = Edge.Normal;
					S.Convex = 1.f;
					S.EdgeBand = 1.f;
					S.Slot = Slot;
					S.GateUV = FMath::Lerp(GateUVOf(*Slot, Attributes, Edge.VA), GateUVOf(*Slot, Attributes, Edge.VB), T);
					const float Height = float(FMath::Max(0.0, P.Z - Params.Floor->FloorBelow(P)));
					const float Gate = GateWeight(*Slot, float(S.N.Z), Height, S.GateUV);
					const float Strength = Edge.Strength * Slot->Profile.Icicles * ZoneW * Gate;
					if (Strength < 0.02f)
					{
						continue;
					}

					Taken.FindOrAdd(GapCell(P)).Add(Out.DripPoints.Num());
					FIcifyDripPoint& Point = Out.DripPoints.AddDefaulted_GetRef();
					Point.P = P;
					Point.Tangent = Dir;
					Point.Outward = Edge.Outward;
					Point.Strength = FMath::Clamp(Strength, 0.f, 1.f);
					Point.Frost = EvaluatePoint(S, Params).R;
					Point.Hash = PointHash(P, Params.Seed);
					Point.bDripSource = Context.bDripSourceMesh;
				}
			}
		}

		/**
		 * Area-weighted coverage per slot and channel, read back from the finished masks (texture or vertex colours) at
		 * seven points per triangle. Done after the bake, so it never changes the masks themselves.
		 */
		void MeasureSlotCoverage(const FStaticMeshLODResources& LOD, const FTopology& Topo, int32 UVChannel, FIcifyComponentResult& Out)
		{
			constexpr float Threshold = 0.1f;
			const int32 NumTris = Topo.Indices.Num() / 3;
			if (NumTris == 0 || Topo.TriSlot.Num() < NumTris)
			{
				return;
			}
			const bool bTexture = Out.Output == EIcifyMaskOutput::Texture && Out.TextureSize > 0 && Out.TexturePixels.Num() == Out.TextureSize * Out.TextureSize;
			const TArray<FColor>* Vertex = !bTexture && Out.LODColors.Num() > 0 && Out.LODColors[0].Num() == Topo.NumVerts ? &Out.LODColors[0] : nullptr;
			if (!bTexture && !Vertex)
			{
				return;
			}
			const FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
			if (bTexture && UVChannel >= int32(Attributes.GetNumTexCoords()))
			{
				return;
			}

			auto Unpack = [](const FColor& C, float* Values)
			{
				Values[0] = C.R / 255.f;
				Values[1] = C.G / 255.f;
				Values[2] = C.B / 255.f;
				Values[3] = C.A / 255.f;
			};
			auto TexelAt = [&Out](const FVector2D& UV) -> const FColor&
			{
				const int32 Size = Out.TextureSize;
				const int32 X = FMath::Clamp(FMath::FloorToInt(UV.X * Size), 0, Size - 1);
				const int32 Y = FMath::Clamp(FMath::FloorToInt(UV.Y * Size), 0, Size - 1);
				return Out.TexturePixels[Y * Size + X];
			};

			// Centroid, edge midpoints and points near each corner (barycentric weights).
			static const FVector3f Samples[7] = {
				{ 1.f / 3, 1.f / 3, 1.f / 3 }, { .5f, .5f, 0.f }, { 0.f, .5f, .5f }, { .5f, 0.f, .5f },
				{ .8f, .1f, .1f }, { .1f, .8f, .1f }, { .1f, .1f, .8f } };

			for (int32 T = 0; T < NumTris; ++T)
			{
				const int32 Slot = Topo.TriSlot[T];
				if (Slot < 0)
				{
					continue;
				}
				if (Out.SlotCoverage.Num() <= Slot)
				{
					Out.SlotCoverage.SetNum(Slot + 1);
				}
				FIcifyComponentResult::FSlotCoverage& Coverage = Out.SlotCoverage[Slot];
				const uint32 I[3] = { Topo.Indices[T * 3], Topo.Indices[T * 3 + 1], Topo.Indices[T * 3 + 2] };
				const double Area = 0.5 * FVector::CrossProduct(Topo.Pos[I[1]] - Topo.Pos[I[0]], Topo.Pos[I[2]] - Topo.Pos[I[0]]).Size() / 10000.0;
				if (Area <= 0.0)
				{
					continue;
				}
				Coverage.Area += Area;
				const double Weight = Area / UE_ARRAY_COUNT(Samples);

				FVector2D UVs[3];
				if (bTexture)
				{
					for (int32 K = 0; K < 3; ++K)
					{
						const FVector2f UV = Attributes.GetVertexUV(I[K], uint32(UVChannel));
						UVs[K] = FVector2D(UV.X, UV.Y);
					}
				}
				for (const FVector3f& B : Samples)
				{
					float Values[4] = { 0, 0, 0, 0 };
					if (bTexture)
					{
						Unpack(TexelAt(UVs[0] * B.X + UVs[1] * B.Y + UVs[2] * B.Z), Values);
					}
					else
					{
						float Corner[3][4];
						for (int32 K = 0; K < 3; ++K)
						{
							Unpack((*Vertex)[I[K]], Corner[K]);
						}
						for (int32 C = 0; C < 4; ++C)
						{
							Values[C] = Corner[0][C] * B.X + Corner[1][C] * B.Y + Corner[2][C] * B.Z;
						}
					}
					for (int32 C = 0; C < 4; ++C)
					{
						if (Values[C] > Threshold)
						{
							Coverage.Covered[C] += Weight;
							Coverage.Sum[C] += Weight * Values[C];
						}
						Coverage.Max[C] = FMath::Max(Coverage.Max[C], Values[C]);
					}
				}
			}
		}

		void BakeVertexColors(TConstArrayView<const FStaticMeshLODResources*> LODs, const FTransform& Transform, const FIcifyComponentContext& Context,
			const FIcifyGenerationParams& Params, float SharpRef, FIcifyComponentResult& Out, int32 (&SurfaceVotes)[3])
		{
			Out.LODColors.SetNum(LODs.Num());
			FMaskStats Stats;

			for (int32 LODIndex = 0; LODIndex < LODs.Num(); ++LODIndex)
			{
				if (Params.CancelRequested())
				{
					Out.bCancelled = true;
					return;
				}
				const FStaticMeshLODResources& LOD = *LODs[LODIndex];
				FTopology Topo;
				if (!Topo.Build(LOD, Transform, SharpRef))
				{
					continue;
				}

				// Each vertex takes the slot of a triangle that uses it.
				TArray<int32> VertSlot;
				VertSlot.Init(0, Topo.NumVerts);
				for (int32 T = 0; T < Topo.TriSlot.Num(); ++T)
				{
					for (int32 K = 0; K < 3; ++K)
					{
						const uint32 V = Topo.Indices[T * 3 + K];
						if (V < uint32(Topo.NumVerts))
						{
							VertSlot[V] = Topo.TriSlot[T];
						}
					}
				}

				const FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
				TArray<FColor>& Colors = Out.LODColors[LODIndex];
				Colors.SetNumUninitialized(Topo.NumVerts);

				for (int32 V = 0; V < Topo.NumVerts; ++V)
				{
					FPointSample S;
					S.P = Topo.Pos[V];
					S.N = Topo.Nrm[V];
					S.Convex = Topo.Convex[Topo.Weld[V]];
					S.Concave = Topo.Concave[Topo.Weld[V]];
					S.Slot = SlotFor(Context, VertSlot[V]);
					if (S.Slot)
					{
						S.GateUV = GateUVOf(*S.Slot, Attributes, uint32(V));
					}
					Colors[V] = ToColor(EvaluatePoint(S, Params));

					if (LODIndex == 0)
					{
						Stats.Add(Colors[V]);
						if (S.Slot)
						{
							++SurfaceVotes[FMath::Clamp(int32(S.Slot->Surface) - 1, 0, 2)];
						}
					}
				}
			}

			Out.Vertices = LODs[0]->GetNumVertices();
			Stats.WriteTo(Out);
		}

		bool BakeTexture(const FStaticMeshLODResources& LOD, const FTopology& Topo, const FIcifyComponentContext& Context,
			const FIcifyGenerationParams& Params, FIcifyComponentResult& Out, int32 (&SurfaceVotes)[3])
		{
			const FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
			const uint32 MaskUV = uint32(Params.MaskUVChannel);

			// Resolution from the physical texel size, assuming ~60% of the UV square is used.
			const double TexelSize = FMath::Max<double>(Params.MaskTexelSize, 0.25);
			const double Texels = Topo.Area / (TexelSize * TexelSize) / 0.6;
			const int32 Wanted = FMath::CeilToInt(FMath::Sqrt(FMath::Max(Texels, 1.0)));
			const int32 Size = FMath::Clamp(int32(FMath::RoundUpToPowerOfTwo(uint32(FMath::Max(Wanted, 1)))), 32, FMath::Max(Params.MaxMaskResolution, 32));
			const int32 NumPixels = Size * Size;

			TArray<FVector2D> UV;
			UV.SetNumUninitialized(Topo.NumVerts);
			for (int32 V = 0; V < Topo.NumVerts; ++V)
			{
				const FVector2f T = Attributes.GetVertexUV(uint32(V), MaskUV);
				UV[V] = FVector2D(T.X * Size, T.Y * Size);
			}

			// Pass 1 (serial, deterministic): each texel belongs to the first triangle that covers its centre.
			TArray<int32> Owner;
			Owner.Init(INDEX_NONE, NumPixels);
			TArray<FVector3f> Bary;
			Bary.SetNumUninitialized(NumPixels);

			const int32 NumTris = Topo.FaceN.Num();
			for (int32 T = 0; T < NumTris; ++T)
			{
				if (Topo.FaceN[T].IsZero())
				{
					continue;
				}
				const FVector2D& P0 = UV[Topo.Indices[T * 3 + 0]];
				const FVector2D& P1 = UV[Topo.Indices[T * 3 + 1]];
				const FVector2D& P2 = UV[Topo.Indices[T * 3 + 2]];
				const float Area2 = Cross2(P1 - P0, P2 - P0);
				if (FMath::Abs(Area2) < 1e-8f)
				{
					continue;
				}

				const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(P0.X, P1.X, P2.X)), 0, Size - 1);
				const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(P0.X, P1.X, P2.X)), 0, Size - 1);
				const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(P0.Y, P1.Y, P2.Y)), 0, Size - 1);
				const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(P0.Y, P1.Y, P2.Y)), 0, Size - 1);

				for (int32 Y = MinY; Y <= MaxY; ++Y)
				{
					for (int32 X = MinX; X <= MaxX; ++X)
					{
						const int32 Pixel = Y * Size + X;
						if (Owner[Pixel] != INDEX_NONE)
						{
							continue;
						}
						const FVector2D C(X + 0.5, Y + 0.5);
						const float W0 = Cross2(P2 - P1, C - P1) / Area2;
						const float W1 = Cross2(P0 - P2, C - P2) / Area2;
						const float W2 = 1.f - W0 - W1;
						if (W0 >= -1e-4f && W1 >= -1e-4f && W2 >= -1e-4f)
						{
							Owner[Pixel] = T;
							Bary[Pixel] = FVector3f(W0, W1, W2);
						}
					}
				}
			}

			if (Params.CancelRequested())
			{
				Out.bCancelled = true;
				return false;
			}

			FEdgeField Edges;
			Edges.Build(Topo, Params.EdgeAngleDeg, Params.FrostEdgeWidth);

			// Pass 2 (parallel over rows): inactive texels keep their ownership but skip the edge queries.
			Out.TexturePixels.Init(FColor(0, 0, 0, 0), NumPixels);
			auto ProcessRow = [&](int32 Y)
			{
				for (int32 X = 0; X < Size; ++X)
				{
					const int32 Pixel = Y * Size + X;
					const int32 T = Owner[Pixel];
					if (T == INDEX_NONE)
					{
						continue;
					}
					const uint32 I0 = Topo.Indices[T * 3 + 0];
					const uint32 I1 = Topo.Indices[T * 3 + 1];
					const uint32 I2 = Topo.Indices[T * 3 + 2];
					const FVector3f& W = Bary[Pixel];

					FPointSample S;
					S.Slot = SlotFor(Context, Topo.TriSlot[T]);
					if (!S.Slot || S.Slot->bExcluded)
					{
						continue;
					}
					S.P = Topo.Pos[I0] * W.X + Topo.Pos[I1] * W.Y + Topo.Pos[I2] * W.Z;
					S.N = (Topo.Nrm[I0] * W.X + Topo.Nrm[I1] * W.Y + Topo.Nrm[I2] * W.Z).GetSafeNormal();
					if (S.N.IsZero())
					{
						S.N = Topo.FaceN[T];
					}
					if (S.Slot->GateSampler)
					{
						S.GateUV = GateUVOf(*S.Slot, Attributes, I0) * W.X + GateUVOf(*S.Slot, Attributes, I1) * W.Y + GateUVOf(*S.Slot, Attributes, I2) * W.Z;
					}
					// The same early-outs as EvaluatePoint, before the edge query. Ownership is kept either way, so black texels
					// still block overlapping triangles, count in the statistics and stay covered during dilation.
					if (ZoneWeight(Params, S.P) <= 0.f)
					{
						continue;
					}
					const float Height = float(FMath::Max(0.0, S.P.Z - Params.Floor->FloorBelow(S.P)));
					if (GateWeight(*S.Slot, float(S.N.Z), Height, S.GateUV) <= 0.f)
					{
						continue;
					}
					Edges.Query(S.P, S.Convex, S.Concave);
					S.EdgeBand = S.Convex;
					Out.TexturePixels[Pixel] = ToColor(EvaluatePoint(S, Params));
				}
			};
			// Rows are processed in batches so cancellation can be polled between them. Batching changes no output.
			constexpr int32 RowsPerBatch = 32;
			for (int32 FirstRow = 0; FirstRow < Size; FirstRow += RowsPerBatch)
			{
				if (Params.CancelRequested())
				{
					Out.bCancelled = true;
					return false;
				}
				const int32 NumRows = FMath::Min(RowsPerBatch, Size - FirstRow);
				ParallelFor(NumRows, [&](int32 Row) { ProcessRow(FirstRow + Row); });
			}

			FMaskStats Stats;
			for (int32 Pixel = 0; Pixel < NumPixels; ++Pixel)
			{
				if (Owner[Pixel] != INDEX_NONE)
				{
					Stats.Add(Out.TexturePixels[Pixel]);
					if (const FIcifySlotInfo* Slot = SlotFor(Context, Topo.TriSlot[Owner[Pixel]]))
					{
						++SurfaceVotes[FMath::Clamp(int32(Slot->Surface) - 1, 0, 2)];
					}
				}
			}
			Stats.WriteTo(Out);

			// Grow the islands a few texels so filtering and mips don't pull in black from the gutters.
			TArray<bool> Covered;
			Covered.SetNumUninitialized(NumPixels);
			for (int32 Pixel = 0; Pixel < NumPixels; ++Pixel)
			{
				Covered[Pixel] = Owner[Pixel] != INDEX_NONE;
			}
			for (int32 Pass = 0; Pass < 4; ++Pass)
			{
				TArray<bool> NextCovered = Covered;
				TArray<FColor> NextPixels = Out.TexturePixels;
				for (int32 Y = 0; Y < Size; ++Y)
				{
					for (int32 X = 0; X < Size; ++X)
					{
						const int32 Pixel = Y * Size + X;
						if (Covered[Pixel])
						{
							continue;
						}
						int32 Sum[4] = { 0, 0, 0, 0 };
						int32 Count = 0;
						const FIntPoint Neighbours[4] = { { X - 1, Y }, { X + 1, Y }, { X, Y - 1 }, { X, Y + 1 } };
						for (const FIntPoint& Nb : Neighbours)
						{
							if (Nb.X < 0 || Nb.Y < 0 || Nb.X >= Size || Nb.Y >= Size || !Covered[Nb.Y * Size + Nb.X])
							{
								continue;
							}
							const FColor& C = Out.TexturePixels[Nb.Y * Size + Nb.X];
							Sum[0] += C.R; Sum[1] += C.G; Sum[2] += C.B; Sum[3] += C.A;
							++Count;
						}
						if (Count > 0)
						{
							NextPixels[Pixel] = FColor(uint8(Sum[0] / Count), uint8(Sum[1] / Count), uint8(Sum[2] / Count), uint8(Sum[3] / Count));
							NextCovered[Pixel] = true;
						}
					}
				}
				Covered = MoveTemp(NextCovered);
				Out.TexturePixels = MoveTemp(NextPixels);
			}

			Out.TextureSize = Size;
			Out.TexelSizeCm = float(FMath::Sqrt(FMath::Max(Topo.Area, 1.0) / 0.6) / Size);
			Out.Vertices = Topo.NumVerts;
			return true;
		}

		/** Area of the triangle clipped to 0..1, not the area of every triangle crossing a boundary. */
		double TriangleUVAreaInsideUnitSquare(const FVector2D (&UV)[3])
		{
			TArray<FVector2D, TInlineAllocator<8>> Polygon;
			TArray<FVector2D, TInlineAllocator<8>> Clipped;
			for (const FVector2D& P : UV)
			{
				Polygon.Add(P);
			}
			// Sutherland-Hodgman clipping against the four sides of the unit square.
			for (int32 Edge = 0; Edge < 4 && Polygon.Num() > 0; ++Edge)
			{
				const int32 Axis = Edge / 2;
				const bool bLower = (Edge % 2) == 0;
				const double Boundary = bLower ? 0.0 : 1.0;
				auto Coordinate = [Axis](const FVector2D& P) { return Axis == 0 ? P.X : P.Y; };
				auto Inside = [&](const FVector2D& P) { return bLower ? Coordinate(P) >= Boundary : Coordinate(P) <= Boundary; };
				Clipped.Reset();
				FVector2D Previous = Polygon.Last();
				bool bPreviousInside = Inside(Previous);
				for (const FVector2D& Current : Polygon)
				{
					const bool bCurrentInside = Inside(Current);
					if (bPreviousInside != bCurrentInside)
					{
						const double T = (Boundary - Coordinate(Previous)) / (Coordinate(Current) - Coordinate(Previous));
						FVector2D Intersection = Previous + (Current - Previous) * T;
						if (Axis == 0) { Intersection.X = Boundary; }
						else { Intersection.Y = Boundary; }
						Clipped.Add(Intersection);
					}
					if (bCurrentInside) { Clipped.Add(Current); }
					Previous = Current;
					bPreviousInside = bCurrentInside;
				}
				Swap(Polygon, Clipped);
			}
			if (Polygon.Num() < 3) { return 0.0; }
			double TwiceArea = 0.0;
			for (int32 K = 1; K + 1 < Polygon.Num(); ++K)
			{
				const FVector2D A = Polygon[K] - Polygon[0];
				const FVector2D B = Polygon[K + 1] - Polygon[0];
				TwiceArea += A.X * B.Y - A.Y * B.X;
			}
			return 0.5 * FMath::Abs(TwiceArea);
		}

		/** CPU copy of a static mesh LOD (positions, tangents, every UV channel, indices, sections). */
		bool CopyStaticLOD(const FStaticMeshLODResources& Source, FStaticMeshLODResources& LOD)
		{
			const FPositionVertexBuffer& Positions = Source.VertexBuffers.PositionVertexBuffer;
			const FStaticMeshVertexBuffer& Attributes = Source.VertexBuffers.StaticMeshVertexBuffer;
			const uint32 NumVerts = Positions.GetNumVertices();
			if (NumVerts == 0 || !Positions.GetVertexData() || !Attributes.GetTangentData() || !Attributes.GetTexCoordData()
				|| Attributes.GetNumVertices() != NumVerts)
			{
				return false;
			}
			const uint32 NumUVs = Attributes.GetNumTexCoords();
			LOD.VertexBuffers.PositionVertexBuffer.Init(NumVerts);
			FStaticMeshVertexBuffer& Copy = LOD.VertexBuffers.StaticMeshVertexBuffer;
			Copy.SetUseFullPrecisionUVs(true);
			Copy.Init(NumVerts, NumUVs);
			for (uint32 V = 0; V < NumVerts; ++V)
			{
				LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(V) = Positions.VertexPosition(V);
				Copy.SetVertexTangents(V, Attributes.VertexTangentX(V), Attributes.VertexTangentY(V), Attributes.VertexTangentZ(V));
				for (uint32 UV = 0; UV < NumUVs; ++UV)
				{
					Copy.SetVertexUV(V, UV, Attributes.GetVertexUV(V, UV));
				}
			}
			TArray<uint32> Indices;
			Source.IndexBuffer.GetCopy(Indices);
			LOD.IndexBuffer.SetIndices(Indices, EIndexBufferStride::Force32Bit);
			LOD.Sections = Source.Sections;
			return Indices.Num() >= 3;
		}
	}

	FIcifyUVReport ValidateMaskUVs(const FStaticMeshLODResources& LOD, const FTransform& Transform, int32 Channel, const FIcifyGenerationParams* CancelParams)
	{
		FIcifyUVReport Report;
		Report.Channel = Channel;
		Report.bChecked = true;

		const FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
		const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
		const int32 NumChannels = int32(Attributes.GetNumTexCoords());
		if (Channel < 0 || Channel >= NumChannels)
		{
			Report.Verdict = TEXT("failed");
			Report.Problems.Add(FString::Printf(TEXT("mesh has %d UV channel(s); mask channel %d is missing (Project Settings > Plugins > Icify > Mask UV Channel)"), NumChannels, Channel));
			return Report;
		}
		Report.bPresent = true;

		TArray<uint32> Indices;
		LOD.IndexBuffer.GetCopy(Indices);
		const uint32 NumVerts = Attributes.GetNumVertices();

		// Coarse raster of strict triangle interiors: texels claimed twice mean overlapping islands.
		// Shared edges are never counted twice because points exactly on an edge are not strictly inside.
		constexpr int32 Res = 256;
		TArray<uint8> Claims;
		Claims.Init(0, Res * Res);

		double UVArea = 0.0;
		double OutsideArea = 0.0;
		int32 NumTris = 0;
		int32 NumDegenerate = 0;
		double MinDensity = TNumericLimits<double>::Max();
		double MaxDensity = 0.0;

		for (int32 T = 0; T + 2 < Indices.Num(); T += 3)
		{
			if (CancelParams && (T % 3072) == 0 && CancelParams->CancelRequested())
			{
				return Report;
			}
			const uint32 I[3] = { Indices[T], Indices[T + 1], Indices[T + 2] };
			if (I[0] >= NumVerts || I[1] >= NumVerts || I[2] >= NumVerts || Positions.GetNumVertices() < NumVerts)
			{
				continue;
			}
			++NumTris;

			FVector2D UV[3];
			FVector P[3];
			for (int32 K = 0; K < 3; ++K)
			{
				const FVector2f Raw = Attributes.GetVertexUV(I[K], uint32(Channel));
				UV[K] = FVector2D(Raw.X, Raw.Y);
				const FVector3f L = Positions.VertexPosition(I[K]);
				P[K] = Transform.TransformPosition(FVector(L.X, L.Y, L.Z));
			}

			const FVector2D A = UV[1] - UV[0], B = UV[2] - UV[0];
			const double TriUVArea = 0.5 * FMath::Abs(A.X * B.Y - A.Y * B.X);
			const double TriWorldArea = 0.5 * FVector::CrossProduct(P[1] - P[0], P[2] - P[0]).Size();
			if (TriUVArea < 1e-12)
			{
				NumDegenerate += TriWorldArea > 1e-4 ? 1 : 0;
				continue;
			}
			UVArea += TriUVArea;
			OutsideArea += FMath::Clamp(TriUVArea - TriangleUVAreaInsideUnitSquare(UV), 0.0, TriUVArea);
			if (TriWorldArea > 1.0) // ignore sub-cm² slivers when measuring density spread
			{
				const double Density = TriWorldArea / TriUVArea;
				MinDensity = FMath::Min(MinDensity, Density);
				MaxDensity = FMath::Max(MaxDensity, Density);
			}

			const FVector2D S0 = UV[0] * Res, S1 = UV[1] * Res, S2 = UV[2] * Res;
			const float Area2 = Cross2(S1 - S0, S2 - S0);
			const int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min3(S0.X, S1.X, S2.X)), 0, Res - 1);
			const int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max3(S0.X, S1.X, S2.X)), 0, Res - 1);
			const int32 MinY = FMath::Clamp(FMath::FloorToInt(FMath::Min3(S0.Y, S1.Y, S2.Y)), 0, Res - 1);
			const int32 MaxY = FMath::Clamp(FMath::CeilToInt(FMath::Max3(S0.Y, S1.Y, S2.Y)), 0, Res - 1);
			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					const FVector2D C(X + 0.5, Y + 0.5);
					const float W0 = Cross2(S2 - S1, C - S1) / Area2;
					const float W1 = Cross2(S0 - S2, C - S2) / Area2;
					const float W2 = 1.f - W0 - W1;
					if (W0 > 1e-4f && W1 > 1e-4f && W2 > 1e-4f)
					{
						uint8& Claim = Claims[Y * Res + X];
						Claim = uint8(FMath::Min<int32>(Claim + 1, 255));
					}
				}
			}
		}

		int32 Covered = 0;
		int32 Overlapped = 0;
		for (const uint8 Claim : Claims)
		{
			Covered += Claim > 0 ? 1 : 0;
			Overlapped += Claim > 1 ? 1 : 0;
		}

		Report.Coverage = float(Covered) / float(Res * Res);
		Report.Overlap = Covered > 0 ? float(Overlapped) / float(Covered) : 0.f;
		Report.OutOfRange = UVArea > 0.0 ? float(OutsideArea / UVArea) : 0.f;
		Report.Degenerate = NumTris > 0 ? float(NumDegenerate) / float(NumTris) : 0.f;
		Report.DensitySpread = (MaxDensity > 0.0 && MinDensity > 0.0) ? float(MaxDensity / MinDensity) : 0.f;

		bool bFailed = false;
		bool bWarn = false;
		auto Problem = [&Report](bool bSerious, bool& bFlag, const FString& Text)
		{
			bFlag = true;
			Report.Problems.Add(FString::Printf(TEXT("%s: %s"), bSerious ? TEXT("failed") : TEXT("warning"), *Text));
		};

		if (Covered == 0)
		{
			Problem(true, bFailed, TEXT("UVs cover no area"));
		}
		if (Report.Overlap > 0.05f)
		{
			Problem(true, bFailed, FString::Printf(TEXT("%.1f%% of covered texels are claimed by overlapping islands; ice would be shared between surfaces"), Report.Overlap * 100.f));
		}
		else if (Report.Overlap > 0.005f)
		{
			Problem(false, bWarn, FString::Printf(TEXT("%.1f%% of covered texels overlap"), Report.Overlap * 100.f));
		}
		if (Report.OutOfRange > 0.02f)
		{
			Problem(true, bFailed, FString::Printf(TEXT("%.1f%% of UV area lies outside 0..1 and would be clamped"), Report.OutOfRange * 100.f));
		}
		else if (Report.OutOfRange > 0.f)
		{
			Problem(false, bWarn, FString::Printf(TEXT("%.1f%% of UV area lies outside 0..1"), Report.OutOfRange * 100.f));
		}
		if (Report.Degenerate > 0.01f)
		{
			Problem(false, bWarn, FString::Printf(TEXT("%.1f%% of triangles have zero UV area and get no ice"), Report.Degenerate * 100.f));
		}
		if (Report.DensitySpread > 8.f)
		{
			Problem(false, bWarn, FString::Printf(TEXT("texel density varies %.0fx across the mesh; ice detail will be uneven"), Report.DensitySpread));
		}
		if (Covered > 0 && Report.Coverage < 0.2f)
		{
			Problem(false, bWarn, FString::Printf(TEXT("UVs use only %.0f%% of the texture; resolution is spent on empty space"), Report.Coverage * 100.f));
		}

		Report.Verdict = bFailed ? TEXT("failed") : bWarn ? TEXT("warning") : TEXT("ok");
		return Report;
	}

	/** Static meshes, from a snapshot only (no component or render data access). */
	static bool GenerateStaticSnapshot(const FIcifyMeshSnapshot& Snapshot, const FIcifyComponentContext& Context, const FIcifyGenerationParams& Params, FIcifyComponentResult& OutResult)
	{
		if (Snapshot.LODs.Num() == 0 || !Params.Floor)
		{
			return false;
		}
		TArray<const FStaticMeshLODResources*> LODs;
		for (const TUniquePtr<FStaticMeshLODResources>& LOD : Snapshot.LODs)
		{
			LODs.Add(LOD.Get());
		}

		OutResult.bNanite = Snapshot.bNanite;
		const FTransform& Transform = Snapshot.Transform;
		const float SharpRef = 1.f - FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Params.EdgeAngleDeg, 5.f, 170.f) * 0.5f));
		int32 SurfaceVotes[3] = { 0, 0, 0 };

		// Decide the output from LOD0 (for Nanite meshes this is the fallback mesh, which shares the UV layout).
		const FStaticMeshLODResources& LOD0 = *LODs[0];
		FTopology Topo0;
		if (!Topo0.Build(LOD0, Transform, SharpRef))
		{
			return false;
		}

		const bool bHasMaskUV = int32(LOD0.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()) > Params.MaskUVChannel;
		const double AreaM2 = FMath::Max(Topo0.Area / 10000.0, 1e-3);
		const double CellsPerM2 = FMath::Square(100.0 / FMath::Max<double>(Params.FeatureScale, 1.0));
		const bool bSparse = Topo0.NumVerts / AreaM2 < CellsPerM2;

		// Validate the mask UV channel whenever a texture is a candidate, so the report says why it was or wasn't used.
		const bool bWantsTexture = Params.MaskOutput == EIcifyMaskOutput::Texture
			|| (Params.MaskOutput == EIcifyMaskOutput::Auto && (OutResult.bNanite || bSparse));
		if (bWantsTexture)
		{
			OutResult.MaskUV = ValidateMaskUVs(LOD0, Transform, Params.MaskUVChannel, &Params);
			if (Params.CancelRequested())
			{
				OutResult.bCancelled = true;
				return false;
			}
		}
		const bool bUVsUsable = bHasMaskUV && (!OutResult.MaskUV.bChecked || OutResult.MaskUV.Verdict != TEXT("failed"));

		bool bTexture = false;
		switch (Params.MaskOutput)
		{
		case EIcifyMaskOutput::Texture:
			// Explicitly requested: honour it whenever the channel exists, but say what's wrong with it.
			bTexture = bHasMaskUV;
			if (!bHasMaskUV)
			{
				OutResult.Notes += FString::Printf(TEXT("No UV channel %d for the mask texture; used vertex colours. "), Params.MaskUVChannel);
			}
			else if (!bUVsUsable)
			{
				OutResult.Notes += FString::Printf(TEXT("UV channel %d failed validation but Texture output was requested; expect artefacts (see Mask UV). "), Params.MaskUVChannel);
			}
			break;
		case EIcifyMaskOutput::Auto:
			bTexture = bUVsUsable && (OutResult.bNanite || bSparse);
			if (!bTexture && (OutResult.bNanite || bSparse))
			{
				OutResult.Notes += FString::Printf(TEXT("%s but UV channel %d is %s; used vertex colours. "),
					OutResult.bNanite ? TEXT("Nanite mesh") : TEXT("Sparse mesh"), Params.MaskUVChannel,
					bHasMaskUV ? TEXT("unsuitable (see Mask UV)") : TEXT("missing"));
			}
			break;
		default:
			break;
		}
		if (bSparse && !bTexture)
		{
			OutResult.Notes += TEXT("Sparse geometry: vertex-colour detail will be coarse. ");
		}

		if (bTexture)
		{
			OutResult.Output = EIcifyMaskOutput::Texture;
			OutResult.MaskUVChannelUsed = Params.MaskUVChannel;
			if (!BakeTexture(LOD0, Topo0, Context, Params, OutResult, SurfaceVotes))
			{
				return false;
			}
		}
		else
		{
			OutResult.Output = EIcifyMaskOutput::VertexColor;
			BakeVertexColors(LODs, Transform, Context, Params, SharpRef, OutResult, SurfaceVotes);
			if (OutResult.bCancelled)
			{
				return false;
			}
			if (OutResult.bNanite)
			{
				OutResult.Notes += TEXT("Nanite mesh with vertex-colour masks: may not render. ");
			}
		}

		MeasureSlotCoverage(LOD0, Topo0, Params.MaskUVChannel, OutResult);

		if (Params.bDripPoints)
		{
			CollectDripPoints(Topo0, LOD0.VertexBuffers.StaticMeshVertexBuffer, Context, Params, OutResult);
		}

		int32 Best = 0;
		for (int32 Index = 1; Index < 3; ++Index)
		{
			if (SurfaceVotes[Index] > SurfaceVotes[Best])
			{
				Best = Index;
			}
		}
		OutResult.DominantSurface = EIcifySurfaceType(Best + 1);
		// Coverage and drip collection don't poll, so a cancel during them is caught here.
		if (Params.CancelRequested())
		{
			OutResult.bCancelled = true;
			return false;
		}
		return true;
	}

	bool SnapshotMesh(UMeshComponent* Component, FIcifyMeshSnapshot& Out)
	{
		Out = FIcifyMeshSnapshot();
		if (!IsValid(Component))
		{
			return false;
		}
		Out.Path = Component->GetPathName();
		Out.Transform = Component->GetComponentTransform();

		if (UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
		{
			const UStaticMesh* Mesh = Static->GetStaticMesh();
			const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
			if (!RenderData || RenderData->LODResources.Num() == 0)
			{
				return false;
			}
			Out.Kind = FIcifyMeshSnapshot::EKind::Static;
			Out.bNanite = Mesh->HasValidNaniteData();
			Out.LocalBounds = Mesh->GetBoundingBox();
			for (int32 LODIndex = 0; LODIndex < RenderData->LODResources.Num(); ++LODIndex)
			{
				TUniquePtr<FStaticMeshLODResources> Copy = MakeUnique<FStaticMeshLODResources>(false);
				if (!CopyStaticLOD(RenderData->LODResources[LODIndex], *Copy))
				{
					// A later LOD that isn't readable only costs its vertex colours; LOD0 is required.
					if (LODIndex == 0)
					{
						return false;
					}
					break;
				}
				Out.LODs.Add(MoveTemp(Copy));
			}
			return true;
		}
		return false;
	}

	bool GenerateFromSnapshot(const FIcifyMeshSnapshot& Snapshot, const FIcifyComponentContext& Context, const FIcifyGenerationParams& Params, FIcifyComponentResult& OutResult)
	{
		switch (Snapshot.Kind)
		{
		case FIcifyMeshSnapshot::EKind::Static: return GenerateStaticSnapshot(Snapshot, Context, Params, OutResult);
		default: return false;
		}
	}

	bool GenerateComponent(UStaticMeshComponent* Component, const FIcifyComponentContext& Context, const FIcifyGenerationParams& Params, FIcifyComponentResult& OutResult)
	{
		FIcifyMeshSnapshot Snapshot;
		OutResult.Component = Component;
		return SnapshotMesh(Component, Snapshot) && Snapshot.Kind == FIcifyMeshSnapshot::EKind::Static && GenerateFromSnapshot(Snapshot, Context, Params, OutResult);
	}

#if WITH_DEV_AUTOMATION_TESTS
	namespace
	{
		/** CPU-only LOD: positions, per-vertex normals and UVs (the same in channels 0 and 1), and triangles. */
		void InitTestLOD(FStaticMeshLODResources& LOD, const TArray<FVector3f>& Positions, const TArray<FVector3f>& Normals, const TArray<uint32>& Indices)
		{
			const uint32 NumVerts = uint32(Positions.Num());
			LOD.VertexBuffers.PositionVertexBuffer.Init(NumVerts);
			FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
			Attributes.SetUseFullPrecisionUVs(true);
			Attributes.Init(NumVerts, 2);
			for (uint32 V = 0; V < NumVerts; ++V)
			{
				LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(V) = Positions[V];
				const FVector3f N = Normals[V].GetSafeNormal();
				const FVector3f Tangent = FMath::Abs(N.Z) < 0.9f ? FVector3f::CrossProduct(FVector3f(0.f, 0.f, 1.f), N).GetSafeNormal() : FVector3f(1.f, 0.f, 0.f);
				Attributes.SetVertexTangents(V, Tangent, FVector3f::CrossProduct(N, Tangent), N);
				Attributes.SetVertexUV(V, 0, FVector2f(Positions[V].X * 0.001f, Positions[V].Y * 0.001f));
				Attributes.SetVertexUV(V, 1, FVector2f(Positions[V].X * 0.001f, Positions[V].Y * 0.001f));
			}
			LOD.IndexBuffer.SetIndices(Indices, EIndexBufferStride::Force32Bit);
			FStaticMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
			Section.MaterialIndex = 0;
			Section.FirstIndex = 0;
			Section.NumTriangles = uint32(Indices.Num() / 3);
			Section.MinVertexIndex = 0;
			Section.MaxVertexIndex = NumVerts - 1;
		}

		/** An axis-aligned box with split (hard-edged) faces, like a ledge or a beam. */
		void InitBoxLOD(FStaticMeshLODResources& LOD, const FVector3f& Min, const FVector3f& Max)
		{
			TArray<FVector3f> Positions;
			TArray<FVector3f> Normals;
			TArray<uint32> Indices;
			auto Face = [&](const FVector3f& N, const FVector3f& A, const FVector3f& B, const FVector3f& C, const FVector3f& D)
			{
				const uint32 Base = uint32(Positions.Num());
				for (const FVector3f& P : { A, B, C, D })
				{
					Positions.Add(P);
					Normals.Add(N);
				}
				Indices.Append({ Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 });
			};
			auto P = [&](bool bX, bool bY, bool bZ) { return FVector3f(bX ? Max.X : Min.X, bY ? Max.Y : Min.Y, bZ ? Max.Z : Min.Z); };
			Face(FVector3f(0, 0, 1), P(0, 0, 1), P(1, 0, 1), P(1, 1, 1), P(0, 1, 1));
			Face(FVector3f(0, 0, -1), P(0, 0, 0), P(0, 1, 0), P(1, 1, 0), P(1, 0, 0));
			Face(FVector3f(0, -1, 0), P(0, 0, 0), P(1, 0, 0), P(1, 0, 1), P(0, 0, 1));
			Face(FVector3f(0, 1, 0), P(0, 1, 0), P(0, 1, 1), P(1, 1, 1), P(1, 1, 0));
			Face(FVector3f(-1, 0, 0), P(0, 0, 0), P(0, 0, 1), P(0, 1, 1), P(0, 1, 0));
			Face(FVector3f(1, 0, 0), P(1, 0, 0), P(1, 1, 0), P(1, 1, 1), P(1, 0, 1));
			InitTestLOD(LOD, Positions, Normals, Indices);
		}

		/** A horizontal cylinder along X (a pipe or rail), smooth-shaded. */
		void InitPipeLOD(FStaticMeshLODResources& LOD, float Length, float Radius, float Z, int32 Sides)
		{
			TArray<FVector3f> Positions;
			TArray<FVector3f> Normals;
			TArray<uint32> Indices;
			for (int32 Ring = 0; Ring < 2; ++Ring)
			{
				for (int32 Side = 0; Side < Sides; ++Side)
				{
					// Side 0 points straight down, so the pipe has a bottom line of vertices.
					const float Angle = -UE_HALF_PI + UE_TWO_PI * float(Side) / float(Sides);
					const FVector3f N(0.f, FMath::Cos(Angle), FMath::Sin(Angle));
					Positions.Add(FVector3f(Ring == 0 ? 0.f : Length, N.Y * Radius, Z + N.Z * Radius));
					Normals.Add(N);
				}
			}
			for (int32 Side = 0; Side < Sides; ++Side)
			{
				const uint32 A = uint32(Side), B = uint32((Side + 1) % Sides);
				const uint32 C = A + uint32(Sides), D = B + uint32(Sides);
				Indices.Append({ A, C, D, A, D, B });
			}
			InitTestLOD(LOD, Positions, Normals, Indices);
		}

		struct FTestScene
		{
			FIcifyFloorField Floor;
			FIcifyGenerationParams Params;
			FIcifyComponentContext Context;

			FTestScene()
			{
				Floor.BuildFlat(0.0);
				Params.Floor = &Floor;
				Params.ZoneExtent = FVector(5000.0, 5000.0, 5000.0);
				Params.FrostFilm = 0.35f;
				Params.EdgeFrost = 0.8f;
				Params.Rime = 0.6f;
				Params.SnowMinUp = FMath::Cos(FMath::DegreesToRadians(50.f));
				Params.GlazeCoat = 0.25f;
				Params.FloorIce = 0.4f;
				Params.CreviceIce = 0.5f;
				Params.ThawIntensity = 1.f;
				Params.WindDir = FVector::ForwardVector;
				Params.bDripPoints = true;
				Params.DripSpacing = 5.f;
				Context.Slots.AddDefaulted();
			}

			FLinearColor At(const FVector& P, const FVector& N, float Convex = 0.f, float Concave = 0.f) const
			{
				FPointSample S;
				S.P = P;
				S.N = N.GetSafeNormal();
				S.Convex = Convex;
				S.Concave = Concave;
				S.Slot = &Context.Slots[0];
				return EvaluatePoint(S, Params);
			}

			bool Drips(const FStaticMeshLODResources& LOD, FIcifyComponentResult& Out) const
			{
				FTopology Topo;
				const float SharpRef = 1.f - FMath::Cos(FMath::DegreesToRadians(22.5f));
				if (!Topo.Build(LOD, FTransform::Identity, SharpRef))
				{
					return false;
				}
				CollectDripPoints(Topo, LOD.VertexBuffers.StaticMeshVertexBuffer, Context, Params, Out);
				return true;
			}
		};
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIcifyLayerRulesTest, "Icify.Generation.LayerRules",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FIcifyLayerRulesTest::RunTest(const FString& Parameters)
	{
		FTestScene Scene;
		const FVector Ledge(0.0, 0.0, 150.0);

		const FLinearColor Top = Scene.At(Ledge, FVector::UpVector);
		const FLinearColor Under = Scene.At(Ledge, -FVector::UpVector);
		const FLinearColor Wall = Scene.At(Ledge, FVector::RightVector);
		TestTrue(TEXT("An open, level top holds snow"), Top.G > 0.8f);
		TestEqual(TEXT("An underside never holds snow"), Under.G, 0.f);
		TestTrue(TEXT("A wall holds far less snow than a top (no wind plaster without wind)"), Wall.G < 0.05f);
		TestTrue(TEXT("Open tops frost more than undersides"), Top.R > Under.R);

		const FLinearColor Steep = Scene.At(Ledge, FVector(0.0, 1.0, 0.5));
		TestTrue(TEXT("A slope steeper than the snow limit sheds snow"), Steep.G < 0.05f);

		Scene.Params.WindInfluence = 1.f;
		const FLinearColor Windward = Scene.At(Ledge, -FVector::ForwardVector, 1.f);
		const FLinearColor Leeward = Scene.At(Ledge, FVector::ForwardVector, 1.f);
		TestTrue(TEXT("Rime grows on edges into the wind"), Windward.R > Leeward.R + 0.1f);
		Scene.Params.WindInfluence = 0.f;

		const FLinearColor Floor = Scene.At(FVector(0.0, 0.0, 0.0), FVector::UpVector);
		TestTrue(TEXT("Open floors get black ice"), Floor.A > 0.3f);
		TestEqual(TEXT("Raised ledges get no black ice"), Top.A, 0.f);

		// A roof 100 cm above the ledge: a sky field whose cells all hit at that depth.
		{
			const FVector Origin(-500.0, -500.0, 1000.0);
			TArray<float> Depths;
			Depths.Init(1000.f - 250.f, 100 * 100);
			FIcifySkyCoverField Sky;
			Sky.BuildFromDepths(Origin, FVector(0.0, 0.0, -1.0), 10.0, 100, 100, Depths);
			Scene.Params.Sky = &Sky;
			const FLinearColor Covered = Scene.At(Ledge, FVector::UpVector);
			TestTrue(TEXT("A top under a roof holds no snow"), Covered.G < 0.01f);
			TestTrue(TEXT("A top under a roof frosts less than an open one"), Covered.R < Top.R);
			Scene.Params.Sky = nullptr;
		}

		FIcifyHeatSource Heat;
		Heat.Location = Ledge + FVector(0.0, 0.0, -20.0);
		Heat.Radius = 150.f;
		Scene.Params.HeatSources.Add(Heat);
		const FLinearColor Thawed = Scene.At(Ledge, FVector::UpVector);
		TestTrue(TEXT("Snow and frost thaw next to a heat source"), Thawed.G < 0.05f && Thawed.R < 0.05f);
		Scene.Params.HeatSources.Reset();

		Scene.Context.Slots[0].bExcluded = true;
		TestEqual(TEXT("Excluded slots get nothing"), Scene.At(Ledge, FVector::UpVector).GetMax(), 0.f);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIcifySkyCoverTest, "Icify.Generation.SkyCoverLookup",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FIcifySkyCoverTest::RunTest(const FString& Parameters)
	{
		// 20 x 20 cells of 10 cm from (0, 0, 500) looking straight down. The half with X < 100 is under a roof at Z = 300.
		FIcifySkyCoverField Sky;
		TArray<float> Depths;
		Depths.Init(-1.f, 20 * 20);
		const FVector Origin(0.0, 0.0, 500.0);
		Sky.BuildFromDepths(Origin, FVector(0.0, 0.0, -1.0), 10.0, 20, 20, Depths);
		if (!TestTrue(TEXT("Field built"), Sky.IsValid()))
		{
			return false;
		}
		// The same grid, every cell first hitting a roof at depth 200 (Z = 300).
		TArray<float> Roofed;
		Roofed.Init(200.f, 20 * 20);
		FIcifySkyCoverField Covered;
		Covered.BuildFromDepths(Origin, FVector(0.0, 0.0, -1.0), 10.0, 20, 20, Roofed);

		const FVector Below(50.0, 50.0, 100.0);
		const FVector Above(50.0, 50.0, 350.0);
		TestEqual(TEXT("With no hits everything is open"), Sky.OpenAt(Below), 1.f);
		TestEqual(TEXT("Below the first hit is closed"), Covered.OpenAt(Below), 0.f);
		TestEqual(TEXT("Above the first hit is open"), Covered.OpenAt(Above), 1.f);
		TestEqual(TEXT("Undersides face away from falling snow"), Covered.SurfaceOpenAt(Above, -FVector::UpVector), 0.f);
		TestEqual(TEXT("A roof top (the first hit itself) is open"), Covered.SurfaceOpenAt(FVector(50.0, 50.0, 300.0), FVector::UpVector), 1.f);
		TestEqual(TEXT("Outside the field is open"), Covered.OpenAt(FVector(-5000.0, 0.0, 0.0)), 1.f);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIcifyDripEdgeTest, "Icify.Generation.DripEdges",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FIcifyDripEdgeTest::RunTest(const FString& Parameters)
	{
		FTestScene Scene;

		// A ledge 100 x 40 x 20 cm at 100 cm: icicles belong on its four bottom edges, never on its top edges.
		{
			FStaticMeshLODResources LOD(false);
			InitBoxLOD(LOD, FVector3f(0.f, 0.f, 100.f), FVector3f(100.f, 40.f, 120.f));
			FIcifyComponentResult Out;
			if (!TestTrue(TEXT("Ledge topology"), Scene.Drips(LOD, Out)))
			{
				return false;
			}
			TestTrue(TEXT("Ledge has drip points"), Out.DripPoints.Num() > 20);
			bool bAllBottom = true;
			bool bOutwardOk = true;
			for (const FIcifyDripPoint& Point : Out.DripPoints)
			{
				bAllBottom &= FMath::IsNearlyEqual(Point.P.Z, 100.0, 0.01);
				bOutwardOk &= FMath::IsNearlyEqual(Point.Outward.Size(), 1.0, 0.01) && FMath::IsNearlyZero(Point.Outward.Z, 0.01);
				if (FMath::IsNearlyEqual(Point.P.Y, 0.0, 0.01) && Point.P.X > 1.0 && Point.P.X < 99.0)
				{
					bOutwardOk &= Point.Outward.Y < -0.99;
				}
			}
			TestTrue(TEXT("Every drip point is on a bottom edge"), bAllBottom);
			TestTrue(TEXT("Drip points face out of the ledge, level"), bOutwardOk);
		}

		// A pipe: drips along its lowest line, with no wall side.
		{
			FStaticMeshLODResources LOD(false);
			InitPipeLOD(LOD, 200.f, 5.f, 150.f, 12);
			FIcifyComponentResult Out;
			if (!TestTrue(TEXT("Pipe topology"), Scene.Drips(LOD, Out)))
			{
				return false;
			}
			TestTrue(TEXT("Pipe has drip points"), Out.DripPoints.Num() > 10);
			bool bBottom = true;
			for (const FIcifyDripPoint& Point : Out.DripPoints)
			{
				bBottom &= FMath::IsNearlyEqual(Point.P.Z, 145.0, 0.05) && Point.Outward.IsNearlyZero();
			}
			TestTrue(TEXT("Pipe drips only from its lowest line"), bBottom);
		}

		// Same seed and mesh, same points; another seed keeps the points and changes their randoms.
		{
			FStaticMeshLODResources LOD(false);
			InitBoxLOD(LOD, FVector3f(0.f, 0.f, 100.f), FVector3f(100.f, 40.f, 120.f));
			FIcifyComponentResult A, B, C;
			Scene.Drips(LOD, A);
			Scene.Drips(LOD, B);
			Scene.Params.Seed = 99u;
			Scene.Drips(LOD, C);
			bool bSame = A.DripPoints.Num() == B.DripPoints.Num();
			bool bReseeded = A.DripPoints.Num() == C.DripPoints.Num();
			int32 Differ = 0;
			for (int32 Index = 0; bSame && Index < A.DripPoints.Num(); ++Index)
			{
				bSame &= A.DripPoints[Index].P == B.DripPoints[Index].P && A.DripPoints[Index].Hash == B.DripPoints[Index].Hash;
				if (bReseeded)
				{
					Differ += A.DripPoints[Index].Hash != C.DripPoints[Index].Hash ? 1 : 0;
				}
			}
			TestTrue(TEXT("Drip points are deterministic"), bSame);
			TestTrue(TEXT("A new seed re-rolls the drip randoms"), bReseeded && Differ > A.DripPoints.Num() / 2);
		}

		// Excluded slots, and slots with icicles off, never drip.
		{
			FStaticMeshLODResources LOD(false);
			InitBoxLOD(LOD, FVector3f(0.f, 0.f, 100.f), FVector3f(100.f, 40.f, 120.f));
			Scene.Context.Slots[0].bIcicles = false;
			FIcifyComponentResult Out;
			Scene.Drips(LOD, Out);
			TestEqual(TEXT("Icicles off: no drip points"), Out.DripPoints.Num(), 0);
		}
		return true;
	}
#endif // WITH_DEV_AUTOMATION_TESTS
}

#endif // WITH_EDITOR
