#pragma once

#include "BrushEngine.h"
#include "FloodFill.h"
#include "LayerStack.h"
#include "TextureDocument.h"
#include "UvLines.h"

#include <QImage>
#include <QPixmap>
#include <QPointF>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <vector>

namespace editor {

/// Tools the canvas can be in.
enum class CanvasTool : uint8_t
{
    Brush = 0,
    Bucket,
    Eyedropper,
};

/// How channels of the underlying TextureDocument should be displayed in the
/// 2D canvas. Brushing always writes to the *underlying* RGBA — these modes
/// only change visualization.
enum class ChannelView : uint8_t
{
    RGB = 0,    ///< RGB composite (alpha forced to 255)
    R,          ///< red channel as grayscale
    G,          ///< green channel as grayscale
    B,          ///< blue channel as grayscale
    Semantic,   ///< per-kind colored visualization
};

const char* channelViewName(ChannelView v);

/// Qt widget that renders a TextureDocument as a 2D image, with optional
/// UV-wireframe overlay (filtered to a sub-range of segments), zoom/pan,
/// channel view, and brush input.
class CanvasWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CanvasWidget(QWidget* parent = nullptr);
    ~CanvasWidget() override;

    /// Bind to a document (non-owning). Pass nullptr to detach.
    void setDocument(TextureDocument* doc);
    TextureDocument* document() const { return m_doc; }

    /// Bind to a LayerStack (non-owning). When set, brush + flood writes go
    /// to the stack's active layer instead of directly to the document, and
    /// the stack is told to recomposite after each write. Pass nullptr to
    /// detach (the canvas falls back to legacy "write straight to doc"
    /// behavior, used in tests / pre-PMX states).
    void setLayerStack(LayerStack* s) { m_stack = s; }
    LayerStack* layerStack() const { return m_stack; }

    /// Replace the entire UV segment buffer (typically from TextureGroupSet).
    /// The canvas does not own this data — the host should call setUvSegmentRange
    /// after to pick which slice to draw.
    void setUvSegments(std::vector<UvSegment> segments);

    /// Pick which sub-range of the segments to draw. Use to filter to a
    /// single TextureGroup. Defaults to "everything" (start=0, count=all).
    void setUvSegmentRange(int start, int count);

    /// Set a background diffuse texture (drawn under the mask overlay).
    /// Pass a null QImage to remove the background.
    void setBackgroundImage(QImage img);

    /// Toggle UV wireframe rendering.
    void setShowUvWireframe(bool on);
    bool showUvWireframe() const { return m_showUv; }

    /// Set channel view mode.
    void setChannelView(ChannelView v);
    ChannelView channelView() const { return m_channelView; }

    /// Reset zoom/pan so the texture fits the widget.
    void fitToView();

    /// Bind a brush engine (non-owning).
    void setBrush(BrushEngine* brush) { m_brush = brush; }
    BrushEngine* brush() const        { return m_brush; }

    /// Tool selection. Affects what a left-click does.
    void setTool(CanvasTool t);
    CanvasTool tool() const { return m_tool; }

    /// Bucket settings (tolerance + scope live with the canvas, not the brush,
    /// because they're tool-specific).
    void setFillTolerance(int t)     { m_fillTolerance = t; }
    void setFillScope(FillScope s)   { m_fillScope = s; }
    int  fillTolerance() const       { return m_fillTolerance; }
    FillScope fillScope() const      { return m_fillScope; }

    /// Pass the active TextureGroup's UV coverage mask (size×size 0/1 bytes).
    /// Required by Bucket + UV-bound modes. Pass nullptr to detach.
    void setUvCoverage(const uint8_t* mask, int size);

signals:
    /// Emitted continuously while the cursor is over the canvas.
    void cursorUvChanged(QPointF uv);

    /// Emitted at endStroke / fill / drop so the host can refresh dependents.
    void strokeFinished();

