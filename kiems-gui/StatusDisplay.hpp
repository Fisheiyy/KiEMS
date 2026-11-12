// The Xcode-style status display centred in the main window's toolbar, as the macOS app has it
// (ToolbarStatusView.swift): the open document and its KiCad board (with a button to choose
// another), what the app is doing right now -- a spinner while it's busy and a thin progress line
// along the bottom edge -- and how many simulations failed their tests or have warnings, each
// count a button that steps through them. MainWindow owns every bit of state shown here.
#pragma once

#include <QWidget>

#include <optional>

class QLabel;
class QToolButton;

namespace kiems_gui {

/// A label that elides its text to fit, rather than demanding the room for all of it.
class ElidedLabel : public QWidget {
    Q_OBJECT

public:
    explicit ElidedLabel(QWidget* parent = nullptr);
    void setText(const QString& text);
    QString text() const { return _text; }
    void setElideMode(Qt::TextElideMode mode) { _mode = mode; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString _text;
    Qt::TextElideMode _mode = Qt::ElideRight;
};

/// A small spinning arc: busy, with no measure of how far along.
class Spinner : public QWidget {
    Q_OBJECT

public:
    explicit Spinner(QWidget* parent = nullptr);
    void setSpinning(bool on);

protected:
    void paintEvent(QPaintEvent* event) override;
    void timerEvent(QTimerEvent* event) override;

private:
    int _timer = 0;
    int _angle = 0;
};

class StatusDisplay : public QWidget {
    Q_OBJECT

public:
    explicit StatusDisplay(QWidget* parent = nullptr);

    /// The open document's name and its board's path; empty for none.
    void setProject(const QString& documentName, const QString& boardPath);
    /// `busy` spins the indicator; `fraction` (0..1) fills the progress line, hidden when nullopt.
    void setActivity(const QString& text, bool busy, std::optional<double> fraction);
    /// Each count is hidden at zero.
    void setIssueCounts(int failedTests, int warnings);

    QString activityText() const;
    QSize sizeHint() const override;

signals:
    void chooseBoardRequested();
    void showFailedTests();
    void showWarnings();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QToolButton* _folder;
    ElidedLabel* _project;
    QWidget* _divider;
    Spinner* _spinner;
    ElidedLabel* _activity;
    QToolButton* _failed;
    QToolButton* _warnings;
    std::optional<double> _fraction;
};

} // namespace kiems_gui
