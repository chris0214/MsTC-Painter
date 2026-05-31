#pragma once

#include "Camera.h"

#include "../editor/TextureDocument.h"   // for TextureKind

#include <QWidget>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <DirectXMath.h>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace pmx { struct Model; }
namespace editor { struct TextureGroupSet; class BrushEngine; }
class StaticMesh;
class MaskTextureSet;
class QTimer;

using Microsoft::WRL::ComPtr;

/// 3D viewport view modes.
/// Numeric values must match the shader's gViewMode constants.
enum class ViewMode : uint32_t
{
    Lit          = 0,
    MaskOverlay  = 1,
    MaskOnly     = 2,
    UvChecker    = 3,
    MstcPreview  = 4,   ///< L2 reserved, not implemented yet
};

/// QWidget that hosts a Direct3D 11 swap chain and renders a PMX model with
/// optional per-group mask overlay.
class DX11Widget : public QWidget
{
    Q_OBJECT

public:
    explicit DX11Widget(QWidget* parent = nullptr);
    ~DX11Widget() override;

    /// Bind the texture group set BEFORE setModel(). The widget creates its
    /// MaskTextureSet from this on the next setModel call. Pass nullptr to detach.
    void setGroupSet(editor::TextureGroupSet* groups);

    /// Build GPU mesh from an already-parsed PMX model. Requires a prior
    /// setGroupSet() call so per-segment group indices can be filled.
    bool setModel(const pmx::Model& model, const std::filesystem::path& modelDir);

    /// Active mask kind to display (synced with the active canvas tab).
    void setActiveKind(editor::TextureKind kind);

    /// Switch view mode (Lit / MaskOverlay / MaskOnly / UvChecker).
    void setViewMode(ViewMode m);
    ViewMode viewMode() const { return m_viewMode; }

    /// Channel view filter for the mask (matches the 2D canvas's ChannelView).
    /// 0 = RGB composite, 1 = R only, 2 = G only, 3 = B only, 4 = Semantic.
    void setChannelView(uint32_t cv) { m_channelView = cv; }
    uint32_t channelView() const     { return m_channelView; }

    /// Provide a brush engine for 3D paint. Same instance the 2D canvas uses
    /// — settings stay in sync automatically. Pass nullptr to disable 3D paint.
    void setBrush(editor::BrushEngine* brush) { m_brush = brush; }

    /// Tell the widget which group is currently active in the left-side UI.
    /// Used to filter 3D paint when `setRestrictPaintToActiveGroup(true)`:
    /// clicks on triangles belonging to other groups are ignored, so the
    /// user can keep painting on group A in 3D without the brush leaking
    /// onto group B even when group B's geometry is between camera and A.
    void setActiveGroup(int groupIndex) { m_activeGroup = groupIndex; }
    int  activeGroup() const            { return m_activeGroup; }

    /// When true, 3D paint clicks/drags are constrained to the active group.
    /// Hits on other groups are silently ignored. When false (default), the
    /// click latches onto whatever group it hit and paints there — original
    /// Phase 6 behavior.
    void setRestrictPaintToActiveGroup(bool on) { m_restrictPaintToActiveGroup = on; }
    bool restrictPaintToActiveGroup() const     { return m_restrictPaintToActiveGroup; }

    /// Read-only access to the orbit camera. Used by project save to record
    /// yaw/pitch/distance/target so view state can be restored on reload.
    const OrbitCamera& camera() const { return m_camera; }
    /// Mutable camera (for project load to restore view).
    OrbitCamera&       camera()       { return m_camera; }

    /// Multiplier on orbit / pan / zoom drag deltas. 1.0 = baseline (Blender-
    /// like default in OrbitCamera). User-adjustable via the viewport
    /// toolbar slider; persisted in QSettings.
    void  setNavSensitivity(float s) { m_navSensitivity = s; }
    float navSensitivity() const     { return m_navSensitivity; }

    /// Show the orange "orbit pivot" disc on the mesh surface. Default ON
    /// when a model is loaded; user can toggle via the viewport bar.
    void setShowPivotMarker(bool on) { m_showPivotMarker = on; }
    bool showPivotMarker() const     { return m_showPivotMarker; }

    /// Arm "set pivot" mode. The next left-click on the model will move the
    /// orbit pivot to the hit point INSTEAD of starting a paint stroke. After
    /// one click the mode disarms automatically. Equivalent to pressing F
    /// at the cursor position; provided as a discoverable toolbar button.
    void armSetPivotMode();
    bool isSetPivotArmed() const { return m_setPivotArmed; }

signals:
    /// Emitted when set-pivot mode disarms (either consumed by a click or
    /// cancelled). Lets the host un-press the toolbar button.
    void setPivotModeChanged(bool armed);

    /// Emitted when a stroke begins on a group (or crosses to a new group
    /// mid-stroke). The host can auto-swap the active Texture combo to match.
    void paintHitGroup(int groupIndex);

    /// Emitted at the end of every 3D-painted stroke.
    void strokeFinished();

public:
    // QWidget overrides
    QPaintEngine* paintEngine() const override { return nullptr; }

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;

    void mousePressEvent(QMouseEvent*)        override;
    void mouseDoubleClickEvent(QMouseEvent*)  override;
    void mouseMoveEvent(QMouseEvent*)         override;
    void mouseReleaseEvent(QMouseEvent*)      override;
    void wheelEvent(QWheelEvent*)             override;
    void keyPressEvent(QKeyEvent*)            override;
    void leaveEvent(QEvent*)                  override;

private:
    bool initDevice();
    bool createSwapChain();
    bool createRenderTarget();
    void releaseRenderTarget();

