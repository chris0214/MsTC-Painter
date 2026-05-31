#include "UndoHistory.h"

#include "Layer.h"
#include "LayerStack.h"

#include <algorithm>
#include <cstring>

namespace editor {

// ─────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────

UndoHistory::UndoHistory(LayerStack* owner, int textureSize)
    : m_owner(owner)
    , m_size(textureSize)
    , m_gridDim((textureSize + kTileSize - 1) / kTileSize)
{
}

size_t UndoHistory::Entry::bytes() const
{
    size_t b = 0;
    for (const auto& [k, t] : pre)  b += t.bytes();
    for (const auto& [k, t] : post) b += t.bytes();
    return b;
}

// ─────────────────────────────────────────────────────────────────
// Tile I/O — direct buffer access (not setPixel, see header rationale)
// ─────────────────────────────────────────────────────────────────

UndoHistory::TileSlice UndoHistory::readTile(const PaintLayer& layer,
                                              int tx, int ty) const
{
    TileSlice s;
    s.tileX = tx;
    s.tileY = ty;
    const int x0 = tx * kTileSize;
    const int y0 = ty * kTileSize;
    s.width  = std::min(kTileSize, m_size - x0);
    s.height = std::min(kTileSize, m_size - y0);
    if (s.width <= 0 || s.height <= 0) return s;   // off-grid

    s.rgba.resize(static_cast<size_t>(s.width) * s.height * 4);
    s.wrote.resize(static_cast<size_t>(s.width) * s.height);

    const uint8_t* src      = layer.pixels();
    const uint8_t* srcWrote = layer.wroteMask();
    for (int row = 0; row < s.height; ++row) {
        const size_t srcOff  = (static_cast<size_t>(y0 + row) * m_size + x0) * 4;
        const size_t srcWOff =  static_cast<size_t>(y0 + row) * m_size + x0;
        std::memcpy(s.rgba.data()  + static_cast<size_t>(row) * s.width * 4,
                    src + srcOff,
                    static_cast<size_t>(s.width) * 4);
        std::memcpy(s.wrote.data() + static_cast<size_t>(row) * s.width,
                    srcWrote + srcWOff,
                    static_cast<size_t>(s.width));
    }
    return s;
}

void UndoHistory::blitTile(PaintLayer& layer, const TileSlice& slice) const
{
    if (slice.width <= 0 || slice.height <= 0) return;
    const int x0 = slice.tileX * kTileSize;
    const int y0 = slice.tileY * kTileSize;

    uint8_t* dst      = layer.pixelsMut();
    uint8_t* dstWrote = layer.wroteMaskMut();
    for (int row = 0; row < slice.height; ++row) {
        const size_t dstOff  = (static_cast<size_t>(y0 + row) * m_size + x0) * 4;
        const size_t dstWOff =  static_cast<size_t>(y0 + row) * m_size + x0;
        std::memcpy(dst + dstOff,
                    slice.rgba.data() + static_cast<size_t>(row) * slice.width * 4,
                    static_cast<size_t>(slice.width) * 4);
        std::memcpy(dstWrote + dstWOff,
                    slice.wrote.data() + static_cast<size_t>(row) * slice.width,
                    static_cast<size_t>(slice.width));
    }
}

void UndoHistory::applyTiles(PaintLayer& layer, const TileMap& tiles) const
{
    for (const auto& [key, slice] : tiles) {
        blitTile(layer, slice);
    }
}

// ─────────────────────────────────────────────────────────────────
// Tile snapshot during stroke
// ─────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────
// Tile-aligned rect helper
// ─────────────────────────────────────────────────────────────────

PixelRect UndoHistory::snapToTileGrid(const PixelRect& box) const
{
    if (box.isEmpty()) return {};
    PixelRect r;
    r.minX = (box.minX / kTileSize) * kTileSize;
    r.minY = (box.minY / kTileSize) * kTileSize;
    r.maxX = std::min(m_size, ((box.maxX + kTileSize - 1) / kTileSize) * kTileSize);
    r.maxY = std::min(m_size, ((box.maxY + kTileSize - 1) / kTileSize) * kTileSize);
    return r;
}

// ─────────────────────────────────────────────────────────────────
// Stroke recording lifecycle
// ─────────────────────────────────────────────────────────────────

void UndoHistory::beginStrokeRecord(const PaintLayer& target)
{
    // If a previous stroke was left open (host bug), drop it silently.
    m_pending = Entry{};
    m_pending.layerId = target.id();
    m_strokeOpen = true;
    // One-time whole-layer copy so we have a known pre-stroke state. The
    // brush writes synchronously inside its own call so we cannot intercept
    // mid-stamp to snapshot tiles before they're dirtied. Tile-incremental
    // capture would need a callback inside BrushEngine::stamp; the simpler
    // whole-layer copy is fine — at 2048² it's a 24 MB memcpy (~5 ms once
    // per stroke) and the snapshot is freed when the stroke ends.
    m_strokeSnapshot = std::make_unique<PaintLayer>(target);
}

void UndoHistory::recordStampBox(const PixelRect& box, const PaintLayer& /*target*/)
{
    if (!m_strokeOpen || box.isEmpty()) return;
    m_pending.rect.unionWith(box);
}

void UndoHistory::endStrokeRecord(const PaintLayer& target)
{
    if (!m_strokeOpen) return;
    m_strokeOpen = false;

    if (m_pending.rect.isEmpty() || !m_strokeSnapshot) {
        m_pending = Entry{};
        m_strokeSnapshot.reset();
        return;   // nothing touched, don't pollute history
    }

    // Build pre/post tile maps for tiles overlapping the union rect.
    const auto box = m_pending.rect;
    const int tx0 = std::max(0,            box.minX / kTileSize);
    const int ty0 = std::max(0,            box.minY / kTileSize);
    const int tx1 = std::min(m_gridDim-1, (box.maxX - 1) / kTileSize);
    const int ty1 = std::min(m_gridDim-1, (box.maxY - 1) / kTileSize);
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            const int key = tileKey(tx, ty, m_gridDim);
            m_pending.pre.emplace (key, readTile(*m_strokeSnapshot, tx, ty));
            m_pending.post.emplace(key, readTile(target, tx, ty));
        }
    }
    m_pending.rect = snapToTileGrid(m_pending.rect);

    truncateRedo();
    m_byteUsage += m_pending.bytes();
    m_entries.push_back(std::move(m_pending));
    m_top = static_cast<int>(m_entries.size());
    m_pending = Entry{};
    m_strokeSnapshot.reset();
    evictToFit();
}

