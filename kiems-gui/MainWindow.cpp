#include "MainWindow.hpp"

#include "BoardView.hpp"
#include "FieldViewer.hpp"
#include "GeometryView.hpp"
#include "PowerAnalysis.hpp"
#include "PowerView.hpp"
#include "Inspector.hpp"
#include "Report.hpp"
#include "ResultsView.hpp"
#include "SimulationEditor.hpp"
#include "SimulationSet.hpp"
#include "TraceView.hpp"
#include "EyeView.hpp"
#include "Icons.hpp"
#include "Sidebar.hpp"
#include "StatusDisplay.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>

namespace kiems_gui {

namespace {

constexpr int kSimulationRole = Qt::UserRole;
constexpr int kPageRole = Qt::UserRole + 1;
/// A power analysis row's index in the document's list; absent on every other row.
constexpr int kPowerRole = Qt::UserRole + 3;

/// The key a sidebar row's expansion is remembered by.
QString expansionKey(const QTreeWidgetItem* item) {
    return item->data(0, Qt::UserRole + 2).toString();
}

QIcon colorSwatch(const QColor& color) {
    QPixmap pixmap(12, 12);
    pixmap.fill(color.isValid() ? QColor(color.red(), color.green(), color.blue()) : Qt::gray);
    return QIcon(pixmap);
}

} // namespace

MainWindow::MainWindow() {
    setWindowIcon(QIcon());
    resize(1500, 950);

    _statusDisplay = new StatusDisplay;

    // Sidebar.
    auto* sidebarPanel = new QWidget;
    auto* sidebarLayout = new QVBoxLayout(sidebarPanel);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    _sidebar = new QTreeWidget;
    _sidebar->setObjectName("sidebar");
    _sidebar->setHeaderHidden(true);
    _sidebar->setColumnCount(1);
    _sidebar->setMinimumWidth(230);
    _sidebar->setIconSize(QSize(16, 16));
    _sidebar->setIndentation(14);
    _sidebar->setFrameShape(QFrame::NoFrame);
    _sidebar->setItemDelegate(new SidebarDelegate(_sidebar));
    _sidebar->setContextMenuPolicy(Qt::CustomContextMenu);
    _addButton = new QPushButton("+");
    _removeButton = new QPushButton(QString::fromUtf8("\xE2\x88\x92"));
    _addButton->setToolTip(tr("Add a simulation, a Simulation Set or a power analysis"));
    _removeButton->setToolTip(tr("Remove the selected simulation"));
    for (QPushButton* button : {_addButton, _removeButton}) {
        button->setFlat(true);
        button->setFixedSize(28, 24);
        QFont glyph = button->font();
        glyph.setPointSizeF(glyph.pointSizeF() * 1.35);
        button->setFont(glyph);
    }
    auto* sidebarButtons = new QHBoxLayout;
    sidebarButtons->setContentsMargins(6, 2, 6, 4);
    sidebarButtons->setSpacing(0);
    sidebarButtons->addWidget(_addButton);
    sidebarButtons->addWidget(_removeButton);
    sidebarButtons->addStretch();
    sidebarLayout->addWidget(_sidebar, 1);
    sidebarLayout->addLayout(sidebarButtons);

    // Pages.
    _pages = new QStackedWidget;
    {
        auto* empty = new QLabel;
        empty->setObjectName("emptyPage");
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(true);
        QFont big = empty->font();
        big.setPointSizeF(big.pointSizeF() * 2);
        big.setWeight(QFont::Medium);
        empty->setFont(big);
        QPalette faint = empty->palette();
        QColor text = faint.color(QPalette::WindowText);
        text.setAlphaF(0.3);
        faint.setColor(QPalette::WindowText, text);
        empty->setPalette(faint);
        _emptyPage = empty;
    }
    // The layer list floats over the board's top-left corner with nothing behind it, as on macOS.
    _boardPage = new QWidget;
    auto* boardLayout = new QGridLayout(_boardPage);
    boardLayout->setContentsMargins(0, 0, 0, 0);
    _boardView = new BoardView;
    _layerList = new QListWidget;
    _layerList->setFrameShape(QFrame::NoFrame);
    _layerList->setAutoFillBackground(false);
    _layerList->viewport()->setAutoFillBackground(false);
    {
        // Transparent through the palette rather than a style sheet, which would also strip the
        // style's checkbox frames.
        QPalette transparent = _layerList->palette();
        transparent.setColor(QPalette::Base, Qt::transparent);
        transparent.setColor(QPalette::AlternateBase, Qt::transparent);
        _layerList->setPalette(transparent);
    }
    _layerList->setSelectionMode(QAbstractItemView::NoSelection);
    _layerList->setFocusPolicy(Qt::NoFocus);
    _layerList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _layerList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    boardLayout->addWidget(_boardView, 0, 0);
    boardLayout->addWidget(_layerList, 0, 0, Qt::AlignTop | Qt::AlignLeft);
    _resultsView = new ResultsView;
    _fieldViewer = new FieldViewer;
    _geometryView = new GeometryView;
    _pages->addWidget(_emptyPage);
    _pages->addWidget(_boardPage);
    _pages->addWidget(_geometryView);
    _powerView = new PowerView(&_document);
    _pages->addWidget(_powerView);
    _pages->addWidget(_resultsView);
    _pages->addWidget(_fieldViewer);
    _traceView = new TraceView;
    _pages->addWidget(_traceView);

    _inspector = new Inspector(&_document);
    _inspector->setMinimumWidth(340);

    auto* splitter = new QSplitter;
    splitter->addWidget(sidebarPanel);
    splitter->addWidget(_pages);
    splitter->addWidget(_inspector);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({250, 850, 400});

    auto* central = new QWidget;
    auto* centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(splitter, 1);
    setCentralWidget(central);

    // Log and status.
    _log = new QPlainTextEdit;
    _log->setReadOnly(true);
    _log->setMaximumBlockCount(20000);
    _log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    _logDock = new QDockWidget(tr("Log"), this);
    _logDock->setObjectName("log");
    _logDock->setWidget(_log);
    addDockWidget(Qt::BottomDockWidgetArea, _logDock);
    _logDock->hide();
    // The queue of runs, like the macOS app's Jobs window.
    _jobList = new QTreeWidget;
    _jobList->setObjectName("jobList");
    _jobList->setHeaderLabels({tr("Simulation"), tr("Status"), tr("Progress"), tr("Time")});
    _jobList->setRootIsDecorated(false);
    _jobList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _jobList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    _jobList->header()->setStretchLastSection(false);
    _cancelJobButton = new QPushButton(tr("Cancel"));
    _cancelJobButton->setObjectName("cancelJob");
    _cancelJobButton->setToolTip(tr("Take the selected runs off the queue, or stop the one running."));
    _clearJobsButton = new QPushButton(tr("Clear Finished"));
    _clearJobsButton->setObjectName("clearJobs");
    auto* jobsPanel = new QWidget;
    auto* jobsLayout = new QVBoxLayout(jobsPanel);
    jobsLayout->setContentsMargins(4, 4, 4, 4);
    jobsLayout->addWidget(_jobList, 1);
    auto* jobButtons = new QHBoxLayout;
    jobButtons->addStretch(1);
    jobButtons->addWidget(_cancelJobButton);
    jobButtons->addWidget(_clearJobsButton);
    jobsLayout->addLayout(jobButtons);
    _jobsDock = new QDockWidget(tr("Jobs"), this);
    _jobsDock->setObjectName("jobs");
    _jobsDock->setWidget(jobsPanel);
    addDockWidget(Qt::BottomDockWidgetArea, _jobsDock);
    tabifyDockWidget(_logDock, _jobsDock);
    _jobsDock->hide();
    _runClock = new QTimer(this);
    _runClock->setInterval(1000);
    connect(_runClock, &QTimer::timeout, this, [this] {
        updateRunStatus();
        updateRunningJobTime();
    });
    // Which engine runs use, and a click away from changing it.
    _engineButton = new QToolButton;
    _engineButton->setObjectName("engineButton");
    _engineButton->setAutoRaise(true);
    _engineButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    _engineButton->setIcon(icons::icon(icons::Glyph::Engine, palette().color(QPalette::WindowText)));
    _engineButton->setToolTip(tr("The engine simulations run on. Click to change it."));

    createActions();

    // The toolbar, as the macOS window has it: run buttons by the sidebar, the status display in
    // the middle, the engine at the end.
    auto* toolbar = addToolBar(tr("Toolbar"));
    toolbar->setObjectName("toolbar");
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->toggleViewAction()->setVisible(false);
    toolbar->setIconSize(QSize(18, 18));
    toolbar->setContextMenuPolicy(Qt::PreventContextMenu);
    toolbar->setStyleSheet("QToolBar { border: none; spacing: 4px; padding: 4px 8px; }");
    const QColor ink = palette().color(QPalette::WindowText);
    _runAction->setIcon(icons::icon(icons::Glyph::Play, ink));
    _runAllAction->setIcon(icons::icon(icons::Glyph::PlayAll, ink));
    _stopAction->setIcon(icons::icon(icons::Glyph::Stop, ink));
    _runAction->setToolTip(tr("Run the selected simulation, or every simulation in the selected set (Ctrl+R)"));
    _runAllAction->setToolTip(tr("Run every simulation (Ctrl+Alt+R)"));
    _stopAction->setToolTip(tr("Stop the running simulation and empty the queue (Ctrl+.)"));
    toolbar->addAction(_runAction);
    toolbar->addAction(_runAllAction);
    toolbar->addAction(_stopAction);
    const auto spacer = [] {
        auto* widget = new QWidget;
        widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        return widget;
    };
    toolbar->addWidget(spacer());
    toolbar->addWidget(_statusDisplay);
    toolbar->addWidget(spacer());
    toolbar->addWidget(_engineButton);
    connect(_statusDisplay, &StatusDisplay::chooseBoardRequested, this, &MainWindow::chooseBoard);
    connect(_statusDisplay, &StatusDisplay::showFailedTests, this, [this] { showNextIssue(true); });
    connect(_statusDisplay, &StatusDisplay::showWarnings, this, [this] { showNextIssue(false); });
    connect(_sidebar, &QTreeWidget::customContextMenuRequested, this, &MainWindow::showSidebarMenu);

    connect(&_document, &Document::changed, this, [this] {
        rebuildSidebar();
        updateHighlights();
        updateActions();
        scheduleSetRegeneration();
    });
    connect(&_document, &Document::modifiedChanged, this, &MainWindow::updateTitle);
    connect(_addButton, &QPushButton::clicked, this, [this] {
        QMenu menu(this);
        menu.addAction(_addAction);
        menu.addAction(_addSetAction);
        menu.addAction(_addPowerAction);
        menu.exec(_addButton->mapToGlobal(QPoint(0, _addButton->height())));
    });
    connect(_removeButton, &QPushButton::clicked, this, &MainWindow::removeSimulation);
    connect(_fieldViewer, &FieldViewer::captureRequested, this, [this] { runSimulation(true); });
    connect(_traceView, &TraceView::runRequested, this, [this] { runSimulation(false); });
    connect(_geometryView, &GeometryView::buildRequested, this, &MainWindow::buildGeometry);
    connect(_powerView, &PowerView::runFinished, this, [this](const QString& name, bool ok) {
        showStatus(ok ? tr("%1's power analysis finished.").arg(name) : tr("%1's power analysis failed.").arg(name));
        updateActions();
    });
    connect(_engineButton, &QToolButton::clicked, this, &MainWindow::showEngineSettings);
    connect(&_gpuDevices, &GpuDeviceList::changed, this, &MainWindow::updateEngineButton);
    connect(_sidebar, &QTreeWidget::currentItemChanged, this, &MainWindow::selectionChanged);
    connect(_boardView, &BoardView::picked, _inspector, &Inspector::setPick);
    connect(_layerList, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        _boardView->setLayerVisible(item->text(), item->checkState() == Qt::Checked);
    });

