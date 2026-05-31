#include "UvCoverage.h"

#include <algorithm>
#include <cmath>

namespace editor {

namespace {

/// Edge function: positive on one side, negative on the other. Sign convention
/// here is CCW-positive in image space (V down). We accept either winding —
/// see fillTriangle below for the symmetric handling.
inline float edgeFn(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/// Rasterize one UV triangle into the mask using the edge-function method.
/// Coordinates already converted to texel space.
void fillTriangle(uint8_t* mask, int size,
                  float ax, float ay,
                  float bx, float by,
                  float cx, float cy)
{
    // Bounding rect, clipped to image bounds
    const float minXf = std::floor(std::min({ax, bx, cx}));
    const float minYf = std::floor(std::min({ay, by, cy}));
    const float maxXf = std::ceil (std::max({ax, bx, cx}));
    const float maxYf = std::ceil (std::max({ay, by, cy}));

    const int x0 = std::max(0,        static_cast<int>(minXf));
    const int y0 = std::max(0,        static_cast<int>(minYf));
    const int x1 = std::min(size - 1, static_cast<int>(maxXf));
    const int y1 = std::min(size - 1, static_cast<int>(maxYf));
    if (x0 > x1 || y0 > y1) return;

    // Twice the signed area — sign tells us the winding so we can accept
    // both orientations symmetrically (PMX has no guaranteed winding in UV space).
    const float area2 = edgeFn(ax, ay, bx, by, cx, cy);
    if (std::abs(area2) < 1e-7f) return;   // degenerate triangle

    for (int y = y0; y <= y1; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = x0; x <= x1; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float w0 = edgeFn(bx, by, cx, cy, px, py);
            const float w1 = edgeFn(cx, cy, ax, ay, px, py);
            const float w2 = edgeFn(ax, ay, bx, by, px, py);
            // All same sign as area2 means we're inside the triangle.
            const bool inside = (area2 >= 0)
                ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
                : (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (inside) {
                mask[y * size + x] = 1;
            }
        }
    }
}

} // anonymous

// ─────────────────────────────────────────────────────────────────
// Public
// ─────────────────────────────────────────────────────────────────

std::vector<uint8_t> buildUvCoverageMask(
    const pmx::Model&       model,
    const std::vector<int>& materialIndices,
    int                     size)
{
    std::vector<uint8_t> mask(static_cast<size_t>(size) * size, 0);

    if (size <= 0 || materialIndices.empty() || model.faces.empty()) return mask;

    // We need to walk model.faces by material. PMX stores face index counts
    // per material; we recompute per-material face spans here.
    std::vector<bool> wantMaterial(model.materials.size(), false);
    for (int mi : materialIndices) {
        if (mi >= 0 && mi < static_cast<int>(model.materials.size())) {
            wantMaterial[mi] = true;
        }
    }

    int faceCursor = 0;
    for (size_t mi = 0; mi < model.materials.size(); ++mi) {
        const int triCount = model.materials[mi].indexCount / 3;
        const int faceStart = faceCursor;
        const int faceEnd   = faceCursor + triCount;
        faceCursor = faceEnd;

        if (!wantMaterial[mi]) continue;

        for (int f = faceStart; f < faceEnd && f < static_cast<int>(model.faces.size()); ++f) {
            const auto& tri = model.faces[f];
            const auto& va  = model.vertices[tri[0]].uv;
            const auto& vb  = model.vertices[tri[1]].uv;
            const auto& vc  = model.vertices[tri[2]].uv;

            // UV (0..1) → texel space (0..size).
            const float s = static_cast<float>(size);
            fillTriangle(mask.data(), size,
                         va.x * s, va.y * s,
                         vb.x * s, vb.y * s,
                         vc.x * s, vc.y * s);
        }
    }

    return mask;
}

} // namespace editor
