#pragma once

#include "editor/BrushEngine.h"
#include "editor/TextureDocument.h"
#include "editor/TextureGroup.h"

#include <QMainWindow>

#include <QStringList>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>

class DX11Widget;
class QLabel;
class QMenu;
class QTabWidget;
class QComboBox;
class QCheckBox;
class QPushButton;
class QAction;
class QCloseEvent;

namespace editor { class CanvasWidget; class BrushPanel; class InfoPanel; class LayerPanel; }

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent*) override;

private:
    void setupUI();
    bool openPmx(const std::filesystem::path& path, int textureSize);

    /// Show the texture-size selection dialog. Returns the chosen size, or
    /// 0 if the user cancelled. matCount is used to compute the VRAM hint.
    int chooseTextureSize(int matCount);

    /// Read the user's last texture-size choice from QSettings, or kFallbackTextureSize.
    int loadDefaultTextureSize() const;
    /// Persist the user's choice for next time.
    void saveDefaultTextureSize(int size) const;

    /// Reset to a state with no model loaded — clear groups, combo, canvases.
    void clearProject();

    /// Push the active group's docs/background/UV range to all 4 canvases.
    /// Idempotent.
    void applyActiveGroupToCanvases();

    /// Bind the LayerPanel to the active (group, kind) — invoked whenever
    /// either changes. No-op if no model is loaded.
    void bindLayerPanelToActive();

    void onCanvasCursorMoved(QPointF uv);
    void onActiveTextureChanged(int tabIndex);
    void onChannelViewChanged(int comboIndex);
    void onShowUvToggled(bool on);
    void onTextureGroupChanged(int comboIndex);
    void on3dPaintHitGroup(int groupIndex);

    // ── Layer ops triggered from LayerPanel ─────────────────────
    void onAddFillLayer();      ///< build FillLayer from brush ink + channelMask
    void onAddImageLayer();     ///< QFileDialog → ImageLayer
    void onAddChannelLayer();   ///< source PNG + channel selection → ChannelLayer

    // ── Undo / Redo (Phase 9) ──────────────────────────────────
    /// Apply undo on the currently active stack. No-op if stack has nothing
    /// to undo or no model is loaded.
    void onUndo();
    void onRedo();
    /// Enable/disable Edit menu items based on the active stack's history.
    /// Called after every action that might change canUndo/canRedo: tab
    /// change, group change, strokeFinished, undo/redo itself.
    void refreshUndoActions();

    /// Active LayerStack — derived from m_activeGroup + the currently selected
    /// texture tab. Returns nullptr if no model is loaded.
    editor::LayerStack* currentStack() const;

    // ── Project I/O (Phase 7) ───────────────────────────────────
    bool saveProject();             ///< uses m_currentProjectPath, falls back to SaveAs
    bool saveProjectAs();
    bool loadProject(const std::filesystem::path& projectPath);
    bool exportActiveTexture();     ///< user picks file, write PNG of current canvas's doc
    bool exportAllTextures();       ///< user picks directory, batch write all groups × kinds
    void newProject();              ///< clear everything, no model loaded
    bool maybePromptSaveDirty();    ///< returns true if it's OK to proceed (saved or discarded)

    /// Mark the project as having unsaved changes. Called from any signal that
    /// indicates a user-driven write to a TextureDocument.
    void markDirty();

    /// Update window title to reflect current project path + dirty state.
    void updateWindowTitle();

    /// Persist + reload the recent-projects list from QSettings. The menu
    /// re-uses pre-allocated QActions, so it can be rebuilt cheaply.
    void loadRecentProjects();
    void saveRecentProjects() const;
    void rebuildRecentProjectsMenu();
    void pushRecentProject(const std::filesystem::path& projectPath);

    // ── Scene state ─────────────────────────────────────────────
    DX11Widget* m_viewport = nullptr;

    /// Texture groups built from the loaded PMX. Empty before PMX load.
    /// Each group owns its own 4 control TextureDocuments and shared diffuse.
    editor::TextureGroupSet m_groupSet;

    /// Index into m_groupSet.groups, or -1 if no model loaded.
    int m_activeGroup = -1;

    /// PMX path of the current model, kept for project-save (so Save can
    /// record it without asking again).
    std::filesystem::path m_currentModelPath;

    // ── Project state ──────────────────────────────────────────
    std::optional<std::filesystem::path> m_currentProjectPath;
    bool        m_dirty = false;
    int         m_currentTextureSize = 2048;
    QStringList m_recentProjects;     ///< up to kMaxRecentProjects entries

    // ── Editor panel ────────────────────────────────────────────
    QWidget*    m_editorPanel = nullptr;
    QTabWidget* m_textureTabs = nullptr;
    QComboBox*  m_channelCombo = nullptr;
    QComboBox*  m_groupCombo = nullptr;        ///< texture group selector
    QCheckBox*  m_uvCheckbox  = nullptr;
    QCheckBox*  m_autoSwitchCheckbox = nullptr; ///< sync left combo when 3D paint hits a group
    QPushButton* m_setPivotBtn = nullptr;       ///< viewport bar "Set Pivot" toggle
    QLabel*     m_uvCoordLabel = nullptr;

    editor::BrushPanel*    m_brushPanel    = nullptr;
    editor::InfoPanel*     m_infoPanel     = nullptr;
    editor::LayerPanel*    m_layerPanel    = nullptr;
    editor::BrushEngine    m_brush;          ///< shared by all canvases

    /// One CanvasWidget per TextureKind (Count = 4).
    std::array<editor::CanvasWidget*, static_cast<size_t>(editor::TextureKind::Count)> m_canvases{};

    // ── File menu actions kept around so we can enable/disable + rebuild ──
    QMenu*   m_recentMenu = nullptr;
    QAction* m_saveAction = nullptr;
    QAction* m_saveAsAction = nullptr;
    QAction* m_exportActiveAction = nullptr;
    QAction* m_exportAllAction = nullptr;
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
};