    connect(&_boardModel, &BoardModel::loadingStarted, this, [this] {
        showStatus(tr("Loading the board through KiCad..."));
    });
    connect(&_boardModel, &BoardModel::loaded, this, [this] {
        showStatus(_boardModel.data() ? tr("Board loaded.") : QString());
        _boardView->setBoard(_boardModel.data());
        _fieldViewer->setBoard(_boardModel.data());
        _inspector->setBoard(_boardModel.data());
        _powerView->setBoard(_boardModel.data());
        _traceView->setBoard(_boardModel.data());
        populateLayerList();
        updateHighlights();
        if (_pages->currentWidget() == _geometryView) refreshGeometryPage();
        // Sets edited before the board was there to match against.
        scheduleSetRegeneration();
    });
    connect(&_boardModel, &BoardModel::failed, this, [this](const QString& error) {
        showStatus(tr("The board couldn't be loaded."));
        QMessageBox::warning(this, tr("KiEMS"), tr("The KiCad board couldn't be loaded:\n\n%1").arg(error));
    });

    connect(&_runner, &PipelineRunner::line, this, [this](const QString& text) { _log->appendPlainText(text); });
    connect(&_runner, &PipelineRunner::progress, &_jobs, &JobQueue::reportProgress);
    connect(&_runner, &PipelineRunner::finished, &_jobs, &JobQueue::reportFinished);
    _jobs.setHooks({[this](const Job& job, QString* error) { return startJob(job, error); }, [this] { _runner.stop(); }});
    connect(&_jobs, &JobQueue::changed, this, [this] {
        updateJobList();
        updateRunStatus();
        refreshSidebarStatus();
        updateActions();
        if (_pages->currentWidget() == _geometryView && currentSimulation() >= 0) {
            _geometryView->setBuilding(_jobs.contains(_document.simulationName(currentSimulation())));
        }
    });
    connect(&_jobs, &JobQueue::jobStarted, this, [this] { rebuildSidebar(); });
    connect(&_jobs, &JobQueue::jobFinished, this, &MainWindow::jobFinished);
    connect(&_jobs, &JobQueue::drained, this, [this] {
        if (_batchStarted > 1 && _batchFailed > 0) {
            showStatus(tr("%1 of %2 runs failed; the Jobs list says why.").arg(_batchFailed).arg(_batchStarted));
        } else if (_batchStarted > 1 && _batchStopped == 0) {
            showStatus(tr("All %1 runs finished.").arg(_batchStarted));
        }
        _batchStarted = _batchFailed = _batchStopped = 0;
        rebuildSidebar();
        updateRunStatus();
        updateActions();
    });
    connect(&_document, &Document::simulationRenamed, &_jobs, &JobQueue::renameSimulation);
    connect(_jobList, &QTreeWidget::itemSelectionChanged, this, &MainWindow::updateJobList);
    connect(_cancelJobButton, &QPushButton::clicked, this, [this] {
        QList<int> ids;
        for (const QTreeWidgetItem* item : _jobList->selectedItems()) ids << item->data(0, Qt::UserRole).toInt();
        for (int id : ids) _jobs.cancel(id);
    });
    connect(_clearJobsButton, &QPushButton::clicked, &_jobs, &JobQueue::clearFinished);

    _engine = EngineSettings::load();
    updateEngineButton();
    _gpuDevices.refresh();

    rebuildSidebar();
    updateTitle();
    updateActions();
}

