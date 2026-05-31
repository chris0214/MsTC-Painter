#include "Project.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <Windows.h>

#include <cstdio>
#include <fstream>
#include <system_error>

using nlohmann::json;
using editor::TextureKind;
using editor::TextureDocument;
using editor::TextureGroup;
using editor::TextureGroupSet;
using editor::Layer;
using editor::PaintLayer;
using editor::FillLayer;
using editor::ImageLayer;
using editor::ChannelLayer;
using editor::LayerStack;
using editor::LayerType;
using editor::BlendMode;

namespace project {

// ─────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────

namespace {

/// Filename of the companion directory for a .mstcproj path.
/// "foo.mstcproj" → "foo.mstcproj.dir"
std::filesystem::path companionDir(const std::filesystem::path& projectPath)
{
    std::filesystem::path d = projectPath;
    d += ".dir";
    return d;
}

/// "ShadowRate" / "SubLightRate" / "EdgeRate" / "FaceSDF" — stable across
/// versions; safe to compare strings to.
const char* kindKey(TextureKind k)
{
    switch (k) {
    case TextureKind::ShadowRate:   return "ShadowRate";
    case TextureKind::SubLightRate: return "SubLightRate";
    case TextureKind::EdgeRate:     return "EdgeRate";
    case TextureKind::FaceSDF:      return "FaceSDF";
    default:                        return "Unknown";
    }
}

bool kindFromKey(const std::string& s, TextureKind& out)
{
    if (s == "ShadowRate")   { out = TextureKind::ShadowRate;   return true; }
    if (s == "SubLightRate") { out = TextureKind::SubLightRate; return true; }
    if (s == "EdgeRate")     { out = TextureKind::EdgeRate;     return true; }
    if (s == "FaceSDF")      { out = TextureKind::FaceSDF;      return true; }
    return false;
}

/// Filesystem-safe filename component. Replaces illegal Windows characters
/// with underscores. Mirrors TextureExport.cpp's sanitizer (kept separate so
/// the modules don't depend on each other).
std::string sanitize(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    bool prevUnderscore = false;
    for (char c : in) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*'
            || c == '?' || c == '"' || c == '<' || c == '>' || c == '|'
            || c == ' ' || c == '\t') {
            if (!prevUnderscore && !out.empty()) {
                out.push_back('_');
                prevUnderscore = true;
            }
        } else {
            out.push_back(c);
            prevUnderscore = false;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = "untitled";
    return out;
}

/// Mask filename for a (group, kind). Path is relative to companionDir.
/// e.g. "masks/000_前髮_ShadowRate.bin"
std::string maskRelPath(size_t groupIdx, const std::string& matName, TextureKind k)
{
    char prefix[8];
    std::snprintf(prefix, sizeof(prefix), "%03zu", groupIdx);
    return std::string("masks/") + prefix + "_"
         + sanitize(matName) + "_" + kindKey(k) + ".bin";
}

/// v2 per-layer sidecar path.
/// e.g. "layers/000_前髮_ShadowRate_03.bin"
std::string layerRelPath(size_t groupIdx, const std::string& matName,
                         TextureKind k, int layerIdx)
{
    char prefix[8];
    std::snprintf(prefix, sizeof(prefix), "%03zu", groupIdx);
    char layerSuffix[8];
    std::snprintf(layerSuffix, sizeof(layerSuffix), "%02d", layerIdx);
    return std::string("layers/") + prefix + "_"
         + sanitize(matName) + "_" + kindKey(k) + "_" + layerSuffix + ".bin";
}

/// Open a binary file for read/write with a wide path (Unicode-safe).
FILE* wfopen(const std::filesystem::path& p, const wchar_t* mode)
{
    FILE* fp = nullptr;
    _wfopen_s(&fp, p.c_str(), mode);
    return fp;
}

/// Small UTF-8 ↔ filesystem::path helpers. nlohmann_json gives us string;
/// std::filesystem on Windows treats narrow strings as ANSI (mangles UTF-8).
/// We build paths from u8string explicitly to keep CJK names intact.
std::filesystem::path u8ToPath(const std::string& s)
{
    const std::u8string u8(reinterpret_cast<const char8_t*>(s.c_str()), s.size());
    return std::filesystem::path(u8);
}

std::string pathToU8(const std::filesystem::path& p)
{
    const auto u8 = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

/// .bin layout for a v1 mask document (Phase 7 legacy):
///   uint32 magic   = 'MMSK'
///   uint32 version = 1
///   uint32 size    = N (texture is N×N)
///   uint8[N*N*4]   = raw RGBA8 pixels, row-major
constexpr uint32_t kMaskMagic   = 0x4B534D4D;  // "MMSK" little-endian
constexpr uint32_t kMaskVersion = 1;

/// .bin layout for a v2 PaintLayer:
///   uint32 magic   = 'MLYR'
///   uint32 version = 1   (PaintLayer payload schema, NOT project schema)
///   uint32 size    = N
///   uint8[N*N*4]   = RGBA pixels, row-major
///   uint8[N*N]     = wroteMask (low 4 bits per byte)
constexpr uint32_t kPaintLayerMagic   = 0x52594C4D;  // "MLYR"
constexpr uint32_t kPaintLayerVersion = 1;

/// .bin layout for an ImageLayer / ChannelLayer source image:
///   uint32 magic   = 'MIMG'
///   uint32 version = 1
///   uint32 size    = N (already resampled to target size for ImageLayer,
///                       or the raw source size for ChannelLayer)
///   uint8[N*N*4]   = RGBA pixels
constexpr uint32_t kImageMagic   = 0x474D494D;  // "MIMG"
constexpr uint32_t kImageVersion = 1;

bool writePaintLayerBin(const std::filesystem::path& path,
                        const PaintLayer&            layer,
                        std::string&                 err)
{
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    FILE* fp = wfopen(path, L"wb");
    if (!fp) { err = "could not open for write: " + pathToU8(path); return false; }

    const uint32_t hdr[3] = { kPaintLayerMagic, kPaintLayerVersion,
                              static_cast<uint32_t>(layer.size()) };
    if (std::fwrite(hdr, sizeof(hdr), 1, fp) != 1) {
        std::fclose(fp); err = "header write failed"; return false;
    }
    const size_t pxBytes    = static_cast<size_t>(layer.size()) * layer.size() * 4;
    const size_t wroteBytes = static_cast<size_t>(layer.size()) * layer.size();
    if (std::fwrite(layer.pixels(), 1, pxBytes, fp) != pxBytes) {
        std::fclose(fp); err = "pixel write failed"; return false;
    }
    if (std::fwrite(layer.wroteMask(), 1, wroteBytes, fp) != wroteBytes) {
        std::fclose(fp); err = "wroteMask write failed"; return false;
    }
    std::fclose(fp);
    return true;
}

bool readPaintLayerBin(const std::filesystem::path& path,
                       PaintLayer&                  layer,
                       std::string&                 err)
{
    FILE* fp = wfopen(path, L"rb");
    if (!fp) { err = "could not open for read: " + pathToU8(path); return false; }

    uint32_t hdr[3];
    if (std::fread(hdr, sizeof(hdr), 1, fp) != 1) {
        std::fclose(fp); err = "header read failed (truncated)"; return false;
    }
    if (hdr[0] != kPaintLayerMagic) {
        std::fclose(fp); err = "bad PaintLayer magic"; return false;
    }
    if (hdr[1] != kPaintLayerVersion) {
        std::fclose(fp);
        err = "unsupported PaintLayer version " + std::to_string(hdr[1]);
        return false;
    }
    if (static_cast<int>(hdr[2]) != layer.size()) {
        std::fclose(fp);
        err = "size mismatch: file " + std::to_string(hdr[2])
            + " vs layer " + std::to_string(layer.size());
        return false;
    }

    const size_t pxBytes    = static_cast<size_t>(layer.size()) * layer.size() * 4;
    const size_t wroteBytes = static_cast<size_t>(layer.size()) * layer.size();
    std::vector<uint8_t> px(pxBytes);
    std::vector<uint8_t> wrote(wroteBytes);
    if (std::fread(px.data(), 1, pxBytes, fp) != pxBytes) {
        std::fclose(fp); err = "pixel read failed (truncated)"; return false;
    }
    if (std::fread(wrote.data(), 1, wroteBytes, fp) != wroteBytes) {
        std::fclose(fp); err = "wroteMask read failed (truncated)"; return false;
    }
    std::fclose(fp);

    layer.setAll(px.data(), wrote.data());
    return true;
}

bool writeImageBin(const std::filesystem::path& path,
                   const uint8_t* rgba, int size,
                   std::string& err)
{
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    FILE* fp = wfopen(path, L"wb");
    if (!fp) { err = "could not open for write: " + pathToU8(path); return false; }
    const uint32_t hdr[3] = { kImageMagic, kImageVersion, static_cast<uint32_t>(size) };
    if (std::fwrite(hdr, sizeof(hdr), 1, fp) != 1) {
        std::fclose(fp); err = "header write failed"; return false;
    }
    const size_t bytes = static_cast<size_t>(size) * size * 4;
    if (std::fwrite(rgba, 1, bytes, fp) != bytes) {
        std::fclose(fp); err = "pixel write failed"; return false;
    }
    std::fclose(fp);
    return true;
}

bool readImageBin(const std::filesystem::path& path,
                  std::vector<uint8_t>&        outRgba,
                  int&                         outSize,
                  std::string&                 err)
{
    FILE* fp = wfopen(path, L"rb");
    if (!fp) { err = "could not open for read: " + pathToU8(path); return false; }
    uint32_t hdr[3];
    if (std::fread(hdr, sizeof(hdr), 1, fp) != 1) {
        std::fclose(fp); err = "header read failed (truncated)"; return false;
    }
    if (hdr[0] != kImageMagic) { std::fclose(fp); err = "bad MIMG magic"; return false; }
    if (hdr[1] != kImageVersion) {
        std::fclose(fp);
        err = "unsupported MIMG version " + std::to_string(hdr[1]);
        return false;
    }
    outSize = static_cast<int>(hdr[2]);
    const size_t bytes = static_cast<size_t>(outSize) * outSize * 4;
    outRgba.resize(bytes);
    if (std::fread(outRgba.data(), 1, bytes, fp) != bytes) {
        std::fclose(fp); err = "pixel read failed (truncated)"; return false;
    }
    std::fclose(fp);
    return true;
}

bool writeMaskBin(const std::filesystem::path& path, const TextureDocument& doc, std::string& err)
{
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    FILE* fp = wfopen(path, L"wb");
    if (!fp) { err = "could not open for write: " + pathToU8(path); return false; }

    const uint32_t hdr[3] = { kMaskMagic, kMaskVersion, static_cast<uint32_t>(doc.size()) };
    if (std::fwrite(hdr, sizeof(hdr), 1, fp) != 1) {
        std::fclose(fp); err = "header write failed"; return false;
    }
    const size_t bytes = static_cast<size_t>(doc.size()) * doc.size() * 4;
    if (std::fwrite(doc.pixels(), 1, bytes, fp) != bytes) {
        std::fclose(fp); err = "pixel write failed"; return false;
    }
    std::fclose(fp);
    return true;
}

bool readMaskBin(const std::filesystem::path& path, TextureDocument& doc, std::string& err)
{
    FILE* fp = wfopen(path, L"rb");
    if (!fp) { err = "could not open for read: " + pathToU8(path); return false; }

    uint32_t hdr[3];
    if (std::fread(hdr, sizeof(hdr), 1, fp) != 1) {
        std::fclose(fp); err = "header read failed (truncated)"; return false;
    }
    if (hdr[0] != kMaskMagic) { std::fclose(fp); err = "bad magic"; return false; }
    if (hdr[1] != kMaskVersion) {
        std::fclose(fp);
        err = "unsupported mask version " + std::to_string(hdr[1]);
        return false;
    }
    if (static_cast<int>(hdr[2]) != doc.size()) {
        std::fclose(fp);
        err = "size mismatch: file has " + std::to_string(hdr[2])
            + " but doc has " + std::to_string(doc.size());
        return false;
    }

    std::vector<uint8_t> buf(static_cast<size_t>(doc.size()) * doc.size() * 4);
    if (std::fread(buf.data(), 1, buf.size(), fp) != buf.size()) {
        std::fclose(fp); err = "pixel read failed (truncated)"; return false;
    }
    std::fclose(fp);

    doc.setPixelsAll(buf.data());   // marks dirty for both consumers + everModified
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────
// Header read
// ─────────────────────────────────────────────────────────────────

bool readProjectHeader(const std::filesystem::path& projectPath,
                       ProjectMeta&                 outMeta,
                       std::string&                 outError)
{
    if (!std::filesystem::exists(projectPath)) {
        outError = "project file not found";
        return false;
    }

    std::ifstream in(projectPath);   // narrow path is fine on Windows for std streams; we use a wifstream pattern below if needed
    if (!in) {
        outError = "could not open project file";
        return false;
    }

    json j;
    try { in >> j; }
    catch (const std::exception& e) {
        outError = std::string("JSON parse error: ") + e.what();
        return false;
    }

    if (!j.contains("version") || !j["version"].is_number_integer()) {
        outError = "missing 'version' field";
        return false;
    }
    const int v = j["version"].get<int>();
    // Accept v1 (Phase 7), v2 (Phase 8), and v3 (Phase 10). loadProjectMasks
    // branches on version; saveProject always writes the latest.
    if (v != 1 && v != 2 && v != 3) {
        outError = "unsupported version " + std::to_string(v);
        return false;
    }

    outMeta = {};   // start clean

    // modelPath is relative to the .mstcproj. Resolve to absolute lazily by caller.
    if (j.contains("modelPath") && j["modelPath"].is_string()) {
        const std::filesystem::path rel = u8ToPath(j["modelPath"].get<std::string>());
        outMeta.modelPath = rel.is_absolute()
                                ? rel
                                : projectPath.parent_path() / rel;
    }
    if (j.contains("textureSize") && j["textureSize"].is_number_integer()) {
        outMeta.textureSize = j["textureSize"].get<int>();
    }

    if (j.contains("ui") && j["ui"].is_object()) {
        const auto& ui = j["ui"];
        if (ui.contains("activeGroupIndex")) outMeta.activeGroupIndex = ui["activeGroupIndex"].get<int>();
        if (ui.contains("activeKind") && ui["activeKind"].is_string()) {
            kindFromKey(ui["activeKind"].get<std::string>(), outMeta.activeKind);
        }
        if (ui.contains("channelView"))     outMeta.channelView     = ui["channelView"].get<int>();
        if (ui.contains("viewMode"))        outMeta.viewMode        = ui["viewMode"].get<int>();
        if (ui.contains("autoSwitchGroup")) outMeta.autoSwitchGroup = ui["autoSwitchGroup"].get<bool>();
    }

    if (j.contains("camera") && j["camera"].is_object()) {
        const auto& c = j["camera"];
        if (c.contains("yaw"))      outMeta.cameraYaw      = c["yaw"].get<float>();
        if (c.contains("pitch"))    outMeta.cameraPitch    = c["pitch"].get<float>();
        if (c.contains("distance")) outMeta.cameraDistance = c["distance"].get<float>();
        if (c.contains("target") && c["target"].is_array() && c["target"].size() == 3) {
            outMeta.cameraTarget = {
                c["target"][0].get<float>(),
                c["target"][1].get<float>(),
                c["target"][2].get<float>(),
            };
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────
// Mask load
// ─────────────────────────────────────────────────────────────────

namespace {

/// Load one v2 layer entry (a JSON object) into its slot in the stack at
/// `stack` (already cleared to its default Base layer). The layer is
/// appended; the host arranges callers to clear the stack first.
bool loadV2Layer(const json&                         lj,
                 LayerStack&                         stack,
                 TextureKind                         k,
                 const std::filesystem::path&        companion)
{
    using editor::Layer;
    using editor::PaintLayer;
    using editor::FillLayer;
    using editor::ImageLayer;
    using editor::ChannelLayer;
    using editor::LayerType;
    using editor::BlendMode;

    if (!lj.is_object() || !lj.contains("type") || !lj["type"].is_string()) {
        return false;
    }
    const std::string typeStr = lj["type"].get<std::string>();

    // Legacy: v3 had a `faceSdf` LayerType. The feature was dropped in the
    // current version, so silently skip these layers when loading old files.
    if (typeStr == "faceSdf") return false;

    LayerType type;
    if (!editor::layerTypeFromName(typeStr, type)) {
        spdlog::warn("Project: unknown layer type '{}'", typeStr);
        return false;
    }

    std::unique_ptr<Layer> layer;
    switch (type) {
    case LayerType::Paint: {
        auto p = std::make_unique<PaintLayer>(stack.size());
        if (lj.contains("data") && lj["data"].is_string()) {
            const auto rel  = u8ToPath(lj["data"].get<std::string>());
            const auto full = rel.is_absolute() ? rel : (companion / rel);
            if (std::filesystem::exists(full)) {
                std::string err;
                if (!readPaintLayerBin(full, *p, err)) {
                    spdlog::warn("Project: paint layer load: {}", err);
                }
            } else {
                spdlog::warn("Project: layer file missing: {}", pathToU8(full));
            }
        }
        layer = std::move(p);
        break;
    }
    case LayerType::Fill: {
        // Fill data is inline in JSON.
        uint8_t r = 0, g = 0, b = 0, a = 255;
        editor::ChannelAuthorMask cm = editor::kAuthorAll;
        if (lj.contains("fill") && lj["fill"].is_object()) {
            const auto& fj = lj["fill"];
            if (fj.contains("r")) r = static_cast<uint8_t>(fj["r"].get<int>());
            if (fj.contains("g")) g = static_cast<uint8_t>(fj["g"].get<int>());
            if (fj.contains("b")) b = static_cast<uint8_t>(fj["b"].get<int>());
            if (fj.contains("a")) a = static_cast<uint8_t>(fj["a"].get<int>());
            if (fj.contains("channelMask"))
                cm = static_cast<editor::ChannelAuthorMask>(fj["channelMask"].get<int>());
        }
        auto f = std::make_unique<FillLayer>(stack.size(), r, g, b, a, cm);
        // Optional selection mask sidecar — stored as raw bytes in MIMG-style file.
        if (lj.contains("selection") && lj["selection"].is_string()) {
            const auto rel = u8ToPath(lj["selection"].get<std::string>());
            const auto full = rel.is_absolute() ? rel : (companion / rel);
            std::vector<uint8_t> sel;
            int selSize = 0;
            std::string err;
            if (readImageBin(full, sel, selSize, err) && selSize == stack.size()) {
                // Stored as RGBA but we only need 1 byte/pixel — extract R.
                std::vector<uint8_t> mask(static_cast<size_t>(selSize) * selSize);
                for (size_t i = 0; i < mask.size(); ++i) mask[i] = sel[i * 4];
                f->setSelection(std::move(mask));
            }
        }
        layer = std::move(f);
        break;
    }
    case LayerType::Image: {
        if (!lj.contains("data") || !lj["data"].is_string()) {
            spdlog::warn("Project: ImageLayer missing data path");
            return false;
        }
        const auto rel  = u8ToPath(lj["data"].get<std::string>());
        const auto full = rel.is_absolute() ? rel : (companion / rel);
        std::vector<uint8_t> rgba;
        int srcSize = 0;
        std::string err;
        if (!readImageBin(full, rgba, srcSize, err)) {
            spdlog::warn("Project: image load: {}", err);
            return false;
        }
        editor::ChannelAuthorMask cm = editor::kAuthorAll;
        if (lj.contains("channelMask"))
            cm = static_cast<editor::ChannelAuthorMask>(lj["channelMask"].get<int>());
        layer = std::make_unique<ImageLayer>(stack.size(), rgba.data(), srcSize, cm);
        break;
    }
    case LayerType::Channel: {
        if (!lj.contains("data") || !lj["data"].is_string()) return false;
        const auto rel  = u8ToPath(lj["data"].get<std::string>());
        const auto full = rel.is_absolute() ? rel : (companion / rel);
        auto srcRgba = std::make_shared<std::vector<uint8_t>>();
        int srcSize = 0;
        std::string err;
        if (!readImageBin(full, *srcRgba, srcSize, err)) {
            spdlog::warn("Project: channel src load: {}", err);
            return false;
        }
        const uint8_t srcCh = lj.contains("srcChannel")
            ? static_cast<uint8_t>(lj["srcChannel"].get<int>()) : 0;
        const uint8_t dstCh = lj.contains("dstChannel")
            ? static_cast<uint8_t>(lj["dstChannel"].get<int>()) : 0;
        layer = std::make_unique<ChannelLayer>(stack.size(), std::move(srcRgba),
                                               srcSize, srcCh, dstCh);
        break;
    }
    } // switch

    if (!layer) return false;

    // Common metadata
    if (lj.contains("name") && lj["name"].is_string())
        layer->name = lj["name"].get<std::string>();
    if (lj.contains("visible"))
        layer->visible = lj["visible"].get<bool>();
    if (lj.contains("opacity"))
        layer->opacity = std::clamp(lj["opacity"].get<float>(), 0.0f, 1.0f);
    if (lj.contains("blend") && lj["blend"].is_string()) {
        BlendMode bm;
        if (editor::blendModeFromName(lj["blend"].get<std::string>(), bm))
            layer->blendMode = bm;
    }

    stack.addLayer(std::move(layer));
    (void)k;   // kind is implicit in stack
    return true;
}

} // anonymous

bool loadProjectMasks(const std::filesystem::path& projectPath,
                      TextureGroupSet&             groups,
                      const ProjectMeta&           /*meta*/,
                      std::string&                 outError)
{
    std::ifstream in(projectPath);
    if (!in) { outError = "could not reopen project file"; return false; }

    json j;
    try { in >> j; }
    catch (const std::exception& e) { outError = std::string("JSON parse: ") + e.what(); return false; }

    const int version = j.contains("version") && j["version"].is_number_integer()
                        ? j["version"].get<int>() : 1;

    if (!j.contains("groups") || !j["groups"].is_array()) {
        // Header-only project (no mask data yet) → nothing to restore. That's
        // valid for a brand-new save before any painting.
        return true;
    }

    const std::filesystem::path companion = companionDir(projectPath);

    for (const auto& gj : j["groups"]) {
        if (!gj.contains("materialIndex")) continue;
        const int mi = gj["materialIndex"].get<int>();
        if (mi < 0 || mi >= static_cast<int>(groups.groups.size())) continue;
        auto& g = groups.groups[mi];

        if (version == 1) {
            // ── v1 path: one mask blob per kind, load into Base PaintLayer ──
            if (!gj.contains("masks") || !gj["masks"].is_object()) continue;
            for (const auto& [keyStr, val] : gj["masks"].items()) {
                if (!val.is_string()) continue;
                TextureKind k;
                if (!kindFromKey(keyStr, k)) continue;
                const size_t ki = static_cast<size_t>(k);
                if (!g.stacks[ki]) continue;

                const std::filesystem::path relBin = u8ToPath(val.get<std::string>());
                const std::filesystem::path full   = relBin.is_absolute()
                                                      ? relBin
                                                      : (companion / relBin);

                if (!std::filesystem::exists(full)) {
                    spdlog::warn("Project: mask file missing: {}", pathToU8(full));
                    continue;
                }
                auto& stack = *g.stacks[ki];
                TextureDocument scratch(k, stack.size());
                std::string err;
                if (!readMaskBin(full, scratch, err)) {
                    spdlog::warn("Project: failed to load {} — {}", pathToU8(full), err);
                    continue;
                }
                auto* base = dynamic_cast<PaintLayer*>(stack.layer(0));
                if (base) {
                    base->setAll(scratch.pixels(), /*wroteSize2=*/nullptr);
                    stack.recompositeAll();
                }
            }
            continue;   // next group
        }

        // ── v2 path: per-layer entries under groups[].layerStacks[kindKey] ──
        if (!gj.contains("layerStacks") || !gj["layerStacks"].is_object()) continue;
        for (const auto& [keyStr, layersJson] : gj["layerStacks"].items()) {
            TextureKind k;
            if (!kindFromKey(keyStr, k)) continue;
            const size_t ki = static_cast<size_t>(k);
            if (!g.stacks[ki]) continue;
            if (!layersJson.is_array() || layersJson.empty()) continue;

            auto& stack = *g.stacks[ki];
            // Track current layer count BEFORE loading. Stack starts with
            // a single auto-created Base layer; we'll remove it after pushing
            // the file's layers if there are any.
            const int oldCount = stack.layerCount();
            int loaded = 0;
            for (const auto& lj : layersJson) {
                if (loadV2Layer(lj, stack, k, companion)) ++loaded;
            }
            // Remove the original Base layer (index 0) IF we successfully
            // pushed at least one new layer. If load failed for every layer,
            // keep Base so the stack stays editable.
            if (loaded > 0 && oldCount == 1) {
                // Old base is now at index 0; loaded ones occupy [1 .. loaded].
                stack.removeLayer(0);
            }
            stack.recompositeAll();
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────
// Save
// ─────────────────────────────────────────────────────────────────

bool saveProject(const std::filesystem::path& projectPath,
                 const ProjectMeta&             meta,
                 const TextureGroupSet&         groups,
                 std::string&                   outError)
{
    // ── companion directory ────────────────────────────────────
    const std::filesystem::path companion = companionDir(projectPath);
    std::error_code ec;
    std::filesystem::create_directories(companion / "layers", ec);
    if (ec) {
        outError = "failed to create companion dir: " + ec.message();
        return false;
    }

    // ── build JSON + write per-layer sidecars ──────────────────
    json root;
    root["version"]     = kProjectVersion;

    // modelPath stored relative to projectPath when possible — keeps projects
    // portable across machines where the model lives at a different absolute
    // path than the project.
    {
        std::filesystem::path rel = meta.modelPath;
        std::error_code relEc;
        auto relTry = std::filesystem::relative(meta.modelPath,
                                                 projectPath.parent_path(),
                                                 relEc);
        if (!relEc && !relTry.empty()) rel = relTry;
        root["modelPath"] = pathToU8(rel.generic_string().empty() ? meta.modelPath : rel);
    }

    root["textureSize"] = meta.textureSize;

    // Stable per-(group, kind, layerIndex) name so layer sidecars don't collide.
    int totalLayersWritten = 0;
    int totalLayersSkipped = 0;

    json groupsArr = json::array();
    for (size_t gi = 0; gi < groups.groups.size(); ++gi) {
        const auto& g = groups.groups[gi];
        json gj;
        gj["materialIndex"] = g.materialIndices.empty() ? -1 : g.materialIndices.front();
        gj["name"]          = g.name;
        gj["diffuseName"]   = g.diffuseName;

        // ── layerStacks: { kindKey: [layer entries] } ───────────
        json layerStacks = json::object();
        for (size_t ki = 0; ki < g.stacks.size(); ++ki) {
            const auto* stack = g.stacks[ki].get();
            if (!stack) continue;
            const auto kind = static_cast<TextureKind>(ki);

            // Skip kinds whose stack is "default" — single Base PaintLayer
            // with no authored pixels. Saves disk on empty kinds (a brand-new
            // project keeps every .mstcproj small).
            const auto* composite = &stack->composite();
            const bool kindUntouched =
                stack->layerCount() == 1
                && stack->layer(0)->type() == LayerType::Paint
                && (!composite->everModified());
            if (kindUntouched) {
                ++totalLayersSkipped;
                continue;
            }

            json arr = json::array();
            for (int li = 0; li < stack->layerCount(); ++li) {
                const Layer* L = stack->layer(li);
                if (!L) continue;

                json lj;
                lj["name"]    = L->name;
                lj["type"]    = editor::layerTypeName(L->type());
                lj["visible"] = L->visible;
                lj["opacity"] = L->opacity;
                lj["blend"]   = editor::blendModeName(L->blendMode);

                switch (L->type()) {
                case LayerType::Paint: {
                    const auto* pl = static_cast<const PaintLayer*>(L);
                    const std::string rel = layerRelPath(gi, g.name, kind, li);
                    const auto full = companion / u8ToPath(rel);
                    std::string err;
                    if (!writePaintLayerBin(full, *pl, err)) {
                        spdlog::error("Project: paint layer write {}: {}", pathToU8(full), err);
                        continue;
                    }
                    lj["data"] = rel;
                    ++totalLayersWritten;
                    break;
                }
                case LayerType::Fill: {
                    const auto* fl = static_cast<const FillLayer*>(L);
                    json fj;
                    fj["r"] = fl->r();
                    fj["g"] = fl->g();
                    fj["b"] = fl->b();
                    fj["a"] = fl->a();
                    fj["channelMask"] = fl->channelMask();
                    lj["fill"] = std::move(fj);
                    // Selection mask (if any) goes to a sidecar as MIMG-style
                    // RGBA (R = mask byte, GBA = 0).
                    if (fl->hasSelection()) {
                        const auto& sel = fl->selection();
                        const int N = fl->size();
                        std::vector<uint8_t> rgba(static_cast<size_t>(N) * N * 4, 0);
                        for (size_t i = 0; i < sel.size(); ++i) {
                            rgba[i * 4 + 0] = sel[i];
                            rgba[i * 4 + 3] = 255;
                        }
                        const std::string rel = layerRelPath(gi, g.name, kind, li);
                        const auto full = companion / u8ToPath(rel);
                        std::string err;
                        if (writeImageBin(full, rgba.data(), N, err)) {
                            lj["selection"] = rel;
                        } else {
                            spdlog::error("Project: fill sel write {}: {}", pathToU8(full), err);
                        }
                    }
                    ++totalLayersWritten;
                    break;
                }
                case LayerType::Image: {
                    const auto* il = static_cast<const ImageLayer*>(L);
                    const std::string rel = layerRelPath(gi, g.name, kind, li);
                    const auto full = companion / u8ToPath(rel);
                    std::string err;
                    if (!writeImageBin(full, il->pixels(), il->size(), err)) {
                        spdlog::error("Project: image write {}: {}", pathToU8(full), err);
                        continue;
                    }
                    lj["data"] = rel;
                    lj["channelMask"] = il->channelMask();
                    ++totalLayersWritten;
                    break;
                }
                case LayerType::Channel: {
                    const auto* cl = static_cast<const ChannelLayer*>(L);
                    const std::string rel = layerRelPath(gi, g.name, kind, li);
                    const auto full = companion / u8ToPath(rel);
                    std::string err;
                    const auto& src = cl->srcImage();
                    if (!src || src->empty()) continue;
                    if (!writeImageBin(full, src->data(), cl->srcSize(), err)) {
                        spdlog::error("Project: channel src write {}: {}", pathToU8(full), err);
                        continue;
                    }
                    lj["data"]       = rel;
                    lj["srcChannel"] = cl->srcChannel();
                    lj["dstChannel"] = cl->dstChannel();
                    ++totalLayersWritten;
                    break;
                }
                }   // switch
                arr.push_back(std::move(lj));
            }
            layerStacks[kindKey(kind)] = std::move(arr);
        }
        gj["layerStacks"] = std::move(layerStacks);

        groupsArr.push_back(std::move(gj));
    }
    root["groups"] = std::move(groupsArr);

    json ui;
    ui["activeGroupIndex"] = meta.activeGroupIndex;
    ui["activeKind"]       = kindKey(meta.activeKind);
    ui["channelView"]      = meta.channelView;
    ui["viewMode"]         = meta.viewMode;
    ui["autoSwitchGroup"]  = meta.autoSwitchGroup;
    root["ui"] = std::move(ui);

    json cam;
    cam["yaw"]      = meta.cameraYaw;
    cam["pitch"]    = meta.cameraPitch;
    cam["distance"] = meta.cameraDistance;
    cam["target"]   = json::array({meta.cameraTarget.x, meta.cameraTarget.y, meta.cameraTarget.z});
    root["camera"] = std::move(cam);

    // ── write JSON ─────────────────────────────────────────────
    {
        FILE* fp = wfopen(projectPath, L"wb");
        if (!fp) { outError = "could not open project file for write"; return false; }
        const std::string s = root.dump(2);
        if (std::fwrite(s.data(), 1, s.size(), fp) != s.size()) {
            std::fclose(fp);
            outError = "JSON write failed";
            return false;
        }
        std::fclose(fp);
    }

    spdlog::info("Project saved: {} layers written, {} kinds skipped (untouched)",
                 totalLayersWritten, totalLayersSkipped);
    return true;
}

} // namespace project
