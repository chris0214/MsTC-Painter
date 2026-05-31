// Standalone tests for Layer + PaintLayer + LayerStack (Phase 8 step 2 / 3).
//
// Run via: run_layers_test.bat (from project root)

#include "editor/Layer.h"
#include "editor/LayerStack.h"
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

// ─────────────────────────────────────────────────────────────────
// Test 1: a fresh stack composites to the doc default (black opaque)
// ─────────────────────────────────────────────────────────────────
static void testFreshStack()
{
    std::puts("\n--- Test: fresh LayerStack default composite ---");
    LayerStack stack(TextureKind::ShadowRate, 64);
    CHECK(stack.layerCount() == 1);
    CHECK(stack.activeIndex() == 0);
    CHECK(stack.layer(0)->name == "Base");
    CHECK(stack.layer(0)->type() == LayerType::Paint);
    CHECK(stack.activeTarget() != nullptr);

    uint8_t px[4];
    stack.composite().getPixel(32, 32, px);
    CHECK(px[0] == 0 && px[1] == 0 && px[2] == 0 && px[3] == 255);
}

// ─────────────────────────────────────────────────────────────────
// Test 2: paint on bottom layer → recomposite → composite reflects it
// ─────────────────────────────────────────────────────────────────
static void testSingleLayerPaint()
{
    std::puts("\n--- Test: single PaintLayer write reflects in composite ---");
    LayerStack stack(TextureKind::ShadowRate, 32);
    auto* target = stack.activeTarget();
    CHECK(target != nullptr);

    // Write R=200 at (10, 10) with channelMask=R only.
    target->setPixel(10, 10, 200, 0, 0, 255, /*channelMask=R*/ 0x1);
    // Before recompositeRect, composite still shows default at (10, 10).
    uint8_t px[4];
    stack.composite().getPixel(10, 10, px);
    CHECK(px[0] == 0);

    PixelRect r; r.minX = 10; r.minY = 10; r.maxX = 11; r.maxY = 11;
    stack.recompositeRect(r);

    stack.composite().getPixel(10, 10, px);
    std::printf("  After composite at (10,10): (%u,%u,%u,%u)\n", px[0], px[1], px[2], px[3]);
    CHECK(px[0] == 200);
    CHECK(px[1] == 0);    // not authored → falls through to default 0
    CHECK(px[2] == 0);
    CHECK(px[3] == 255);  // not authored → default 255

    // Untouched pixel still default
    stack.composite().getPixel(0, 0, px);
    CHECK(px[0] == 0 && px[3] == 255);
}

// ─────────────────────────────────────────────────────────────────
// Test 3: per-channel replace, top-down — top wrote R, bottom wrote G
// ─────────────────────────────────────────────────────────────────
static void testPerChannelReplace()
{
    std::puts("\n--- Test: top-down per-channel replace ---");
    LayerStack stack(TextureKind::ShadowRate, 16);
    // Bottom Base layer writes G=128 at (5, 5)
    auto* base = stack.activeTarget();
    base->setPixel(5, 5, 0, 128, 0, 255, /*G*/ 0x2);

    // Add a new PaintLayer on top, write R=200 at the same pixel
    auto top = std::make_unique<PaintLayer>(16);
    top->name = "Top";
    const int topIdx = stack.addLayer(std::move(top));
    CHECK(topIdx == 1);
    CHECK(stack.activeIndex() == 1);   // newly added is active
    auto* topTarget = stack.activeTarget();
    topTarget->setPixel(5, 5, 200, 0, 0, 255, /*R*/ 0x1);

    // addLayer() already recomposited the full texture, but the latest
    // setPixel happened after — recomposite that pixel.
    PixelRect r; r.minX = 5; r.minY = 5; r.maxX = 6; r.maxY = 6;
    stack.recompositeRect(r);

    uint8_t px[4];
    stack.composite().getPixel(5, 5, px);
    std::printf("  Composite at (5,5): R=%u G=%u B=%u A=%u (expect R=200 G=128)\n",
                px[0], px[1], px[2], px[3]);
    CHECK(px[0] == 200);   // top wrote R
    CHECK(px[1] == 128);   // top didn't write G → bottom's G shows through
    CHECK(px[2] == 0);     // neither wrote B → default 0
    CHECK(px[3] == 255);   // default 255
}

