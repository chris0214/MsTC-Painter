// Standalone smoke tests for project::saveProject / loadProjectMasks.
//
// Covers:
//   1. Round-trip: build a mock GroupSet → save → load to a fresh GroupSet at
//      the same size → meta + pixels match exactly.
//   2. Untouched docs are not written: a group whose docs were never modified
//      should produce zero .bin files (and load should restore them as default).
//   3. Header-only read: readProjectHeader pulls modelPath/textureSize/UI/camera
//      without requiring the .bin sidecar files to exist.
//   4. Mixed dirty: only some kinds in some groups are modified. Save must skip
//      the rest; load must restore only what was saved and leave others default.
//
// Run via: run_project_test.bat (from project root)

#include "project/Project.h"
#include "editor/TextureDocument.h"
#include "editor/TextureGroup.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

using namespace editor;
namespace fs = std::filesystem;
using project::ProjectMeta;

static int s_failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #expr); \
        ++s_failures; \
    } \
} while (0)

static fs::path makeTempDir(const std::string& tag)
{
    fs::path base = fs::temp_directory_path() / ("mstc_test_project_" + tag);
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

/// Build a TextureGroupSet by hand. Each group gets `size` × 4 docs.
/// The optional `paint` callback can stamp deterministic pixels.
static TextureGroupSet makeMockGroups(int size,
                                      std::initializer_list<const char*> names)
{
    TextureGroupSet set;
    int gi = 0;
    for (const char* name : names) {
        TextureGroup g{};
        g.name = name;
        g.diffuseName = "atlas";
        g.materialIndices.push_back(gi);
        for (size_t k = 0; k < g.stacks.size(); ++k) {
            g.stacks[k] = std::make_unique<LayerStack>(
                static_cast<TextureKind>(k), size);
            // Construction's initial clear should NOT count as user edit.
            CHECK(g.doc(k)->everModified() == false);
        }
        set.groups.push_back(std::move(g));
        ++gi;
    }
    return set;
}

/// Stamp a recognizable pattern into a single doc so a load round-trip can
/// verify pixel equality.
/// Paint a deterministic pattern through the LayerStack's active PaintLayer,
/// then recomposite so the doc reflects what we wrote. This is what
/// production code does (paintPattern direct to the doc would bypass the
/// layer system and the next recompositeAll would erase the writes).
static void paintPattern(LayerStack& stack, int seed)
{
    auto* target = stack.activeTarget();
    if (!target) return;
    const int N = stack.size();
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            target->setPixel(x, y,
                static_cast<uint8_t>((x + seed)  & 0xFF),
                static_cast<uint8_t>((y + seed)  & 0xFF),
                static_cast<uint8_t>((x ^ y)     & 0xFF),
                255,
                /*channelsAuthored=*/ 0xF);
        }
    }
    stack.recompositeAll();
}

