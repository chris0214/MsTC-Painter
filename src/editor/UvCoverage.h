#pragma once

#include "../pmx/PmxTypes.h"

#include <cstdint>
#include <vector>

namespace editor {

/// Build a 0/1 coverage mask of size×size for a subset of materials in a PMX
/// model. Output[y*size + x] == 1 iff the texel center (x+0.5, y+0.5) lies
/// inside the UV triangle of any face owned by one of `materialIndices`.
///
/// V coordinates are taken as-is (matches our top-left origin canvas; same
/// convention used everywhere in the project). UV values outside [0, 1] are
/// clamped to the texture bounds, so degenerate or out-of-range UVs simply
/// don't write to the mask.
///
/// Cost on a 2048² texture for ~10k triangles: ~30–50 ms on a modern CPU.
/// Computed once per TextureGroup at PMX load and cached.
std::vector<uint8_t> buildUvCoverageMask(
    const pmx::Model&        model,
    const std::vector<int>&  materialIndices,
    int                      size);

} // namespace editor
