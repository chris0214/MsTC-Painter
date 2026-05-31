#include "DX11Widget.h"

#include "MaskTextureSet.h"
#include "MeshRaycast.h"
#include "StaticMesh.h"
#include "Shaders.h"
#include "../editor/BrushEngine.h"
#include "../editor/ChannelColors.h"
#include "../editor/TextureGroup.h"
#include "../pmx/PmxTypes.h"

#include <QResizeEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QCursor>
#include <QTimer>
#include <spdlog/spdlog.h>

#include <d3dcompiler.h>

#include <d3dcompiler.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace {

// Per-frame constant buffer (matches Shaders.h)
struct PerFrameCB
{
    XMFLOAT4X4 viewProj;
    XMFLOAT3   lightDir;
    float      _pad0;
    XMFLOAT3   eyePos;
    float      _pad1;

    // Phase 5 additions
    uint32_t   viewMode;
    uint32_t   activeKind;
    uint32_t   channelView;
    uint32_t   _padPF1;

    XMFLOAT4   channelTints[4];   // RGB tint per (R,G,B,A) channel
    XMFLOAT4   reservedMstc[2];   // L2 hooks

    // Phase 6 additions — brush cursor ring
    XMFLOAT3   brushHitWorldPos;
    float      brushRadiusWorld;
    uint32_t   brushVisible;
    uint32_t   _padPF6_0;
    uint32_t   _padPF6_1;
    uint32_t   _padPF6_2;

    // Orbit pivot marker — screen-space orange dot at the camera target's
    // projected position. Drawn entirely in screen space so it doesn't
    // bleed onto nearby mesh surfaces or float at the wrong depth.
    XMFLOAT3   pivotWorldPos;
    float      pivotRadiusPx;     // marker radius in screen pixels
    uint32_t   pivotVisible;
    uint32_t   _padPivot0;
    XMFLOAT2   viewportSize;      // (w, h) in pixels for NDC→pixel
};
static_assert(sizeof(PerFrameCB) % 16 == 0, "");

// Per-material CB layout matches StaticMesh::Material
struct PerMaterialCB
{
    XMFLOAT4 diffuse;
    XMFLOAT3 ambient;
    float    _pad0;
    XMFLOAT3 specular;
    float    shininess;
};
static_assert(sizeof(PerMaterialCB) % 16 == 0, "");

struct PerObjectCB
{
    XMFLOAT4X4 world;
};
static_assert(sizeof(PerObjectCB) % 16 == 0, "");

bool compileShader(const char* source, const char* entry, const char* profile,
                   const D3D_SHADER_MACRO* defines,
                   ID3DBlob** outBlob, std::string& err)
{
    ComPtr<ID3DBlob> errors;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    HRESULT hr = D3DCompile(source, std::strlen(source),
                            nullptr, defines, nullptr,
                            entry, profile, flags, 0,
                            outBlob, errors.GetAddressOf());
    if (FAILED(hr)) {
        if (errors) {
            err.assign(static_cast<const char*>(errors->GetBufferPointer()),
                       errors->GetBufferSize());
        } else {
            err = "shader compile failed (no error blob)";
        }
        return false;
    }
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────────────────────────

DX11Widget::DX11Widget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    if (!initDevice()) {
        spdlog::error("DX11Widget: failed to init device");
        return;
    }
    if (!createSwapChain()) {
        spdlog::error("DX11Widget: failed to create swap chain");
        return;
    }
    if (!createRenderTarget()) {
        spdlog::error("DX11Widget: failed to create render target");
        return;
    }

    std::string shaderErr;
    if (!createShaders(shaderErr)) {
        spdlog::error("DX11Widget: shader compile failed: {}", shaderErr);
        return;
    }
    if (!createConstantBuffers()) {
        spdlog::error("DX11Widget: failed to create constant buffers");
        return;
    }
    if (!createRasterAndDepthStates()) {
        spdlog::error("DX11Widget: failed to create raster/depth states");
        return;
    }

    m_deviceReady = true;
    spdlog::info("DX11Widget: ready");

    m_camera.setViewport(width(), height());

    // Allocate the mask texture set; it stays empty until setGroupSet/setModel.
    m_maskSet = std::make_unique<MaskTextureSet>();

    m_renderTimer = new QTimer(this);
    connect(m_renderTimer, &QTimer::timeout, this, [this]() {
        if (!m_deviceReady) return;
        if (m_resizePending) handleResize();
        render();
    });
    m_renderTimer->start(16);
}

DX11Widget::~DX11Widget()
{
    if (m_renderTimer) m_renderTimer->stop();
    m_maskSet.reset();
    m_mesh.reset();
    releaseRenderTarget();
    spdlog::info("DX11Widget destroyed");
}

// ─────────────────────────────────────────────────────────────────
// Device / SwapChain (unchanged from Phase 1, kept here)
// ─────────────────────────────────────────────────────────────────

bool DX11Widget::initDevice()
{
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL actualLevel{};

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        featureLevels, 1, D3D11_SDK_VERSION,
        m_device.GetAddressOf(), &actualLevel, m_context.GetAddressOf()
    );
    if (FAILED(hr)) return false;

    spdlog::info("D3D11 device created, FL {}.{}",
                 (actualLevel >> 12) & 0xF, (actualLevel >> 8) & 0xF);
    return true;
}