    /// Emitted when the eyedropper picked a value. The host can update UI
    /// (e.g. BrushPanel "Ink" swatch).
    void inkPicked(int channelIndex, uint8_t value);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;

    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void leaveEvent(QEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    void rebuildDisplay(const PixelRect& rect);
    void rebuildUvCache();         ///< (re-)rasterize the active material group
    void syncDirtyToDisplay();

    /// Pick the current write target for brush/flood. Returns the active
    /// layer's PixelTarget when bound to a stack; otherwise the doc itself
    /// (which still implements PixelTarget for legacy single-layer cases
    /// like tests). Returns nullptr only if neither is bound.
    PixelTarget* paintTarget();

    /// Active PaintLayer, or nullptr if no stack bound or active layer is
    /// non-paintable. Used by undo recording to read pre/post state.
    PaintLayer*  activePaintLayer();

    /// Tell the bound LayerStack (if any) to recomposite the rect just
    /// touched by a brush stamp, then reset m_pendingDirtyBox.
    void flushPendingComposite();

    QPointF widgetToUv(QPointF widgetPt) const;
    QPointF uvToWidget(QPointF uv) const;

    TextureDocument* m_doc = nullptr;
    BrushEngine*     m_brush = nullptr;
    LayerStack*      m_stack = nullptr;   ///< non-owning; nullable

    QImage  m_displayImage;          ///< mask overlay with channel-derived alpha
    QImage  m_backgroundImage;       ///< optional diffuse texture under the mask
    QPixmap m_uvCache;               ///< rasterized UV wireframe at texture resolution
    bool    m_uvCacheDirty = true;

    UvWireframe m_uvData;
    int    m_uvRangeStart = 0;
    int    m_uvRangeCount = -1;       ///< -1 = all
    bool   m_showUv = true;
    ChannelView m_channelView = ChannelView::RGB;

    double m_zoom = 1.0;
    QPointF m_pan { 0, 0 };

    enum class Drag { None, Pan, Paint };
    Drag    m_drag = Drag::None;
    QPointF m_dragStart { 0, 0 };

    /// Current tool. Default Brush.
    CanvasTool m_tool = CanvasTool::Brush;

    /// Bucket settings.
    int       m_fillTolerance = 32;
    FillScope m_fillScope     = FillScope::ColorAndUv;

    /// UV coverage mask (non-owning) for the active group. nullptr if no model
    /// loaded or coverage not built.
    const uint8_t* m_uvCoverage = nullptr;
    int            m_uvCoverageSize = 0;

    /// Last UV coord at which a stroke ended. Used by Shift+click line tool.
    /// `(-1, -1)` means "no previous stroke yet".
    QPointF m_lastStrokeEndUv { -1, -1 };

    /// Stroke origin (UV), used for Shift-drag 15° angle snapping.
    QPointF m_strokeStartUv { 0, 0 };

    /// Accumulator passed to BrushEngine/FloodFill for the union of texel
    /// rects modified during the current operation. In Step 1 this is just
    /// a sink — Step 2 will forward it to LayerStack::recompositeRect.
    PixelRect m_pendingDirtyBox{};

    /// Union of every stamp's dirty box across a whole stroke (begin → end).
    /// Tracked separately from m_pendingDirtyBox because the latter is reset
    /// after every stamp's recompositeRect call. Used by Phase 9 undo to
    /// snapshot only the tiles actually touched by the stroke.
    PixelRect m_strokeUnionBox{};

    /// True until we've successfully fit-to-viewed at a reasonable widget size.
    /// Initially set when a document is bound; cleared after a real fit happens.
    bool    m_pendingFit = false;

    /// Set when a new document is bound; cleared on the first showEvent that
    /// follows. The first time a hidden tab page actually becomes visible, the
    /// QTabWidget gives it its real size — we use that opportunity to redo
    /// the initial fit (since the one done at bind time was against the
    /// tab's pre-show layout size).
    bool    m_needsInitialFit = false;

    // Cached checkerboard pattern (shared style across all canvases via static)
    static QPixmap checkerboardPattern();
};

} // namespace editor
