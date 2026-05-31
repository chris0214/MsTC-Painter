#include "TextureExport.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <Windows.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <system_error>

namespace editor {

const char* textureKindShortName(TextureKind k)
{
    switch (k) {
    case TextureKind::ShadowRate:   return "ShadowRate";
    case TextureKind::SubLightRate: return "SubLightRate";
    case TextureKind::EdgeRate:     return "EdgeRate";
    case TextureKind::FaceSDF:      return "FaceSDF";
    default:                        return "Unknown";
    }
}

namespace {

/// Replace filesystem-unsafe characters with '_'. Targets the Windows-illegal
/// set since that's our deployment platform; POSIX is more permissive.
/// Also collapses runs of whitespace into a single underscore.
std::string sanitizeForFilename(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    bool prevUnderscore = false;
    for (char c : in) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*'
            || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            // illegal — collapse
            if (!prevUnderscore && !out.empty()) {
                out.push_back('_');
                prevUnderscore = true;
            }
        } else if (c == ' ' || c == '\t') {
            if (!prevUnderscore && !out.empty()) {
                out.push_back('_');
                prevUnderscore = true;
            }
        } else {
            out.push_back(c);
            prevUnderscore = false;
        }
    }
    // Trim trailing underscores
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = "untitled";
    return out;
}

/// Open a file for binary writing with a wide path (handles non-ASCII names).
/// Returns nullptr on failure.
FILE* wfopenBinaryWrite(const std::filesystem::path& path)
{
    FILE* fp = nullptr;
    _wfopen_s(&fp, path.c_str(), L"wb");
    return fp;
}

/// stb writes via a callback so we can route bytes through our own FILE*.
/// This lets us open via _wfopen_s and support Unicode paths on Windows
/// (stb_image_write's stbi_write_png() uses fopen(), which mangles UTF-8).
void stbWriteCallback(void* user, void* data, int size)
{
    FILE* fp = static_cast<FILE*>(user);
    if (fp && data && size > 0) {
        std::fwrite(data, 1, static_cast<size_t>(size), fp);
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────
// Single-doc export
// ─────────────────────────────────────────────────────────────────

bool exportTextureToPng(const TextureDocument&       doc,
                        const std::filesystem::path& outPath,
                        std::string&                 outError)
{
    if (doc.size() <= 0 || !doc.pixels()) {
        outError = "document has no pixels";
        return false;
    }

    // Make sure the parent directory exists.
    std::error_code ec;
    if (outPath.has_parent_path()) {
        std::filesystem::create_directories(outPath.parent_path(), ec);
        // ignore ec — write below will surface failure if create failed
    }

    FILE* fp = wfopenBinaryWrite(outPath);
    if (!fp) {
        outError = "could not open file for writing";
        return false;
    }

    const int sz       = doc.size();
    const int rowPitch = sz * 4;
    const int ok = stbi_write_png_to_func(
        &stbWriteCallback, fp,
        sz, sz, /*comp*/ 4, doc.pixels(), rowPitch);
    std::fclose(fp);

    if (!ok) {
        // stb returns 0 on failure but no detail — best we can do is
        // remove the partial file and report a generic error.
        std::filesystem::remove(outPath, ec);
        outError = "stbi_write_png failed (disk full or path unwritable?)";
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────
// Batch
// ─────────────────────────────────────────────────────────────────

BatchExportResult exportAllToFolder(const TextureGroupSet&       groups,
                                    const std::filesystem::path& outDir)
{
    BatchExportResult result{};

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    // Continue even on ec — exportTextureToPng will surface per-file failures.

    for (size_t gi = 0; gi < groups.groups.size(); ++gi) {
        const auto& g = groups.groups[gi];
        const std::string safeName = sanitizeForFilename(g.name);

        for (size_t k = 0; k < g.stacks.size(); ++k) {
            const TextureDocument* doc = g.doc(k);
            if (!doc) continue;
            ++result.totalCount;

            const auto kind = static_cast<TextureKind>(k);
            char prefix[8];
            std::snprintf(prefix, sizeof(prefix), "%03zu", gi);

            const std::string fileName =
                std::string(prefix) + "_" + safeName + "_" + textureKindShortName(kind) + ".png";

            // Convert UTF-8 string → wide for the path. std::filesystem on
            // Windows treats narrow strings as legacy ANSI; non-ASCII names
            // would mangle. Build the path from a u8string explicitly.
            const std::u8string fileNameU8(reinterpret_cast<const char8_t*>(fileName.c_str()),
                                            fileName.size());
            const std::filesystem::path outPath =
                outDir / std::filesystem::path(fileNameU8);

            std::string err;
            if (exportTextureToPng(*doc, outPath, err)) {
                ++result.successCount;
            } else {
                result.errors.emplace_back(outPath, std::move(err));
            }
        }
    }

    spdlog::info("exportAllToFolder: {}/{} succeeded ({} errors)",
                 result.successCount, result.totalCount, result.errors.size());
    return result;
}

} // namespace editor
