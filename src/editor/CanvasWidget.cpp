#include "CanvasWidget.h"

#include "FloodFill.h"

#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace editor {

const char* channelViewName(ChannelView v)
{
    switch (v) {
    case ChannelView::RGB:      return "RGB";
    case ChannelView::R:        return "R";
    case ChannelView::G:        return "G";
    case ChannelView::B:        return "B";
    case ChannelView::Semantic: return "Semantic";
    default:                    return "?";
    }
}

// ─────────────────────────────────────────────────────────────────
// Static helpers
// ─────────────────────────────────────────────────────────────────

QPixmap CanvasWidget::checkerboardPattern()
{
    static QPixmap pattern;
    if (pattern.isNull()) {
        constexpr int cell = 8;
        QPixmap p(cell * 2, cell * 2);
        p.fill(QColor(60, 60, 60));
        QPainter cp(&p);
        cp.fillRect(0,    0,    cell, cell, QColor(80, 80, 80));
        cp.fillRect(cell, cell, cell, cell, QColor(80, 80, 80));
        pattern = p;
    }
    return pattern;
}

// ─────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────

CanvasWidget::CanvasWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(200, 200);
    setAutoFillBackground(false);
}

CanvasWidget::~CanvasWidget() = default;

// ─────────────────────────────────────────────────────────────────
// Document binding
// ─────────────────────────────────────────────────────────────────

void CanvasWidget::setDocument(TextureDocument* doc)
{
    m_doc = doc;
    if (doc) {
        m_displayImage = QImage(doc->size(), doc->size(), QImage::Format_RGBA8888);
        m_displayImage.fill(Qt::black);
        rebuildDisplay({ 0, 0, doc->size(), doc->size() });
        doc->takeDirtyBounds();
        m_uvCacheDirty = true;

        // Mark this widget for a fit on first show. Hidden QTabWidget pages
        // get a pre-show layout size that is NOT their final visible size
        // (they may pass the "usable" threshold but still be smaller than
        // what the user will eventually see), so we wait for the first real
        // showEvent before fitting.
        m_needsInitialFit = true;
        m_pendingFit = true;

        // Still try once now in case we're already visible (active tab on
        // startup). If not visible yet, showEvent will redo it.
        fitToView();
    } else {
        m_displayImage = QImage();
        m_uvCache = QPixmap();
    }
    update();
}

void CanvasWidget::setUvSegments(std::vector<UvSegment> segs)
{
    m_uvData.segs = std::move(segs);
    m_uvData.groups.clear();
    m_uvRangeStart = 0;
    m_uvRangeCount = -1;
    m_uvCacheDirty = true;
    update();
}

void CanvasWidget::setUvSegmentRange(int start, int count)
{
    if (m_uvRangeStart == start && m_uvRangeCount == count) return;
    m_uvRangeStart = start;
    m_uvRangeCount = count;
    m_uvCacheDirty = true;
    update();
}

void CanvasWidget::setBackgroundImage(QImage img)
{
    m_backgroundImage = std::move(img);
    update();
}

void CanvasWidget::setTool(CanvasTool t)
{
    if (m_tool == t) return;
    m_tool = t;
    // Reset cursor — paintEvent draws tool-specific overlays
    switch (t) {
    case CanvasTool::Brush:      setCursor(Qt::ArrowCursor);  break;
    case CanvasTool::Bucket:     setCursor(Qt::PointingHandCursor); break;
    case CanvasTool::Eyedropper: setCursor(Qt::CrossCursor);  break;
    }
    update();
}

void CanvasWidget::setUvCoverage(const uint8_t* mask, int size)
{
    m_uvCoverage = mask;
    m_uvCoverageSize = size;
}

void CanvasWidget::setShowUvWireframe(bool on)
{
    if (m_showUv == on) return;
    m_showUv = on;
    update();
}

void CanvasWidget::setChannelView(ChannelView v)
{
    if (m_channelView == v) return;
    m_channelView = v;
    if (m_doc) rebuildDisplay({ 0, 0, m_doc->size(), m_doc->size() });
    update();
}

