// Copyright Fallen Signal Studios. All Rights Reserved.
//
// A tiny stand-in for the parts of Unreal's CoreMinimal.h that Source/Icify/Private/IcifyIcicleBuilder.cpp uses, so
// the real builder source compiles and runs without the engine (Scripts/CheckIcicles.py). Semantics follow Unreal:
// FVector is double precision, TArray::Reset keeps capacity, FMath::SmoothStep is Hermite on a clamped range.
// It is not a general replacement for the engine headers.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

#define ICIFY_API

typedef int32_t int32;
typedef uint32_t uint32;
typedef uint8_t uint8;
typedef int64_t int64;

template <typename T>
class TArray
{
public:
	TArray() = default;
	TArray(std::initializer_list<T> List) : Data(List) {}
	int32 Num() const { return int32(Data.size()); }
	int32 Add(const T& Item) { Data.push_back(Item); return int32(Data.size()) - 1; }
	void Append(std::initializer_list<T> List) { Data.insert(Data.end(), List.begin(), List.end()); }
	void Append(const TArray& Other) { Data.insert(Data.end(), Other.Data.begin(), Other.Data.end()); }
	void Reset() { Data.clear(); }
	void Reserve(int32 Count) { Data.reserve(size_t(Count)); }
	void SetNum(int32 Count) { Data.resize(size_t(Count)); }
	bool IsValidIndex(int32 Index) const { return Index >= 0 && Index < Num(); }
	T& operator[](int32 Index) { return Data[size_t(Index)]; }
	const T& operator[](int32 Index) const { return Data[size_t(Index)]; }
	T* GetData() { return Data.data(); }
	const T* GetData() const { return Data.data(); }
	typename std::vector<T>::iterator begin() { return Data.begin(); }
	typename std::vector<T>::iterator end() { return Data.end(); }
	typename std::vector<T>::const_iterator begin() const { return Data.begin(); }
	typename std::vector<T>::const_iterator end() const { return Data.end(); }

private:
	std::vector<T> Data;
};

struct FVector
{
	double X = 0, Y = 0, Z = 0;
	FVector() = default;
	FVector(double InX, double InY, double InZ) : X(InX), Y(InY), Z(InZ) {}
	explicit FVector(double V) : X(V), Y(V), Z(V) {}

	FVector operator+(const FVector& O) const { return FVector(X + O.X, Y + O.Y, Z + O.Z); }
	FVector operator-(const FVector& O) const { return FVector(X - O.X, Y - O.Y, Z - O.Z); }
	FVector operator-() const { return FVector(-X, -Y, -Z); }
	FVector operator*(double S) const { return FVector(X * S, Y * S, Z * S); }
	FVector operator/(double S) const { return FVector(X / S, Y / S, Z / S); }
	FVector& operator+=(const FVector& O) { X += O.X; Y += O.Y; Z += O.Z; return *this; }
	bool operator==(const FVector& O) const { return X == O.X && Y == O.Y && Z == O.Z; }

	double SizeSquared() const { return X * X + Y * Y + Z * Z; }
	double Size() const { return std::sqrt(SizeSquared()); }
	bool IsZero() const { return X == 0 && Y == 0 && Z == 0; }
	bool IsNearlyZero(double Tolerance = 1.e-4) const { return std::fabs(X) <= Tolerance && std::fabs(Y) <= Tolerance && std::fabs(Z) <= Tolerance; }
	FVector GetSafeNormal(double Tolerance = 1.e-8) const
	{
		const double Square = SizeSquared();
		if (Square == 1.0) { return *this; }
		if (Square < Tolerance) { return FVector(0, 0, 0); }
		return *this * (1.0 / std::sqrt(Square));
	}

	static double DotProduct(const FVector& A, const FVector& B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }
	static FVector CrossProduct(const FVector& A, const FVector& B)
	{
		return FVector(A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X);
	}
	static double Dist(const FVector& A, const FVector& B) { return (A - B).Size(); }

	static const FVector ZeroVector;
	static const FVector UpVector;
	static const FVector ForwardVector;
	static const FVector RightVector;
};
inline const FVector FVector::ZeroVector(0, 0, 0);
inline const FVector FVector::UpVector(0, 0, 1);
inline const FVector FVector::ForwardVector(1, 0, 0);
inline const FVector FVector::RightVector(0, 1, 0);
inline FVector operator*(double S, const FVector& V) { return V * S; }

struct FVector2D
{
	double X = 0, Y = 0;
	FVector2D() = default;
	FVector2D(double InX, double InY) : X(InX), Y(InY) {}
};

struct FLinearColor
{
	float R = 0, G = 0, B = 0, A = 1;
	FLinearColor() = default;
	FLinearColor(float InR, float InG, float InB, float InA = 1.f) : R(InR), G(InG), B(InB), A(InA) {}
};

struct FMath
{
	template <typename T> static T Clamp(T V, T Lo, T Hi) { return V < Lo ? Lo : (V > Hi ? Hi : V); }
	template <typename T> static T Min(T A, T B) { return A < B ? A : B; }
	template <typename T> static T Max(T A, T B) { return A > B ? A : B; }
	template <typename T> static T Abs(T A) { return A < 0 ? -A : A; }
	template <typename T> static T Square(T A) { return A * A; }
	static float Sqrt(float V) { return std::sqrt(V); }
	static double Sqrt(double V) { return std::sqrt(V); }
	static float Sin(float V) { return std::sin(V); }
	static float Cos(float V) { return std::cos(V); }
	static float Pow(float A, float B) { return std::pow(A, B); }
	static float FloorToFloat(float V) { return std::floor(V); }
	static int32 FloorToInt(float V) { return int32(std::floor(V)); }
	static int32 CeilToInt(float V) { return int32(std::ceil(V)); }
	static int32 RoundToInt(float V) { return int32(std::floor(V + 0.5f)); }
	template <typename T, typename U> static T Lerp(const T& A, const T& B, const U& Alpha) { return T(A + (B - A) * Alpha); }
	static float SmoothStep(float A, float B, float X)
	{
		if (X < A) { return 0.f; }
		if (X >= B) { return 1.f; }
		const float T = (X - A) / (B - A);
		return T * T * (3.f - 2.f * T);
	}
};
