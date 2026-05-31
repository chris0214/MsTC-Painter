#include "MainWindow.h"

#include "renderer/DX11Widget.h"
#include "renderer/Camera.h"
#include "editor/BrushPanel.h"
#include "editor/CanvasWidget.h"
#include "editor/InfoPanel.h"
#include "editor/LayerPanel.h"
#include "editor/TextureExport.h"
#include "editor/TextureGroup.h"
#include "editor/UvLines.h"
#include "pmx/PmxParser.h"
#include "project/Project.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <spdlog/spdlog.h>
#include <stb_image.h>

namespace {
std::string u8s(const std::u8string& s)
{
    return std::string(reinterpret_cast<const char*>(s.c_str()), s.size());
}

constexpr int kFallbackTextureSize = 2048;

// Texture sizes the user can pick. The artist's choice should match how big
// they expect to zoom in — small models (~10 mats) are fine at 1024², while
// dense character models (40+ mats × 4 channels) eat VRAM fast at 4096+.
constexpr int kSizeOptions[] = { 512, 1024, 2048, 4096, 8192 };

constexpr int kMaxRecentProjects = 5;
constexpr const char* kProjectExt = ".mstcproj";
constexpr const char* kProjectFilter = "msTC Project (*.mstcproj)";
} // namespace

// ─────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("msTC Texture Studio"));
    resize(1700, 950);

    setupUI();
    loadRecentProjects();
    rebuildRecentProjectsMenu();
    updateWindowTitle();
}

MainWindow::~MainWindow() = default;

// ─────────────────────────────────────────────────────────────────
// UI
// ─────────────────────────────────────────────────────────────────

