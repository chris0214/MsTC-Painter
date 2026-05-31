// Standalone smoke tests for BrushEngine + TextureDocument.
//
// Covers:
//   1. Basic stamp + channel mask + dirty bounds (Phase 4)
//   2. Eraser mode (Phase 4)
//   3. Wash mode caps stroke alpha at opacity (Phase 4.1)
//   4. Buildup mode accumulates past opacity (Phase 4.1)
//   5. Mirror X paints symmetric points (Phase 4.1)
//
// Run via: run_brush_test.bat (from project root)

#include "editor/BrushEngine.h"
#include "editor/TextureDocument.h"

#include <cstdio>
#include <cstdlib>

using namespace editor;

static int s_failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #expr); \
        ++s_failures; \
    } \
} while (0)

static void testBasicStrokeAndMask()
{
    std::puts("\n--- Test: basic stroke + channel mask ---");
    constexpr int N = 256;
    TextureDocument doc(TextureKind::ShadowRate, N);
    doc.takeDirtyBounds();

    BrushEngine brush;
    BrushSettings s;
    s.radiusPx    = 16;
    s.hardness    = 0.5f;
    s.opacity     = 1.0f;
    s.spacingPx   = 4;
    s.channelMask = ChannelMaskR;
    s.inkR = 200;
    s.inkG = 200;       // these should be ignored due to mask
    s.inkB = 200;
    s.flowMode = FlowMode::Buildup;  // simpler accumulating math for this test
    brush.setSettings(s);

    PixelRect dbox;
    brush.beginStroke(doc, 0.25f, 0.5f, dbox);
    brush.strokeTo(doc, 0.50f, 0.5f, dbox);
    brush.strokeTo(doc, 0.75f, 0.5f, dbox);
    brush.endStroke();

    uint8_t px[4];
    doc.getPixel(N/2, N/2, px);
    std::printf("  Center: (%u,%u,%u,%u)\n", px[0], px[1], px[2], px[3]);
    CHECK(px[0] >= 150);
    CHECK(px[1] == 0);
    CHECK(px[2] == 0);

    doc.getPixel(5, 5, px);
    CHECK(px[0] == 0 && px[1] == 0 && px[2] == 0);

    doc.getPixel(N/2, N/2 - 50, px);
    CHECK(px[0] == 0);

    PixelRect dirty = doc.peekDirtyBounds();
    CHECK(!dirty.isEmpty());
    CHECK(dirty.minX <= 64 + 16);
    CHECK(dirty.maxX >= 192 - 16);
}

static void testEraser()
{
    std::puts("\n--- Test: eraser ---");
    constexpr int N = 256;
    TextureDocument doc(TextureKind::ShadowRate, N);
    doc.clear(200, 200, 200, 255);
    doc.takeDirtyBounds();

    BrushEngine brush;
    BrushSettings s;
    s.radiusPx = 20;
    s.hardness = 1.0f;
    s.opacity  = 1.0f;
    s.channelMask = ChannelMaskR;
    s.erase = true;
    s.flowMode = FlowMode::Buildup;
    brush.setSettings(s);

    PixelRect dbox2;
    brush.beginStroke(doc, 0.5f, 0.5f, dbox2);
    brush.endStroke();

    uint8_t px[4];
    doc.getPixel(N/2, N/2, px);
    std::printf("  Erased center R = %u\n", px[0]);
    CHECK(px[0] < 50);
}

