#pragma once

#include "TextureDocument.h"

#include <QWidget>

class QLabel;

namespace editor {

/// Compact info card showing semantic documentation for the active control
/// texture. Sits above the canvas. Updates when the active TextureKind changes.
class InfoPanel : public QWidget
{
    Q_OBJECT

public:
    explicit InfoPanel(QWidget* parent = nullptr);

    void setKind(TextureKind kind);

private:
    void rebuild();

    TextureKind m_kind = TextureKind::ShadowRate;

    QLabel* m_title    = nullptr;
    QLabel* m_filename = nullptr;
    QLabel* m_tagline  = nullptr;

    // Each channel row: [swatch] [letter] [label] — populated with QLabels.
    // We rebuild on setKind().
    QWidget* m_channelsHost = nullptr;

    QLabel* m_tip = nullptr;
};

} // namespace editor