void CanvasWidget::fitToView()
{
    if (!m_doc) return;

    // If the widget hasn't been laid out at a usable size yet (e.g. it's a
    // hidden QTabWidget page), defer the fit. showEvent / resizeEvent will
    // call us again once the size is real.
    constexpr int kMinUsable = 100;
    if (width() < kMinUsable || height() < kMinUsable) {
        m_pendingFit = true;
        return;
    }

    const double w = width();
    const double h = height();
    const double s = static_cast<double>(m_doc->size());
    const double zx = (w - 8.0) / s;
    const double zy = (h - 8.0) / s;
    m_zoom = std::min(zx, zy);
    if (m_zoom <= 0.0) m_zoom = 1.0;

    const double drawnW = s * m_zoom;
    const double drawnH = s * m_zoom;
    m_pan = QPointF((w - drawnW) * 0.5, (h - drawnH) * 0.5);

    m_pendingFit = false;
    update();
}

// ─────────────────────────────────────────────────────────────────
// Display rebuild
// ─────────────────────────────────────────────────────────────────

void CanvasWidget::rebuildDisplay(const PixelRect& rect)
{
    if (!m_doc || rect.isEmpty()) return;

    const int N = m_doc->size();
    const uint8_t* src = m_doc->pixels();

    const int x0 = std::clamp(rect.minX, 0, N);
    const int y0 = std::clamp(rect.minY, 0, N);
    const int x1 = std::clamp(rect.maxX, 0, N);
    const int y1 = std::clamp(rect.maxY, 0, N);

    // The display image is an *overlay* drawn on top of the diffuse background.
    // The alpha channel encodes "how visible this pixel is":
    //   intensity 0   → alpha 0     (fully transparent — show background)
    //   intensity 255 → alpha 255   (fully opaque — show mask color)
    //
    // IMPORTANT: RGB is stored "full saturation + alpha as coverage", NOT
    // alpha-premultiplied. That way a brush edge with G=128 displays as
    // "half-coverage of bright green" rather than "full coverage of dim
    // green". The latter mixes with the diffuse background to produce a
    // muddy/dark halo around every brush stamp.
    //
    // For the single-channel views (R/G/B) we just show that one channel as
    // grayscale with alpha = its own intensity.

    auto kindTint = [&](uint8_t r, uint8_t g, uint8_t b,
                        uint8_t& outR, uint8_t& outG, uint8_t& outB,
                        uint8_t& outA)
    {
        // Compute the tinted color as before, then normalize to full
        // saturation so brush-edge falloff doesn't darken the result.
        int tR, tG, tB;
        switch (m_doc->kind()) {
        case TextureKind::ShadowRate:
            tR = std::min(255, int(r) * 255 / 255 + int(g));
            tG = std::min(255, int(r) * 220 / 255 + int(g) * 235 / 255);
            tB = std::min(255, int(b) * 255 / 255);
            break;
        case TextureKind::SubLightRate:
            tR = std::min(255, int(r) * 240 / 255 + int(b));
            tG = std::min(255, int(r) * 140 / 255 + int(g) * 230 / 255);
            tB = std::min(255, int(g) * 230 / 255);
            break;
        case TextureKind::EdgeRate:
            tR = 80;
            tG = std::min(255, int(r) + 40);
            tB = std::min(255, int(r) + 60);
            break;
        case TextureKind::FaceSDF:
            tR = tG = tB = r;
            break;
        default:
            tR = r; tG = g; tB = b;
            break;
        }
        outA = static_cast<uint8_t>(std::max({int(r), int(g), int(b)}));
        const int peak = std::max({tR, tG, tB});
        if (peak > 0) {
            // Scale tinted color up so the brightest component is 255,
            // preserving channel ratios. Coverage moves into outA.
            outR = static_cast<uint8_t>(std::min(255, tR * 255 / peak));
            outG = static_cast<uint8_t>(std::min(255, tG * 255 / peak));
            outB = static_cast<uint8_t>(std::min(255, tB * 255 / peak));
        } else {
            outR = outG = outB = 0;
        }
    };

    for (int y = y0; y < y1; ++y) {
        uint8_t* dst = m_displayImage.scanLine(y);
        const uint8_t* row = src + (static_cast<size_t>(y) * N + x0) * 4;
        for (int x = x0; x < x1; ++x) {
            const uint8_t r = row[0];
            const uint8_t g = row[1];
            const uint8_t b = row[2];
            uint8_t* p = dst + x * 4;
            switch (m_channelView) {
            case ChannelView::RGB: {
                // "Full color + alpha as coverage" semantics: scale RGB so
                // its peak is 255 and stash the original peak as alpha. This
                // makes brush-edge falloff fade smoothly to the background
                // instead of darkening it.
                const uint8_t alpha =
                    static_cast<uint8_t>(std::max({int(r), int(g), int(b)}));
                if (alpha > 0) {
                    p[0] = static_cast<uint8_t>(std::min(255, int(r) * 255 / alpha));
                    p[1] = static_cast<uint8_t>(std::min(255, int(g) * 255 / alpha));
                    p[2] = static_cast<uint8_t>(std::min(255, int(b) * 255 / alpha));
                } else {
                    p[0] = p[1] = p[2] = 0;
                }
                p[3] = alpha;
                break;
            }
            case ChannelView::R:
                p[0] = 255; p[1] = 80;  p[2] = 80;  p[3] = r;  // red-tinted, alpha = R
                break;
            case ChannelView::G:
                p[0] = 80;  p[1] = 255; p[2] = 80;  p[3] = g;  // green-tinted
                break;
            case ChannelView::B:
                p[0] = 100; p[1] = 140; p[2] = 255; p[3] = b;  // blue-tinted
                break;
            case ChannelView::Semantic:
                kindTint(r, g, b, p[0], p[1], p[2], p[3]);
                break;
            }
            row += 4;
        }
    }
}

