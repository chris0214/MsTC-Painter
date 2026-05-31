// Standalone smoke tests for FloodFill across all 3 scopes.
//
// Run via: run_floodfill_test.bat (from project root)

#include "editor/BrushEngine.h"   // for ChannelMask* constants
#include "editor/FloodFill.h"
#include "editor/TextureDocument.h"

#include <cstdio>
#include <vector>

using namespace editor;

static int s_failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #expr); \
        ++s_failures; \
    } \
} while (0)

/// Helper: paint a solid rectangle of the given R value into a doc.
static void fillRect(TextureDocument& doc, int x0, int y0, int x1, int y1,
                     uint8_t r, uint8_t g = 0, uint8_t b = 0)
{
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            doc.setPixel(x, y, r, g, b, 255);
    doc.takeDirtyBounds();
}

// ─────────────────────────────────────────────────────────────────
// Test 1: ColorConnected — bucket spreads only to similar pixels
// ─────────────────────────────────────────────────────────────────

static void testColorConnected()
{
    std::puts("\n--- Test: ColorConnected ---");
    constexpr int N = 64;
    TextureDocument doc(TextureKind::ShadowRate, N);
    doc.takeDirtyBounds();

    // Paint a 20x20 R=200 region in the center (rest is R=0).
    fillRect(doc, 22, 22, 42, 42, 200);

    FillSettings fs;
    fs.channelMask = ChannelMaskR;
    fs.inkR = 255;
    fs.opacity = 1.0f;
    fs.tolerance = 30;
    fs.scope = FillScope::ColorConnected;

    // Click in the center of the R=200 region. Should fill it to R=255 only.
    PixelRect dbox;
    const int filled = floodFill(doc, doc, 32, 32, fs, /*uvMask*/ nullptr, dbox);
    std::printf("  Filled %d texels (expected ~400)\n", filled);
    CHECK(filled >= 350 && filled <= 450);

    uint8_t px[4];

    // Inside the original region — should be 255
    doc.getPixel(32, 32, px);
    CHECK(px[0] == 255);

    // Outside — should remain 0
    doc.getPixel(5, 5, px);
    CHECK(px[0] == 0);
    doc.getPixel(50, 50, px);
    CHECK(px[0] == 0);
}

// ─────────────────────────────────────────────────────────────────
// Test 2: UvIsland — bucket fills mask=1 region, ignores color
// ─────────────────────────────────────────────────────────────────

static void testUvIsland()
{
    std::puts("\n--- Test: UvIsland ---");
    constexpr int N = 32;
    TextureDocument doc(TextureKind::ShadowRate, N);
    doc.takeDirtyBounds();

    // Mask: only a 10x10 square at (5..15, 5..15) is in the UV island.
    std::vector<uint8_t> mask(N * N, 0);
    for (int y = 5; y < 15; ++y)
        for (int x = 5; x < 15; ++x)
            mask[y * N + x] = 1;

    FillSettings fs;
    fs.channelMask = ChannelMaskR;
    fs.inkR = 200;
    fs.opacity = 1.0f;
    fs.scope = FillScope::UvIsland;

    PixelRect dbox;
    const int filled = floodFill(doc, doc, 10, 10, fs, mask.data(), dbox);
    std::printf("  Filled %d texels (expected 100)\n", filled);
    CHECK(filled == 100);

    uint8_t px[4];
    doc.getPixel(10, 10, px);
    CHECK(px[0] == 200);

    // Just outside the mask
    doc.getPixel(4, 4, px);
    CHECK(px[0] == 0);
    doc.getPixel(15, 15, px);
    CHECK(px[0] == 0);  // mask doesn't include these (mask is exclusive at 15)
}

// ─────────────────────────────────────────────────────────────────
// Test 3: ColorAndUv — both constraints enforced
// ─────────────────────────────────────────────────────────────────

static void testColorAndUv()
{
    std::puts("\n--- Test: ColorAndUv ---");
    constexpr int N = 32;
    TextureDocument doc(TextureKind::ShadowRate, N);
    doc.takeDirtyBounds();

    // Mask covers (5..15, 5..15)
    std::vector<uint8_t> mask(N * N, 0);
    for (int y = 5; y < 15; ++y)
        for (int x = 5; x < 15; ++x)
            mask[y * N + x] = 1;

    // Pre-fill the LEFT half of the UV island with R=100, leaving the right half R=0.
    fillRect(doc, 5, 5, 10, 15, 100);

    FillSettings fs;
    fs.channelMask = ChannelMaskR;
    fs.inkR = 255;
    fs.opacity = 1.0f;
    fs.tolerance = 10;          // small — won't reach R=0 from R=100 seed
    fs.scope = FillScope::ColorAndUv;

    // Click in the left half (R=100). Should fill ONLY the left half — color
    // tolerance prevents bleeding into R=0, and UV mask prevents leaking out.
    PixelRect dbox;
    const int filled = floodFill(doc, doc, 7, 10, fs, mask.data(), dbox);
    std::printf("  Filled %d texels (expected 50, left half of mask)\n", filled);
    CHECK(filled == 50);

    uint8_t px[4];
    doc.getPixel(7, 10, px);    // in filled region
    CHECK(px[0] == 255);
    doc.getPixel(12, 10, px);   // right half (R=0) — should NOT be filled
    CHECK(px[0] == 0);
    doc.getPixel(20, 10, px);   // outside mask — should NOT be filled
    CHECK(px[0] == 0);
}

// ─────────────────────────────────────────────────────────────────
// Test 4: Channel mask — bucket only writes to enabled channel
// ─────────────────────────────────────────────────────────────────

static void testChannelMask()
{
    std::puts("\n--- Test: channel mask ---");
    constexpr int N = 16;
    TextureDocument doc(TextureKind::ShadowRate, N);
    doc.takeDirtyBounds();

    FillSettings fs;
    fs.channelMask = ChannelMaskG;   // only green
    fs.inkR = 255;                   // these should be ignored
    fs.inkG = 200;
    fs.inkB = 255;
    fs.opacity = 1.0f;
    fs.tolerance = 255;              // accept all
    fs.scope = FillScope::ColorConnected;

    PixelRect dbox;
    floodFill(doc, doc, 0, 0, fs, nullptr, dbox);

    uint8_t px[4];
    doc.getPixel(8, 8, px);
    std::printf("  Center pixel: (%u,%u,%u,%u)\n", px[0], px[1], px[2], px[3]);
    CHECK(px[0] == 0);    // R untouched
    CHECK(px[1] == 200);  // G filled
    CHECK(px[2] == 0);    // B untouched
}

int main()
{
    testColorConnected();
    testUvIsland();
    testColorAndUv();
    testChannelMask();

    if (s_failures == 0) {
        std::puts("\n=== ALL FLOODFILL TESTS PASSED ===");
        return 0;
    }
    std::printf("\n=== %d FAILURES ===\n", s_failures);
    return 1;
}
