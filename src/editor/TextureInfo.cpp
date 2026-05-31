#include "TextureInfo.h"

#include "ChannelColors.h"

namespace editor {

namespace {

/// Convert a channel_colors::Tint to a QColor (8-bit per channel).
QColor toQColor(const channel_colors::Tint& t)
{
    auto u8 = [](float v) {
        const int n = static_cast<int>(v * 255.0f + 0.5f);
        return n < 0 ? 0 : (n > 255 ? 255 : n);
    };
    return QColor(u8(t.r), u8(t.g), u8(t.b));
}

// Convenience: per-kind tint accessors (same single source of truth as the
// 3D mask overlay shader uses).
const QColor kShadowRateR = toQColor(channel_colors::tintsFor(TextureKind::ShadowRate)[0]);
const QColor kShadowRateG = toQColor(channel_colors::tintsFor(TextureKind::ShadowRate)[1]);
const QColor kShadowRateB = toQColor(channel_colors::tintsFor(TextureKind::ShadowRate)[2]);
const QColor kSubLight1   = toQColor(channel_colors::tintsFor(TextureKind::SubLightRate)[0]);
const QColor kSubLight2   = toQColor(channel_colors::tintsFor(TextureKind::SubLightRate)[1]);
const QColor kCancel      = toQColor(channel_colors::tintsFor(TextureKind::SubLightRate)[2]);
const QColor kEdgeColor   = toQColor(channel_colors::tintsFor(TextureKind::EdgeRate)[0]);
const QColor kSDFColor    = toQColor(channel_colors::tintsFor(TextureKind::FaceSDF)[0]);
const QColor kUnusedColor (0x40, 0x42, 0x4a);  // dark grey, UI-only

// ─────────────────────────────────────────────────────────────────
// Shadow Rate
// ─────────────────────────────────────────────────────────────────
const TextureInfo kShadowRate = {
    QStringLiteral("Shadow Rate"),
    QStringLiteral("shadow_rate.png"),
    QStringLiteral(
        "控制材质在 toon 分级中倾向于哪个色阶。"
        "msTC 的 Shade.fx 会根据 ShadeRT 通道阈值决定每个像素出 Base / Highlight / Shadow 哪一档。"
    ),
    QStringLiteral(
        "提示：在希望强制变亮的部位涂 R（Base）或 G（Highlight），在希望加深的部位涂 B（Shadow）。"
        "默认黑色表示不干预 toon 判断，由 ExcellentShadow 的光照计算决定。"
    ),
    {{
        { QChar('R'), kShadowRateR, QStringLiteral("Base"),
          QStringLiteral("Base 色（中间调）出易度。涂亮 → 该像素更容易停留在 Base 档。常用于皮肤、衣物中性区。"),
          true },
        { QChar('G'), kShadowRateG, QStringLiteral("Highlight"),
          QStringLiteral("高光色出易度。涂亮 → 该像素更容易进入 Highlight 档。常用于鼻梁、肩部、金属边等需要强调反光的位置。"),
          true },
        { QChar('B'), kShadowRateB, QStringLiteral("Shadow"),
          QStringLiteral("阴影色出易度。涂亮 → 该像素更容易进入 Shadow 档。常用于颈下、腋下等结构阴影位置。注意：实际算法是 `Color.b *= (1 - ShadowRate.b)`，建议视觉测试调整。"),
          true },
        { QChar('A'), kUnusedColor, QStringLiteral("—"),
          QStringLiteral("Alpha 通道未使用，保持 255。"),
          false },
    }}
};

// ─────────────────────────────────────────────────────────────────
// SubLight Rate
// ─────────────────────────────────────────────────────────────────
const TextureInfo kSubLightRate = {
    QStringLiteral("SubLight Rate"),
    QStringLiteral("sublight_rate.png"),
    QStringLiteral(
        "副光遮罩。控制材质是否额外被 SubLight1/SubLight2 染色，以及是否取消高光/副光。"
        "用于实现轮廓光、反射光、特定区域纯色等效果。"
    ),
    QStringLiteral(
        "提示：通常一张材质只涂 R 或 G 中一个通道。"
        "B 通道是取消遮罩 —— 涂亮 B 会把该像素从所有副光和高光中排除（适合背光面、纯色部位）。"
    ),
    {{
        { QChar('R'), kSubLight1, QStringLiteral("SubLight1"),
          QStringLiteral("副光1 出易度。涂亮 → 该像素强制接收 SubLight1 染色。常用于侧后方轮廓光、暖色补光。"),
          true },
        { QChar('G'), kSubLight2, QStringLiteral("SubLight2"),
          QStringLiteral("副光2 出易度。涂亮 → 该像素强制接收 SubLight2 染色。常用于反方向的冷色补光或第二组轮廓光。"),
          true },
        { QChar('B'), kCancel, QStringLiteral("Cancel"),
          QStringLiteral("高光/副光取消遮罩。涂亮 B → 该像素不会出现 Highlight、SubLight1、SubLight2 任何染色。适合需要保持纯色的区域。"),
          true },
        { QChar('A'), kUnusedColor, QStringLiteral("—"),
          QStringLiteral("Alpha 通道未使用，保持 255。"),
          false },
    }}
};

// ─────────────────────────────────────────────────────────────────
// Edge Rate
// ─────────────────────────────────────────────────────────────────
const TextureInfo kEdgeRate = {
    QStringLiteral("Edge Rate"),
    QStringLiteral("edge_rate.png"),
    QStringLiteral(
        "描边控制。控制材质是否在 Edge.fx 后处理中出现描边。"
        "msTC 的 Edge.fx 实际逻辑是 `EdgeBi = step(threshold, EdgeRT.r)`，所以涂亮 R 反而会压低 EdgeRT.r 让边线更明显。"
    ),
    QStringLiteral(
        "提示：只用 R 通道。涂亮 R → 该像素更容易出现描边；保持黑色 → 沿用默认 toon 描边规则。"
        "常用于希望强制有边线的眼线、嘴线、外轮廓位置。"
    ),
    {{
        { QChar('R'), kEdgeColor, QStringLiteral("Edge"),
          QStringLiteral("描边强制度。涂亮 R → 该像素优先出现描边。整张贴图保持黑色 = 完全沿用 toon 默认。"),
          true },
        { QChar('G'), kUnusedColor, QStringLiteral("—"),
          QStringLiteral("Edge.fx 中未读取 G 通道，留空即可。"),
          false },
        { QChar('B'), kUnusedColor, QStringLiteral("—"),
          QStringLiteral("Edge.fx 中未读取 B 通道，留空即可。"),
          false },
        { QChar('A'), kUnusedColor, QStringLiteral("—"),
          QStringLiteral("Alpha 通道未使用，保持 255。"),
          false },
    }}
};

// ─────────────────────────────────────────────────────────────────
// Face SDF
// ─────────────────────────────────────────────────────────────────
const TextureInfo kFaceSDF = {
    QStringLiteral("Face SDF"),
    QStringLiteral("face_sdf.png"),
    QStringLiteral(
        "面部阴影距离场。灰度距离场贴图，配合 msTC 控制面部阴影形状。"
        "R/G/B 三通道复制同一份灰度数据，A 保持 255。"
    ),
    QStringLiteral(
        "提示：通常通过 Bézier 轮廓自动生成（Phase 8 工具）。"
        "灰度值代表从亮区到阴影区的距离 —— 中间灰是阴影边界，越深越深入阴影区。"
    ),
    {{
        { QChar('R'), kSDFColor, QStringLiteral("SDF"),
          QStringLiteral("灰度距离场。0 = 完全阴影区，255 = 完全亮区，中间灰是过渡。在 Shade.fx 中用于判断面部光影边界。"),
          true },
        { QChar('G'), kSDFColor, QStringLiteral("(= R)"),
          QStringLiteral("镜像 R 通道，方便部分 shader 直接采样 G。绘制时保持与 R 一致即可。"),
          true },
        { QChar('B'), kSDFColor, QStringLiteral("(= R)"),
          QStringLiteral("镜像 R 通道。绘制时保持与 R 一致即可。"),
          true },
        { QChar('A'), kUnusedColor, QStringLiteral("—"),
          QStringLiteral("Alpha 通道未使用，保持 255。"),
          false },
    }}
};

} // anonymous

const TextureInfo& textureInfo(TextureKind k)
{
    switch (k) {
    case TextureKind::ShadowRate:   return kShadowRate;
    case TextureKind::SubLightRate: return kSubLightRate;
    case TextureKind::EdgeRate:     return kEdgeRate;
    case TextureKind::FaceSDF:      return kFaceSDF;
    default:                        return kShadowRate;
    }
}

} // namespace editor
