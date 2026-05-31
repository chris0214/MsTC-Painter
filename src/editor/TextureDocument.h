#pragma once

#include "PixelTarget.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace editor {

/// One of the 4 control textures msToonCoordinator uses.
/// Channel semantics differ per type (see N:\MS\渲染管线.md §4):
///
///   ShadowRate    : R=Base out-rate, G=Highlight out-rate, B=Shadow out-rate
///   SubLightRate  : R=SubLight1,    G=SubLight2,         B=Highlight/SubLight cancel
///   EdgeRate      : R=Edge force    (G/B unused)
///   FaceSDF       : R=grayscale SDF (G=B=R)
enum class TextureKind : uint8_t
{
    ShadowRate = 0,
    SubLightRate,
    EdgeRate,
    FaceSDF,

    Count,
};

/// Default per-channel value when a new document is created.
/// All four control textures default to RGBA = (0,0,0,255):
/// no extra force on any channel, alpha opaque.
constexpr uint8_t kDefaultR = 0;
constexpr uint8_t kDefaultG = 0;
constexpr uint8_t kDefaultB = 0;
constexpr uint8_t kDefaultA = 255;

/// Tile size for dirty-region tracking. 64×64 tiles ⇒ 32×32 grid for 2048², or
/// 64×64 grid for 4096². Keep it a power-of-two so the math stays cheap.
constexpr int kTileSize = 64;

/// A 2D rectangle in texel coordinates (inclusive min, exclusive max).
struct PixelRect
{
    int minX = 0;
    int minY = 0;
    int maxX = 0;     // exclusive
    int maxY = 0;     // exclusive

    bool isEmpty() const { return maxX <= minX || maxY <= minY; }
    int  width()   const { return maxX - minX; }
    int  height()  const { return maxY - minY; }

    /// Grow this rect to also cover `other`. Empty rects are absorbed cleanly:
    /// `empty.unionWith(r)` becomes `r`; `r.unionWith(empty)` stays `r`.
    void unionWith(const PixelRect& other) {
        if (other.isEmpty()) return;
        if (isEmpty()) { *this = other; return; }
        if (other.minX < minX) minX = other.minX;
        if (other.minY < minY) minY = other.minY;
        if (other.maxX > maxX) maxX = other.maxX;
        if (other.maxY > maxY) maxY = other.maxY;
    }
};

/// CPU-side RGBA8 pixel buffer for a single control texture.
///
/// Holds the canonical pixels plus a tile-bitmap of dirty regions so consumers
/// (the 2D canvas widget, the 3D viewport's GPU texture, the auto-saver, …) can
/// pick up only what changed since their last sync.
///
/// Implements both `PixelTarget` (writable) and `CompositeView` (readable) so
/// callers from Phase 7 — which wrote directly to a doc — and Phase 8 —
/// where the doc serves as the LayerStack's composite output cache — share the
/// same interfaces. Direct callers should treat `setPixel` as
/// "internal — call via LayerStack" once Phase 8 is fully landed.
///
/// The brush engine writes here. Everything else reads.
class TextureDocument : public PixelTarget, public CompositeView
{
public:
    TextureDocument(TextureKind kind, int size);

    // ── Identity ────────────────────────────────────────────────
    TextureKind kind()       const { return m_kind; }
    int         size()       const override { return m_size; }   // width == height
    const std::string& name() const { return m_name; }

    // ── Pixel access (read-only) ────────────────────────────────
    const uint8_t* pixels() const { return m_pixels.data(); }
    /// Get the 4-byte RGBA at (x,y). No bounds checking — caller's job.
    void getPixel(int x, int y, uint8_t* outRgba) const override;

    // ── Pixel write (the brush calls this) ──────────────────────
    /// Set a single pixel. Marks the containing tile dirty.
    /// `channelsAuthored` is recorded for PaintLayer's wroteMask in Phase 8;
    /// TextureDocument itself ignores it (the composite has no per-channel
    /// authorship — every channel is always "written"). Default value
    /// preserves the pre-Phase-8 call sites that don't know about authorship.
    void setPixel(int x, int y,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                  ChannelAuthorMask channelsAuthored = kAuthorAll) override;

    /// Mark a rect dirty (used by bulk operations like layer composite).
    /// Clamps to texture bounds.
    void markDirty(const PixelRect& rect);

    /// Replace all pixels with a flat color. Marks the entire texture dirty.
    void clear(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    /// Replace pixel data wholesale (used by layer composite). Size must match.
    /// Marks everything dirty.
    void setPixelsAll(const uint8_t* rgba);

    // ── Dirty tracking ──────────────────────────────────────────
    //
    // Two independent consumers track tile dirtiness:
    //   - Canvas: the 2D editor uses this to repaint only what changed.
    //   - Gpu:    the DX11 viewport uses this to UpdateSubresource only what
    //             changed since its last frame.
    //
    // Every write (setPixel / clear / etc.) marks tiles dirty in BOTH consumers.
    // Each consumer drains independently via takeDirtyBounds(Consumer) which
    // returns the bounds rect and clears that consumer's bitmap.

    enum class DirtyConsumer : uint8_t
    {
        Canvas = 0,
        Gpu    = 1,
        Count,
    };

    /// Bounds rect of dirty tiles for `consumer`, clearing them on return.
    PixelRect takeDirtyBounds(DirtyConsumer consumer = DirtyConsumer::Canvas);
    /// Inspect dirty bounds without consuming.
    PixelRect peekDirtyBounds(DirtyConsumer consumer = DirtyConsumer::Canvas) const;
    /// True if any tile is dirty for that consumer.
    bool hasDirtyTiles(DirtyConsumer consumer = DirtyConsumer::Canvas) const;

    // ── "Ever modified" flag ────────────────────────────────────
    //
    // True iff any user-driven write (brush stamp / flood fill / image paste
    // / setPixelsAll) has touched this doc since the last reset. Distinct
    // from dirty tiles — those track "since last sync"; this tracks "since
    // creation or load." Project save uses it to skip dumping untouched
    // documents (avoids GBs of all-black mask files).

    bool everModified() const { return m_everModified; }
    /// Manually clear the flag (called right after construction's clear() and
    /// after a successful project load, since that load isn't a "user edit").
    void resetEverModified() { m_everModified = false; }
    /// Manually set the flag (used by tests, project load when restoring
    /// non-default state).
    void setEverModified() { m_everModified = true; }

private:
    int  tileGridDim() const { return (m_size + kTileSize - 1) / kTileSize; }
    void markTileDirty(int tileX, int tileY);

    TextureKind m_kind;
    int         m_size;
    std::string m_name;

    std::vector<uint8_t> m_pixels;

    // One bitmap + count per consumer. Indexed by DirtyConsumer.
    static constexpr size_t kNumConsumers = static_cast<size_t>(DirtyConsumer::Count);
    std::array<std::vector<uint8_t>, kNumConsumers> m_tileDirty;
    std::array<int,                  kNumConsumers> m_dirtyTileCount{};

    /// Has any write op (other than construction's initial clear) been
    /// applied? Persists across dirty-bound drains.
    bool m_everModified = false;
};

/// Display name (e.g. "Shadow Rate") for a TextureKind.
const char* textureKindName(TextureKind k);

} // namespace editor