static void testWashCapping()
{
    std::puts("\n--- Test: Wash caps a single stroke at opacity ---");
    constexpr int N = 64;
    TextureDocument doc(TextureKind::ShadowRate, N);
    doc.takeDirtyBounds();

    BrushEngine brush;
    BrushSettings s;
    s.radiusPx    = 10;
    s.hardness    = 1.0f;   // hard so center is full opacity
    s.opacity     = 0.5f;   // cap at 50%
    s.spacingPx   = 1;
    s.channelMask = ChannelMaskR;
    s.inkR = 255;
    s.flowMode = FlowMode::Wash;
    brush.setSettings(s);

    // Single stroke that passes over the center pixel many times by going
    // back-and-forth through it. The walker will emit dabs at every spacing
    // step, all overlapping the center.
    PixelRect dbox3;
    brush.beginStroke(doc, 0.4f, 0.5f, dbox3);
    for (int i = 0; i < 10; ++i) {
        brush.strokeTo(doc, 0.6f, 0.5f, dbox3);
        brush.strokeTo(doc, 0.4f, 0.5f, dbox3);
    }
    brush.endStroke();

    uint8_t px[4];
    doc.getPixel(N/2, N/2, px);
    std::printf("  After back-and-forth Wash stroke (op=50%%): R = %u (expect ~128, NOT >128)\n", px[0]);
    // Wash should produce ≤ 50% of 255 → ~128. Allow a few-percent rounding slack.
    CHECK(px[0] >= 120 && px[0] <= 135);
}

static void testBuildupExceedsOpacity()
{
    std::puts("\n--- Test: Buildup accumulates past opacity ---");
    constexpr int N = 64;
    TextureDocument doc(TextureKind::ShadowRate, N);
    doc.takeDirtyBounds();

    BrushEngine brush;
    BrushSettings s;
    s.radiusPx    = 10;
    s.hardness    = 1.0f;
    s.opacity     = 0.5f;
    s.spacingPx   = 1;   // minimum spacing
    s.channelMask = ChannelMaskR;
    s.inkR = 255;
    s.flowMode = FlowMode::Buildup;
    brush.setSettings(s);

    // strokeTo() with zero distance is a no-op; we have to do separate strokes
    // to get repeated dabs at the same point in Buildup mode (otherwise the
    // walker emits exactly one dab and never advances). This simulates the
    // real user behavior of clicking 30 times at the same pixel.
    for (int i = 0; i < 30; ++i) {
        PixelRect dboxBuildup;
        brush.beginStroke(doc, 0.5f, 0.5f, dboxBuildup);
        brush.endStroke();
    }

    uint8_t px[4];
    doc.getPixel(N/2, N/2, px);
    std::printf("  After 30 Buildup clicks at same point (op=50%%): R = %u (expect close to 255)\n", px[0]);
    CHECK(px[0] >= 200);   // 1 - 0.5^30 → essentially 255
}

static void testMirrorX()
{
    std::puts("\n--- Test: Mirror X paints both sides ---");
    constexpr int N = 256;
    TextureDocument doc(TextureKind::ShadowRate, N);
    doc.takeDirtyBounds();

    BrushEngine brush;
    BrushSettings s;
    s.radiusPx    = 8;
    s.hardness    = 1.0f;
    s.opacity     = 1.0f;
    s.spacingPx   = 1;
    s.channelMask = ChannelMaskR;
    s.inkR = 200;
    s.flowMode = FlowMode::Buildup;
    s.mirrorX = true;
    brush.setSettings(s);

    // Paint a click at (u=0.3, v=0.5). Mirrored point should be at (u=0.7, v=0.5).
    PixelRect dboxMirror;
    brush.beginStroke(doc, 0.3f, 0.5f, dboxMirror);
    brush.endStroke();

    uint8_t pxL[4], pxR[4];
    const int xL = static_cast<int>(0.3f * N);
    const int xR = static_cast<int>(0.7f * N);
    doc.getPixel(xL, N/2, pxL);
    doc.getPixel(xR, N/2, pxR);
    std::printf("  Original at x=%d: R=%u\n", xL, pxL[0]);
    std::printf("  Mirrored at x=%d: R=%u\n", xR, pxR[0]);
    CHECK(pxL[0] >= 150);
    CHECK(pxR[0] >= 150);
    // Should be roughly equal (within rounding)
    CHECK(std::abs(int(pxL[0]) - int(pxR[0])) <= 3);
}

int main()
{
    testBasicStrokeAndMask();
    testEraser();
    testWashCapping();
    testBuildupExceedsOpacity();
    testMirrorX();

    if (s_failures == 0) {
        std::puts("\n=== ALL BRUSH TESTS PASSED ===");
        return 0;
    }
    std::printf("\n=== %d FAILURES ===\n", s_failures);
    return 1;
}