bool DX11Widget::createSwapChain()
{
    ComPtr<IDXGIDevice> dxgi;
    m_device.As(&dxgi);
    ComPtr<IDXGIAdapter> adapter;
    dxgi->GetAdapter(adapter.GetAddressOf());
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));

    UINT w = std::max<UINT>(1, static_cast<UINT>(width()  * devicePixelRatioF()));
    UINT h = std::max<UINT>(1, static_cast<UINT>(height() * devicePixelRatioF()));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width       = w;
    desc.Height      = h;
    desc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc  = { 1, 0 };
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (FAILED(factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &desc,
                                               nullptr, nullptr,
                                               m_swapChain.GetAddressOf())))
        return false;

    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

bool DX11Widget::createRenderTarget()
{
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(bb.GetAddressOf())))) return false;
    if (FAILED(m_device->CreateRenderTargetView(bb.Get(), nullptr, m_rtv.GetAddressOf()))) return false;

    D3D11_TEXTURE2D_DESC bbDesc{};
    bb->GetDesc(&bbDesc);

    D3D11_TEXTURE2D_DESC dd{};
    dd.Width      = bbDesc.Width;
    dd.Height     = bbDesc.Height;
    dd.MipLevels  = 1;
    dd.ArraySize  = 1;
    dd.Format     = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc = { 1, 0 };
    dd.Usage      = D3D11_USAGE_DEFAULT;
    dd.BindFlags  = D3D11_BIND_DEPTH_STENCIL;

    if (FAILED(m_device->CreateTexture2D(&dd, nullptr, m_depthBuffer.GetAddressOf()))) return false;
    if (FAILED(m_device->CreateDepthStencilView(m_depthBuffer.Get(), nullptr, m_dsv.GetAddressOf()))) return false;
    return true;
}

void DX11Widget::releaseRenderTarget()
{
    if (m_context) m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_rtv.Reset();
    m_dsv.Reset();
    m_depthBuffer.Reset();
}

void DX11Widget::handleResize()
{
    m_resizePending = false;
    releaseRenderTarget();

    UINT w = std::max<UINT>(1, static_cast<UINT>(width()  * devicePixelRatioF()));
    UINT h = std::max<UINT>(1, static_cast<UINT>(height() * devicePixelRatioF()));

    HRESULT hr = m_swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        spdlog::error("ResizeBuffers failed (0x{:08X})", static_cast<unsigned>(hr));
        return;
    }
    createRenderTarget();
    m_camera.setViewport(static_cast<int>(w), static_cast<int>(h));
}

// ─────────────────────────────────────────────────────────────────
// Shaders / CB / states
// ─────────────────────────────────────────────────────────────────

bool DX11Widget::createShaders(std::string& err)
{
    // Compile VS
    ComPtr<ID3DBlob> vsBlob;
    if (!compileShader(shaders::kBasicLambertHLSL, "vs_main", "vs_5_0",
                       nullptr, vsBlob.GetAddressOf(), err)) return false;

    // Compile PS — shader handles all view modes via gViewMode at runtime.
    ComPtr<ID3DBlob> psBlob;
    if (!compileShader(shaders::kBasicLambertHLSL, "ps_main", "ps_5_0",
                       nullptr, psBlob.GetAddressOf(), err)) return false;

    if (FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                            vsBlob->GetBufferSize(), nullptr,
                                            m_vsBasic.GetAddressOf()))) {
        err = "CreateVertexShader failed"; return false;
    }
    if (FAILED(m_device->CreatePixelShader(psBlob->GetBufferPointer(),
                                           psBlob->GetBufferSize(), nullptr,
                                           m_psBasic.GetAddressOf()))) {
        err = "CreatePixelShader failed"; return false;
    }

    // Input layout matching GpuVertex { pos, normal, uv }
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,                            D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(m_device->CreateInputLayout(layout, _countof(layout),
                                           vsBlob->GetBufferPointer(),
                                           vsBlob->GetBufferSize(),
                                           m_inputLayout.GetAddressOf()))) {
        err = "CreateInputLayout failed"; return false;
    }

    // Sampler
    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    sd.MaxAnisotropy = 1;
    if (FAILED(m_device->CreateSamplerState(&sd, m_linearSampler.GetAddressOf()))) {
        err = "CreateSamplerState failed"; return false;
    }

    return true;
}

bool DX11Widget::createConstantBuffers()
{
    auto makeCB = [this](UINT size, ID3D11Buffer** out) -> bool {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = size;
        bd.Usage     = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        return SUCCEEDED(m_device->CreateBuffer(&bd, nullptr, out));
    };

    if (!makeCB(sizeof(PerFrameCB),    m_cbPerFrame.GetAddressOf()))    return false;
    if (!makeCB(sizeof(PerMaterialCB), m_cbPerMaterial.GetAddressOf())) return false;
    if (!makeCB(sizeof(PerObjectCB),   m_cbPerObject.GetAddressOf()))   return false;
    return true;
}

