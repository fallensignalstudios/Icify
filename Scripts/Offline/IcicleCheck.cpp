// Copyright Fallen Signal Studios. All Rights Reserved.
//
// Offline checks for Source/Icify/Private/IcifyIcicleBuilder.cpp, compiled against the CoreMinimal.h shim beside this
// file (see Scripts/CheckIcicles.py). Exit code 0 when every check passes. With an argument, also writes a sample
// cluster of icicles to that path as an OBJ.
#include "IcifyIcicleBuilder.h"

#include <cstdio>
#include <map>
#include <string>
#include <tuple>

namespace
{
	int Failures = 0;

	void Check(bool bOk, const char* What, const std::string& Detail = std::string())
	{
		if (!bOk)
		{
			++Failures;
			std::printf("FAIL %s%s%s\n", What, Detail.empty() ? "" : ": ", Detail.c_str());
		}
	}

	bool Finite(double V) { return std::isfinite(V); }

	std::string Describe(const FIcifyIcicleSpec& Spec)
	{
		char Buffer[160];
		std::snprintf(Buffer, sizeof(Buffer), "L=%.1f R=%.2f sides=%d bend=%.2f seed=%u", Spec.Length, Spec.Radius, Spec.Sides, Spec.Bend, Spec.Seed);
		return Buffer;
	}