void MainWindow::createActions() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New..."), QKeySequence::New, this, &MainWindow::newDocument);
    file->addAction(tr("&Open..."), QKeySequence::Open, this, &MainWindow::openDocumentDialog);
    _saveAction = file->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::saveDocument);
    file->addAction(tr("Save &Report..."), QKeySequence(Qt::CTRL | Qt::Key_P), this, &MainWindow::saveReport);
    file->addSeparator();
    file->addAction(tr("Change &Board..."), this, &MainWindow::chooseBoard);
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* simulation = menuBar()->addMenu(tr("&Simulation"));
    _addAction = simulation->addAction(tr("&Add Simulation..."), this, &MainWindow::addSimulation);
    _addSetAction = simulation->addAction(tr("Add Simulation &Set..."), this, &MainWindow::addSimulationSet);
    _addSetAction->setToolTip(tr("Add a template that generates one simulation per net it selects."));
    _addPowerAction = simulation->addAction(tr("Add &Power Analysis..."), this, &MainWindow::addPowerAnalysis);
    _addPowerAction->setToolTip(tr("Add a DC analysis of a power rail: its voltage drop and current density."));
    _removeAction = simulation->addAction(tr("&Remove Simulation"), this, &MainWindow::removeSimulation);
    simulation->addSeparator();
    _runAction = simulation->addAction(tr("&Run"), QKeySequence(Qt::CTRL | Qt::Key_R), this, &MainWindow::runSelected);
    _runFieldsAction = simulation->addAction(tr("Run and Store &Fields"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R),
                                             this, [this] { runSimulation(true); });
    _runAllAction = simulation->addAction(tr("Run &All"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_R), this,
                                          &MainWindow::runAll);
    _stopAction = simulation->addAction(tr("S&top"), QKeySequence(Qt::CTRL | Qt::Key_Period), this, &MainWindow::stopRun);
    simulation->addSeparator();
    simulation->addAction(tr("&Engine..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), this,
                          &MainWindow::showEngineSettings);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("&Fit Board"), QKeySequence(Qt::CTRL | Qt::Key_0), this, [this] { _boardView->fitToBoard(); });
    view->addAction(_logDock->toggleViewAction());
    view->addAction(_jobsDock->toggleViewAction());
}

bool MainWindow::openDocument(const QString& path) {
    if (!maybeSave()) return false;
    QString error;
    if (!_document.open(path, &error)) {
        QMessageBox::warning(this, tr("KiEMS"), error);
        return false;
    }
    resetSetSnapshots();
    updateTitle();
    loadBoard();
    if (_document.simulationCount() > 0) showPage(0, Page::Board);
    else _pages->setCurrentWidget(_boardPage);
    return true;
}

void MainWindow::newDocument() {
    if (!maybeSave()) return;
    const QString board = QFileDialog::getOpenFileName(this, tr("Choose the KiCad Board to Simulate"), QString(),
                                                       tr("KiCad boards (*.kicad_pcb)"));
    if (board.isEmpty()) return;
    QString package = QFileDialog::getSaveFileName(this, tr("Save the New Simulation Document"),
                                                   QFileInfo(board).absolutePath() + "/" +
                                                       QFileInfo(board).completeBaseName() + ".g2esim",
                                                   tr("KiEMS simulations (*.g2esim)"));
    if (package.isEmpty()) return;
    if (!package.endsWith(".g2esim")) package += ".g2esim";
    QString error;
    if (!_document.create(package, board, &error)) {
        QMessageBox::warning(this, tr("KiEMS"), error);
        return;
    }
    resetSetSnapshots();
    updateTitle();
    loadBoard();
    _pages->setCurrentWidget(_boardPage);
}

void MainWindow::openDocumentDialog() {
    // A .g2esim document is a folder; picking the simulation.json inside it opens it too.
    const QString path = QFileDialog::getOpenFileName(this, tr("Open a Simulation (pick its simulation.json)"),
                                                      QString(), tr("KiEMS simulations (simulation.json)"));
    if (!path.isEmpty()) openDocument(path);
}

bool MainWindow::saveDocument() {
    QString error;
    if (!_document.save(&error)) {
        QMessageBox::warning(this, tr("KiEMS"), error);
        return false;
    }
    return true;
}

