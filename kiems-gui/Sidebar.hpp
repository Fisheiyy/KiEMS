// How the main window's sidebar draws its rows, as the macOS source list does: each simulation's
// pages (Geometry, Simulation Results, Field Viewer, Trace) carry a status mark at the row's right
// -- a blue ring before they've run, a green dot after, a red stop sign when the results fail their
// tests, a yellow triangle when the run failed, and a filling pie while it runs -- and thin
// separator rows divide Board Setup, the simulations and the power analyses.
#pragma once

#include <QStyledItemDelegate>

namespace kiems_gui {

/// A row's status mark (sidebarStatusRole).
enum class SidebarStatus { None = 0, NotRun, Done, TestsFailed, Failed, Running, Queued };

constexpr int kSidebarStatusRole = Qt::UserRole + 10;   // SidebarStatus, as int
constexpr int kSidebarProgressRole = Qt::UserRole + 11; // double in [0, 1]; negative for no measure
constexpr int kSidebarSeparatorRole = Qt::UserRole + 12; // bool: a divider, not a row

class SidebarDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    /// Draws `status` in `rect` (a square), filled to `progress` while running; `onSelection` for a
    /// selected row, whose accent background the blue ring would vanish into.
    static void paintStatus(QPainter* painter, const QRectF& rect, SidebarStatus status, double progress,
                            const QColor& accent, bool onSelection = false);
};

} // namespace kiems_gui
