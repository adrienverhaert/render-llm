#include "Render/Cinema/CinemaBvh.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace CppProject
{
	namespace
	{
		// --------------------------------------------------------------------
		// Small file-local math helpers
		// --------------------------------------------------------------------

		struct Vec3f
		{
			float x, y, z;

			float Comp(IntType axis) const
			{
				return axis == 0 ? x : (axis == 1 ? y : z);
			}
		};

		inline Vec3f Sub(const Vec3f& a, const Vec3f& b)
		{
			return { a.x - b.x, a.y - b.y, a.z - b.z };
		}

		inline Vec3f Cross(const Vec3f& a, const Vec3f& b)
		{
			return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
		}

		inline float Dot(const Vec3f& a, const Vec3f& b)
		{
			return a.x * b.x + a.y * b.y + a.z * b.z;
		}

		inline float MaxAbsComp(const Vec3f& a)
		{
			return fmaxf(fabsf(a.x), fmaxf(fabsf(a.y), fabsf(a.z)));
		}

		inline Vec3f VertPos(const PTVert& v)
		{
			return { v.px, v.py, v.pz };
		}

		struct Aabb
		{
			Vec3f bmin = { FLT_MAX, FLT_MAX, FLT_MAX };
			Vec3f bmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

			void Grow(const Vec3f& p)
			{
				bmin.x = fminf(bmin.x, p.x); bmin.y = fminf(bmin.y, p.y); bmin.z = fminf(bmin.z, p.z);
				bmax.x = fmaxf(bmax.x, p.x); bmax.y = fmaxf(bmax.y, p.y); bmax.z = fmaxf(bmax.z, p.z);
			}

			void Grow(const Aabb& o)
			{
				bmin.x = fminf(bmin.x, o.bmin.x); bmin.y = fminf(bmin.y, o.bmin.y); bmin.z = fminf(bmin.z, o.bmin.z);
				bmax.x = fmaxf(bmax.x, o.bmax.x); bmax.y = fmaxf(bmax.y, o.bmax.y); bmax.z = fmaxf(bmax.z, o.bmax.z);
			}

			float Extent(IntType axis) const
			{
				return bmax.Comp(axis) - bmin.Comp(axis);
			}

			float Area() const
			{
				if (bmax.x < bmin.x)
					return 0.0f; // empty
				float ex = bmax.x - bmin.x, ey = bmax.y - bmin.y, ez = bmax.z - bmin.z;
				return 2.0f * (ex * ey + ey * ez + ez * ex);
			}
		};

		// --------------------------------------------------------------------
		// Intersection primitives. The HLSL kernel must mirror these exactly
		// (same epsilons) so the CPU oracle stays a valid reference.
		// --------------------------------------------------------------------

		// Parallel-ray rejection threshold, relative to the operand scale.
		const float kDetEpsRel = 4.0f * FLT_EPSILON;
		// Relative epsilon on the accepted hit distance (self-intersection guard
		// that scales with the distance instead of using a fixed world offset).
		const float kTEpsRel = 1e-6f;

		// Moller-Trumbore with relative epsilons. Accepts hits in (tMin, tMax).
		inline BoolType IntersectTri(const PTVert* verts, const PTTri& tri, const Vec3f& o, const Vec3f& d,
			float tMin, float tMax, float& outT, float& outU, float& outV)
		{
			Vec3f p0 = VertPos(verts[tri.i0]);
			Vec3f p1 = VertPos(verts[tri.i1]);
			Vec3f p2 = VertPos(verts[tri.i2]);
			Vec3f e1 = Sub(p1, p0);
			Vec3f e2 = Sub(p2, p0);

			Vec3f pv = Cross(d, e2);
			float det = Dot(e1, pv);

			// Relative epsilon: the det magnitude scales with |e1||e2||d|, so the
			// threshold does too (a fixed absolute epsilon breaks on tiny or huge
			// geometry). Near-parallel and degenerate triangles are rejected.
			float detEps = kDetEpsRel * MaxAbsComp(e1) * MaxAbsComp(e2) * MaxAbsComp(d);
			if (fabsf(det) <= detEps)
				return false;

			float inv = 1.0f / det;
			Vec3f tv = Sub(o, p0);
			float u = Dot(tv, pv) * inv;
			if (u < 0.0f || u > 1.0f)
				return false;

			Vec3f qv = Cross(tv, e1);
			float v = Dot(d, qv) * inv;
			if (v < 0.0f || u + v > 1.0f)
				return false;

			float t = Dot(e2, qv) * inv;

			// Relative epsilon in t; the negated comparison also rejects NaN.
			if (!(t > tMin + kTEpsRel * fabsf(t)) || t >= tMax)
				return false;

			outT = t;
			outU = u;
			outV = v;
			return true;
		}

		// Ray vs node AABB slab test against the interval (tMin, tMax). Writes
		// the (clamped) entry distance for near-first ordering when it hits.
		// invD components may be +-inf (axis-aligned rays): the 0 * inf = NaN
		// case is resolved correctly because fminf/fmaxf return the non-NaN
		// operand (IEEE-754 semantics of fmin/fmax).
		inline BoolType SlabHit(const PTNode& n, const Vec3f& o, const Vec3f& invD, float tMin, float tMax, float& outEntry)
		{
			float t1 = (n.bminx - o.x) * invD.x;
			float t2 = (n.bmaxx - o.x) * invD.x;
			float tlo = fminf(t1, t2);
			float thi = fmaxf(t1, t2);

			t1 = (n.bminy - o.y) * invD.y;
			t2 = (n.bmaxy - o.y) * invD.y;
			tlo = fmaxf(tlo, fminf(t1, t2));
			thi = fminf(thi, fmaxf(t1, t2));

			t1 = (n.bminz - o.z) * invD.z;
			t2 = (n.bmaxz - o.z) * invD.z;
			tlo = fmaxf(tlo, fminf(t1, t2));
			thi = fminf(thi, fmaxf(t1, t2));

			outEntry = fmaxf(tlo, tMin);
			return tlo <= thi && tlo < tMax && thi > tMin;
		}

		// The build bounds the depth (PT_BVH_MEDIAN_DEPTH), so overflowing the
		// fixed traversal stack should be impossible; warn once if it ever is.
		inline void WarnStackOverflow()
		{
			static BoolType warned = false;
			if (!warned)
			{
				warned = true;
				WARNING("BVH traversal stack overflow, results may miss geometry");
			}
		}

		// --------------------------------------------------------------------
		// Build internals
		// --------------------------------------------------------------------

		struct BuildCtx
		{
			const PTTri* tris;
			const Aabb* triBounds;
			const Vec3f* centroids;
			uint32_t* order; // triangle index array, partitioned in place
		};

		// Output of one (sub)tree build. Subtrees use local node indices and
		// local triangle offsets; the splice pass remaps them.
		struct BuildOutput
		{
			std::vector<PTNode> nodes;
			std::vector<uint32_t> triOrder; // original triangle indices, in leaf order
			IntType numLeaves = 0;
			IntType maxDepth = 0;
		};

		// Range deferred by the sequential top pass for the parallel subtree pass.
		struct PendingSubtree
		{
			uint32_t nodeIdx; // placeholder node in the main output
			IntType start, end, depth;
		};

		inline IntType BinOf(float c, float cbMin, float scale)
		{
			IntType b = (IntType)((c - cbMin) * scale);
			return b < 0 ? 0 : (b >= PT_BVH_SAH_BINS ? PT_BVH_SAH_BINS - 1 : b);
		}

		// Binned SAH split: searches PT_BVH_SAH_BINS bins on each axis over the
		// centroid bounds and partitions order[start, end) in place at the best
		// split. Returns false when no usable split exists (degenerate centroid
		// bounds or a one-sided partition): the caller falls back to a median.
		BoolType SahSplit(const BuildCtx& ctx, IntType start, IntType end, const Aabb& cb, IntType& outMid)
		{
			IntType bestAxis = -1, bestBin = -1;
			float bestCost = FLT_MAX, bestScale = 0.0f, bestMin = 0.0f;

			for (IntType axis = 0; axis < 3; axis++)
			{
				float extent = cb.Extent(axis);
				if (!(extent > 0.0f))
					continue;

				float cbMin = cb.bmin.Comp(axis);
				float scale = (float)PT_BVH_SAH_BINS / extent;

				// A denormal extent overflows the division to +inf, and then
				// (c - cbMin) * inf produces NaN for the minimum centroid;
				// casting NaN to an integer in BinOf is undefined. Treat the
				// axis as degenerate instead (the median fallback covers it).
				if (!std::isfinite(scale))
					continue;

				Aabb binBounds[PT_BVH_SAH_BINS];
				IntType binCount[PT_BVH_SAH_BINS] = {};
				for (IntType i = start; i < end; i++)
				{
					uint32_t t = ctx.order[i];
					IntType b = BinOf(ctx.centroids[t].Comp(axis), cbMin, scale);
					binCount[b]++;
					binBounds[b].Grow(ctx.triBounds[t]);
				}

				// Suffix sweep: bounds area and count of everything right of each bin
				float rightArea[PT_BVH_SAH_BINS];
				IntType rightCount[PT_BVH_SAH_BINS];
				Aabb accR;
				IntType cntR = 0;
				for (IntType b = PT_BVH_SAH_BINS - 1; b > 0; b--)
				{
					accR.Grow(binBounds[b]);
					cntR += binCount[b];
					rightArea[b] = accR.Area();
					rightCount[b] = cntR;
				}

				// Prefix sweep: evaluate the SAH cost of each of the 15 split planes
				Aabb accL;
				IntType cntL = 0;
				for (IntType s = 0; s < PT_BVH_SAH_BINS - 1; s++)
				{
					accL.Grow(binBounds[s]);
					cntL += binCount[s];
					if (!cntL || !rightCount[s + 1])
						continue;

					float cost = accL.Area() * (float)cntL + rightArea[s + 1] * (float)rightCount[s + 1];
					if (cost < bestCost)
					{
						bestCost = cost;
						bestAxis = axis;
						bestBin = s;
						bestScale = scale;
						bestMin = cbMin;
					}
				}
			}

			if (bestAxis < 0)
				return false;

			const Vec3f* centroids = ctx.centroids;
			uint32_t* mid = std::partition(ctx.order + start, ctx.order + end,
				[&](uint32_t t) { return BinOf(centroids[t].Comp(bestAxis), bestMin, bestScale) <= bestBin; });

			outMid = (IntType)(mid - ctx.order);
			return outMid > start && outMid < end;
		}

		// Fallback split: index median along the widest centroid axis.
		void MedianSplit(const BuildCtx& ctx, IntType start, IntType end, const Aabb& cb, IntType& outMid)
		{
			IntType axis = 0;
			if (cb.Extent(1) > cb.Extent(axis))
				axis = 1;
			if (cb.Extent(2) > cb.Extent(axis))
				axis = 2;

			outMid = (start + end) / 2;
			const Vec3f* centroids = ctx.centroids;
			std::nth_element(ctx.order + start, ctx.order + outMid, ctx.order + end,
				[&](uint32_t a, uint32_t b) { return centroids[a].Comp(axis) < centroids[b].Comp(axis); });
		}

		// Builds the subtree over order[start, end); returns its node index in
		// out.nodes. When pending is given and the recursion reaches cutDepth,
		// the range is emitted as a placeholder node and deferred for the
		// parallel subtree pass instead of being built here.
		uint32_t BuildRange(const BuildCtx& ctx, BuildOutput& out, IntType start, IntType end,
			IntType depth, IntType cutDepth, std::vector<PendingSubtree>* pending)
		{
			// Node bounds from triangle bounds; centroid bounds for the split
			Aabb bounds, cb;
			for (IntType i = start; i < end; i++)
			{
				uint32_t t = ctx.order[i];
				bounds.Grow(ctx.triBounds[t]);
				cb.Grow(ctx.centroids[t]);
			}

			uint32_t nodeIdx = (uint32_t)out.nodes.size();
			PTNode node;
			node.bminx = bounds.bmin.x; node.bminy = bounds.bmin.y; node.bminz = bounds.bmin.z;
			node.bmaxx = bounds.bmax.x; node.bmaxy = bounds.bmax.y; node.bmaxz = bounds.bmax.z;
			node.leftOrFirst = 0;
			node.rightOrCount = 0;
			out.nodes.push_back(node);

			IntType count = end - start;
			if (count <= PT_BVH_MAX_LEAF_TRIS)
			{
				// Leaf: append the range to the leaf-ordered triangle list
				uint32_t first = (uint32_t)out.triOrder.size();
				for (IntType i = start; i < end; i++)
					out.triOrder.push_back(ctx.order[i]);

				out.nodes[nodeIdx].leftOrFirst = PT_NODE_LEAF_BIT | first;
				out.nodes[nodeIdx].rightOrCount = (uint32_t)count;
				out.numLeaves++;
				if (depth > out.maxDepth)
					out.maxDepth = depth;
				return nodeIdx;
			}

			if (pending && depth >= cutDepth)
			{
				// Defer: the placeholder keeps the bounds, children get patched
				// in by the splice pass
				pending->push_back({ nodeIdx, start, end, depth });
				return nodeIdx;
			}

			IntType mid = 0;
			if (depth >= PT_BVH_MEDIAN_DEPTH || !SahSplit(ctx, start, end, cb, mid))
				MedianSplit(ctx, start, end, cb, mid);

			uint32_t left = BuildRange(ctx, out, start, mid, depth + 1, cutDepth, pending);
			uint32_t right = BuildRange(ctx, out, mid, end, depth + 1, cutDepth, pending);
			out.nodes[nodeIdx].leftOrFirst = left;
			out.nodes[nodeIdx].rightOrCount = right;
			return nodeIdx;
		}
	}

	// ------------------------------------------------------------------------
	// Build
	// ------------------------------------------------------------------------

	BoolType CinemaBvhBuild(CinemaBvh& bvh, const PTVert* verts, IntType numVerts, const PTTri* tris, IntType numTris)
	{
		bvh.Clear();

		if (!verts || !tris || numVerts <= 0 || numTris <= 0)
		{
			WARNING("Empty BVH input");
			return false;
		}

		// Validate indices up front so a corrupt flatten cannot crash the build
		for (IntType i = 0; i < numTris; i++)
		{
			const PTTri& t = tris[i];
			if (t.i0 >= (uint64_t)numVerts || t.i1 >= (uint64_t)numVerts || t.i2 >= (uint64_t)numVerts)
			{
				WARNING("Triangle index out of range");
				return false;
			}
		}

		// Precompute per-triangle bounds and centroids
		std::vector<Aabb> triBounds(numTris);
		std::vector<Vec3f> centroids(numTris);
		std::vector<uint32_t> order(numTris);

		#pragma OPENMP_FOR
		for (IntType i = 0; i < numTris; i++)
		{
			Vec3f p0 = VertPos(verts[tris[i].i0]);
			Vec3f p1 = VertPos(verts[tris[i].i1]);
			Vec3f p2 = VertPos(verts[tris[i].i2]);

			Aabb b;
			b.Grow(p0);
			b.Grow(p1);
			b.Grow(p2);
			triBounds[i] = b;
			centroids[i] = { (p0.x + p1.x + p2.x) * (1.0f / 3.0f),
				(p0.y + p1.y + p2.y) * (1.0f / 3.0f),
				(p0.z + p1.z + p2.z) * (1.0f / 3.0f) };
			order[i] = (uint32_t)i;
		}

		BuildCtx ctx = { tris, triBounds.data(), centroids.data(), order.data() };

		BuildOutput out;
		out.nodes.reserve(numTris);
		out.triOrder.reserve(numTris);

		// Sequential top of the tree; large ranges below cutDepth are deferred
		// and built in parallel. The subtree list and the splice both follow the
		// recursion order, so the final layout is deterministic regardless of
		// thread scheduling (same input -> bit-identical BVH).
		std::vector<PendingSubtree> pending;
		IntType cutDepth = -1;
		if (numTris >= 16384)
		{
			IntType threads = omp_get_max_threads();
			cutDepth = 2;
			while ((IntType(1) << cutDepth) < threads * 2 && cutDepth < 6)
				cutDepth++;
		}

		BuildRange(ctx, out, 0, numTris, 0, cutDepth, cutDepth >= 0 ? &pending : nullptr);

		if (pending.size())
		{
			std::vector<BuildOutput> sub(pending.size());

			#pragma OPENMP_FOR
			for (IntType p = 0; p < (IntType)pending.size(); p++)
				BuildRange(ctx, sub[p], pending[p].start, pending[p].end, pending[p].depth, -1, nullptr);

			// Splice each subtree into the main output. Local node index 0 (the
			// subtree root) replaces the placeholder; local index L >= 1 maps to
			// baseNode + L - 1. Leaf triangle offsets shift by baseTri; triOrder
			// values are original triangle indices and need no remap.
			for (IntType p = 0; p < (IntType)pending.size(); p++)
			{
				BuildOutput& s = sub[p];
				uint32_t baseNode = (uint32_t)out.nodes.size();
				uint32_t baseTri = (uint32_t)out.triOrder.size();

				for (size_t l = 0; l < s.nodes.size(); l++)
				{
					PTNode n = s.nodes[l];
					if (n.leftOrFirst & PT_NODE_LEAF_BIT)
						n.leftOrFirst = PT_NODE_LEAF_BIT | ((n.leftOrFirst & ~PT_NODE_LEAF_BIT) + baseTri);
					else
					{
						n.leftOrFirst = baseNode + n.leftOrFirst - 1;
						n.rightOrCount = baseNode + n.rightOrCount - 1;
					}

					if (!l)
						out.nodes[pending[p].nodeIdx] = n;
					else
						out.nodes.push_back(n);
				}

				out.triOrder.insert(out.triOrder.end(), s.triOrder.begin(), s.triOrder.end());
				out.numLeaves += s.numLeaves;
				if (s.maxDepth > out.maxDepth)
					out.maxDepth = s.maxDepth;
			}
		}

		if ((IntType)out.triOrder.size() != numTris)
		{
			WARNING("BVH build lost triangles");
			return false;
		}

		// Final reorder: triangles laid out in leaf order for GPU cache coherence
		bvh.tris.resize(numTris);

		#pragma OPENMP_FOR
		for (IntType i = 0; i < numTris; i++)
			bvh.tris[i] = tris[out.triOrder[i]];

		bvh.nodes = std::move(out.nodes);
		bvh.triSource = std::move(out.triOrder);
		bvh.verts = verts;
		bvh.numVerts = numVerts;
		bvh.numLeaves = out.numLeaves;
		bvh.maxDepth = out.maxDepth;

		if (bvh.maxDepth >= (IntType)PT_BVH_TRACE_STACK)
			WARNING("BVH depth exceeds traversal stack"); // unreachable per PT_BVH_MEDIAN_DEPTH bound

		return true;
	}

	// ------------------------------------------------------------------------
	// Reference traversal
	// ------------------------------------------------------------------------

	CinemaBvhHit CinemaBvhTrace(const CinemaBvh& bvh, const CinemaBvhRay& ray)
	{
		CinemaBvhHit hit = { ray.tMax, PT_BVH_NO_HIT, 0.0f, 0.0f };
		if (!bvh.IsValid())
			return hit;

		const Vec3f o = { ray.ox, ray.oy, ray.oz };
		const Vec3f d = { ray.dx, ray.dy, ray.dz };
		const Vec3f invD = { 1.0f / d.x, 1.0f / d.y, 1.0f / d.z };

		float entry;
		if (!SlabHit(bvh.nodes[0], o, invD, ray.tMin, hit.t, entry))
			return hit;

		// Near-first traversal: the far child is stacked with its entry distance
		// so it can be skipped later if a closer hit has already been found.
		struct StackEntry
		{
			uint32_t node;
			float entry;
		};
		StackEntry stack[PT_BVH_TRACE_STACK];
		IntType sp = 0;
		uint32_t nodeIdx = 0;

		for (;;)
		{
			const PTNode& node = bvh.nodes[nodeIdx];
			BoolType descend = false;

			if (node.leftOrFirst & PT_NODE_LEAF_BIT)
			{
				uint32_t first = node.leftOrFirst & ~PT_NODE_LEAF_BIT;
				for (uint32_t i = 0; i < node.rightOrCount; i++)
				{
					float t, u, v;
					if (IntersectTri(bvh.verts, bvh.tris[first + i], o, d, ray.tMin, hit.t, t, u, v))
					{
						hit.t = t;
						hit.triIdx = first + i;
						hit.u = u;
						hit.v = v;
					}
				}
			}
			else
			{
				float tLeft, tRight;
				BoolType hitLeft = SlabHit(bvh.nodes[node.leftOrFirst], o, invD, ray.tMin, hit.t, tLeft);
				BoolType hitRight = SlabHit(bvh.nodes[node.rightOrCount], o, invD, ray.tMin, hit.t, tRight);

				if (hitLeft && hitRight)
				{
					uint32_t nearNode = node.leftOrFirst, farNode = node.rightOrCount;
					float farEntry = tRight;
					if (tRight < tLeft)
					{
						nearNode = node.rightOrCount;
						farNode = node.leftOrFirst;
						farEntry = tLeft;
					}

					if (sp < PT_BVH_TRACE_STACK)
						stack[sp++] = { farNode, farEntry };
					else
						WarnStackOverflow();

					nodeIdx = nearNode;
					descend = true;
				}
				else if (hitLeft)
				{
					nodeIdx = node.leftOrFirst;
					descend = true;
				}
				else if (hitRight)
				{
					nodeIdx = node.rightOrCount;
					descend = true;
				}
			}

			if (descend)
				continue;

			// Pop, skipping nodes that can no longer beat the current hit
			BoolType popped = false;
			while (sp)
			{
				StackEntry e = stack[--sp];
				if (e.entry < hit.t)
				{
					nodeIdx = e.node;
					popped = true;
					break;
				}
			}
			if (!popped)
				break;
		}

		return hit;
	}

	BoolType CinemaBvhTraceShadow(const CinemaBvh& bvh, const CinemaBvhRay& ray)
	{
		if (!bvh.IsValid())
			return false;

		const Vec3f o = { ray.ox, ray.oy, ray.oz };
		const Vec3f d = { ray.dx, ray.dy, ray.dz };
		const Vec3f invD = { 1.0f / d.x, 1.0f / d.y, 1.0f / d.z };

		float entry;
		if (!SlabHit(bvh.nodes[0], o, invD, ray.tMin, ray.tMax, entry))
			return false;

		// Any-hit: no ordering needed, early-out on the first occluder
		uint32_t stack[PT_BVH_TRACE_STACK];
		IntType sp = 0;
		uint32_t nodeIdx = 0;

		for (;;)
		{
			const PTNode& node = bvh.nodes[nodeIdx];

			if (node.leftOrFirst & PT_NODE_LEAF_BIT)
			{
				uint32_t first = node.leftOrFirst & ~PT_NODE_LEAF_BIT;
				for (uint32_t i = 0; i < node.rightOrCount; i++)
				{
					float t, u, v;
					if (IntersectTri(bvh.verts, bvh.tris[first + i], o, d, ray.tMin, ray.tMax, t, u, v))
						return true;
				}

				if (!sp)
					return false;
				nodeIdx = stack[--sp];
				continue;
			}

			float tLeft, tRight;
			BoolType hitLeft = SlabHit(bvh.nodes[node.leftOrFirst], o, invD, ray.tMin, ray.tMax, tLeft);
			BoolType hitRight = SlabHit(bvh.nodes[node.rightOrCount], o, invD, ray.tMin, ray.tMax, tRight);

			if (hitLeft && hitRight)
			{
				if (sp < PT_BVH_TRACE_STACK)
					stack[sp++] = node.rightOrCount;
				else
					WarnStackOverflow();
				nodeIdx = node.leftOrFirst;
			}
			else if (hitLeft)
				nodeIdx = node.leftOrFirst;
			else if (hitRight)
				nodeIdx = node.rightOrCount;
			else
			{
				if (!sp)
					return false;
				nodeIdx = stack[--sp];
			}
		}
	}

	// ------------------------------------------------------------------------
	// Selftest oracle
	// ------------------------------------------------------------------------

	namespace
	{
		// Deterministic RNG for the selftest (xorshift32, fixed seed). Kept
		// local on purpose: the GPU kernels use pcg4d + blue noise instead.
		inline uint32_t SelftestRand(uint32_t& s)
		{
			s ^= s << 13;
			s ^= s >> 17;
			s ^= s << 5;
			return s;
		}

		inline float SelftestFloat(uint32_t& s)
		{
			return (float)(SelftestRand(s) >> 8) * (1.0f / 16777216.0f); // [0, 1)
		}

		inline float SelftestRange(uint32_t& s, float a, float b)
		{
			return a + (b - a) * SelftestFloat(s);
		}
	}

	BoolType CinemaBvhSelftest(IntType* outMismatches)
	{
		const IntType numTris = 500;
		const IntType numRays = 1000;
		uint32_t seed = 0x51C0FFEEu;

		// Procedural scene: random triangles (edge ~ 12) scattered in a cube
		std::vector<PTVert> verts(numTris * 3); // value-initialized: pads are 0
		std::vector<PTTri> tris(numTris);
		for (IntType i = 0; i < numTris; i++)
		{
			float cx = SelftestRange(seed, -40.0f, 40.0f);
			float cy = SelftestRange(seed, -40.0f, 40.0f);
			float cz = SelftestRange(seed, -40.0f, 40.0f);

			for (IntType k = 0; k < 3; k++)
			{
				PTVert& v = verts[i * 3 + k];
				v.px = cx + SelftestRange(seed, -6.0f, 6.0f);
				v.py = cy + SelftestRange(seed, -6.0f, 6.0f);
				v.pz = cz + SelftestRange(seed, -6.0f, 6.0f);
				v.colorABGR = 0xFFFFFFFFu;
			}

			tris[i] = { (uint32_t)(i * 3), (uint32_t)(i * 3 + 1), (uint32_t)(i * 3 + 2), (uint32_t)(i & 7) };
		}

		Timer timer;
		CinemaBvh bvh;
		if (!CinemaBvhBuild(bvh, verts.data(), numTris * 3, tris.data(), numTris))
		{
			WARNING("Selftest BVH build failed");
			if (outMismatches)
				*outMismatches = numRays;
			return false;
		}

		// Deterministic rays: origins in a wide shell, aimed at the triangle cloud
		IntType closestMismatches = 0, shadowMismatches = 0, hits = 0;
		for (IntType r = 0; r < numRays; r++)
		{
			float ox = SelftestRange(seed, -90.0f, 90.0f);
			float oy = SelftestRange(seed, -90.0f, 90.0f);
			float oz = SelftestRange(seed, -90.0f, 90.0f);
			float dx = SelftestRange(seed, -30.0f, 30.0f) - ox;
			float dy = SelftestRange(seed, -30.0f, 30.0f) - oy;
			float dz = SelftestRange(seed, -30.0f, 30.0f) - oz;

			float len = sqrtf(dx * dx + dy * dy + dz * dz);
			if (len < 1e-4f)
			{
				dx = 1.0f; dy = 0.0f; dz = 0.0f;
				len = 1.0f;
			}
			dx /= len; dy /= len; dz /= len;

			CinemaBvhRay ray = { ox, oy, oz, dx, dy, dz, 1e-3f, 1e6f };
			CinemaBvhHit hb = CinemaBvhTrace(bvh, ray);

			// Brute force O(n) over the ORIGINAL triangle order (also validates
			// the leaf reordering via triSource)
			const Vec3f o = { ox, oy, oz };
			const Vec3f d = { dx, dy, dz };
			float bestT = ray.tMax;
			IntType bestTri = -1;
			for (IntType i = 0; i < numTris; i++)
			{
				float t, u, v;
				if (IntersectTri(verts.data(), tris[i], o, d, ray.tMin, bestT, t, u, v))
				{
					bestT = t;
					bestTri = i;
				}
			}

			BoolType bvhHit = hb.triIdx != PT_BVH_NO_HIT;
			BoolType bruteHit = bestTri >= 0;
			if (bvhHit != bruteHit)
				closestMismatches++;
			else if (bvhHit)
			{
				hits++;

				// Same distance is required; a different triangle at the same t
				// within tolerance is a coplanar tie, not an error. triSource
				// maps the reordered hit index back to the brute-force order.
				BoolType sameTri = (IntType)bvh.triSource[hb.triIdx] == bestTri;
				BoolType sameT = fabsf(hb.t - bestT) <= 1e-4f * fmaxf(1.0f, bestT);
				if (!sameTri && !sameT)
					closestMismatches++;
			}

			// TraceShadow must agree with "any hit exists in (tMin, tMax)"
			if (CinemaBvhTraceShadow(bvh, ray) != bruteHit)
				shadowMismatches++;
		}

		IntType mismatches = closestMismatches + shadowMismatches;
		if (outMismatches)
			*outMismatches = mismatches;

		DEBUG("CinemaBvhSelftest: " + NumStr((RealType)numRays) + " rays, " + NumStr((RealType)hits) + " hits, "
			+ NumStr((RealType)closestMismatches) + " closest mismatches, " + NumStr((RealType)shadowMismatches)
			+ " shadow mismatches, " + NumStr((RealType)(IntType)bvh.nodes.size()) + " nodes, depth "
			+ NumStr((RealType)bvh.maxDepth) + ", " + NumStr(timer.ElapsedMs()) + " ms");

		if (mismatches)
			WARNING("CinemaBvhSelftest FAILED with " + NumStr((RealType)mismatches) + " mismatches");

		return !mismatches;
	}
}
