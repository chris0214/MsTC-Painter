#include "BrushPanel.h"

#include "TextureInfo.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

namespace editor {

namespace {

/// Build a colored style for a channel button so its checked color matches
/// the semantic chip color in InfoPanel.
QString buttonStyleFor(const QColor& accent, bool used)
{
    if (!used) {
        return QStringLiteral(
            "QPushButton { color: #555861; background: #20222a; border-color: #20222a; }"
        );
    }
    const QString hex = accent.name();
    return QString(
        "QPushButton { background-color: #2a2d36; border: 1px solid #383b46; color: #d6d8de; border-radius: 4px; padding: 5px 8px; }"
        "QPushButton:hover { border-color: %1; }"
        "QPushButton:checked { background-color: %1; border-color: %1; color: #14161b; font-weight: 600; }"
    ).arg(hex);
}

} // anonymous

// ─────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────

BrushPanel::BrushPanel(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);

    // ── Tool selection row ──────────────────────────────────────
    {
        auto* row = new QWidget(this);
        auto* hl  = new QHBoxLayout(row);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(4);

        auto makeToolBtn = [&](const QString& text, const QString& tip, QPushButton** out) {
            auto* b = new QPushButton(text, row);
            b->setCheckable(true);
            b->setToolTip(tip);
            b->setMinimumWidth(90);
            hl->addWidget(b);
            *out = b;
        };
        makeToolBtn("\xF0\x9F\x96\x8C  " + tr("Brush"),
                    tr("Brush — paint freehand strokes (B)"),     &m_toolBrush);
        makeToolBtn("\xF0\x9F\xAA\xA3  " + tr("Bucket"),
                    tr("Bucket — flood-fill a region (G)"),       &m_toolBucket);
        makeToolBtn("\xF0\x9F\x92\xA7  " + tr("Eyedrop"),
                    tr("Eyedropper — pick the active channel value (I)"), &m_toolEyedropper);
        m_toolBrush->setChecked(true);

        // Mutually exclusive selection
        auto onToolToggled = [this](CanvasTool t, bool on) {
            if (!on) return;
            m_toolBrush     ->setChecked(t == CanvasTool::Brush);
            m_toolBucket    ->setChecked(t == CanvasTool::Bucket);
            m_toolEyedropper->setChecked(t == CanvasTool::Eyedropper);
            if (m_bucketRow) m_bucketRow->setVisible(t == CanvasTool::Bucket);
            m_tool = t;
            emit toolChanged(t);
        };
        connect(m_toolBrush,      &QPushButton::toggled, this, [=](bool on){ onToolToggled(CanvasTool::Brush,      on); });
        connect(m_toolBucket,     &QPushButton::toggled, this, [=](bool on){ onToolToggled(CanvasTool::Bucket,     on); });
        connect(m_toolEyedropper, &QPushButton::toggled, this, [=](bool on){ onToolToggled(CanvasTool::Eyedropper, on); });

        hl->addStretch();
        root->addWidget(row);
    }

    // ── Bucket-only settings row ────────────────────────────────
    {
        m_bucketRow = new QWidget(this);
        auto* hl = new QHBoxLayout(m_bucketRow);
        hl->setContentsMargins(0, 0, 0, 0);

        auto* l = new QLabel(tr("Tolerance:"), m_bucketRow);
        l->setMinimumWidth(70);
        hl->addWidget(l);

        m_toleranceSlider = new QSlider(Qt::Horizontal, m_bucketRow);
        m_toleranceSlider->setRange(0, 255);
        m_toleranceSlider->setValue(32);
        hl->addWidget(m_toleranceSlider, 1);

        m_toleranceLabel = new QLabel("32", m_bucketRow);
        m_toleranceLabel->setMinimumWidth(30);
        m_toleranceLabel->setAlignment(Qt::AlignRight);
        hl->addWidget(m_toleranceLabel);

        m_scopeCombo = new QComboBox(m_bucketRow);
        m_scopeCombo->addItem(tr("Color Connected"),  static_cast<int>(FillScope::ColorConnected));
        m_scopeCombo->addItem(tr("UV Island"),        static_cast<int>(FillScope::UvIsland));
        m_scopeCombo->addItem(tr("Color + UV Bound"), static_cast<int>(FillScope::ColorAndUv));
        m_scopeCombo->setCurrentIndex(2);   // default Color + UV Bound
        m_scopeCombo->setToolTip(
            tr("<b>Color Connected</b> — Photoshop bucket: spread by color similarity (tolerance).<br>"
               "<b>UV Island</b> — fill the entire UV island regardless of current color.<br>"
               "<b>Color + UV Bound</b> — color similarity, but limited to UV-mapped texels.")
        );
        hl->addWidget(m_scopeCombo);

        connect(m_toleranceSlider, &QSlider::valueChanged, this, [this](int v) {
            m_toleranceLabel->setText(QString::number(v));
            emit fillToleranceChanged(v);
        });
        connect(m_scopeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int idx) {
            const auto s = static_cast<FillScope>(m_scopeCombo->itemData(idx).toInt());
            emit fillScopeChanged(s);
        });

