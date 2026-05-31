#pragma once

#include <QApplication>
#include <QPalette>
#include <QString>

namespace ui {

/// QSS stylesheet for the entire application.
/// Modeled after Substance Painter / VS Code dark themes.
inline constexpr const char* kDarkStyleSheet = R"qss(

/* ── Window / base ───────────────────────────────────────────── */
QWidget {
    background-color: #1e2027;
    color: #d6d8de;
    font-family: "Segoe UI", "Microsoft YaHei UI", sans-serif;
    font-size: 12px;
}

QMainWindow {
    background-color: #16181d;
}

QStatusBar {
    background-color: #14161b;
    color: #909298;
    border-top: 1px solid #0d0e12;
    padding: 2px 8px;
}

QStatusBar::item { border: none; }

/* ── Menus ───────────────────────────────────────────────────── */
QMenuBar {
    background-color: #14161b;
    border-bottom: 1px solid #0d0e12;
    padding: 2px 0;
}
QMenuBar::item {
    background: transparent;
    padding: 6px 12px;
    color: #c4c6cc;
}
QMenuBar::item:selected {
    background-color: #2a73d6;
    color: #ffffff;
    border-radius: 4px;
}
QMenu {
    background-color: #1f2128;
    border: 1px solid #0d0e12;
    padding: 4px;
}
QMenu::item {
    padding: 6px 24px 6px 12px;
    border-radius: 3px;
}
QMenu::item:selected {
    background-color: #2a73d6;
    color: #ffffff;
}
QMenu::separator {
    height: 1px;
    background: #2a2d36;
    margin: 4px 8px;
}

/* ── Splitter ────────────────────────────────────────────────── */
QSplitter::handle {
    background-color: #0d0e12;
}
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical   { height: 1px; }

/* ── Tabs ────────────────────────────────────────────────────── */
QTabWidget::pane {
    border: none;
    background-color: #16181d;
}
QTabBar { background: transparent; }
QTabBar::tab {
    background-color: #1e2027;
    color: #909298;
    padding: 8px 18px;
    border: none;
    border-right: 1px solid #14161b;
    min-width: 80px;
}
QTabBar::tab:selected {
    background-color: #16181d;
    color: #ffffff;
    border-top: 2px solid #4a90f0;
    padding-top: 6px;
}
QTabBar::tab:hover:!selected {
    background-color: #25272f;
    color: #d6d8de;
}

/* ── Buttons ─────────────────────────────────────────────────── */
QPushButton {
    background-color: #2a2d36;
    color: #d6d8de;
    border: 1px solid #383b46;
    border-radius: 4px;
    padding: 5px 12px;
    min-height: 18px;
}
QPushButton:hover {
    background-color: #353944;
    border-color: #4a90f0;
}
QPushButton:pressed {
    background-color: #2a73d6;
    color: #ffffff;
    border-color: #2a73d6;
}
QPushButton:checked {
    background-color: #2a73d6;
    color: #ffffff;
    border-color: #2a73d6;
}
QPushButton:disabled {
    background-color: #20222a;
    color: #555861;
    border-color: #20222a;
}

/* ── ComboBox ────────────────────────────────────────────────── */
QComboBox {
    background-color: #2a2d36;
    color: #d6d8de;
    border: 1px solid #383b46;
    border-radius: 4px;
    padding: 4px 28px 4px 8px;
    min-height: 18px;
}
QComboBox:hover { border-color: #4a90f0; }
QComboBox:on    { border-color: #4a90f0; }
QComboBox::drop-down {
    width: 22px;
    border-left: 1px solid #383b46;
    background: transparent;
}
QComboBox::down-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid #909298;
    width: 0;
    height: 0;
    margin-right: 6px;
}
QComboBox QAbstractItemView {
    background-color: #1f2128;
    color: #d6d8de;
    border: 1px solid #0d0e12;
    selection-background-color: #2a73d6;
    selection-color: #ffffff;
    outline: 0;
}

/* ── Sliders ─────────────────────────────────────────────────── */
QSlider::groove:horizontal {
    height: 4px;
    background: #2a2d36;
    border-radius: 2px;
    margin: 6px 0;
}
QSlider::sub-page:horizontal {
    background: #4a90f0;
    border-radius: 2px;
}
QSlider::add-page:horizontal {
    background: #2a2d36;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    width: 14px;
    height: 14px;
    margin: -6px 0;
    background: #d6d8de;
    border: none;
    border-radius: 7px;
}
QSlider::handle:horizontal:hover {
    background: #ffffff;
}
QSlider::handle:horizontal:pressed {
    background: #4a90f0;
}

/* ── Checkbox ────────────────────────────────────────────────── */
QCheckBox {
    spacing: 6px;
    color: #d6d8de;
}
QCheckBox::indicator {
    width: 14px;
    height: 14px;
    border: 1px solid #383b46;
    border-radius: 3px;
    background: #2a2d36;
}
QCheckBox::indicator:hover {
    border-color: #4a90f0;
}
QCheckBox::indicator:checked {
    background: #4a90f0;
    border-color: #4a90f0;
    image: none;
}
QCheckBox::indicator:checked:hover {
    background: #5aa0ff;
}

/* ── Labels ──────────────────────────────────────────────────── */
QLabel { background: transparent; }

/* ── Tooltips ────────────────────────────────────────────────── */
QToolTip {
    background-color: #14161b;
    color: #d6d8de;
    border: 1px solid #2a73d6;
    padding: 4px 8px;
    border-radius: 3px;
}

/* ── Scrollbars ──────────────────────────────────────────────── */
QScrollBar:vertical {
    background: #16181d;
    width: 10px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: #2a2d36;
    border-radius: 5px;
    min-height: 30px;
}
QScrollBar::handle:vertical:hover { background: #383b46; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal {
    background: #16181d;
    height: 10px;
    margin: 0;
}
QScrollBar::handle:horizontal {
    background: #2a2d36;
    border-radius: 5px;
    min-width: 30px;
}
QScrollBar::handle:horizontal:hover { background: #383b46; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }

/* ── Lines / inputs ──────────────────────────────────────────── */
QLineEdit, QSpinBox, QDoubleSpinBox {
    background-color: #14161b;
    color: #d6d8de;
    border: 1px solid #383b46;
    border-radius: 3px;
    padding: 4px 6px;
    selection-background-color: #2a73d6;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {
    border-color: #4a90f0;
}

)qss";

/// Apply the dark stylesheet + matching palette to a QApplication.
inline void applyDarkTheme(QApplication& app)
{
    // Native palette so non-styled widgets (file dialog, etc.) feel consistent
    QPalette p;
    p.setColor(QPalette::Window,          QColor(0x1e, 0x20, 0x27));
    p.setColor(QPalette::WindowText,      QColor(0xd6, 0xd8, 0xde));
    p.setColor(QPalette::Base,            QColor(0x14, 0x16, 0x1b));
    p.setColor(QPalette::AlternateBase,   QColor(0x1e, 0x20, 0x27));
    p.setColor(QPalette::ToolTipBase,     QColor(0x14, 0x16, 0x1b));
    p.setColor(QPalette::ToolTipText,     QColor(0xd6, 0xd8, 0xde));
    p.setColor(QPalette::Text,            QColor(0xd6, 0xd8, 0xde));
    p.setColor(QPalette::Button,          QColor(0x2a, 0x2d, 0x36));
    p.setColor(QPalette::ButtonText,      QColor(0xd6, 0xd8, 0xde));
    p.setColor(QPalette::BrightText,      QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::Highlight,       QColor(0x2a, 0x73, 0xd6));
    p.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::Link,            QColor(0x4a, 0x90, 0xf0));
    p.setColor(QPalette::Disabled, QPalette::Text,       QColor(0x55, 0x58, 0x61));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x55, 0x58, 0x61));
    app.setPalette(p);

    app.setStyleSheet(kDarkStyleSheet);
}

} // namespace ui
