#pragma once

#include "../editor/TextureDocument.h"
#include "../editor/TextureGroup.h"

#include <DirectXMath.h>

#include <filesystem>
#include <string>

namespace project {

/// Format version. Bump on incompatible JSON-schema changes.
///   v1 — Phase 7: per-(group, kind) single mask in `masks/` directory.
///   v2 — Phase 8: per-layer storage. Each kind has a `layerStacks` JSON
///        array listing each layer's type/name/visible/opacity/blend +
///        sidecar data path (`.bin` for paint/image/channel; selection mask
///        for fill is stored inline in JSON if present).
///   v3 — Phase 10 v3: adds `faceSdf` layer type with inline JSON contour
///        list + sidecar cached raster. Other layer types unchanged.
///
/// readProjectHeader accepts v1, v2, and v3; saveProject only writes the
/// latest. On load, v1 projects materialize a single PaintLayer per kind
/// (matching what the user saw in Phase 7).
constexpr int kProjectVersion = 3;

/// Persistent state of the editor outside the mask pixel data.
/// Anything in the UI worth restoring on reopen lives here.
struct ProjectMeta
{
    std::filesystem::path modelPath;        ///< stored as a path RELATIVE to the .mstcproj file
    int                   textureSize = 2048;

    // ── UI state ───────────────────────────────────────────────
    int                   activeGroupIndex = 0;
    editor::TextureKind   activeKind       = editor::TextureKind::ShadowRate;
    int                   channelView      = 0;  ///< 0=RGB, 1/2/3=R/G/B, 4=Semantic
    int                   viewMode         = 1;  ///< matches ViewMode enum (1 = MaskOverlay)
    bool                  autoSwitchGroup  = true;

    // ── Camera ─────────────────────────────────────────────────
    DirectX::XMFLOAT3     cameraTarget   { 0, 0, 0 };
    float                 cameraYaw      = 0.0f;
    float                 cameraPitch    = 0.2f;
    float                 cameraDistance = 30.0f;
};

/// Read just the header (version, modelPath, textureSize, UI/camera state) from a
/// .mstcproj file without touching any mask pixels. Lets the caller open the PMX
/// and build groups at the correct size BEFORE we restore mask pixels.
bool readProjectHeader(const std::filesystem::path& projectPath,
                       ProjectMeta&                 outMeta,
                       std::string&                 outError);

/// Restore mask pixel data into an already-built TextureGroupSet. The caller
/// must have built `groups` at `meta.textureSize` (typically by calling
/// readProjectHeader first, then openPmx, then this).
bool loadProjectMasks(const std::filesystem::path& projectPath,
                      editor::TextureGroupSet&     groups,
                      const ProjectMeta&           meta,
                      std::string&                 outError);

/// Save a project: writes the .mstcproj JSON header + a sibling
/// "<basename>.mstcproj.dir/masks/" directory with raw RGBA8 dumps for each
/// modified mask document. Untouched docs (everModified() == false) are
/// skipped — their absence is inferred as "default state" on load.
bool saveProject(const std::filesystem::path& projectPath,
                 const ProjectMeta&             meta,
                 const editor::TextureGroupSet& groups,
                 std::string&                   outError);

} // namespace project