        m_bucketRow->setVisible(false);  // shown only when Bucket tool active
        root->addWidget(m_bucketRow);
    }

    // ── Sliders (size / hardness / opacity / spacing) ──────────
    auto makeSliderRow = [&](const QString& label, int initial, int lo, int hi,
                              QSlider** sliderOut, QLabel** valueOut)
    {
        auto* row = new QWidget(this);
        auto* hl  = new QHBoxLayout(row);
        hl->setContentsMargins(0, 0, 0, 0);

        auto* l = new QLabel(label, row);
        l->setMinimumWidth(70);
        hl->addWidget(l);

        auto* s = new QSlider(Qt::Horizontal, row);
        s->setRange(lo, hi);
        s->setValue(initial);
        hl->addWidget(s, 1);

        auto* v = new QLabel(QString::number(initial), row);
        v->setMinimumWidth(40);
        v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        hl->addWidget(v);

        root->addWidget(row);
        *sliderOut = s;
        *valueOut  = v;
    };

    makeSliderRow(tr("Size"),     32, 1, 512,  &m_sizeSlider,     &m_sizeLabel);
    makeSliderRow(tr("Hardness"), 70, 0, 100,  &m_hardnessSlider, &m_hardnessLabel);
    makeSliderRow(tr("Opacity"), 100, 0, 100,  &m_opacitySlider,  &m_opacityLabel);
    makeSliderRow(tr("Spacing"),  8,  1, 32,   &m_spacingSlider,  &m_spacingLabel);
    makeSliderRow(tr("Smoothing"), 0, 0, 100,  &m_smoothingSlider,&m_smoothingLabel);

    // ── Flow mode (Wash / Buildup) ──────────────────────────────
    {
        auto* row = new QWidget(this);
        auto* hl  = new QHBoxLayout(row);
        hl->setContentsMargins(0, 0, 0, 0);

        auto* l = new QLabel(tr("Flow:"), row);
        l->setMinimumWidth(70);
        hl->addWidget(l);

        m_flowWash = new QPushButton(tr("Wash"), row);
        m_flowWash->setCheckable(true);
        m_flowWash->setChecked(true);
        m_flowWash->setToolTip(
            tr("<b>Wash mode</b><br>"
               "Within a single stroke, total opacity is capped at the slider value. "
               "Like Krita's alpha_darken / Photoshop default. "
               "Repeated strokes still build up."));
        hl->addWidget(m_flowWash);

        m_flowBuildup = new QPushButton(tr("Buildup"), row);
        m_flowBuildup->setCheckable(true);
        m_flowBuildup->setToolTip(
            tr("<b>Buildup mode</b><br>"
               "Each dab blends naively. Overlapping dabs within a single stroke "
               "accumulate past the opacity slider value. Useful for "
               "spray-can / airbrush effects."));
        hl->addWidget(m_flowBuildup);

        // Wash and Buildup are mutually exclusive
        connect(m_flowWash, &QPushButton::toggled, this, [this](bool on) {
            if (on) m_flowBuildup->setChecked(false);
            else if (!m_flowBuildup->isChecked()) m_flowWash->setChecked(true);  // can't both be off
        });
        connect(m_flowBuildup, &QPushButton::toggled, this, [this](bool on) {
            if (on) m_flowWash->setChecked(false);
            else if (!m_flowWash->isChecked()) m_flowBuildup->setChecked(true);
        });

        hl->addStretch();
        root->addWidget(row);
    }

    // ── Channel toggle buttons ─────────────────────────────────
    {
        auto* row = new QWidget(this);
        auto* hl  = new QHBoxLayout(row);
        hl->setContentsMargins(0, 0, 0, 0);

        auto* l = new QLabel(tr("Channels:"), row);
        l->setMinimumWidth(70);
        hl->addWidget(l);

        auto makeChan = [&](QPushButton** out, const QString& tip) {
            auto* b = new QPushButton(row);
            b->setCheckable(true);
            b->setChecked(true);
            b->setMinimumWidth(70);
            b->setToolTip(tip);
            hl->addWidget(b);
            *out = b;
        };
        makeChan(&m_chanR, "Red channel");
        makeChan(&m_chanG, "Green channel");
        makeChan(&m_chanB, "Blue channel");

        hl->addStretch();
        root->addWidget(row);
    }

    // ── Eraser + Mirror X + Ink swatch ──────────────────────────
    {
        auto* row = new QWidget(this);
        auto* hl  = new QHBoxLayout(row);
        hl->setContentsMargins(0, 0, 0, 0);

        m_eraserCheck = new QCheckBox(tr("Eraser"), row);
        m_eraserCheck->setToolTip(tr("When on, the brush paints toward 0 on the active channels."));
        hl->addWidget(m_eraserCheck);

        m_mirrorXCheck = new QCheckBox(tr("Mirror X"), row);
        m_mirrorXCheck->setToolTip(
            tr("<b>Symmetric paint</b><br>"
               "Each dab is also painted at the X-mirrored position in UV space. "
               "Use on character textures with symmetric UV layouts to paint "
               "both sides at once (eyes, eyebrows, cheeks, etc.)"));
        hl->addWidget(m_mirrorXCheck);

        hl->addStretch();

        // Ink display: a small colored square + numeric label
        auto* inkContainer = new QWidget(row);
        auto* inkLay = new QHBoxLayout(inkContainer);
        inkLay->setContentsMargins(0, 0, 0, 0);
        inkLay->setSpacing(4);

        auto* inkText = new QLabel(tr("Ink:"), inkContainer);
        inkText->setStyleSheet("color: #909298;");
        inkLay->addWidget(inkText);

        m_inkSwatch = new QFrame(inkContainer);
        m_inkSwatch->setFixedSize(18, 18);
        m_inkSwatch->setStyleSheet("background-color: #ffffff; border: 1px solid #555861; border-radius: 3px;");
        m_inkSwatch->setToolTip(tr("Current ink value for the active channel. Updates via the eyedropper (I)."));
        inkLay->addWidget(m_inkSwatch);

        m_inkLabel = new QLabel("255", inkContainer);
        m_inkLabel->setMinimumWidth(28);
        m_inkLabel->setStyleSheet("color: #d6d8de; font-family: Consolas, 'Courier New', monospace;");
        inkLay->addWidget(m_inkLabel);

        hl->addWidget(inkContainer);
        root->addWidget(row);
    }

    rebuildChannelLabels();

    // ── Connect everything to push settings to brush ────────────
    auto pushOnChange = [this]() { emitSettingsToBrush(); };
    auto labelOnChange = [this](QSlider* s, QLabel* l, const QString& suffix) {
        connect(s, &QSlider::valueChanged, l, [l, suffix](int v) {
            l->setText(QString::number(v) + suffix);
        });
    };
    labelOnChange(m_sizeSlider,      m_sizeLabel,      "");
    labelOnChange(m_hardnessSlider,  m_hardnessLabel,  "%");
    labelOnChange(m_opacitySlider,   m_opacityLabel,   "%");
    labelOnChange(m_spacingSlider,   m_spacingLabel,   "");
    labelOnChange(m_smoothingSlider, m_smoothingLabel, "%");

    // Initialize text suffixes
    m_hardnessLabel->setText("70%");
    m_opacityLabel->setText("100%");
    m_smoothingLabel->setText("0%");

    connect(m_sizeSlider,      &QSlider::valueChanged, this, pushOnChange);
    connect(m_hardnessSlider,  &QSlider::valueChanged, this, pushOnChange);
    connect(m_opacitySlider,   &QSlider::valueChanged, this, pushOnChange);
    connect(m_spacingSlider,   &QSlider::valueChanged, this, pushOnChange);
    connect(m_smoothingSlider, &QSlider::valueChanged, this, pushOnChange);
    connect(m_chanR,           &QPushButton::toggled,  this, pushOnChange);
    connect(m_chanG,           &QPushButton::toggled,  this, pushOnChange);
    connect(m_chanB,           &QPushButton::toggled,  this, pushOnChange);
    connect(m_flowWash,        &QPushButton::toggled,  this, pushOnChange);
    connect(m_flowBuildup,     &QPushButton::toggled,  this, pushOnChange);
    connect(m_eraserCheck,     &QCheckBox::toggled,    this, pushOnChange);
    connect(m_mirrorXCheck,    &QCheckBox::toggled,    this, pushOnChange);
}

