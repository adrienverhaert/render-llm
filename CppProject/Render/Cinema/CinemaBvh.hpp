#pragma once
#include "Common.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// CinemaBvh: CPU-side BVH2 builder and reference ray traversal for the Cinema
// path tracer (HEADLESS/CINEMA_DESIGN.md, sections 3 and 5).
//
// This module is pure CPU: it has NO D3D dependencies and compiles in isolation.
// The arrays it produces (nodes, tris) use the exact byte layout the GPU kernels
// consume, so CinemaRender can upload them to structured buffers without any
// conversion. The traversal implemented here (TraceClosest/TraceShadow) is the
// reference the HLSL kernel must mirror, and the base of the CinemaOracle
// correctness tests (CinemaBvhSelftest).

namespace CppProject
{
	// ------------------------------------------------------------------------
	// GPU-shared structs (CINEMA_DESIGN.md section 3). The HLSL side declares
	// identical layouts; sizes are locked below with static_assert. All fields
	// are 4-byte scalars: no compiler padding is possible, the layout is exact.
	// ------------------------------------------------------------------------

	// Leaf mark: bit 31 of PTNode::leftOrFirst.
	#define PT_NODE_LEAF_BIT 0x80000000u
	// Leaves hold 2-4 triangles (1 only in degenerate or tiny scenes).
	#define PT_BVH_MAX_LEAF_TRIS 4
	// Bins per axis for the SAH split search.
	#define PT_BVH_SAH_BINS 16
	// Fixed traversal stack size, shared with the HLSL kernel.
	#define PT_BVH_TRACE_STACK 48
	// Past this depth the builder forces median splits, which bounds the total
	// tree depth to PT_BVH_MEDIAN_DEPTH + ceil(log2(count)) and guarantees the
	// traversal stack can never overflow for any realistic scene size.
	#define PT_BVH_MEDIAN_DEPTH 24
	// Miss marker for CinemaBvhHit::triIdx.
	#define PT_BVH_NO_HIT 0xFFFFFFFFu

	// Path tracer vertex, 48 bytes (3 x 16: fetchable as float4/uint4 triplets).
	// Filled by CinemaScene during the flatten (positions already in world space,
	// UVs RAW from the source vertex -- the atlas remap happens in the kernel
	// via PTMaterial.uvRect -- color after the vertex AO policy).
	struct PTVert
	{
		float px, py, pz;    // position in world space
		uint32_t normal888;  // packed 8:8:8 normal, engine encoding, re-normalized after the world matrix
		float u, v;          // RAW source UVs (kernel remaps into the atlas via PTMaterial.uvRect)
		uint32_t colorABGR;  // vertex color after the AO policy (section 7.1); alpha in byte 3
		uint32_t dataFlags;  // bit2 SUBSURFACE + byte1 emissive, copied from the source vertex
		uint32_t tangent888; // packed 8:8:8 tangent (normal maps, M2)
		uint32_t pad0, pad1, pad2; // pad to 48 bytes, always 0
	};

	// Path tracer triangle, 16 bytes. Indices into the PTVert array.
	struct PTTri
	{
		uint32_t i0, i1, i2;
		uint32_t matId; // index into the PTMaterial table (CinemaScene)
	};

	// BVH2 node, 32 bytes. Exact layout, mirrored in HLSL:
	//   float3 bmin | uint leftOrFirst | float3 bmax | uint rightOrCount
	// Inner node (bit 31 of leftOrFirst CLEAR):
	//   leftOrFirst  = index of the left child in the node array
	//   rightOrCount = index of the right child in the node array
	// Leaf node (bit 31 of leftOrFirst SET):
	//   leftOrFirst  = PT_NODE_LEAF_BIT | first triangle offset
	//   rightOrCount = triangle count (1..PT_BVH_MAX_LEAF_TRIS)
	// The first-triangle offset indexes the REORDERED triangle array
	// (CinemaBvh::tris), which is laid out in leaf order after the build.
	// Node 0 is always the root; a 1-triangle scene yields a single leaf root.
	struct PTNode
	{
		float bminx, bminy, bminz;
		uint32_t leftOrFirst;
		float bmaxx, bmaxy, bmaxz;
		uint32_t rightOrCount;
	};

