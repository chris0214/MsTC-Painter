#pragma once

#include "PixelTarget.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace editor {

/// Per-layer compositing operator.
///
/// v1 implements only `Normal` (per-channel replace + opacity-weighted lerp
/// with whatever the lower layers have so far resolved). The other modes are
/// reserved so the file format / UI can carry them across versions; the
/// compositor falls back to Normal and warns once if an unimplemented mode
/// is encountered.
enum class BlendMode : uint8_t
{
    Normal   = 0,
    Multiply = 1,   ///< not implemented in v1
    Screen   = 2,   ///< not implemented in v1
};

const char* blendModeName(BlendMode m);
bool        blendModeFromName(const std::string& s, BlendMode& out);

/// Type tag — used by serializer + LayerPanel to dispatch.
enum class LayerType : uint8_t
{
    Paint   = 0,
    Fill    = 1,
    Image   = 2,
    Channel = 3,
};

const char* layerTypeName(LayerType t);
bool        layerTypeFromName(const std::string& s, LayerType& out);

/// Abstract base.
///
/// Per-layer composite contribution is sampled point-by-point: the LayerStack
/// walks layers TOP-down, asks each for `(rgba, channelMask)` at (x,y), and
/// merges the result into a running composite. A layer reports `channelMask`
/// = 0 to mean "I'm transparent at this pixel; skip me entirely".
class Layer
{
public:
    Layer();
    virtual ~Layer() = default;

    /// Stable, process-unique ID assigned at construction. Used by Phase 9
    /// undo to identify the target layer even after the user reorders or
    /// deletes layers between the recorded action and the undo. Not
    /// persisted across project save/load — undo history is also not
    /// persisted, so the IDs only need to be unique within one session.
    uint64_t id() const { return m_id; }

    /// Sample contribution at (x, y) into outRgba (4 bytes). outChannels is
    /// a bitmask of which RGBA channels this layer authored at that pixel.
    /// PaintLayer's mask is per-pixel (its `wroteMask`); FillLayer/ImageLayer
    /// return their global channelMask uniformly.
    virtual void sample(int x, int y,
                        uint8_t outRgba[4],
                        ChannelAuthorMask& outChannels) const = 0;

    /// LayerType for serialization + UI dispatch.
    virtual LayerType type() const = 0;

    /// Brush + flood fill target. Only PaintLayer overrides.
    virtual PixelTarget* asPixelTarget() { return nullptr; }
    virtual const PixelTarget* asPixelTarget() const { return nullptr; }

    // ── Common metadata ────────────────────────────────────────
    std::string name;
    bool        visible    = true;
    float       opacity    = 1.0f;
    BlendMode   blendMode  = BlendMode::Normal;

private:
    uint64_t    m_id;   ///< assigned in ctor from a static atomic counter
};

/// The brush / flood-fill target. Per-pixel RGBA8 + per-pixel "wroteMask"
/// recording which channels the user has actually authored here.
///
/// `wroteMask`'s low 4 bits correspond to R, G, B, A. A bit set = "this layer
/// has explicit data on this channel; the compositor should take it." A bit
/// clear = "fall through to the layer below for this channel."
///
/// Memory: size² × (4 + 1) = 5 N² bytes. 2048² = 20 MB / layer.
class PaintLayer : public Layer, public PixelTarget
{
public:
    explicit PaintLayer(int size);

    LayerType type() const override { return LayerType::Paint; }
    PixelTarget*       asPixelTarget()       override { return this; }
    const PixelTarget* asPixelTarget() const override { return this; }

    void sample(int x, int y,
                uint8_t outRgba[4],
                ChannelAuthorMask& outChannels) const override;

    // PixelTarget — invoked by BrushEngine / FloodFill / merge ops.
    int  size() const override { return m_size; }
    void getPixel(int x, int y, uint8_t out[4]) const override;
    void setPixel(int x, int y,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                  ChannelAuthorMask channelsAuthored) override;

    // ── Direct buffer access (for serialization + merge) ───────
    const uint8_t*       pixels()    const { return m_rgba.data();  }
    uint8_t*             pixelsMut()       { return m_rgba.data();  }
    const uint8_t*       wroteMask() const { return m_wrote.data(); }
    uint8_t*             wroteMaskMut()    { return m_wrote.data(); }