void MainWindow::setupUI()
{
    // ── Menu bar ────────────────────────────────────────────────
    auto* fileMenu = menuBar()->addMenu(tr("&File"));

    auto* newAction = fileMenu->addAction(tr("&New Project"));
    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, [this]() {
        if (!maybePromptSaveDirty()) return;
        newProject();
    });

    auto* openProjectAction = fileMenu->addAction(tr("&Open Project..."));
    openProjectAction->setShortcut(QKeySequence::Open);
    connect(openProjectAction, &QAction::triggered, this, [this]() {
        if (!maybePromptSaveDirty()) return;
        QString path = QFileDialog::getOpenFileName(
            this, tr("Open Project"), QString(), kProjectFilter);
        if (path.isEmpty()) return;
        loadProject(std::filesystem::path(path.toStdU16String()));
    });

    m_saveAction = fileMenu->addAction(tr("&Save"));
    m_saveAction->setShortcut(QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this, [this]() { saveProject(); });

    m_saveAsAction = fileMenu->addAction(tr("Save &As..."));
    m_saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(m_saveAsAction, &QAction::triggered, this, [this]() { saveProjectAs(); });

    fileMenu->addSeparator();

    auto* importPmxAction = fileMenu->addAction(tr("&Import PMX Model..."));
    connect(importPmxAction, &QAction::triggered, this, [this]() {
        if (!maybePromptSaveDirty()) return;
        QString path = QFileDialog::getOpenFileName(
            this, tr("Import PMX Model"), QString(), tr("PMX Models (*.pmx)"));
        if (path.isEmpty()) return;

        // Peek the material count so the size dialog can show an informed
        // VRAM estimate. We re-parse during openPmx; PMX parsing is cheap
        // (low tens of ms) so this duplication is fine.
        pmx::Model peek;
        std::string err;
        const std::filesystem::path fsPath(path.toStdU16String());
        const int matCount = pmx::loadModel(fsPath, peek, err)
                                ? static_cast<int>(peek.materials.size())
                                : 0;

        const int chosen = chooseTextureSize(matCount);
        if (chosen <= 0) return;   // user cancelled
        saveDefaultTextureSize(chosen);

        openPmx(fsPath, chosen);
    });

    fileMenu->addSeparator();

    m_exportActiveAction = fileMenu->addAction(tr("Export &Active Texture..."));
    connect(m_exportActiveAction, &QAction::triggered, this, [this]() { exportActiveTexture(); });

    m_exportAllAction = fileMenu->addAction(tr("Export A&ll Textures..."));
    connect(m_exportAllAction, &QAction::triggered, this, [this]() { exportAllTextures(); });

    fileMenu->addSeparator();

    // Recent menu (populated on demand by rebuildRecentProjectsMenu).
    m_recentMenu = fileMenu->addMenu(tr("&Recent Projects"));

    fileMenu->addSeparator();
    fileMenu->addAction(tr("E&xit"), this, &QMainWindow::close);

    // ── Edit menu (Phase 9 undo/redo) ──────────────────────────
    auto* editMenu = menuBar()->addMenu(tr("&Edit"));

    m_undoAction = editMenu->addAction(tr("&Undo"));
    m_undoAction->setShortcut(QKeySequence::Undo);   // Ctrl+Z
    m_undoAction->setEnabled(false);
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::onUndo);

    m_redoAction = editMenu->addAction(tr("&Redo"));
    // Both Ctrl+Y (Windows convention) and Ctrl+Shift+Z (Mac/Linux convention).
    // QKeySequence::Redo is platform-dependent; explicit list covers both.
    m_redoAction->setShortcuts({ QKeySequence::Redo, QKeySequence("Ctrl+Y") });
    m_redoAction->setEnabled(false);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::onRedo);

    // ── Language menu ───────────────────────────────────────────
    // Picks the saved preference; switching here re-saves QSettings and
    // tells the user a restart is needed (live retranslation would require
    // calling retranslateUi everywhere — too invasive for v1).
    {
        auto* langMenu = menuBar()->addMenu(tr("&Language"));
        auto* langGroup = new QActionGroup(this);
        langGroup->setExclusive(true);

        QSettings ls("msTC", "TextureStudio");
        const QString currentLang = ls.value("uiLanguage", QString()).toString();

        struct LangEntry { const char* code; const char* label; };
        const LangEntry langs[] = {
            { "en",    "English" },
            { "zh_CN", "简体中文" },
        };
        for (const auto& [code, label] : langs) {
            auto* act = langMenu->addAction(QString::fromUtf8(label));
            act->setCheckable(true);
            act->setActionGroup(langGroup);
            const QString codeStr = QString::fromLatin1(code);
            // Mark the active one. If no preference saved, mark whatever
            // resolveUiLanguage picked at startup (zh_CN under a Chinese
            // system locale, else English).
            if (currentLang == codeStr
                || (currentLang.isEmpty() && (
                       (codeStr == "zh_CN" && QLocale::system().name().startsWith("zh"))
                    || (codeStr == "en"    && !QLocale::system().name().startsWith("zh")))))
            {
                act->setChecked(true);
            }
            connect(act, &QAction::triggered, this, [this, codeStr]() {
                QSettings s2("msTC", "TextureStudio");
                s2.setValue("uiLanguage", codeStr);
                QMessageBox::information(this,
                    tr("Language change"),
                    tr("Restart the application for the language change to take full effect."));
            });
        }
    }

    // ── Editor panel ────────────────────────────────────────────
    m_editorPanel = new QWidget(this);
    auto* editorLayout = new QVBoxLayout(m_editorPanel);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);

    // Info card (semantic doc for the active texture kind)
    m_infoPanel = new editor::InfoPanel(m_editorPanel);
    editorLayout->addWidget(m_infoPanel);

    // Brush panel
    m_brushPanel = new editor::BrushPanel(m_editorPanel);
    m_brushPanel->setBrush(&m_brush);
    editorLayout->addWidget(m_brushPanel);

    // Layer panel — sits between the brush settings and the canvas tabs so
    // it's adjacent to both ("here's where my next brush stroke lands; here's
    // what it looks like"). Stack rebinding happens on tab change.
    m_layerPanel = new editor::LayerPanel(m_editorPanel);
    editorLayout->addWidget(m_layerPanel);
    connect(m_layerPanel, &editor::LayerPanel::layerStackChanged, this, [this]() {
        markDirty();
        // Repaint all bound canvases so visibility/opacity changes appear.
        for (auto* c : m_canvases) if (c) c->update();
    });
    connect(m_layerPanel, &editor::LayerPanel::addFillRequested,
            this, &MainWindow::onAddFillLayer);
    connect(m_layerPanel, &editor::LayerPanel::addImageRequested,
            this, &MainWindow::onAddImageLayer);
    connect(m_layerPanel, &editor::LayerPanel::addChannelRequested,
            this, &MainWindow::onAddChannelLayer);

    // Toolbar: view mode + UV toggle + texture group + cursor coord
    {
        auto* tb = new QWidget(m_editorPanel);
        auto* tbLay = new QHBoxLayout(tb);
        tbLay->setContentsMargins(8, 4, 8, 4);

        tbLay->addWidget(new QLabel(tr("View:"), tb));

        m_channelCombo = new QComboBox(tb);
        m_channelCombo->addItem("RGB",      static_cast<int>(editor::ChannelView::RGB));
        m_channelCombo->addItem("R",        static_cast<int>(editor::ChannelView::R));
        m_channelCombo->addItem("G",        static_cast<int>(editor::ChannelView::G));
        m_channelCombo->addItem("B",        static_cast<int>(editor::ChannelView::B));
        m_channelCombo->addItem("Semantic", static_cast<int>(editor::ChannelView::Semantic));
        connect(m_channelCombo,
                QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &MainWindow::onChannelViewChanged);
        tbLay->addWidget(m_channelCombo);

        m_uvCheckbox = new QCheckBox(tr("UV"), tb);
        m_uvCheckbox->setChecked(true);
        connect(m_uvCheckbox, &QCheckBox::toggled,
                this, &MainWindow::onShowUvToggled);
        tbLay->addWidget(m_uvCheckbox);

        tbLay->addSpacing(8);
        tbLay->addWidget(new QLabel(tr("Texture:"), tb));
        m_groupCombo = new QComboBox(tb);
        m_groupCombo->setMinimumWidth(180);
        m_groupCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_groupCombo->setToolTip(
            "Each entry is one shared diffuse texture, plus the materials "
            "that use it. Switching here changes which set of control "
            "textures you are editing — they are fully independent.");
        connect(m_groupCombo,
                QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &MainWindow::onTextureGroupChanged);
        tbLay->addWidget(m_groupCombo, 1);

        m_uvCoordLabel = new QLabel(tr("UV: -"), tb);
        m_uvCoordLabel->setMinimumWidth(140);
        tbLay->addWidget(m_uvCoordLabel);

        editorLayout->addWidget(tb);
    }

    // Tab widget: one CanvasWidget per TextureKind
    m_textureTabs = new QTabWidget(m_editorPanel);
    for (size_t i = 0; i < m_canvases.size(); ++i) {
        const auto kind = static_cast<editor::TextureKind>(i);
        auto* canvas = new editor::CanvasWidget(m_textureTabs);
        canvas->setBrush(&m_brush);
        connect(canvas, &editor::CanvasWidget::cursorUvChanged,
                this, &MainWindow::onCanvasCursorMoved);
        connect(canvas, &editor::CanvasWidget::inkPicked,
                m_brushPanel, &editor::BrushPanel::onInkPicked);
        // Any 2D edit (stroke / fill / paste / drop) → mark project dirty.
        connect(canvas, &editor::CanvasWidget::strokeFinished,
                this, &MainWindow::markDirty);
        // Phase 9: refresh Edit > Undo/Redo enabled state after each stroke.
        connect(canvas, &editor::CanvasWidget::strokeFinished,
                this, &MainWindow::refreshUndoActions);
        m_canvases[i] = canvas;
        // Keep the panel UI in sync when canvas changes tool via keyboard (B/G/I)
        m_canvases[i] = canvas;
        m_textureTabs->addTab(canvas, editor::textureKindName(kind));
    }
    connect(m_textureTabs, &QTabWidget::currentChanged,
            this, &MainWindow::onActiveTextureChanged);
    editorLayout->addWidget(m_textureTabs, /*stretch*/ 1);

    // ── Brush panel signals → canvases ──────────────────────────
    connect(m_brushPanel, &editor::BrushPanel::toolChanged, this, [this](editor::CanvasTool t) {
        for (auto* c : m_canvases) if (c) c->setTool(t);
    });
    connect(m_brushPanel, &editor::BrushPanel::fillToleranceChanged, this, [this](int v) {
        for (auto* c : m_canvases) if (c) c->setFillTolerance(v);
    });
    connect(m_brushPanel, &editor::BrushPanel::fillScopeChanged, this, [this](editor::FillScope s) {
        for (auto* c : m_canvases) if (c) c->setFillScope(s);
    });

    // Initialize brush panel + info panel for tab 0
    m_brushPanel->setActiveKind(static_cast<editor::TextureKind>(0));
    m_infoPanel->setKind(static_cast<editor::TextureKind>(0));

    m_editorPanel->setMinimumWidth(380);

    // ── Right side: viewport with thin toolbar on top ──────────
    auto* viewportSide = new QWidget(this);
    auto* viewportLayout = new QVBoxLayout(viewportSide);
    viewportLayout->setContentsMargins(0, 0, 0, 0);
    viewportLayout->setSpacing(0);

    {
        auto* viewBar = new QWidget(viewportSide);
        viewBar->setObjectName("viewportBar");
        viewBar->setStyleSheet(
            "QWidget#viewportBar { background-color: #14161b; "
            "border-bottom: 1px solid #0d0e12; }");
        auto* hl = new QHBoxLayout(viewBar);
        hl->setContentsMargins(8, 4, 8, 4);

        hl->addWidget(new QLabel(tr("View:"), viewBar));

        auto* viewCombo = new QComboBox(viewBar);
        viewCombo->setMinimumWidth(140);
        viewCombo->addItem(tr("Mask Overlay"), static_cast<int>(ViewMode::MaskOverlay));
        viewCombo->addItem(tr("Lit"),          static_cast<int>(ViewMode::Lit));
        viewCombo->addItem(tr("Mask Only"),    static_cast<int>(ViewMode::MaskOnly));
        viewCombo->addItem(tr("UV Checker"),   static_cast<int>(ViewMode::UvChecker));
        viewCombo->setToolTip(
            tr("<b>Mask Overlay</b> — Lit shading + tinted mask painted on top.<br>"
               "<b>Lit</b> — clean Lambert + diffuse, no mask (default before paint).<br>"
               "<b>Mask Only</b> — pure mask, no lighting/diffuse.<br>"
               "<b>UV Checker</b> — replace diffuse with a UV checker pattern."));
        connect(viewCombo,
                QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, viewCombo](int idx) {
                    if (idx < 0) return;
                    const auto m = static_cast<ViewMode>(viewCombo->itemData(idx).toInt());
                    if (m_viewport) m_viewport->setViewMode(m);
                });
        hl->addWidget(viewCombo);

        // Auto-switch group checkbox: when the user paints in 3D on a group
        // different from the currently active one, automatically switch the
        // left-side Texture combo to match. Default on.
        m_autoSwitchCheckbox = new QCheckBox(tr("Auto-switch group"), viewBar);
        m_autoSwitchCheckbox->setChecked(true);
        m_autoSwitchCheckbox->setToolTip(
            tr("When ON, painting on the 3D model also switches the left-side "
               "Texture dropdown to the group you hit, so the 2D canvas follows "
               "along. Turn OFF to keep the 2D view focused on a single group "
               "while painting other groups in 3D."));
        hl->addSpacing(12);
        hl->addWidget(m_autoSwitchCheckbox);

        // Nav sensitivity slider: controls orbit / pan / zoom speed in the
        // 3D viewport. 1-200%, default 100%. Persisted in QSettings so it
        // survives restarts.
        hl->addSpacing(12);
        hl->addWidget(new QLabel(tr("Nav:"), viewBar));
        auto* navSlider = new QSlider(Qt::Horizontal, viewBar);
        navSlider->setRange(10, 200);   // 0.1× .. 2.0×
        navSlider->setMinimumWidth(80);
        navSlider->setMaximumWidth(120);
        navSlider->setToolTip(
            tr("3D viewport orbit / pan / zoom sensitivity.\n"
               "Lower = slower & more precise.\n"
               "Hold Shift while dragging for an additional ×0.2 fine-tune."));
        auto* navLabel = new QLabel("100%", viewBar);
        navLabel->setMinimumWidth(40);
        {
            QSettings s("msTC", "TextureStudio");
            const int saved = s.value("navSensitivityPct", 100).toInt();
            navSlider->setValue(std::clamp(saved, 10, 200));
            navLabel->setText(QString::number(navSlider->value()) + "%");
            if (m_viewport) m_viewport->setNavSensitivity(navSlider->value() / 100.0f);
        }
        connect(navSlider, &QSlider::valueChanged, this,
                [this, navLabel](int v) {
                    navLabel->setText(QString::number(v) + "%");
                    if (m_viewport) m_viewport->setNavSensitivity(v / 100.0f);
                    QSettings s("msTC", "TextureStudio");
                    s.setValue("navSensitivityPct", v);
                });
        hl->addWidget(navSlider);
        hl->addWidget(navLabel);

        // Pivot controls — set / show.
        // "Set Pivot" is a one-shot armed mode: click the button, then click
        // anywhere on the model to move the orbit pivot there. Disarms after
        // one click. Equivalent to pressing F with the cursor over the spot.
        hl->addSpacing(12);
        auto* setPivotBtn = new QPushButton(tr("Set Pivot"), viewBar);
        setPivotBtn->setCheckable(true);
        setPivotBtn->setToolTip(
            tr("Click then click on the model to move the orbit pivot there.\n"
               "Shortcut: hover the cursor over the spot and press F."));
        connect(setPivotBtn, &QPushButton::toggled, this, [this](bool on) {
            if (!m_viewport) return;
            if (on) m_viewport->armSetPivotMode();
        });
        // Disarm UI when the click is consumed inside DX11Widget.
        if (m_viewport == nullptr) { /* placeholder so connect compiles */ }
        // (Wire the disarm signal AFTER m_viewport is constructed below; we
        // capture setPivotBtn so the lambda can un-toggle it.)
        hl->addWidget(setPivotBtn);

        auto* pivotShowCheck = new QCheckBox(tr("Show Pivot"), viewBar);
        pivotShowCheck->setChecked(true);
        pivotShowCheck->setToolTip(
            tr("Show the orange marker disc at the orbit pivot point."));
        connect(pivotShowCheck, &QCheckBox::toggled, this, [this](bool on) {
            if (m_viewport) m_viewport->setShowPivotMarker(on);
        });
        hl->addWidget(pivotShowCheck);

        // Stash for post-construction wire-up of the disarm signal.
        m_setPivotBtn = setPivotBtn;

        hl->addStretch();
        viewportLayout->addWidget(viewBar);
    }

    m_viewport = new DX11Widget(viewportSide);
    m_viewport->setMinimumWidth(400);
    m_viewport->setViewMode(ViewMode::MaskOverlay);
    m_viewport->setBrush(&m_brush);
    // Auto-switch OFF means: stay focused on the active group in 2D, AND
    // ignore 3D clicks on other groups. Both behaviors hang off the same
    // checkbox so the user has a single mental switch ("track my edits"
    // vs. "lock to current group").
    m_viewport->setRestrictPaintToActiveGroup(
        m_autoSwitchCheckbox && !m_autoSwitchCheckbox->isChecked());
    viewportLayout->addWidget(m_viewport, /*stretch*/ 1);

    // 3D paint → MainWindow: when a stroke begins on a different group, swap
    // the Texture combo (which in turn syncs all the 2D canvases).
    connect(m_viewport, &DX11Widget::paintHitGroup,
            this, &MainWindow::on3dPaintHitGroup);

    // Auto-switch checkbox toggle → push restrict flag to the viewport.
    if (m_autoSwitchCheckbox) {
        connect(m_autoSwitchCheckbox, &QCheckBox::toggled,
                this, [this](bool on) {
                    if (m_viewport) m_viewport->setRestrictPaintToActiveGroup(!on);
                });
    }

    // Pivot mode disarm — DX11Widget signals when the click is consumed,
    // so the toolbar button can pop back out automatically.
    if (m_setPivotBtn && m_viewport) {
        connect(m_viewport, &DX11Widget::setPivotModeChanged,
                this, [this](bool armed) {
                    if (m_setPivotBtn) {
                        QSignalBlocker block(m_setPivotBtn);
                        m_setPivotBtn->setChecked(armed);
                    }
                });
    }
    // Any 3D paint stroke → mark project dirty too. Uses the same handler
    // as 2D since the dirty bit is a single MainWindow flag.
    connect(m_viewport, &DX11Widget::strokeFinished,
            this, &MainWindow::markDirty);
    connect(m_viewport, &DX11Widget::strokeFinished,
            this, &MainWindow::refreshUndoActions);

    // ── Splitter ────────────────────────────────────────────────
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_editorPanel);
    splitter->addWidget(viewportSide);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({700, 1000});

    setCentralWidget(splitter);

    statusBar()->showMessage(tr("Ready — open a PMX model to begin."));
}

