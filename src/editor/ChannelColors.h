#pragma once

#include "TextureDocument.h"

#include <array>
#include <cstdint>

namespace editor::channel_colors {

/// One RGB tint as floats 0..1, plus an "in use" flag.
/// Used both by the shader (filled into a constant buffer) and by the UI
/// (converted to QColor in TextureInfo.cpp).
struct Tint
{
    float r, g, b;
    bool  used;
};

/// Get the 3 RGB tints (plus a 4th for A — usually unused) for one texture kind.
/// Returned as an array indexed [0]=R, [1]=G, [2]=B, [3]=A.
///
/// Single source of truth for "what color does Base / Highlight / Shadow look
/// like in the UI and in the 3D mask overlay".
inline constexpr std::array<Tint, 4> tintsFor(TextureKind k)
{
    switch (k) {
    case TextureKind::ShadowRate:
        return {{
            { 1.00f, 0.78f, 0.30f, true  },   // R = Base (warm yellow)
            { 1.00f, 0.93f, 0.44f, true  },   // G = Highlight (bright yellow)
            { 0.29f, 0.42f, 1.00f, true  },   // B = Shadow (deep blue)
            { 0.25f, 0.26f, 0.29f, false },   // A = unused
        }};
    case TextureKind::SubLightRate:
        return {{
            { 1.00f, 0.55f, 0.26f, true  },   // R = SubLight1 (orange)
            { 0.29f, 0.82f, 0.91f, true  },   // G = SubLight2 (cyan)
            { 0.91f, 0.29f, 0.29f, true  },   // B = Cancel (red)
            { 0.25f, 0.26f, 0.29f, false },
        }};
    case TextureKind::EdgeRate:
        return {{
            { 0.26f, 0.84f, 0.80f, true  },   // R = Edge (teal)
            { 0.25f, 0.26f, 0.29f, false },
            { 0.25f, 0.26f, 0.29f, false },
            { 0.25f, 0.26f, 0.29f, false },
        }};
    case TextureKind::FaceSDF:
        return {{
            { 0.78f, 0.78f, 0.78f, true  },   // R = SDF (gray)
            { 0.78f, 0.78f, 0.78f, true  },   // G mirrors R
            { 0.78f, 0.78f, 0.78f, true  },   // B mirrors R
            { 0.25f, 0.26f, 0.29f, false },
        }};
    default:
        return {{ {1,1,1,false}, {1,1,1,false}, {1,1,1,false}, {1,1,1,false} }};
    }
}

} // namespace editor::channel_colors