// ─────────────────────────────────────────────────────────────────
// Test 4: visibility OFF → top layer disappears
// ─────────────────────────────────────────────────────────────────
static void testVisibility()
{
    std::puts("\n--- Test: hide top layer reverts to bottom ---");
    LayerStack stack(TextureKind::ShadowRate, 16);
    stack.activeTarget()->setPixel(2, 2, 50, 0, 0, 255, 0x1);

    auto top = std::make_unique<PaintLayer>(16);
    top->name = "Top";
    stack.addLayer(std::move(top));
    stack.activeTarget()->setPixel(2, 2, 200, 0, 0, 255, 0x1);

    stack.recompositeAll();
    uint8_t px[4];
    stack.composite().getPixel(2, 2, px);
    CHECK(px[0] == 200);   // top visible

    stack.layer(1)->visible = false;
    stack.recompositeAll();
    stack.composite().getPixel(2, 2, px);
    std::printf("  After hiding top: R=%u (expect 50, the bottom layer)\n", px[0]);
    CHECK(px[0] == 50);
}

// ─────────────────────────────────────────────────────────────────
// Test 5: opacity blends top into running composite
// ─────────────────────────────────────────────────────────────────
static void testOpacity()
{
    std::puts("\n--- Test: layer opacity halves contribution ---");
    LayerStack stack(TextureKind::ShadowRate, 8);
    // Bottom writes R=0 (== default, but explicitly authored)
    stack.activeTarget()->setPixel(1, 1, 0, 0, 0, 255, 0x1);

    auto top = std::make_unique<PaintLayer>(8);
    top->name    = "Top";
    top->opacity = 0.5f;
    stack.addLayer(std::move(top));
    stack.activeTarget()->setPixel(1, 1, 200, 0, 0, 255, 0x1);
    stack.recompositeAll();

    uint8_t px[4];
    stack.composite().getPixel(1, 1, px);
    std::printf("  Composite at (1,1) with top opacity=0.5: R=%u (expect ~100)\n", px[0]);
    // Top is on TOP so resolves first. Its starting "running" rgba is (0,0,0,255).
    // lerp(0, 200, 0.5) = 100.
    CHECK(px[0] >= 99 && px[0] <= 101);
}

// ─────────────────────────────────────────────────────────────────
// Test 6: remove layer leaves the stack usable
// ─────────────────────────────────────────────────────────────────
static void testRemoveLayer()
{
    std::puts("\n--- Test: remove layer keeps stack non-empty ---");
    LayerStack stack(TextureKind::ShadowRate, 8);
    stack.addLayer(std::make_unique<PaintLayer>(8));
    stack.addLayer(std::make_unique<PaintLayer>(8));
    CHECK(stack.layerCount() == 3);

    CHECK(stack.removeLayer(2));
    CHECK(stack.layerCount() == 2);

    CHECK(stack.removeLayer(0));
    CHECK(stack.layerCount() == 1);

    // Refuse to remove the last layer
    CHECK(!stack.removeLayer(0));
    CHECK(stack.layerCount() == 1);
}

// ─────────────────────────────────────────────────────────────────
// Test 7: moveLayer reorders top-down compositing
// ─────────────────────────────────────────────────────────────────
static void testMoveLayer()
{
    std::puts("\n--- Test: moveLayer reorders compositing ---");
    LayerStack stack(TextureKind::ShadowRate, 8);
    // Index 0 (bottom, "Base"): write R=50
    stack.activeTarget()->setPixel(3, 3, 50, 0, 0, 255, 0x1);

    // Index 1 (top): write R=200
    auto top = std::make_unique<PaintLayer>(8);
    top->name = "L1";
    stack.addLayer(std::move(top));
    stack.activeTarget()->setPixel(3, 3, 200, 0, 0, 255, 0x1);
    stack.recompositeAll();

    uint8_t px[4];
    stack.composite().getPixel(3, 3, px);
    CHECK(px[0] == 200);

    // Move L1 below Base. Now Base is on top.
    CHECK(stack.moveLayer(1, 0));
    stack.composite().getPixel(3, 3, px);
    std::printf("  After moving top below base: R=%u (expect 50)\n", px[0]);
    CHECK(px[0] == 50);
}

