#include "SdfRaster.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace editor {

namespace {

// ─────────────────────────────────────────────────────────────────
// Cubic Bezier helpers
// ─────────────────────────────────────────────────────────────────

inline UvPt lerp(UvPt a, UvPt b, float t)
{
    return { a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t };
}

/// De Casteljau: split a cubic Bezier (P0, P1, P2, P3) at parameter 0.5
/// into two cubics (left and right halves). Endpoints of the halves are
/// shared with the original, so the caller manages emit-points carefully.
struct CubicSplit { UvPt l1, l2, l3_r0, r1, r2; };
inline CubicSplit splitCubicAtMid(UvPt p0, UvPt p1, UvPt p2, UvPt p3)
{
    UvPt l1     = lerp(p0, p1, 0.5f);
    UvPt mid12  = lerp(p1, p2, 0.5f);
    UvPt r2     = lerp(p2, p3, 0.5f);
    UvPt l2     = lerp(l1, mid12, 0.5f);
    UvPt r1     = lerp(mid12, r2, 0.5f);
    UvPt l3_r0  = lerp(l2, r1, 0.5f);
    return { l1, l2, l3_r0, r1, r2 };
}

/// Squared distance from point `p` to the line segment from `a` to `b`.
/// (We only need the absolute scale — the final value is compared against
/// a squared chord tolerance.)
inline float distPointToSegmentSq(UvPt p, UvPt a, UvPt b)
{
    const float du = b.u - a.u;
    const float dv = b.v - a.v;
    const float lenSq = du * du + dv * dv;
    if (lenSq <= 0.0f) {
        const float ddu = p.u - a.u;
        const float ddv = p.v - a.v;
        return ddu * ddu + ddv * ddv;
    }
    float t = ((p.u - a.u) * du + (p.v - a.v) * dv) / lenSq;
    t = std::clamp(t, 0.0f, 1.0f);
    const float qu = a.u + du * t;
    const float qv = a.v + dv * t;
    const float ddu = p.u - qu;
    const float ddv = p.v - qv;
    return ddu * ddu + ddv * ddv;
}

/// Adaptive flatten: subdivide until both control-handle midpoints are
/// within `chordTolUv` of the chord (P0→P3). Emits points starting from
/// p0 (caller pushes p3 once at the end of all flattens). Recursive but
/// bounded — typical contour: ~4-5 splits per segment, max depth ~10.
void flattenCubicRecursive(UvPt p0, UvPt p1, UvPt p2, UvPt p3,
                           float chordTolSq,
                           int    depthRemaining,
                           std::vector<UvPt>& out)
{
    if (depthRemaining <= 0) {
        out.push_back(p3);
        return;
    }
    const float d1 = distPointToSegmentSq(p1, p0, p3);
    const float d2 = distPointToSegmentSq(p2, p0, p3);
    if (d1 <= chordTolSq && d2 <= chordTolSq) {
        out.push_back(p3);
        return;
    }
    const auto s = splitCubicAtMid(p0, p1, p2, p3);
    flattenCubicRecursive(p0, s.l1, s.l2, s.l3_r0, chordTolSq, depthRemaining - 1, out);
    flattenCubicRecursive(s.l3_r0, s.r1, s.r2, p3, chordTolSq, depthRemaining - 1, out);
}

} // anonymous

// ─────────────────────────────────────────────────────────────────
// Public: flatten one contour
// ─────────────────────────────────────────────────────────────────