// ─────────────────────────────────────────────────────────────────
// PMX open + group setup
// ─────────────────────────────────────────────────────────────────

bool MainWindow::openPmx(const std::filesystem::path& path, int textureSize)
{
    pmx::Model model;
    std::string err;
    if (!pmx::loadModel(path, model, err)) {
        statusBar()->showMessage(QString("Failed to load PMX: ") + QString::fromStdString(err));
        return false;
    }

    // Build texture groups (per-material) — replaces the old single-doc model.
    clearProject();
    m_currentModelPath   = path;
    m_currentTextureSize = textureSize;
    m_groupSet = editor::buildTextureGroups(model, path.parent_path(), textureSize);

    // Bind the group set to the viewport BEFORE setModel — StaticMesh build
    // needs it to fill per-segment group indices, and MaskTextureSet build
    // needs it to allocate GPU mask textures.
    m_viewport->setGroupSet(&m_groupSet);

    if (!m_viewport->setModel(model, path.parent_path())) {
        statusBar()->showMessage(tr("Loaded PMX but DX11 mesh build failed"));
        return false;
    }

    // Push the flat segment buffer to all canvases once. setUvSegmentRange
    // per group switches between sub-ranges cheaply.
    for (auto* c : m_canvases) {
        if (!c) continue;
        c->setUvSegments(m_groupSet.wireframe.segs);
    }

    // Repopulate the Texture combo. One entry per material; diffuse name is
    // shown as a parenthetical hint so artists can scan for shared atlases.
    {
        QSignalBlocker blocker(m_groupCombo);
        m_groupCombo->clear();
        for (size_t gi = 0; gi < m_groupSet.groups.size(); ++gi) {
            const auto& g = m_groupSet.groups[gi];
            QString label;
            if (!g.diffuseName.empty()) {
                label = QString("%1  ·  %2")
                            .arg(QString::fromStdString(g.name))
                            .arg(QString::fromStdString(g.diffuseName));
            } else {
                label = QString::fromStdString(g.name) + "  ·  (no texture)";
            }
            m_groupCombo->addItem(label, static_cast<int>(gi));
            m_groupCombo->setItemData(static_cast<int>(gi),
                QString("Material #%1 — %2 UV edges")
                    .arg(gi)
                    .arg(g.uvSegmentCount),
                Qt::ToolTipRole);
        }
    }

    m_activeGroup = m_groupSet.groups.empty() ? -1 : 0;
    if (m_viewport) m_viewport->setActiveGroup(m_activeGroup);
    if (m_activeGroup >= 0) {
        m_groupCombo->setCurrentIndex(0);
        applyActiveGroupToCanvases();
    }

    statusBar()->showMessage(
        QString::fromStdString("Loaded: " + u8s(path.filename().u8string())
                               + "  ·  " + std::to_string(m_groupSet.groups.size())
                               + " materials  ·  " + std::to_string(textureSize) + "²"));

    // A fresh PMX load means no project file is open and there's nothing
    // user-edited yet to save.
    m_dirty = false;
    updateWindowTitle();
    return true;
}

