#include "StaticMesh.h"

#include <algorithm>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <Windows.h>
#include <spdlog/spdlog.h>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ─────────────────────────────────────────────────────────────────
// GPU vertex layout (matches Shaders.h VSIn)
// ─────────────────────────────────────────────────────────────────

namespace {
struct GpuVertex
{
    XMFLOAT3 pos;
    XMFLOAT3 normal;
    XMFLOAT2 uv;
};

/// Convert a PMX texture path (which may use either separator style and
/// occasionally backslash-escaped sequences) into a filesystem::path
/// resolved against the model directory.
std::filesystem::path resolveTexturePath(const std::filesystem::path& modelDir,
                                          const std::string&           rel)
{
    // Replace backslashes with the native separator
    std::string normalized = rel;
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }
    // Convert UTF-8 -> wide for filesystem
    int wlen = MultiByteToWideChar(CP_UTF8, 0, normalized.data(),
                                   static_cast<int>(normalized.size()),
                                   nullptr, 0);
    std::wstring wide(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, normalized.data(),
                        static_cast<int>(normalized.size()),
                        wide.data(), wlen);

    return modelDir / wide;
}

} // namespace

// ─────────────────────────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────────────────────────

StaticMesh::StaticMesh()  = default;
StaticMesh::~StaticMesh() = default;

// ─────────────────────────────────────────────────────────────────
// Build
// ─────────────────────────────────────────────────────────────────

bool StaticMesh::build(ID3D11Device*                  device,
                       const pmx::Model&              model,
                       const std::filesystem::path&   modelDir,
                       const editor::TextureGroupSet& groups,
                       std::string&                   outError)
{
    if (!createBuffers(device, model, groups, outError)) return false;
    if (!createTextures(device, model, modelDir, outError)) return false;

    spdlog::info("StaticMesh built: {} verts, {} indices, {} segments, {} textures",
                 model.vertices.size(),
                 m_indexCount,
                 m_segments.size(),
                 m_textures.size());

    spdlog::info("Bounds: center=({:.2f},{:.2f},{:.2f}), radius={:.2f}",
                 m_boundsCenter.x, m_boundsCenter.y, m_boundsCenter.z,
                 m_boundsRadius);

    return true;
}

// ─────────────────────────────────────────────────────────────────
// VB / IB / segments
// ─────────────────────────────────────────────────────────────────