std::vector<UvPt> flattenContour(const SdfContour& contour, float chordTolUv)
{
    std::vector<UvPt> out;
    if (contour.anchors.empty()) return out;

    const float chordTolSq = chordTolUv * chordTolUv;

    // Push the first anchor; subsequent flatten calls only push their
    // segment endpoints, so no anchor is duplicated.
    out.push_back({ contour.anchors[0].posU, contour.anchors[0].posV });

    auto pushSegment = [&](const SdfAnchor& A, const SdfAnchor& B) {
        const UvPt p0 { A.posU, A.posV };
        const UvPt p1 { A.posU + A.outDU, A.posV + A.outDV };
        const UvPt p2 { B.posU + B.inDU,  B.posV + B.inDV  };
        const UvPt p3 { B.posU, B.posV };
        flattenCubicRecursive(p0, p1, p2, p3, chordTolSq, /*depth=*/16, out);
    };

    const int N = static_cast<int>(contour.anchors.size());
    for (int i = 0; i + 1 < N; ++i) {
        pushSegment(contour.anchors[i], contour.anchors[i + 1]);
    }
    if (contour.closed && N >= 2) {
        // Wrap-around segment from last anchor back to first.
        pushSegment(contour.anchors[N - 1], contour.anchors[0]);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────
// Inside / outside test (winding number — Dan Sunday)
// ─────────────────────────────────────────────────────────────────

namespace {

/// Sign of (segment endpoint cross product). 0 if collinear, +/-1 otherwise.
/// Uses Dan Sunday's `isLeft` (https://geomalgorithms.com/a03-_inclusion.html).
inline float isLeft(UvPt a, UvPt b, UvPt p)
{
    return (b.u - a.u) * (p.v - a.v) - (p.u - a.u) * (b.v - a.v);
}

/// Compute winding number of point `p` w.r.t. the closed polyline.
/// Polyline is implicitly closed: last vertex connects back to first.
int windingNumber(const std::vector<UvPt>& poly, UvPt p)
{
    int wn = 0;
    const int N = static_cast<int>(poly.size());
    if (N < 3) return 0;
    for (int i = 0; i < N; ++i) {
        const UvPt a = poly[i];
        const UvPt b = poly[(i + 1) % N];
        if (a.v <= p.v) {
            if (b.v > p.v && isLeft(a, b, p) > 0.0f) ++wn;
        } else {
            if (b.v <= p.v && isLeft(a, b, p) < 0.0f) --wn;
        }
    }
    return wn;
}

/// Squared min-distance from `p` to any edge of the polyline (treated as
/// closed: the wraparound edge is also considered).
float minDistSqToPolyline(const std::vector<UvPt>& poly, UvPt p)
{
    float minSq = std::numeric_limits<float>::max();
    const int N = static_cast<int>(poly.size());
    if (N < 2) return minSq;
    for (int i = 0; i < N; ++i) {
        const UvPt a = poly[i];
        const UvPt b = poly[(i + 1) % N];
        const float d = distPointToSegmentSq(p, a, b);
        if (d < minSq) minSq = d;
    }
    return minSq;
}

} // anonymous

// ─────────────────────────────────────────────────────────────────
// Public: generate signed distance field
// ─────────────────────────────────────────────────────────────────

std::vector<uint8_t> generateBinarySdf(const std::vector<SdfContour>& contours,
                                       int textureSize,
                                       int sdfRangePx)
{
    if (textureSize <= 0) return {};

    // Default: byte 255 (deep "outside" / bright). With no contours every
    // pixel is outside.
    std::vector<uint8_t> out(static_cast<size_t>(textureSize) * textureSize, 255);
    if (contours.empty()) return out;

    // Flatten all contours once. Tolerance: half a texel in normalized UV.
    const float chordTolUv = 0.5f / static_cast<float>(textureSize);
    std::vector<std::vector<UvPt>> polys;
    polys.reserve(contours.size());
    for (const auto& c : contours) {
        if (c.anchors.size() < 2) continue;
        auto pl = flattenContour(c, chordTolUv);
        if (pl.size() >= 3) polys.push_back(std::move(pl));
    }
    if (polys.empty()) return out;

    const float invSize     = 1.0f / static_cast<float>(textureSize);
    const float rangeSafe   = static_cast<float>(std::max(1, sdfRangePx));
    const float pxPerUv     = static_cast<float>(textureSize);

    for (int y = 0; y < textureSize; ++y) {
        for (int x = 0; x < textureSize; ++x) {
            const UvPt p { (x + 0.5f) * invSize, (y + 0.5f) * invSize };

            // Aggregate: minimum distance across every polyline; combined
            // winding number (sum) so that nested/holed contours could one
            // day work via even-odd or non-zero rules. MVP just uses
            // non-zero (any nonzero winding = inside).
            float minSq = std::numeric_limits<float>::max();
            int   wn   = 0;
            for (const auto& pl : polys) {
                const float d = minDistSqToPolyline(pl, p);
                if (d < minSq) minSq = d;
                wn += windingNumber(pl, p);
            }

            const float distPx       = std::sqrt(minSq) * pxPerUv;
            const float signedDistPx = (wn != 0) ? -distPx : distPx;

            // msTC convention: inside (negative signed) maps to LOW byte
            // (0 = deep shadow); outside maps HIGH (255 = bright);
            // boundary ~ 128.
            const float scaled = 128.0f + (signedDistPx / rangeSafe) * 128.0f;
            out[static_cast<size_t>(y) * textureSize + x] =
                static_cast<uint8_t>(std::clamp(scaled, 0.0f, 255.0f) + 0.5f);
        }
    }
    return out;
}

} // namespace editor