bool MainWindow::maybeSave() {
    if (!_document.isOpen() || !_document.isModified()) return true;
    const auto choice = QMessageBox::question(this, tr("KiEMS"),
                                              tr("Save changes to \"%1\"?").arg(_document.displayName()),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (choice == QMessageBox::Cancel) return false;
    return choice == QMessageBox::Discard || saveDocument();
}

void MainWindow::saveReport() {
    if (!_document.isOpen()) return;
    const QString suggested = QFileInfo(_document.configPath()).absoluteDir().absoluteFilePath(
        QFileInfo(_document.displayName()).completeBaseName() + " Report.pdf");
    QString path = QFileDialog::getSaveFileName(this, tr("Save Report"), suggested, tr("PDF documents (*.pdf)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(".pdf", Qt::CaseInsensitive)) path += ".pdf";
    showStatus(tr("Writing the report..."));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool written = writeReport(_document, path, &error, _boardModel.data().get());
    QApplication::restoreOverrideCursor();
    if (!written) {
        showStatus(QString());
        QMessageBox::warning(this, tr("KiEMS"), error);
        return;
    }
    showStatus(tr("Saved the report as %1.").arg(QDir::toNativeSeparators(path)));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (_jobs.isBusy()) {
        const int queued = _jobs.queuedCount();
        const QString question = queued == 0 ? tr("A simulation is running. Stop it and quit?")
                                              : tr("A simulation is running and %n more are queued. Stop them and quit?",
                                                   nullptr, queued);
        if (QMessageBox::question(this, tr("KiEMS"), question) != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        _jobs.stopAll();
    }
    if (maybeSave()) event->accept();
    else event->ignore();
}

void MainWindow::chooseBoard() {
    if (!_document.isOpen()) return;
    const QString board = QFileDialog::getOpenFileName(this, tr("Choose the KiCad Board"),
                                                       QFileInfo(_document.kicadPcbPath()).absolutePath(),
                                                       tr("KiCad boards (*.kicad_pcb)"));
    if (board.isEmpty()) return;
    _document.setKicadPcbPath(board);
    updateTitle();
    loadBoard();
}

void MainWindow::loadBoard() {
    const QString board = _document.kicadPcbPath();
    if (board.isEmpty()) {
        _boardModel.clear();
        return;
    }
    if (!QFileInfo::exists(board)) {
        showStatus(tr("The linked board %1 doesn't exist.").arg(QDir::toNativeSeparators(board)));
        _boardModel.clear();
        return;
    }
    _boardModel.load(board);
}

QString MainWindow::guessGroundNet() const {
    QString ground = "GND";
    if (auto board = _boardModel.data(); board && !board->nets.contains(ground)) {
        for (const QString& net : board->nets) {
            if (net.contains("GND", Qt::CaseInsensitive)) return net;
        }
    }
    return ground;
}

void MainWindow::addSimulation() {
    if (!_document.isOpen()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Add Simulation"), tr("Name:"), QLineEdit::Normal,
                                               _document.uniqueSimulationName(tr("Simulation")), &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) return;
    const int index = _document.addSimulation(_document.uniqueSimulationName(name), guessGroundNet());
    // Board Setup's ground, when chosen, replaces the guess above.
    SimulationEditor(_document, _boardModel.data().get(), index).applyBoardSetup();
    showPage(index, Page::Board);
}

void MainWindow::addSimulationSet() {
    if (!_document.isOpen()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Add Simulation Set"), tr("Name:"), QLineEdit::Normal,
                                               _document.uniqueSimulationName(tr("Simulation Set")), &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) return;
    int index;
    {
        const Document::ChangeBatch batch(_document);
        index = _document.addSimulationSet(_document.uniqueSimulationName(name), guessGroundNet());
        // What it generates starts from Board Setup's defaults, as any new simulation does.
        SimulationEditor(_document, _boardModel.data().get(), index).applyBoardSetup();
    }
    showPage(index, Page::Board);
}

void MainWindow::removeSimulation() {
    if (const int analysis = currentPowerAnalysis(); analysis >= 0) {
        const auto config = powerAnalysis(_document, analysis);
        if (QMessageBox::question(this, tr("KiEMS"),
                                  tr("Remove the power analysis \"%1\"?")
                                      .arg(config ? QString::fromStdString(config->name) : QString())) == QMessageBox::Yes) {
            removePowerAnalysis(_document, analysis);
        }
        return;
    }
    const int simulation = currentSimulation();
    if (simulation < 0) return;
    // A set goes with the simulations it generated; they can't be removed on their own, as the set
    // would only generate them again.
    const QList<int> generated = generatedSimulations(_document, simulation);
    const QString name = _document.simulationName(simulation);
    const QString question =
        _document.isSimulationSet(simulation)
            ? tr("Remove the Simulation Set \"%1\" and the %n simulation(s) it generated?", nullptr,
                 static_cast<int>(generated.size()))
                  .arg(name)
            : tr("Remove the simulation \"%1\"?").arg(name);
    if (QMessageBox::question(this, tr("KiEMS"), question) != QMessageBox::Yes) return;
    QList<int> doomed = generated;
    doomed << simulation;
    std::sort(doomed.begin(), doomed.end(), std::greater<>());
    const Document::ChangeBatch batch(_document);
    for (int index : doomed) _document.removeSimulation(index);
}

void MainWindow::runSelected() {
    runSimulation(false);
}

void MainWindow::buildGeometry() {
    const int simulation = currentSimulation();
    if (simulation < 0 || _document.isSimulationSet(simulation)) return;
    if (const QString problem = _document.validate(); !problem.isEmpty()) {
        QMessageBox::warning(this, tr("KiEMS"), tr("The geometry can't be built yet:\n\n%1").arg(problem));
        return;
    }
    _jobs.enqueue(_document.simulationName(simulation), false, true);
    refreshGeometryPage();
}

void MainWindow::refreshGeometryPage() {
    const int simulation = currentSimulation();
    if (simulation < 0 || _document.isSimulationSet(simulation)) return;
    const auto board = _boardModel.data();
    const GeometryContext context = board ? GeometryContext::of(*board, _document, simulation) : GeometryContext();
    _geometryView->setBuilding(_jobs.contains(_document.simulationName(simulation)));
    _geometryView->showSimulation(_document.simulationName(simulation), _document.geometryFile(simulation), context);
}

QList<int> MainWindow::runTargets(int simulation) const {
    if (_document.isSimulationSet(simulation)) return generatedSimulations(_document, simulation);
    return simulation >= 0 ? QList<int>{simulation} : QList<int>();
}

void MainWindow::runSimulation(bool storeFields) {
    if (currentPowerAnalysis() >= 0) {
        _powerView->run();
        return;
    }
    const int simulation = currentSimulation();
    if (simulation < 0) return;
    if (const QString problem = _document.validate(); !problem.isEmpty()) {
        QMessageBox::warning(this, tr("KiEMS"), tr("The simulation can't run yet:\n\n%1").arg(problem));
        return;
    }
    // A set runs the simulations it generated.
    if (_document.isSimulationSet(simulation)) {
        QStringList names;
        for (int target : runTargets(simulation)) {
            if (_document.mainExcitation(target)) names << _document.simulationName(target);
        }
        if (names.isEmpty()) {
            QMessageBox::information(this, tr("KiEMS"),
                                     tr("The set hasn't generated any simulations yet. Choose its excitation IC and "
                                        "nets in the Simulation tab."));
            return;
        }
        for (const QString& name : names) _jobs.enqueue(name, storeFields);
        if (_jobs.queuedCount() > 0) _jobsDock->show();
        return;
    }
    if (!_document.mainExcitation(simulation)) {
        QMessageBox::information(this, tr("KiEMS"),
                                 tr("Pick the pin to drive on the board, and tick \"Excited\" in the Selection tab."));
        return;
    }
    const QString name = _document.simulationName(simulation);
    const Job* running = _jobs.running();
    if (!_jobs.enqueue(name, storeFields)) {
        showStatus(running && running->simulation == name ? tr("%1 is already running.").arg(name)
                                                                     : tr("%1 is already queued.").arg(name));
    } else if (_jobs.running() && _jobs.running()->simulation != name) {
        showStatus(tr("%1 is queued.").arg(name));
        _jobsDock->show();
    }
}

void MainWindow::runAll() {
    if (const QString problem = _document.validate(); !problem.isEmpty()) {
        QMessageBox::warning(this, tr("KiEMS"), tr("The simulations can't run yet:\n\n%1").arg(problem));
        return;
    }
    QStringList skipped;
    QStringList runnable;
    for (int i = 0; i < _document.simulationCount(); ++i) {
        // A set's template never runs; the simulations it generated are among these.
        if (_document.isSimulationSet(i)) continue;
        (_document.mainExcitation(i) ? runnable : skipped) << _document.simulationName(i);
    }
    if (runnable.isEmpty()) {
        QMessageBox::information(this, tr("KiEMS"),
                                 tr("No simulation has a pin to drive yet. Pick one on the board, and tick "
                                    "\"Excited\" in the Selection tab."));
        return;
    }
    for (const QString& name : runnable) _jobs.enqueue(name, false);
    if (_jobs.queuedCount() > 0) _jobsDock->show();
    if (!skipped.isEmpty()) {
        _log->appendPlainText(tr("Not queued, as they have no pin to drive: %1").arg(skipped.join(", ")));
        showStatus(tr("Skipped %n simulation(s) with no pin to drive.", nullptr, static_cast<int>(skipped.size())));
    }
}

bool MainWindow::startJob(const Job& job, QString* error) {
    ++_batchStarted;
    const int simulation = _document.simulationIndex(job.simulation);
    if (simulation < 0) {
        *error = tr("The simulation no longer exists.");
        return false;
    }
    if (!job.geometryOnly && !_document.mainExcitation(simulation)) {
        *error = tr("It has no pin to drive.");
        return false;
    }
    if (const QString problem = _document.validate(); !problem.isEmpty()) {
        *error = problem;
        return false;
    }
    // The pipeline reads the document from disk.
    if (_document.isModified() && !_document.save(error)) return false;
    if (_batchStarted == 1) _log->clear();
    else _log->appendPlainText(QString());
    _log->appendPlainText(tr("== %1 ==").arg(job.simulation));
    _log->appendPlainText(tr("Engine: %1").arg(_engine.describe(_gpuDevices)));
    QStringList arguments = _engine.arguments(_gpuDevices);
    if (job.storeFields) arguments << "--field-frames";
    // A run clears the simulation's old output, which Windows refuses while a file in it is open:
    // let go of the Field Viewer's series first if it's showing this simulation.
    if (_fieldViewer->simulationDirectory() == _document.simulationDir(simulation)) _fieldViewer->close();
    if (!_runner.start(_document.configPath(), _document.kicadPcbPath(), job.simulation, arguments, error,
                       job.geometryOnly, job.geometryOnly ? -1.0 : _runSeconds.value(job.simulation, -1.0))) {
        return false;
    }
    _timedJobId = job.id;
    _logDock->show();
    return true;
}

void MainWindow::jobFinished(const Job& job) {
    if (job.id == _timedJobId) {
        const double seconds = _runner.timing().elapsed;
        _jobSeconds.insert(job.id, seconds);
        // A full run's length, for estimating the next one (fields stored or not, it's the same run).
        if (job.status == Job::Status::Succeeded && !job.geometryOnly) _runSeconds.insert(job.simulation, seconds);
        updateJobList();
    }
    if (job.status == Job::Status::Failed) ++_batchFailed;
    if (job.status == Job::Status::Stopped) ++_batchStopped;
    if (job.status == Job::Status::Failed) (job.geometryOnly ? _failedGeometry : _failedRuns).insert(job.simulation);
    if (job.status == Job::Status::Succeeded) {
        _failedGeometry.remove(job.simulation);
        if (!job.geometryOnly) _failedRuns.remove(job.simulation);
    }
    switch (job.status) {
    case Job::Status::Succeeded: showStatus(tr("%1 finished.").arg(job.simulation)); break;
    case Job::Status::Stopped: showStatus(tr("%1 stopped.").arg(job.simulation)); break;
    default: showStatus(tr("%1: %2").arg(job.simulation, job.message)); break;
    }
    rebuildSidebar();
    // A lone run shows its results (or why it failed) straight away. In a batch, the page in view
    // is only refreshed, so the next result doesn't pull the window away from what's being read.
    const bool lone = _batchStarted <= 1 && _jobs.queuedCount() == 0;
    const int simulation = _document.simulationIndex(job.simulation);
    if (job.status == Job::Status::Succeeded && simulation >= 0) {
        if (lone) showPage(simulation, job.geometryOnly ? Page::Geometry : job.storeFields ? Page::Fields : Page::Results);
        else if (currentSimulation() == simulation) selectionChanged();
    } else if (job.status == Job::Status::Failed && lone) {
        QMessageBox::warning(this, tr("KiEMS"), tr("The simulation failed:\n\n%1").arg(job.message));
    }
}

void MainWindow::updateJobList() {
    QSet<int> selected;
    for (const QTreeWidgetItem* item : _jobList->selectedItems()) selected << item->data(0, Qt::UserRole).toInt();
    bool canCancel = false;
    bool anyFinished = false;
    {
        const QSignalBlocker blocker(_jobList);
        _jobList->clear();
        for (const Job& job : _jobs.jobs()) {
            QString status = statusText(job.status);
            if (job.status == Job::Status::Running && !job.stage.isEmpty()) status = job.stage;
            QString progress;
            if (job.status == Job::Status::Running && job.fraction >= 0) {
                progress = QString("%1%").arg(static_cast<int>(job.fraction * 100));
            }
            auto* item = new QTreeWidgetItem(
                _jobList, {job.geometryOnly  ? tr("%1 (geometry)").arg(job.simulation)
                           : job.storeFields ? tr("%1 (with fields)").arg(job.simulation)
                                             : job.simulation,
                           status, progress, timeText(job)});
            item->setData(0, Qt::UserRole, job.id);
            if (!job.message.isEmpty()) {
                item->setToolTip(0, job.message);
                item->setToolTip(1, job.message);
            }
            if (job.status == Job::Status::Failed) item->setForeground(1, QColor(0xd0, 0x3a, 0x2f));
            if (job.finished()) anyFinished = true;
            if (selected.contains(job.id)) {
                item->setSelected(true);
                if (!job.finished()) canCancel = true;
            }
        }
    }
    _jobList->resizeColumnToContents(1);
    _jobList->resizeColumnToContents(2);
    _jobList->resizeColumnToContents(3);
    _cancelJobButton->setEnabled(canCancel);
    _clearJobsButton->setEnabled(anyFinished);
}

namespace {

/// ", about 1:05 left", or ", finishing" once the estimate has run out.
QString timeLeft(const RunTiming& timing) {
    if (timing.remaining < 0) return QString();
    if (timing.remaining < 1) return MainWindow::tr(", finishing");
    return MainWindow::tr(", about %1 left").arg(formatDuration(timing.remaining));
}

} // namespace

QString MainWindow::timeText(const Job& job) const {
    switch (job.status) {
    case Job::Status::Running: {
        const RunTiming timing = _runner.timing();
        return formatDuration(timing.elapsed) + timeLeft(timing);
    }
    case Job::Status::Queued:
        if (!job.geometryOnly && _runSeconds.contains(job.simulation)) {
            return tr("about %1").arg(formatDuration(_runSeconds.value(job.simulation)));
        }
        return QString();
    default:
        return _jobSeconds.contains(job.id) ? formatDuration(_jobSeconds.value(job.id)) : QString();
    }
}

void MainWindow::updateRunningJobTime() {
    const Job* job = _jobs.running();
    if (!job) return;
    for (int i = 0; i < _jobList->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = _jobList->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toInt() == job->id) item->setText(3, timeText(*job));
    }
}

void MainWindow::updateRunStatus() {
    const Job* job = _jobs.running();
    const int queued = _jobs.queuedCount();
    if (!job) {
        _runClock->stop();
        _statusDisplay->setActivity(queued > 0 ? tr("%n simulation(s) queued", nullptr, queued) : _lastStatus, false, std::nullopt);
        _statusDisplay->setToolTip(QString());
        return;
    }
    if (!_runClock->isActive()) _runClock->start();
    // "Simulating “USB” — 42% · port 1 of 2 · about 1:05 left · 2 more queued"
    QString text = QString("%1 \u201C%2\u201D").arg(job->stage.isEmpty() ? tr("Starting") : job->stage, job->simulation);
    if (job->fraction >= 0) text += QString(" \u2014 %1%").arg(static_cast<int>(std::lround(job->fraction * 100)));
    QStringList parts{text};
    const RunTiming timing = _runner.timing();
    if (timing.ports > 1 && timing.port > 0) parts << tr("port %1 of %2").arg(timing.port).arg(timing.ports);
    if (timing.remaining >= 0) {
        parts << (timing.remaining < 1 ? tr("finishing") : tr("about %1 left").arg(formatDuration(timing.remaining)));
    } else if (!job->geometryOnly && job->stage == tr("Simulating")) {
        parts << tr("estimating the time left");
    }
    if (queued > 0) parts << tr("%n more queued", nullptr, queued);
    _statusDisplay->setActivity(parts.join(QString::fromUtf8(" \u00B7 ")), true,
                                job->fraction >= 0 ? std::optional<double>(job->fraction) : std::nullopt);
    _statusDisplay->setToolTip(tr("%1 so far").arg(formatDuration(timing.elapsed)) +
                               (timing.remaining >= 0
                                    ? (timing.decayLimited
                                           ? tr("; expected to stop when the field energy has decayed by %1 dB, judged "
                                                "from how fast it's decaying now.")
                                                 .arg(RunEstimator::kTargetDecayDB)
                                           : tr("; expected to run to its step limit, at the current speed."))
                                    : QString()));
}

void MainWindow::showStatus(const QString& message) {
    _lastStatus = message;
    if (!_jobs.running()) updateRunStatus();
}

void MainWindow::stopRun() {
    _jobs.stopAll();
}

void MainWindow::showEngineSettings() {
    EngineSettingsDialog dialog(&_gpuDevices, _engine, this);
    if (dialog.exec() != QDialog::Accepted) return;
    _engine = dialog.settings();
    _engine.save();
    updateEngineButton();
}

void MainWindow::updateEngineButton() {
    QString text = _engine.describe(_gpuDevices);
    if (_engine.mode == EngineSettings::Mode::Automatic && _engine.usesGPU(_gpuDevices)) text += tr(" (automatic)");
    _engineButton->setText(tr("Engine: %1").arg(text));
}

void MainWindow::resetSetSnapshots() {
    _setSnapshots.clear();
    for (int i = 0; i < _document.simulationCount(); ++i) {
        if (_document.isSimulationSet(i)) _setSnapshots.insert(_document.simulationSetId(i), *_document.simulationJson(i));
    }
}

void MainWindow::scheduleSetRegeneration() {
    if (_setRegenerationPending) return;
    _setRegenerationPending = true;
    // After the edit that prompted it has finished, and once for a burst of edits.
    QTimer::singleShot(0, this, &MainWindow::regenerateChangedSets);
}

void MainWindow::regenerateChangedSets() {
    _setRegenerationPending = false;
    const auto board = _boardModel.data();
    if (!_document.isOpen() || !board) return; // left until there's a board to match against
    QStringList ids;
    for (int i = 0; i < _document.simulationCount(); ++i) {
        if (_document.isSimulationSet(i)) ids << _document.simulationSetId(i);
    }
    for (const QString& id : ids) {
        int index = -1;
        for (int i = 0; i < _document.simulationCount() && index < 0; ++i) {
            if (_document.simulationSetId(i) == id) index = i;
        }
        if (index < 0) continue;
        const nlohmann::json snapshot = *_document.simulationJson(index);
        if (_setSnapshots.contains(id) && _setSnapshots.value(id) == snapshot) continue;
        _setSnapshots.insert(id, snapshot);
        const QString name = _document.simulationName(index);
        const auto result = regenerateSimulationSet(_document, board.get(), index);
        if (result && (result->added || result->removed)) {
            // Removing simulations may have moved the template.
            int generated = 0;
            for (int i = 0; i < _document.simulationCount(); ++i) {
                if (!_document.isSimulationSet(i) && _document.generatedBySet(i) == id) ++generated;
            }
            showStatus(tr("%1 now generates %n simulation(s).", nullptr, generated).arg(name));
        }
    }
    for (auto it = _setSnapshots.begin(); it != _setSnapshots.end();) {
        it = ids.contains(it.key()) ? std::next(it) : _setSnapshots.erase(it);
    }
}

int MainWindow::currentPowerAnalysis() const {
    const QTreeWidgetItem* item = _sidebar->currentItem();
    return item && item->data(0, kPowerRole).isValid() ? item->data(0, kPowerRole).toInt() : -1;
}

void MainWindow::showPowerAnalysis(int index) {
    for (QTreeWidgetItemIterator it(_sidebar); *it; ++it) {
        if ((*it)->data(0, kPowerRole).isValid() && (*it)->data(0, kPowerRole).toInt() == index) {
            _sidebar->setCurrentItem(*it);
            selectionChanged();
            return;
        }
    }
}

void MainWindow::addPowerAnalysis() {
    if (!_document.isOpen()) return;
    const auto board = _boardModel.data();
    if (!board) {
        QMessageBox::information(this, tr("KiEMS"), tr("The board is still loading; try again once it has."));
        return;
    }
    bool ok = false;
    const QString net = QInputDialog::getItem(this, tr("Add Power Analysis"), tr("The rail's net:"),
                                              powerNetCandidates(*board), 0, true, &ok)
                            .trimmed();
    if (!ok || net.isEmpty()) return;
    const int index = kiems_gui::addPowerAnalysis(_document, newPowerAnalysis(*board, net));
    showPowerAnalysis(index);
}

int MainWindow::currentSimulation() const {
    const QTreeWidgetItem* item = _sidebar->currentItem();
    return item ? item->data(0, kSimulationRole).toInt() : -1;
}

void MainWindow::rebuildSidebar() {
    _rebuildingSidebar = true;
    const int selectedSimulation = currentSimulation();
    const int selectedPage = _sidebar->currentItem() ? _sidebar->currentItem()->data(0, kPageRole).toInt() : -1;
    const int selectedPower = currentPowerAnalysis();
    for (QTreeWidgetItemIterator it(_sidebar); *it; ++it) {
        if ((*it)->childCount() > 0) _expanded.insert(expansionKey(*it), (*it)->isExpanded());
    }
    _sidebar->clear();
    const QColor ink = palette().color(QPalette::WindowText);
    const auto separator = [this] {
        auto* item = new QTreeWidgetItem(_sidebar);
        item->setData(0, kSidebarSeparatorRole, true);
        item->setData(0, kSimulationRole, -1);
        item->setData(0, kPageRole, -1);
        item->setFlags(Qt::NoItemFlags);
        return item;
    };
    if (_document.isOpen()) {
        // Board Setup, pinned above every simulation, as on macOS.
        auto* setup = new QTreeWidgetItem(_sidebar, {tr("Board Setup")});
        setup->setIcon(0, icons::icon(icons::Glyph::Sliders, ink));
        setup->setData(0, kSimulationRole, -1);
        setup->setData(0, kPageRole, static_cast<int>(Page::BoardSetup));
        setup->setToolTip(0, tr("The board's defaults: its ground, impedances, bit rate and pin roles."));
        if (selectedPage == static_cast<int>(Page::BoardSetup)) _sidebar->setCurrentItem(setup);
        if (_document.simulationCount() > 0) separator();
    }
    for (int i = 0; i < _document.simulationCount(); ++i) {
        if (_document.isSimulationSet(i)) {
            // A set's row holds the simulations it generated, sorted by name.
            auto* set = new QTreeWidgetItem(_sidebar, {_document.simulationName(i)});
            set->setData(0, kSimulationRole, i);
            set->setData(0, kPageRole, -1);
            set->setData(0, Qt::UserRole + 2, "set:" + _document.simulationSetId(i));
            set->setIcon(0, icons::icon(icons::Glyph::Stack, ink));
            const QList<int> generated = generatedSimulations(_document, i);
            set->setToolTip(0, tr("Simulation Set: generates %n simulation(s).", nullptr, static_cast<int>(generated.size())));
            if (i == selectedSimulation) _sidebar->setCurrentItem(set);
            for (int g : generated) addSimulationItem(set, g, selectedSimulation, selectedPage);
            set->setExpanded(_expanded.value(expansionKey(set), true));
        } else if (_document.generatingTemplate(i) < 0) {
            addSimulationItem(nullptr, i, selectedSimulation, selectedPage);
        }
    }
    // Power analyses after the simulations.
    const auto analyses = powerAnalyses(_document);
    if (!analyses.empty()) separator();
    for (int i = 0; i < static_cast<int>(analyses.size()); ++i) {
        auto* item = new QTreeWidgetItem(_sidebar, {QString::fromStdString(analyses[static_cast<std::size_t>(i)].name)});
        item->setIcon(0, icons::icon(icons::Glyph::Bolt, QColor(0xf2, 0xc0, 0x3b)));
        item->setData(0, kSimulationRole, -1);
        item->setData(0, kPageRole, static_cast<int>(Page::Power));
        item->setData(0, kPowerRole, i);
        item->setToolTip(0, tr("DC power analysis of %1").arg(QString::fromStdString(analyses[static_cast<std::size_t>(i)].net)));
        if (i == selectedPower) _sidebar->setCurrentItem(item);
    }
    // The selection stays in view, even inside a collapsed row.
    if (QTreeWidgetItem* current = _sidebar->currentItem()) {
        for (QTreeWidgetItem* parent = current->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
    }
    _rebuildingSidebar = false;
    refreshSidebarStatus();
}

QTreeWidgetItem* MainWindow::addSimulationItem(QTreeWidgetItem* parent, int i, int selectedSimulation,
                                               int selectedPage) {
    const QString name = _document.simulationName(i);
    auto* top = parent ? new QTreeWidgetItem(parent, {name}) : new QTreeWidgetItem(_sidebar, {name});
    top->setData(0, kSimulationRole, i);
    top->setData(0, kPageRole, -1);
    top->setData(0, Qt::UserRole + 2, name);
    top->setToolTip(0, tr("%1: its board, involved nets and settings").arg(name));
    // The pages, in the grey of the macOS list's secondary rows.
    QColor secondary = palette().color(QPalette::WindowText);
    secondary.setAlphaF(0.78);
    const auto page = [&](const QString& title, icons::Glyph glyph, Page kind, const QString& tip) {
        auto* item = new QTreeWidgetItem(top, {title});
        item->setData(0, kSimulationRole, i);
        item->setData(0, kPageRole, static_cast<int>(kind));
        item->setIcon(0, icons::icon(glyph, secondary));
        item->setForeground(0, secondary);
        item->setToolTip(0, tip);
        return item;
    };
    auto* geometry = page(tr("Geometry"), icons::Glyph::Cube, Page::Geometry, tr("The simulated copper and its mesh, in 3D."));
    auto* results = page(tr("Simulation Results"), icons::Glyph::Chart, Page::Results,
                         tr("S-parameters, impedance, eyes and the time-domain response."));
    auto* fields = page(tr("Field Viewer"), icons::Glyph::Waveform, Page::Fields,
                        tr("The fields over time, from a run that stored them."));
    auto* trace = page(tr("Trace"), icons::Glyph::Trace, Page::Trace, tr("The impedance and ringing along the simulated traces."));
    // A set's simulations start folded, as there can be many.
    top->setExpanded(_expanded.value(expansionKey(top), parent == nullptr));
    if (i == selectedSimulation) {
        _sidebar->setCurrentItem(selectedPage == static_cast<int>(Page::Results)    ? results
                                 : selectedPage == static_cast<int>(Page::Fields)   ? fields
                                 : selectedPage == static_cast<int>(Page::Trace)    ? trace
                                 : selectedPage == static_cast<int>(Page::Geometry) ? geometry
                                                                                    : top);
    }
    return top;
}

void MainWindow::selectionChanged() {
    if (_rebuildingSidebar) return;
    const QTreeWidgetItem* item = _sidebar->currentItem();
    const int simulation = item ? item->data(0, kSimulationRole).toInt() : -1;
    const int page = item ? item->data(0, kPageRole).toInt() : -1;
    if (page == static_cast<int>(Page::BoardSetup)) {
        _inspector->setSimulation(-1);
        _inspector->showBoardSetup();
        _pages->setCurrentWidget(_boardPage);
        _inspector->show();
        updateHighlights();
        updateActions();
        return;
    }
    if (!item || (simulation < 0 && page < 0)) {
        _inspector->setSimulation(-1);
        static_cast<QLabel*>(_emptyPage)->setText(_document.isOpen() ? tr("No Simulation Selected")
                                                                     : tr("Open or create a simulation document to begin."));
        _pages->setCurrentWidget(_emptyPage);
        _inspector->hide();
        updateActions();
        return;
    }
    if (page == static_cast<int>(Page::Power)) {
        _inspector->setSimulation(-1);
        _powerView->showAnalysis(item->data(0, kPowerRole).toInt());
        _pages->setCurrentWidget(_powerView);
        _inspector->hide();
        updateHighlights();
        updateActions();
        return;
    }
    _inspector->setSimulation(simulation);
    updateHighlights();
    updateActions();
    if (page == static_cast<int>(Page::Results)) {
        _resultsView->showResults(_document.simulationName(simulation),
                                  SimulationResults::load(_document.resultsDir(simulation),
                                                          _document.simulationDir(simulation)));
        _pages->setCurrentWidget(_resultsView);
        _inspector->hide();
    } else if (page == static_cast<int>(Page::Geometry)) {
        refreshGeometryPage();
        _pages->setCurrentWidget(_geometryView);
        _inspector->hide();
    } else if (page == static_cast<int>(Page::Trace)) {
        const nlohmann::json start = _document.documentValue("/frequency/start");
        const nlohmann::json stop = _document.documentValue("/frequency/stop");
        _traceView->showSimulation(_document.simulationName(simulation), _document.simulationDir(simulation),
                                   stop.is_number() ? stop.get<double>() : 1e9, start.is_number() ? start.get<double>() : 0);
        _pages->setCurrentWidget(_traceView);
        _inspector->hide();
    } else if (page == static_cast<int>(Page::Fields)) {
        const SimulationResults results =
            SimulationResults::load(_document.resultsDir(simulation), _document.simulationDir(simulation));
        _fieldViewer->showSimulation(_document.simulationName(simulation), _document.simulationDir(simulation),
                                     results.portNames);
        _pages->setCurrentWidget(_fieldViewer);
        _inspector->hide();
    } else if (_document.isOpen()) {
        _pages->setCurrentWidget(_boardPage);
        _inspector->show();
    }
}

void MainWindow::showPage(int simulation, Page page) {
    for (QTreeWidgetItemIterator it(_sidebar); *it; ++it) {
        QTreeWidgetItem* item = *it;
        if (item->data(0, kSimulationRole).toInt() != simulation || item->data(0, kPageRole).toInt() != -1) continue;
        // A set's row has no pages of its own.
        QTreeWidgetItem* target = item;
        for (int c = 0; c < item->childCount() && page != Page::Board; ++c) {
            if (item->child(c)->data(0, kPageRole).toInt() == static_cast<int>(page)) target = item->child(c);
        }
        for (QTreeWidgetItem* parent = target->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
        _sidebar->setCurrentItem(target);
        selectionChanged();
        return;
    }
}

void MainWindow::updateHighlights() {
    const int simulation = currentSimulation();
    if (simulation < 0 || !_document.isOpen()) {
        _boardView->setHighlightedNets({});
        _boardView->setExcitation({}, {});
        return;
    }
    // A set shows every net its simulations cover.
    QSet<QString> nets;
    for (int target : _document.isSimulationSet(simulation) ? runTargets(simulation) : QList<int>{simulation}) {
        const QStringList involved = _document.involvedNets(target);
        nets.unite(QSet<QString>(involved.begin(), involved.end()));
    }
    _boardView->setHighlightedNets(nets);
    const auto excitation = _document.mainExcitation(simulation);
    _boardView->setExcitation(excitation ? excitation->footprint : QString(), excitation ? excitation->pin : QString());
}

void MainWindow::populateLayerList() {
    _layerList->blockSignals(true);
    _layerList->clear();
    for (const QString& layer : _boardView->drawableLayers()) {
        auto* item = new QListWidgetItem(colorSwatch(_boardView->layerColor(layer)), layer, _layerList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(_boardView->isLayerVisible(layer) ? Qt::Checked : Qt::Unchecked);
    }
    _layerList->blockSignals(false);
    // Exactly as big as its rows, so the board stays clickable everywhere else.
    const int rows = _layerList->count();
    const int rowHeight = rows > 0 ? _layerList->sizeHintForRow(0) : 0;
    _layerList->setFixedSize(_layerList->sizeHintForColumn(0) + 12,
                             std::min(rows * rowHeight + 4, std::max(_boardPage->height() - 8, rowHeight * 4)));
}

void MainWindow::updateTitle() {
    if (!_document.isOpen()) {
        setWindowTitle(tr("KiEMS"));
        _statusDisplay->setProject({}, {});
        return;
    }
    setWindowTitle(QString("%1[*] - KiEMS").arg(_document.displayName()));
    setWindowModified(_document.isModified());
    _statusDisplay->setProject(_document.displayName(), _document.kicadPcbPath());
}

void MainWindow::updateActions() {
    const bool open = _document.isOpen();
    const int simulation = currentSimulation();
    const bool haveSimulation = simulation >= 0;
    // The selected simulation can't be removed while its run is queued or under way, nor a set
    // while any of its simulations' are, nor one a set generated.
    bool busy = false;
    for (int target : runTargets(simulation)) busy = busy || _jobs.contains(_document.simulationName(target));
    const bool generated = haveSimulation && _document.generatingTemplate(simulation) >= 0;
    const bool canRun = !PipelineRunner::cliPath().isEmpty();
    _saveAction->setEnabled(open);
    _addAction->setEnabled(open);
    _addSetAction->setEnabled(open);
    _addPowerAction->setEnabled(open);
    _addButton->setEnabled(open);
    _removeAction->setEnabled((haveSimulation && !busy && !generated) || currentPowerAnalysis() >= 0);
    _removeButton->setEnabled(_removeAction->isEnabled());
    _removeButton->setToolTip(generated ? tr("A simulation its set generated goes when the set no longer selects its net.")
                                        : QString());
    _runAction->setEnabled((haveSimulation && canRun && !runTargets(simulation).isEmpty()) ||
                           (currentPowerAnalysis() >= 0 && _powerView->canRun()));
    _runFieldsAction->setEnabled(_runAction->isEnabled() && currentPowerAnalysis() < 0);
    _runAllAction->setEnabled(open && _document.simulationCount() > 0 && canRun);
    _stopAction->setEnabled(_jobs.isBusy());
}

bool MainWindow::failsTests(int simulation) const {
    const QString resultsDir = _document.resultsDir(simulation);
    const QFileInfo eyes(QDir(resultsDir).filePath("eyes.json"));
    if (!eyes.exists()) return false;
    const qint64 stamp = eyes.lastModified().toMSecsSinceEpoch();
    if (const auto it = _testVerdicts.constFind(resultsDir); it != _testVerdicts.constEnd() && it->first == stamp) return it->second;
    const bool fails = judgeSimulation(true, EyeResults::load(resultsDir)).kind == SimulationVerdict::Kind::Fail;
    _testVerdicts.insert(resultsDir, {stamp, fails});
    return fails;
}

int MainWindow::pageStatus(int simulation, Page page, double* progress) const {
    *progress = -1;
    const QString name = _document.simulationName(simulation);
    const Job* job = _jobs.running();
    if (job && job->simulation == name) {
        // The page the run is filling: geometry first, then the results (and the fields with them).
        const bool buildingGeometry = job->geometryOnly || job->stage == tr("Building geometry") || job->stage == tr("Starting");
        const bool runs = page == Page::Geometry ? buildingGeometry
                          : page == Page::Results || page == Page::Trace ? !job->geometryOnly
                          : page == Page::Fields ? job->storeFields
                                                 : false;
        if (runs) {
            if (!buildingGeometry || page == Page::Geometry) *progress = job->fraction;
            return static_cast<int>(SidebarStatus::Running);
        }
    } else if (_jobs.contains(name)) {
        for (const Job& queued : _jobs.jobs()) {
            if (queued.simulation != name || queued.status != Job::Status::Queued) continue;
            const bool queues = page == Page::Geometry || (!queued.geometryOnly && (page != Page::Fields || queued.storeFields));
            if (queues) return static_cast<int>(SidebarStatus::Queued);
        }
    }
    switch (page) {
    case Page::Geometry:
        if (_failedGeometry.contains(name)) return static_cast<int>(SidebarStatus::Failed);
        return static_cast<int>(_document.hasGeometry(simulation) ? SidebarStatus::Done : SidebarStatus::NotRun);
    case Page::Results:
        if (_failedRuns.contains(name)) return static_cast<int>(SidebarStatus::Failed);
        if (!_document.hasResults(simulation)) return static_cast<int>(SidebarStatus::NotRun);
        return static_cast<int>(failsTests(simulation) ? SidebarStatus::TestsFailed : SidebarStatus::Done);
    case Page::Fields:
        return static_cast<int>(FieldViewer::findSeries(_document.simulationDir(simulation)).empty() ? SidebarStatus::NotRun
                                                                                                     : SidebarStatus::Done);
    case Page::Trace:
        return static_cast<int>(_document.hasResults(simulation) ? SidebarStatus::Done : SidebarStatus::NotRun);
    default:
        return static_cast<int>(SidebarStatus::None);
    }
}

void MainWindow::refreshSidebarStatus() {
    if (!_document.isOpen()) return;
    for (QTreeWidgetItemIterator it(_sidebar); *it; ++it) {
        QTreeWidgetItem* item = *it;
        const int simulation = item->data(0, kSimulationRole).toInt();
        const int page = item->data(0, kPageRole).toInt();
        if (simulation < 0 || page < 0 || simulation >= _document.simulationCount()) continue;
        double progress = -1;
        const int status = pageStatus(simulation, static_cast<Page>(page), &progress);
        if (item->data(0, kSidebarStatusRole).toInt() != status) item->setData(0, kSidebarStatusRole, status);
        if (item->data(0, kSidebarProgressRole).toDouble() != progress) item->setData(0, kSidebarProgressRole, progress);
    }
    refreshIssues();
}

QList<QPair<int, MainWindow::Page>> MainWindow::issues(bool failedTests) const {
    QList<QPair<int, Page>> stops;
    for (QTreeWidgetItemIterator it(_sidebar); *it; ++it) {
        const QTreeWidgetItem* item = *it;
        const int simulation = item->data(0, kSimulationRole).toInt();
        const int page = item->data(0, kPageRole).toInt();
        if (simulation < 0 || page < 0) continue;
        const auto status = static_cast<SidebarStatus>(item->data(0, kSidebarStatusRole).toInt());
        if (failedTests ? status == SidebarStatus::TestsFailed : status == SidebarStatus::Failed) {
            stops.append({simulation, static_cast<Page>(page)});
        }
    }
    return stops;
}

void MainWindow::refreshIssues() {
    _statusDisplay->setIssueCounts(static_cast<int>(issues(true).size()), static_cast<int>(issues(false).size()));
}

void MainWindow::showNextIssue(bool failedTests) {
    // The next after the selected row, wrapping round, so repeated clicks step through them all.
    const QList<QPair<int, Page>> stops = issues(failedTests);
    if (stops.isEmpty()) return;
    const QTreeWidgetItem* current = _sidebar->currentItem();
    const QPair<int, Page> here = current ? QPair<int, Page>(current->data(0, kSimulationRole).toInt(),
                                                               static_cast<Page>(current->data(0, kPageRole).toInt()))
                                          : QPair<int, Page>(-1, Page::Board);
    const qsizetype at = stops.indexOf(here);
    const QPair<int, Page> next = stops[at < 0 ? 0 : (at + 1) % stops.size()];
    showPage(next.first, next.second);
}

void MainWindow::showSidebarMenu(const QPoint& at) {
    QTreeWidgetItem* item = _sidebar->itemAt(at);
    if (!item || item->data(0, kSidebarSeparatorRole).toBool()) return;
    _sidebar->setCurrentItem(item);
    const int simulation = item->data(0, kSimulationRole).toInt();
    const int page = item->data(0, kPageRole).toInt();
    QMenu menu(this);
    if (simulation >= 0) {
        if (page == static_cast<int>(Page::Geometry)) {
            QAction* build = menu.addAction(tr("Build Geometry"), this, &MainWindow::buildGeometry);
            build->setEnabled(_runAction->isEnabled());
        }
        menu.addAction(_runAction);
        menu.addAction(_runFieldsAction);
        if (page < 0) {
            menu.addSeparator();
            menu.addAction(_removeAction);
        }
    } else if (page == static_cast<int>(Page::Power)) {
        menu.addAction(_runAction);
        menu.addSeparator();
        menu.addAction(_removeAction);
    } else {
        return;
    }
    menu.exec(_sidebar->viewport()->mapToGlobal(at));
}

} // namespace kiems_gui