// ─────────────────────────────────────────────────────────────────
// Test 8: FillLayer covers everything with constant ink × channelMask
// ─────────────────────────────────────────────────────────────────
static void testFillLayer()
{
    std::puts("\n--- Test: FillLayer constant fill on selected channel ---");
    LayerStack stack(TextureKind::ShadowRate, 16);
    // Bottom Base writes G=50 at every pixel we sample
    auto* base = stack.activeTarget();
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            base->setPixel(x, y, 0, 50, 0, 255, 0x2);

    // Top FillLayer paints R=180 only on R-channel, no selection mask.
    auto fill = std::make_unique<FillLayer>(
        16, /*r=*/180, /*g=*/0, /*b=*/0, /*a=*/255, /*chMask=*/ 0x1);
    fill->name = "FillR";
    stack.addLayer(std::move(fill));   // recomposites all

    uint8_t px[4];
    stack.composite().getPixel(7, 7, px);
    std::printf("  Composite: R=%u G=%u B=%u (expect R=180 G=50)\n",
                px[0], px[1], px[2]);
    CHECK(px[0] == 180);   // FillLayer wrote R
    CHECK(px[1] == 50);    // Base's G shows through (FillLayer didn't author G)
    CHECK(px[2] == 0);     // neither wrote B
}

// ─────────────────────────────────────────────────────────────────
// Test 9: FillLayer with selection mask only paints inside the mask
// ─────────────────────────────────────────────────────────────────
static void testFillLayerSelection()
{
    std::puts("\n--- Test: FillLayer with selection mask ---");
    LayerStack stack(TextureKind::ShadowRate, 16);

    auto fill = std::make_unique<FillLayer>(
        16, 200, 0, 0, 255, 0x1);
    // Selection: only the top-left 8×8 quadrant
    std::vector<uint8_t> sel(16 * 16, 0);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            sel[y * 16 + x] = 1;
    fill->setSelection(std::move(sel));
    stack.addLayer(std::move(fill));

    uint8_t px[4];
    stack.composite().getPixel(3, 3, px);   // inside selection
    CHECK(px[0] == 200);
    stack.composite().getPixel(12, 12, px); // outside selection
    std::printf("  Outside selection R=%u (expect 0)\n", px[0]);
    CHECK(px[0] == 0);
}

// ─────────────────────────────────────────────────────────────────
// Test 10: ImageLayer same-size NN copy
// ─────────────────────────────────────────────────────────────────
static void testImageLayerSameSize()
{
    std::puts("\n--- Test: ImageLayer same-size copy ---");
    constexpr int N = 16;
    std::vector<uint8_t> src(N * N * 4, 0);
    // Diagonal: src[(y,y)] = (y, 100, 200, 255)
    for (int y = 0; y < N; ++y) {
        const size_t i = (y * N + y) * 4;
        src[i + 0] = static_cast<uint8_t>(y * 8);
        src[i + 1] = 100;
        src[i + 2] = 200;
        src[i + 3] = 255;
    }
    LayerStack stack(TextureKind::ShadowRate, N);
    auto img = std::make_unique<ImageLayer>(N, src.data(), N, /*chMask=*/0xF);
    img->name = "Img";
    stack.addLayer(std::move(img));

    uint8_t px[4];
    stack.composite().getPixel(5, 5, px);
    std::printf("  Composite (5,5): (%u,%u,%u,%u) (expect (40,100,200,255))\n",
                px[0], px[1], px[2], px[3]);
    CHECK(px[0] == 40);
    CHECK(px[1] == 100);
    CHECK(px[2] == 200);
}

// ─────────────────────────────────────────────────────────────────
// Test 11: ImageLayer NN resample (4x4 src → 8x8 target)
// ─────────────────────────────────────────────────────────────────
static void testImageLayerResample()
{
    std::puts("\n--- Test: ImageLayer NN resample ---");
    constexpr int srcN = 4;
    constexpr int dstN = 8;
    std::vector<uint8_t> src(srcN * srcN * 4, 0);
    // Quadrant top-left (src 0..1, 0..1) = R=200; rest = R=0
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            const size_t i = (y * srcN + x) * 4;
            src[i + 0] = 200;
            src[i + 3] = 255;
        }
    LayerStack stack(TextureKind::ShadowRate, dstN);
    auto img = std::make_unique<ImageLayer>(dstN, src.data(), srcN, /*chMask=*/0x1);
    stack.addLayer(std::move(img));

    uint8_t px[4];
    // dst (1,1) maps via NN to src (0,0) = R=200
    stack.composite().getPixel(1, 1, px);
    CHECK(px[0] == 200);
    // dst (6,6) maps to src (3,3) = R=0
    stack.composite().getPixel(6, 6, px);
    std::printf("  dst(6,6) R=%u (expect 0)\n", px[0]);
    CHECK(px[0] == 0);
}