void MainWindow::clearProject()
{
    m_activeGroup = -1;
    if (m_viewport) m_viewport->setActiveGroup(-1);
    m_groupSet.groups.clear();
    m_groupSet.wireframe.segs.clear();
    m_groupSet.wireframe.groups.clear();
    m_currentModelPath.clear();
    // m_currentProjectPath / m_dirty are managed by the higher-level
    // newProject / loadProject / openPmx callers; do not stomp them here.

    if (m_viewport) m_viewport->setGroupSet(nullptr);

    for (auto* c : m_canvases) {
        if (!c) continue;
        c->setDocument(nullptr);
        c->setLayerStack(nullptr);
        c->setBackgroundImage(QImage());
        c->setUvSegments({});
        c->setUvCoverage(nullptr, 0);
    }
    if (m_layerPanel) m_layerPanel->setStack(nullptr);
    refreshUndoActions();   // no model → both menu items disabled
}

void MainWindow::applyActiveGroupToCanvases()
{
    if (m_activeGroup < 0
        || m_activeGroup >= static_cast<int>(m_groupSet.groups.size())) {
        return;
    }
    auto& g = m_groupSet.groups[m_activeGroup];
    const uint8_t* coverage = g.uvCoverage.empty() ? nullptr : g.uvCoverage.data();
    for (size_t i = 0; i < m_canvases.size(); ++i) {
        auto* c = m_canvases[i];
        if (!c) continue;
        c->setDocument(g.doc(i));
        c->setLayerStack(g.stacks[i].get());
        c->setBackgroundImage(g.diffuse);
        c->setUvSegmentRange(g.uvSegmentStart, g.uvSegmentCount);
        c->setUvCoverage(coverage, g.uvCoverageSize);
    }
    bindLayerPanelToActive();
}

void MainWindow::bindLayerPanelToActive()
{
    if (!m_layerPanel) return;
    if (m_activeGroup < 0
        || m_activeGroup >= static_cast<int>(m_groupSet.groups.size())) {
        m_layerPanel->setStack(nullptr);
        return;
    }
    auto& g = m_groupSet.groups[m_activeGroup];
    const int kindIdx = m_textureTabs ? m_textureTabs->currentIndex() : 0;
    if (kindIdx < 0 || kindIdx >= static_cast<int>(g.stacks.size())) {
        m_layerPanel->setStack(nullptr);
        return;
    }
    m_layerPanel->setStack(g.stacks[kindIdx].get());
    const auto kind = static_cast<editor::TextureKind>(kindIdx);
    m_layerPanel->setKindLabel(QString::fromUtf8(editor::textureKindName(kind)));
}

// ─────────────────────────────────────────────────────────────────
// UI slots
// ─────────────────────────────────────────────────────────────────

