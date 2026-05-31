#pragma once

#include "../pmx/PmxTypes.h"
#include "../editor/TextureGroup.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <DirectXMath.h>
#include <filesystem>
#include <functional>
#include <vector>

/// A static (non-skinned) mesh built from a PMX model.
/// Holds GPU vertex/index buffers + per-material draw segments + diffuse textures.
///
/// Z axis is flipped (PMX is right-handed, we render LH) and triangle winding
/// is reversed accordingly.
class StaticMesh
{
public:
    StaticMesh();
    ~StaticMesh();

    /// Build GPU resources from an in-memory PMX model.
    /// `modelDir` is used to resolve relative texture paths.
    /// `groups` is used to attach a TextureGroup index to each draw segment
    /// so the renderer can bind the right per-group mask texture later.
    bool build(ID3D11Device*                  device,
               const pmx::Model&              model,
               const std::filesystem::path&   modelDir,
               const editor::TextureGroupSet& groups,
               std::string&                   outError);

    /// Draw the whole model. For each segment, `cbMaterial` is updated with the
    /// segment's material constants, diffuse is bound to t0 (white fallback if
    /// none), and the optional `bindMaskForGroup` callback is invoked with the
    /// segment's group index — implementations should bind the appropriate
    /// per-group mask SRV to t1 (and any other supplementary slots).
    void draw(ID3D11DeviceContext* ctx,
              ID3D11Buffer*        cbMaterial,
              const std::function<void(int groupIndex)>& bindMaskForGroup) const;

    // Bounding sphere of the mesh (for camera fitting)
    DirectX::XMFLOAT3 boundsCenter() const { return m_boundsCenter; }
    float             boundsRadius() const { return m_boundsRadius; }

    bool isReady() const { return m_indexCount > 0; }

    // ── CPU-side accessors (Phase 6 — for raycast) ─────────────
    /// Number of triangles (0 if not built).
    int triangleCount() const { return static_cast<int>(m_cpuIndices.size() / 3); }

    /// Get the three world-space (LH, post-Z-flip) vertex positions of a tri.
    bool getTriPositions(int triIdx, DirectX::XMFLOAT3& v0,
                                     DirectX::XMFLOAT3& v1,
                                     DirectX::XMFLOAT3& v2) const;

    /// Get the three UVs of a tri (in [0,1]).
    bool getTriUvs(int triIdx, DirectX::XMFLOAT2& uv0,
                               DirectX::XMFLOAT2& uv1,
                               DirectX::XMFLOAT2& uv2) const;

    /// Group index for a triangle, or -1 if its material has no group.
    int triangleGroup(int triIdx) const
    {
        if (triIdx < 0 || triIdx >= static_cast<int>(m_triGroup.size())) return -1;
        return m_triGroup[triIdx];
    }

private:
    bool createBuffers(ID3D11Device*                  device,
                       const pmx::Model&              model,
                       const editor::TextureGroupSet& groups,
                       std::string&                   err);
    bool createTextures(ID3D11Device* device, const pmx::Model& model,
                        const std::filesystem::path& modelDir, std::string& err);

    struct Material
    {
        DirectX::XMFLOAT4 diffuse;
        DirectX::XMFLOAT3 ambient;
        float             _pad0;
        DirectX::XMFLOAT3 specular;
        float             shininess;
    };

    static_assert(sizeof(Material) % 16 == 0, "constant buffer must be 16-byte aligned");

    struct DrawSegment
    {
        UINT     indexStart;
        UINT     indexCount;
        Material material;
        int      diffuseTextureIndex;  // index into m_textures, or -1
        int      groupIndex;           // index into TextureGroupSet, or -1 if no group
        bool     noCull;
        bool     hasAlpha;
    };

    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_ib;
    UINT m_indexCount = 0;

    // CPU-side mesh kept for raycast (used by 3D paint). Coords are LH
    // (Z flipped) and winding is reversed, matching the GPU mesh exactly so
    // raycast results align with what the user sees on screen.
    struct CpuVertex { DirectX::XMFLOAT3 pos; DirectX::XMFLOAT2 uv; };
    std::vector<CpuVertex> m_cpuVerts;
    std::vector<uint32_t>  m_cpuIndices;     // 3 per triangle, post-winding-flip
    std::vector<int>       m_triGroup;       // group index per triangle, -1 if none

    std::vector<DrawSegment> m_segments;
    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_textures;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_whiteTexture;

    DirectX::XMFLOAT3 m_boundsCenter { 0, 0, 0 };
    float             m_boundsRadius = 1.0f;
};
