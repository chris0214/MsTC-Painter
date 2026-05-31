#pragma once

#include "TextureDocument.h"

#include <QColor>
#include <QString>

#include <array>

namespace editor {

/// Per-channel semantic info for one control texture.
struct ChannelInfo
{
    QChar    letter;      ///< 'R' / 'G' / 'B' / 'A'
    QColor   chipColor;   ///< swatch color shown in UI (matches semantic visualization)
    QString  shortLabel;  ///< short noun, e.g. "Base", "Highlight", "Shadow"
    QString  description; ///< 1-2 sentences explaining what painting white here does
    bool     used;        ///< false for unused channels (Edge G/B)
};

/// Per-texture-kind documentation, shown to artists in the editor UI.
struct TextureInfo
{
    QString  title;        ///< "Shadow Rate" etc. (display label)
    QString  filename;     ///< Suggested output filename ("shadow_rate.png")
    QString  tagline;      ///< 1-sentence summary of what this texture does in MS
    QString  artistTip;    ///< practical guidance for the artist
    std::array<ChannelInfo, 4> channels;  ///< R, G, B, A
};

/// Get info for a TextureKind. Stable, content read from N:\MS\渲染管线.md.
const TextureInfo& textureInfo(TextureKind k);

} // namespace editor