bool DX11Widget::createRasterAndDepthStates()
{
    // Cull back, CW front (we already reversed winding so this matches LH)
    {
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_BACK;
        rd.FrontCounterClockwise = FALSE;
        rd.DepthClipEnable = TRUE;
        if (FAILED(m_device->CreateRasterizerState(&rd, m_rsCull.GetAddressOf()))) return false;

        rd.CullMode = D3D11_CULL_NONE;
        if (FAILED(m_device->CreateRasterizerState(&rd, m_rsNoCull.GetAddressOf()))) return false;
    }

    {
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (FAILED(m_device->CreateDepthStencilState(&dd, m_dssDefault.GetAddressOf()))) return false;
    }

    {
        D3D11_BLEND_DESC bd{};
        auto& rt = bd.RenderTarget[0];
        rt.BlendEnable           = TRUE;
        rt.SrcBlend              = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp               = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha         = D3D11_BLEND_ONE;
        rt.DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha          = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(m_device->CreateBlendState(&bd, m_bsAlpha.GetAddressOf()))) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────

void DX11Widget::setGroupSet(editor::TextureGroupSet* groups)
{
    m_groups = groups;
    // The actual GPU mask textures are (re)built in setModel — at that point
    // we have both the mesh and the group docs in lockstep.
}

bool DX11Widget::setModel(const pmx::Model& model, const std::filesystem::path& modelDir)
{
    if (!m_deviceReady) {
        spdlog::error("DX11Widget::setModel called before device is ready");
        return false;
    }
    if (!m_groups) {
        spdlog::error("DX11Widget::setModel called before setGroupSet");
        return false;
    }

    auto mesh = std::make_unique<StaticMesh>();
    std::string err;
    if (!mesh->build(m_device.Get(), model, modelDir, *m_groups, err)) {
        spdlog::error("StaticMesh build failed: {}", err);
        return false;
    }

    // Build per-group GPU mask textures from the just-loaded group docs.
    if (!m_maskSet->build(m_device.Get(), *m_groups, err)) {
        spdlog::error("MaskTextureSet build failed: {}", err);
        return false;
    }

    m_mesh = std::move(mesh);
    m_camera.resetToFit(m_mesh->boundsCenter(), m_mesh->boundsRadius());
    return true;
}

void DX11Widget::setActiveKind(editor::TextureKind kind)
{
    m_activeKind = kind;
}

void DX11Widget::setViewMode(ViewMode m)
{
    m_viewMode = m;
}

// ─────────────────────────────────────────────────────────────────
// Render
// ─────────────────────────────────────────────────────────────────

void DX11Widget::render()
{
    // Pull any dirty mask tiles from CPU docs onto the GPU first — so the
    // shader samples up-to-date content this frame.
    if (m_maskSet && m_groups) {
        m_maskSet->syncDirty(m_context.Get(), *m_groups);
    }

    const float clear[4] = { 0.12f, 0.14f, 0.18f, 1.0f };
    m_context->ClearRenderTargetView(m_rtv.Get(), clear);
    m_context->ClearDepthStencilView(m_dsv.Get(),
                                     D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                     1.0f, 0);

    ID3D11RenderTargetView* rtvs[] = { m_rtv.Get() };
    m_context->OMSetRenderTargets(1, rtvs, m_dsv.Get());

    // Viewport
    ComPtr<ID3D11Texture2D> bb;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(bb.GetAddressOf()));
    D3D11_TEXTURE2D_DESC bbDesc{};
    bb->GetDesc(&bbDesc);

    D3D11_VIEWPORT vp{};
    vp.Width    = static_cast<float>(bbDesc.Width);
    vp.Height   = static_cast<float>(bbDesc.Height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    // Common pipeline state
    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->VSSetShader(m_vsBasic.Get(), nullptr, 0);
    m_context->PSSetShader(m_psBasic.Get(), nullptr, 0);
    ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };
    m_context->PSSetSamplers(0, 1, samplers);

    m_context->RSSetState(m_rsCull.Get());
    m_context->OMSetDepthStencilState(m_dssDefault.Get(), 0);
    const float blendFactor[4] = { 0, 0, 0, 0 };
    m_context->OMSetBlendState(m_bsAlpha.Get(), blendFactor, 0xFFFFFFFF);

    // ── Update per-frame CB ─────────────────────────────────────
    {
        PerFrameCB cb{};
        XMStoreFloat4x4(&cb.viewProj, XMMatrixTranspose(m_camera.viewProj()));
        XMVECTOR ld = XMVector3Normalize(XMVectorSet(-0.3f, 0.8f, -0.5f, 0));
        XMStoreFloat3(&cb.lightDir, ld);
        cb.eyePos = m_camera.eyePosition();

        cb.viewMode    = static_cast<uint32_t>(m_viewMode);
        cb.activeKind  = static_cast<uint32_t>(m_activeKind);
        cb.channelView = m_channelView;

        // Tints for the active kind. Same colors as the UI chips / canvas
        // Semantic view (single source of truth: editor/ChannelColors.h).
        const auto tints = editor::channel_colors::tintsFor(m_activeKind);
        for (size_t i = 0; i < 4; ++i) {
            cb.channelTints[i] = { tints[i].r, tints[i].g, tints[i].b, 0.0f };
        }
        // L2 reserved slots — zeroed for now.
        cb.reservedMstc[0] = { 0, 0, 0, 0 };
        cb.reservedMstc[1] = { 0, 0, 0, 0 };

        // Brush cursor: show whenever the mouse is over the model. Painting
        // takes priority (use the in-stroke hit point so the ring tracks the
        // stroke), otherwise fall back to the hover hit position.
        const bool hasPaint = (m_drag == DragMode::Paint && m_lastHitValid);
        const bool hasHover = m_hoverHitValid;
        if ((hasPaint || hasHover) && m_mesh && m_brush) {
            cb.brushHitWorldPos = hasPaint ? m_lastHitWorld : m_hoverHitWorldPos;

            // Convert brush radius from texture pixels to world units via
            // the local UV→world Jacobian of the hit triangle. Falls back
            // gracefully if the hit triangle is degenerate / unknown.
            //
            //   uvRadius   = brushRadiusPx / textureSize     (in UV units)
            //   worldScale = sqrt(triWorldArea / triUvArea)  (world per UV)
            //   worldRadius = uvRadius * worldScale
            //
            // The doc size comes from the active group's stack, so picking
            // 1024 vs 2048 vs 4096 just works.
            const int triIdx = hasPaint ? m_paintHitTriIdx : m_hoverHitTriIdx;
            int texSize = 2048;   // sane default if we can't query
            if (m_groups) {
                const int gi = hasPaint ? m_paintGroupIndex
                                        : (m_mesh->triangleGroup(triIdx));
                if (gi >= 0 && gi < static_cast<int>(m_groups->groups.size())) {
                    if (auto* doc = m_groups->groups[gi].doc(m_activeKind)) {
                        texSize = doc->size();
                    }
                }
            }
            const float uvRadius   = m_brush->settings().radiusPx
                                   / static_cast<float>(std::max(1, texSize));
            const float worldScale = uvToWorldScale(triIdx);
            cb.brushRadiusWorld    = uvRadius * worldScale;
            cb.brushVisible        = 1u;
        } else {
            cb.brushHitWorldPos = { 0, 0, 0 };
            cb.brushRadiusWorld = 0.0f;
            cb.brushVisible     = 0u;
        }

        // Pivot marker: screen-space orange dot at the camera target's
        // projected position. Toggleable via the viewport bar checkbox.
        const float dprP = static_cast<float>(devicePixelRatioF());
        cb.viewportSize = {
            std::max(1.0f, static_cast<float>(width()) * dprP),
            std::max(1.0f, static_cast<float>(height()) * dprP)
        };
        if (m_showPivotMarker && m_mesh && m_mesh->isReady()) {
            cb.pivotWorldPos = m_camera.target();
            cb.pivotRadiusPx = 6.0f * dprP;   // 6 logical px, scaled for HiDPI
            cb.pivotVisible  = 1u;
        } else {
            cb.pivotWorldPos = { 0, 0, 0 };
            cb.pivotRadiusPx = 0.0f;
            cb.pivotVisible  = 0u;
        }

        m_context->UpdateSubresource(m_cbPerFrame.Get(), 0, nullptr, &cb, 0, 0);

        ID3D11Buffer* cbs[] = { m_cbPerFrame.Get() };
        m_context->VSSetConstantBuffers(0, 1, cbs);
        m_context->PSSetConstantBuffers(0, 1, cbs);
    }
    // ── Per-object CB (identity world) ──────────────────────────
    {
        PerObjectCB cb{};
        XMStoreFloat4x4(&cb.world, XMMatrixIdentity());
        m_context->UpdateSubresource(m_cbPerObject.Get(), 0, nullptr, &cb, 0, 0);
        ID3D11Buffer* cbs[] = { m_cbPerObject.Get() };
        m_context->VSSetConstantBuffers(2, 1, cbs);
    }
    // PerMaterial CB is updated per-segment inside StaticMesh::draw
    {
        ID3D11Buffer* cbs[] = { m_cbPerMaterial.Get() };
        m_context->PSSetConstantBuffers(1, 1, cbs);
    }

    if (m_mesh && m_mesh->isReady()) {
        // Bind callback: pick the active group's mask SRV for t1.
        ID3D11ShaderResourceView* fallback =
            m_maskSet ? m_maskSet->fallbackSrv() : nullptr;
        const editor::TextureKind activeKind = m_activeKind;

        m_mesh->draw(m_context.Get(), m_cbPerMaterial.Get(),
            [this, fallback, activeKind](int groupIndex) {
                ID3D11ShaderResourceView* mask = nullptr;
                if (m_maskSet && groupIndex >= 0) {
                    mask = m_maskSet->srv(groupIndex, activeKind);
                }
                if (!mask) mask = fallback;
                ID3D11ShaderResourceView* srvs[] = { mask };
                m_context->PSSetShaderResources(1, 1, srvs);
            });
    }

    m_swapChain->Present(1, 0);
}