void CanvasWidget::syncDirtyToDisplay()
{
    if (!m_doc || !m_doc->hasDirtyTiles()) return;
    rebuildDisplay(m_doc->takeDirtyBounds());
}

// ─────────────────────────────────────────────────────────────────
// UV cache rasterization
// ─────────────────────────────────────────────────────────────────

void CanvasWidget::rebuildUvCache()
{
    m_uvCacheDirty = false;

    if (!m_doc) {
        m_uvCache = QPixmap();
        return;
    }

    const int N = m_doc->size();
    QPixmap pix(N, N);
    pix.fill(Qt::transparent);

    if (m_uvData.segs.empty()) {
        m_uvCache = std::move(pix);
        return;
    }

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(QColor(255, 220, 0, 140));
    pen.setWidthF(1.0);
    pen.setCosmetic(true);
    p.setPen(pen);

    // Painter is in pixel space; UV (0..1) needs scaling by N.
    p.scale(N, N);

    // Determine the segment sub-range to draw
    int segStart = m_uvRangeStart;
    int segCount = m_uvRangeCount;
    if (segCount < 0) segCount = static_cast<int>(m_uvData.segs.size()) - segStart;
    if (segStart < 0) segStart = 0;
    if (segStart + segCount > static_cast<int>(m_uvData.segs.size())) {
        segCount = static_cast<int>(m_uvData.segs.size()) - segStart;
    }
    if (segCount <= 0) {
        m_uvCache = std::move(pix);
        return;
    }

    const auto* base = m_uvData.segs.data() + segStart;
    for (int i = 0; i < segCount; ++i) {
        p.drawLine(base[i].a, base[i].b);
    }

    m_uvCache = std::move(pix);
}

// ─────────────────────────────────────────────────────────────────
// View transforms
// ─────────────────────────────────────────────────────────────────

QPointF CanvasWidget::widgetToUv(QPointF widgetPt) const
{
    if (!m_doc) return { -1, -1 };
    const double s = static_cast<double>(m_doc->size());
    const double texX = (widgetPt.x() - m_pan.x()) / m_zoom;
    const double texY = (widgetPt.y() - m_pan.y()) / m_zoom;
    return QPointF(texX / s, texY / s);
}

QPointF CanvasWidget::uvToWidget(QPointF uv) const
{
    if (!m_doc) return { 0, 0 };
    const double s = static_cast<double>(m_doc->size());
    return QPointF(m_pan.x() + uv.x() * s * m_zoom,
                   m_pan.y() + uv.y() * s * m_zoom);
}

// ─────────────────────────────────────────────────────────────────
// Painting
// ─────────────────────────────────────────────────────────────────

