// Phase 9 undo / redo tests.
//
// Run via: run_undo_test.bat (from project root)

#include "editor/BrushEngine.h"
#include "editor/FloodFill.h"
#include "editor/Layer.h"
#include "editor/LayerStack.h"
#include "editor/UndoHistory.h"

#include <cstdio>
#include <memory>

using namespace editor;

static int s_failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #expr); \
        ++s_failures; \
    } \
} while (0)

// ─────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────

/// Paint one stroke (begin → strokeTo → end) on the active layer of `stack`.
/// Returns true if a stroke was actually recorded.
static bool paintOneStroke(LayerStack& stack, BrushEngine& brush,
                           float u0, float v0, float u1, float v1)
{
    auto* paint = dynamic_cast<PaintLayer*>(stack.activeLayer());
    if (!paint) return false;

    PixelRect box;
    stack.history().beginStrokeRecord(*paint);
    brush.beginStroke(*paint, u0, v0, box);
    stack.history().recordStampBox(box, *paint);
    stack.recompositeRect(box);

    box = {};
    brush.strokeTo(*paint, u1, v1, box);
    stack.history().recordStampBox(box, *paint);
    stack.recompositeRect(box);

    brush.endStroke();
    stack.history().endStrokeRecord(*paint);
    return true;
}

static BrushSettings makeBrush(uint8_t inkR, uint8_t mask = ChannelMaskR)
{
    BrushSettings s;
    s.radiusPx    = 8;
    s.hardness    = 1.0f;
    s.opacity     = 1.0f;
    s.spacingPx   = 4;
    s.channelMask = mask;
    s.inkR        = inkR;
    s.flowMode    = FlowMode::Buildup;
    return s;
}

// ─────────────────────────────────────────────────────────────────
// Test 1: single stroke → undo → composite back to default
// ─────────────────────────────────────────────────────────────────
static void testSingleStrokeUndo()
{
    std::puts("\n--- Test: single stroke + undo ---");
    LayerStack stack(TextureKind::ShadowRate, 128);
    BrushEngine brush;
    brush.setSettings(makeBrush(200));

    paintOneStroke(stack, brush, 0.3f, 0.5f, 0.7f, 0.5f);

    // After paint: composite at center should be near 200.
    uint8_t px[4];
    stack.composite().getPixel(64, 64, px);
    std::printf("  post-stroke center R = %u\n", px[0]);
    CHECK(px[0] >= 150);

    CHECK(stack.history().canUndo());
    CHECK(!stack.history().canRedo());

    const PixelRect r = stack.history().undo();
    CHECK(!r.isEmpty());
    stack.recompositeRect(r);

    stack.composite().getPixel(64, 64, px);
    std::printf("  post-undo center R = %u\n", px[0]);
    CHECK(px[0] == 0);

    CHECK(!stack.history().canUndo());
    CHECK(stack.history().canRedo());
}

// ─────────────────────────────────────────────────────────────────
// Test 2: sequential strokes → undo undo → both reverted in order
// ─────────────────────────────────────────────────────────────────
static void testSequentialUndo()
{
    std::puts("\n--- Test: sequential stroke undo ---");
    LayerStack stack(TextureKind::ShadowRate, 64);
    BrushEngine brush;
    brush.setSettings(makeBrush(100));

    paintOneStroke(stack, brush, 0.2f, 0.5f, 0.4f, 0.5f);   // A: left
    brush.setSettings(makeBrush(200));
    paintOneStroke(stack, brush, 0.6f, 0.5f, 0.8f, 0.5f);   // B: right

    uint8_t px[4];
    stack.composite().getPixel(20, 32, px);   // in A
    CHECK(px[0] >= 80);
    stack.composite().getPixel(45, 32, px);   // in B
    CHECK(px[0] >= 150);

    // Undo B
    auto r = stack.history().undo();
    stack.recompositeRect(r);
    stack.composite().getPixel(45, 32, px);
    std::printf("  after undo B, B-region R = %u (expect 0)\n", px[0]);
    CHECK(px[0] == 0);
    stack.composite().getPixel(20, 32, px);
    CHECK(px[0] >= 80);   // A intact

    // Undo A
    r = stack.history().undo();
    stack.recompositeRect(r);
    stack.composite().getPixel(20, 32, px);
    std::printf("  after undo A, A-region R = %u (expect 0)\n", px[0]);
    CHECK(px[0] == 0);
}

