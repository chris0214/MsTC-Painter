#include "InfoPanel.h"

#include "TextureInfo.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace editor {

namespace {

/// A single colored "chip" showing one channel's letter and short label.
QWidget* makeChannelChip(const ChannelInfo& ch, QWidget* parent)
{
    auto* row = new QFrame(parent);
    row->setObjectName("channelChip");
    row->setProperty("class", "chip");
    row->setStyleSheet(QString(R"(
        QFrame#channelChip {
            background-color: #1a1c22;
            border: 1px solid #2a2d36;
            border-radius: 6px;
            padding: 0;
        }
    )"));

    auto* hl = new QHBoxLayout(row);
    hl->setContentsMargins(8, 6, 8, 6);
    hl->setSpacing(8);

    // Color swatch (square)
    auto* swatch = new QLabel(row);
    swatch->setFixedSize(14, 14);
    QString hex = ch.chipColor.name();
    swatch->setStyleSheet(QString(
        "background-color: %1;"
        "border-radius: 3px;"
        "border: 1px solid rgba(0,0,0,0.4);"
    ).arg(hex));
    hl->addWidget(swatch);

    // Channel letter
    auto* letter = new QLabel(QString(ch.letter), row);
    letter->setStyleSheet(QString(
        "color: %1;"
        "font-weight: 700;"
        "font-size: 13px;"
    ).arg(ch.used ? hex : "#555861"));
    letter->setMinimumWidth(12);
    hl->addWidget(letter);

    // Short label
    auto* label = new QLabel(ch.shortLabel, row);
    label->setStyleSheet(QString(
        "color: %1;"
        "font-size: 12px;"
    ).arg(ch.used ? "#d6d8de" : "#666873"));
    hl->addWidget(label, 1);

    // Tooltip with the full description
    QString tip = QString("<b style='color:%1'>%2 — %3</b><br><br>%4")
                      .arg(hex)
                      .arg(ch.letter)
                      .arg(ch.shortLabel)
                      .arg(ch.description);
    row->setToolTip(tip);

    return row;
}

} // namespace

// ─────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────

InfoPanel::InfoPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("infoPanel");
    setStyleSheet(R"(
        QWidget#infoPanel {
            background-color: #1a1c22;
            border-bottom: 1px solid #0d0e12;
        }
    )");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(8);

    // ── Header: title + filename badge ──────────────────────────
    {
        auto* row = new QWidget(this);
        auto* hl  = new QHBoxLayout(row);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(10);

        m_title = new QLabel(row);
        m_title->setStyleSheet(
            "color: #ffffff;"
            "font-size: 15px;"
            "font-weight: 600;"
        );
        hl->addWidget(m_title);

        m_filename = new QLabel(row);
        m_filename->setStyleSheet(
            "background-color: #2a2d36;"
            "color: #909298;"
            "font-family: Consolas, 'Courier New', monospace;"
            "font-size: 11px;"
            "padding: 2px 8px;"
            "border-radius: 3px;"
        );
        hl->addWidget(m_filename);

        hl->addStretch();
        root->addWidget(row);
    }

    // ── Tagline ─────────────────────────────────────────────────
    m_tagline = new QLabel(this);
    m_tagline->setWordWrap(true);
    m_tagline->setStyleSheet("color: #b8bac0; font-size: 12px; line-height: 1.4;");
    root->addWidget(m_tagline);

    // ── Channel chips host (rebuilt per kind) ───────────────────
    m_channelsHost = new QWidget(this);
    auto* chHl = new QHBoxLayout(m_channelsHost);
    chHl->setContentsMargins(0, 2, 0, 2);
    chHl->setSpacing(6);
    root->addWidget(m_channelsHost);

    // ── Artist tip footer ───────────────────────────────────────
    m_tip = new QLabel(this);
    m_tip->setWordWrap(true);
    m_tip->setStyleSheet(
        "color: #909298;"
        "font-size: 11px;"
        "background-color: #14161b;"
        "border-left: 3px solid #4a90f0;"
        "padding: 6px 10px;"
        "border-radius: 2px;"
    );
    root->addWidget(m_tip);

    rebuild();
}

// ─────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────

void InfoPanel::setKind(TextureKind k)
{
    if (m_kind == k) return;
    m_kind = k;
    rebuild();
}

void InfoPanel::rebuild()
{
    const TextureInfo& info = textureInfo(m_kind);

    m_title->setText(info.title);
    m_filename->setText(info.filename);
    m_tagline->setText(info.tagline);
    m_tip->setText(QString("\xF0\x9F\x92\xA1  ") + info.artistTip);  // 💡 emoji prefix

    // Rebuild channel chips
    QLayout* lay = m_channelsHost->layout();
    while (QLayoutItem* item = lay->takeAt(0)) {
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }
    auto* hl = static_cast<QHBoxLayout*>(lay);
    for (size_t i = 0; i < 3; ++i) {  // R, G, B (skip A — never relevant)
        hl->addWidget(makeChannelChip(info.channels[i], m_channelsHost), 1);
    }
}

} // namespace editor