	/** Checks one icicle built on its own. */
	void CheckIcicle(const FIcifyIcicleSpec& Spec)
	{
		const std::string Name = Describe(Spec);
		FIcifyIcicleMesh Mesh;
		const FVector Origin(1000.0, -2000.0, 300.0);
		const int32 Added = Icify::AppendIcicle(Spec, Origin, Mesh);
		Check(Added == Icify::IcicleVertexCount(Spec), "vertex count matches IcicleVertexCount", Name);
		Check(Added > 0, "icicle has vertices", Name);
		Check(Mesh.Normals.Num() == Added && Mesh.Tangents.Num() == Added && Mesh.UVs.Num() == Added && Mesh.Colors.Num() == Added,
			"every vertex stream has one entry per vertex", Name);
		Check(Mesh.Triangles.Num() % 3 == 0 && Mesh.Triangles.Num() > 0, "triangle list is a multiple of 3", Name);

		bool bFinite = true, bUnitNormals = true, bIndices = true, bColors = true;
		for (int32 V = 0; V < Mesh.NumVertices(); ++V)
		{
			const FVector& P = Mesh.Positions[V];
			const FVector& N = Mesh.Normals[V];
			bFinite &= Finite(P.X) && Finite(P.Y) && Finite(P.Z) && Finite(N.X) && Finite(N.Y) && Finite(N.Z)
				&& Finite(Mesh.UVs[V].X) && Finite(Mesh.UVs[V].Y);
			bUnitNormals &= std::fabs(N.Size() - 1.0) < 1e-3;
			const FLinearColor& C = Mesh.Colors[V];
			bColors &= C.R >= 0.f && C.R <= 1.f && C.G >= 0.f && C.G <= 1.f && C.B >= 0.f && C.B <= 1.f && C.A >= 0.f && C.A <= 1.f;
		}
		for (int32 Index : Mesh.Triangles)
		{
			bIndices &= Index >= 0 && Index < Mesh.NumVertices();
		}
		Check(bFinite, "positions, normals and UVs are finite", Name);
		Check(bUnitNormals, "normals are unit length", Name);
		Check(bIndices, "triangle indices are in range", Name);
		Check(bColors, "vertex colours are in 0..1", Name);
		if (!bIndices || !bFinite)
		{
			return;
		}

		// Watertight: weld by position (the UV seam is split), then every edge must be shared by exactly two triangles,
		// once in each direction (consistent winding).
		std::map<std::tuple<long long, long long, long long>, int> Weld;
		std::vector<int> WeldOf(size_t(Mesh.NumVertices()));
		for (int32 V = 0; V < Mesh.NumVertices(); ++V)
		{
			const FVector& P = Mesh.Positions[V];
			const auto Key = std::make_tuple((long long)std::llround(P.X * 1e4), (long long)std::llround(P.Y * 1e4), (long long)std::llround(P.Z * 1e4));
			auto Found = Weld.find(Key);
			if (Found == Weld.end())
			{
				Found = Weld.emplace(Key, int(Weld.size())).first;
			}
			WeldOf[size_t(V)] = Found->second;
		}
		std::map<std::pair<int, int>, int> Directed;
		int Degenerate = 0;
		double SignedVolume = 0.0;
		int NormalDisagree = 0;
		for (int32 T = 0; T < Mesh.NumTriangles(); ++T)
		{
			const int32 I[3] = { Mesh.Triangles[T * 3], Mesh.Triangles[T * 3 + 1], Mesh.Triangles[T * 3 + 2] };
			const int W[3] = { WeldOf[size_t(I[0])], WeldOf[size_t(I[1])], WeldOf[size_t(I[2])] };
			if (W[0] == W[1] || W[1] == W[2] || W[0] == W[2])
			{
				++Degenerate;
				continue;
			}
			for (int K = 0; K < 3; ++K)
			{
				++Directed[std::make_pair(W[K], W[(K + 1) % 3])];
			}
			const FVector& A = Mesh.Positions[I[0]];
			const FVector& B = Mesh.Positions[I[1]];
			const FVector& C = Mesh.Positions[I[2]];
			SignedVolume += FVector::DotProduct(A, FVector::CrossProduct(B, C)) / 6.0;
			// Unreal winding: the geometric cross points inwards, so it must disagree with the outward vertex normals.
			const FVector Cross = FVector::CrossProduct(B - A, C - A);
			const FVector Smooth = Mesh.Normals[I[0]] + Mesh.Normals[I[1]] + Mesh.Normals[I[2]];
			if (Cross.Size() > 1e-9 && FVector::DotProduct(Cross, Smooth) > 0.0)
			{
				++NormalDisagree;
			}
		}
		int Open = 0, Doubled = 0;
		for (const auto& Edge : Directed)
		{
			const auto Reverse = Directed.find(std::make_pair(Edge.first.second, Edge.first.first));
			if (Reverse == Directed.end())
			{
				++Open;
			}
			if (Edge.second > 1)
			{
				++Doubled;
			}
		}
		Check(Degenerate == 0, "no triangle collapses onto a point or line", Name + " degenerate=" + std::to_string(Degenerate));
		Check(Open == 0, "surface is closed (every edge has a partner)", Name + " open=" + std::to_string(Open));
		Check(Doubled == 0, "winding is consistent (no edge used twice the same way)", Name + " doubled=" + std::to_string(Doubled));
		// The standard signed volume is positive for counter-clockwise (outward cross) winding; Unreal's is the reverse.
		Check(SignedVolume < 0.0, "Unreal (clockwise) winding: signed volume is negative", Name + " volume=" + std::to_string(SignedVolume));
		Check(NormalDisagree == 0, "every face agrees with the outward vertex normals", Name + " disagree=" + std::to_string(NormalDisagree));

		// Hangs down from the root: the tip is the lowest point along Down, and nothing reaches far above the root.
		const FVector Down = Spec.Down.GetSafeNormal();
		const FVector Root = Spec.Root - Origin;
		double Lowest = -1e30, Highest = 1e30;
		for (const FVector& P : Mesh.Positions)
		{
			const double Along = FVector::DotProduct(P - Root, Down);
			Lowest = std::max(Lowest, Along);
			Highest = std::min(Highest, Along);
		}
		Check(Lowest > Spec.Length * 0.95 && Lowest < Spec.Length * 1.05, "tip is about Length below the root", Name + " tip=" + std::to_string(Lowest));
		Check(Highest > -2.5 * Spec.Radius, "root cap stays close above the root", Name + " top=" + std::to_string(Highest));

		// Thickest near the root, thin at the tip.
		Check(Mesh.Colors[Mesh.NumVertices() - 1].B == 1.f, "tip vertex has B = 1", Name);

		// Deterministic, and seed-sensitive.
		FIcifyIcicleMesh Again;
		Icify::AppendIcicle(Spec, Origin, Again);
		bool bSame = Again.NumVertices() == Mesh.NumVertices();
		for (int32 V = 0; bSame && V < Mesh.NumVertices(); ++V)
		{
			bSame = Again.Positions[V] == Mesh.Positions[V] && Again.Normals[V] == Mesh.Normals[V];
		}
		Check(bSame, "same spec grows the same icicle", Name);
		FIcifyIcicleSpec Other = Spec;
		Other.Seed = Spec.Seed ^ 0x1234567u;
		FIcifyIcicleMesh Reseeded;
		Icify::AppendIcicle(Other, Origin, Reseeded);
		bool bDiffers = Reseeded.NumVertices() != Mesh.NumVertices();
		for (int32 V = 0; !bDiffers && V < Mesh.NumVertices(); ++V)
		{
			bDiffers = !(Reseeded.Positions[V] == Mesh.Positions[V]);
		}
		Check(bDiffers, "another seed grows a different icicle", Name);
	}

