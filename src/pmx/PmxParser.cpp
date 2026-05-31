#include "PmxParser.h"

#include <fstream>
#include <vector>
#include <cstring>

#include <Windows.h>
#include <spdlog/spdlog.h>

namespace pmx {

namespace {

// ─────────────────────────────────────────────────────────────────
// Binary reader — little endian, in-memory buffer
// ─────────────────────────────────────────────────────────────────

class Reader
{
public:
    explicit Reader(const std::vector<uint8_t>& buf) : m_buf(buf) {}

    bool ok() const { return !m_failed; }
    size_t pos() const { return m_pos; }
    size_t remaining() const { return m_buf.size() - m_pos; }

    void fail(const char* why)
    {
        if (!m_failed) {
            m_failed = true;
            m_error  = why;
        }
    }
    const std::string& error() const { return m_error; }

    template <class T>
    T read()
    {
        T v{};
        if (m_pos + sizeof(T) > m_buf.size()) {
            fail("unexpected EOF");
            return v;
        }
        std::memcpy(&v, m_buf.data() + m_pos, sizeof(T));
        m_pos += sizeof(T);
        return v;
    }

    void skip(size_t n)
    {
        if (m_pos + n > m_buf.size()) {
            fail("skip past EOF");
            return;
        }
        m_pos += n;
    }

    void readBytes(void* dst, size_t n)
    {
        if (m_pos + n > m_buf.size()) {
            fail("read past EOF");
            return;
        }
        std::memcpy(dst, m_buf.data() + m_pos, n);
        m_pos += n;
    }

    /// Read a "PMX index" — variable size 1/2/4 bytes, signed, with -1 sentinel.
    int32_t readIndex(uint8_t indexSize)
    {
        switch (indexSize) {
        case 1: return static_cast<int32_t>(read<int8_t>());
        case 2: return static_cast<int32_t>(read<int16_t>());
        case 4: return read<int32_t>();
        default: fail("bad index size"); return -1;
        }
    }

    /// Read a "PMX vertex index" — variable size 1/2/4 bytes, UNSIGNED, no sentinel.
    int32_t readVertexIndex(uint8_t indexSize)
    {
        switch (indexSize) {
        case 1: return static_cast<int32_t>(read<uint8_t>());
        case 2: return static_cast<int32_t>(read<uint16_t>());
        case 4: return read<int32_t>();
        default: fail("bad vertex index size"); return 0;
        }
    }

