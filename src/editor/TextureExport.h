#pragma once

#include "TextureDocument.h"
#include "TextureGroup.h"

#include <filesystem>
#include <string>
#include <vector>

namespace editor {

/// Filename-safe short name for a TextureKind, e.g. "ShadowRate" / "FaceSDF".
/// Differs from textureKindName() which is the human-readable "Shadow Rate".
const char* textureKindShortName(TextureKind k);

/// Write a single TextureDocument's pixels to PNG (RGBA8).
/// Returns true on success. On failure, fills outError with the reason.
bool exportTextureToPng(const TextureDocument&       doc,
                        const std::filesystem::path& outPath,
                        std::string&                 outError);

/// Result of a batch export over a TextureGroupSet.
struct BatchExportResult
{
    int successCount = 0;
    int totalCount   = 0;
    /// Per-file errors (path → reason). Empty on full success.
    std::vector<std::pair<std::filesystem::path, std::string>> errors;
};

/// Export every (group × kind) under outDir.
///
/// File naming: "{ggg}_{matName}_{kindShort}.png" where ggg is the
/// zero-padded group index — keeps files sorted in MMD-material order
/// even when material names start with the same characters.
///
/// All groups produce a file even if `everModified() == false`, so MMD's
/// per-material `.fx` doesn't fall back to a default texture for masks
/// that were intentionally left at the default state.
BatchExportResult exportAllToFolder(const TextureGroupSet&       groups,
                                    const std::filesystem::path& outDir);

} // namespace editor