void CanvasWidget::paintEvent(QPaintEvent*)
{
    syncDirtyToDisplay();
    if (m_uvCacheDirty) rebuildUvCache();

    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.setRenderHint(QPainter::Antialiasing, false);

    // Background
    p.fillRect(rect(), QColor(35, 38, 45));

    if (!m_doc || m_displayImage.isNull()) {
        p.setPen(QColor(120, 120, 120));
        p.drawText(rect(), Qt::AlignCenter, "No texture loaded");
        return;
    }

    const double s = static_cast<double>(m_doc->size());
    QRectF dstRect(m_pan.x(), m_pan.y(), s * m_zoom, s * m_zoom);

    // Layer 1: checkerboard, but ONLY when there's no diffuse to fill the area.
    // With a diffuse loaded, every pixel has meaningful content, and the
    // checkerboard would just bleed visual noise through the mask.
    if (m_backgroundImage.isNull()) {
        p.fillRect(dstRect, QBrush(checkerboardPattern()));
    }

    // Layer 2: diffuse background at full opacity. The mask overlay above
    // already encodes its own alpha (= channel intensity), so there's no
    // need to dim the diffuse to make the mask "pop".
    if (!m_backgroundImage.isNull()) {
        p.drawImage(dstRect, m_backgroundImage);
    }

    // Layer 3: mask overlay — alpha encodes intensity, so unpainted areas
    // show through to the diffuse layer underneath
    p.drawImage(dstRect, m_displayImage);

    // Layer 4: border
    p.setPen(QPen(QColor(70, 75, 85), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(dstRect);

    // Layer 5: UV wireframe overlay (cached pixmap, single blit)
    if (m_showUv && !m_uvCache.isNull()) {
        p.drawPixmap(dstRect, m_uvCache, m_uvCache.rect());
    }

    // Brush cursor preview (only shown for the Brush tool).
    // Three layers, drawn from inside-out so the user can see at a glance:
    //   - inner solid disc  = full-strength region (radTex * hardness)
    //   - soft-edge ring    = falloff region (full extent − inner)
    //   - outer hard ring   = full extent of brush radius (where the last
    //                         non-zero pixel will land)
    // All scaled by m_zoom so the cursor footprint exactly matches the
    // pixels that will be written by the next stamp.
    if (m_brush && underMouse() && m_drag != Drag::Pan && m_tool == CanvasTool::Brush) {
        const QPointF cursor = mapFromGlobal(QCursor::pos());
        const QPointF uv = widgetToUv(cursor);
        if (uv.x() >= 0 && uv.y() >= 0 && uv.x() <= 1 && uv.y() <= 1) {
            const float radTex   = m_brush->settings().radiusPx;
            const float radPx    = static_cast<float>(radTex * m_zoom);
            const float innerPx  = radPx * std::clamp(m_brush->settings().hardness, 0.0f, 1.0f);

            p.setRenderHint(QPainter::Antialiasing, true);

            auto drawCursor = [&](const QPointF& center, bool primary) {
                // Faint translucent fill of the soft-edge region — visualizes
                // the actual paint footprint without obscuring what's under it.
                if (primary && innerPx < radPx - 0.5f) {
                    QColor fill(255, 255, 255, 35);
                    p.setBrush(fill);
                    p.setPen(Qt::NoPen);
                    p.drawEllipse(center, radPx, radPx);
                }
                p.setBrush(Qt::NoBrush);
                // Outer ring — full extent. Double-stroke (black + white) so
                // it's readable on any background.
                p.setPen(QPen(QColor(0, 0, 0, primary ? 200 : 120), primary ? 2 : 1));
                p.drawEllipse(center, radPx, radPx);
                p.setPen(QPen(QColor(255, 255, 255, primary ? 230 : 130), 1));
                p.drawEllipse(center, radPx, radPx);
                // Inner ring at the hardness boundary. Only draw if it's
                // visibly inside the outer ring (skip when hardness is ~0
                // or ~100% so we don't clutter the cursor).
                if (primary && innerPx > 1.5f && innerPx < radPx - 1.5f) {
                    p.setPen(QPen(QColor(0, 0, 0, 160), 1, Qt::DashLine));
                    p.drawEllipse(center, innerPx, innerPx);
                    p.setPen(QPen(QColor(255, 255, 255, 200), 1, Qt::DashLine));
                    p.drawEllipse(center, innerPx, innerPx);
                }
            };

            drawCursor(cursor, /*primary*/ true);

            // Mirror cursor preview
            if (m_brush->settings().mirrorX) {
                const QPointF mirroredUv(1.0 - uv.x(), uv.y());
                const QPointF mirroredCursor = uvToWidget(mirroredUv);
                drawCursor(mirroredCursor, /*primary*/ false);
            }

            // Stroke smoothing tail — line from raw cursor to lagged paint point
            if (m_drag == Drag::Paint && m_brush->settings().smoothing > 0.001f) {
                float sx = -1, sy = -1;
                m_brush->smoothedPoint(sx, sy);
                if (sx >= 0 && sy >= 0 && m_doc) {
                    const double s = static_cast<double>(m_doc->size());
                    const QPointF lagWidget(
                        m_pan.x() + (sx / s) * s * m_zoom,
                        m_pan.y() + (sy / s) * s * m_zoom);
                    p.setPen(QPen(QColor(74, 144, 240, 200), 1.5));
                    p.drawLine(lagWidget, cursor);
                    p.setBrush(QBrush(QColor(74, 144, 240, 200)));
                    p.drawEllipse(lagWidget, 3, 3);
                    p.setBrush(Qt::NoBrush);
                }
            }
        }
    }
}

void CanvasWidget::resizeEvent(QResizeEvent*)
{
    // If a fit was deferred (e.g. we were a hidden tab page), do it now that
    // we have a real size. Otherwise leave the user's pan/zoom alone.
    if (m_pendingFit) fitToView();
}

void CanvasWidget::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);

    // The first time this canvas actually becomes visible after a document
    // was bound, the QTabWidget has finally given us our real on-screen size.
    // Re-run fitToView even if a previous "pre-show" fit already ran — that
    // one was against the wrong size.
    if (m_needsInitialFit) {
        m_needsInitialFit = false;
        m_pendingFit = true;
        fitToView();
    } else if (m_pendingFit) {
        fitToView();
    }
}

