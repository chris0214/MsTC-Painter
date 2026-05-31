// Standalone smoke tests for MeshRaycast's intersection math.
//
// Validates Möller-Trumbore ray-triangle intersection. We don't link the
// full StaticMesh / Qt stack here — only the algorithm matters. The test
// inlines the same routine and verifies it on known geometry.

#include <DirectXMath.h>

#include <cstdio>
#include <cmath>

using namespace DirectX;

static int s_failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #expr); \
        ++s_failures; \
    } \
} while (0)

#define CHECK_NEAR(actual, expected, eps) do { \
    const float a = float(actual), e = float(expected); \
    if (std::abs(a - e) > (eps)) { \
        std::fprintf(stderr, "FAIL [%s:%d]: %s ≈ %s (got %f, want %f, eps %f)\n", \
                     __FILE__, __LINE__, #actual, #expected, a, e, float(eps)); \
        ++s_failures; \
    } \
} while (0)

// We don't link the full StaticMesh because it needs the whole DX11/Qt stack.
// Instead, this test directly exercises Möller-Trumbore via a faked mesh
// implementing the same accessor interface. To do that, we'd need a header-
// only mesh interface.
//
// For simplicity, we just test the math via a single-triangle scenario by
// reimplementing intersect inline (copy-paste from MeshRaycast.cpp). This
// gives us coverage of the algorithm without dragging in the rest.
static bool intersectTri(FXMVECTOR origin, FXMVECTOR dir,
                         FXMVECTOR v0, FXMVECTOR v1, FXMVECTOR v2,
                         float& t, float& u, float& v)
{
    const XMVECTOR e1 = XMVectorSubtract(v1, v0);
    const XMVECTOR e2 = XMVectorSubtract(v2, v0);
    const XMVECTOR pv = XMVector3Cross(dir, e2);
    const float det = XMVectorGetX(XMVector3Dot(e1, pv));
    if (std::abs(det) < 1e-7f) return false;
    const float invDet = 1.0f / det;
    const XMVECTOR tv = XMVectorSubtract(origin, v0);
    u = XMVectorGetX(XMVector3Dot(tv, pv)) * invDet;
    if (u < 0 || u > 1) return false;
    const XMVECTOR qv = XMVector3Cross(tv, e1);
    v = XMVectorGetX(XMVector3Dot(dir, qv)) * invDet;
    if (v < 0 || u + v > 1) return false;
    t = XMVectorGetX(XMVector3Dot(e2, qv)) * invDet;
    return t >= 0;
}

static void testStraightShotCenter()
{
    std::puts("\n--- Test: straight shot at triangle center ---");
    // Triangle in z=0 plane, vertices forming an equilateral-ish triangle.
    XMVECTOR v0 = XMVectorSet(-1, -1, 0, 0);
    XMVECTOR v1 = XMVectorSet( 1, -1, 0, 0);
    XMVECTOR v2 = XMVectorSet( 0,  1, 0, 0);

    // Centroid is at (0, -1/3, 0). Shoot ray from (0, -1/3, -5) towards +Z.
    XMVECTOR origin = XMVectorSet(0, -1.0f/3.0f, -5, 0);
    XMVECTOR dir    = XMVectorSet(0, 0, 1, 0);

    float t, u, v;
    bool ok = intersectTri(origin, dir, v0, v1, v2, t, u, v);
    std::printf("  ok=%d, t=%f, u=%f, v=%f\n", ok, t, u, v);
    CHECK(ok);
    CHECK_NEAR(t, 5.0f, 1e-4f);
    // Centroid barycentrics: 1/3 each
    CHECK_NEAR(u, 1.0f/3.0f, 1e-4f);
    CHECK_NEAR(v, 1.0f/3.0f, 1e-4f);
}

static void testMissBeyondEdge()
{
    std::puts("\n--- Test: ray misses outside triangle ---");
    XMVECTOR v0 = XMVectorSet(0, 0, 0, 0);
    XMVECTOR v1 = XMVectorSet(1, 0, 0, 0);
    XMVECTOR v2 = XMVectorSet(0, 1, 0, 0);
    // Shoot at (10, 10, -5) → way outside
    XMVECTOR origin = XMVectorSet(10, 10, -5, 0);
    XMVECTOR dir    = XMVectorSet(0, 0, 1, 0);

    float t, u, v;
    bool ok = intersectTri(origin, dir, v0, v1, v2, t, u, v);
    std::printf("  ok=%d (expected 0)\n", ok);
    CHECK(!ok);
}

static void testParallelRay()
{
    std::puts("\n--- Test: ray parallel to triangle plane ---");
    XMVECTOR v0 = XMVectorSet(-1, 0, 0, 0);
    XMVECTOR v1 = XMVectorSet( 1, 0, 0, 0);
    XMVECTOR v2 = XMVectorSet( 0, 1, 0, 0);
    XMVECTOR origin = XMVectorSet(0, 0.5f, -5, 0);
    XMVECTOR dir    = XMVectorSet(1, 0, 0, 0);  // parallel to triangle (XY plane)

    float t, u, v;
    bool ok = intersectTri(origin, dir, v0, v1, v2, t, u, v);
    std::printf("  ok=%d (expected 0 — parallel)\n", ok);
    CHECK(!ok);
}

static void testBehindRayOrigin()
{
    std::puts("\n--- Test: triangle behind ray origin ---");
    XMVECTOR v0 = XMVectorSet(-1, -1, 0, 0);
    XMVECTOR v1 = XMVectorSet( 1, -1, 0, 0);
    XMVECTOR v2 = XMVectorSet( 0,  1, 0, 0);
    // Ray origin AT z=+5 looking in +Z direction (away from triangle at z=0)
    XMVECTOR origin = XMVectorSet(0, 0, 5, 0);
    XMVECTOR dir    = XMVectorSet(0, 0, 1, 0);

    float t, u, v;
    bool ok = intersectTri(origin, dir, v0, v1, v2, t, u, v);
    std::printf("  ok=%d (expected 0 — behind)\n", ok);
    CHECK(!ok);
}

static void testOffCenterUv()
{
    std::puts("\n--- Test: off-center hit produces reasonable barycentrics ---");
    XMVECTOR v0 = XMVectorSet(0, 0, 0, 0);
    XMVECTOR v1 = XMVectorSet(1, 0, 0, 0);
    XMVECTOR v2 = XMVectorSet(0, 1, 0, 0);
    // Hit at (0.5, 0.25, 0): bary = (0.25, 0.5, 0.25) ⇒ u=0.5, v=0.25
    XMVECTOR origin = XMVectorSet(0.5f, 0.25f, -3, 0);
    XMVECTOR dir    = XMVectorSet(0, 0, 1, 0);

    float t, u, v;
    bool ok = intersectTri(origin, dir, v0, v1, v2, t, u, v);
    std::printf("  ok=%d, t=%f, u=%f, v=%f\n", ok, t, u, v);
    CHECK(ok);
    CHECK_NEAR(u, 0.5f, 1e-4f);
    CHECK_NEAR(v, 0.25f, 1e-4f);
}

int main()
{
    testStraightShotCenter();
    testMissBeyondEdge();
    testParallelRay();
    testBehindRayOrigin();
    testOffCenterUv();

    if (s_failures == 0) {
        std::puts("\n=== ALL RAYCAST TESTS PASSED ===");
        return 0;
    }
    std::printf("\n=== %d FAILURES ===\n", s_failures);
    return 1;
}
