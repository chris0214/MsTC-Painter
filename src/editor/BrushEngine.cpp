#include "BrushEngine.h"

#include <algorithm>
#include <cmath>

namespace editor {

BrushEngine::BrushEngine() = default;

void BrushEngine::setSettings(const BrushSettings& s)
{
    m_settings = s;
}

void BrushEngine::setInkValue(int channelIndex, uint8_t v)
{
    switch (channelIndex) {
    case 0: m_settings.inkR = v; break;
    case 1: m_settings.inkG = v; break;
    case 2: m_settings.inkB = v; break;
    case 3: m_settings.inkA = v; break;
    default: break;
    }
}

// ─────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────

namespace {

/// Number of points retained for weighted smoothing, derived from settings.smoothing.
/// 0 → 1 (no smoothing). 1 → 32 (very laggy, Lazy-Nezumi feel).
int smoothingBufferSize(float smoothing)
{
    smoothing = std::clamp(smoothing, 0.0f, 1.0f);
    const float n = 1.0f + smoothing * 31.0f;
    return static_cast<int>(std::round(n));
}

/// Linear-weighted mean of the buffer: weight[i] = i+1 (oldest = 1, newest = N).
/// Returns the smoothed (sx, sy) in the same coord space as the inputs.
std::pair<float, float> weightedMean(const std::deque<std::pair<float, float>>& buf)
{
    if (buf.empty()) return { 0.0f, 0.0f };
    float sx = 0.0f, sy = 0.0f, w = 0.0f;
    int i = 1;
    for (const auto& p : buf) {
        sx += p.first  * i;
        sy += p.second * i;
        w  += static_cast<float>(i);
        ++i;
    }
    return { sx / w, sy / w };
}

} // anonymous

// ─────────────────────────────────────────────────────────────────
// Stroke lifecycle
// ─────────────────────────────────────────────────────────────────

void BrushEngine::beginStroke(PixelTarget& target, float u, float v, PixelRect& outDirtyBox)
{
    const float x = u * target.size();
    const float y = v * target.size();

    m_stroking  = true;
    m_lastX     = x;
    m_lastY     = y;
    m_distAccum = 0.0f;

    m_smoothBuf.clear();
    m_smoothBuf.emplace_back(x, y);

    // Allocate wash alpha-cap mask, full target size, all zeroed.
    if (m_settings.flowMode == FlowMode::Wash) {
        m_strokeAlphaCap.assign(
            static_cast<size_t>(target.size()) * target.size(), 0);
        m_strokeMaskSize   = target.size();
        m_strokeMaskActive = true;
    } else {
        m_strokeAlphaCap.clear();
        m_strokeMaskActive = false;
    }

    // First dab at the click point.
    Dab d{ x, y, 1.0f, 0.0f, 0.0f };
    emitDab(target, d, outDirtyBox);
}

void BrushEngine::strokeTo(PixelTarget& target, float u, float v, PixelRect& outDirtyBox)
{
    if (!m_stroking) return;

    const float rawX = u * target.size();
    const float rawY = v * target.size();

    // ── Push raw sample into smoothing ring buffer ──────────────
    const int N = smoothingBufferSize(m_settings.smoothing);
    m_smoothBuf.emplace_back(rawX, rawY);
    while (static_cast<int>(m_smoothBuf.size()) > N) {
        m_smoothBuf.pop_front();
    }

    // ── Smoothed target this frame ──────────────────────────────
    const auto [tx, ty] = (N <= 1)
        ? std::pair<float, float>(rawX, rawY)
        : weightedMean(m_smoothBuf);

    const float dx  = tx - m_lastX;
    const float dy  = ty - m_lastY;
    const float len = std::sqrt(dx*dx + dy*dy);

    // Effective spacing in pixels.
    //
    // The slider value is an absolute pixel count (1-32 px). For normal-size
    // brushes this matches the user's expectation. But when the brush is
    // small (radius ~ 2-4 px) and spacing is 8 px, individual stamps don't
    // overlap → visible "dotted line" instead of a stroke.
    //
    // Cap spacing at 25% of brush DIAMETER, matching Photoshop / Substance
    // Painter / Krita defaults. This guarantees every stamp overlaps the
    // previous one regardless of brush size.
    const float diameter      = 2.0f * std::max(0.5f, m_settings.radiusPx);
    const float maxContinuous = diameter * 0.25f;
    const float spacing = std::max(
        1.0f,
        std::min(m_settings.spacingPx, maxContinuous));
    if (len <= 0.0f) return;

    float remaining = len + m_distAccum;
    float t = (spacing - m_distAccum) / len;

    while (t <= 1.0f) {
        const float sx = m_lastX + dx * t;
        const float sy = m_lastY + dy * t;
        Dab d{ sx, sy, 1.0f, 0.0f, 0.0f };
        emitDab(target, d, outDirtyBox);
        t += spacing / len;
        remaining -= spacing;
    }

    m_distAccum = std::max(0.0f, remaining);
    m_lastX = tx;
    m_lastY = ty;
}

void BrushEngine::endStroke()
{
    m_stroking         = false;
    m_distAccum        = 0.0f;
    m_strokeMaskActive = false;
    m_strokeAlphaCap.clear();
    m_smoothBuf.clear();
}

void BrushEngine::smoothedPoint(float& outX, float& outY) const
{
    if (!m_stroking) { outX = -1; outY = -1; return; }
    outX = m_lastX;
    outY = m_lastY;
}

// ─────────────────────────────────────────────────────────────────
// Dab emit (mirror handled here so wash mask sees both)
// ─────────────────────────────────────────────────────────────────

void BrushEngine::emitDab(PixelTarget& target, const Dab& d, PixelRect& outDirtyBox)
{
    stamp(target, d, outDirtyBox);

    if (m_settings.mirrorX) {
        Dab m = d;
        m.x = static_cast<float>(target.size()) - d.x;
        // Skip duplicate stamp if the mirrored point is the same as the original
        // (right on the seam). Avoids double-blend in buildup mode.
        if (std::abs(m.x - d.x) > 0.5f) {
            stamp(target, m, outDirtyBox);
        }
    }
}

// ─────────────────────────────────────────────────────────────────
// Single stamp — circular brush with hardness-controlled falloff
// ─────────────────────────────────────────────────────────────────

void BrushEngine::stamp(PixelTarget& target, const Dab& d, PixelRect& outDirtyBox)
{
    const int   N      = target.size();
    const float radius = std::max(0.5f, m_settings.radiusPx);
    const float r2     = radius * radius;

    const float innerR  = radius * std::clamp(m_settings.hardness, 0.0f, 1.0f);
    const float innerR2 = innerR * innerR;

    const int x0 = std::max(0,        static_cast<int>(std::floor(d.x - radius)));
    const int y0 = std::max(0,        static_cast<int>(std::floor(d.y - radius)));
    const int x1 = std::min(N - 1,    static_cast<int>(std::ceil (d.x + radius)));
    const int y1 = std::min(N - 1,    static_cast<int>(std::ceil (d.y + radius)));

    if (x0 > x1 || y0 > y1) return;

    const uint8_t targetR = m_settings.erase ? 0 : m_settings.inkR;
    const uint8_t targetG = m_settings.erase ? 0 : m_settings.inkG;
    const uint8_t targetB = m_settings.erase ? 0 : m_settings.inkB;
    const uint8_t targetA = m_settings.erase ? 0 : m_settings.inkA;

    const uint8_t mask    = m_settings.channelMask;
    const float   maxA    = std::clamp(m_settings.opacity, 0.0f, 1.0f);
    const bool    wash    = m_strokeMaskActive
                            && m_strokeMaskSize == N
                            && static_cast<int>(m_strokeAlphaCap.size())
                               == N * N;

    auto blend = [](uint8_t cur, uint8_t target, float a) -> uint8_t {
        const float v = static_cast<float>(cur) * (1.0f - a)
                      + static_cast<float>(target) * a;
        return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
    };

    uint8_t rgba[4];
    bool wroteAny = false;
    int  bx0 = N, by0 = N, bx1 = -1, by1 = -1;
    for (int y = y0; y <= y1; ++y) {
        const float dy = static_cast<float>(y) + 0.5f - d.y;
        for (int x = x0; x <= x1; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - d.x;
            const float dist2 = dx*dx + dy*dy;
            if (dist2 > r2) continue;

            // Smoothstep falloff from innerR² to r².
            float falloff = 1.0f;
            if (dist2 > innerR2 && r2 > innerR2) {
                const float tt = (dist2 - innerR2) / (r2 - innerR2);
                falloff = 1.0f - tt;
                falloff = falloff * falloff * (3.0f - 2.0f * falloff);
            }

            // Per-dab wanted alpha
            const float aWant = maxA * falloff;
            if (aWant <= 0.0f) continue;

            float aApply = aWant;
            if (wash) {
                // Wash: cap the cumulative alpha this stroke at aWant <= cap_max.
                // Translate from "wanted alpha" to "how much more alpha to apply
                // so that the running cap reaches max(prev, aWant)".
                const size_t idx = static_cast<size_t>(y) * N + x;
                const uint8_t prev8 = m_strokeAlphaCap[idx];
                const float   prev  = prev8 / 255.0f;
                if (aWant <= prev) continue;       // already darker than this dab wants
                const float newCap = aWant;        // (== max(prev, aWant) since aWant > prev)
                // The blend factor needed for the next-layer-over math so that
                //   1 - (1-prev) * (1-aApply) = newCap
                //   aApply = (newCap - prev) / (1 - prev)
                if (prev >= 0.9999f) continue;
                aApply = (newCap - prev) / (1.0f - prev);
                m_strokeAlphaCap[idx] = static_cast<uint8_t>(newCap * 255.0f + 0.5f);
            }

            if (aApply <= 0.0f) continue;

            target.getPixel(x, y, rgba);
            if (mask & ChannelMaskR) rgba[0] = blend(rgba[0], targetR, aApply);
            if (mask & ChannelMaskG) rgba[1] = blend(rgba[1], targetG, aApply);
            if (mask & ChannelMaskB) rgba[2] = blend(rgba[2], targetB, aApply);
            if (mask & ChannelMaskA) rgba[3] = blend(rgba[3], targetA, aApply);
            target.setPixel(x, y, rgba[0], rgba[1], rgba[2], rgba[3], mask);

            wroteAny = true;
            if (x < bx0) bx0 = x;
            if (y < by0) by0 = y;
            if (x > bx1) bx1 = x;
            if (y > by1) by1 = y;
        }
    }

    if (wroteAny) {
        // Union the per-stamp box into outDirtyBox so the caller can
        // recomposite the union of all stamps in this beginStroke/strokeTo
        // call in one go.
        if (outDirtyBox.isEmpty()) {
            outDirtyBox.minX = bx0;
            outDirtyBox.minY = by0;
            outDirtyBox.maxX = bx1 + 1;   // exclusive
            outDirtyBox.maxY = by1 + 1;
        } else {
            if (bx0       < outDirtyBox.minX) outDirtyBox.minX = bx0;
            if (by0       < outDirtyBox.minY) outDirtyBox.minY = by0;
            if (bx1 + 1   > outDirtyBox.maxX) outDirtyBox.maxX = bx1 + 1;
            if (by1 + 1   > outDirtyBox.maxY) outDirtyBox.maxY = by1 + 1;
        }
    }
}

} // namespace editor