// ─────────────────────────────────────────────────────────────────
// Qt events
// ─────────────────────────────────────────────────────────────────

void DX11Widget::paintEvent(QPaintEvent*)
{
    // No-op: timer drives rendering
}

void DX11Widget::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (m_deviceReady) m_resizePending = true;
}

void DX11Widget::mousePressEvent(QMouseEvent* ev)
{
    using namespace DirectX;

    m_lastMouseX = ev->position().x();
    m_lastMouseY = ev->position().y();

    // Blender-style: Middle button = orbit. Shift+MMB = pan.
    // Right button kept as a fallback orbit for users with 2-button mice.
    if (ev->button() == Qt::MiddleButton) {
        m_drag = (ev->modifiers() & Qt::ShiftModifier)
            ? DragMode::Pan : DragMode::Orbit;
        // Make sure the widget can receive keyPressEvent for shift-modifier
        // updates / numpad shortcuts (clicking grants focus on most platforms,
        // but be explicit).
        setFocus();
        return;
    }
    if (ev->button() == Qt::RightButton) {
        m_drag = DragMode::Orbit;
        setFocus();
        return;
    }
    if (ev->button() != Qt::LeftButton) {
        m_drag = DragMode::None;
        return;
    }

    // ── Left button: alt-modifier turns it into orbit (Maya/Houdini style) ──
    // Common alternate ergonomic for users who don't have a middle button
    // (e.g. trackpad). Alt+LMB = orbit, Shift+Alt+LMB = pan.
    if (ev->modifiers() & Qt::AltModifier) {
        m_drag = (ev->modifiers() & Qt::ShiftModifier)
            ? DragMode::Pan : DragMode::Orbit;
        setFocus();
        return;
    }

    setFocus();

    // ── Set-Pivot mode: next LMB click goes to focus, not paint. ──
    // User armed it via the toolbar button. Consumes the click and disarms.
    if (m_setPivotArmed && m_mesh && m_mesh->isReady()) {
        const float dpr = static_cast<float>(devicePixelRatioF());
        const int viewW = std::max(1, static_cast<int>(width()  * dpr));
        const int viewH = std::max(1, static_cast<int>(height() * dpr));
        XMVECTOR rayOrigin, rayDir;
        renderer::screenToRay(static_cast<int>(m_lastMouseX * dpr),
                              static_cast<int>(m_lastMouseY * dpr),
                              viewW, viewH, m_camera,
                              rayOrigin, rayDir);
        const auto hit = renderer::raycastMesh(*m_mesh, rayOrigin, rayDir);
        if (hit.triangleIndex >= 0) {
            m_camera.focusOn(hit.worldPos);
        }
        m_setPivotArmed = false;
        emit setPivotModeChanged(false);
        m_drag = DragMode::None;
        return;
    }

    // Left button = paint. Need a model, a brush, and a group set.
    if (!m_mesh || !m_mesh->isReady() || !m_brush || !m_groups) {
        m_drag = DragMode::None;
        return;
    }

    // Build a ray from this pixel and intersect the mesh.
    const float dpr = static_cast<float>(devicePixelRatioF());
    const int viewW = std::max(1, static_cast<int>(width()  * dpr));
    const int viewH = std::max(1, static_cast<int>(height() * dpr));
    XMVECTOR rayOrigin, rayDir;
    renderer::screenToRay(static_cast<int>(m_lastMouseX * dpr),
                          static_cast<int>(m_lastMouseY * dpr),
                          viewW, viewH, m_camera,
                          rayOrigin, rayDir);
    const auto hit = renderer::raycastMesh(*m_mesh, rayOrigin, rayDir);
    if (hit.triangleIndex < 0 || hit.groupIndex < 0
        || hit.groupIndex >= static_cast<int>(m_groups->groups.size())) {
        // Click missed the model — do nothing (don't fall back to orbit).
        m_drag = DragMode::None;
        return;
    }

    // Restrict-to-active-group: when the user has unchecked auto-switch,
    // they want the 3D paint to behave as a "draw only on the group I'm
    // editing" tool. Clicks on geometry belonging to other groups are
    // silently dropped — clearer than letting the brush leak onto a group
    // the user isn't currently looking at in 2D.
    if (m_restrictPaintToActiveGroup
        && m_activeGroup >= 0
        && hit.groupIndex != m_activeGroup) {
        m_drag = DragMode::None;
        return;
    }

    auto& stack = *m_groups->groups[hit.groupIndex].stacks[
        static_cast<size_t>(m_activeKind)];
    auto* target = stack.activeTarget();
    if (!target) {
        // Active layer is non-paintable (e.g. FillLayer). Don't enter paint
        // mode — let the user select a paintable layer first.
        m_drag = DragMode::None;
        return;
    }

    m_drag = DragMode::Paint;
    m_paintGroupIndex = hit.groupIndex;
    m_lastPaintUv     = hit.uv;
    m_lastHitWorld    = hit.worldPos;
    m_paintHitTriIdx  = hit.triangleIndex;
    m_lastHitValid    = true;
    emit paintHitGroup(hit.groupIndex);

    // Open undo entry if active layer is paintable.
    auto* paint = dynamic_cast<editor::PaintLayer*>(stack.activeLayer());
    if (paint) stack.history().beginStrokeRecord(*paint);

    m_pendingDirtyBox = {};
    m_brush->beginStroke(*target, hit.uv.x, hit.uv.y, m_pendingDirtyBox);
    if (paint) stack.history().recordStampBox(m_pendingDirtyBox, *paint);
    if (!m_pendingDirtyBox.isEmpty()) {
        stack.recompositeRect(m_pendingDirtyBox);
        m_pendingDirtyBox = {};
    }
}

