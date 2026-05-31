#include "MaskTextureSet.h"

#include <spdlog/spdlog.h>

using Microsoft::WRL::ComPtr;
using editor::TextureKind;
using editor::TextureGroupSet;
using editor::TextureDocument;

MaskTextureSet::MaskTextureSet()  = default;
MaskTextureSet::~MaskTextureSet() = default;

void MaskTextureSet::release()
{
    m_groupTextures.clear();
    m_fallback.Reset();
}

// ─────────────────────────────────────────────────────────────────
// build
// ─────────────────────────────────────────────────────────────────

bool MaskTextureSet::build(ID3D11Device* device, const TextureGroupSet& groups, std::string& outError)
{
    release();

    if (!device) {
        outError = "MaskTextureSet::build: null device";
        return false;
    }

    // ── 1×1 black fallback SRV ─────────────────────────────────
    {
        const uint8_t black[4] = { 0, 0, 0, 255 };
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 1;
        td.MipLevels = td.ArraySize = 1;
        td.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc = { 1, 0 };
        td.Usage     = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA srd{};
        srd.pSysMem = black;
        srd.SysMemPitch = 4;
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(device->CreateTexture2D(&td, &srd, tex.GetAddressOf()))) {
            outError = "MaskTextureSet: failed to create fallback texture";
            return false;
        }
        if (FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, m_fallback.GetAddressOf()))) {
            outError = "MaskTextureSet: failed to create fallback SRV";
            return false;
        }
    }

    // ── Per-group × per-kind textures ──────────────────────────
    m_groupTextures.resize(groups.groups.size());

    size_t totalAllocated = 0;
    size_t totalBytes     = 0;
    for (size_t gi = 0; gi < groups.groups.size(); ++gi) {
        const auto& g = groups.groups[gi];
        auto& row = m_groupTextures[gi];

        for (size_t k = 0; k < kKinds; ++k) {
            const TextureDocument* doc = g.doc(k);
            if (!doc) continue;
            const int sz = doc->size();

            D3D11_TEXTURE2D_DESC td{};
            td.Width  = sz;
            td.Height = sz;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc = { 1, 0 };
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            // Initial contents from the doc's current pixels
            D3D11_SUBRESOURCE_DATA srd{};
            srd.pSysMem     = doc->pixels();
            srd.SysMemPitch = sz * 4;

            Slot& slot = row[k];
            slot.size = sz;
            if (FAILED(device->CreateTexture2D(&td, &srd, slot.tex.GetAddressOf()))) {
                outError = "MaskTextureSet: CreateTexture2D failed";
                return false;
            }
            if (FAILED(device->CreateShaderResourceView(slot.tex.Get(), nullptr,
                                                       slot.srv.GetAddressOf()))) {
                outError = "MaskTextureSet: CreateShaderResourceView failed";
                return false;
            }
            ++totalAllocated;
            totalBytes += static_cast<size_t>(sz) * sz * 4;
        }
    }

    spdlog::info("MaskTextureSet: built {} GPU mask textures ({} MB VRAM)",
                 totalAllocated, totalBytes / (1024 * 1024));
    return true;
}

// ─────────────────────────────────────────────────────────────────
// syncDirty
// ─────────────────────────────────────────────────────────────────

void MaskTextureSet::syncDirty(ID3D11DeviceContext* ctx, TextureGroupSet& groups)
{
    if (!ctx) return;

    // For each group × kind, drain the doc's GPU-dirty bounds and upload that
    // sub-rect. Note we use the GPU consumer slot, which is independent of
    // the canvas's own dirty tracking.
    for (size_t gi = 0; gi < groups.groups.size() && gi < m_groupTextures.size(); ++gi) {
        auto& g = groups.groups[gi];
        auto& row = m_groupTextures[gi];

        for (size_t k = 0; k < kKinds; ++k) {
            TextureDocument* doc = g.doc(k);
            if (!doc) continue;
            const Slot& slot = row[k];
            if (!slot.tex) continue;

            if (!doc->hasDirtyTiles(TextureDocument::DirtyConsumer::Gpu)) continue;

            const editor::PixelRect r = doc->takeDirtyBounds(TextureDocument::DirtyConsumer::Gpu);
            if (r.isEmpty()) continue;

            const int sz = doc->size();
            const int rowPitch = sz * 4;

            // Pointer to the (r.minX, r.minY) pixel in the doc's buffer
            const uint8_t* base = doc->pixels()
                                  + (static_cast<size_t>(r.minY) * sz + r.minX) * 4;

            D3D11_BOX box{};
            box.left   = r.minX;
            box.top    = r.minY;
            box.front  = 0;
            box.right  = r.maxX;
            box.bottom = r.maxY;
            box.back   = 1;

            ctx->UpdateSubresource(slot.tex.Get(), 0, &box, base, rowPitch, 0);
        }
    }
}

// ─────────────────────────────────────────────────────────────────
// SRV lookup
// ─────────────────────────────────────────────────────────────────

ID3D11ShaderResourceView* MaskTextureSet::srv(int groupIndex, TextureKind kind) const
{
    if (groupIndex < 0 || groupIndex >= static_cast<int>(m_groupTextures.size())) {
        return nullptr;
    }
    const size_t k = static_cast<size_t>(kind);
    if (k >= kKinds) return nullptr;
    return m_groupTextures[groupIndex][k].srv.Get();
}
