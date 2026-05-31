#include "FloodFill.h"

#include <algorithm>
#include <cstdlib>
#include <queue>
#include <vector>

namespace editor {

namespace {

/// Which channel index (0..3) is the "active" one (first set bit of channelMask).
/// Returns -1 if no channel selected.
int activeChannel(uint8_t mask)
{
    for (int i = 0; i < 4; ++i) {
        if (mask & (1u << i)) return i;
    }
    return -1;
}

} // anonymous

// ─────────────────────────────────────────────────────────────────
// Public
// ─────────────────────────────────────────────────────────────────

int floodFill(const CompositeView& src,
              PixelTarget&         dst,
              int                  startX,
              int                  startY,
              const FillSettings&  s,
              const uint8_t*       uvMask,
              PixelRect&           outDirtyBox)
{
    const int N = src.size();
    if (N <= 0 || dst.size() != N) return 0;
    if (startX < 0 || startY < 0 || startX >= N || startY >= N) return 0;

    const int activeCh = activeChannel(s.channelMask);
    if (activeCh < 0) return 0;

    const bool useColor = (s.scope == FillScope::ColorConnected ||
                           s.scope == FillScope::ColorAndUv);
    const bool useUv    = (s.scope == FillScope::UvIsland ||
                           s.scope == FillScope::ColorAndUv);
    if (useUv && !uvMask) return 0;   // caller error — fail safely

    // Seed pixel's active-channel value (only used in color modes).
    // Always read from the *composite* so tolerance behaves as PS does.
    uint8_t seedRgba[4];
    src.getPixel(startX, startY, seedRgba);
    const uint8_t seedVal = seedRgba[activeCh];

    // Optionally bail if seed isn't on UV (UV-bound modes).
    if (useUv && uvMask[startY * N + startX] == 0) return 0;

    // Visited bitmap (1 byte per pixel — coarse but cheapest in code).
    std::vector<uint8_t> visited(static_cast<size_t>(N) * N, 0);

    // 4-connected flood. std::queue here; for 4M pixels worst-case the
    // deque grows ~half a million entries max, still cheap.
    struct PixelStack { std::vector<int32_t> xs, ys; };
    PixelStack stk;
    stk.xs.reserve(1024);
    stk.ys.reserve(1024);

    auto push = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= N || y >= N) return;
        const size_t idx = static_cast<size_t>(y) * N + x;
        if (visited[idx]) return;
        if (useUv && uvMask[idx] == 0) return;
        if (useColor) {
            uint8_t px[4];
            src.getPixel(x, y, px);
            if (std::abs(int(px[activeCh]) - int(seedVal)) > int(s.tolerance)) return;
        }
        visited[idx] = 1;
        stk.xs.push_back(x);
        stk.ys.push_back(y);
    };

    push(startX, startY);

    // Per-channel blend math (same as BrushEngine but simpler — no falloff).
    const float a = std::clamp(s.opacity, 0.0f, 1.0f);
    auto blend = [a](uint8_t cur, uint8_t target) -> uint8_t {
        const float v = cur * (1.0f - a) + target * a;
        return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
    };

    int filled = 0;
    int bx0 = N, by0 = N, bx1 = -1, by1 = -1;
    uint8_t rgba[4];
    while (!stk.xs.empty()) {
        const int x = stk.xs.back();
        const int y = stk.ys.back();
        stk.xs.pop_back();
        stk.ys.pop_back();

        // Read CURRENT layer pixel (so blending against partially-painted
        // pixels in the same fill does the right thing if a layer doesn't
        // start blank). For Phase 8 PaintLayer, pixels at (x,y) where
        // wroteMask is clear in the active channel read back as 0, which
        // is the correct "paint over default" behavior.
        dst.getPixel(x, y, rgba);
        if (s.channelMask & 0x1) rgba[0] = blend(rgba[0], s.inkR);
        if (s.channelMask & 0x2) rgba[1] = blend(rgba[1], s.inkG);
        if (s.channelMask & 0x4) rgba[2] = blend(rgba[2], s.inkB);
        if (s.channelMask & 0x8) rgba[3] = blend(rgba[3], s.inkA);
        dst.setPixel(x, y, rgba[0], rgba[1], rgba[2], rgba[3], s.channelMask);
        ++filled;
        if (x < bx0) bx0 = x;
        if (y < by0) by0 = y;
        if (x > bx1) bx1 = x;
        if (y > by1) by1 = y;

        // Expand 4-neighbors
        push(x + 1, y);
        push(x - 1, y);
        push(x, y + 1);
        push(x, y - 1);
    }

    if (filled > 0) {
        if (outDirtyBox.isEmpty()) {
            outDirtyBox.minX = bx0;
            outDirtyBox.minY = by0;
            outDirtyBox.maxX = bx1 + 1;
            outDirtyBox.maxY = by1 + 1;
        } else {
            if (bx0     < outDirtyBox.minX) outDirtyBox.minX = bx0;
            if (by0     < outDirtyBox.minY) outDirtyBox.minY = by0;
            if (bx1 + 1 > outDirtyBox.maxX) outDirtyBox.maxX = bx1 + 1;
            if (by1 + 1 > outDirtyBox.maxY) outDirtyBox.maxY = by1 + 1;
        }
    }

    return filled;
}

} // namespace editor