	void WriteObj(const char* Path)
	{
		// A row of icicles under a 2 m eave, plus a few round-rooted ones, as one mesh.
		FIcifyIcicleMesh Mesh;
		const FVector Origin(0.0, 0.0, 0.0);
		uint32 State = 12345u;
		auto Next = [&State]() { State = State * 1664525u + 1013904223u; return float(State >> 8) / 16777216.f; };
		for (int32 Index = 0; Index < 34; ++Index)
		{
			FIcifyIcicleSpec Spec;
			const float X = -95.f + 190.f * (float(Index) + 0.5f * Next()) / 34.f;
			const float Clump = 0.5f + 0.5f * std::sin(X * 0.045f + 1.3f);
			Spec.Root = FVector(X, 0.0, 0.0);
			Spec.Along = FVector(1.0, 0.0, 0.0);
			Spec.Length = 6.f + 54.f * Clump * (0.35f + 0.65f * Next());
			Spec.Radius = FMath::Clamp(Spec.Length * (0.06f + 0.04f * Next()), 0.5f, 4.f);
			Spec.Down = FVector(0.08 * (Next() - 0.5), 0.05, -1.0).GetSafeNormal();
			Spec.Bend = 0.3f;
			Spec.Frost = 0.3f * Next();
			Spec.Seed = 1000u + uint32(Index) * 7919u;
			Spec.Sides = 8;
			Icify::AppendIcicle(Spec, Origin, Mesh);
		}
		FILE* File = std::fopen(Path, "w");
		if (!File)
		{
			std::printf("FAIL could not write %s\n", Path);
			++Failures;
			return;
		}
		for (int32 V = 0; V < Mesh.NumVertices(); ++V)
		{
			std::fprintf(File, "v %.4f %.4f %.4f\n", Mesh.Positions[V].X, Mesh.Positions[V].Y, Mesh.Positions[V].Z);
		}
		for (int32 V = 0; V < Mesh.NumVertices(); ++V)
		{
			std::fprintf(File, "vn %.4f %.4f %.4f\n", Mesh.Normals[V].X, Mesh.Normals[V].Y, Mesh.Normals[V].Z);
		}
		for (int32 T = 0; T < Mesh.NumTriangles(); ++T)
		{
			const int32 A = Mesh.Triangles[T * 3] + 1, B = Mesh.Triangles[T * 3 + 1] + 1, C = Mesh.Triangles[T * 3 + 2] + 1;
			std::fprintf(File, "f %d//%d %d//%d %d//%d\n", A, A, B, B, C, C);
		}
		std::fclose(File);
		std::printf("     wrote %s: %d vertices, %d triangles\n", Path, Mesh.NumVertices(), Mesh.NumTriangles());
	}
}

/** --specs In.txt Out.obj: one icicle per line (root xyz, down xyz, along xyz, length, radius, ripple, frost, bend, seed, sides). */
int BuildFromSpecs(const char* InPath, const char* OutPath)
{
	FILE* In = std::fopen(InPath, "r");
	if (!In)
	{
		std::printf("FAIL could not read %s\n", InPath);
		return 1;
	}
	FIcifyIcicleMesh Mesh;
	int Count = 0;
	double V[16];
	while (std::fscanf(In, "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf",
		&V[0], &V[1], &V[2], &V[3], &V[4], &V[5], &V[6], &V[7], &V[8], &V[9], &V[10], &V[11], &V[12], &V[13], &V[14], &V[15]) == 16)
	{
		FIcifyIcicleSpec Spec;
		Spec.Root = FVector(V[0], V[1], V[2]);
		Spec.Down = FVector(V[3], V[4], V[5]);
		Spec.Along = FVector(V[6], V[7], V[8]);
		Spec.Length = float(V[9]);
		Spec.Radius = float(V[10]);
		Spec.RippleCm = float(V[11]);
		Spec.Frost = float(V[12]);
		Spec.Bend = float(V[13]);
		Spec.Seed = uint32(V[14]);
		Spec.Sides = int32(V[15]);
		Count += Icify::AppendIcicle(Spec, FVector::ZeroVector, Mesh) > 0 ? 1 : 0;
	}
	std::fclose(In);
	FILE* Out = std::fopen(OutPath, "w");
	if (!Out)
	{
		return 1;
	}
	for (int32 I = 0; I < Mesh.NumVertices(); ++I)
	{
		std::fprintf(Out, "v %.4f %.4f %.4f\n", Mesh.Positions[I].X, Mesh.Positions[I].Y, Mesh.Positions[I].Z);
	}
	for (int32 I = 0; I < Mesh.NumVertices(); ++I)
	{
		std::fprintf(Out, "vn %.4f %.4f %.4f\n", Mesh.Normals[I].X, Mesh.Normals[I].Y, Mesh.Normals[I].Z);
	}
	for (int32 T = 0; T < Mesh.NumTriangles(); ++T)
	{
		const int32 A = Mesh.Triangles[T * 3] + 1, B = Mesh.Triangles[T * 3 + 1] + 1, C = Mesh.Triangles[T * 3 + 2] + 1;
		std::fprintf(Out, "f %d//%d %d//%d %d//%d\n", A, A, B, B, C, C);
	}
	std::fclose(Out);
	std::printf("     built %d icicles: %d vertices, %d triangles -> %s\n", Count, Mesh.NumVertices(), Mesh.NumTriangles(), OutPath);
	return 0;
}

