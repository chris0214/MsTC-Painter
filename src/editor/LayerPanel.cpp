#include "LayerPanel.h"

#include "LayerStack.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace editor {

namespace {
constexpr int kRowHeight = 22;
} // anonymous

LayerPanel::LayerPanel(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(3);

    m_titleLabel = new QLabel(tr("Layers"), this);
    QFont f = m_titleLabel->font();
    f.setBold(true);
    m_titleLabel->setFont(f);
    root->addWidget(m_titleLabel);

    // Toolbar row 1: Add buttons
    auto* addRow = new QHBoxLayout;
    addRow->setContentsMargins(0, 0, 0, 0);
    addRow->setSpacing(2);
    m_addPaintBtn   = new QPushButton(tr("+ Paint"), this);
    m_addFillBtn    = new QPushButton(tr("+ Fill"),  this);
    m_addImageBtn   = new QPushButton(tr("+ Img"),   this);
    m_addChannelBtn = new QPushButton(tr("+ Ch"),    this);
    for (auto* b : { m_addPaintBtn, m_addFillBtn, m_addImageBtn,
                     m_addChannelBtn }) {
        b->setMinimumWidth(0);
        addRow->addWidget(b);
    }
    addRow->addStretch();
    root->addLayout(addRow);

    // Toolbar row 2: Reorder / Delete / Merge
    auto* opRow = new QHBoxLayout;
    opRow->setContentsMargins(0, 0, 0, 0);
    opRow->setSpacing(2);
    m_upBtn     = new QPushButton("↑", this);
    m_downBtn   = new QPushButton("↓", this);
    m_deleteBtn = new QPushButton("✕", this);
    m_mergeBtn  = new QPushButton(tr("Merge ↓"), this);
    for (auto* b : { m_upBtn, m_downBtn, m_deleteBtn, m_mergeBtn }) {
        b->setFixedHeight(kRowHeight);
        opRow->addWidget(b);
    }
    opRow->addStretch();
    root->addLayout(opRow);

    // List of layers
    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    // No InternalMove drag-drop yet — buttons handle it. This avoids the
    // checkbox-toggle-on-drag-pickup quirk QListWidget has.
    root->addWidget(m_list, /*stretch*/ 1);

    // Opacity slider for the selected layer
    auto* opacRow = new QHBoxLayout;
    opacRow->setContentsMargins(0, 0, 0, 0);
    opacRow->setSpacing(4);
    auto* opacLbl = new QLabel(tr("Opacity:"), this);
    m_opacitySlider = new QSlider(Qt::Horizontal, this);
    m_opacitySlider->setRange(0, 100);
    m_opacitySlider->setValue(100);
    m_opacityLabel  = new QLabel("100%", this);
    m_opacityLabel->setMinimumWidth(36);
    opacRow->addWidget(opacLbl);
    opacRow->addWidget(m_opacitySlider, 1);
    opacRow->addWidget(m_opacityLabel);
    root->addLayout(opacRow);

    // ── Wiring ──
    connect(m_addPaintBtn, &QPushButton::clicked, this, &LayerPanel::onAddPaintLayer);
    connect(m_addFillBtn,    &QPushButton::clicked, this, &LayerPanel::addFillRequested);
    connect(m_addImageBtn,   &QPushButton::clicked, this, &LayerPanel::addImageRequested);
    connect(m_addChannelBtn, &QPushButton::clicked, this, &LayerPanel::addChannelRequested);
    connect(m_upBtn,       &QPushButton::clicked, this, &LayerPanel::onMoveUp);
    connect(m_downBtn,     &QPushButton::clicked, this, &LayerPanel::onMoveDown);
    connect(m_deleteBtn,   &QPushButton::clicked, this, &LayerPanel::onDeleteSelected);
    connect(m_mergeBtn,    &QPushButton::clicked, this, &LayerPanel::onMergeDown);
    connect(m_list,        &QListWidget::currentRowChanged, this,
            [this](int) { onSelectionChanged(); });
    connect(m_list, &QListWidget::itemChanged, this, &LayerPanel::onItemChanged);
    connect(m_opacitySlider, &QSlider::valueChanged, this, &LayerPanel::onOpacitySliderChanged);

    setMinimumHeight(140);
    setStack(nullptr);
}

void LayerPanel::setStack(LayerStack* stack)
{
    m_stack = stack;
    rebuild();
}

void LayerPanel::setKindLabel(const QString& kindName)
{
    m_titleLabel->setText(tr("Layers — %1").arg(kindName));
}

// ─────────────────────────────────────────────────────────────────
// List rebuild
// ─────────────────────────────────────────────────────────────────

