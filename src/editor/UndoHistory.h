#pragma once

#include "Layer.h"            // for PaintLayer (unique_ptr member)
#include "TextureDocument.h"  // for PixelRect, kTileSize

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace editor {

class LayerStack;

/// Per-(group, kind) undo / redo history.
///
/// Records "before" / "after" pixel state per stroke / flood fill, scoped
/// to the dirty rect (tile-aligned, snapped to 64×64 grid). Layer is
/// identified by stable ID (`Layer::id()`), so reorders / deletes between
/// record and undo are handled gracefully — orphan entries silently no-op
/// or are pruned via `pruneByLayerId`.
///
/// Memory budget: 200 entries OR 256 MB total, whichever hits first.
/// On overflow, oldest entries are dropped (FIFO).
///
/// Tile-incremental snapshot: per stroke, only tiles ACTUALLY touched are
/// captured. Typical 200×200 stroke = ~16 tiles × 5 KB = ~80 KB pre-data
/// (vs. 24 MB for a whole-layer snapshot). Flood fills use a simpler
/// "snapshot whole layer pre, capture only the touched tiles into pre/post"
/// pattern since the rect isn't known until after the fill runs.
class UndoHistory
{
public:
    explicit UndoHistory(LayerStack* owner, int textureSize);

    // ── Stroke recording (called by CanvasWidget / DX11Widget) ──

    /// Brush beginStroke: snapshot the whole layer (one-time ~24 MB memcpy)
    /// so we have a safe "pre-stroke" reference, then prepare a tentative
    /// entry. If a stroke was left open from a previous bug, it's discarded.
    void beginStrokeRecord(const PaintLayer& target);

    /// Called after every stamp / strokeTo: union `box` into the entry's
    /// running dirty rect. Box is what the brush just wrote; we don't need
    /// to read pixels here — the snapshot already captured the pre-state.
    /// `target` is unused for now but kept in the signature for future use.
    void recordStampBox(const PixelRect& box, const PaintLayer& target);

    /// Brush endStroke: extract pre tiles (from the beginStroke snapshot)
    /// and post tiles (from `target` as it stands now) for the union rect's
    /// tile set, push the entry. Releases the snapshot. If no tiles were
    /// touched (rect is empty), discards the entry.
    void endStrokeRecord(const PaintLayer& target);

    /// Discard the in-progress stroke without pushing. Used when the host
    /// abandons a stroke for a non-undo-worthy reason (e.g. window focus
    /// loss). Cheap to call when no stroke is open.
    void abandonStroke();

    // ── Flood fill: single-shot ──

    /// Caller does the fill, then provides `before` (pre-fill snapshot —
    /// can be a full copy of the layer; we take only the tiles overlapping
    /// `box`) and `after` (the layer post-fill). `box` is the dirty rect
    /// reported by `floodFill()`.
    void recordFloodFill(const PaintLayer& before,
                         const PaintLayer& after,
                         const PixelRect&  box);

    // ── Apply ──
    bool canUndo() const { return m_top > 0; }
    bool canRedo() const { return m_top < static_cast<int>(m_entries.size()); }

    /// Undo the most recent entry. Returns the rect that was modified
    /// (caller calls `LayerStack::recompositeRect(rect)`). Empty rect if
    /// nothing to undo, or the target layer no longer exists (orphan).
    PixelRect undo();
    PixelRect redo();

    /// Drop entries whose layerId equals `goneId`. Called when a layer is
    /// removed / merged so its undo history becomes inert (rather than
    /// trying to restore pixels onto a layer that doesn't exist).
    void pruneByLayerId(uint64_t goneId);

    /// Drop everything. Called from project load to start fresh.
    void clear();

    // ── Stats (for tests + debug) ──
    int    entryCount() const { return static_cast<int>(m_entries.size()); }
    int    topIndex()   const { return m_top; }
    size_t byteUsage()  const { return m_byteUsage; }

private:
    /// One tile of pixel + wroteMask data. Edge tiles (when texture size
    /// isn't a multiple of kTileSize) are smaller; we always store the full
    /// per-tile slice exactly.
    struct TileSlice {
        int                  tileX = 0;
        int                  tileY = 0;
        int                  width = 0;   // in pixels
        int                  height = 0;
        std::vector<uint8_t> rgba;        // width * height * 4
        std::vector<uint8_t> wrote;       // width * height
        size_t bytes() const { return rgba.size() + wrote.size(); }
    };
    using TileMap = std::unordered_map<int /*tileKey*/, TileSlice>;

    struct Entry {
        uint64_t  layerId = 0;
        PixelRect rect;       // tile-aligned union of all stamps in this entry
        TileMap   pre;
        TileMap   post;
        size_t bytes() const;
    };

    static int tileKey(int tx, int ty, int gridDim) { return ty * gridDim + tx; }

    /// Read tile (tx, ty) from `layer` into a fresh TileSlice. Caller decides
    /// what map to insert it into.
    TileSlice readTile(const PaintLayer& layer, int tx, int ty) const;

    /// Write tile slice back into `layer`'s raw pixel + wroteMask buffers.
    /// Bypasses PaintLayer::setPixel (which OR's into wroteMask, preventing
    /// undo from clearing bits that the user authored).
    void blitTile(PaintLayer& layer, const TileSlice& slice) const;

    /// For each tile in `tiles`, blit it back into `layer`. Used by undo / redo.
    void applyTiles(PaintLayer& layer, const TileMap& tiles) const;

    /// After pushing or replacing entries, drop the OLDEST entries until
    /// both step count and byte usage are within budget. Adjusts m_top.
    void evictToFit();

    /// Drop all entries at index >= m_top (i.e. the redo half). Called
    /// before pushing a new entry, since any new action invalidates redo.
    void truncateRedo();

    /// Compute tile-aligned rect: snap minX/minY DOWN to tile boundaries,
    /// snap maxX/maxY UP. Used to ensure entry.rect covers all snapshotted
    /// tiles exactly.
    PixelRect snapToTileGrid(const PixelRect& box) const;

    LayerStack*           m_owner;
    int                   m_size;
    int                   m_gridDim;     // ceil(size / kTileSize)
    std::vector<Entry>    m_entries;
    int                   m_top = 0;
    size_t                m_byteUsage = 0;

    // In-progress stroke recording state.
    //
    // We snapshot the WHOLE active PaintLayer at beginStrokeRecord (a one-
    // time ~24 MB memcpy) and accumulate the union dirty rect throughout
    // the stroke. At endStrokeRecord, we extract only the tiles overlapping
    // that rect into the entry's pre/post maps; the snapshot is then freed.
    // This avoids the chicken-and-egg of "snapshot tiles before they're
    // dirtied" — the brush writes synchronously inside its own call so we
    // can't intercept mid-stamp.
    bool                  m_strokeOpen = false;
    Entry                 m_pending;
    std::unique_ptr<PaintLayer> m_strokeSnapshot;   ///< pre-stroke whole-layer copy

    static constexpr int    kMaxEntries = 200;
    static constexpr size_t kMaxBytes   = 256ull * 1024 * 1024;
};

} // namespace editor