// ─────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────

void BrushPanel::setBrush(BrushEngine* brush)
{
    m_brush = brush;
    if (m_brush) emitSettingsToBrush();
}

void BrushPanel::setActiveKind(TextureKind kind)
{
    if (m_kind == kind) return;
    m_kind = kind;
    rebuildChannelLabels();

    // Disable buttons for unused channels per the texture's spec.
    const TextureInfo& info = textureInfo(kind);
    m_chanR->setEnabled(info.channels[0].used);
    m_chanG->setEnabled(info.channels[1].used);
    m_chanB->setEnabled(info.channels[2].used);

    // Auto-uncheck disabled channels; ensure at least one is checked.
    if (!info.channels[0].used) m_chanR->setChecked(false);
    if (!info.channels[1].used) m_chanG->setChecked(false);
    if (!info.channels[2].used) m_chanB->setChecked(false);

    if (!m_chanR->isChecked() && !m_chanG->isChecked() && !m_chanB->isChecked()) {
        // First available channel
        if (info.channels[0].used) m_chanR->setChecked(true);
        else if (info.channels[1].used) m_chanG->setChecked(true);
        else if (info.channels[2].used) m_chanB->setChecked(true);
    }

    emitSettingsToBrush();
}

void BrushPanel::rebuildChannelLabels()
{
    const TextureInfo& info = textureInfo(m_kind);

    auto applyToButton = [&](QPushButton* btn, const ChannelInfo& ch) {
        btn->setText(QString("%1: %2").arg(ch.letter).arg(ch.shortLabel));
        btn->setStyleSheet(buttonStyleFor(ch.chipColor, ch.used));

        const QString tip = QString(
            "<b style='color:%1'>%2 channel — %3</b><br><br>"
            "%4<br><br>"
            "<span style='color:#909298'>Tab: %5 &nbsp;·&nbsp; File: %6</span>"
        ).arg(ch.chipColor.name())
         .arg(ch.letter)
         .arg(ch.shortLabel)
         .arg(ch.description)
         .arg(info.title)
         .arg(info.filename);
        btn->setToolTip(tip);
    };

    applyToButton(m_chanR, info.channels[0]);
    applyToButton(m_chanG, info.channels[1]);
    applyToButton(m_chanB, info.channels[2]);
}

