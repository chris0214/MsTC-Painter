#pragma once

#include "../editor/TextureDocument.h"
#include "../editor/TextureGroup.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <vector>

/// GPU-side mask textures: one per (TextureGroup × TextureKind).
///
/// On PMX load, `build()` allocates one DEFAULT-usage R8G8B8A8 texture per
/// group×kind (matching the group's docs[kind]->size()). Each frame, the
/// renderer calls `syncDirty()` which uploads any tile-dirty regions from
/// the CPU TextureDocuments to their corresponding GPU textures.
///
/// Texture lookup is by (groupIndex, kind) and always returns a valid SRV
/// — `build()` guarantees full population for every group.
class MaskTextureSet
{
public:
    MaskTextureSet();
    ~MaskTextureSet();

    /// Allocate one DX11 texture+SRV per group×kind. Replaces any prior data.
    /// Initial GPU contents = the docs' current pixels (so initial state is
    /// correct, e.g. after a project load).
    bool build(ID3D11Device* device, const editor::TextureGroupSet& groups, std::string& outError);

    /// Drain GPU-dirty bounds from each TextureDocument and UpdateSubresource
    /// to the corresponding GPU texture. Cheap when nothing changed.
    void syncDirty(ID3D11DeviceContext* ctx, editor::TextureGroupSet& groups);

    /// SRV lookup. Returns nullptr only if `build()` hasn't been called or
    /// indices are out of range.
    ID3D11ShaderResourceView* srv(int groupIndex, editor::TextureKind kind) const;

    /// 1×1 black texture used as a fallback when nothing else is available.
    ID3D11ShaderResourceView* fallbackSrv() const { return m_fallback.Get(); }

    /// Number of groups currently held.
    int groupCount() const { return static_cast<int>(m_groupTextures.size()); }

    /// Free all GPU resources. Called by destructor or before rebuild.
    void release();

private:
    static constexpr size_t kKinds = static_cast<size_t>(editor::TextureKind::Count);

    /// Per-(group, kind) GPU texture + SRV.
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D>          tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        int size = 0;
    };

    /// One row per group; each row has kKinds slots.
    std::vector<std::array<Slot, kKinds>> m_groupTextures;

    /// 1×1 black RGBA fallback (used by shader binding when no real mask).
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_fallback;
};
