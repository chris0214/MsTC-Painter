#pragma once

#include "../pmx/PmxTypes.h"

#include <QPointF>
#include <string>
#include <vector>

namespace editor {

/// One line segment in UV space (0..1, 0..1).
/// Note PMX V coords are stored as-is (V=0 at top, like image space).
struct UvSegment
{
    QPointF a;
    QPointF b;
};

/// One material's slice of the wireframe.
/// `segmentStart` and `segmentCount` index into `UvWireframe::segs`.
struct MaterialGroup
{
    int         materialIndex = -1;
    std::string name;             // UTF-8, JA name from PMX
    int         segmentStart = 0;
    int         segmentCount = 0;
};

/// All UV edges of a model, plus per-material partitioning.
/// Segments are stored grouped by material in order of appearance, so:
///   for each MaterialGroup g:
///     segs[g.segmentStart .. g.segmentStart + g.segmentCount)
///   are the edges belonging to that material, deduped within the material.
///
/// (Dedup is per-material, not global, because two materials may legitimately
/// share an edge along their boundary — both should draw it.)
struct UvWireframe
{
    std::vector<UvSegment>     segs;
    std::vector<MaterialGroup> groups;

    int totalSegmentCount() const { return static_cast<int>(segs.size()); }
};

/// Build the UV wireframe of a PMX model with per-material grouping.
UvWireframe extractUvWireframe(const pmx::Model& model);

} // namespace editor
