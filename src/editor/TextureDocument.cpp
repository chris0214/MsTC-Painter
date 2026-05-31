#include "TextureDocument.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace editor {

const char* textureKindName(TextureKind k)
{
    switch (k) {
    case TextureKind::ShadowRate:   return "Shadow Rate";
    case TextureKind::SubLightRate: return "SubLight Rate";
    case TextureKind::EdgeRate:     return "Edge Rate";
    case TextureKind::FaceSDF:      return "Face SDF";
    default:                        return "<unknown>";
    }
}

TextureDocument::TextureDocument(TextureKind kind, int size)
    : m_kind(kind)
    , m_size(size)
    , m_name(textureKindName(kind))
{
    assert(size > 0 && (size & (size - 1)) == 0 && "size must be power of two");

    m_pixels.resize(static_cast<size_t>(size) * size * 4);
    clear(kDefaultR, kDefaultG, kDefaultB, kDefaultA);

    const int tileGridSide = tileGridDim();
    for (auto& bm : m_tileDirty) {
        bm.assign(static_cast<size_t>(tileGridSide) * tileGridSide, 0);
    }
    m_dirtyTileCount.fill(0);
    // clear() above set everything dirty for both consumers; reset baseline.
    // Likewise: clear() marks the doc as "ever modified" — that's intended
    // for *user* clears, not the initial fill. Roll back so a freshly-
    // constructed doc reports "untouched."
    m_everModified = false;
}

// ─────────────────────────────────────────────────────────────────
// Pixel access
// ─────────────────────────────────────────────────────────────────

void TextureDocument::getPixel(int x, int y, uint8_t* out) const
{
    const size_t i = (static_cast<size_t>(y) * m_size + x) * 4;
    out[0] = m_pixels[i + 0];
    out[1] = m_pixels[i + 1];
    out[2] = m_pixels[i + 2];
    out[3] = m_pixels[i + 3];
}

void TextureDocument::setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                                ChannelAuthorMask /*channelsAuthored*/)
{
    if (x < 0 || y < 0 || x >= m_size || y >= m_size) return;
    const size_t i = (static_cast<size_t>(y) * m_size + x) * 4;
    m_pixels[i + 0] = r;
    m_pixels[i + 1] = g;
    m_pixels[i + 2] = b;
    m_pixels[i + 3] = a;
    markTileDirty(x / kTileSize, y / kTileSize);
    m_everModified = true;
    // channelsAuthored is meaningful for PaintLayer's wroteMask but not for
    // the composite cache — the composite has no per-channel authorship.
}

// ─────────────────────────────────────────────────────────────────
// Bulk ops
// ─────────────────────────────────────────────────────────────────

void TextureDocument::clear(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    const size_t totalTexels = static_cast<size_t>(m_size) * m_size;
    for (size_t i = 0; i < totalTexels; ++i) {
        m_pixels[i*4 + 0] = r;
        m_pixels[i*4 + 1] = g;
        m_pixels[i*4 + 2] = b;
        m_pixels[i*4 + 3] = a;
    }
    // Mark everything dirty for ALL consumers.
    for (size_t c = 0; c < kNumConsumers; ++c) {
        std::fill(m_tileDirty[c].begin(), m_tileDirty[c].end(), uint8_t{1});
        m_dirtyTileCount[c] = static_cast<int>(m_tileDirty[c].size());
    }
    m_everModified = true;   // construction explicitly resets this afterwards
}

void TextureDocument::setPixelsAll(const uint8_t* rgba)
{
    std::memcpy(m_pixels.data(), rgba, m_pixels.size());
    for (size_t c = 0; c < kNumConsumers; ++c) {
        std::fill(m_tileDirty[c].begin(), m_tileDirty[c].end(), uint8_t{1});
        m_dirtyTileCount[c] = static_cast<int>(m_tileDirty[c].size());
    }
    m_everModified = true;
}

void TextureDocument::markDirty(const PixelRect& r)
{
    if (r.isEmpty()) return;

    const int x0 = std::clamp(r.minX, 0, m_size);
    const int y0 = std::clamp(r.minY, 0, m_size);
    const int x1 = std::clamp(r.maxX, 0, m_size);
    const int y1 = std::clamp(r.maxY, 0, m_size);

    const int tx0 = x0 / kTileSize;
    const int ty0 = y0 / kTileSize;
    const int tx1 = (x1 - 1) / kTileSize;
    const int ty1 = (y1 - 1) / kTileSize;

    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            markTileDirty(tx, ty);
        }
    }
}

// ─────────────────────────────────────────────────────────────────
// Dirty tile tracking (per consumer)
// ─────────────────────────────────────────────────────────────────

void TextureDocument::markTileDirty(int tileX, int tileY)
{
    const int dim = tileGridDim();
    if (tileX < 0 || tileY < 0 || tileX >= dim || tileY >= dim) return;
    const size_t idx = static_cast<size_t>(tileY) * dim + tileX;
    for (size_t c = 0; c < kNumConsumers; ++c) {
        if (!m_tileDirty[c][idx]) {
            m_tileDirty[c][idx] = 1;
            ++m_dirtyTileCount[c];
        }
    }
}

bool TextureDocument::hasDirtyTiles(DirtyConsumer c) const
{
    return m_dirtyTileCount[static_cast<size_t>(c)] > 0;
}

PixelRect TextureDocument::peekDirtyBounds(DirtyConsumer consumer) const
{
    const size_t c = static_cast<size_t>(consumer);
    if (m_dirtyTileCount[c] == 0) return {};

    const int dim = tileGridDim();
    int tMinX = dim, tMinY = dim, tMaxX = -1, tMaxY = -1;

    const auto& bm = m_tileDirty[c];
    for (int ty = 0; ty < dim; ++ty) {
        for (int tx = 0; tx < dim; ++tx) {
            if (bm[static_cast<size_t>(ty) * dim + tx]) {
                tMinX = std::min(tMinX, tx);
                tMinY = std::min(tMinY, ty);
                tMaxX = std::max(tMaxX, tx);
                tMaxY = std::max(tMaxY, ty);
            }
        }
    }

    PixelRect r;
    r.minX = tMinX * kTileSize;
    r.minY = tMinY * kTileSize;
    r.maxX = std::min((tMaxX + 1) * kTileSize, m_size);
    r.maxY = std::min((tMaxY + 1) * kTileSize, m_size);
    return r;
}

PixelRect TextureDocument::takeDirtyBounds(DirtyConsumer consumer)
{
    const PixelRect r = peekDirtyBounds(consumer);
    const size_t c = static_cast<size_t>(consumer);
    std::fill(m_tileDirty[c].begin(), m_tileDirty[c].end(), uint8_t{0});
    m_dirtyTileCount[c] = 0;
    return r;
}

} // namespace editor
