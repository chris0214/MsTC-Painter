#include "Layer.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace editor {

namespace {
/// Process-wide monotonic Layer ID source. Starts at 1; 0 reserved as
/// "no layer" sentinel for pruneByLayerId / similar.
std::atomic<uint64_t> g_nextLayerId{1};
} // anonymous

Layer::Layer()
    : m_id(g_nextLayerId.fetch_add(1, std::memory_order_relaxed))
{
}

// ─────────────────────────────────────────────────────────────────
// Name <-> enum mapping (stable across versions for serialization)
// ─────────────────────────────────────────────────────────────────

const char* blendModeName(BlendMode m)
{
    switch (m) {
    case BlendMode::Normal:   return "normal";
    case BlendMode::Multiply: return "multiply";
    case BlendMode::Screen:   return "screen";
    default:                  return "normal";
    }
}

bool blendModeFromName(const std::string& s, BlendMode& out)
{
    if (s == "normal")   { out = BlendMode::Normal;   return true; }
    if (s == "multiply") { out = BlendMode::Multiply; return true; }
    if (s == "screen")   { out = BlendMode::Screen;   return true; }
    return false;
}

const char* layerTypeName(LayerType t)
{
    switch (t) {
    case LayerType::Paint:   return "paint";
    case LayerType::Fill:    return "fill";
    case LayerType::Image:   return "image";
    case LayerType::Channel: return "channel";
    default:                 return "paint";
    }
}

bool layerTypeFromName(const std::string& s, LayerType& out)
{
    if (s == "paint")   { out = LayerType::Paint;   return true; }
    if (s == "fill")    { out = LayerType::Fill;    return true; }
    if (s == "image")   { out = LayerType::Image;   return true; }
    if (s == "channel") { out = LayerType::Channel; return true; }
    return false;
}

// ─────────────────────────────────────────────────────────────────
// PaintLayer
// ─────────────────────────────────────────────────────────────────

PaintLayer::PaintLayer(int size)
    : m_size(size)
    , m_rgba(static_cast<size_t>(size) * size * 4, 0)
    , m_wrote(static_cast<size_t>(size) * size, 0)
{
    // Default alpha so a pixel-as-rgba reads (0,0,0,255). The wroteMask is
    // all-zero, so the compositor will fall through to lower layers (or the
    // doc default) for every pixel — equivalent to "blank layer."
    for (size_t i = 0; i < static_cast<size_t>(size) * size; ++i) {
        m_rgba[i * 4 + 3] = 255;
    }
}

void PaintLayer::clear()
{
    std::fill(m_rgba.begin(),  m_rgba.end(),  uint8_t{0});
    std::fill(m_wrote.begin(), m_wrote.end(), uint8_t{0});
    for (size_t i = 0; i < static_cast<size_t>(m_size) * m_size; ++i) {
        m_rgba[i * 4 + 3] = 255;
    }
}

void PaintLayer::setAll(const uint8_t* rgbaSize2x4, const uint8_t* wroteSize2)
{
    std::memcpy(m_rgba.data(), rgbaSize2x4,
                static_cast<size_t>(m_size) * m_size * 4);
    if (wroteSize2) {
        std::memcpy(m_wrote.data(), wroteSize2,
                    static_cast<size_t>(m_size) * m_size);
    } else {
        // No mask provided — assume "everything authored," typical for v1
        // project migration where the old format had no wroteMask.
        std::fill(m_wrote.begin(), m_wrote.end(), uint8_t{0xF});
    }
}

void PaintLayer::sample(int x, int y,
                        uint8_t outRgba[4],
                        ChannelAuthorMask& outChannels) const
{
    const size_t idx = static_cast<size_t>(y) * m_size + x;
    const size_t pi  = idx * 4;
    outRgba[0] = m_rgba[pi + 0];
    outRgba[1] = m_rgba[pi + 1];
    outRgba[2] = m_rgba[pi + 2];
    outRgba[3] = m_rgba[pi + 3];
    outChannels = m_wrote[idx];
}

void PaintLayer::getPixel(int x, int y, uint8_t out[4]) const
{
    const size_t i = (static_cast<size_t>(y) * m_size + x) * 4;
    out[0] = m_rgba[i + 0];
    out[1] = m_rgba[i + 1];
    out[2] = m_rgba[i + 2];
    out[3] = m_rgba[i + 3];
}

void PaintLayer::setPixel(int x, int y,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                          ChannelAuthorMask channelsAuthored)
{
    if (x < 0 || y < 0 || x >= m_size || y >= m_size) return;
    const size_t idx = static_cast<size_t>(y) * m_size + x;
    const size_t pi  = idx * 4;
    m_rgba[pi + 0] = r;
    m_rgba[pi + 1] = g;
    m_rgba[pi + 2] = b;
    m_rgba[pi + 3] = a;
    m_wrote[idx] = static_cast<uint8_t>(m_wrote[idx] | channelsAuthored);
}

// ─────────────────────────────────────────────────────────────────
// FillLayer
// ─────────────────────────────────────────────────────────────────

FillLayer::FillLayer(int size,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                     ChannelAuthorMask channelMask)
    : m_size(size)
    , m_channelMask(channelMask)
{
    m_rgba[0] = r;
    m_rgba[1] = g;
    m_rgba[2] = b;
    m_rgba[3] = a;
}

