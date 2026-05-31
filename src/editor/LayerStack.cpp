#include "LayerStack.h"

#include <algorithm>

namespace editor {

namespace {

/// Linear interpolation in 0..255 space.
inline uint8_t lerp8(uint8_t cur, uint8_t target, float a)
{
    const float v = static_cast<float>(cur)    * (1.0f - a)
                  + static_cast<float>(target) * a;
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
}

} // anonymous

// ─────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────

LayerStack::LayerStack(TextureKind kind, int size)
    : m_kind(kind)
    , m_size(size)
    , m_composite(std::make_unique<TextureDocument>(kind, size))
    , m_history(this, size)
{
    // Always start with a base PaintLayer so the brush has somewhere to
    // write. Name "Base" matches how the Photoshop/SP "background" layer
    // reads to artists.
    auto base = std::make_unique<PaintLayer>(size);
    base->name    = "Base";
    base->visible = true;
    base->opacity = 1.0f;
    m_layers.push_back(std::move(base));
    m_active = 0;
    // Composite already at default (TextureDocument's clear sets 0,0,0,255
    // which matches our compositor's "no layer wrote anything" fallback).
    // No need to recomposite.
}

// ─────────────────────────────────────────────────────────────────
// Active-layer accessors
// ─────────────────────────────────────────────────────────────────

Layer* LayerStack::activeLayer()
{
    if (m_active < 0 || m_active >= layerCount()) return nullptr;
    return m_layers[m_active].get();
}

PixelTarget* LayerStack::activeTarget()
{
    auto* l = activeLayer();
    return l ? l->asPixelTarget() : nullptr;
}

void LayerStack::setActiveIndex(int idx)
{
    if (idx < 0 || idx >= layerCount()) return;
    m_active = idx;
}

Layer* LayerStack::findLayerById(uint64_t id)
{
    for (auto& up : m_layers) {
        if (up && up->id() == id) return up.get();
    }
    return nullptr;
}

const Layer* LayerStack::findLayerById(uint64_t id) const
{
    for (const auto& up : m_layers) {
        if (up && up->id() == id) return up.get();
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────
// Mutations
// ─────────────────────────────────────────────────────────────────

int LayerStack::addLayer(std::unique_ptr<Layer> l)
{
    m_layers.push_back(std::move(l));
    const int idx = layerCount() - 1;
    m_active = idx;
    recompositeAll();
    return idx;
}

int LayerStack::insertLayer(int at, std::unique_ptr<Layer> l)
{
    const int N = layerCount();
    at = std::clamp(at, 0, N);
    m_layers.insert(m_layers.begin() + at, std::move(l));
    m_active = at;
    recompositeAll();
    return at;
}

bool LayerStack::removeLayer(int idx)
{
    if (idx < 0 || idx >= layerCount())  return false;
    if (layerCount() <= 1)                return false;   // keep the stack non-empty
    const uint64_t goneId = m_layers[idx]->id();
    m_layers.erase(m_layers.begin() + idx);
    m_history.pruneByLayerId(goneId);   // drop dangling undo entries
    if (m_active >= layerCount()) m_active = layerCount() - 1;
    if (m_active < 0)             m_active = 0;
    recompositeAll();
    return true;
}

bool LayerStack::moveLayer(int from, int to)
{
    const int N = layerCount();
    if (from < 0 || from >= N) return false;
    to = std::clamp(to, 0, N - 1);
    if (from == to) return true;
    auto layer = std::move(m_layers[from]);
    m_layers.erase(m_layers.begin() + from);
    m_layers.insert(m_layers.begin() + to, std::move(layer));
    if (m_active == from)               m_active = to;
    else if (from < m_active && m_active <= to)   --m_active;
    else if (to <= m_active && m_active < from)   ++m_active;
    recompositeAll();
    return true;
}

bool LayerStack::mergeDown(int idx)
{
    if (idx <= 0 || idx >= layerCount()) return false;

    // Composite layer[idx] over layer[idx-1] into a fresh PaintLayer that
    // captures the combined authored channels per pixel.
    //
    // Algorithm: for each pixel, sample the two layers (top first, then
    // below). Use the same per-channel replace rule as the main compositor
    // — top wins for channels it authored, bottom fills in the rest.
    // Visibility/opacity bake into the result.
    auto* upper = m_layers[idx].get();
    auto* lower = m_layers[idx - 1].get();
    if (!upper || !lower) return false;

    auto merged = std::make_unique<PaintLayer>(m_size);
    merged->name      = lower->name + " + " + upper->name;
    merged->visible   = lower->visible;            // keep the lower's visibility
    merged->opacity   = 1.0f;                       // bake opacity into pixels
    merged->blendMode = BlendMode::Normal;

    for (int y = 0; y < m_size; ++y) {
        for (int x = 0; x < m_size; ++x) {
            uint8_t up[4] = {0,0,0,0}, lo[4] = {0,0,0,255};
            ChannelAuthorMask upMask = 0, loMask = 0;
            if (upper->visible) upper->sample(x, y, up, upMask);
            if (lower->visible) lower->sample(x, y, lo, loMask);

            // Start from the lower's contribution (with its opacity baked).
            uint8_t out[4] = {0, 0, 0, 255};
            ChannelAuthorMask outMask = 0;
            if (loMask) {
                const float a = std::clamp(lower->opacity, 0.0f, 1.0f);
                if (loMask & 0x1) out[0] = static_cast<uint8_t>(lo[0] * a + 0.5f);
                if (loMask & 0x2) out[1] = static_cast<uint8_t>(lo[1] * a + 0.5f);
                if (loMask & 0x4) out[2] = static_cast<uint8_t>(lo[2] * a + 0.5f);
                if (loMask & 0x8) out[3] = static_cast<uint8_t>(lo[3] * a + 0.5f);
                outMask = loMask;
            }

            // Lerp the upper's channels (where authored) into the running.
            if (upMask) {
                const float a = std::clamp(upper->opacity, 0.0f, 1.0f);
                if (upMask & 0x1) {
                    const uint8_t cur = out[0];
                    out[0] = static_cast<uint8_t>(cur * (1.0f - a) + up[0] * a + 0.5f);
                }
                if (upMask & 0x2) {
                    const uint8_t cur = out[1];
                    out[1] = static_cast<uint8_t>(cur * (1.0f - a) + up[1] * a + 0.5f);
                }
                if (upMask & 0x4) {
                    const uint8_t cur = out[2];
                    out[2] = static_cast<uint8_t>(cur * (1.0f - a) + up[2] * a + 0.5f);
                }
                if (upMask & 0x8) {
                    const uint8_t cur = out[3];
                    out[3] = static_cast<uint8_t>(cur * (1.0f - a) + up[3] * a + 0.5f);
                }
                outMask = static_cast<ChannelAuthorMask>(outMask | upMask);
            }

            if (outMask) {
                merged->setPixel(x, y, out[0], out[1], out[2], out[3], outMask);
            }
        }
    }

    // Replace [idx-1] with merged; remove [idx]. Order matters: do the
    // upper-remove first so [idx-1] still points at the lower.
    const uint64_t upperId = m_layers[idx]->id();
    const uint64_t lowerId = m_layers[idx - 1]->id();
    m_layers.erase(m_layers.begin() + idx);
    m_layers[idx - 1] = std::move(merged);
    // Both layer IDs are now invalid (upper destroyed; lower replaced).
    // Drop any undo history that targeted them — restoring pixels onto a
    // merged-and-destroyed layer would corrupt the new merged layer.
    m_history.pruneByLayerId(upperId);
    m_history.pruneByLayerId(lowerId);
    if (m_active >= idx)       m_active = idx - 1;
    if (m_active >= layerCount()) m_active = layerCount() - 1;
    recompositeAll();
    return true;
}

// ─────────────────────────────────────────────────────────────────
// Compositor — per-channel replace, top-down
// ─────────────────────────────────────────────────────────────────

void LayerStack::compositePixel(int x, int y, uint8_t outRgba[4]) const
{
    // Doc default: black fully opaque.
    outRgba[0] = 0; outRgba[1] = 0; outRgba[2] = 0; outRgba[3] = 255;
    ChannelAuthorMask resolved = 0;

    // Walk top-down. For each channel: first authoring layer wins, modulated
    // by that layer's opacity (lerping into whatever the lower layers have
    // accumulated so far in outRgba).
    for (int li = layerCount() - 1; li >= 0; --li) {
        const Layer* L = m_layers[li].get();
        if (!L || !L->visible || L->opacity <= 0.0f) continue;

        uint8_t lRgba[4] = {0, 0, 0, 0};
        ChannelAuthorMask lWrote = 0;
        L->sample(x, y, lRgba, lWrote);

        // Only consider channels this layer authored AND not yet resolved.
        const ChannelAuthorMask consider = static_cast<ChannelAuthorMask>(
            lWrote & ~resolved);
        if (!consider) continue;

        const float a = std::clamp(L->opacity, 0.0f, 1.0f);
        if (consider & 0x1) outRgba[0] = lerp8(outRgba[0], lRgba[0], a);
        if (consider & 0x2) outRgba[1] = lerp8(outRgba[1], lRgba[1], a);
        if (consider & 0x4) outRgba[2] = lerp8(outRgba[2], lRgba[2], a);
        if (consider & 0x8) outRgba[3] = lerp8(outRgba[3], lRgba[3], a);

        resolved = static_cast<ChannelAuthorMask>(resolved | consider);
        if (resolved == 0xF) break;
    }
}

void LayerStack::recompositeRect(const PixelRect& r)
{
    if (r.isEmpty()) return;
    const int x0 = std::max(0,        r.minX);
    const int y0 = std::max(0,        r.minY);
    const int x1 = std::min(m_size,   r.maxX);
    const int y1 = std::min(m_size,   r.maxY);
    if (x0 >= x1 || y0 >= y1) return;

    uint8_t rgba[4];
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            compositePixel(x, y, rgba);
            // setPixel marks tile dirty for both Canvas + Gpu consumers.
            m_composite->setPixel(x, y, rgba[0], rgba[1], rgba[2], rgba[3]);
        }
    }
}

void LayerStack::recompositeAll()
{
    PixelRect full;
    full.minX = 0; full.minY = 0; full.maxX = m_size; full.maxY = m_size;
    recompositeRect(full);
}

} // namespace editor
