#pragma once

#include "LayerStack.h"
#include "TextureDocument.h"
#include "UvLines.h"

#include "../pmx/PmxTypes.h"

#include <QImage>
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace editor {

/// A "texture group" = exactly ONE PMX material's editable canvas.
///
/// We deliberately keep this 1:1 with the model's material list, even when
/// several materials share the same diffuse atlas (a face PMX often has 8
/// materials all sampling head.png). msToonCoordinator's `.fx` is bound
/// per-material — each material reads its OWN ShadowRate/SubLightRate/
/// EdgeRate/FaceSDF — so anything coarser than per-material would force
/// independent looks (e.g. eyebrow vs. cheek) to bleed into one mask.
///
/// The trade-off: many groups with nominally-shared diffuses load the same
/// reference image multiple times; `buildTextureGroups` deduplicates that
/// load via a cache, but each group still owns four full-resolution mask
/// documents, so VRAM scales with materialCount × 4 × size².
///
/// Materials with no diffuse still get a group (they need editable masks
/// too); they just have an empty `diffuse` — the canvas falls back to
/// checker.
struct TextureGroup
{
    int          diffuseTextureIndex = -1;   ///< -1 for "no texture" sentinel
    std::string  name;                       ///< material's JP name (or EN fallback)
    std::string  diffuseName;                ///< filename stem of the diffuse, "" if none
    std::vector<int> materialIndices;         ///< always size 1 — the PMX material this group represents
    QImage       diffuse;                    ///< shared diffuse, used as canvas background (may be empty)

    /// One LayerStack per TextureKind. Each stack owns its own composite
    /// TextureDocument that downstream consumers (canvas, GPU mask, project
    /// I/O, PNG export) read. The brush writes to the stack's active layer
    /// (typically a PaintLayer), not directly to the composite.
    ///
    /// Phase 7 used `std::array<unique_ptr<TextureDocument>, Count> docs`
    /// directly. Phase 8 wraps each in a LayerStack; the helper `doc(kind)`
    /// returns the composite for legacy call sites.
    std::array<std::unique_ptr<LayerStack>,
               static_cast<size_t>(TextureKind::Count)> stacks;

    /// Convenience for downstream code that wants the composite for a kind.
    /// Returns nullptr if the stack hasn't been allocated (shouldn't happen
    /// after `buildTextureGroups` but guard anyway).
    TextureDocument* doc(TextureKind k) {
        const size_t i = static_cast<size_t>(k);
        return stacks[i] ? &stacks[i]->composite() : nullptr;
    }
    const TextureDocument* doc(TextureKind k) const {
        const size_t i = static_cast<size_t>(k);
        return stacks[i] ? &stacks[i]->composite() : nullptr;
    }
    /// Same, indexed by raw size_t (for loops over `static_cast<size_t>(kind)`).
    TextureDocument* doc(size_t k) {
        return stacks[k] ? &stacks[k]->composite() : nullptr;
    }
    const TextureDocument* doc(size_t k) const {
        return stacks[k] ? &stacks[k]->composite() : nullptr;
    }

    /// UV segments that belong to this group's materials, stored as a single
    /// flattened buffer (concatenation of each material's wireframe). The
    /// canvas reads `uvWireframe.segs[uvSegmentStart .. uvSegmentStart+uvSegmentCount]`.
    int uvSegmentStart = 0;
    int uvSegmentCount = 0;

    /// Rasterized UV coverage mask, size×size 0/1 bytes. Used by the bucket
    /// tool to constrain flood fill to this group's actual triangles
    /// (i.e. "don't leak into atlas whitespace").
    std::vector<uint8_t> uvCoverage;
    int                  uvCoverageSize = 0;  ///< == doc(0)->size()
};

/// Result of grouping a model's materials by diffuse texture, plus a flattened
/// UV wireframe whose segment ranges align with each group's `uvSegmentStart`.
struct TextureGroupSet
{
    std::vector<TextureGroup> groups;
    UvWireframe               wireframe;     ///< segs concatenated per group order
};

/// Build per-material control texture documents from a PMX model.
///
///   - One TextureGroup per PMX material — they map 1:1 onto the material list.
///   - Materials sharing a diffuse texture each get their own copy of the same
///     QImage (the loader caches loads internally, but each group still has
///     its own mask documents — that's the whole point of doing this per-mat).
///   - Materials with `diffuseTextureIndex == -1` still get a group, just
///     with an empty diffuse.
///   - Each group gets a freshly-allocated empty TextureDocument for each kind.
///   - `wireframe.segs` is a concatenation: groups[0]'s segments first, then
///     groups[1]'s, etc. Each TextureGroup records its [start, count) into segs.
///   - `wireframe.groups` (the legacy material-level groups inside UvWireframe)
///     is left empty — we use TextureGroup-level ranges instead.
TextureGroupSet buildTextureGroups(
    const pmx::Model&            model,
    const std::filesystem::path& modelDir,
    int                          textureSize);

} // namespace editor