void MainWindow::onCanvasCursorMoved(QPointF uv)
{
    if (uv.x() < 0 || uv.y() < 0 || uv.x() > 1 || uv.y() > 1) {
        m_uvCoordLabel->setText(tr("UV: -"));
        return;
    }
    m_uvCoordLabel->setText(QString("UV: %1, %2")
                                .arg(uv.x(), 0, 'f', 4)
                                .arg(uv.y(), 0, 'f', 4));
}

void MainWindow::onActiveTextureChanged(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(m_canvases.size())) return;
    const auto kind = static_cast<editor::TextureKind>(idx);
    m_brushPanel->setActiveKind(kind);
    m_infoPanel->setKind(kind);
    if (m_viewport) m_viewport->setActiveKind(kind);
    bindLayerPanelToActive();   // panel is kind-aware
    refreshUndoActions();        // history is per-stack — refresh menu
}

void MainWindow::onChannelViewChanged(int idx)
{
    if (idx < 0) return;
    const int raw = m_channelCombo->itemData(idx).toInt();
    const auto v  = static_cast<editor::ChannelView>(raw);
    for (auto* c : m_canvases) if (c) c->setChannelView(v);
    // 3D viewport mirrors the same channel filter (same numeric values).
    if (m_viewport) m_viewport->setChannelView(static_cast<uint32_t>(raw));
}

void MainWindow::onShowUvToggled(bool on)
{
    for (auto* c : m_canvases) if (c) c->setShowUvWireframe(on);
}

void MainWindow::onTextureGroupChanged(int comboIdx)
{
    if (comboIdx < 0) return;
    const int gi = m_groupCombo->itemData(comboIdx).toInt();
    if (gi == m_activeGroup) return;
    m_activeGroup = gi;
    if (m_viewport) m_viewport->setActiveGroup(gi);
    applyActiveGroupToCanvases();
    refreshUndoActions();   // each (group, kind) pair has its own history
}

void MainWindow::on3dPaintHitGroup(int groupIndex)
{
    // Auto-sync the left-side Texture combo so the 2D canvas tracks what the
    // user is painting in 3D. User can disable this with the toolbar checkbox.
    // When OFF, the viewport already filters paint to the active group, so
    // this signal will only fire for `groupIndex == m_activeGroup` and the
    // early-return below is the only path taken.
    if (!m_autoSwitchCheckbox || !m_autoSwitchCheckbox->isChecked()) return;
    if (groupIndex < 0 || groupIndex == m_activeGroup) return;

    // Find the combo item whose data == groupIndex.
    for (int i = 0; i < m_groupCombo->count(); ++i) {
        if (m_groupCombo->itemData(i).toInt() == groupIndex) {
            m_groupCombo->setCurrentIndex(i);   // triggers onTextureGroupChanged
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────
// Layer-add slots (driven from LayerPanel signals)
// ─────────────────────────────────────────────────────────────────

void MainWindow::onAddFillLayer()
{
    if (!m_layerPanel) return;
    auto* stack = m_layerPanel->stack();
    if (!stack) return;

    // Pull ink + channel mask from the active brush settings. This means
    // the user picks the color via the BrushPanel's existing controls,
    // then clicks "+ Fill" to bake those settings into a layer.
    const auto& bs = m_brush.settings();
    auto fill = std::make_unique<editor::FillLayer>(
        stack->size(),
        bs.inkR, bs.inkG, bs.inkB, bs.inkA,
        bs.channelMask);
    fill->name = "Fill " + std::to_string(stack->layerCount() + 1);
    m_layerPanel->pushLayer(std::move(fill));
}

void MainWindow::onAddImageLayer()
{
    if (!m_layerPanel) return;
    auto* stack = m_layerPanel->stack();
    if (!stack) return;

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import image as layer"),
        QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.tga);;All files (*)"));
    if (path.isEmpty()) return;

    // Read bytes via QFile so non-ASCII paths work (stbi_load uses fopen
    // which mangles UTF-8 on Windows; stbi_load_from_memory sidesteps that).
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Image import"),
                             tr("Could not open file: %1").arg(path));
        return;
    }
    const QByteArray bytes = f.readAll();
    f.close();

    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.constData()),
        static_cast<int>(bytes.size()),
        &w, &h, &comp, /*desired_channels=*/4);
    if (!px || w <= 0 || h <= 0) {
        QMessageBox::warning(this, tr("Image import"),
                             tr("Failed to decode: %1\n%2")
                                 .arg(path)
                                 .arg(QString::fromUtf8(stbi_failure_reason())));
        if (px) stbi_image_free(px);
        return;
    }

    // ImageLayer resamples to the stack's texture size on construction (NN).
    // Square crop the input by using min(w,h) as the source extent — this
    // avoids stretching non-square reference images.
    const int srcSize = std::min(w, h);
    std::vector<uint8_t> square;
    square.reserve(static_cast<size_t>(srcSize) * srcSize * 4);
    const int offX = (w - srcSize) / 2;
    const int offY = (h - srcSize) / 2;
    for (int y = 0; y < srcSize; ++y) {
        const uint8_t* row = px + ((offY + y) * w + offX) * 4;
        square.insert(square.end(), row, row + srcSize * 4);
    }
    stbi_image_free(px);

    auto layer = std::make_unique<editor::ImageLayer>(
        stack->size(),
        square.data(), srcSize,
        /*channelMask=*/ static_cast<editor::ChannelAuthorMask>(
            editor::ChannelMaskR | editor::ChannelMaskG | editor::ChannelMaskB));
    QFileInfo fi(path);
    layer->name = "Img: " + fi.fileName().toStdString();
    m_layerPanel->pushLayer(std::move(layer));
}

