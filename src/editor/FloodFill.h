#pragma once

#include "PixelTarget.h"
#include "TextureDocument.h"

#include <cstdint>

namespace editor {

/// Bucket fill scope — defines which neighbors a flood fill is allowed to expand into.
enum class FillScope : uint8_t
{
    /// Photoshop-style: spread to neighbors whose active-channel value differs
    /// from the seed by ≤ tolerance. Ignores UV.
    ColorConnected = 0,

    /// Ignore color difference entirely; spread to every neighbor that is
    /// inside the current group's UV coverage mask. Fills the whole UV island.
    UvIsland = 1,

    /// Both constraints: color similarity AND inside UV mask. Useful to fill
    /// "this colored blob, but don't leak into atlas whitespace".
    ColorAndUv = 2,
};

/// Settings for a single bucket-fill operation. Mirrors a subset of BrushSettings
/// so the bucket can reuse the brush's ink + channel mask while having its
/// own tolerance / scope knobs.
struct FillSettings
{
    uint8_t channelMask = 0xF;   ///< only these channels are written (RGBA bits)
    uint8_t inkR = 255;
    uint8_t inkG = 255;
    uint8_t inkB = 255;
    uint8_t inkA = 255;
    float   opacity = 1.0f;      ///< 0..1, multiplier on the written ink

    /// Tolerance for ColorConnected / ColorAndUv: max absolute difference on
    /// the *active* channel (first set bit of channelMask) between seed and
    /// candidate texel. Ignored in pure UvIsland.
    uint8_t tolerance = 32;

    FillScope scope = FillScope::ColorAndUv;
};

/// Run flood fill seeded at (startX, startY).
///
/// Reads tolerance comparisons from `src` (the *composite* — what the user
/// sees) and writes results into `dst` (typically the active layer's
/// PixelTarget). For Phase 7 callers pass the same TextureDocument as both,
/// preserving prior behavior; Phase 8 callers split them so the fill writes
/// to the active layer but compares against the merged composite.
///
/// `uvMask` is optional. When `scope` is UvIsland or ColorAndUv, this must
/// point at a `src.size()² uint8` buffer where 1 = inside UV coverage. Pass
/// nullptr for ColorConnected.
///
/// `outDirtyBox` is grown to encompass every texel actually written (in
/// dst-space — same coords as src). Caller forwards to LayerStack::recompositeRect.
///
/// Returns the number of texels affected.
int floodFill(const CompositeView& src,
              PixelTarget&         dst,
              int                  startX,
              int                  startY,
              const FillSettings&  settings,
              const uint8_t*       uvMask /*nullable*/,
              PixelRect&           outDirtyBox);

} // namespace editor