static bool pixelsEqual(const TextureDocument& a, const TextureDocument& b)
{
    if (a.size() != b.size()) return false;
    const int N = a.size();
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            uint8_t ra[4], rb[4];
            a.getPixel(x, y, ra);
            b.getPixel(x, y, rb);
            for (int c = 0; c < 4; ++c) if (ra[c] != rb[c]) return false;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────
static void testFullRoundTrip()
{
    std::puts("\n--- Test: full round-trip (meta + pixels) ---");
    constexpr int N = 32;
    auto src = makeMockGroups(N, {"head", "body"});

    // Touch every kind of every group with a unique pattern so cross-wiring
    // (e.g. group 0's ShadowRate landing in group 1's slot) shows up.
    int seed = 0;
    for (auto& g : src.groups) {
        for (size_t k = 0; k < g.stacks.size(); ++k) {
            paintPattern(*g.stacks[k], ++seed);
            CHECK(g.doc(k)->everModified());
        }
    }

    fs::path tmp     = makeTempDir("roundtrip");
    fs::path projPath = tmp / "test.mstcproj";

    ProjectMeta meta;
    meta.modelPath        = "models/dummy.pmx";
    meta.textureSize      = N;
    meta.activeGroupIndex = 1;
    meta.activeKind       = TextureKind::SubLightRate;
    meta.channelView      = 2;
    meta.viewMode         = 1;
    meta.autoSwitchGroup  = false;
    meta.cameraTarget     = { 1.0f, 2.0f, 3.0f };
    meta.cameraYaw        = 0.5f;
    meta.cameraPitch      = -0.3f;
    meta.cameraDistance   = 25.5f;

    std::string err;
    CHECK(project::saveProject(projPath, meta, src, err));
    if (!err.empty()) std::printf("  save err: %s\n", err.c_str());

    CHECK(fs::exists(projPath));
    CHECK(fs::exists(tmp / "test.mstcproj.dir" / "layers"));

    // Read header back.
    ProjectMeta loadedMeta;
    err.clear();
    CHECK(project::readProjectHeader(projPath, loadedMeta, err));
    if (!err.empty()) std::printf("  readHeader err: %s\n", err.c_str());

    CHECK(loadedMeta.textureSize      == N);
    CHECK(loadedMeta.activeGroupIndex == 1);
    CHECK(loadedMeta.activeKind       == TextureKind::SubLightRate);
    CHECK(loadedMeta.channelView      == 2);
    CHECK(loadedMeta.viewMode         == 1);
    CHECK(loadedMeta.autoSwitchGroup  == false);
    CHECK(loadedMeta.cameraYaw        == 0.5f);
    CHECK(loadedMeta.cameraPitch      == -0.3f);
    CHECK(loadedMeta.cameraDistance   == 25.5f);
    CHECK(loadedMeta.cameraTarget.x   == 1.0f);
    CHECK(loadedMeta.cameraTarget.y   == 2.0f);
    CHECK(loadedMeta.cameraTarget.z   == 3.0f);

    // Build a fresh GroupSet at the same size, load masks into it.
    auto dst = makeMockGroups(N, {"head", "body"});
    err.clear();
    CHECK(project::loadProjectMasks(projPath, dst, loadedMeta, err));
    if (!err.empty()) std::printf("  loadMasks err: %s\n", err.c_str());

    CHECK(dst.groups.size() == src.groups.size());
    for (size_t gi = 0; gi < src.groups.size(); ++gi) {
        for (size_t k = 0; k < src.groups[gi].stacks.size(); ++k) {
            const bool eq = pixelsEqual(*src.groups[gi].doc(k),
                                        *dst.groups[gi].doc(k));
            if (!eq) {
                std::printf("  pixel mismatch group=%zu kind=%zu\n", gi, k);
            }
            CHECK(eq);
        }
    }

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// ─────────────────────────────────────────────────────────────────
static void testUntouchedSkipped()
{
    std::puts("\n--- Test: untouched docs not written ---");
    constexpr int N = 16;
    auto src = makeMockGroups(N, {"alpha", "beta"});
    // No painting — every doc should be everModified() == false.

    fs::path tmp = makeTempDir("untouched");
    fs::path projPath = tmp / "blank.mstcproj";

    ProjectMeta meta;
    meta.modelPath   = "x.pmx";
    meta.textureSize = N;

    std::string err;
    CHECK(project::saveProject(projPath, meta, src, err));

    fs::path layersDir = tmp / "blank.mstcproj.dir" / "layers";
    int count = 0;
    if (fs::exists(layersDir)) {
        for (auto& e : fs::directory_iterator(layersDir)) {
            (void)e;
            ++count;
        }
    }
    std::printf("  bin files written: %d\n", count);
    CHECK(count == 0);

    // .mstcproj should still exist and be small (just metadata).
    CHECK(fs::exists(projPath));
    const auto sz = fs::file_size(projPath);
    std::printf("  .mstcproj size: %llu bytes\n",
                static_cast<unsigned long long>(sz));
    CHECK(sz < 8000);   // metadata-only, well under 8 KB

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// ─────────────────────────────────────────────────────────────────
static void testMixedDirty()
{
    std::puts("\n--- Test: only modified kinds restored, rest stay default ---");
    constexpr int N = 16;
    auto src = makeMockGroups(N, {"g0", "g1"});

    // Paint group 0 ShadowRate AND group 1 EdgeRate. Leave the other 6 docs
    // pristine. Save → load → painted pair should round-trip; the 6 others
    // should be all-zero (default state from construction).
    paintPattern(*src.groups[0].stacks[static_cast<size_t>(TextureKind::ShadowRate)], 42);
    paintPattern(*src.groups[1].stacks[static_cast<size_t>(TextureKind::EdgeRate)],   99);

    fs::path tmp = makeTempDir("mixed");
    fs::path projPath = tmp / "mixed.mstcproj";

    ProjectMeta meta;
    meta.modelPath   = "y.pmx";
    meta.textureSize = N;

    std::string err;
    CHECK(project::saveProject(projPath, meta, src, err));

    // Should have exactly 2 .bin files written (one paint layer per touched kind).
    fs::path layersDir = tmp / "mixed.mstcproj.dir" / "layers";
    int binCount = 0;
    for (auto& e : fs::directory_iterator(layersDir)) {
        (void)e;
        ++binCount;
    }
    CHECK(binCount == 2);

    // Fresh dst, load.
    auto dst = makeMockGroups(N, {"g0", "g1"});
    ProjectMeta loadedMeta;
    err.clear();
    CHECK(project::readProjectHeader(projPath, loadedMeta, err));
    CHECK(project::loadProjectMasks(projPath, dst, loadedMeta, err));

    // The two painted docs should round-trip pixel-perfectly.
    CHECK(pixelsEqual(*src.groups[0].doc(TextureKind::ShadowRate),
                       *dst.groups[0].doc(TextureKind::ShadowRate)));
    CHECK(pixelsEqual(*src.groups[1].doc(TextureKind::EdgeRate),
                       *dst.groups[1].doc(TextureKind::EdgeRate)));

    // The other 6 should be all-default (R=G=B=0, A=255) — i.e. they should
    // match a freshly-constructed doc.
    auto refSet = makeMockGroups(N, {"g0", "g1"});
    for (size_t gi = 0; gi < 2; ++gi) {
        for (size_t k = 0; k < dst.groups[gi].stacks.size(); ++k) {
            const bool isPainted =
                (gi == 0 && k == static_cast<size_t>(TextureKind::ShadowRate)) ||
                (gi == 1 && k == static_cast<size_t>(TextureKind::EdgeRate));
            if (isPainted) continue;
            const bool eqDefault = pixelsEqual(*dst.groups[gi].doc(k),
                                                *refSet.groups[gi].doc(k));
            if (!eqDefault) {
                std::printf("  unexpected non-default at group=%zu kind=%zu\n", gi, k);
            }
            CHECK(eqDefault);
        }
    }

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// ─────────────────────────────────────────────────────────────────
static void testHeaderRejectsBadVersion()
{
    std::puts("\n--- Test: header rejects bogus version field ---");
    fs::path tmp = makeTempDir("badversion");
    fs::path projPath = tmp / "bad.mstcproj";
    {
        // Hand-craft a JSON with version = 999.
        FILE* fp = nullptr;
        _wfopen_s(&fp, projPath.c_str(), L"wb");
        CHECK(fp != nullptr);
        if (fp) {
            const char* json = R"({"version":999,"modelPath":"x.pmx","textureSize":256})";
            std::fwrite(json, 1, std::strlen(json), fp);
            std::fclose(fp);
        }
    }
    ProjectMeta m;
    std::string err;
    const bool ok = project::readProjectHeader(projPath, m, err);
    CHECK(ok == false);
    CHECK(err.find("version") != std::string::npos);
    std::printf("  expected error: %s\n", err.c_str());

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// ─────────────────────────────────────────────────────────────────
// Test: v1 .mstcproj loads into the v2 layer system (single Base PaintLayer
// per touched kind, pixels intact).
// ─────────────────────────────────────────────────────────────────
static void testV1Migration()
{
    std::puts("\n--- Test: v1 → v2 migration ---");
    constexpr int N = 32;
    fs::path tmp = makeTempDir("v1mig");
    fs::path projPath = tmp / "v1.mstcproj";
    fs::path companion = tmp / "v1.mstcproj.dir";
    fs::create_directories(companion / "masks");

    // ── 1. Hand-craft a v1 .mstcproj JSON ────────────────────────
    {
        FILE* fp = nullptr;
        _wfopen_s(&fp, projPath.c_str(), L"wb");
        CHECK(fp != nullptr);
        const std::string json =
            "{\n"
            "  \"version\":1,\n"
            "  \"modelPath\":\"y.pmx\",\n"
            "  \"textureSize\":" + std::to_string(N) + ",\n"
            "  \"groups\":[\n"
            "    {\"materialIndex\":0,\"name\":\"g0\",\"masks\":{\"ShadowRate\":\"masks/000_g0_ShadowRate.bin\"}}\n"
            "  ]\n"
            "}";
        std::fwrite(json.data(), 1, json.size(), fp);
        std::fclose(fp);
    }

    // ── 2. Hand-craft the v1 .bin payload (MMSK header + raw RGBA) ──
    {
        FILE* fp = nullptr;
        _wfopen_s(&fp, (companion / "masks" / "000_g0_ShadowRate.bin").c_str(), L"wb");
        CHECK(fp != nullptr);
        const uint32_t hdr[3] = { 0x4B534D4D, 1u, static_cast<uint32_t>(N) };  // 'MMSK', v1
        std::fwrite(hdr, sizeof(hdr), 1, fp);
        // Write a deterministic pattern (same algorithm as paintPattern with seed=7).
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                const uint8_t px[4] = {
                    static_cast<uint8_t>((x + 7) & 0xFF),
                    static_cast<uint8_t>((y + 7) & 0xFF),
                    static_cast<uint8_t>((x ^ y) & 0xFF),
                    255 };
                std::fwrite(px, 1, 4, fp);
            }
        }
        std::fclose(fp);
    }

    // ── 3. Read header (should accept v1) ────────────────────────
    ProjectMeta meta;
    std::string err;
    CHECK(project::readProjectHeader(projPath, meta, err));
    CHECK(meta.textureSize == N);

    // ── 4. Build a mock GroupSet and load v1 mask into it ────────
    auto dst = makeMockGroups(N, {"g0"});
    CHECK(project::loadProjectMasks(projPath, dst, meta, err));

    // The stack for g0/ShadowRate should still have exactly one layer (Base)
    // since v1 migration writes into the auto-created Base PaintLayer.
    auto& stack = *dst.groups[0].stacks[static_cast<size_t>(TextureKind::ShadowRate)];
    CHECK(stack.layerCount() == 1);

    // Composite should match the pattern.
    uint8_t px[4];
    stack.composite().getPixel(5, 7, px);
    std::printf("  composite (5,7) = (%u,%u,%u,%u) (expect (12,14,2,255))\n",
                px[0], px[1], px[2], px[3]);
    CHECK(px[0] == 12);   // (5+7)
    CHECK(px[1] == 14);   // (7+7)
    CHECK(px[2] == 2);    // (5^7)

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// ─────────────────────────────────────────────────────────────────
int main()
{
    testFullRoundTrip();
    testUntouchedSkipped();
    testMixedDirty();
    testHeaderRejectsBadVersion();
    testV1Migration();

    if (s_failures == 0) {
        std::puts("\n=== ALL PROJECT TESTS PASSED ===");
        return 0;
    }
    std::printf("\n=== %d FAILURES ===\n", s_failures);
    return 1;
}