// ─────────────────────────────────────────────────────────────────
// Test 3: undo + redo restores stroke
// ─────────────────────────────────────────────────────────────────
static void testUndoRedo()
{
    std::puts("\n--- Test: undo + redo round-trip ---");
    LayerStack stack(TextureKind::ShadowRate, 64);
    BrushEngine brush;
    brush.setSettings(makeBrush(180));

    paintOneStroke(stack, brush, 0.3f, 0.5f, 0.7f, 0.5f);

    uint8_t painted[4];
    stack.composite().getPixel(32, 32, painted);

    auto r = stack.history().undo();
    stack.recompositeRect(r);
    uint8_t undone[4];
    stack.composite().getPixel(32, 32, undone);
    CHECK(undone[0] == 0);

    r = stack.history().redo();
    CHECK(!r.isEmpty());
    stack.recompositeRect(r);
    uint8_t redone[4];
    stack.composite().getPixel(32, 32, redone);
    std::printf("  painted R=%u, undone R=%u, redone R=%u\n",
                painted[0], undone[0], redone[0]);
    CHECK(redone[0] == painted[0]);

    CHECK(stack.history().canUndo());
    CHECK(!stack.history().canRedo());
}

// ─────────────────────────────────────────────────────────────────
// Test 4: new action after undo truncates redo
// ─────────────────────────────────────────────────────────────────
static void testRedoTruncatedByNewAction()
{
    std::puts("\n--- Test: new action drops redo ---");
    LayerStack stack(TextureKind::ShadowRate, 64);
    BrushEngine brush;
    brush.setSettings(makeBrush(100));

    paintOneStroke(stack, brush, 0.2f, 0.5f, 0.4f, 0.5f);   // A
    auto r = stack.history().undo();
    stack.recompositeRect(r);
    CHECK(stack.history().canRedo());

    // New stroke B → redo should be gone
    paintOneStroke(stack, brush, 0.6f, 0.5f, 0.8f, 0.5f);
    CHECK(!stack.history().canRedo());
    CHECK(stack.history().canUndo());
}

// ─────────────────────────────────────────────────────────────────
// Test 5: flood fill undo
// ─────────────────────────────────────────────────────────────────
static void testFloodFillUndo()
{
    std::puts("\n--- Test: flood fill undo ---");
    LayerStack stack(TextureKind::ShadowRate, 32);
    auto* paint = dynamic_cast<PaintLayer*>(stack.activeLayer());
    CHECK(paint != nullptr);

    // Pre-snapshot for the fill record.
    PaintLayer before(*paint);

    FillSettings fs;
    fs.channelMask = ChannelMaskR;
    fs.inkR        = 222;
    fs.opacity     = 1.0f;
    fs.tolerance   = 255;
    fs.scope       = FillScope::ColorConnected;

    PixelRect box;
    floodFill(stack.composite(), *paint, 16, 16, fs, nullptr, box);
    stack.recompositeRect(box);
    stack.history().recordFloodFill(before, *paint, box);

    uint8_t px[4];
    stack.composite().getPixel(8, 8, px);
    CHECK(px[0] == 222);

    CHECK(stack.history().canUndo());
    auto r = stack.history().undo();
    CHECK(!r.isEmpty());
    stack.recompositeRect(r);
    stack.composite().getPixel(8, 8, px);
    std::printf("  after fill undo: R=%u (expect 0)\n", px[0]);
    CHECK(px[0] == 0);
}

