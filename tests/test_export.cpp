// Standalone smoke tests for editor::exportTextureToPng + exportAllToFolder.
//
// Covers:
//   1. Single-doc round-trip: write PNG → load with stb_image → pixels match
//   2. Batch export: produces N×4 files with the expected naming pattern
//   3. Filename sanitizer: a material name with spaces / Windows-illegal chars
//      yields a filesystem-safe filename
//
// Run via: run_export_test.bat (from project root)

#include "editor/TextureExport.h"
#include "editor/TextureDocument.h"
#include "editor/TextureGroup.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

using namespace editor;
namespace fs = std::filesystem;

static int s_failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #expr); \
        ++s_failures; \
    } \
} while (0)

/// Get a unique temp directory under %TEMP%, creating it if needed.
/// Cleaned up at the end of each test by remove_all.
static fs::path makeTempDir(const std::string& tag)
{
    fs::path base = fs::temp_directory_path() / ("mstc_test_export_" + tag);
    std::error_code ec;
    fs::remove_all(base, ec);   // clean stale state
    fs::create_directories(base, ec);
    return base;
}

static void testSingleRoundTrip()
{
    std::puts("\n--- Test: single PNG round-trip ---");
    constexpr int N = 32;
    TextureDocument doc(TextureKind::ShadowRate, N);

    // Write a deterministic gradient: R=x, G=y, B=(x+y)/2, A=255.
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const auto r = static_cast<uint8_t>(x * 8);          // 0..248
            const auto g = static_cast<uint8_t>(y * 8);
            const auto b = static_cast<uint8_t>((x + y) * 4);
            doc.setPixel(x, y, r, g, b, 255);
        }
    }

    fs::path tmp = makeTempDir("single");
    fs::path out = tmp / "round_trip.png";

    std::string err;
    const bool ok = exportTextureToPng(doc, out, err);
    if (!ok) std::printf("  exportTextureToPng err: %s\n", err.c_str());
    CHECK(ok);
    CHECK(fs::exists(out));
    CHECK(fs::file_size(out) > 0);

    // Load it back and compare pixel-perfect.
    int w = 0, h = 0, comp = 0;
    unsigned char* loaded = stbi_load(out.string().c_str(), &w, &h, &comp, 4);
    CHECK(loaded != nullptr);
    CHECK(w == N);
    CHECK(h == N);

    if (loaded) {
        bool match = true;
        for (int y = 0; y < N && match; ++y) {
            for (int x = 0; x < N && match; ++x) {
                uint8_t src[4];
                doc.getPixel(x, y, src);
                const unsigned char* p = loaded + (y * N + x) * 4;
                if (p[0] != src[0] || p[1] != src[1] || p[2] != src[2] || p[3] != src[3]) {
                    std::printf("  mismatch @ (%d,%d): wrote (%u,%u,%u,%u), loaded (%u,%u,%u,%u)\n",
                                x, y, src[0], src[1], src[2], src[3], p[0], p[1], p[2], p[3]);
                    match = false;
                }
            }
        }
        CHECK(match);
        stbi_image_free(loaded);
    }

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

/// Build a TextureGroupSet by hand (no PMX) for batch tests. Each group has
/// a name and 4 fully-allocated docs in a small size for fast IO.
static TextureGroupSet makeMockGroups(int N, std::initializer_list<const char*> names)
{
    TextureGroupSet set;
    int gi = 0;
    for (const char* name : names) {
        TextureGroup g{};
        g.name = name;
        g.diffuseName = "atlas";
        g.materialIndices.push_back(gi);
        for (size_t k = 0; k < g.stacks.size(); ++k) {
            g.stacks[k] = std::make_unique<LayerStack>(static_cast<TextureKind>(k), N);
            // Tag each composite with a known pixel so we can verify the
            // right one ended up in the right file. Writing through the
            // composite directly is safe in this test — we don't care about
            // layer fidelity, only that exportAll picks up doc(k).
            g.doc(k)->setPixel(0, 0,
                               static_cast<uint8_t>(gi * 30 + 1),
                               static_cast<uint8_t>(k * 50 + 1),
                               255, 255);
        }
        set.groups.push_back(std::move(g));
        ++gi;
    }
    return set;
}

static void testBatchExport()
{
    std::puts("\n--- Test: batch export to folder ---");
    constexpr int N = 16;
    auto set = makeMockGroups(N, {"head", "body", "hair"});

    fs::path tmp = makeTempDir("batch");

    BatchExportResult res = exportAllToFolder(set, tmp);
    std::printf("  %d/%d succeeded, %zu errors\n",
                res.successCount, res.totalCount, res.errors.size());

    CHECK(res.totalCount == 3 * static_cast<int>(TextureKind::Count));
    CHECK(res.successCount == res.totalCount);
    CHECK(res.errors.empty());

    // Spot-check a couple of files exist with the expected names.
    CHECK(fs::exists(tmp / "000_head_ShadowRate.png"));
    CHECK(fs::exists(tmp / "000_head_SubLightRate.png"));
    CHECK(fs::exists(tmp / "001_body_EdgeRate.png"));
    CHECK(fs::exists(tmp / "002_hair_FaceSDF.png"));

    // Round-trip one to make sure it's the right doc (group 1, kind 1 → R=31, G=51).
    int w, h, comp;
    unsigned char* px = stbi_load((tmp / "001_body_SubLightRate.png").string().c_str(),
                                   &w, &h, &comp, 4);
    CHECK(px != nullptr);
    if (px) {
        std::printf("  001_body_SubLightRate.png (0,0) = (%u,%u,%u,%u)\n",
                    px[0], px[1], px[2], px[3]);
        CHECK(px[0] == 1 * 30 + 1);   // groupIdx 1
        CHECK(px[1] == 1 * 50 + 1);   // kind 1 (SubLightRate)
        stbi_image_free(px);
    }

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

static void testFilenameSanitizer()
{
    std::puts("\n--- Test: filename sanitizer ---");
    constexpr int N = 8;
    // A name with spaces and Windows-illegal characters that would otherwise
    // crash CreateFile. The sanitizer should strip / collapse them.
    auto set = makeMockGroups(N, {"foo bar"});
    set.groups[0].name = "weird:name*with?stuff";

    fs::path tmp = makeTempDir("sanitize");
    BatchExportResult res = exportAllToFolder(set, tmp);

    CHECK(res.successCount == res.totalCount);

    // The exact mangling is implementation-defined, but no file should contain
    // any of the illegal chars, and 4 files should land in the dir.
    int found = 0;
    for (auto& entry : fs::directory_iterator(tmp)) {
        const std::string fn = entry.path().filename().string();
        std::printf("  produced: %s\n", fn.c_str());
        for (char c : fn) {
            CHECK(c != ':' && c != '*' && c != '?' && c != '<' && c != '>'
                  && c != '|' && c != '"' && c != '/' && c != '\\');
        }
        ++found;
    }
    CHECK(found == 4);   // one per kind

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

int main()
{
    testSingleRoundTrip();
    testBatchExport();
    testFilenameSanitizer();

    if (s_failures == 0) {
        std::puts("\n=== ALL EXPORT TESTS PASSED ===");
        return 0;
    }
    std::printf("\n=== %d FAILURES ===\n", s_failures);
    return 1;
}