void MainWindow::onAddChannelLayer()
{
    if (!m_layerPanel) return;
    auto* stack = m_layerPanel->stack();
    if (!stack) return;

    // ── 1. Pick source PNG ───────────────────────────────────────
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Source image for channel layer"),
        QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.tga);;All files (*)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Channel layer"),
                             tr("Could not open file: %1").arg(path));
        return;
    }
    const QByteArray bytes = f.readAll();
    f.close();

    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.constData()),
        static_cast<int>(bytes.size()),
        &w, &h, &comp, /*desired_channels=*/4);
    if (!px || w <= 0 || h <= 0) {
        QMessageBox::warning(this, tr("Channel layer"),
                             tr("Failed to decode: %1").arg(path));
        if (px) stbi_image_free(px);
        return;
    }

    // Square-crop centred (same convention as onAddImageLayer)
    const int srcSize = std::min(w, h);
    auto srcRgba = std::make_shared<std::vector<uint8_t>>();
    srcRgba->reserve(static_cast<size_t>(srcSize) * srcSize * 4);
    const int offX = (w - srcSize) / 2;
    const int offY = (h - srcSize) / 2;
    for (int y = 0; y < srcSize; ++y) {
        const uint8_t* row = px + ((offY + y) * w + offX) * 4;
        srcRgba->insert(srcRgba->end(), row, row + srcSize * 4);
    }
    stbi_image_free(px);

    // ── 2. Channel selection dialog ──────────────────────────────
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Channel layer — select channels"));
    auto* form = new QFormLayout(&dlg);

    auto* srcCombo = new QComboBox(&dlg);
    srcCombo->addItem("R", 0); srcCombo->addItem("G", 1);
    srcCombo->addItem("B", 2); srcCombo->addItem("A", 3);

    auto* dstCombo = new QComboBox(&dlg);
    dstCombo->addItem("R", 0); dstCombo->addItem("G", 1);
    dstCombo->addItem("B", 2); dstCombo->addItem("A", 3);

    form->addRow(tr("Source channel:"), srcCombo);
    form->addRow(tr("Target channel:"), dstCombo);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);

    if (dlg.exec() != QDialog::Accepted) return;

    const uint8_t srcCh = static_cast<uint8_t>(srcCombo->currentData().toInt());
    const uint8_t dstCh = static_cast<uint8_t>(dstCombo->currentData().toInt());

    // ── 3. Build layer ───────────────────────────────────────────
    auto layer = std::make_unique<editor::ChannelLayer>(
        stack->size(),
        std::move(srcRgba), srcSize,
        srcCh, dstCh);
    QFileInfo fi(path);
    static const char* kChNames = "RGBA";
    layer->name = std::string("Ch ") + kChNames[srcCh] + "→" + kChNames[dstCh]
                + ": " + fi.fileName().toStdString();
    m_layerPanel->pushLayer(std::move(layer));
}


// ─────────────────────────────────────────────────────────────────
// Undo / Redo (Phase 9)
// ─────────────────────────────────────────────────────────────────

editor::LayerStack* MainWindow::currentStack() const
{
    if (m_activeGroup < 0
        || m_activeGroup >= static_cast<int>(m_groupSet.groups.size())) {
        return nullptr;
    }
    if (!m_textureTabs) return nullptr;
    const int kindIdx = m_textureTabs->currentIndex();
    if (kindIdx < 0
        || kindIdx >= static_cast<int>(m_groupSet.groups[m_activeGroup].stacks.size())) {
        return nullptr;
    }
    return m_groupSet.groups[m_activeGroup].stacks[kindIdx].get();
}

void MainWindow::onUndo()
{
    auto* st = currentStack();
    if (!st) return;
    const editor::PixelRect r = st->history().undo();
    if (r.isEmpty()) return;
    st->recompositeRect(r);
    // Force canvas repaint; doc dirty bits make MaskTextureSet pick this up
    // automatically on the next viewport frame. Project gets marked dirty
    // because undo is a user-driven mutation of state.
    for (auto* c : m_canvases) if (c) c->update();
    markDirty();
    refreshUndoActions();
}

void MainWindow::onRedo()
{
    auto* st = currentStack();
    if (!st) return;
    const editor::PixelRect r = st->history().redo();
    if (r.isEmpty()) return;
    st->recompositeRect(r);
    for (auto* c : m_canvases) if (c) c->update();
    markDirty();
    refreshUndoActions();
}

void MainWindow::refreshUndoActions()
{
    auto* st = currentStack();
    const bool canU = st && st->history().canUndo();
    const bool canR = st && st->history().canRedo();
    if (m_undoAction) m_undoAction->setEnabled(canU);
    if (m_redoAction) m_redoAction->setEnabled(canR);
}
// Texture-size selection dialog + QSettings persistence
// ─────────────────────────────────────────────────────────────────

int MainWindow::loadDefaultTextureSize() const
{
    QSettings s("msTC", "TextureStudio");
    const int v = s.value("textureSize", kFallbackTextureSize).toInt();
    // Clamp to the supported set in case we ever change the options.
    for (int opt : kSizeOptions) if (opt == v) return v;
    return kFallbackTextureSize;
}

void MainWindow::saveDefaultTextureSize(int size) const
{
    QSettings s("msTC", "TextureStudio");
    s.setValue("textureSize", size);
}

int MainWindow::chooseTextureSize(int matCount)
{
    // VRAM math: per material we keep 4 (kinds) × size² × 4 bytes (RGBA8) on
    // the GPU. The 2D canvases share the same docs, so RAM cost is identical
    // (no separate CPU↔GPU copy). Display in MiB so users feel the impact.
    auto vramMb = [matCount](int sz) -> double {
        const double bytesPerGroup = 4.0 * sz * sz * 4.0;   // 4 kinds × pixels × RGBA8
        return (bytesPerGroup * std::max(matCount, 1)) / (1024.0 * 1024.0);
    };

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Choose Texture Resolution"));
    dlg.setMinimumWidth(420);

    auto* vbox = new QVBoxLayout(&dlg);

    auto* hint = new QLabel(
        tr("This model has <b>%1 materials</b>. Each material gets its "
           "own set of 4 control textures (ShadowRate, SubLightRate, "
           "EdgeRate, FaceSDF). Higher resolution = sharper edges, but "
           "VRAM scales quadratically.").arg(matCount), &dlg);
    hint->setWordWrap(true);
    vbox->addWidget(hint);

    auto* form = new QFormLayout();
    auto* group = new QButtonGroup(&dlg);

    const int defaultSize = loadDefaultTextureSize();
    QRadioButton* defaultBtn = nullptr;

    for (int sz : kSizeOptions) {
        QString text;
        if (matCount > 0) {
            text = tr("%1 × %1   —   ~%2 MB VRAM")
                       .arg(sz)
                       .arg(QString::number(vramMb(sz), 'f', 0));
        } else {
            text = QString("%1 × %1").arg(sz);
        }
        auto* rb = new QRadioButton(text, &dlg);
        rb->setProperty("texSize", sz);
        if (sz == defaultSize) {
            rb->setChecked(true);
            defaultBtn = rb;
        }
        // Soft-warn for very large allocations.
        if (matCount > 0 && vramMb(sz) > 4096.0) {
            rb->setToolTip(tr("Over 4 GB VRAM — only enable if your GPU has the "
                              "headroom. RTX 4090 and similar are fine; entry-level "
                              "GPUs may run out."));
        }
        group->addButton(rb);
        form->addRow(rb);
    }
    if (!defaultBtn) {
        // Fallback: pick the closest to kFallbackTextureSize
        if (auto* btn = group->buttons().value(2)) btn->setChecked(true);
    }
    vbox->addLayout(form);

    auto* tip = new QLabel(
        tr("<i>Tip:</i> 1024² is fine for most stylized characters. Pick 4096+ "
           "only if you need pixel-precise edges on close-up shots."), &dlg);
    tip->setWordWrap(true);
    tip->setStyleSheet("color: #888;");
    vbox->addWidget(tip);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    vbox->addWidget(buttons);

    // Center over the main window. Qt 6 sometimes parks dialogs at the edge
    // of the parent's screen rect on multi-monitor setups. We do NOT call
    // adjustSize() — that can collapse the dialog to title-bar height before
    // the QFormLayout has resolved its geometry. Move only.
    const QRect parentGeo = geometry();
    dlg.move(parentGeo.center() - QPoint(210, 200));

    spdlog::info("Showing texture-size dialog");
    if (dlg.exec() != QDialog::Accepted) {
        return 0;
    }

    if (auto* picked = group->checkedButton()) {
        return picked->property("texSize").toInt();
    }
    return kFallbackTextureSize;
}

