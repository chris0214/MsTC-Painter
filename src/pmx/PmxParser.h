#pragma once

#include "PmxTypes.h"

#include <filesystem>
#include <string>

namespace pmx {

/// Parse a PMX 2.0 / 2.1 binary file.
/// Returns true on success; on failure returns false and sets `outError`.
///
/// Only the subset needed by msTC Texture Studio is parsed:
///   header, vertices, faces, texture paths, materials.
/// Bones / morphs / display frames / rigid bodies / joints / soft bodies
/// are skipped (we still advance the file pointer correctly so multiple
/// files parse without error).
bool loadModel(const std::filesystem::path& filepath,
               Model&                       outModel,
               std::string&                 outError);

} // namespace pmx