int main(int Argc, char** Argv)
{
	if (Argc == 4 && std::string(Argv[1]) == "--specs")
	{
		return BuildFromSpecs(Argv[2], Argv[3]);
	}
	const float Lengths[] = { 1.f, 4.f, 12.f, 45.f, 160.f };
	const float Radii[] = { 0.3f, 1.5f, 5.f };
	const int32 Sides[] = { 4, 8, 16 };
	int Checked = 0;
	for (float Length : Lengths)
	{
		for (float Radius : Radii)
		{
			for (int32 SideCount : Sides)
			{
				FIcifyIcicleSpec Spec;
				Spec.Root = FVector(1010.0, -1990.0, 280.0);
				Spec.Length = Length;
				Spec.Radius = Radius;
				Spec.Sides = SideCount;
				Spec.Bend = (Checked % 3) * 0.5f;
				Spec.Along = (Checked % 2) ? FVector(0.6, 0.8, 0.0) : FVector::ZeroVector;
				Spec.Down = (Checked % 4) ? FVector(0.2, -0.1, -1.0).GetSafeNormal() : FVector(0.0, 0.0, -1.0);
				Spec.RippleCm = (Checked % 5) ? 1.2f : 0.f;
				Spec.Frost = 0.5f;
				Spec.Seed = 77u + uint32(Checked) * 131u;
				CheckIcicle(Spec);
				++Checked;
			}
		}
	}

	FIcifyIcicleSpec Degenerate;
	Degenerate.Length = 0.f;
	FIcifyIcicleMesh Empty;
	Check(Icify::AppendIcicle(Degenerate, FVector::ZeroVector, Empty) == 0 && Empty.NumVertices() == 0, "a zero-length spec adds nothing");
	Degenerate.Length = 10.f;
	Degenerate.Radius = 0.f;
	Check(Icify::AppendIcicle(Degenerate, FVector::ZeroVector, Empty) == 0 && Empty.NumVertices() == 0, "a zero-radius spec adds nothing");

	// Appending keeps earlier icicles intact and offsets indices.
	{
		FIcifyIcicleSpec A, B;
		A.Seed = 1u;
		B.Seed = 2u;
		B.Root = FVector(50.0, 0.0, 0.0);
		FIcifyIcicleMesh Both;
		const int32 First = Icify::AppendIcicle(A, FVector::ZeroVector, Both);
		const int32 FirstTriangles = Both.NumTriangles();
		Icify::AppendIcicle(B, FVector::ZeroVector, Both);
		bool bOffset = true;
		for (int32 T = FirstTriangles; T < Both.NumTriangles(); ++T)
		{
			bOffset &= Both.Triangles[T * 3] >= First && Both.Triangles[T * 3 + 1] >= First && Both.Triangles[T * 3 + 2] >= First;
		}
		Check(bOffset, "a second icicle's triangles index only its own vertices");
	}

	if (Argc > 1)
	{
		WriteObj(Argv[1]);
	}
	std::printf("%s: %d icicle specs checked, %d failure(s)\n", Failures == 0 ? "ICICLES OK" : "ICICLES FAIL", Checked, Failures);
	return Failures == 0 ? 0 : 1;
}
