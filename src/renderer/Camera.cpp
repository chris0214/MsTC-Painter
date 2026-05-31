#include "Camera.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

OrbitCamera::OrbitCamera() = default;

// ─────────────────────────────────────────────────────────────────
// Interaction
// ─────────────────────────────────────────────────────────────────

void OrbitCamera::orbit(float dxPixels, float dyPixels)
{
    // Sensitivity: ~0.3°/px feels like Blender's turntable. The previous
    // 0.01 rad/px (~0.57°/px) was almost twice that and made the camera
    // feel jumpy — small wrist movements would spin past the target.
    constexpr float k = 0.005f;
    constexpr float PITCH_LIMIT = XM_PIDIV2 - 0.01f;

    if (!m_pivotLocked) {
        // Standard orbit around m_target. Keep orbitPivot in sync so a
        // future "lock" snapshot sees the right anchor.
        m_yaw   += dxPixels * k;
        m_pitch += dyPixels * k;
        m_pitch  = std::clamp(m_pitch, -PITCH_LIMIT, PITCH_LIMIT);
        m_orbitPivot = m_target;
        return;
    }

    // ── Locked orbit ───────────────────────────────────────────
    // Rotate eye AND lookAt rigidly around m_orbitPivot. After the rotation,
    // re-derive yaw/pitch/distance from the new (eye → lookAt) so the
    // existing eyePosition() formula still produces the post-rotation eye.
    XMFLOAT3 eyeF = eyePosition();
    XMVECTOR vEye = XMLoadFloat3(&eyeF);
    XMVECTOR vTgt = XMLoadFloat3(&m_target);
    XMVECTOR vPiv = XMLoadFloat3(&m_orbitPivot);

    XMVECTOR fwd  = XMVectorSubtract(vTgt, vEye);
    if (XMVectorGetX(XMVector3LengthSq(fwd)) < 1e-10f) return;
    fwd = XMVector3Normalize(fwd);

    XMVECTOR upW   = XMVectorSet(0, 1, 0, 0);
    XMVECTOR right = XMVector3Cross(upW, fwd);
    if (XMVectorGetX(XMVector3LengthSq(right)) < 1e-6f) return;   // gimbal
    right = XMVector3Normalize(right);

    // Yaw around world Y, then pitch around the camera's right axis.
    // Same axes the unlocked-orbit parameterization uses, just applied to
    // (eye-pivot) and (target-pivot) instead of just (eye-target).
    const XMMATRIX Ryaw   = XMMatrixRotationY(dxPixels * k);
    const XMMATRIX Rpitch = XMMatrixRotationAxis(right, dyPixels * k);
    const XMMATRIX R      = XMMatrixMultiply(Ryaw, Rpitch);

    const XMVECTOR offEye = XMVectorSubtract(vEye, vPiv);
    const XMVECTOR offTgt = XMVectorSubtract(vTgt, vPiv);
    const XMVECTOR newEye = XMVectorAdd(vPiv, XMVector3TransformNormal(offEye, R));
    const XMVECTOR newTgt = XMVectorAdd(vPiv, XMVector3TransformNormal(offTgt, R));

    XMStoreFloat3(&m_target, newTgt);

    // Re-derive yaw / pitch / distance so eyePosition() returns newEye.
    const XMVECTOR dirVec = XMVectorSubtract(newEye, newTgt);
    const float dist = XMVectorGetX(XMVector3Length(dirVec));
    if (dist > 1e-5f) {
        m_distance = dist;
        XMFLOAT3 dirF;
        XMStoreFloat3(&dirF, XMVectorScale(dirVec, 1.0f / dist));
        // dir = (sin(yaw)cos(pitch), sin(pitch), cos(yaw)cos(pitch))
        m_pitch = std::asin(std::clamp(dirF.y, -1.0f, 1.0f));
        m_yaw   = std::atan2(dirF.x, dirF.z);
        m_pitch = std::clamp(m_pitch, -PITCH_LIMIT, PITCH_LIMIT);
    }
}

void OrbitCamera::pan(float dxPixels, float dyPixels)
{
    // Pan scale: world units per pixel, scaled by current distance so
    // far-away pans don't feel like crawling. The 0.001 base feels close
    // to Blender at typical character-paint distances; user can hold Shift
    // (DX11Widget applies a ×0.2 multiplier) for finer control.
    const float panScale = m_distance * 0.001f;

    XMFLOAT3 eye = eyePosition();
    XMVECTOR vEye = XMLoadFloat3(&eye);
    XMVECTOR vTgt = XMLoadFloat3(&m_target);
    XMVECTOR vUp  = XMVectorSet(0, 1, 0, 0);

    XMVECTOR forward = XMVector3Normalize(XMVectorSubtract(vTgt, vEye));
    XMVECTOR right   = XMVector3Normalize(XMVector3Cross(vUp, forward));
    XMVECTOR upTrue  = XMVector3Cross(forward, right);

    // "Drag the model" convention: the cursor grabs the model and the
    // model follows the cursor in screen space. Camera target moves the
    // OPPOSITE direction so the model appears to translate with the mouse.
    XMVECTOR delta = XMVectorAdd(
        XMVectorScale(right, -dxPixels * panScale),
        XMVectorScale(upTrue, dyPixels * panScale)
    );

    XMStoreFloat3(&m_target, XMVectorAdd(vTgt, delta));
    if (!m_pivotLocked) {
        // Unlocked: orbit pivot tracks lookAt.
        m_orbitPivot = m_target;
    }
    // Locked: m_orbitPivot stays where the user locked it. Pan slides the
    // view but the orbit anchor is preserved, so the next orbit comes
    // back around the locked spot.
}