void FillLayer::setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    m_rgba[0] = r;
    m_rgba[1] = g;
    m_rgba[2] = b;
    m_rgba[3] = a;
}

void FillLayer::setSelection(std::vector<uint8_t> sel)
{
    m_selection = std::move(sel);
    if (m_selection.size() != static_cast<size_t>(m_size) * m_size) {
        m_selection.clear();   // size mismatch — treat as "everywhere"
    }
}

void FillLayer::sample(int x, int y,
                       uint8_t outRgba[4],
                       ChannelAuthorMask& outChannels) const
{
    if (!m_selection.empty()) {
        if (m_selection[static_cast<size_t>(y) * m_size + x] == 0) {
            outChannels = 0;
            return;
        }
    }
    outRgba[0] = m_rgba[0];
    outRgba[1] = m_rgba[1];
    outRgba[2] = m_rgba[2];
    outRgba[3] = m_rgba[3];
    outChannels = m_channelMask;
}

// ─────────────────────────────────────────────────────────────────
// ImageLayer (nearest-neighbor resample at construction)
// ─────────────────────────────────────────────────────────────────

ImageLayer::ImageLayer(int targetSize,
                       const uint8_t* srcRgba, int srcSize,
                       ChannelAuthorMask channelMask)
    : m_size(targetSize)
    , m_rgba(static_cast<size_t>(targetSize) * targetSize * 4, 0)
    , m_channelMask(channelMask)
{
    if (!srcRgba || srcSize <= 0) {
        // Empty source — leave as default-init (transparent black). sample()
        // will still report channelMask, but values will be zero.
        return;
    }
    if (srcSize == targetSize) {
        std::memcpy(m_rgba.data(), srcRgba,
                    static_cast<size_t>(targetSize) * targetSize * 4);
        return;
    }
    // Nearest-neighbor resample. Quick + adequate for "reference image" UX.
    // (Bilinear / area average can come later if users complain about quality.)
    const float sx = static_cast<float>(srcSize) / targetSize;
    const float sy = static_cast<float>(srcSize) / targetSize;
    for (int y = 0; y < targetSize; ++y) {
        const int srcY = std::min(srcSize - 1, static_cast<int>(y * sy));
        for (int x = 0; x < targetSize; ++x) {
            const int srcX = std::min(srcSize - 1, static_cast<int>(x * sx));
            const size_t si = (static_cast<size_t>(srcY) * srcSize + srcX) * 4;
            const size_t di = (static_cast<size_t>(y) * targetSize + x) * 4;
            m_rgba[di + 0] = srcRgba[si + 0];
            m_rgba[di + 1] = srcRgba[si + 1];
            m_rgba[di + 2] = srcRgba[si + 2];
            m_rgba[di + 3] = srcRgba[si + 3];
        }
    }
}

void ImageLayer::sample(int x, int y,
                        uint8_t outRgba[4],
                        ChannelAuthorMask& outChannels) const
{
    const size_t i = (static_cast<size_t>(y) * m_size + x) * 4;
    outRgba[0] = m_rgba[i + 0];
    outRgba[1] = m_rgba[i + 1];
    outRgba[2] = m_rgba[i + 2];
    outRgba[3] = m_rgba[i + 3];
    outChannels = m_channelMask;
}

// ─────────────────────────────────────────────────────────────────
// ChannelLayer (nearest-neighbor source sample at sample())
// ─────────────────────────────────────────────────────────────────

ChannelLayer::ChannelLayer(int targetSize,
                           std::shared_ptr<std::vector<uint8_t>> srcRgba,
                           int srcSize,
                           uint8_t srcChannel,
                           uint8_t dstChannel)
    : m_size(targetSize)
    , m_src(std::move(srcRgba))
    , m_srcSize(srcSize)
    , m_srcChannel(static_cast<uint8_t>(srcChannel & 3))
    , m_dstChannel(static_cast<uint8_t>(dstChannel & 3))
{
}

void ChannelLayer::sample(int x, int y,
                          uint8_t outRgba[4],
                          ChannelAuthorMask& outChannels) const
{
    outRgba[0] = 0; outRgba[1] = 0; outRgba[2] = 0; outRgba[3] = 0;
    if (!m_src || m_srcSize <= 0
        || m_src->size() < static_cast<size_t>(m_srcSize) * m_srcSize * 4) {
        outChannels = 0;
        return;
    }
    // Map (x,y) → src pixel via NN.
    int srcX = (m_srcSize == m_size) ? x
        : std::min(m_srcSize - 1,
                   static_cast<int>(static_cast<float>(x) * m_srcSize / m_size));
    int srcY = (m_srcSize == m_size) ? y
        : std::min(m_srcSize - 1,
                   static_cast<int>(static_cast<float>(y) * m_srcSize / m_size));

    const uint8_t* src = m_src->data();
    const size_t i = (static_cast<size_t>(srcY) * m_srcSize + srcX) * 4;
    const uint8_t v = src[i + m_srcChannel];

    outRgba[m_dstChannel] = v;
    outChannels = static_cast<ChannelAuthorMask>(1u << m_dstChannel);
}

} // namespace editor
