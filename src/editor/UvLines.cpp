#include "UvLines.h"

#include <unordered_set>
#include <cstdint>

namespace editor {

namespace {

// Pack (smaller index, larger index) into one 64-bit key for an undirected edge.
inline uint64_t edgeKey(int32_t a, int32_t b)
{
    const uint32_t lo = static_cast<uint32_t>(std::min(a, b));
    const uint32_t hi = static_cast<uint32_t>(std::max(a, b));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

} // namespace

UvWireframe extractUvWireframe(const pmx::Model& model)
{
    UvWireframe out;
    out.segs.reserve(model.faces.size() * 3);
    out.groups.reserve(model.materials.size());

    // ── Walk materials in order, each owning a contiguous span of faces ──
    int faceCursor = 0;
    for (size_t mi = 0; mi < model.materials.size(); ++mi) {
        const auto& mat = model.materials[mi];
        const int triCount = mat.indexCount / 3;
        const int faceStart = faceCursor;
        const int faceEnd   = faceCursor + triCount;
        faceCursor = faceEnd;

        MaterialGroup g{};
        g.materialIndex = static_cast<int>(mi);
        g.name          = mat.nameJa.empty() ? mat.nameEn : mat.nameJa;
        if (g.name.empty()) g.name = std::string("Material ") + std::to_string(mi);
        g.segmentStart  = static_cast<int>(out.segs.size());

        // Dedup edges only within this material's faces
        std::unordered_set<uint64_t> seen;
        seen.reserve(static_cast<size_t>(triCount) * 3);

        auto pushEdge = [&](int32_t i0, int32_t i1) {
            if (i0 < 0 || i1 < 0
                || i0 >= static_cast<int32_t>(model.vertices.size())
                || i1 >= static_cast<int32_t>(model.vertices.size())
                || i0 == i1)
                return;
            const uint64_t k = edgeKey(i0, i1);
            if (!seen.insert(k).second) return;
            const auto& uv0 = model.vertices[i0].uv;
            const auto& uv1 = model.vertices[i1].uv;
            out.segs.push_back({ QPointF(uv0.x, uv0.y), QPointF(uv1.x, uv1.y) });
        };

        for (int f = faceStart; f < faceEnd && f < static_cast<int>(model.faces.size()); ++f) {
            const auto& tri = model.faces[f];
            pushEdge(tri[0], tri[1]);
            pushEdge(tri[1], tri[2]);
            pushEdge(tri[2], tri[0]);
        }

        g.segmentCount = static_cast<int>(out.segs.size()) - g.segmentStart;
        out.groups.push_back(std::move(g));
    }

    return out;
}

} // namespace editor
