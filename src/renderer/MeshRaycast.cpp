#include "MeshRaycast.h"

#include <limits>

using namespace DirectX;

namespace renderer {

// ─────────────────────────────────────────────────────────────────
// Möller-Trumbore ray-triangle intersection
// ─────────────────────────────────────────────────────────────────

/// Classic Möller-Trumbore. On hit, returns true and fills outT (ray distance)
/// and barycentrics (u, v). Triangle vertices are passed in CCW order — but
/// since we accept hits with any sign convention by allowing both faces (no
/// backface culling here), the winding doesn't matter for hit/miss.
static bool intersectTri(FXMVECTOR origin, FXMVECTOR dir,
                         FXMVECTOR v0, FXMVECTOR v1, FXMVECTOR v2,
                         float& outT, float& outU, float& outV)
{
    const XMVECTOR e1 = XMVectorSubtract(v1, v0);
    const XMVECTOR e2 = XMVectorSubtract(v2, v0);
    const XMVECTOR pv = XMVector3Cross(dir, e2);

    const float det = XMVectorGetX(XMVector3Dot(e1, pv));
    // No culling: accept both windings. Reject only near-zero det (parallel).
    constexpr float kEps = 1e-7f;
    if (std::abs(det) < kEps) return false;
    const float invDet = 1.0f / det;

    const XMVECTOR tv = XMVectorSubtract(origin, v0);
    const float u = XMVectorGetX(XMVector3Dot(tv, pv)) * invDet;
    if (u < 0.0f || u > 1.0f) return false;

    const XMVECTOR qv = XMVector3Cross(tv, e1);
    const float v = XMVectorGetX(XMVector3Dot(dir, qv)) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;

    const float t = XMVectorGetX(XMVector3Dot(e2, qv)) * invDet;
    if (t < 0.0f) return false;     // intersection is behind the ray origin

    outT = t;
    outU = u;
    outV = v;
    return true;
}

// ─────────────────────────────────────────────────────────────────
// raycastMesh
// ─────────────────────────────────────────────────────────────────

RaycastHit raycastMesh(const StaticMesh& mesh,
                       FXMVECTOR rayOrigin, FXMVECTOR rayDir)
{
    RaycastHit hit;
    hit.distance = std::numeric_limits<float>::max();

    const int triCount = mesh.triangleCount();
    XMFLOAT3 v0f, v1f, v2f;

    int   bestTri = -1;
    float bestT   = hit.distance;
    float bestU   = 0.0f, bestV = 0.0f;

    for (int t = 0; t < triCount; ++t) {
        if (!mesh.getTriPositions(t, v0f, v1f, v2f)) continue;
        const XMVECTOR v0 = XMLoadFloat3(&v0f);
        const XMVECTOR v1 = XMLoadFloat3(&v1f);
        const XMVECTOR v2 = XMLoadFloat3(&v2f);

        float tDist, uu, vv;
        if (!intersectTri(rayOrigin, rayDir, v0, v1, v2, tDist, uu, vv)) continue;
        if (tDist >= bestT) continue;
        bestT   = tDist;
        bestU   = uu;
        bestV   = vv;
        bestTri = t;
    }

    if (bestTri < 0) {
        hit.triangleIndex = -1;
        return hit;
    }

    hit.triangleIndex = bestTri;
    hit.groupIndex    = mesh.triangleGroup(bestTri);
    hit.distance      = bestT;

    // World-space hit position
    const XMVECTOR worldPos = XMVectorAdd(rayOrigin, XMVectorScale(rayDir, bestT));
    XMStoreFloat3(&hit.worldPos, worldPos);

    // Interpolate UV via barycentrics (1-u-v, u, v)
    XMFLOAT2 uv0, uv1, uv2;
    if (mesh.getTriUvs(bestTri, uv0, uv1, uv2)) {
        const float w = 1.0f - bestU - bestV;
        hit.uv.x = uv0.x * w + uv1.x * bestU + uv2.x * bestV;
        hit.uv.y = uv0.y * w + uv1.y * bestU + uv2.y * bestV;
    }

    return hit;
}

// ─────────────────────────────────────────────────────────────────
// screenToRay — viewport pixel → world-space ray
// ─────────────────────────────────────────────────────────────────

void screenToRay(int pixelX, int pixelY, int viewW, int viewH,
                 const OrbitCamera& cam,
                 XMVECTOR& outOrigin, XMVECTOR& outDir)
{
    if (viewW < 1) viewW = 1;
    if (viewH < 1) viewH = 1;

    // NDC: x in [-1, +1], y flipped (Qt origin is top-left, NDC y is bottom-up)
    const float ndcX = (2.0f * static_cast<float>(pixelX) / static_cast<float>(viewW))  - 1.0f;
    const float ndcY = 1.0f - (2.0f * static_cast<float>(pixelY) / static_cast<float>(viewH));

    // Take two clip-space points: near (z=0) and far (z=1).
    const XMVECTOR clipNear = XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
    const XMVECTOR clipFar  = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);

    const XMMATRIX vp     = cam.viewProj();
    const XMMATRIX invVp  = XMMatrixInverse(nullptr, vp);

    // Unproject
    const XMVECTOR worldNear4 = XMVector4Transform(clipNear, invVp);
    const XMVECTOR worldFar4  = XMVector4Transform(clipFar,  invVp);

    // Perspective divide
    const XMVECTOR worldNear = XMVectorScale(worldNear4, 1.0f / XMVectorGetW(worldNear4));
    const XMVECTOR worldFar  = XMVectorScale(worldFar4,  1.0f / XMVectorGetW(worldFar4));

    outOrigin = worldNear;
    outDir    = XMVector3Normalize(XMVectorSubtract(worldFar, worldNear));
}

} // namespace renderer