// ─────────────────────────────────────────────────────────────────
// Project I/O (Phase 7)
// ─────────────────────────────────────────────────────────────────

void MainWindow::markDirty()
{
    if (m_dirty) return;
    m_dirty = true;
    updateWindowTitle();
}

void MainWindow::updateWindowTitle()
{
    QString base = "msTC Texture Studio";
    QString projectPart;
    if (m_currentProjectPath.has_value()) {
        projectPart = QString::fromStdString(
            u8s(m_currentProjectPath->filename().u8string()));
    } else if (!m_currentModelPath.empty()) {
        projectPart = QString::fromStdString(
            u8s(m_currentModelPath.filename().u8string())) + " (no project)";
    }
    if (!projectPart.isEmpty()) {
        base = projectPart + (m_dirty ? " *" : "") + "  —  " + base;
    }
    setWindowTitle(base);
}

void MainWindow::newProject()
{
    clearProject();
    m_currentProjectPath.reset();
    m_dirty = false;
    updateWindowTitle();
    statusBar()->showMessage(tr("New project — import a PMX to get started."));
}

bool MainWindow::saveProject()
{
    if (!m_currentProjectPath.has_value()) return saveProjectAs();
    if (m_groupSet.groups.empty()) {
        statusBar()->showMessage(tr("Nothing to save — no PMX is loaded."));
        return false;
    }

    project::ProjectMeta meta;
    meta.modelPath        = m_currentModelPath;
    meta.textureSize      = m_currentTextureSize;
    meta.activeGroupIndex = m_activeGroup;
    if (m_textureTabs)    meta.activeKind  = static_cast<editor::TextureKind>(m_textureTabs->currentIndex());
    if (m_channelCombo)   meta.channelView = m_channelCombo->currentData().toInt();
    if (m_viewport)       meta.viewMode    = static_cast<int>(m_viewport->viewMode());
    if (m_autoSwitchCheckbox) meta.autoSwitchGroup = m_autoSwitchCheckbox->isChecked();
    if (m_viewport) {
        const auto& cam = m_viewport->camera();
        meta.cameraYaw      = cam.yaw();
        meta.cameraPitch    = cam.pitch();
        meta.cameraDistance = cam.distance();
        meta.cameraTarget   = cam.target();
    }

    std::string err;
    if (!project::saveProject(*m_currentProjectPath, meta, m_groupSet, err)) {
        QMessageBox::critical(this, tr("Save Project"),
            tr("Failed to save:\n%1").arg(QString::fromStdString(err)));
        return false;
    }

    m_dirty = false;
    updateWindowTitle();
    pushRecentProject(*m_currentProjectPath);
    statusBar()->showMessage(QString("Saved: %1")
        .arg(QString::fromStdString(u8s(m_currentProjectPath->filename().u8string()))));
    return true;
}

bool MainWindow::saveProjectAs()
{
    if (m_groupSet.groups.empty()) {
        statusBar()->showMessage(tr("Nothing to save — no PMX is loaded."));
        return false;
    }
    QString suggested;
    if (m_currentProjectPath.has_value()) {
        suggested = QString::fromStdU16String(m_currentProjectPath->u16string());
    } else if (!m_currentModelPath.empty()) {
        const auto base = m_currentModelPath.parent_path()
                        / m_currentModelPath.stem();
        suggested = QString::fromStdU16String(base.u16string()) + kProjectExt;
    }
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save Project As"), suggested, kProjectFilter);
    if (path.isEmpty()) return false;

    if (!path.endsWith(kProjectExt, Qt::CaseInsensitive)) path += kProjectExt;
    m_currentProjectPath = std::filesystem::path(path.toStdU16String());
    return saveProject();
}