void DX11Widget::mouseMoveEvent(QMouseEvent* ev)
{
    using namespace DirectX;

    const int x = ev->position().x();
    const int y = ev->position().y();
    const float dx = static_cast<float>(x - m_lastMouseX);
    const float dy = static_cast<float>(y - m_lastMouseY);
    m_lastMouseX = x;
    m_lastMouseY = y;

    // Shift = slow modifier (×0.2) for both orbit & pan, matches Blender
    // and Maya. Lets the user nudge views into precise positions without
    // having to zoom way in first. m_navSensitivity is a global multiplier
    // (1.0 = engine default) controlled by the viewport-bar slider.
    const float speed = m_navSensitivity *
        ((ev->modifiers() & Qt::ShiftModifier) ? 0.2f : 1.0f);

    if (m_drag == DragMode::Orbit) {
        m_camera.orbit(dx * speed, dy * speed);
        return;
    }
    if (m_drag == DragMode::Pan) {
        m_camera.pan(dx * speed, dy * speed);
        // Pan in 3D space can drift the pivot off the mesh surface. After
        // each pan delta, raycast through the pivot's new projected screen
        // position and snap to whatever surface is there. Keeps the pivot
        // anchored to mesh so the next orbit feels predictable.
        snapPivotToSurface();
        return;
    }
    if (m_drag == DragMode::None) {
        // Hover: cast a ray so the cursor ring follows the model surface.
        // Cheap (~1-2 ms) and only runs when no drag is in progress.
        if (m_mesh && m_mesh->isReady()) {
            const float dpr = static_cast<float>(devicePixelRatioF());
            const int viewW = std::max(1, static_cast<int>(width()  * dpr));
            const int viewH = std::max(1, static_cast<int>(height() * dpr));
            XMVECTOR rayOrigin, rayDir;
            renderer::screenToRay(static_cast<int>(x * dpr),
                                  static_cast<int>(y * dpr),
                                  viewW, viewH, m_camera,
                                  rayOrigin, rayDir);
            const auto hit = renderer::raycastMesh(*m_mesh, rayOrigin, rayDir);
            if (hit.triangleIndex >= 0) {
                m_hoverHitValid    = true;
                m_hoverHitWorldPos = hit.worldPos;
                m_hoverHitTriIdx   = hit.triangleIndex;
            } else {
                m_hoverHitValid  = false;
                m_hoverHitTriIdx = -1;
            }
        }
        return;
    }
    if (m_drag != DragMode::Paint) return;

    // Continue stroke. Cast a ray; if the cursor went off the mesh or onto a
    // different group, just SKIP this sample. Strokes are LOCKED to the group
    // they started on — the user can drag freely, including over other groups,
    // without accidentally painting on them or switching mid-stroke.
    if (!m_mesh || !m_brush || !m_groups) return;

    const float dpr = static_cast<float>(devicePixelRatioF());
    const int viewW = std::max(1, static_cast<int>(width()  * dpr));
    const int viewH = std::max(1, static_cast<int>(height() * dpr));
    XMVECTOR rayOrigin, rayDir;
    renderer::screenToRay(static_cast<int>(x * dpr),
                          static_cast<int>(y * dpr),
                          viewW, viewH, m_camera,
                          rayOrigin, rayDir);
    const auto hit = renderer::raycastMesh(*m_mesh, rayOrigin, rayDir);
    if (hit.triangleIndex < 0) return;
    if (hit.groupIndex != m_paintGroupIndex) {
        // Cursor wandered onto a different group — ignore this sample. The
        // stroke stays paused (BrushEngine knows it's mid-stroke). When the
        // cursor comes back onto m_paintGroupIndex, painting resumes.
        return;
    }

    // Detect a UV jump (seam) within the same group → break the stroke.
    auto& stack = *m_groups->groups[m_paintGroupIndex].stacks[
        static_cast<size_t>(m_activeKind)];
    auto* target = stack.activeTarget();
    if (!target) return;
    auto* paint = dynamic_cast<editor::PaintLayer*>(stack.activeLayer());

    constexpr float kSeamJumpSq = 0.05f * 0.05f;
    const float du = hit.uv.x - m_lastPaintUv.x;
    const float dv = hit.uv.y - m_lastPaintUv.y;
    m_pendingDirtyBox = {};
    if (du*du + dv*dv > kSeamJumpSq) {
        // Seam jump: close the previous undo entry, open a new one. Each
        // connected island traversed becomes its own undoable step — matches
        // the user's mental model of "one continuous painted region = one undo".
        m_brush->endStroke();
        if (paint) {
            stack.history().endStrokeRecord(*paint);
            stack.history().beginStrokeRecord(*paint);
        }
        m_brush->beginStroke(*target, hit.uv.x, hit.uv.y, m_pendingDirtyBox);
    } else {
        m_brush->strokeTo(*target, hit.uv.x, hit.uv.y, m_pendingDirtyBox);
    }
    if (paint) stack.history().recordStampBox(m_pendingDirtyBox, *paint);
    if (!m_pendingDirtyBox.isEmpty()) {
        stack.recompositeRect(m_pendingDirtyBox);
        m_pendingDirtyBox = {};
    }
    m_lastPaintUv    = hit.uv;
    m_lastHitWorld   = hit.worldPos;
    m_paintHitTriIdx = hit.triangleIndex;
}

