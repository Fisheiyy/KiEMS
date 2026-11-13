// A simulation's Trace page: signal integrity along its driven net, for tuning the trace (see
// TraceAnalysis.hpp and libkiems/kiems/trace_profile.hpp for the method).
//
// The map shows the board with the simulated nets' tracks, the driven one coloured by the impedance
// a step meets along it (blue below the target, red above) or by how much it rings once the step
// has passed. Under it: the impedance profile against distance from the driver, with the target and
// a +/-10 % band; the step response at the point picked and at the net's ports; the ringing along
// the trace; and a distance-against-time picture of the step's travel, where each reflection is a
// line leaving the spot that made it. A list of findings points out the stretches off the target
// and the worst ringing. Click the map, a chart or a finding to pick a point; drag to pan, scroll to
// zoom, double-click to frame the copper. The step's rise time sets the resolution: a faster edge
// separates features closer together, down to what the simulation's sweep can carry.
#pragma once

#include "BoardModel.hpp"
#include "PowerView.hpp"
#include "TraceAnalysis.hpp"

#include <QWidget>

#include <memory>
#include <optional>

class QChartView;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTabWidget;
class QTreeWidget;

namespace kiems_gui {

class QuantityEdit;

class TraceCanvas : public QWidget {
    Q_OBJECT

public:
    enum class Quantity { Impedance, Ringing };

    explicit TraceCanvas(QWidget* parent = nullptr);

    void setBackdrop(std::shared_ptr<const PowerCanvas::Backdrop> backdrop);
    void setShowBoard(bool on);
    void setAnalysis(std::shared_ptr<const TraceAnalysis> analysis, bool refit);
    void setQuantity(Quantity quantity);
    /// Marks the point `distanceMm` along the driven net (nullopt clears it).
    void setSelection(std::optional<double> distanceMm);
    void fit();
    /// Shown when there's nothing to draw.
    void setMessage(const QString& message);

signals:
    /// A click on the driven net: how far along it, and the trace probe nearest (-1 for none).
    void picked(double distanceMm, int probe);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QPointF toScreen(double x, double y) const;
    QPointF toBoard(const QPointF& screen) const;
    QRectF mapArea() const;
    /// The value shown at `distanceMm` along the driven net; NaN where there's none.
    double valueAt(double distanceMm) const;
    QColor colorOf(double value) const;
    /// The driven-net point nearest `board` (mm): its distance, its position and the probe nearest it.
    struct Hit {
        double distance = kiems::trace::kNaN;
        QPointF at;
        int probe = -1;
        double pixels = 1e9;
    };
    Hit hitTest(const QPointF& screen) const;

    std::shared_ptr<const PowerCanvas::Backdrop> _backdrop;
    std::shared_ptr<const TraceAnalysis> _analysis;
    bool _showBoard = true;
    Quantity _quantity = Quantity::Impedance;
    std::optional<double> _selection;
    QString _message;
    double _scale = 10;
    QPointF _origin;
    bool _autoFit = true;
    QPoint _lastMouse, _pressedAt;
    double _ringingTop = 0.3; // the colour scale's top, as a fraction
};

/// Distance along the trace (down) against time (across), coloured by how far the step response
/// there is from where it settles: the step's front, and each reflection, are lines across it.
class SpaceTimeView : public QWidget {
    Q_OBJECT

public:
    explicit SpaceTimeView(QWidget* parent = nullptr);
    void setAnalysis(std::shared_ptr<const TraceAnalysis> analysis);
    void setSelection(std::optional<double> distanceMm);

signals:
    void picked(double distanceMm, int probe);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QRectF plotArea() const;
    std::shared_ptr<const TraceAnalysis> _analysis;
    std::vector<int> _rows; // driven-net probes, by distance
    QImage _image;
    double _scale = 0.3; // the colours' full scale, as a fraction of the step
    std::optional<double> _selection;
};

class TraceView : public QWidget {
    Q_OBJECT

public:
    explicit TraceView(QWidget* parent = nullptr);

    void setBoard(std::shared_ptr<const BoardData> board);
    /// Shows `title`'s runs in `simulationDirectory`; `fMax` is the top of its sweep and
    /// `pulseSeconds` how long its excitation pulse plays (0 when unknown).
    void showSimulation(const QString& title, const QString& simulationDirectory, double fMax, double pulseSeconds = 0);
    /// The analysis shown, once it's ready.
    std::shared_ptr<const TraceAnalysis> analysis() const { return _analysis; }
    TraceCanvas* canvas() const { return _canvas; }

signals:
    /// "Run" was clicked, for a simulation without runs (or without trace probes).
    void runRequested();
    /// An analysis finished (for tests).
    void analysed(bool ok);

private:
    void analyse();
    void showAnalysis();
    void select(double distanceMm, int probe);
    void updateCharts();

    std::shared_ptr<const BoardData> _board;
    std::shared_ptr<const PowerCanvas::Backdrop> _backdrop;
    QString _title, _simulationDirectory;
    double _fMax = 0, _pulseSeconds = 0;
    std::shared_ptr<const TraceAnalysis> _analysis;
    int _generation = 0;
    std::optional<double> _selection;
    int _selectedProbe = -1;

    QLabel* _heading;
    QLabel* _status;
    QComboBox* _driver;
    QComboBox* _quantity;
    QuantityEdit* _riseTime;
    QCheckBox* _otherNets;
    QPushButton* _runButton;
    TraceCanvas* _canvas;
    QTabWidget* _tabs;
    QChartView* _impedanceChart;
    QChartView* _stepChart;
    QChartView* _ringingChart;
    SpaceTimeView* _spaceTime;
    QTreeWidget* _findings;
};

} // namespace kiems_gui
