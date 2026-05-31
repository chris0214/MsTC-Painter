#pragma once

#include <cstdint>

namespace editor {

/// Bitmask of channels written by a single setPixel call. The PaintLayer's
/// `wroteMask` ORs these in. For BrushEngine / FloodFill, callers pass the
/// active channelMask of the current operation.
///
/// (Bit 0 = R, 1 = G, 2 = B, 3 = A — same layout as ChannelMask in BrushEngine.)
using ChannelAuthorMask = uint8_t;

constexpr ChannelAuthorMask kAuthorAll = 0xF;

/// What BrushEngine / FloodFill / future ImageLayer.bake() write into.
///
/// In Phase 7 the only implementation was TextureDocument itself. In Phase 8
/// we add PaintLayer as a second implementation; the brush hot path doesn't
/// know which it's writing to.
struct PixelTarget
{
    virtual ~PixelTarget() = default;

    /// Width == height. Always a power of two (matches the doc/layer
    /// invariant set by TextureDocument's constructor).
    virtual int  size() const = 0;

    /// Read 4 bytes (R, G, B, A) at (x,y). Caller bounds-checks.
    /// For PaintLayer pixels at coords with `wroteMask` clear in their
    /// channels, those channels read back as 0 — see PaintLayer for why.
    virtual void getPixel(int x, int y, uint8_t out[4]) const = 0;

    /// Write 4 bytes at (x,y). `channelsAuthored` records which channels the
    /// caller intentionally wrote — PaintLayer ORs this into its wroteMask
    /// so the compositor knows what to take from this layer vs. fall through
    /// to a layer below.
    virtual void setPixel(int x, int y,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                          ChannelAuthorMask channelsAuthored) = 0;
};

/// Read-only view of the *composite* — what the user sees on screen, after
/// the LayerStack has merged all layers.
///
/// FloodFill's tolerance check uses this so the comparison is "what you see
/// is what you compare" — the PS-correct behavior. The brush itself does NOT
/// read the composite (it writes layer-locally), so this interface is only
/// passed where the read source must be the merged result.
struct CompositeView
{
    virtual ~CompositeView() = default;
    virtual int  size() const = 0;
    virtual void getPixel(int x, int y, uint8_t out[4]) const = 0;
};

} // namespace editor