void BrushPanel::emitSettingsToBrush()
{
    if (!m_brush) return;

    // Preserve the current ink (eyedropper may have set it). Start from the
    // brush's existing settings, override only what the panel controls.
    BrushSettings s = m_brush->settings();
    s.radiusPx  = static_cast<float>(m_sizeSlider->value());
    s.hardness  = m_hardnessSlider->value() / 100.0f;
    s.opacity   = m_opacitySlider->value()  / 100.0f;
    s.spacingPx = static_cast<float>(m_spacingSlider->value());
    s.smoothing = m_smoothingSlider->value() / 100.0f;

    uint8_t mask = 0;
    if (m_chanR->isChecked()) mask |= ChannelMaskR;
    if (m_chanG->isChecked()) mask |= ChannelMaskG;
    if (m_chanB->isChecked()) mask |= ChannelMaskB;
    s.channelMask = mask;

    s.erase   = m_eraserCheck->isChecked();
    s.mirrorX = m_mirrorXCheck->isChecked();
    s.flowMode = m_flowBuildup->isChecked() ? FlowMode::Buildup : FlowMode::Wash;

    m_brush->setSettings(s);
    rebuildInkSwatch();
}

// ─────────────────────────────────────────────────────────────────
// New in Phase 4.2: tool sync + ink swatch
// ─────────────────────────────────────────────────────────────────

void BrushPanel::syncToolFromCanvas(CanvasTool t)
{
    if (m_tool == t) return;
    m_tool = t;
    QSignalBlocker bb(m_toolBrush);
    QSignalBlocker bk(m_toolBucket);
    QSignalBlocker be(m_toolEyedropper);
    m_toolBrush     ->setChecked(t == CanvasTool::Brush);
    m_toolBucket    ->setChecked(t == CanvasTool::Bucket);
    m_toolEyedropper->setChecked(t == CanvasTool::Eyedropper);
    if (m_bucketRow) m_bucketRow->setVisible(t == CanvasTool::Bucket);
}

void BrushPanel::onInkPicked(int /*channelIndex*/, uint8_t /*v*/)
{
    rebuildInkSwatch();
}

void BrushPanel::rebuildInkSwatch()
{
    if (!m_brush || !m_inkSwatch) return;

    // Pick the value of the first set channel (matches eyedropper convention)
    const auto& s = m_brush->settings();
    uint8_t v = 255;
    QColor swatchColor(255, 255, 255);
    if (s.channelMask & ChannelMaskR) {
        v = s.inkR;
        // Tint the swatch by the semantic color of this channel
        swatchColor = QColor(255, v, v);
    } else if (s.channelMask & ChannelMaskG) {
        v = s.inkG;
        swatchColor = QColor(v, 255, v);
    } else if (s.channelMask & ChannelMaskB) {
        v = s.inkB;
        swatchColor = QColor(v, v, 255);
    }

    m_inkLabel->setText(QString::number(v));
    m_inkSwatch->setStyleSheet(QString(
        "background-color: %1;"
        "border: 1px solid #555861;"
        "border-radius: 3px;"
    ).arg(swatchColor.name()));
}

} // namespace editor
