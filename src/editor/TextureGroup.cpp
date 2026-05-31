#include "TextureGroup.h"

#include "UvCoverage.h"

#include <QImage>
#include <Windows.h>
#include <spdlog/spdlog.h>

#include <unordered_map>
#include <unordered_set>

namespace editor {

namespace {

std::filesystem::path resolveTexturePath(const std::filesystem::path& modelDir,
                                          const std::string&           rel)
{
    std::string normalized = rel;
    for (char& c : normalized) if (c == '\\') c = '/';

    int wlen = MultiByteToWideChar(CP_UTF8, 0, normalized.data(),
                                   static_cast<int>(normalized.size()),
                                   nullptr, 0);
    std::wstring wide(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, normalized.data(),
                        static_cast<int>(normalized.size()),
                        wide.data(), wlen);
    return modelDir / wide;
}

/// Pretty name for a group: prefer the material's JP name, fall back to EN, then "Material #N".
std::string makeMaterialName(const pmx::Model& model, int matIdx)
{
    if (matIdx < 0 || matIdx >= static_cast<int>(model.materials.size())) {
        return "Material";
    }
    const auto& m = model.materials[matIdx];
    if (!m.nameJa.empty()) return m.nameJa;
    if (!m.nameEn.empty()) return m.nameEn;
    return "Material #" + std::to_string(matIdx);
}

/// Filename stem of the diffuse path (e.g. "head.png" → "head"). Empty if no diffuse.
std::string makeDiffuseName(const pmx::Model& model, int diffuseIdx)
{
    if (diffuseIdx < 0 || diffuseIdx >= static_cast<int>(model.texturePaths.size())) {
        return {};
    }
    const std::string& path = model.texturePaths[diffuseIdx];
    size_t slash = path.find_last_of("/\\");
    size_t start = (slash == std::string::npos) ? 0 : slash + 1;
    size_t dot   = path.find_last_of('.');
    if (dot == std::string::npos || dot < start) dot = path.size();
    return path.substr(start, dot - start);
}

inline uint64_t edgeKey(int32_t a, int32_t b)
{
    const uint32_t lo = static_cast<uint32_t>(std::min(a, b));
    const uint32_t hi = static_cast<uint32_t>(std::max(a, b));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

QImage loadDiffuse(const pmx::Model& model, int diffuseIdx,
                   const std::filesystem::path& modelDir)
{
    if (diffuseIdx < 0 || diffuseIdx >= static_cast<int>(model.texturePaths.size())) {
        return {};
    }
    const auto full = resolveTexturePath(modelDir, model.texturePaths[diffuseIdx]);
    if (!std::filesystem::exists(full)) return {};
    QImage img(QString::fromStdWString(full.wstring()));
    if (img.isNull()) return {};

    // Force alpha = 255. PMX diffuse textures often carry an alpha mask used
    // by the in-game shader to cut hair/eyelashes — but in our 2D editor the
    // background is just a reference image, and a transparent alpha channel
    // would let the checkerboard bleed through and clutter the view.
    //
    // Convert to RGB888 (drops alpha) then back to a 32-bit format that the
    // canvas can paint efficiently. RGB888 -> ARGB32 fills alpha with 255.
    img = img.convertToFormat(QImage::Format_RGB888)
             .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    return img;
}

} // anonymous

// ─────────────────────────────────────────────────────────────────
// buildTextureGroups
// ─────────────────────────────────────────────────────────────────

TextureGroupSet buildTextureGroups(
    const pmx::Model&            model,
    const std::filesystem::path& modelDir,
    int                          textureSize)
{
    TextureGroupSet out;

    // ── One group per material, in PMX material order ──────────
    // Diffuse images are deduped via a small cache: many materials of the
    // same model frequently share an atlas (e.g. 8 face mats all on
    // head.png), so we don't want to decode the PNG 8 times.
    std::unordered_map<int, QImage> diffuseCache;

    out.groups.reserve(model.materials.size());
    for (size_t mi = 0; mi < model.materials.size(); ++mi) {
        const auto& mat = model.materials[mi];
        TextureGroup g{};
        g.diffuseTextureIndex = mat.diffuseTextureIndex;
        g.materialIndices.push_back(static_cast<int>(mi));
        g.name        = makeMaterialName(model, static_cast<int>(mi));
        g.diffuseName = makeDiffuseName(model, mat.diffuseTextureIndex);

        // Resolve diffuse via cache
        if (mat.diffuseTextureIndex >= 0) {
            auto it = diffuseCache.find(mat.diffuseTextureIndex);
            if (it == diffuseCache.end()) {
                QImage img = loadDiffuse(model, mat.diffuseTextureIndex, modelDir);
                it = diffuseCache.emplace(mat.diffuseTextureIndex, std::move(img)).first;
            }
            g.diffuse = it->second;   // QImage shares pixel buffer (implicit sharing)
        }

        // Allocate a fresh LayerStack (with one auto-created Base PaintLayer)
        // per kind for this material. The composite TextureDocument owned by
        // the stack is what downstream consumers read.
        for (size_t i = 0; i < g.stacks.size(); ++i) {
            const auto kind = static_cast<TextureKind>(i);
            g.stacks[i] = std::make_unique<LayerStack>(kind, textureSize);
            // Drain the composite's init-time dirty state so the canvas/GPU
            // don't think the texture changed before any user edit.
            auto& doc = g.stacks[i]->composite();
            doc.clear(0, 0, 0, 255);
            doc.takeDirtyBounds();
            doc.resetEverModified();
        }

        // UV coverage of THIS material only — flood-fill / bucket constraints
        // shouldn't escape a single material's UV island.
        g.uvCoverage = buildUvCoverageMask(model, g.materialIndices, textureSize);
        g.uvCoverageSize = textureSize;

        out.groups.push_back(std::move(g));
    }

    // ── Build UV wireframe, concatenated per group ──────────────
    // Each group corresponds to a single material, so segment ownership is
    // straightforward — no inter-material dedup needed (those were already
    // separate edges between adjacent materials).

    std::vector<std::vector<UvSegment>> perGroupSegs(out.groups.size());
    std::vector<std::unordered_set<uint64_t>> perGroupSeen(out.groups.size());

    int faceCursor = 0;
    for (size_t mi = 0; mi < model.materials.size(); ++mi) {
        const int triCount = model.materials[mi].indexCount / 3;
        const int faceStart = faceCursor;
        const int faceEnd   = faceCursor + triCount;
        faceCursor = faceEnd;

        const int gi = static_cast<int>(mi);   // 1:1 mapping
        if (gi < 0 || gi >= static_cast<int>(out.groups.size())) continue;

        auto& segs = perGroupSegs[gi];
        auto& seen = perGroupSeen[gi];

        auto pushEdge = [&](int32_t i0, int32_t i1) {
            if (i0 < 0 || i1 < 0 || i0 == i1
                || i0 >= static_cast<int32_t>(model.vertices.size())
                || i1 >= static_cast<int32_t>(model.vertices.size())) return;
            const uint64_t k = edgeKey(i0, i1);
            if (!seen.insert(k).second) return;
            const auto& uv0 = model.vertices[i0].uv;
            const auto& uv1 = model.vertices[i1].uv;
            segs.push_back({ QPointF(uv0.x, uv0.y), QPointF(uv1.x, uv1.y) });
        };

        for (int f = faceStart; f < faceEnd && f < static_cast<int>(model.faces.size()); ++f) {
            const auto& tri = model.faces[f];
            pushEdge(tri[0], tri[1]);
            pushEdge(tri[1], tri[2]);
            pushEdge(tri[2], tri[0]);
        }
    }

    // Flatten into wireframe.segs and record per-group ranges
    int totalSegs = 0;
    for (const auto& v : perGroupSegs) totalSegs += static_cast<int>(v.size());
    out.wireframe.segs.reserve(totalSegs);

    for (size_t gi = 0; gi < out.groups.size(); ++gi) {
        out.groups[gi].uvSegmentStart = static_cast<int>(out.wireframe.segs.size());
        out.groups[gi].uvSegmentCount = static_cast<int>(perGroupSegs[gi].size());
        for (const auto& s : perGroupSegs[gi]) out.wireframe.segs.push_back(s);
    }

    spdlog::info("TextureGroups: {} groups (1:1 with materials, {} unique diffuses)",
                 out.groups.size(), diffuseCache.size());
    for (const auto& g : out.groups) {
        spdlog::info("  '{}' (diffuse '{}'): {} UV edges, {}x{}",
                     g.name,
                     g.diffuseName.empty() ? "<none>" : g.diffuseName,
                     g.uvSegmentCount,
                     g.diffuse.width(), g.diffuse.height());
    }

    return out;
}

} // namespace editor