// ─────────────────────────────────────────────────────────────────
// Mouse interaction
// ─────────────────────────────────────────────────────────────────

void CanvasWidget::mousePressEvent(QMouseEvent* ev)
{
    const QPointF pos = ev->position();

    if (ev->button() == Qt::MiddleButton) {
        m_drag = Drag::Pan;
        m_dragStart = pos;
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    if (ev->button() != Qt::LeftButton) return;
    if (!m_doc) return;

    const QPointF uv = widgetToUv(pos);
    if (uv.x() < 0 || uv.y() < 0 || uv.x() > 1 || uv.y() > 1) return;

    const int N  = m_doc->size();
    const int tx = std::clamp(static_cast<int>(uv.x() * N), 0, N - 1);
    const int ty = std::clamp(static_cast<int>(uv.y() * N), 0, N - 1);

    // ── Alt+click is always eyedropper, regardless of active tool ──
    const bool altHeld = (ev->modifiers() & Qt::AltModifier) != 0;
    if (altHeld || m_tool == CanvasTool::Eyedropper) {
        if (!m_brush) return;
        // Pick the first set channel from the brush's channelMask
        const uint8_t mask = m_brush->settings().channelMask;
        int ch = -1;
        for (int i = 0; i < 4; ++i) {
            if (mask & (1u << i)) { ch = i; break; }
        }
        if (ch < 0) return;
        uint8_t rgba[4];
        m_doc->getPixel(tx, ty, rgba);
        m_brush->setInkValue(ch, rgba[ch]);
        emit inkPicked(ch, rgba[ch]);
        return;
    }

    // ── Bucket fill ──
    if (m_tool == CanvasTool::Bucket) {
        if (!m_brush) return;
        FillSettings fs;
        const auto& bs = m_brush->settings();
        fs.channelMask = bs.channelMask;
        fs.inkR = bs.inkR; fs.inkG = bs.inkG; fs.inkB = bs.inkB; fs.inkA = bs.inkA;
        fs.opacity   = bs.opacity;
        fs.tolerance = static_cast<uint8_t>(std::clamp(m_fillTolerance, 0, 255));
        fs.scope     = m_fillScope;

        const uint8_t* uvm = (m_uvCoverageSize == N) ? m_uvCoverage : nullptr;
        if ((fs.scope == FillScope::UvIsland || fs.scope == FillScope::ColorAndUv)
            && !uvm) {
            // Fallback: degrade to ColorConnected when no coverage available
            fs.scope = FillScope::ColorConnected;
        }

        // When bound to a LayerStack, write to the active layer (so the user
        // can hide / merge / re-order the fill afterward) and tell the stack
        // to recomposite the affected rect. Without a stack (tests / pre-PMX)
        // fall back to writing straight to the doc.
        m_pendingDirtyBox = {};
        if (m_stack) {
            auto* target = m_stack->activeTarget();
            if (target) {
                // Snapshot the active PaintLayer BEFORE the fill so we can
                // record the pre-state for undo. PaintLayer's copy ctor is
                // cheap memcpy of pixels + wroteMask (~24 MB at 2048², a
                // one-shot transient — see plan rationale).
                auto* paint = activePaintLayer();
                std::unique_ptr<PaintLayer> snapshot;
                if (paint) {
                    snapshot = std::make_unique<PaintLayer>(*paint);
                }
                floodFill(*m_doc, *target, tx, ty, fs, uvm, m_pendingDirtyBox);
                if (!m_pendingDirtyBox.isEmpty()) {
                    m_stack->recompositeRect(m_pendingDirtyBox);
                    if (paint && snapshot) {
                        m_stack->history().recordFloodFill(*snapshot, *paint,
                                                            m_pendingDirtyBox);
                    }
                }
            }
        } else {
            floodFill(*m_doc, *m_doc, tx, ty, fs, uvm, m_pendingDirtyBox);
        }
        m_pendingDirtyBox = {};
        emit strokeFinished();
        update();
        return;
    }

    // ── Brush ──
    if (!m_brush) return;

    PixelTarget* target = paintTarget();
    if (!target) return;

    // Shift+Click: synthetic stroke from last stroke end → click position
    const bool shiftHeld = (ev->modifiers() & Qt::ShiftModifier) != 0;
    const bool hasPrev   = m_lastStrokeEndUv.x() >= 0 && m_lastStrokeEndUv.y() >= 0;
    if (shiftHeld && hasPrev) {
        // Open undo entry for this synthetic stroke.
        PaintLayer* paint = activePaintLayer();
        if (paint && m_stack) m_stack->history().beginStrokeRecord(*paint);
        m_strokeUnionBox = {};

        m_pendingDirtyBox = {};
        m_brush->beginStroke(*target,
                             static_cast<float>(m_lastStrokeEndUv.x()),
                             static_cast<float>(m_lastStrokeEndUv.y()),
                             m_pendingDirtyBox);
        m_strokeUnionBox.unionWith(m_pendingDirtyBox);
        if (paint && m_stack) m_stack->history().recordStampBox(m_pendingDirtyBox, *paint);
        flushPendingComposite();

        m_brush->strokeTo  (*target,
                            static_cast<float>(uv.x()),
                            static_cast<float>(uv.y()),
                            m_pendingDirtyBox);
        m_strokeUnionBox.unionWith(m_pendingDirtyBox);
        if (paint && m_stack) m_stack->history().recordStampBox(m_pendingDirtyBox, *paint);
        flushPendingComposite();

        m_brush->endStroke();
        if (paint && m_stack) m_stack->history().endStrokeRecord(*paint);
        m_lastStrokeEndUv = uv;
        emit strokeFinished();
        update();
        return;
    }

    m_drag = Drag::Paint;
    m_strokeStartUv = uv;

    // Open undo entry for the drag-stroke.
    {
        PaintLayer* paint = activePaintLayer();
        if (paint && m_stack) m_stack->history().beginStrokeRecord(*paint);
    }
    m_strokeUnionBox = {};

    m_pendingDirtyBox = {};
    m_brush->beginStroke(*target, static_cast<float>(uv.x()), static_cast<float>(uv.y()),
                         m_pendingDirtyBox);
    m_strokeUnionBox.unionWith(m_pendingDirtyBox);
    if (auto* paint = activePaintLayer())
        if (m_stack) m_stack->history().recordStampBox(m_pendingDirtyBox, *paint);
    flushPendingComposite();
    update();
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* ev)
{
    const QPointF pos = ev->position();

    if (m_drag == Drag::Pan) {
        m_pan += pos - m_dragStart;
        m_dragStart = pos;
        update();
    } else if (m_drag == Drag::Paint) {
        if (m_doc && m_brush) {
            QPointF uv = widgetToUv(pos);

            // Shift while painting → snap angle to 15° from stroke start
            if (ev->modifiers() & Qt::ShiftModifier) {
                const QPointF d = uv - m_strokeStartUv;
                const double  ang = std::atan2(d.y(), d.x());
                constexpr double step = 15.0 * 3.14159265358979 / 180.0;
                const double snapped = std::round(ang / step) * step;
                const double len = std::hypot(d.x(), d.y());
                uv = m_strokeStartUv + QPointF(std::cos(snapped) * len,
                                                std::sin(snapped) * len);
            }

            if (PixelTarget* target = paintTarget()) {
                m_pendingDirtyBox = {};
                m_brush->strokeTo(*target,
                                  static_cast<float>(uv.x()),
                                  static_cast<float>(uv.y()),
                                  m_pendingDirtyBox);
                m_strokeUnionBox.unionWith(m_pendingDirtyBox);
                if (auto* paint = activePaintLayer())
                    if (m_stack) m_stack->history().recordStampBox(m_pendingDirtyBox, *paint);
                flushPendingComposite();
                update();
            }
        }
    } else {
        // Hover update for brush cursor
        update();
    }

    emit cursorUvChanged(widgetToUv(pos));
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* ev)
{
    if (m_drag == Drag::Pan && ev->button() == Qt::MiddleButton) {
        m_drag = Drag::None;
        setCursor(Qt::ArrowCursor);
    } else if (m_drag == Drag::Paint && ev->button() == Qt::LeftButton) {
        m_drag = Drag::None;
        m_lastStrokeEndUv = widgetToUv(ev->position());
        if (m_brush) m_brush->endStroke();
        // Close the undo entry for this stroke. If `m_stack` is null (test
        // path) we never opened one and endStrokeRecord is a no-op.
        if (auto* paint = activePaintLayer())
            if (m_stack) m_stack->history().endStrokeRecord(*paint);
        emit strokeFinished();
        update();
    }
}

void CanvasWidget::wheelEvent(QWheelEvent* ev)
{
    if (!m_doc) return;
    const double steps  = ev->angleDelta().y() / 120.0;
    const double factor = std::pow(1.15, steps);
    const double newZoom = std::clamp(m_zoom * factor, 0.05, 64.0);

    const QPointF cursor = ev->position();
    const QPointF tex    = (cursor - m_pan) / m_zoom;
    m_zoom = newZoom;
    m_pan  = cursor - tex * m_zoom;

    update();
}

void CanvasWidget::leaveEvent(QEvent*)
{
    emit cursorUvChanged(QPointF(-1, -1));
    update();   // refresh to remove brush cursor
}

void CanvasWidget::keyPressEvent(QKeyEvent* ev)
{
    switch (ev->key()) {
    case Qt::Key_B: setTool(CanvasTool::Brush);      break;
    case Qt::Key_G: setTool(CanvasTool::Bucket);     break;
    case Qt::Key_I: setTool(CanvasTool::Eyedropper); break;
    default:        QWidget::keyPressEvent(ev);      return;
    }
}

// ─────────────────────────────────────────────────────────────────
// Layer-stack glue
// ─────────────────────────────────────────────────────────────────

PixelTarget* CanvasWidget::paintTarget()
{
    if (m_stack) {
        if (auto* t = m_stack->activeTarget()) return t;
        // Stack bound but active layer is non-paintable. Fall through to doc
        // is wrong — we'd write the composite straight, bypassing layers.
        // Returning nullptr makes the caller bail, which surfaces as "click
        // does nothing" in the UI. (Step 3 will surface a status message.)
        return nullptr;
    }
    return m_doc;
}

PaintLayer* CanvasWidget::activePaintLayer()
{
    if (!m_stack) return nullptr;
    return dynamic_cast<PaintLayer*>(m_stack->activeLayer());
}

void CanvasWidget::flushPendingComposite()
{
    if (m_stack && !m_pendingDirtyBox.isEmpty()) {
        m_stack->recompositeRect(m_pendingDirtyBox);
    }
    m_pendingDirtyBox = {};
}


} // namespace editor