bool MainWindow::loadProject(const std::filesystem::path& projectPath)
{
    project::ProjectMeta meta;
    std::string err;
    if (!project::readProjectHeader(projectPath, meta, err)) {
        QMessageBox::critical(this, tr("Open Project"),
            tr("Failed to read header:\n%1").arg(QString::fromStdString(err)));
        return false;
    }

    if (meta.modelPath.empty() || !std::filesystem::exists(meta.modelPath)) {
        QMessageBox::warning(this, tr("Open Project"),
            tr("Model file not found:\n%1\n\nPick the PMX manually.")
                .arg(QString::fromStdString(u8s(meta.modelPath.u8string()))));
        QString manual = QFileDialog::getOpenFileName(
            this, tr("Locate PMX Model"), QString(), tr("PMX Models (*.pmx)"));
        if (manual.isEmpty()) return false;
        meta.modelPath = std::filesystem::path(manual.toStdU16String());
    }

    // openPmx clears any existing state and reads the PMX at the requested size.
    if (!openPmx(meta.modelPath, meta.textureSize)) {
        QMessageBox::critical(this, tr("Open Project"), tr("PMX load failed."));
        return false;
    }

    if (!project::loadProjectMasks(projectPath, m_groupSet, meta, err)) {
        QMessageBox::warning(this, tr("Open Project"),
            tr("Mask data load reported errors:\n%1").arg(QString::fromStdString(err)));
        // continue anyway — some masks may have loaded
    }

    // Restore UI state.
    if (meta.activeGroupIndex >= 0
        && meta.activeGroupIndex < static_cast<int>(m_groupSet.groups.size())) {
        m_activeGroup = meta.activeGroupIndex;
        if (m_viewport) m_viewport->setActiveGroup(m_activeGroup);
        m_groupCombo->setCurrentIndex(m_activeGroup);
        applyActiveGroupToCanvases();
    }
    if (m_textureTabs) m_textureTabs->setCurrentIndex(static_cast<int>(meta.activeKind));
    if (m_channelCombo) {
        for (int i = 0; i < m_channelCombo->count(); ++i) {
            if (m_channelCombo->itemData(i).toInt() == meta.channelView) {
                m_channelCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    if (m_viewport) m_viewport->setViewMode(static_cast<ViewMode>(meta.viewMode));
    if (m_autoSwitchCheckbox) m_autoSwitchCheckbox->setChecked(meta.autoSwitchGroup);
    if (m_viewport) {
        auto& cam = m_viewport->camera();
        cam.setTarget(meta.cameraTarget);
        cam.setYaw(meta.cameraYaw);
        cam.setPitch(meta.cameraPitch);
        cam.setDistance(meta.cameraDistance);
    }

    m_currentProjectPath = projectPath;
    m_dirty = false;
    updateWindowTitle();
    pushRecentProject(projectPath);
    statusBar()->showMessage(QString("Opened project: %1")
        .arg(QString::fromStdString(u8s(projectPath.filename().u8string()))));

    // Phase 9: undo history is in-memory only; reload starts a fresh slate
    // (matches Photoshop default).
    for (auto& g : m_groupSet.groups) {
        for (auto& s : g.stacks) {
            if (s) s->history().clear();
        }
    }
    refreshUndoActions();

    return true;
}

bool MainWindow::maybePromptSaveDirty()
{
    if (!m_dirty) return true;
    auto rc = QMessageBox::question(this,
        tr("Unsaved Changes"),
        tr("You have unsaved changes. Save before continuing?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (rc == QMessageBox::Cancel)  return false;
    if (rc == QMessageBox::Discard) return true;
    return saveProject();
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (maybePromptSaveDirty()) {
        e->accept();
    } else {
        e->ignore();
    }
}

// ─────────────────────────────────────────────────────────────────
// PNG Export
// ─────────────────────────────────────────────────────────────────

bool MainWindow::exportActiveTexture()
{
    if (m_activeGroup < 0 || m_activeGroup >= static_cast<int>(m_groupSet.groups.size())) {
        statusBar()->showMessage(tr("Nothing to export — load a PMX first."));
        return false;
    }
    const auto& g = m_groupSet.groups[m_activeGroup];
    const auto kind = m_textureTabs
        ? static_cast<editor::TextureKind>(m_textureTabs->currentIndex())
        : editor::TextureKind::ShadowRate;
    const auto* doc = g.doc(kind);
    if (!doc) return false;

    QString suggested = QString::fromStdString(
        g.name + "_" + editor::textureKindShortName(kind) + ".png");
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export Texture"), suggested, tr("PNG Image (*.png)"));
    if (path.isEmpty()) return false;
    if (!path.endsWith(".png", Qt::CaseInsensitive)) path += ".png";

    std::string err;
    if (!editor::exportTextureToPng(*doc,
            std::filesystem::path(path.toStdU16String()), err)) {
        QMessageBox::critical(this, tr("Export Texture"),
            tr("Failed:\n%1").arg(QString::fromStdString(err)));
        return false;
    }
    statusBar()->showMessage(QString("Exported %1").arg(path));
    return true;
}

bool MainWindow::exportAllTextures()
{
    if (m_groupSet.groups.empty()) {
        statusBar()->showMessage(tr("Nothing to export — load a PMX first."));
        return false;
    }
    QString dir = QFileDialog::getExistingDirectory(this, tr("Export All Textures To..."));
    if (dir.isEmpty()) return false;

    QProgressDialog prog("Exporting textures...", "Cancel", 0,
        static_cast<int>(m_groupSet.groups.size() *
                         static_cast<size_t>(editor::TextureKind::Count)),
        this);
    prog.setWindowModality(Qt::WindowModal);
    prog.setMinimumDuration(0);
    prog.setValue(0);

    // The library function exports everything synchronously; for now we just
    // let it run and update the dialog after each batch completes. A future
    // pass can break it into per-file ticks with QtConcurrent.
    QApplication::processEvents();
    auto result = editor::exportAllToFolder(m_groupSet,
        std::filesystem::path(dir.toStdU16String()));
    prog.setValue(result.totalCount);

    if (!result.errors.empty()) {
        QString msg = tr("%1 of %2 files exported. Errors:\n")
            .arg(result.successCount).arg(result.totalCount);
        int shown = 0;
        for (const auto& [p, err] : result.errors) {
            if (++shown > 5) { msg += tr("…and %1 more\n").arg(result.errors.size() - 5); break; }
            msg += QString("  %1: %2\n")
                .arg(QString::fromStdString(u8s(p.filename().u8string())))
                .arg(QString::fromStdString(err));
        }
        QMessageBox::warning(this, tr("Export All"), msg);
    } else {
        statusBar()->showMessage(tr("Exported %1 textures to %2")
            .arg(result.successCount).arg(dir));
    }
    return result.successCount > 0;
}

// ─────────────────────────────────────────────────────────────────
// Recent projects
// ─────────────────────────────────────────────────────────────────

void MainWindow::loadRecentProjects()
{
    QSettings s("msTC", "TextureStudio");
    m_recentProjects = s.value("recentProjects").toStringList();
    // Prune missing files so the menu never shows stale entries.
    QStringList alive;
    for (const auto& p : m_recentProjects) {
        if (std::filesystem::exists(std::filesystem::path(p.toStdU16String()))) {
            alive.push_back(p);
        }
    }
    if (alive.size() != m_recentProjects.size()) {
        m_recentProjects = alive;
        saveRecentProjects();
    }
}

void MainWindow::saveRecentProjects() const
{
    QSettings s("msTC", "TextureStudio");
    s.setValue("recentProjects", m_recentProjects);
}

void MainWindow::pushRecentProject(const std::filesystem::path& projectPath)
{
    const QString p = QString::fromStdU16String(projectPath.u16string());
    m_recentProjects.removeAll(p);
    m_recentProjects.prepend(p);
    while (m_recentProjects.size() > kMaxRecentProjects) {
        m_recentProjects.removeLast();
    }
    saveRecentProjects();
    rebuildRecentProjectsMenu();
}

void MainWindow::rebuildRecentProjectsMenu()
{
    if (!m_recentMenu) return;
    m_recentMenu->clear();
    if (m_recentProjects.isEmpty()) {
        auto* placeholder = m_recentMenu->addAction(tr("(none)"));
        placeholder->setEnabled(false);
        return;
    }
    for (const QString& p : m_recentProjects) {
        // Show just the basename in the menu; full path in tooltip.
        std::filesystem::path fsPath(p.toStdU16String());
        QString label = QString::fromStdString(u8s(fsPath.filename().u8string()));
        QAction* act = m_recentMenu->addAction(label);
        act->setToolTip(p);
        connect(act, &QAction::triggered, this, [this, p]() {
            if (!maybePromptSaveDirty()) return;
            const std::filesystem::path fsPath(p.toStdU16String());
            if (!std::filesystem::exists(fsPath)) {
                QMessageBox::warning(this, tr("Open Recent"),
                    tr("Project no longer exists:\n%1").arg(p));
                m_recentProjects.removeAll(p);
                saveRecentProjects();
                rebuildRecentProjectsMenu();
                return;
            }
            loadProject(fsPath);
        });
    }
}
