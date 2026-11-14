// Engine stub for tools/voxelize_test: just enough of the engine's types for
// src/common/models/voxels.cpp to compile UNCHANGED outside the engine, so the
// round-trip test runs the engine's own KVX parser (R_LoadKVX). Not engine code.
#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>
#include <cmath>

template<class T> class TArray
{
public:
	void Resize(unsigned n) { v.resize(n); }
	T *Data() { return v.data(); }
	const T *Data() const { return v.data(); }
	unsigned Size() const { return (unsigned)v.size(); }
	unsigned Push(const T &x) { v.push_back(x); return (unsigned)v.size() - 1; }
	void Reset() { v.clear(); }
	T &operator[](size_t i) { return v[i]; }
	T &operator[](size_t i) const { return v[i]; }   // as the engine's TArray: const access yields T&
private:
	mutable std::vector<T> v;
};

template<class T> class TDeletingArray : public TArray<T>
{
public:
	~TDeletingArray() { for (unsigned i = 0; i < this->Size(); i++) delete (*this)[i]; }
};

struct DVector3
{
	double X = 0, Y = 0, Z = 0;
	void Zero() { X = Y = Z = 0; }
	DVector3 operator/(double d) const { DVector3 r; r.X = X / d; r.Y = Y / d; r.Z = Z / d; return r; }
};

struct DAngle
{
	double deg = 0;
	static DAngle fromDeg(double d) { DAngle a; a.deg = d; return a; }
};

struct PalEntry { uint8_t b, g, r, a; };
struct FPaletteStub { PalEntry BaseColors[256]; };
extern FPaletteStub GPalette;

// The engine's ColorMatcher picks the nearest GAME palette entry; the stub
// returns 0. Only R_LoadKVX is under test, which does not call it.
struct FColorMatcherStub { int Pick(int, int, int) { return 0; } };
extern FColorMatcherStub ColorMatcher;
