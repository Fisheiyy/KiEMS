// The KiEMS document window, laid out like the macOS app's: the linked board across the top,
// simulations in a sidebar (each with its Board and Results pages), the selected page in the
// middle, and the inspector on the right. A log panel and the status bar follow running jobs.
#pragma once

#include "BoardModel.hpp"
#include "Document.hpp"
#include "EngineSettings.hpp"
#include "JobQueue.hpp"
#include "PipelineRunner.hpp"

#include <QMainWindow>

class QAction;
class QDockWidget;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace kiems_gui {

class BoardView;
class FieldViewer;
class Inspector;
class ResultsView;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    bool openDocument(const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    enum class Page { Board, Results, Fields };

    void createActions();
    void newDocument();
    void openDocumentDialog();
    bool saveDocument();
    bool maybeSave();
    void saveReport();
    void chooseBoard();
    void loadBoard();
    void addSimulation();
    void removeSimulation();
    void runSelected();
    /// Queues the selected simulation; `storeFields` also keeps its field frames for the Field
    /// Viewer.
    void runSimulation(bool storeFields);
    /// Queues every simulation that has a pin to drive.
    void runAll();
    void stopRun();
    /// JobQueue's start hook: saves the document and starts the run for `job`.
    bool startJob(const Job& job, QString* error);
    void jobFinished(const Job& job);
    void updateJobList();
    void updateRunStatus();
    void showEngineSettings();
    void updateEngineButton();

    void rebuildSidebar();
    void selectionChanged();
    void showPage(int simulation, Page page);
    void updateHighlights();
    void updateTitle();
    void updateActions();
    void populateLayerList();
    int currentSimulation() const;

    Document _document;
    BoardModel _boardModel;
    PipelineRunner _runner;
    JobQueue _jobs;
    GpuDeviceList _gpuDevices;
    EngineSettings _engine;

    QLabel* _projectLabel;
    QPushButton* _chooseBoardButton;
    QTreeWidget* _sidebar;
    QPushButton* _addButton;
    QPushButton* _removeButton;
    QStackedWidget* _pages;
    QWidget* _emptyPage;
    QWidget* _boardPage;
    QListWidget* _layerList;
    BoardView* _boardView;
    ResultsView* _resultsView;
    FieldViewer* _fieldViewer;
    Inspector* _inspector;
    QDockWidget* _logDock;
    QDockWidget* _jobsDock;
    QTreeWidget* _jobList;
    QPushButton* _cancelJobButton;
    QPushButton* _clearJobsButton;
    QPlainTextEdit* _log;
    QLabel* _statusLabel;
    QProgressBar* _progress;
    QPushButton* _stopButton;
    QPushButton* _engineButton;

    QAction* _saveAction;
    QAction* _runAction;
    QAction* _runFieldsAction;
    QAction* _runAllAction;
    QAction* _stopAction;
    QAction* _addAction;
    QAction* _removeAction;
    bool _rebuildingSidebar = false;
    /// Jobs started, and how many of them failed, since the queue was last empty: a lone run that
    /// fails says so in a dialog, but a batch carries on and leaves its failures in the Jobs list.
    int _batchStarted = 0;
    int _batchFailed = 0;
    int _batchStopped = 0;
};

} // namespace kiems_gui