    bool createShaders(std::string& err);
    bool createConstantBuffers();
    bool createRasterAndDepthStates();

    void render();
    void handleResize();

    /// Raycast from the camera through the orbit pivot's projected screen
    /// position and snap the pivot's world position to the resulting hit.
    /// Used after pan so the pivot stays anchored to the model surface
    /// instead of drifting into empty space. No-op if the projection lands
    /// on background (no mesh hit).
    void snapPivotToSurface();

    /// Compute the local "world units per UV unit" scale at a triangle.
    /// Used to convert the brush's texel radius into a world-space radius
    /// for the 3D cursor ring so it matches the actual painted footprint.
    /// Falls back to a heuristic when the triangle is degenerate in UV.
    float uvToWorldScale(int triangleIndex) const;

    // ── D3D11 core ──────────────────────────────────────────────
    ComPtr<ID3D11Device>           m_device;
    ComPtr<ID3D11DeviceContext>    m_context;
    ComPtr<IDXGISwapChain1>        m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11DepthStencilView> m_dsv;
    ComPtr<ID3D11Texture2D>        m_depthBuffer;

    // ── Pipeline state ──────────────────────────────────────────
    ComPtr<ID3D11VertexShader>      m_vsBasic;
    ComPtr<ID3D11PixelShader>       m_psBasic;
    ComPtr<ID3D11InputLayout>       m_inputLayout;
    ComPtr<ID3D11SamplerState>      m_linearSampler;
    ComPtr<ID3D11RasterizerState>   m_rsCull;
    ComPtr<ID3D11RasterizerState>   m_rsNoCull;
    ComPtr<ID3D11DepthStencilState> m_dssDefault;
    ComPtr<ID3D11BlendState>        m_bsAlpha;

    ComPtr<ID3D11Buffer> m_cbPerFrame;
    ComPtr<ID3D11Buffer> m_cbPerMaterial;
    ComPtr<ID3D11Buffer> m_cbPerObject;

    // ── Scene ───────────────────────────────────────────────────
    std::unique_ptr<StaticMesh>     m_mesh;
    std::unique_ptr<MaskTextureSet> m_maskSet;
    OrbitCamera                     m_camera;

    /// Non-owning. Required for syncDirty.
    editor::TextureGroupSet* m_groups = nullptr;

    /// Non-owning brush engine shared with the 2D canvases. nullptr until
    /// MainWindow calls setBrush() — 3D paint is disabled while null.
    editor::BrushEngine* m_brush = nullptr;

    /// Active mask kind to display in MaskOverlay / MaskOnly modes.
    editor::TextureKind m_activeKind = editor::TextureKind::ShadowRate;

    /// Current view mode.
    ViewMode m_viewMode = ViewMode::MaskOverlay;

    /// Current mask channel filter (matches editor::ChannelView numerically).
    uint32_t m_channelView = 0;   // 0 = RGB composite

    // ── State ───────────────────────────────────────────────────
    bool m_deviceReady   = false;
    bool m_resizePending = false;

    QTimer* m_renderTimer = nullptr;

    // Mouse drag state:
    //   Right-button → Orbit
    //   Middle-button → Pan
    //   Left-button → Paint (when raycast hits)
    enum class DragMode { None, Orbit, Pan, Paint };
    DragMode m_drag = DragMode::None;
    int      m_lastMouseX = 0;
    int      m_lastMouseY = 0;

    // Paint state (only valid while m_drag == Paint)
    int      m_paintGroupIndex = -1;
    DirectX::XMFLOAT2 m_lastPaintUv { -1, -1 };
    DirectX::XMFLOAT3 m_lastHitWorld { 0, 0, 0 };
    bool     m_lastHitValid = false;
    /// Dirty-box accumulator for BrushEngine (Step 1: sink only, unused).
    editor::PixelRect m_pendingDirtyBox{};

    /// Currently-active group in the left-side UI. -1 = no model loaded.
    /// Setter pushed by MainWindow whenever the Texture combo changes.
    int      m_activeGroup = -1;
    /// When true, mousePress + mouseMove ignore raycast hits on groups
    /// other than m_activeGroup. Bound to MainWindow's auto-switch checkbox.
    bool     m_restrictPaintToActiveGroup = false;

    // Hover state — used to draw the brush cursor ring even when not painting.
    // Updated by mouseMoveEvent when not in an active drag.
    bool                m_hoverHitValid    = false;
    DirectX::XMFLOAT3   m_hoverHitWorldPos { 0, 0, 0 };
    int                 m_hoverHitTriIdx   = -1;   ///< for UV-scale lookup
    int                 m_paintHitTriIdx   = -1;   ///< triangle of last paint stamp

    /// Orbit / pan / zoom sensitivity multiplier (× the engine baseline).
    /// 1.0 = default. Persisted via MainWindow into QSettings.
    float m_navSensitivity = 1.0f;

    /// Whether to render the orbit-pivot marker disc. ON by default; user
    /// toggles via the viewport bar checkbox.
    bool m_showPivotMarker = true;

    /// "Set pivot" mode armed: next left-click on the model moves the orbit
    /// target instead of painting. Cleared after consumption.
    bool m_setPivotArmed = false;
};
