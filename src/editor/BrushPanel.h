#pragma once

#include "BrushEngine.h"
#include "CanvasWidget.h"     // for CanvasTool enum
#include "FloodFill.h"        // for FillScope enum
#include "TextureDocument.h"

#include <QWidget>

class QSlider;
class QLabel;
class QCheckBox;
class QComboBox;
class QFrame;
class QPushButton;
class QButtonGroup;

namespace editor {

/// Compact UI panel that drives a BrushEngine and tool selection on a
/// CanvasWidget.
class BrushPanel : public QWidget
{
    Q_OBJECT

public:
    explicit BrushPanel(QWidget* parent = nullptr);

    void setBrush(BrushEngine* brush);
    void setActiveKind(TextureKind kind);

    /// Reflect a tool change initiated elsewhere (e.g. keyboard shortcut on canvas).
    void syncToolFromCanvas(CanvasTool t);

    /// Update the Ink swatch after an eyedropper pick.
    void onInkPicked(int channelIndex, uint8_t v);

signals:
    /// Emitted when the user switches tool via the panel buttons.
    void toolChanged(CanvasTool t);

    /// Emitted when bucket settings change.
    void fillToleranceChanged(int value);
    void fillScopeChanged(FillScope scope);

private slots:
    void emitSettingsToBrush();

private:
    void rebuildChannelLabels();
    void rebuildInkSwatch();

    BrushEngine* m_brush = nullptr;
    TextureKind  m_kind  = TextureKind::ShadowRate;
    CanvasTool   m_tool  = CanvasTool::Brush;

    // Tool buttons
    QPushButton* m_toolBrush;
    QPushButton* m_toolBucket;
    QPushButton* m_toolEyedropper;

    // Bucket-specific (visible only when Bucket tool active)
    QWidget*  m_bucketRow;
    QSlider*  m_toleranceSlider;
    QLabel*   m_toleranceLabel;
    QComboBox* m_scopeCombo;

    // Brush sliders
    QSlider* m_sizeSlider;
    QLabel*  m_sizeLabel;
    QSlider* m_hardnessSlider;
    QLabel*  m_hardnessLabel;
    QSlider* m_opacitySlider;
    QLabel*  m_opacityLabel;
    QSlider* m_spacingSlider;
    QLabel*  m_spacingLabel;
    QSlider* m_smoothingSlider;
    QLabel*  m_smoothingLabel;

    // Channels + flow
    QPushButton* m_chanR;
    QPushButton* m_chanG;
    QPushButton* m_chanB;
    QPushButton* m_flowWash;
    QPushButton* m_flowBuildup;

    // Eraser + Mirror + Ink swatch
    QCheckBox* m_eraserCheck;
    QCheckBox* m_mirrorXCheck;
    QFrame*    m_inkSwatch;
    QLabel*    m_inkLabel;
};

} // namespace editor