void LayerPanel::rebuild()
{
    m_suppressSignals = true;
    m_list->clear();

    const bool en = (m_stack != nullptr);
    m_addPaintBtn  ->setEnabled(en);
    m_addFillBtn   ->setEnabled(en);
    m_addImageBtn  ->setEnabled(en);
    m_addChannelBtn->setEnabled(en);
    m_upBtn      ->setEnabled(en);
    m_downBtn    ->setEnabled(en);
    m_deleteBtn  ->setEnabled(en);
    m_mergeBtn   ->setEnabled(en);
    m_opacitySlider->setEnabled(en);

    if (!m_stack) {
        m_titleLabel->setText(tr("Layers"));
        m_suppressSignals = false;
        return;
    }

    // Top-down list ordering matches Photoshop: [0] in widget = topmost layer.
    // Internally LayerStack[0] is the BOTTOM, so we walk indices in reverse.
    const int N = m_stack->layerCount();
    for (int li = N - 1; li >= 0; --li) {
        const Layer* L = m_stack->layer(li);
        auto* it = new QListWidgetItem(QString::fromStdString(L->name));
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
        it->setCheckState(L->visible ? Qt::Checked : Qt::Unchecked);
        // Stash the LayerStack-internal index in user data for slot dispatch.
        it->setData(Qt::UserRole, li);
        m_list->addItem(it);
    }
    // Select the row corresponding to activeIndex.
    syncRowToActive();
    m_suppressSignals = false;
}

void LayerPanel::syncRowToActive()
{
    if (!m_stack) return;
    // Active layer's stack index → row in widget (which is N-1-li).
    const int N    = m_stack->layerCount();
    const int li   = m_stack->activeIndex();
    const int row  = (N - 1) - li;
    if (row >= 0 && row < N) {
        m_list->setCurrentRow(row);
    }
    // Also push opacity to slider.
    if (auto* L = m_stack->activeLayer()) {
        const int v = static_cast<int>(L->opacity * 100.0f + 0.5f);
        m_opacitySlider->setValue(v);
        m_opacityLabel->setText(QString::number(v) + "%");
    }
}

int LayerPanel::selectedIndex() const
{
    if (!m_stack) return -1;
    auto* it = m_list->currentItem();
    if (!it) return -1;
    return it->data(Qt::UserRole).toInt();
}

// ─────────────────────────────────────────────────────────────────
// Slots
// ─────────────────────────────────────────────────────────────────

void LayerPanel::onAddPaintLayer()
{
    if (!m_stack) return;
    auto layer = std::make_unique<PaintLayer>(m_stack->size());
    layer->name = "Layer " + std::to_string(m_stack->layerCount() + 1);
    m_stack->addLayer(std::move(layer));   // recomposites internally
    rebuild();
    emit layerStackChanged();
    emit paintLayerAdded();
}

int LayerPanel::pushLayer(std::unique_ptr<Layer> l)
{
    if (!m_stack || !l) return -1;
    const int idx = m_stack->addLayer(std::move(l));
    rebuild();
    emit layerStackChanged();
    return idx;
}

void LayerPanel::onMoveUp()
{
    if (!m_stack) return;
    const int li = selectedIndex();
    if (li < 0 || li >= m_stack->layerCount() - 1) return;
    m_stack->moveLayer(li, li + 1);
    rebuild();
    emit layerStackChanged();
}

void LayerPanel::onMoveDown()
{
    if (!m_stack) return;
    const int li = selectedIndex();
    if (li <= 0) return;
    m_stack->moveLayer(li, li - 1);
    rebuild();
    emit layerStackChanged();
}

void LayerPanel::onDeleteSelected()
{
    if (!m_stack) return;
    const int li = selectedIndex();
    if (li < 0) return;
    if (!m_stack->removeLayer(li)) return;   // refuses last layer
    rebuild();
    emit layerStackChanged();
}

void LayerPanel::onMergeDown()
{
    if (!m_stack) return;
    const int li = selectedIndex();
    // Need at least one layer below to merge into.
    if (li <= 0) return;
    if (!m_stack->mergeDown(li)) return;
    rebuild();
    emit layerStackChanged();
}

void LayerPanel::onSelectionChanged()
{
    if (m_suppressSignals || !m_stack) return;
    const int li = selectedIndex();
    if (li < 0) return;
    m_stack->setActiveIndex(li);
    if (auto* L = m_stack->activeLayer()) {
        const int v = static_cast<int>(L->opacity * 100.0f + 0.5f);
        m_suppressSignals = true;
        m_opacitySlider->setValue(v);
        m_opacityLabel->setText(QString::number(v) + "%");
        m_suppressSignals = false;
    }
    emit layerStackChanged();
}

void LayerPanel::onItemChanged(QListWidgetItem* it)
{
    if (m_suppressSignals || !m_stack || !it) return;
    const int li = it->data(Qt::UserRole).toInt();
    auto* L = m_stack->layer(li);
    if (!L) return;
    const bool wantVisible = (it->checkState() == Qt::Checked);
    const std::string newName = it->text().toStdString();
    bool changed = false;
    if (L->visible != wantVisible) {
        L->visible = wantVisible;
        m_stack->recompositeAll();
        changed = true;
    }
    if (L->name != newName) {
        L->name = newName;
        changed = true;
    }
    if (changed) emit layerStackChanged();
}

void LayerPanel::onOpacitySliderChanged(int v)
{
    if (m_suppressSignals || !m_stack) return;
    auto* L = m_stack->activeLayer();
    if (!L) return;
    const float nf = std::clamp(v / 100.0f, 0.0f, 1.0f);
    if (std::abs(L->opacity - nf) < 1e-4f) {
        m_opacityLabel->setText(QString::number(v) + "%");
        return;
    }
    L->opacity = nf;
    m_stack->recompositeAll();
    m_opacityLabel->setText(QString::number(v) + "%");
    emit layerStackChanged();
}

// ─────────────────────────────────────────────────────────────────

} // namespace editor