void UndoHistory::abandonStroke()
{
    m_pending = Entry{};
    m_strokeSnapshot.reset();
    m_strokeOpen = false;
}

// ─────────────────────────────────────────────────────────────────
// Flood fill (single-shot record)
// ─────────────────────────────────────────────────────────────────

void UndoHistory::recordFloodFill(const PaintLayer& before,
                                  const PaintLayer& after,
                                  const PixelRect&  box)
{
    if (box.isEmpty()) return;

    Entry e;
    e.layerId = after.id();   // after.id() == before.id() (same layer, copied)

    // Walk the tiles overlapping `box`, capture pre from `before`, post from `after`.
    const int tx0 = std::max(0,            box.minX / kTileSize);
    const int ty0 = std::max(0,            box.minY / kTileSize);
    const int tx1 = std::min(m_gridDim-1, (box.maxX - 1) / kTileSize);
    const int ty1 = std::min(m_gridDim-1, (box.maxY - 1) / kTileSize);
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            const int key = tileKey(tx, ty, m_gridDim);
            e.pre.emplace (key, readTile(before, tx, ty));
            e.post.emplace(key, readTile(after,  tx, ty));
        }
    }
    if (e.pre.empty()) return;
    e.rect = snapToTileGrid(box);

    truncateRedo();
    m_byteUsage += e.bytes();
    m_entries.push_back(std::move(e));
    m_top = static_cast<int>(m_entries.size());
    evictToFit();
}

// ─────────────────────────────────────────────────────────────────
// Apply
// ─────────────────────────────────────────────────────────────────

PixelRect UndoHistory::undo()
{
    if (m_top == 0) return {};
    --m_top;
    const Entry& e = m_entries[m_top];
    Layer* L = m_owner ? m_owner->findLayerById(e.layerId) : nullptr;
    auto* paint = dynamic_cast<PaintLayer*>(L);
    if (!paint) return {};   // orphaned (layer deleted) — silent no-op
    applyTiles(*paint, e.pre);
    return e.rect;
}

PixelRect UndoHistory::redo()
{
    if (m_top >= static_cast<int>(m_entries.size())) return {};
    const Entry& e = m_entries[m_top];
    ++m_top;
    Layer* L = m_owner ? m_owner->findLayerById(e.layerId) : nullptr;
    auto* paint = dynamic_cast<PaintLayer*>(L);
    if (!paint) return {};
    applyTiles(*paint, e.post);
    return e.rect;
}

void UndoHistory::pruneByLayerId(uint64_t goneId)
{
    // Walk forwards, dropping any entry whose layerId matches. Adjust m_top
    // for each drop that was before the cursor.
    size_t writeIdx = 0;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].layerId == goneId) {
            m_byteUsage -= m_entries[i].bytes();
            if (static_cast<int>(i) < m_top) --m_top;
            // skip (drop)
        } else {
            if (writeIdx != i) m_entries[writeIdx] = std::move(m_entries[i]);
            ++writeIdx;
        }
    }
    m_entries.resize(writeIdx);
    m_top = std::clamp(m_top, 0, static_cast<int>(m_entries.size()));
}

void UndoHistory::clear()
{
    m_entries.clear();
    m_top = 0;
    m_byteUsage = 0;
    m_pending = Entry{};
    m_strokeSnapshot.reset();
    m_strokeOpen = false;
}

// ─────────────────────────────────────────────────────────────────
// Budget enforcement
// ─────────────────────────────────────────────────────────────────

void UndoHistory::truncateRedo()
{
    while (static_cast<int>(m_entries.size()) > m_top) {
        m_byteUsage -= m_entries.back().bytes();
        m_entries.pop_back();
    }
}

void UndoHistory::evictToFit()
{
    // Drop oldest entries until we're within both step count and byte budget.
    while (!m_entries.empty()
           && (static_cast<int>(m_entries.size()) > kMaxEntries
               || m_byteUsage > kMaxBytes)) {
        m_byteUsage -= m_entries.front().bytes();
        m_entries.erase(m_entries.begin());
        if (m_top > 0) --m_top;
    }
}

} // namespace editor
