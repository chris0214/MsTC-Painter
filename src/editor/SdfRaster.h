#pragma once

#include "SdfContour.h"

#include <cstdint>
#include <vector>

namespace editor {

/// 2D point in normalized UV space.
struct UvPt { float u, v; };

/// Adaptive De Casteljau flatten of every cubic segment in `contour` into
/// a polyline. `chordTolUv` is the max permitted chord deviation, in
/// normalized UV. For a 2048² texture, 0.5 / 2048 ≈ 2.4e-4 keeps the
/// approximation sub-texel.
std::vector<UvPt> flattenContour(const SdfContour& contour, float chordTolUv);

/// Generate a binary signed-distance-field raster matching the msTC
/// FaceSDF convention:
///   - INSIDE  any closed contour → byte LOW  (0..127)
///   - OUTSIDE all contours       → byte HIGH (128..255)
///   - On the boundary            → byte ≈ 128
/// Mapping: byte = clamp(128 + (signedDistPx / sdfRangePx) * 128, 0, 255)
/// where signedDist is + outside / - inside, in pixel units.
///
/// `textureSize` is the side length of the output (assumed square, power
/// of two). Output is `textureSize * textureSize` bytes, single channel.
///
/// Brute force: O(W * H * E) where E = total flattened edges across all
/// contours. ~1 sec on CPU at 2048² × 200 edges. Run on a worker thread.
std::vector<uint8_t> generateBinarySdf(const std::vector<SdfContour>& contours,
                                       int textureSize,
                                       int sdfRangePx);

} // namespace editor
