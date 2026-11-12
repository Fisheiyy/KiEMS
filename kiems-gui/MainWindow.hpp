// The KiEMS document window, laid out like the macOS app's: a toolbar with the run buttons and the
// status display (the document and its board, what's running and how far along, and the failed
// tests and warnings to step through); a sidebar with Board Setup and the simulations, each row its
// board and each simulation's Geometry, Results, Field Viewer and Trace pages marked with how they
// stand; the selected page in the middle; and the inspector on the right. Log and Jobs panels
// follow running jobs.
#pragma once

#include "BoardModel.hpp"
#include "Document.hpp"
#include "EngineSettings.hpp"
#include "JobQueue.hpp"
#include "PipelineRunner.hpp"

#include <QHash>
#include <QToolButton>
#include <QSet>
#include <QTimer>
#include <QMainWindow>

#include <nlohmann/json.hpp>

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
class GeometryView;
class PowerView;
class TraceView;
class StatusDisplay;
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
    enum class Page { Board, Geometry, Results, Fields, Power, Trace, BoardSetup };

    void createActions();
    void newDocument();
    void openDocumentDialog();
    bool saveDocument();
    bool maybeSave();
    void saveReport();
    void chooseBoard();
    void loadBoard();
    void addSimulation();
    /// Adds a Simulation Set template (see SimulationSet.hpp).
    void addSimulationSet();
    /// Adds a DC power analysis of a net the user picks (see PowerAnalysis.hpp).
    void addPowerAnalysis();
    /// The power analysis whose row is selected; -1 if none is.
    int currentPowerAnalysis() const;
    void showPowerAnalysis(int index);
    /// The ground net a new simulation starts with, before Board Setup has its say.
    QString guessGroundNet() const;
    void removeSimulation();
    void runSelected();
    /// Queues the selected simulation; `storeFields` also keeps its field frames for the Field
    /// Viewer.
    void runSimulation(bool storeFields);
    /// Queues building the selected simulation's geometry alone, for the Geometry page.
    void buildGeometry();
    /// Shows the Geometry page's simulation again, with what's changed since.
    void refreshGeometryPage();
    /// Queues every simulation that has a pin to drive.
    void runAll();
    void stopRun();
    /// JobQueue's start hook: saves the document and starts the run for `job`.
    bool startJob(const Job& job, QString* error);
    void jobFinished(const Job& job);
    void updateJobList();
    void updateRunStatus();
    /// The running job's time so far and time left, in its row of the Jobs list.
    void updateRunningJobTime();
    QString timeText(const Job& job) const;
    void showEngineSettings();
    void updateEngineButton();

    /// The simulations running `simulation` queues: itself, or every simulation a set generated.
    QList<int> runTargets(int simulation) const;
    /// Regenerates, soon, every set whose template changed since it was last regenerated.
    void scheduleSetRegeneration();
    void regenerateChangedSets();
    /// Takes the open document's set templates as already regenerated.
    void resetSetSnapshots();

    void rebuildSidebar();
    /// Refreshes each page row's status mark (SidebarStatus) without rebuilding the list.
    void refreshSidebarStatus();
    /// The status mark of `simulation`'s `page`.
    int pageStatus(int simulation, Page page, double* progress) const;
    /// Whether `simulation`'s results fail their tests (cached on its eyes.json).
    bool failsTests(int simulation) const;
    /// The failed tests and warnings the status display counts, as (simulation, page) stops in
    /// sidebar order; refreshIssues() recounts them, showNextIssue() steps through them.
    QList<QPair<int, Page>> issues(bool failedTests) const;
    void refreshIssues();
    void showNextIssue(bool failedTests);
    /// What the status display says while nothing is running: the last thing that happened.
    void showStatus(const QString& message);
    void showSidebarMenu(const QPoint& at);
    /// One simulation's row and its Geometry, Results, Field Viewer and Trace pages, under `parent`
    /// (or at the top level).
    QTreeWidgetItem* addSimulationItem(QTreeWidgetItem* parent, int simulation, int selectedSimulation,
                                       int selectedPage);
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

    StatusDisplay* _statusDisplay;
    /// The status display's message while nothing is running.
    QString _lastStatus;
    /// Simulations whose last run (geometry or full) failed, for their rows' marks and the warnings.
    QSet<QString> _failedGeometry, _failedRuns;
    /// failsTests()'s cache: by results directory, eyes.json's time and whether it fails.
    mutable QHash<QString, QPair<qint64, bool>> _testVerdicts;
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
    GeometryView* _geometryView;
    PowerView* _powerView;
    TraceView* _traceView;
    Inspector* _inspector;
    QDockWidget* _logDock;
    QDockWidget* _jobsDock;
    QTreeWidget* _jobList;
    QPushButton* _cancelJobButton;
    QPushButton* _clearJobsButton;
    QPlainTextEdit* _log;
    QToolButton* _engineButton;
    /// Ticks the running job's elapsed time and countdown between log lines.
    QTimer* _runClock;
    /// How long each simulation's last full run took, in seconds: what a run of it is first
    /// expected to take, and what a queued one shows.
    QHash<QString, double> _runSeconds;
    /// How long each finished job took, by job id.
    QHash<int, double> _jobSeconds;
    /// The job the runner's clock is timing (one that failed to start never had it).
    int _timedJobId = -1;

    QAction* _saveAction;
    QAction* _runAction;
    QAction* _runFieldsAction;
    QAction* _runAllAction;
    QAction* _stopAction;
    QAction* _addAction;
    QAction* _addSetAction;
    QAction* _addPowerAction;
    QAction* _removeAction;
    bool _rebuildingSidebar = false;
    /// Whether each sidebar row with children (by simulation name, or "set:" and the set's id) was
    /// left expanded, so a rebuild keeps them as they were.
    QHash<QString, bool> _expanded;
    /// Each set template's JSON when its simulations were last generated from it, by set id.
    QHash<QString, nlohmann::json> _setSnapshots;
    bool _setRegenerationPending = false;
    /// Jobs started, and how many of them failed, since the queue was last empty: a lone run that
    /// fails says so in a dialog, but a batch carries on and leaves its failures in the Jobs list.
    int _batchStarted = 0;
    int _batchFailed = 0;
    int _batchStopped = 0;
};

} // namespace kiems_gui
