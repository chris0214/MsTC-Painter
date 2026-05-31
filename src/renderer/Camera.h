#pragma once

#include <DirectXMath.h>

/// Orbit camera around a target point.
/// Yaw / pitch / distance, with optional panning of the target.
class OrbitCamera
{
public:
    OrbitCamera();

    // ── Interaction (called by DX11Widget mouse events) ────────
    void orbit(float dxPixels, float dyPixels);
    void pan(float dxPixels, float dyPixels);
    void zoom(float wheelDelta);    // wheelDelta in Qt units (120 per notch)

    // ── View presets (Blender-style numpad) ────────────────────
    enum class StandardView : uint8_t
    {
        Front,   ///< Numpad 1 — looking down +Z (model's front)
        Back,    ///< Ctrl+Numpad 1
        Right,   ///< Numpad 3 — looking down -X
        Left,    ///< Ctrl+Numpad 3
        Top,     ///< Numpad 7 — looking down -Y
        Bottom,  ///< Ctrl+Numpad 7
    };
    /// Snap yaw/pitch to a standard axis-aligned view. Distance & target preserved.
    void setStandardView(StandardView v);

    /// Toggle between perspective and orthographic projection (Numpad 5).
    void toggleOrtho()         { m_ortho = !m_ortho; }
    void setOrtho(bool on)     { m_ortho = on; }
    bool isOrtho() const       { return m_ortho; }

    /// Move the orbit pivot (target) to a world-space point and adjust
    /// distance to a comfortable viewing range. Used by F-key focus.
    void focusOn(DirectX::XMFLOAT3 worldPos);

    // ── Setup ──────────────────────────────────────────────────
    void setViewport(int w, int h);
    void setTarget(DirectX::XMFLOAT3 t)   { m_target = t; if (!m_pivotLocked) m_orbitPivot = t; }
    void setDistance(float d)             { m_distance = d; }
    void setYaw(float y)                  { m_yaw = y; }
    void setPitch(float p)                { m_pitch = p; }
    void resetToFit(DirectX::XMFLOAT3 center, float radius);

    // ── Pivot lock ─────────────────────────────────────────────
    /// When ON: orbit always rotates around `orbitPivot()` (the locked point)
    /// instead of the camera's lookAt. Pan can still slide the view, but the
    /// orbit pivot stays put — so subsequent rotations come back around the
    /// same spot you locked. When OFF (default), orbit pivot follows lookAt
    /// (original behavior).
    void setPivotLocked(bool on);
    bool pivotLocked() const              { return m_pivotLocked; }
    DirectX::XMFLOAT3 orbitPivot() const  { return m_orbitPivot; }

    // ── Matrices ───────────────────────────────────────────────
    DirectX::XMMATRIX view()       const;
    DirectX::XMMATRIX projection() const;
    DirectX::XMMATRIX viewProj()   const { return DirectX::XMMatrixMultiply(view(), projection()); }

    DirectX::XMFLOAT3 eyePosition() const;
    DirectX::XMFLOAT3 target()      const { return m_target; }
    float             yaw()         const { return m_yaw; }
    float             pitch()       const { return m_pitch; }
    float             distance()    const { return m_distance; }

private:
    DirectX::XMFLOAT3 m_target { 0, 10, 0 };       // camera lookAt
    DirectX::XMFLOAT3 m_orbitPivot { 0, 10, 0 };   // orbit center (== target when unlocked)
    bool  m_pivotLocked = false;
    float m_yaw       = 0.0f;        // radians
    float m_pitch     = 0.2f;        // radians
    float m_distance  = 30.0f;

    float m_fovY      = DirectX::XMConvertToRadians(45.0f);
    float m_nearZ     = 0.1f;
    float m_farZ      = 500.0f;

    int   m_viewW = 1;
    int   m_viewH = 1;

    bool  m_ortho = false;   ///< Perspective by default; Numpad 5 toggles
};