void DX11Widget::mouseReleaseEvent(QMouseEvent* ev)
{
    if (m_drag == DragMode::Paint
        && ev->button() == Qt::LeftButton
        && m_brush && m_brush->isStroking())
    {
        m_brush->endStroke();
        // Close the in-progress undo entry. m_paintGroupIndex must still
        // point at the group we started painting on (mouseMove never
        // changes it once a paint is active).
        if (m_groups
            && m_paintGroupIndex >= 0
            && m_paintGroupIndex < static_cast<int>(m_groups->groups.size())) {
            auto& stack = *m_groups->groups[m_paintGroupIndex].stacks[
                static_cast<size_t>(m_activeKind)];
            if (auto* paint = dynamic_cast<editor::PaintLayer*>(stack.activeLayer())) {
                stack.history().endStrokeRecord(*paint);
            }
        }
        emit strokeFinished();
    }
    m_drag = DragMode::None;
}

void DX11Widget::wheelEvent(QWheelEvent* ev)
{
    const float speed = m_navSensitivity *
        ((ev->modifiers() & Qt::ShiftModifier) ? 0.25f : 1.0f);
    m_camera.zoom(static_cast<float>(ev->angleDelta().y()) * speed);
}

void DX11Widget::leaveEvent(QEvent*)
{
    // Cursor left the viewport — drop the hover ring.
    m_hoverHitValid = false;
}