// ─────────────────────────────────────────────────────────────────
// Test 6: empty stack undo is a no-op
// ─────────────────────────────────────────────────────────────────
static void testEmptyUndoNoOp()
{
    std::puts("\n--- Test: empty stack undo ---");
    LayerStack stack(TextureKind::ShadowRate, 32);
    CHECK(!stack.history().canUndo());
    CHECK(!stack.history().canRedo());
    auto r = stack.history().undo();
    CHECK(r.isEmpty());
    auto r2 = stack.history().redo();
    CHECK(r2.isEmpty());
}

// ─────────────────────────────────────────────────────────────────
// Test 7: 201 strokes — first one is dropped (200 cap)
// ─────────────────────────────────────────────────────────────────
static void testStepCap()
{
    std::puts("\n--- Test: 200-step cap eviction ---");
    LayerStack stack(TextureKind::ShadowRate, 32);
    BrushEngine brush;
    brush.setSettings(makeBrush(100));

    for (int i = 0; i < 205; ++i) {
        paintOneStroke(stack, brush, 0.3f, 0.5f, 0.5f, 0.5f);
    }
    // History should be capped at 200 entries.
    CHECK(stack.history().entryCount() == 200);

    // Undo all 200 — should return non-empty rect each time, then go empty.
    int undoneCount = 0;
    while (stack.history().canUndo()) {
        auto r = stack.history().undo();
        if (!r.isEmpty()) {
            stack.recompositeRect(r);
            ++undoneCount;
        } else {
            break;
        }
    }
    std::printf("  undone %d strokes (expect 200)\n", undoneCount);
    CHECK(undoneCount == 200);
}

// ─────────────────────────────────────────────────────────────────
// Test 8: cross-stack isolation
// ─────────────────────────────────────────────────────────────────
static void testCrossStackIsolation()
{
    std::puts("\n--- Test: cross-stack isolation ---");
    LayerStack a(TextureKind::ShadowRate, 32);
    LayerStack b(TextureKind::SubLightRate, 32);
    BrushEngine brush;
    brush.setSettings(makeBrush(100));

    paintOneStroke(a, brush, 0.3f, 0.5f, 0.7f, 0.5f);   // only A

    CHECK(a.history().canUndo());
    CHECK(!b.history().canUndo());   // B untouched

    auto rB = b.history().undo();
    CHECK(rB.isEmpty());             // no-op on B
    auto rA = a.history().undo();
    CHECK(!rA.isEmpty());             // works on A
}

// ─────────────────────────────────────────────────────────────────
// Test 9: layer deletion prunes orphan undo entries
// ─────────────────────────────────────────────────────────────────
static void testLayerDeletePrunes()
{
    std::puts("\n--- Test: deleting a layer prunes its undo entries ---");
    LayerStack stack(TextureKind::ShadowRate, 32);
    BrushEngine brush;
    brush.setSettings(makeBrush(100));

    // Add a second layer L1 on top, paint on it.
    auto top = std::make_unique<PaintLayer>(32);
    top->name = "L1";
    const int li = stack.addLayer(std::move(top));
    CHECK(li == 1);

    paintOneStroke(stack, brush, 0.3f, 0.5f, 0.7f, 0.5f);   // entry targets L1
    CHECK(stack.history().entryCount() == 1);

    // Delete L1 → pruneByLayerId should drop the entry.
    CHECK(stack.removeLayer(1));
    CHECK(stack.history().entryCount() == 0);
    CHECK(!stack.history().canUndo());

    // Undo on empty history is no-op (does not crash).
    auto r = stack.history().undo();
    CHECK(r.isEmpty());
}

int main()
{
    testSingleStrokeUndo();
    testSequentialUndo();
    testUndoRedo();
    testRedoTruncatedByNewAction();
    testFloodFillUndo();
    testEmptyUndoNoOp();
    testStepCap();
    testCrossStackIsolation();
    testLayerDeletePrunes();

    if (s_failures == 0) {
        std::puts("\n=== ALL UNDO TESTS PASSED ===");
        return 0;
    }
    std::printf("\n=== %d FAILURES ===\n", s_failures);
    return 1;
}