bool StaticMesh::createBuffers(ID3D11Device*                  device,
                               const pmx::Model&              model,
                               const editor::TextureGroupSet& groups,
                               std::string&                   err)
{
    if (model.vertices.empty() || model.faces.empty()) {
        err = "model has no geometry";
        return false;
    }

    // ── Convert vertices (RH -> LH: flip Z, flip normal Z, flip V) ──
    std::vector<GpuVertex> verts(model.vertices.size());

    // Also fill the CPU-side raycast buffer (pos + uv only; normals not needed
    // for hit testing).
    m_cpuVerts.assign(model.vertices.size(), {});

    XMFLOAT3 boundsMin {  FLT_MAX,  FLT_MAX,  FLT_MAX };
    XMFLOAT3 boundsMax { -FLT_MAX, -FLT_MAX, -FLT_MAX };

    for (size_t i = 0; i < model.vertices.size(); ++i) {
        const auto& s = model.vertices[i];
        GpuVertex& d  = verts[i];
        d.pos    = { s.position.x, s.position.y, -s.position.z };
        d.normal = { s.normal.x,   s.normal.y,   -s.normal.z };
        d.uv     = { s.uv.x, s.uv.y };

        m_cpuVerts[i].pos = d.pos;
        m_cpuVerts[i].uv  = d.uv;

        boundsMin.x = std::min(boundsMin.x, d.pos.x);
        boundsMin.y = std::min(boundsMin.y, d.pos.y);
        boundsMin.z = std::min(boundsMin.z, d.pos.z);
        boundsMax.x = std::max(boundsMax.x, d.pos.x);
        boundsMax.y = std::max(boundsMax.y, d.pos.y);
        boundsMax.z = std::max(boundsMax.z, d.pos.z);
    }

    m_boundsCenter = {
        (boundsMin.x + boundsMax.x) * 0.5f,
        (boundsMin.y + boundsMax.y) * 0.5f,
        (boundsMin.z + boundsMax.z) * 0.5f,
    };
    const float dx = boundsMax.x - boundsMin.x;
    const float dy = boundsMax.y - boundsMin.y;
    const float dz = boundsMax.z - boundsMin.z;
    m_boundsRadius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
    if (m_boundsRadius < 0.5f) m_boundsRadius = 0.5f;

    // ── Convert indices (reverse winding for LH) ─────────────────
    std::vector<uint32_t> indices(model.faces.size() * 3);
    for (size_t t = 0; t < model.faces.size(); ++t) {
        // Reverse winding: 0,1,2 -> 0,2,1
        indices[t*3 + 0] = static_cast<uint32_t>(model.faces[t][0]);
        indices[t*3 + 1] = static_cast<uint32_t>(model.faces[t][2]);
        indices[t*3 + 2] = static_cast<uint32_t>(model.faces[t][1]);
    }
    m_indexCount = static_cast<UINT>(indices.size());

    // Retain a CPU copy of the indices for raycast.
    m_cpuIndices = indices;

    // ── VB ───────────────────────────────────────────────────────
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(GpuVertex));
        bd.Usage     = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA srd{};
        srd.pSysMem = verts.data();
        if (FAILED(device->CreateBuffer(&bd, &srd, m_vb.GetAddressOf()))) {
            err = "failed to create vertex buffer";
            return false;
        }
    }

    // ── IB ───────────────────────────────────────────────────────
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
        bd.Usage     = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA srd{};
        srd.pSysMem = indices.data();
        if (FAILED(device->CreateBuffer(&bd, &srd, m_ib.GetAddressOf()))) {
            err = "failed to create index buffer";
            return false;
        }
    }

    // ── Build per-material draw segments ────────────────────────
    m_segments.clear();
    m_segments.reserve(model.materials.size());

    // Pre-compute material → group index for O(1) lookup per segment.
    std::vector<int> matToGroup(model.materials.size(), -1);
    for (size_t gi = 0; gi < groups.groups.size(); ++gi) {
        for (int mi : groups.groups[gi].materialIndices) {
            if (mi >= 0 && mi < static_cast<int>(matToGroup.size())) {
                matToGroup[mi] = static_cast<int>(gi);
            }
        }
    }

    // Per-triangle group lookup, parallel to m_cpuIndices/3. Filled below.
    m_triGroup.assign(model.faces.size(), -1);

    UINT cursor = 0;
    for (size_t mi = 0; mi < model.materials.size(); ++mi) {
        const auto& mat = model.materials[mi];
        DrawSegment seg{};
        seg.indexStart = cursor;
        seg.indexCount = static_cast<UINT>(mat.indexCount);
        seg.material.diffuse   = mat.diffuse;
        seg.material.ambient   = mat.ambient;
        seg.material._pad0     = 0.0f;
        seg.material.specular  = mat.specular;
        seg.material.shininess = mat.shininess;
        seg.diffuseTextureIndex= mat.diffuseTextureIndex;
        seg.groupIndex         = matToGroup[mi];
        seg.noCull             = (mat.flags & pmx::MaterialFlag::NoCull) != 0;
        seg.hasAlpha           = mat.diffuse.w < 0.999f;
        m_segments.push_back(seg);

        // Fill triGroup for this segment's triangle range.
        const int triStart = cursor / 3;
        const int triEnd   = (cursor + seg.indexCount) / 3;
        for (int t = triStart; t < triEnd && t < static_cast<int>(m_triGroup.size()); ++t) {
            m_triGroup[t] = matToGroup[mi];
        }

        cursor += seg.indexCount;
    }

    if (cursor != m_indexCount) {
        spdlog::warn("StaticMesh: material indexCount sum ({}) != total indices ({})",
                     cursor, m_indexCount);
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────
// Textures (stb_image)
// ─────────────────────────────────────────────────────────────────

bool StaticMesh::createTextures(ID3D11Device* device, const pmx::Model& model,
                                 const std::filesystem::path& modelDir,
                                 std::string& err)
{
    // Always create a 1x1 white fallback texture (bound when material has no diffuse)
    {
        const uint8_t whitePixel[4] = { 255, 255, 255, 255 };
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 1;
        td.MipLevels = td.ArraySize = 1;
        td.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc = { 1, 0 };
        td.Usage     = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA srd{};
        srd.pSysMem = whitePixel;
        srd.SysMemPitch = 4;
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(device->CreateTexture2D(&td, &srd, tex.GetAddressOf()))) {
            err = "failed to create fallback texture";
            return false;
        }
        if (FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, m_whiteTexture.GetAddressOf()))) {
            err = "failed to create fallback SRV";
            return false;
        }
    }

    // Load actual textures referenced by materials
    m_textures.resize(model.texturePaths.size());

    for (size_t i = 0; i < model.texturePaths.size(); ++i) {
        const std::filesystem::path full = resolveTexturePath(modelDir, model.texturePaths[i]);

        // For logging only (UTF-8). std::u8string -> std::string.
        const auto u8 = full.u8string();
        const std::string fullU8(reinterpret_cast<const char*>(u8.c_str()), u8.size());

        // Open the file via wide path for non-ASCII filename support
        FILE* fp = nullptr;
        _wfopen_s(&fp, full.c_str(), L"rb");
        if (!fp) {
            spdlog::warn("StaticMesh: texture not found: '{}'", fullU8);
            continue;
        }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        std::vector<uint8_t> bytes(sz);
        fread(bytes.data(), 1, sz, fp);
        fclose(fp);

        int w, h, channels;
        stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                &w, &h, &channels, 4);
        if (!pixels) {
            spdlog::warn("StaticMesh: stbi_load failed for '{}': {}", fullU8, stbi_failure_reason());
            continue;
        }

        D3D11_TEXTURE2D_DESC td{};
        td.Width      = w;
        td.Height     = h;
        td.MipLevels  = 0;                        // generate full chain
        td.ArraySize  = 1;
        td.Format     = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc = { 1, 0 };
        td.Usage      = D3D11_USAGE_DEFAULT;
        td.BindFlags  = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags  = D3D11_RESOURCE_MISC_GENERATE_MIPS;

        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(device->CreateTexture2D(&td, nullptr, tex.GetAddressOf()))) {
            stbi_image_free(pixels);
            spdlog::warn("StaticMesh: CreateTexture2D failed for '{}'", fullU8);
            continue;
        }

        ComPtr<ID3D11DeviceContext> ctx;
        device->GetImmediateContext(ctx.GetAddressOf());
        ctx->UpdateSubresource(tex.Get(), 0, nullptr, pixels, w * 4, 0);
        stbi_image_free(pixels);

        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf()))) {
            spdlog::warn("StaticMesh: CreateShaderResourceView failed for '{}'", fullU8);
            continue;
        }
        ctx->GenerateMips(srv.Get());

        m_textures[i] = srv;
        spdlog::debug("Loaded texture[{}]: {}x{}, '{}'", i, w, h, fullU8);
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────
// Draw
// ─────────────────────────────────────────────────────────────────

