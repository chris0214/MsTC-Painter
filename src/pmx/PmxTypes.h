#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

namespace pmx {

using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;

/// PMX header / metadata
struct Header
{
    float       version    = 0.0f;          // 2.0 or 2.1
    uint8_t     encoding   = 0;             // 0 = UTF-16LE, 1 = UTF-8
    uint8_t     uvCount    = 0;             // number of additional UV layers (0..4)
    uint8_t     vertexIndexSize  = 0;       // 1, 2, 4
    uint8_t     textureIndexSize = 0;
    uint8_t     materialIndexSize = 0;
    uint8_t     boneIndexSize    = 0;
    uint8_t     morphIndexSize   = 0;
    uint8_t     rigidIndexSize   = 0;

    std::string nameJa;
    std::string nameEn;
    std::string commentJa;
    std::string commentEn;
};

/// Vertex weight types
enum class WeightType : uint8_t
{
    BDEF1 = 0,
    BDEF2 = 1,
    BDEF4 = 2,
    SDEF  = 3,
    QDEF  = 4,
};

struct Vertex
{
    XMFLOAT3 position{};
    XMFLOAT3 normal{};
    XMFLOAT2 uv{};
    std::array<XMFLOAT4, 4> additionalUVs{};

    WeightType weightType = WeightType::BDEF1;
    std::array<int32_t, 4> boneIndices = {-1, -1, -1, -1};
    std::array<float,   4> boneWeights = { 0, 0, 0, 0 };

    XMFLOAT3 sdefC{};
    XMFLOAT3 sdefR0{};
    XMFLOAT3 sdefR1{};

    float edgeRatio = 1.0f;
};

/// Face = 3 vertex indices
using Face = std::array<int32_t, 3>;

/// Material flags (bit field)
enum MaterialFlag : uint8_t
{
    NoCull       = 1 << 0,
    GroundShadow = 1 << 1,
    DrawShadow   = 1 << 2,
    ReceiveShadow= 1 << 3,
    HasEdge      = 1 << 4,
    VertexColor  = 1 << 5,
    PointDraw    = 1 << 6,
    LineDraw     = 1 << 7,
};

/// Sphere mode
enum class SphereMode : uint8_t
{
    None     = 0,
    Multiply = 1,
    Add      = 2,
    SubTex   = 3,
};

/// Toon reference type
enum class ToonRef : uint8_t
{
    Texture  = 0,
    Internal = 1,
};

struct Material
{
    std::string nameJa;
    std::string nameEn;

    XMFLOAT4 diffuse  { 1, 1, 1, 1 };
    XMFLOAT3 specular { 0, 0, 0 };
    float    shininess = 0.0f;
    XMFLOAT3 ambient  { 0.5f, 0.5f, 0.5f };

    uint8_t  flags = 0;

    XMFLOAT4 edgeColor { 0, 0, 0, 1 };
    float    edgeSize  = 1.0f;

    int32_t    diffuseTextureIndex = -1;
    int32_t    sphereTextureIndex  = -1;
    SphereMode sphereMode          = SphereMode::None;

    ToonRef toonRef     = ToonRef::Texture;
    int32_t toonIndex   = -1;       // texture index when ToonRef::Texture, internal toon (0..9) when ToonRef::Internal

    std::string memo;

    int32_t indexCount = 0;         // number of indices (= triangles * 3) belonging to this material
};

/// Top-level PMX model data (only what we need for now)
struct Model
{
    Header                   header;
    std::vector<Vertex>      vertices;
    std::vector<Face>        faces;       // flattened into triangles
    std::vector<std::string> texturePaths; // relative paths (filesystem-native)
    std::vector<Material>    materials;
};

} // namespace pmx