	static_assert(sizeof(PTVert) == 48, "PTVert must be 48 bytes (GPU layout)");
	static_assert(sizeof(PTTri) == 16, "PTTri must be 16 bytes (GPU layout)");
	static_assert(sizeof(PTNode) == 32, "PTNode must be 32 bytes (GPU layout)");
	static_assert(offsetof(PTNode, leftOrFirst) == 12, "PTNode layout mismatch");
	static_assert(offsetof(PTNode, rightOrCount) == 28, "PTNode layout mismatch");

	// ------------------------------------------------------------------------
	// CPU-side BVH container and reference traversal
	// ------------------------------------------------------------------------

	// Ray for the reference traversal. The direction does not need to be
	// normalized; t values are expressed in units of the direction length.
	// tMin excludes self-intersections (the caller scales it to the scene).
	struct CinemaBvhRay
	{
		float ox, oy, oz; // origin
		float dx, dy, dz; // direction
		float tMin, tMax; // accepted hit interval (tMin, tMax)
	};

	// Result of CinemaBvhTrace. triIdx indexes the REORDERED triangle array
	// (CinemaBvh::tris); use CinemaBvh::triSource to map back to the input
	// order. On a miss, triIdx == PT_BVH_NO_HIT and t == ray.tMax.
	// u/v are the Moller-Trumbore barycentrics of i1/i2 (weight of i0 = 1-u-v).
	struct CinemaBvhHit
	{
		float t;
		uint32_t triIdx;
		float u, v;
	};

	// Built BVH. nodes and tris are GPU-uploadable as-is (upload tris, NOT the
	// original input array: triangles are reordered by leaf for cache coherence).
	struct CinemaBvh
	{
		std::vector<PTNode> nodes;       // node 0 is the root
		std::vector<PTTri> tris;         // reordered by leaf order
		std::vector<uint32_t> triSource; // reordered position -> original triangle index
		const PTVert* verts = nullptr;   // non-owning; caller keeps the vertex array alive
		IntType numVerts = 0;
		IntType numLeaves = 0;
		IntType maxDepth = 0;

		BoolType IsValid() const { return nodes.size() && verts; }

		void Clear()
		{
			nodes.clear();
			tris.clear();
			triSource.clear();
			verts = nullptr;
			numVerts = 0;
			numLeaves = 0;
			maxDepth = 0;
		}
	};

	// Builds a BVH2 with top-down binned SAH (PT_BVH_SAH_BINS bins per axis,
	// leaves of up to PT_BVH_MAX_LEAF_TRIS triangles), partitioning an index
	// array in place and reordering the triangles by leaf order at the end.
	// The upper levels are built in parallel (OpenMP) with a deterministic
	// subtree splice: the resulting layout never depends on thread scheduling.
	// verts is stored as a non-owning pointer and must outlive the traces.
	// Returns false on empty input or out-of-range triangle indices.
	BoolType CinemaBvhBuild(CinemaBvh& bvh, const PTVert* verts, IntType numVerts, const PTTri* tris, IntType numTris);

	// Reference closest-hit traversal (TraceClosest): near-first ordered with a
	// fixed stack of PT_BVH_TRACE_STACK entries, Moller-Trumbore with relative
	// epsilons. Purely geometric: alpha-test continuation lives in the GPU
	// kernel, which samples the atlas (this module has no texture access).
	CinemaBvhHit CinemaBvhTrace(const CinemaBvh& bvh, const CinemaBvhRay& ray);

	// Reference shadow traversal (TraceShadow): returns true if any triangle
	// occludes the interval (tMin, tMax), early-out on the first hit.
	BoolType CinemaBvhTraceShadow(const CinemaBvh& bvh, const CinemaBvhRay& ray);

	// Correctness oracle: builds a deterministic procedural scene (500 random
	// triangles, fixed seed), traces 1000 deterministic rays and compares
	// CinemaBvhTrace against an O(n) brute force over all triangles, plus a
	// TraceShadow consistency check. Returns true when there are no mismatches;
	// the mismatch count is written to outMismatches when provided.
	BoolType CinemaBvhSelftest(IntType* outMismatches = nullptr);
}
