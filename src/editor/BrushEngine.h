#pragma once

#include "PixelTarget.h"
#include "TextureDocument.h"

#include <cstdint>
#include <deque>
#include <vector>

namespace editor {

/// Which RGBA channels a brush stroke writes to.
/// Bit field — combine with bitwise OR.
enum ChannelMask : uint8_t
{
    ChannelMaskNone = 0,
    ChannelMaskR    = 1 << 0,
    ChannelMaskG    = 1 << 1,
    ChannelMaskB    = 1 << 2,
    ChannelMaskA    = 1 << 3,
    ChannelMaskRGB  = ChannelMaskR | ChannelMaskG | ChannelMaskB,
};

/// How successive dabs of the same stroke combine.
enum class FlowMode : uint8_t
{
    Wash    = 0,    ///< Krita "alpha_darken": within one stroke, alpha is capped at opacity
    Buildup = 1,    ///< Naive over: each dab blends; overlaps accumulate past opacity
};

/// One paint sample. Fully describes what the engine needs to apply at one
/// point along a stroke. Mouse drivers fill `pressure = 1`; future tablet
/// support will write the real value.
struct Dab
{
    float x        = 0.0f;   ///< texture-pixel coordinate
    float y        = 0.0f;
    float pressure = 1.0f;   ///< 0..1
    float rotation = 0.0f;   ///< radians (for future stamp-image brushes)
    float speed    = 0.0f;   ///< px/s along the stroke (for speed-driven dynamics)
};

/// Brush configuration. Lives on the BrushPanel UI; copied per stroke.
struct BrushSettings
{
    float radiusPx  = 32.0f;
    float hardness  = 0.7f;
    float opacity   = 1.0f;
    float spacingPx = 8.0f;

    uint8_t channelMask = ChannelMaskRGB;
    uint8_t inkR        = 255;
    uint8_t inkG        = 255;
    uint8_t inkB        = 255;
    uint8_t inkA        = 255;

    bool erase = false;

    // ── New in Phase 4.1 ────────────────────────────────────────
    FlowMode flowMode  = FlowMode::Wash;  ///< default to Krita-like wash
    float    smoothing = 0.0f;            ///< 0..1, weighted-average lag
    bool     mirrorX   = false;           ///< paint a mirrored dab at (texSize - x, y)
};

/// Stateful brush engine. Tracks an in-progress stroke so successive
/// strokeTo() calls can interpolate samples between mouse events.
///
/// Workflow:
///   beginStroke(uv0)
///   strokeTo(uv1)
///   strokeTo(uv2)
///   ...
///   endStroke()
class BrushEngine
{
public:
    BrushEngine();

    void setSettings(const BrushSettings& s);
    const BrushSettings& settings() const { return m_settings; }

    /// Set just one channel of the ink color (used by eyedropper).
    /// channelIndex: 0=R, 1=G, 2=B, 3=A.
    void setInkValue(int channelIndex, uint8_t v);

    /// Begin a stroke at UV coords (0..1). Stamps once at the start.
    /// Updates `outDirtyBox` to the union of texel rects modified by this
    /// call (caller forwards to LayerStack::recompositeRect).
    void beginStroke(PixelTarget& target, float u, float v, PixelRect& outDirtyBox);

    /// Continue the stroke to a new UV. Stamps at intervals along the
    /// (smoothed) segment from the previous point.
    void strokeTo(PixelTarget& target, float u, float v, PixelRect& outDirtyBox);

    /// Finalize the stroke. Future Phase 9 (undo) will commit a snapshot here.
    void endStroke();

    bool isStroking() const { return m_stroking; }

    /// Get the latest smoothed point in texture-pixel coords, or {-1,-1} if
    /// not stroking. Used by the canvas to draw a stabilizer "tail".
    void smoothedPoint(float& outX, float& outY) const;

private:
    void emitDab(PixelTarget& target, const Dab& d, PixelRect& outDirtyBox);   ///< handles mirror + walker per dab
    void stamp(PixelTarget& target, const Dab& d, PixelRect& outDirtyBox);     ///< single circular dab onto target

    /// In Wash mode, the actual blend factor we want to apply at (x,y) is
    ///   newCap = max(prevCap[x,y], wantedAlpha)
    ///   delta  = newCap - prevCap[x,y]
    /// The dab then blends the *delta* (not wantedAlpha) toward target,
    /// preserving the invariant: total alpha within one stroke ≤ opacity.
    bool          m_strokeMaskActive = false;
    int           m_strokeMaskSize   = 0;
    std::vector<uint8_t> m_strokeAlphaCap;   ///< per-pixel max alpha so far this stroke

    BrushSettings m_settings;
    bool          m_stroking   = false;
    float         m_lastX      = 0.0f;       ///< last stamped point (post-smoothing)
    float         m_lastY      = 0.0f;
    float         m_distAccum  = 0.0f;

    // Weighted-smoothing ring buffer — recent raw mouse samples in pixel coords.
    std::deque<std::pair<float, float>> m_smoothBuf;
};

} // namespace editor