    /// Bulk overwrite (used by project loader).
    void setAll(const uint8_t* rgbaSize2x4, const uint8_t* wroteSize2);

    /// Clear everything to (0,0,0,255) + wroteMask=0 (fully transparent).
    void clear();

private:
    int                  m_size;
    std::vector<uint8_t> m_rgba;       ///< size² × 4
    std::vector<uint8_t> m_wrote;      ///< size², low 4 bits per byte
};

/// Constant fill: every pixel returns the same RGBA × channelMask.
/// Optional per-pixel selection mask restricts where the fill applies.
class FillLayer : public Layer
{
public:
    FillLayer(int size,
              uint8_t r, uint8_t g, uint8_t b, uint8_t a,
              ChannelAuthorMask channelMask);

    LayerType type() const override { return LayerType::Fill; }
    void sample(int x, int y,
                uint8_t outRgba[4],
                ChannelAuthorMask& outChannels) const override;

    /// Set a size² selection bitmap (1 = fill here, 0 = skip). Pass nullptr
    /// or empty to remove restriction (fill everywhere).
    void setSelection(std::vector<uint8_t> sel);
    bool hasSelection() const { return !m_selection.empty(); }
    const std::vector<uint8_t>& selection() const { return m_selection; }

    uint8_t r() const { return m_rgba[0]; }
    uint8_t g() const { return m_rgba[1]; }
    uint8_t b() const { return m_rgba[2]; }
    uint8_t a() const { return m_rgba[3]; }
    ChannelAuthorMask channelMask() const { return m_channelMask; }

    void setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void setChannelMask(ChannelAuthorMask m) { m_channelMask = m; }

    int size() const { return m_size; }

private:
    int                  m_size;
    uint8_t              m_rgba[4];
    ChannelAuthorMask    m_channelMask;
    std::vector<uint8_t> m_selection;   ///< size² 0/1, empty = "everywhere"
};

/// External RGBA8 image, resampled (nearest-neighbor) to `size` × `size`.
/// Used for "drop a reference shadow map in" workflows.
class ImageLayer : public Layer
{
public:
    /// `srcRgba` must be `srcSize × srcSize × 4`. Resampled to `targetSize`
    /// during construction.
    ImageLayer(int targetSize,
               const uint8_t* srcRgba, int srcSize,
               ChannelAuthorMask channelMask);

    LayerType type() const override { return LayerType::Image; }
    void sample(int x, int y,
                uint8_t outRgba[4],
                ChannelAuthorMask& outChannels) const override;

    int                size() const { return m_size; }
    const uint8_t*     pixels() const { return m_rgba.data(); }
    ChannelAuthorMask  channelMask() const { return m_channelMask; }

private:
    int                  m_size;
    std::vector<uint8_t> m_rgba;       ///< size² × 4
    ChannelAuthorMask    m_channelMask;
};

/// Copies one channel of a source image into one channel of the target stack.
/// Only writes to `dstChannel`; reports that channel as authored, all other
/// channels fall through.
class ChannelLayer : public Layer
{
public:
    /// `srcRgba` is shared with potentially multiple ChannelLayers reading
    /// from the same source image (saves memory).
    ChannelLayer(int targetSize,
                 std::shared_ptr<std::vector<uint8_t>> srcRgba,
                 int srcSize,
                 uint8_t srcChannel,
                 uint8_t dstChannel);

    LayerType type() const override { return LayerType::Channel; }
    void sample(int x, int y,
                uint8_t outRgba[4],
                ChannelAuthorMask& outChannels) const override;

    int     size()        const { return m_size; }
    int     srcSize()     const { return m_srcSize; }
    uint8_t srcChannel()  const { return m_srcChannel; }
    uint8_t dstChannel()  const { return m_dstChannel; }
    const std::shared_ptr<std::vector<uint8_t>>& srcImage() const { return m_src; }

private:
    int                                       m_size;
    std::shared_ptr<std::vector<uint8_t>>     m_src;
    int                                       m_srcSize;
    uint8_t                                   m_srcChannel;
    uint8_t                                   m_dstChannel;
};

} // namespace editor
