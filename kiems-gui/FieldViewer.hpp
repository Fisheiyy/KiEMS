// A simulation's Field Viewer: plays back the field frames a run stored (kiems --field-frames, or
// the macOS app's own runs) as a top-down slice through the board -- one excited port's series at
// a time, any field quantity, at any height, frame by frame or playing. The series are Copper's
// frame-series files, found through the simulation's field_frames_current.txt manifest as the
// macOS app finds them. Without stored frames it offers to run the simulation and store them.
#pragma once

#include "BoardModel.hpp"

#include <QImage>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <map>
#include <memory>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTimer;

namespace copper {
class FieldFrameSeriesReader;
}

namespace kiems_gui {

class FieldCanvas;

/// One stored series: the excited port it was run for and its file.
struct FieldSeriesFile {
    int port = 0;
    QString path;
};

class FieldViewer : public QWidget {
    Q_OBJECT

public:
    explicit FieldViewer(QWidget* parent = nullptr);
    ~FieldViewer() override;

    /// The series stored for the simulation whose directory is `simulationDirectory`.
    static std::vector<FieldSeriesFile> findSeries(const QString& simulationDirectory);

    /// Shows `title`'s fields; `portNames` labels the excited ports (from the run's ports.txt).
    void showSimulation(const QString& title, const QString& simulationDirectory, const std::map<int, QString>& portNames);
    /// Closes the open series (before a run replaces them).
    void close();
    /// The board drawn over the fields for orientation: its outline and top copper.
    void setBoard(std::shared_ptr<const BoardData> board);

signals:
    /// "Run and store fields" was clicked.
    void captureRequested();

private:
    void openSeries(int index);
    void populateHeights();
    void requestFrame();
    void frameRead(int generation, int frame, std::shared_ptr<std::vector<float>> values, QString error);
    void updateTimeLabel();

    QString _simulationDirectory;
    std::vector<FieldSeriesFile> _series;
    std::map<int, QString> _portNames;
    std::shared_ptr<copper::FieldFrameSeriesReader> _reader;

    QStackedWidget* _stack;
    QLabel* _emptyLabel;
    QPushButton* _capture;
    QLabel* _title;
    QComboBox* _port;
    QComboBox* _quantity;
    QSlider* _height;
    QLabel* _heightLabel;
    QSpinBox* _range;
    QCheckBox* _showBoard;
    FieldCanvas* _canvas;
    QPushButton* _play;
    QSlider* _time;
    QLabel* _timeLabel;
    QTimer* _playTimer;

    int _generation = 0;      ///< bumped whenever what's shown changes, so stale reads are dropped
    bool _reading = false;
    bool _readAgain = false;  ///< the wanted frame changed while a read was in flight
    int _shownFrame = -1;
};

} // namespace kiems_gui