void OrbitCamera::zoom(float wheelDelta)
{
    // Each notch = 120; one notch -> 10% closer/farther
    const float steps = wheelDelta / 120.0f;
    const float factor = std::pow(0.9f, steps);
    m_distance = std::clamp(m_distance * factor, 0.5f, 500.0f);
}

// ─────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────

void OrbitCamera::setViewport(int w, int h)
{
    m_viewW = (w < 1) ? 1 : w;
    m_viewH = (h < 1) ? 1 : h;
}

void OrbitCamera::resetToFit(XMFLOAT3 center, float radius)
{
    m_target     = center;
    m_orbitPivot = center;
    m_pivotLocked = false;   // full reset releases the lock too
    m_yaw        = 0.0f;
    m_pitch      = 0.15f;
    // Pull camera back so the bounding sphere fully fits in vertical FOV
    m_distance = std::max(radius / std::tan(m_fovY * 0.5f), 1.0f) * 1.2f;

    // Update far plane so we don't z-clip large models
    m_farZ = std::max(m_farZ, m_distance * 4.0f + radius * 4.0f);
}

// ─────────────────────────────────────────────────────────────────
// Matrices
// ─────────────────────────────────────────────────────────────────

XMFLOAT3 OrbitCamera::eyePosition() const
{
    const float cp = std::cos(m_pitch);
    const float sp = std::sin(m_pitch);
    const float cy = std::cos(m_yaw);
    const float sy = std::sin(m_yaw);

    XMFLOAT3 dir { sy * cp, sp, cy * cp };
    return {
        m_target.x + dir.x * m_distance,
        m_target.y + dir.y * m_distance,
        m_target.z + dir.z * m_distance,
    };
}

XMMATRIX OrbitCamera::view() const
{
    XMFLOAT3 eye = eyePosition();
    return XMMatrixLookAtLH(
        XMLoadFloat3(&eye),
        XMLoadFloat3(&m_target),
        XMVectorSet(0, 1, 0, 0)
    );
}

XMMATRIX OrbitCamera::projection() const
{
    const float aspect = static_cast<float>(m_viewW) / static_cast<float>(m_viewH);
    if (m_ortho) {
        // Ortho: derive view-frustum height from current distance + fov so
        // toggling between perspective and ortho doesn't visually jump much.
        const float viewH = 2.0f * m_distance * std::tan(m_fovY * 0.5f);
        const float viewW = viewH * aspect;
        return XMMatrixOrthographicLH(viewW, viewH, m_nearZ, m_farZ);
    }
    return XMMatrixPerspectiveFovLH(m_fovY, aspect, m_nearZ, m_farZ);
}

// ─────────────────────────────────────────────────────────────────
// View presets + focus
// ─────────────────────────────────────────────────────────────────

void OrbitCamera::setStandardView(StandardView v)
{
    // yaw and pitch defined by eyePosition() formula:
    //   dir = (sin(yaw)*cos(pitch), sin(pitch), cos(yaw)*cos(pitch))
    // eye = target + dir * distance, looking back at target with world-up Y.
    // We want eye at +Z for "Front" (so the camera looks toward -Z, seeing
    // the model's +Z face). MMD models face +Z by convention, so Front =
    // see the character's face.
    switch (v) {
    case StandardView::Front:  m_yaw = 0.0f;          m_pitch = 0.0f; break;
    case StandardView::Back:   m_yaw = DirectX::XM_PI; m_pitch = 0.0f; break;
    case StandardView::Right:  m_yaw = -DirectX::XM_PIDIV2; m_pitch = 0.0f; break;
    case StandardView::Left:   m_yaw =  DirectX::XM_PIDIV2; m_pitch = 0.0f; break;
    case StandardView::Top:    m_yaw = 0.0f; m_pitch =  DirectX::XM_PIDIV2 - 0.001f; break;
    case StandardView::Bottom: m_yaw = 0.0f; m_pitch = -(DirectX::XM_PIDIV2 - 0.001f); break;
    }
}

void OrbitCamera::focusOn(XMFLOAT3 worldPos)
{
    // Move both the lookAt and the orbit pivot to the new world position.
    // Don't touch m_distance — preserves the user's current zoom level.
    // Lock state is preserved: focusing while locked just relocates the
    // locked pivot to a new spot (still locked there).
    m_target     = worldPos;
    m_orbitPivot = worldPos;
}

void OrbitCamera::setPivotLocked(bool on)
{
    if (m_pivotLocked == on) return;
    m_pivotLocked = on;
    if (on) {
        // Snapshot the current lookAt as the locked orbit center. Subsequent
        // pans drift m_target but m_orbitPivot stays here.
        m_orbitPivot = m_target;
    } else {
        // Releasing the lock — re-sync the pivot to the (possibly panned)
        // lookAt so the next orbit feels natural (around what you see).
        m_orbitPivot = m_target;
    }
}