void StaticMesh::draw(ID3D11DeviceContext* ctx,
                      ID3D11Buffer* cbMaterial,
                      const std::function<void(int groupIndex)>& bindMaskForGroup) const
{
    UINT stride = sizeof(GpuVertex);
    UINT offset = 0;
    ID3D11Buffer* vbs[] = { m_vb.Get() };
    ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (const auto& seg : m_segments) {
        if (seg.indexCount == 0) continue;

        // Update PerMaterial CB (b1)
        ctx->UpdateSubresource(cbMaterial, 0, nullptr, &seg.material, 0, 0);

        // Bind diffuse texture (or white fallback) to t0
        ID3D11ShaderResourceView* srv = m_whiteTexture.Get();
        if (seg.diffuseTextureIndex >= 0
            && seg.diffuseTextureIndex < static_cast<int>(m_textures.size())
            && m_textures[seg.diffuseTextureIndex])
        {
            srv = m_textures[seg.diffuseTextureIndex].Get();
        }
        ID3D11ShaderResourceView* srvs[] = { srv };
        ctx->PSSetShaderResources(0, 1, srvs);

        // Let the caller bind the per-group mask SRV to t1.
        if (bindMaskForGroup) bindMaskForGroup(seg.groupIndex);

        ctx->DrawIndexed(seg.indexCount, seg.indexStart, 0);
    }
}

// ─────────────────────────────────────────────────────────────────
// CPU-side accessors (Phase 6 — for raycast)
// ─────────────────────────────────────────────────────────────────

bool StaticMesh::getTriPositions(int triIdx, XMFLOAT3& v0, XMFLOAT3& v1, XMFLOAT3& v2) const
{
    const int triCount = triangleCount();
    if (triIdx < 0 || triIdx >= triCount) return false;
    const uint32_t i0 = m_cpuIndices[triIdx * 3 + 0];
    const uint32_t i1 = m_cpuIndices[triIdx * 3 + 1];
    const uint32_t i2 = m_cpuIndices[triIdx * 3 + 2];
    if (i0 >= m_cpuVerts.size() || i1 >= m_cpuVerts.size() || i2 >= m_cpuVerts.size()) {
        return false;
    }
    v0 = m_cpuVerts[i0].pos;
    v1 = m_cpuVerts[i1].pos;
    v2 = m_cpuVerts[i2].pos;
    return true;
}

bool StaticMesh::getTriUvs(int triIdx, XMFLOAT2& uv0, XMFLOAT2& uv1, XMFLOAT2& uv2) const
{
    const int triCount = triangleCount();
    if (triIdx < 0 || triIdx >= triCount) return false;
    const uint32_t i0 = m_cpuIndices[triIdx * 3 + 0];
    const uint32_t i1 = m_cpuIndices[triIdx * 3 + 1];
    const uint32_t i2 = m_cpuIndices[triIdx * 3 + 2];
    if (i0 >= m_cpuVerts.size() || i1 >= m_cpuVerts.size() || i2 >= m_cpuVerts.size()) {
        return false;
    }
    uv0 = m_cpuVerts[i0].uv;
    uv1 = m_cpuVerts[i1].uv;
    uv2 = m_cpuVerts[i2].uv;
    return true;
}