// ─────────────────────────────────────────────────────────────────
// Pivot surface snap + UV→world scale
// ─────────────────────────────────────────────────────────────────

float DX11Widget::uvToWorldScale(int triangleIndex) const
{
    using namespace DirectX;

    // Fall back to a coarse heuristic if we don't have a valid hit triangle:
    // assumes a packed UV chart over roughly half the bounding sphere.
    auto fallback = [this]() -> float {
        if (!m_mesh) return 1.0f;
        return std::max(0.001f, m_mesh->boundsRadius() * 0.5f);
    };

    if (!m_mesh || !m_mesh->isReady() || triangleIndex < 0) return fallback();

    XMFLOAT3 v0, v1, v2;
    XMFLOAT2 uv0, uv1, uv2;
    if (!m_mesh->getTriPositions(triangleIndex, v0, v1, v2)) return fallback();
    if (!m_mesh->getTriUvs(triangleIndex, uv0, uv1, uv2)) return fallback();

    // World-space edges and area. Use the cross-product magnitude / 2.
    const XMVECTOR vv0 = XMLoadFloat3(&v0);
    const XMVECTOR e1  = XMVectorSubtract(XMLoadFloat3(&v1), vv0);
    const XMVECTOR e2  = XMVectorSubtract(XMLoadFloat3(&v2), vv0);
    const float wArea  = 0.5f * XMVectorGetX(XMVector3Length(XMVector3Cross(e1, e2)));

    // UV-space area: |det| / 2 of the 2D edge matrix.
    const float du1 = uv1.x - uv0.x, dv1 = uv1.y - uv0.y;
    const float du2 = uv2.x - uv0.x, dv2 = uv2.y - uv0.y;
    const float uArea = 0.5f * std::abs(du1 * dv2 - dv1 * du2);

    if (uArea < 1e-10f || wArea <= 0.0f) return fallback();

    // worldPerUv ≈ sqrt(worldArea / uvArea). Same units of "world per UV unit"
    // independent of triangle shape (assumes locally isotropic UV mapping,
    // which is approximately true within one packed chart).
    return std::sqrt(wArea / uArea);
}

