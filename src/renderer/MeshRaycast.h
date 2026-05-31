#pragma once

#include "Camera.h"
#include "StaticMesh.h"

#include <DirectXMath.h>

namespace renderer {

/// Result of a ray-mesh intersection test.
struct RaycastHit
{
    int                triangleIndex = -1;     ///< -1 = miss
    int                groupIndex    = -1;
    float              distance      = 0.0f;   ///< along ray, in world units
    DirectX::XMFLOAT3  worldPos      { 0,0,0 };
    DirectX::XMFLOAT2  uv            { 0,0 };  ///< barycentric-interpolated UV
};

/// Cast a ray against `mesh` (CPU-side geometry). Linear scan, no acceleration
/// structure — fine for ~100k tris and one-cast-per-frame.
///
/// Ray is in world space (the same space as StaticMesh's CPU vertex positions).
/// Returns the closest hit; `hit.triangleIndex == -1` if none.
RaycastHit raycastMesh(const StaticMesh&        mesh,
                       DirectX::FXMVECTOR       rayOrigin,
                       DirectX::FXMVECTOR       rayDir);

/// Build a world-space ray from a viewport pixel using the camera matrices.
/// pixel coords are top-left origin (Qt convention). viewW/H are the
/// viewport dimensions in pixels. Outputs are written in world space.
void screenToRay(int pixelX, int pixelY, int viewW, int viewH,
                 const OrbitCamera&  cam,
                 DirectX::XMVECTOR&  outOrigin,
                 DirectX::XMVECTOR&  outDir);

} // namespace renderer
