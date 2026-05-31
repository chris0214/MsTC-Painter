#pragma once

#include "Layer.h"
#include "TextureDocument.h"

#include <QWidget>

class QListWidget;
class QListWidgetItem;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;

namespace editor {

class LayerStack;

/// Compact panel listing the layers of one LayerStack. The host (MainWindow)
/// rebinds the panel to a new stack when the active TextureKind tab changes.
///
/// UI layout (from top to bottom):
///   - Header label "Layers — <kindName>"
///   - Toolbar:  [+ Paint] [+ Fill] [+ Img] [+ Ch] [↑] [↓] [Merge] [Delete]
///   - QListWidget of layer rows. Each row:
///       checkbox (visible) | name | opacity slider
///   - For Step 3 only the [+ Paint], move-up/down, delete are wired —
///     the other Add buttons sit disabled until Step 4..6.
///   - Selecting a row makes it the active layer (brush writes go there).
class LayerPanel : public QWidget
{
    Q_OBJECT
public:
    explicit LayerPanel(QWidget* parent = nullptr);

    /// Bind to a stack. Pass nullptr to detach (e.g. when no model is loaded).
    /// The panel keeps a non-owning pointer; caller must outlive the panel
    /// or null-rebind first.
    void setStack(LayerStack* stack);
    LayerStack* stack() const { return m_stack; }

    /// Update the title label so the user knows which kind they're editing.
    void setKindLabel(const QString& kindName);

    /// Push a layer at the top of the bound stack and refresh the UI.
    /// Used by the host when responding to addFillRequested / addImageRequested
    /// / addChannelRequested signals. Returns the new layer's index, or -1
    /// if no stack is bound.
    int  pushLayer(std::unique_ptr<Layer> l);

signals:
    /// Emitted whenever the active layer or any layer's visibility/opacity
    /// changes. Host should mark the project dirty + repaint canvas/3D.
    void layerStackChanged();

    /// "+ Paint" button — host can either let the panel default-add a blank
    /// PaintLayer (if no slot is connected) or override with custom logic.
    /// Currently the panel handles this internally; the signal is emitted
    /// AFTER the add for hosts that want to react.
    void paintLayerAdded();

    /// "+ Fill" button — panel does NOT add anything itself. Host receives
    /// this and decides what color/channelMask to use (typically reading
    /// from the active BrushEngine's settings) and adds the layer.
    void addFillRequested();

    /// "+ Img" — host should QFileDialog a PNG, build ImageLayer, push it.
    void addImageRequested();

    /// "+ Ch"  — host should pick source PNG + channels, push ChannelLayer.
    void addChannelRequested();

private slots:
    void onAddPaintLayer();
    void onMoveUp();
    void onMoveDown();
    void onDeleteSelected();
    void onMergeDown();
    void onSelectionChanged();
    void onItemChanged(QListWidgetItem* it);   ///< checkbox + rename
    void onOpacitySliderChanged(int v);

private:
    void rebuild();   ///< repopulate list from m_stack
    void syncRowToActive();
    int  selectedIndex() const;

    LayerStack*    m_stack       = nullptr;
    QLabel*        m_titleLabel  = nullptr;
    QListWidget*   m_list        = nullptr;
    QPushButton*   m_addPaintBtn = nullptr;
    QPushButton*   m_addFillBtn  = nullptr;
    QPushButton*   m_addImageBtn = nullptr;
    QPushButton*   m_addChannelBtn = nullptr;
    QPushButton*   m_upBtn       = nullptr;
    QPushButton*   m_downBtn     = nullptr;
    QPushButton*   m_deleteBtn   = nullptr;
    QPushButton*   m_mergeBtn    = nullptr;
    QSlider*       m_opacitySlider = nullptr;
    QLabel*        m_opacityLabel  = nullptr;

    /// Reentrancy guard — rebuild()/syncRowToActive() set checkbox state, which
    /// emits itemChanged; that signal would otherwise loop back into this code.
    bool           m_suppressSignals = false;
};

} // namespace editor