void DX11Widget::snapPivotToSurface()
{
    using namespace DirectX;
    if (!m_mesh || !m_mesh->isReady()) return;

    const float dpr = static_cast<float>(devicePixelRatioF());
    const int viewW = std::max(1, static_cast<int>(width()  * dpr));
    const int viewH = std::max(1, static_cast<int>(height() * dpr));

    // Project pivot world pos to clip space.
    XMFLOAT3 pivotPos = m_camera.target();   // local copy — XMLoadFloat3 needs addressable storage
    XMVECTOR pivotW   = XMLoadFloat3(&pivotPos);
    XMVECTOR pivotW4  = XMVectorSetW(pivotW, 1.0f);
    XMMATRIX vp       = m_camera.viewProj();
    XMVECTOR clip     = XMVector4Transform(pivotW4, vp);
    const float w = XMVectorGetW(clip);
    if (w < 0.0001f) return;   // pivot at or behind the camera

    const float ndcX = XMVectorGetX(clip) / w;
    const float ndcY = XMVectorGetY(clip) / w;
    const int   scrX = static_cast<int>((ndcX * 0.5f + 0.5f) * viewW);
    const int   scrY = static_cast<int>((1.0f - (ndcY * 0.5f + 0.5f)) * viewH);

    // Out of viewport — leave pivot alone (nothing to snap to here).
    if (scrX < 0 || scrX >= viewW || scrY < 0 || scrY >= viewH) return;

    XMVECTOR rayOrigin, rayDir;
    renderer::screenToRay(scrX, scrY, viewW, viewH, m_camera, rayOrigin, rayDir);
    const auto hit = renderer::raycastMesh(*m_mesh, rayOrigin, rayDir);
    if (hit.triangleIndex >= 0) {
        m_camera.setTarget(hit.worldPos);
    }
}

// ─────────────────────────────────────────────────────────────────
// Keyboard navigation (Blender-style numpad + F-focus)
// ─────────────────────────────────────────────────────────────────

void DX11Widget::keyPressEvent(QKeyEvent* ev)
{
    using namespace DirectX;

    const bool ctrl = (ev->modifiers() & Qt::ControlModifier) != 0;

    switch (ev->key()) {
    // Numpad views — Ctrl flips to the opposite axis (Blender convention).
    case Qt::Key_1:   // Numpad 1 — Front view (or Ctrl: Back)
        m_camera.setStandardView(ctrl ? OrbitCamera::StandardView::Back
                                       : OrbitCamera::StandardView::Front);
        update();
        return;
    case Qt::Key_3:   // Numpad 3 — Right view (or Ctrl: Left)
        m_camera.setStandardView(ctrl ? OrbitCamera::StandardView::Left
                                       : OrbitCamera::StandardView::Right);
        update();
        return;
    case Qt::Key_7:   // Numpad 7 — Top view (or Ctrl: Bottom)
        m_camera.setStandardView(ctrl ? OrbitCamera::StandardView::Bottom
                                       : OrbitCamera::StandardView::Top);
        update();
        return;
    case Qt::Key_5:   // Numpad 5 — toggle perspective / orthographic
        m_camera.toggleOrtho();
        update();
        return;
    case Qt::Key_0:   // Numpad 0 — reset view to fit model
        if (m_mesh && m_mesh->isReady()) {
            m_camera.resetToFit(m_mesh->boundsCenter(), m_mesh->boundsRadius());
            update();
        }
        return;

    // F — focus orbit pivot on whatever geometry is under the cursor.
    // If the cursor isn't over the mesh, fall back to the last hover point.
    case Qt::Key_F: {
        if (!m_mesh || !m_mesh->isReady()) return;
        const QPoint cursor = mapFromGlobal(QCursor::pos());
        const float dpr = static_cast<float>(devicePixelRatioF());
        const int viewW = std::max(1, static_cast<int>(width()  * dpr));
        const int viewH = std::max(1, static_cast<int>(height() * dpr));
        XMVECTOR rayOrigin, rayDir;
        renderer::screenToRay(static_cast<int>(cursor.x() * dpr),
                              static_cast<int>(cursor.y() * dpr),
                              viewW, viewH, m_camera,
                              rayOrigin, rayDir);
        const auto hit = renderer::raycastMesh(*m_mesh, rayOrigin, rayDir);
        if (hit.triangleIndex >= 0) {
            m_camera.focusOn(hit.worldPos);
        } else if (m_hoverHitValid) {
            m_camera.focusOn(m_hoverHitWorldPos);
        }
        // If neither — refuse silently. Don't snap to origin, that's jarring.
        update();
        return;
    }

    default:
        QWidget::keyPressEvent(ev);
        return;
    }
}

void DX11Widget::mouseDoubleClickEvent(QMouseEvent* ev)
{
    using namespace DirectX;

    // Middle-button double-click → focus orbit pivot on the geometry under
    // the cursor. Same effect as F-key, just discoverable by mouse-only users.
    // Left button stays as paint (a paint stroke that happens to be a
    // double-click is still a paint stroke).
    if (ev->button() != Qt::MiddleButton) {
        QWidget::mouseDoubleClickEvent(ev);
        return;
    }
    if (!m_mesh || !m_mesh->isReady()) return;

    const float dpr = static_cast<float>(devicePixelRatioF());
    const int viewW = std::max(1, static_cast<int>(width()  * dpr));
    const int viewH = std::max(1, static_cast<int>(height() * dpr));
    XMVECTOR rayOrigin, rayDir;
    renderer::screenToRay(static_cast<int>(ev->position().x() * dpr),
                          static_cast<int>(ev->position().y() * dpr),
                          viewW, viewH, m_camera,
                          rayOrigin, rayDir);
    const auto hit = renderer::raycastMesh(*m_mesh, rayOrigin, rayDir);
    if (hit.triangleIndex >= 0) {
        m_camera.focusOn(hit.worldPos);
        update();
    }
}
