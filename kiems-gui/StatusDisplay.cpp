#include "StatusDisplay.hpp"

#include "Icons.hpp"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QTimerEvent>
#include <QToolButton>

#include <algorithm>
#include <cmath>

namespace kiems_gui {

namespace {

constexpr int kHeight = 30;

/// A count with its glyph -- a red stop sign for failed tests, a yellow triangle for warnings.
QIcon issueIcon(bool failed) {
    QIcon icon;
    for (int scale : {1, 2}) {
        QPixmap pixmap(14 * scale, 14 * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        if (failed) {
            p.setBrush(QColor(0xff, 0x45, 0x3a));
            QPolygonF octagon;
            for (int k = 0; k < 8; ++k) {
                const double a = (k + 0.5) * 3.14159265358979 / 4;
                octagon << QPointF(7 + 6.2 * std::cos(a), 7 + 6.2 * std::sin(a));
            }
            p.drawPolygon(octagon);
        } else {
            p.setBrush(QColor(0xff, 0xcc, 0x00));
            const QPointF tri[] = {{7, 1}, {13.5, 12.5}, {0.5, 12.5}};
            p.drawPolygon(tri, 3);
        }
        p.setBrush(QColor(30, 30, 30));
        p.drawRoundedRect(QRectF(6.25, failed ? 3.5 : 4.5, 1.5, failed ? 5 : 4.5), 0.7, 0.7);
        p.drawEllipse(QPointF(7, failed ? 10.3 : 10.6), 0.9, 0.9);
        p.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

} // namespace

ElidedLabel::ElidedLabel(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void ElidedLabel::setText(const QString& text) {
    if (text == _text) return;
    _text = text;
    setToolTip(text);
    updateGeometry();
    update();
}

QSize ElidedLabel::sizeHint() const { return {fontMetrics().horizontalAdvance(_text) + 2, fontMetrics().height()}; }

QSize ElidedLabel::minimumSizeHint() const { return {std::min(40, sizeHint().width()), fontMetrics().height()}; }

void ElidedLabel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::WindowText));
    painter.drawText(rect(), Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(_text, _mode, width()));
}

Spinner::Spinner(QWidget* parent) : QWidget(parent) {
    setFixedSize(14, 14);
    hide();
}

void Spinner::setSpinning(bool on) {
    setVisible(on);
    if (on && !_timer) _timer = startTimer(40);
    if (!on && _timer) {
        killTimer(_timer);
        _timer = 0;
    }
}

void Spinner::timerEvent(QTimerEvent* event) {
    if (event->timerId() != _timer) return;
    _angle = (_angle + 24) % 360;
    update();
}

void Spinner::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF ring = QRectF(rect()).adjusted(2, 2, -2, -2);
    QColor faint = palette().color(QPalette::WindowText);
    faint.setAlphaF(0.2);
    painter.setPen(QPen(faint, 2));
    painter.drawEllipse(ring);
    painter.setPen(QPen(palette().color(QPalette::Highlight), 2, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(ring, -_angle * 16, 100 * 16);
}

StatusDisplay::StatusDisplay(QWidget* parent) : QWidget(parent) {
    setObjectName("statusDisplay");
    setFixedHeight(kHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumWidth(320);
    setMaximumWidth(760);
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.95);
    setFont(small);

    _folder = new QToolButton;
    _folder->setAutoRaise(true);
    QColor folder = palette().color(QPalette::WindowText);
    folder.setAlphaF(0.6);
    _folder->setIcon(icons::icon(icons::Glyph::Folder, folder, 14));
    _folder->setIconSize(QSize(14, 14));
    _folder->setToolTip(tr("Choose the KiCad board this document simulates"));
    _project = new ElidedLabel;
    _project->setElideMode(Qt::ElideMiddle);
    _divider = new QWidget;
    _divider->setFixedSize(1, 16);
    _divider->setAutoFillBackground(true);
    QPalette dividerPalette = _divider->palette();
    dividerPalette.setColor(QPalette::Window, palette().color(QPalette::Mid));
    _divider->setPalette(dividerPalette);
    _spinner = new Spinner;
    _activity = new ElidedLabel;
    _activity->setObjectName("statusActivity");
    {
        QPalette dim = _activity->palette();
        QColor text = dim.color(QPalette::WindowText);
        text.setAlphaF(0.72);
        dim.setColor(QPalette::WindowText, text);
        _activity->setPalette(dim);
    }
    _failed = new QToolButton;
    _warnings = new QToolButton;
    _failed->setObjectName("failedTests");
    _warnings->setObjectName("warnings");
    for (auto [button, failed] : {std::pair(_failed, true), std::pair(_warnings, false)}) {
        button->setAutoRaise(true);
        button->setIcon(issueIcon(failed));
        button->setIconSize(QSize(13, 13));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->hide();
    }

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 0, 12, 2);
    layout->setSpacing(6);
    layout->addWidget(_folder);
    layout->addWidget(_project, 2);
    layout->addSpacing(4);
    layout->addWidget(_divider);
    layout->addSpacing(4);
    layout->addWidget(_spinner);
    layout->addWidget(_activity, 3);
    layout->addWidget(_warnings);
    layout->addWidget(_failed);

    connect(_folder, &QToolButton::clicked, this, &StatusDisplay::chooseBoardRequested);
    connect(_failed, &QToolButton::clicked, this, &StatusDisplay::showFailedTests);
    connect(_warnings, &QToolButton::clicked, this, &StatusDisplay::showWarnings);
    setProject({}, {});
}

QSize StatusDisplay::sizeHint() const { return {620, kHeight}; }

void StatusDisplay::setProject(const QString& documentName, const QString& boardPath) {
    if (documentName.isEmpty()) {
        _project->setText(tr("No document open"));
        _folder->setEnabled(false);
        return;
    }
    _folder->setEnabled(true);
    // Breadcrumbs, as the macOS path control shows them: the document, then its board.
    const QString board = boardPath.isEmpty() ? tr("no KiCad board") : QFileInfo(boardPath).fileName();
    _project->setText(QString("%1  ›  %2").arg(QFileInfo(documentName).completeBaseName(), board));
    _project->setToolTip(boardPath.isEmpty() ? documentName : QDir::toNativeSeparators(boardPath));
}

void StatusDisplay::setActivity(const QString& text, bool busy, std::optional<double> fraction) {
    _activity->setText(text);
    _spinner->setSpinning(busy);
    _divider->setVisible(!text.isEmpty() || busy);
    if (fraction) fraction = std::clamp(*fraction, 0.0, 1.0);
    if (fraction != _fraction) {
        _fraction = fraction;
        update();
    }
}

QString StatusDisplay::activityText() const { return _activity->text(); }

void StatusDisplay::setIssueCounts(int failedTests, int warnings) {
    _failed->setText(QString::number(failedTests));
    _failed->setVisible(failedTests > 0);
    _failed->setToolTip(tr("%n simulation(s) failed their tests. Click to show the next.", nullptr, failedTests));
    _warnings->setText(QString::number(warnings));
    _warnings->setVisible(warnings > 0);
    _warnings->setToolTip(tr("%n warning(s): runs that failed. Click to show the next.", nullptr, warnings));
}

void StatusDisplay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF pill = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const double radius = pill.height() / 2;
    // A shade off the window, as the macOS display's glass sits off the toolbar.
    QColor fill = palette().color(QPalette::Window);
    fill = fill.lightness() < 128 ? fill.lighter(135) : fill.darker(106);
    QColor edge = palette().color(QPalette::Mid);
    edge.setAlphaF(0.6);
    painter.setPen(QPen(edge, 1));
    painter.setBrush(fill);
    painter.drawRoundedRect(pill, radius, radius);
    if (_fraction) {
        // Along the bottom edge, clear of the rounded ends, like Xcode's build progress.
        const QRectF track(pill.left() + radius, pill.bottom() - 3, pill.width() - 2 * radius, 2);
        QColor faint = palette().color(QPalette::WindowText);
        faint.setAlphaF(0.12);
        painter.setPen(Qt::NoPen);
        painter.setBrush(faint);
        painter.drawRoundedRect(track, 1, 1);
        painter.setBrush(palette().color(QPalette::Highlight));
        painter.drawRoundedRect(QRectF(track.left(), track.top(), track.width() * *_fraction, track.height()), 1, 1);
    }
}

} // namespace kiems_gui
