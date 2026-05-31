#pragma once

#include "Layer.h"
#include "PixelTarget.h"
#include "TextureDocument.h"
#include "UndoHistory.h"

#include <memory>
#include <vector>

namespace editor {

/// A stack of layers + their composite cache, owned per (TextureGroup, TextureKind).
///
/// The composite cache IS a `TextureDocument` — that's intentional. Phase 7
/// downstream consumers (CanvasWidget, MaskTextureSet, TextureExport, Project
/// I/O) all read `TextureDocument`s. By keeping the LayerStack's output as a
/// TextureDocument we preserve their interfaces untouched: they just see the
/// merged result of all layers.
///
/// Mutations (add / remove / move / opacity-change / visibility-change /
/// brush stamp) are followed by a call to `recompositeRect()` or
/// `recompositeAll()` to refresh the composite. The dirty-tile mechanism on
/// TextureDocument propagates the change to Canvas + GPU consumers.
class LayerStack
{
public:
    /// Construct an empty stack with a fresh "Base" PaintLayer pre-added so
    /// the brush has something to write to immediately. `kind` is stored only
    /// so the composite TextureDocument knows what semantic name to expose;
    /// the math itself is kind-agnostic.
    LayerStack(TextureKind kind, int size);

    // ── Read access ───────────────────────────────────────────
    int       layerCount()  const { return static_cast<int>(m_layers.size()); }
    Layer*    layer(int i)        { return (i >= 0 && i < layerCount()) ? m_layers[i].get() : nullptr; }
    const Layer* layer(int i) const { return (i >= 0 && i < layerCount()) ? m_layers[i].get() : nullptr; }
    int       activeIndex() const { return m_active; }
    Layer*    activeLayer();
    /// Returns the brush/flood write target, or nullptr if active layer is
    /// non-paintable (FillLayer / ImageLayer / ChannelLayer).
    PixelTarget* activeTarget();
    TextureKind kind() const { return m_kind; }
    int         size() const { return m_size; }

    /// Read-only handle to the composite. Phase 7 sites that used to call
    /// `g.docs[k].get()` now go through this. The doc's dirty-tile machinery
    /// is the canvas/GPU sync mechanism.
    TextureDocument&       composite()       { return *m_composite; }
    const TextureDocument& composite() const { return *m_composite; }

    // ── Mutations ─────────────────────────────────────────────
    /// Append at the top of the stack. Returns the new layer's index.
    int  addLayer(std::unique_ptr<Layer> l);
    /// Insert at a specific index (0 = bottom). Clamped to [0, layerCount()].
    int  insertLayer(int at, std::unique_ptr<Layer> l);
    /// Remove. Refuses to remove the last remaining layer (UI guarantees a
    /// non-empty stack so the brush always has a target).
    bool removeLayer(int idx);
    /// Move layer at `from` to position `to`. Clamps both indices.
    bool moveLayer(int from, int to);
    void setActiveIndex(int idx);

    /// Merge `idx` (the higher layer) into `idx-1` (the layer below).
    /// The merged result becomes a single PaintLayer at index `idx-1`.
    /// Layer `idx` is removed. The combined layer's `wroteMask` is the
    /// union of the two layers' authored channels at each pixel; pixels
    /// are the result of compositing the two with their visibility +
    /// opacity + blend modes baked in. Returns false if idx <= 0 or out
    /// of range.
    bool mergeDown(int idx);

    // ── Compositor ────────────────────────────────────────────
    /// Recompose just the rect (typically a brush stamp's bounding box).
    void recompositeRect(const PixelRect& r);
    /// Recompose the entire texture — used after visibility/opacity changes
    /// or layer reorders.
    void recompositeAll();

    // ── Undo/Redo (Phase 9) ───────────────────────────────────
    /// Per-stack undo/redo history. Each stack has its own — Ctrl+Z affects
    /// only the currently active (group, kind) tab.
    UndoHistory&       history()       { return m_history; }
    const UndoHistory& history() const { return m_history; }

    /// Locate a layer by stable ID (Layer::id()). Returns nullptr if no
    /// layer with that ID exists in the stack — typically because the user
    /// deleted or merged it after the undo entry was recorded.
    Layer*       findLayerById(uint64_t id);
    const Layer* findLayerById(uint64_t id) const;

private:
    /// Composite one pixel in place into `outRgba` (modifies outRgba and
    /// runs through every visible layer top-down).
    void compositePixel(int x, int y, uint8_t outRgba[4]) const;

    TextureKind                          m_kind;
    int                                  m_size;
    std::vector<std::unique_ptr<Layer>>  m_layers;   ///< [0] = bottom
    int                                  m_active = 0;
    std::unique_ptr<TextureDocument>     m_composite;
    UndoHistory                          m_history;
};

} // namespace editor