// ─────────────────────────────────────────────────────────────────
// Test 12: ChannelLayer copies src R → dst G
// ─────────────────────────────────────────────────────────────────
static void testChannelLayer()
{
    std::puts("\n--- Test: ChannelLayer R→G copy ---");
    constexpr int N = 16;
    auto src = std::make_shared<std::vector<uint8_t>>(N * N * 4, 0);
    // src R gradient: R = x * 16
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const size_t i = (y * N + x) * 4;
            (*src)[i + 0] = static_cast<uint8_t>(x * 16);
            (*src)[i + 3] = 255;
        }

    LayerStack stack(TextureKind::ShadowRate, N);
    // Below: a PaintLayer with G=99 baseline so we can verify the channel
    // layer REPLACED G with src R.
    stack.activeTarget()->setPixel(8, 5, 0, 99, 0, 255, 0x2);

    auto ch = std::make_unique<ChannelLayer>(
        N, src, N, /*srcCh=*/0, /*dstCh=*/1);
    ch->name = "R->G";
    stack.addLayer(std::move(ch));

    uint8_t px[4];
    stack.composite().getPixel(8, 5, px);
    // Expected: G = src R at (8, 5) = 8 * 16 = 128 (top layer wins for G)
    std::printf("  Composite (8,5) G=%u (expect 128)\n", px[1]);
    CHECK(px[1] == 128);
    // R untouched by channel layer (default 0 at this pixel since paint layer
    // didn't author R there)
    CHECK(px[0] == 0);
}

// ─────────────────────────────────────────────────────────────────
// Test 13: mergeDown collapses two layers into one with combined wroteMask
// ─────────────────────────────────────────────────────────────────
static void testMergeDown()
{
    std::puts("\n--- Test: mergeDown collapses two layers ---");
    LayerStack stack(TextureKind::ShadowRate, 8);
    // Bottom Base writes G=99
    stack.activeTarget()->setPixel(2, 2, 0, 99, 0, 255, 0x2);
    // Top: PaintLayer writes R=200
    auto top = std::make_unique<PaintLayer>(8);
    top->name = "Top";
    stack.addLayer(std::move(top));
    stack.activeTarget()->setPixel(2, 2, 200, 0, 0, 255, 0x1);
    stack.recompositeAll();

    // Pre-merge: composite shows R=200, G=99
    uint8_t px[4];
    stack.composite().getPixel(2, 2, px);
    CHECK(px[0] == 200);
    CHECK(px[1] == 99);

    CHECK(stack.layerCount() == 2);
    CHECK(stack.mergeDown(1));   // merge top into base
    CHECK(stack.layerCount() == 1);

    // After merge: composite still shows the same pixel.
    stack.composite().getPixel(2, 2, px);
    std::printf("  After merge composite (2,2): R=%u G=%u (expect 200, 99)\n",
                px[0], px[1]);
    CHECK(px[0] == 200);
    CHECK(px[1] == 99);

    // Untouched pixel should remain default.
    stack.composite().getPixel(0, 0, px);
    CHECK(px[0] == 0 && px[1] == 0 && px[2] == 0 && px[3] == 255);
}

int main()
{
    testFreshStack();
    testSingleLayerPaint();
    testPerChannelReplace();
    testVisibility();
    testOpacity();
    testRemoveLayer();
    testMoveLayer();
    testFillLayer();
    testFillLayerSelection();
    testImageLayerSameSize();
    testImageLayerResample();
    testChannelLayer();
    testMergeDown();

    if (s_failures == 0) {
        std::puts("\n=== ALL LAYER TESTS PASSED ===");
        return 0;
    }
    std::printf("\n=== %d FAILURES ===\n", s_failures);
    return 1;
}
