// A DC power analysis's page: its setup on the right (the net, the pins that supply it and draw
// from it, the solver's options and the limits to check), and once run, a map of the voltage or
// current density across each copper layer with the pins and vias marked, the pins' voltages, the
// vias' currents, and anything over its limit. Hover the map to read it; drag to pan, scroll to zoom,
// double-click to frame the copper. Selecting a pin, via or problem in the lists points it out.
// Before a run the map shows the rail's copper from the board; clicking one of its pads, before or
// after, makes it a source or a load, or takes it out of the analysis.
#pragma once

#include "BoardModel.hpp"
#include "PowerAnalysis.hpp"

#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QPainterPath>
#include <QWidget>

#include <memory>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTreeWidget;

namespace kiems_gui {

class Document;
class QuantityEdit;

class PowerCanvas : public QWidget {
    Q_OBJECT

public:
    enum class Quantity { Voltage, CurrentDensity };
    enum class Role { None, Source, Load, Sense };

    /// One pad of the rail, in board millimetres.
    struct PadShape {
        QString footprint;
        QString pin;
        int layer = 0;
        QPainterPath path;
    };

    explicit PowerCanvas(QWidget* parent = nullptr);

    /// The rail's copper on each layer and its pads, from the board: shown before a run, and
    /// what clicks pick pins on.
    void setPreview(std::vector<QPainterPath> copper, std::vector<PadShape> pads);
    /// The pins the analysis has, marked on the map before it's run.
    void setRoles(QHash<QString, Role> roles);
    static QString pinKey(const QString& footprint, const QString& pin) { return footprint + '\x1f' + pin; }

    void setRun(std::shared_ptr<const PowerRun> run, bool refit);
    /// Shows `layer` of the rail, or of its return.
    void setLayer(int layer, bool onReturn = false);
    void setQuantity(Quantity quantity);
    void setLogScale(bool on);
    void fit();
    /// Rings a spot (mm) on the map; nullopt clears it.
    void mark(std::optional<QPointF> point);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

signals:
    /// A click (not a drag) on one of the rail's pads.
    void padClicked(const QString& footprint, const QString& pin, const QPoint& globalPosition);

private:
    bool showingResult() const;
    QPointF toScreen(double x, double y) const;
    QPointF toBoard(const QPointF& screen) const;
    /// The value shown for one triangle, or NaN.
    double valueOf(std::size_t triangle) const;
    QColor colorOf(double value) const;
    void updateRange();
    void renderMap();
    QRectF mapArea() const;
    const kiems::dc::Result& result() const;
    const kiems::dc::Problem& problem() const;

    std::shared_ptr<const PowerRun> _run;
    int _layer = 0;
    bool _onReturn = false;
    Quantity _quantity = Quantity::Voltage;
    bool _logScale = true;
    double _low = 0, _high = 1;
    double _scale = 10; // pixels per mm
    QPointF _origin;    // the screen point of board (0, 0)
    QImage _map;
    bool _mapValid = false;
    std::optional<QPointF> _mark;
    QPoint _lastMouse;
    QPoint _pressedAt;
    /// Frames the copper again when resized, until the user zooms or pans.
    bool _autoFit = true;
    std::vector<QPainterPath> _previewCopper;
    std::vector<PadShape> _pads;
    QHash<QString, Role> _roles;
};

class PowerView : public QWidget {
    Q_OBJECT

public:
    explicit PowerView(Document* document, QWidget* parent = nullptr);

    void setBoard(std::shared_ptr<const BoardData> board);
    /// Shows the analysis at `index` in the document's list.
    void showAnalysis(int index);
    /// Runs the analysis shown, in the background.
    void run();
    bool canRun() const;
    PowerCanvas* canvas() const { return _canvas; }
    /// Makes a pin of the rail a source, a load, or neither -- what a click on its pad offers.
    void setPinRole(const QString& footprint, const QString& pin, PowerCanvas::Role role);
    /// The last run of the analysis shown, if any.
    std::shared_ptr<const PowerRun> currentRun() const;

signals:
    /// A run finished (for tests and the status bar).
    void runFinished(const QString& analysis, bool ok);

private:
    void refreshSetup();
    void refreshResults();
    void edit(const std::function<void(kiems::dc::AnalysisConfig&)>& change);
    void commitTables();
    void fillFromPinTypes();
    void updateStatus();
    /// The map's view of the board before a run: the current net's copper and pads.
    void refreshPreview();
    std::optional<kiems::dc::AnalysisConfig> current() const;

    Document* _document;
    std::shared_ptr<const BoardData> _board;
    int _index = -1;
    bool _updating = false;
    bool _running = false;
    QString _previewNet;
    /// Each analysis's last run, by name.
    QHash<QString, std::shared_ptr<const PowerRun>> _runs;

    QLabel* _title;
    QLabel* _status;
    QComboBox* _layer;
    QComboBox* _quantity;
    QCheckBox* _logScale;
    QPushButton* _runButton;
    PowerCanvas* _canvas;
    QTabWidget* _resultTabs;
    QTreeWidget* _pins;
    QTreeWidget* _vias;
    QListWidget* _problems;

    QLineEdit* _name;
    QComboBox* _net;
    QComboBox* _returnNet;
    QLineEdit* _sensePart;
    QLineEdit* _sensePin;
    QuantityEdit* _senseVolts;
    QComboBox* _loadPins;
    QTableWidget* _sources;
    QTableWidget* _loads;
    QuantityEdit* _totalCurrent;
    QuantityEdit* _temperature;
    QuantityEdit* _plating;
    QuantityEdit* _maxElement;
    QComboBox* _loadPads;
    QuantityEdit* _outerLimit;
    QuantityEdit* _innerLimit;
    QuantityEdit* _viaLimit;
};

} // namespace kiems_gui