    /// PMX text: int32 length (in bytes) + raw bytes (UTF-16LE or UTF-8).
    std::string readText(uint8_t encoding)
    {
        const int32_t lenBytes = read<int32_t>();
        if (!ok() || lenBytes < 0) { fail("bad text length"); return {}; }

        if (lenBytes == 0) return {};

        if (m_pos + static_cast<size_t>(lenBytes) > m_buf.size()) {
            fail("text past EOF");
            return {};
        }

        const char* raw = reinterpret_cast<const char*>(m_buf.data() + m_pos);
        std::string out;

        if (encoding == 0) {
            // UTF-16LE -> UTF-8
            int wcharCount = lenBytes / 2;
            int u8len = WideCharToMultiByte(CP_UTF8, 0,
                reinterpret_cast<LPCWCH>(raw), wcharCount,
                nullptr, 0, nullptr, nullptr);
            if (u8len > 0) {
                out.resize(u8len);
                WideCharToMultiByte(CP_UTF8, 0,
                    reinterpret_cast<LPCWCH>(raw), wcharCount,
                    out.data(), u8len, nullptr, nullptr);
            }
        } else {
            // already UTF-8
            out.assign(raw, lenBytes);
        }

        m_pos += lenBytes;
        return out;
    }

private:
    const std::vector<uint8_t>& m_buf;
    size_t      m_pos = 0;
    bool        m_failed = false;
    std::string m_error;
};

// ─────────────────────────────────────────────────────────────────
// Per-section parsers
// ─────────────────────────────────────────────────────────────────

bool readHeader(Reader& r, Header& h)
{
    char magic[4];
    r.readBytes(magic, 4);
    if (!r.ok()) return false;
    if (std::memcmp(magic, "PMX ", 4) != 0) {
        r.fail("not a PMX file (bad magic)");
        return false;
    }

    h.version = r.read<float>();
    if (!r.ok()) return false;
    if (h.version != 2.0f && h.version != 2.1f) {
        r.fail("unsupported PMX version (only 2.0 / 2.1)");
        return false;
    }

    const uint8_t globalCount = r.read<uint8_t>();
    if (!r.ok() || globalCount < 8) {
        r.fail("bad globals count");
        return false;
    }

    h.encoding         = r.read<uint8_t>();
    h.uvCount          = r.read<uint8_t>();
    h.vertexIndexSize  = r.read<uint8_t>();
    h.textureIndexSize = r.read<uint8_t>();
    h.materialIndexSize= r.read<uint8_t>();
    h.boneIndexSize    = r.read<uint8_t>();
    h.morphIndexSize   = r.read<uint8_t>();
    h.rigidIndexSize   = r.read<uint8_t>();

    // Skip any extra globals (forward-compat)
    if (globalCount > 8) r.skip(globalCount - 8);

    h.nameJa    = r.readText(h.encoding);
    h.nameEn    = r.readText(h.encoding);
    h.commentJa = r.readText(h.encoding);
    h.commentEn = r.readText(h.encoding);

    return r.ok();
}

bool readVertices(Reader& r, const Header& h, std::vector<Vertex>& out)
{
    const int32_t count = r.read<int32_t>();
    if (!r.ok() || count < 0) return false;

    out.resize(count);
    for (int32_t i = 0; i < count; ++i) {
        Vertex& v = out[i];

        v.position = r.read<XMFLOAT3>();
        v.normal   = r.read<XMFLOAT3>();
        v.uv       = r.read<XMFLOAT2>();

        for (uint8_t u = 0; u < h.uvCount; ++u) {
            if (u < 4) v.additionalUVs[u] = r.read<XMFLOAT4>();
            else r.skip(sizeof(XMFLOAT4));
        }

        v.weightType = static_cast<WeightType>(r.read<uint8_t>());
        if (!r.ok()) return false;

        switch (v.weightType) {
        case WeightType::BDEF1:
            v.boneIndices[0] = r.readIndex(h.boneIndexSize);
            v.boneWeights[0] = 1.0f;
            break;
        case WeightType::BDEF2:
            v.boneIndices[0] = r.readIndex(h.boneIndexSize);
            v.boneIndices[1] = r.readIndex(h.boneIndexSize);
            v.boneWeights[0] = r.read<float>();
            v.boneWeights[1] = 1.0f - v.boneWeights[0];
            break;
        case WeightType::BDEF4:
        case WeightType::QDEF:
            v.boneIndices[0] = r.readIndex(h.boneIndexSize);
            v.boneIndices[1] = r.readIndex(h.boneIndexSize);
            v.boneIndices[2] = r.readIndex(h.boneIndexSize);
            v.boneIndices[3] = r.readIndex(h.boneIndexSize);
            v.boneWeights[0] = r.read<float>();
            v.boneWeights[1] = r.read<float>();
            v.boneWeights[2] = r.read<float>();
            v.boneWeights[3] = r.read<float>();
            break;
        case WeightType::SDEF:
            v.boneIndices[0] = r.readIndex(h.boneIndexSize);
            v.boneIndices[1] = r.readIndex(h.boneIndexSize);
            v.boneWeights[0] = r.read<float>();
            v.boneWeights[1] = 1.0f - v.boneWeights[0];
            v.sdefC  = r.read<XMFLOAT3>();
            v.sdefR0 = r.read<XMFLOAT3>();
            v.sdefR1 = r.read<XMFLOAT3>();
            break;
        default:
            r.fail("bad weight type");
            return false;
        }

        v.edgeRatio = r.read<float>();
        if (!r.ok()) return false;
    }
    return true;
}

bool readFaces(Reader& r, const Header& h, std::vector<Face>& out)
{
    const int32_t count = r.read<int32_t>();
    if (!r.ok() || count < 0 || count % 3 != 0) {
        r.fail("bad face count");
        return false;
    }
    const int32_t triCount = count / 3;
    out.resize(triCount);
    for (int32_t i = 0; i < triCount; ++i) {
        out[i][0] = r.readVertexIndex(h.vertexIndexSize);
        out[i][1] = r.readVertexIndex(h.vertexIndexSize);
        out[i][2] = r.readVertexIndex(h.vertexIndexSize);
    }
    return r.ok();
}

bool readTextures(Reader& r, const Header& h, std::vector<std::string>& out)
{
    const int32_t count = r.read<int32_t>();
    if (!r.ok() || count < 0) return false;
    out.resize(count);
    for (int32_t i = 0; i < count; ++i) {
        out[i] = r.readText(h.encoding);
    }
    return r.ok();
}

bool readMaterials(Reader& r, const Header& h, std::vector<Material>& out)
{
    const int32_t count = r.read<int32_t>();
    if (!r.ok() || count < 0) return false;
    out.resize(count);
    for (int32_t i = 0; i < count; ++i) {
        Material& m = out[i];
        m.nameJa = r.readText(h.encoding);
        m.nameEn = r.readText(h.encoding);

        m.diffuse   = r.read<XMFLOAT4>();
        m.specular  = r.read<XMFLOAT3>();
        m.shininess = r.read<float>();
        m.ambient   = r.read<XMFLOAT3>();

        m.flags = r.read<uint8_t>();

        m.edgeColor = r.read<XMFLOAT4>();
        m.edgeSize  = r.read<float>();

        m.diffuseTextureIndex = r.readIndex(h.textureIndexSize);
        m.sphereTextureIndex  = r.readIndex(h.textureIndexSize);
        m.sphereMode = static_cast<SphereMode>(r.read<uint8_t>());

        m.toonRef = static_cast<ToonRef>(r.read<uint8_t>());
        if (m.toonRef == ToonRef::Texture) {
            m.toonIndex = r.readIndex(h.textureIndexSize);
        } else {
            m.toonIndex = static_cast<int32_t>(r.read<uint8_t>());
        }

        m.memo = r.readText(h.encoding);
        m.indexCount = r.read<int32_t>();

        if (!r.ok()) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────
// Skip the trailing sections (bones / morphs / display / rigid / joint / soft).
// We don't use them yet but must consume bytes correctly so downstream
// code (if any) sees clean EOF, and so we can validate the file fully parses.
// ─────────────────────────────────────────────────────────────────

bool skipBones(Reader& r, const Header& h)
{
    const int32_t count = r.read<int32_t>();
    if (!r.ok() || count < 0) return false;
    for (int32_t i = 0; i < count; ++i) {
        r.readText(h.encoding); // name JA
        r.readText(h.encoding); // name EN
        r.skip(sizeof(XMFLOAT3)); // position
        r.readIndex(h.boneIndexSize); // parent
        r.skip(sizeof(int32_t)); // layer
        const uint16_t flags = r.read<uint16_t>();
        if (!r.ok()) return false;

        if (flags & 0x0001) r.readIndex(h.boneIndexSize); // tail bone index
        else r.skip(sizeof(XMFLOAT3));                    // tail offset

        if (flags & 0x0100) { // assign rotate
            r.readIndex(h.boneIndexSize);
            r.skip(sizeof(float));
        }
        if (flags & 0x0200) { // assign translate
            r.readIndex(h.boneIndexSize);
            r.skip(sizeof(float));
        }
        if (flags & 0x0400) r.skip(sizeof(XMFLOAT3));     // fixed axis
        if (flags & 0x0800) r.skip(sizeof(XMFLOAT3) * 2); // local axis
        if (flags & 0x2000) r.skip(sizeof(int32_t));      // external parent

        if (flags & 0x0020) { // IK
            r.readIndex(h.boneIndexSize);   // target
            r.skip(sizeof(int32_t));        // loop count
            r.skip(sizeof(float));          // limit
            const int32_t linkCount = r.read<int32_t>();
            if (!r.ok() || linkCount < 0) return false;
            for (int32_t k = 0; k < linkCount; ++k) {
                r.readIndex(h.boneIndexSize); // link bone
                const uint8_t hasLimit = r.read<uint8_t>();
                if (hasLimit) r.skip(sizeof(XMFLOAT3) * 2);
            }
        }

        if (!r.ok()) return false;
    }
    return true;
}

bool skipMorphs(Reader& r, const Header& h)
{
    const int32_t count = r.read<int32_t>();
    if (!r.ok() || count < 0) return false;
    for (int32_t i = 0; i < count; ++i) {
        r.readText(h.encoding);
        r.readText(h.encoding);
        r.skip(sizeof(uint8_t));               // panel
        const uint8_t type = r.read<uint8_t>();
        const int32_t offsetCount = r.read<int32_t>();
        if (!r.ok() || offsetCount < 0) return false;

        size_t bytesPerOffset = 0;
        switch (type) {
        case 0: // group
            bytesPerOffset = h.morphIndexSize + sizeof(float);
            break;
        case 1: // vertex
            bytesPerOffset = h.vertexIndexSize + sizeof(XMFLOAT3);
            break;
        case 2: // bone
            bytesPerOffset = h.boneIndexSize + sizeof(XMFLOAT3) + sizeof(XMFLOAT4);
            break;
        case 3: // UV
        case 4: case 5: case 6: case 7: // additional UVs
            bytesPerOffset = h.vertexIndexSize + sizeof(XMFLOAT4);
            break;
        case 8: // material
            bytesPerOffset = h.materialIndexSize + 1
                           + sizeof(XMFLOAT4)         // diffuse
                           + sizeof(XMFLOAT3)         // specular
                           + sizeof(float)            // shininess
                           + sizeof(XMFLOAT3)         // ambient
                           + sizeof(XMFLOAT4)         // edge color
                           + sizeof(float)            // edge size
                           + sizeof(XMFLOAT4) * 3;    // tex / sphere / toon factor
            break;
        case 9: // flip (PMX 2.1)
            bytesPerOffset = h.morphIndexSize + sizeof(float);
            break;
        case 10: // impulse (PMX 2.1)
            bytesPerOffset = h.rigidIndexSize + 1
                           + sizeof(XMFLOAT3) * 2;
            break;
        default:
            r.fail("unknown morph type");
            return false;
        }
        r.skip(static_cast<size_t>(offsetCount) * bytesPerOffset);
        if (!r.ok()) return false;
    }
    return true;
}

bool skipDisplayFrames(Reader& r, const Header& h)
{
    const int32_t count = r.read<int32_t>();
    if (!r.ok() || count < 0) return false;
    for (int32_t i = 0; i < count; ++i) {
        r.readText(h.encoding);
        r.readText(h.encoding);
        r.skip(sizeof(uint8_t)); // special
        const int32_t elemCount = r.read<int32_t>();
        if (!r.ok() || elemCount < 0) return false;
        for (int32_t k = 0; k < elemCount; ++k) {
            const uint8_t kind = r.read<uint8_t>();
            if (kind == 0)      r.readIndex(h.boneIndexSize);
            else                r.readIndex(h.morphIndexSize);
        }
        if (!r.ok()) return false;
    }
    return true;
}

bool skipRigidBodies(Reader& r, const Header& h)
{
    const int32_t count = r.read<int32_t>();
    if (!r.ok() || count < 0) return false;
    for (int32_t i = 0; i < count; ++i) {
        r.readText(h.encoding);
        r.readText(h.encoding);
        r.readIndex(h.boneIndexSize);
        r.skip(1);                         // group
        r.skip(2);                         // non-collision flags
        r.skip(1);                         // shape
        r.skip(sizeof(XMFLOAT3) * 3);      // size, pos, rot
        r.skip(sizeof(float) * 5);         // mass, linDamp, angDamp, restitution, friction
        r.skip(1);                         // physics mode
        if (!r.ok()) return false;
    }
    return true;
}

bool skipJoints(Reader& r, const Header& h)
{
    const int32_t count = r.read<int32_t>();
    if (!r.ok() || count < 0) return false;
    for (int32_t i = 0; i < count; ++i) {
        r.readText(h.encoding);
        r.readText(h.encoding);
        r.skip(1);                         // type
        r.readIndex(h.rigidIndexSize);
        r.readIndex(h.rigidIndexSize);
        r.skip(sizeof(XMFLOAT3) * 2);      // pos, rot
        r.skip(sizeof(XMFLOAT3) * 4);      // limits
        r.skip(sizeof(XMFLOAT3) * 2);      // springs
        if (!r.ok()) return false;
    }
    return true;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────
// Public entry
// ─────────────────────────────────────────────────────────────────

bool loadModel(const std::filesystem::path& filepath, Model& outModel, std::string& outError)
{
    std::ifstream f(filepath, std::ios::binary | std::ios::ate);
    if (!f) {
        outError = "cannot open file";
        return false;
    }
    const auto sz = f.tellg();
    f.seekg(0);

    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    if (!f) {
        outError = "read error";
        return false;
    }

    Reader r(buf);

    if (!readHeader(r, outModel.header))     { outError = r.error().empty() ? "header parse failed" : r.error(); return false; }
    if (!readVertices(r, outModel.header, outModel.vertices)) { outError = r.error(); return false; }
    if (!readFaces(r, outModel.header, outModel.faces))       { outError = r.error(); return false; }
    if (!readTextures(r, outModel.header, outModel.texturePaths)) { outError = r.error(); return false; }
    if (!readMaterials(r, outModel.header, outModel.materials))   { outError = r.error(); return false; }

    // The remaining sections are skipped, but we still parse them to validate
    if (!skipBones(r, outModel.header))         { outError = r.error(); return false; }
    if (!skipMorphs(r, outModel.header))        { outError = r.error(); return false; }
    if (!skipDisplayFrames(r, outModel.header)) { outError = r.error(); return false; }
    if (!skipRigidBodies(r, outModel.header))   { outError = r.error(); return false; }
    if (!skipJoints(r, outModel.header))        { outError = r.error(); return false; }
    // PMX 2.1 may add soft bodies here; we don't bother — bytes after this point are unused.

    spdlog::info("PMX: '{}' v{:.1f} | {} verts, {} tris, {} mats, {} textures",
                 outModel.header.nameJa,
                 outModel.header.version,
                 outModel.vertices.size(),
                 outModel.faces.size(),
                 outModel.materials.size(),
                 outModel.texturePaths.size());

    return true;
}

} // namespace pmx
