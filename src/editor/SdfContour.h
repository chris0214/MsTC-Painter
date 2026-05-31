#pragma once

#include <vector>

namespace editor {

/// One Bezier anchor + its two attached handles.
///
/// Handle positions are stored as DELTAS in normalized UV (relative to the
/// anchor's pos), not as absolute UV coordinates. Two practical wins:
///   - A "corner" anchor (no curve) is just both deltas = 0.
///   - Dragging the anchor moves the handles automatically; no need to
///     recompute their positions each frame.
///
/// For closed contours the segment between consecutive anchors A and B is
/// a cubic Bezier with control points:
///   c0 = A.pos + A.outDelta
///   c1 = B.pos + B.inDelta
struct SdfAnchor
{
    float posU = 0.0f;
    float posV = 0.0f;
    float inDU = 0.0f;
    float inDV = 0.0f;
    float outDU = 0.0f;
    float outDV = 0.0f;
};

/// One contour. MVP only ships closed contours; `closed` is reserved so
/// schema v3 forward-compatibly carries an open-contour flag for Phase 11+.
struct SdfContour
{
    std::vector<SdfAnchor> anchors;
    bool closed = true;
};

} // namespace editor
