#include "MainWindow.hpp"

#include "BoardView.hpp"
#include "FieldViewer.hpp"
#include "Inspector.hpp"
#include "Report.hpp"
#include "ResultsView.hpp"
#include "SimulationEditor.hpp"

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
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace kiems_gui {

namespace {

constexpr int kSimulationRole = Qt::UserRole;
constexpr int kPageRole = Qt::UserRole + 1;

/// A status dot like the macOS sidebar's: hollow until a stage has run, filled once it has.
QIcon statusDot(bool done, const QColor& color) {
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.5));
    painter.setBrush(done ? QBrush(color) : Qt::NoBrush);
    painter.drawEllipse(QRectF(2, 2, 10, 10));
    return QIcon(pixmap);
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

    // Project bar.
    auto* projectBar = new QWidget;
    auto* projectLayout = new QHBoxLayout(projectBar);
    projectLayout->setContentsMargins(12, 8, 12, 8);
    auto* projectTitle = new QLabel(tr("<b>Project:</b>"));
    _projectLabel = new QLabel;
    _projectLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _chooseBoardButton = new QPushButton(tr("Change Board..."));
    projectLayout->addWidget(projectTitle);
    projectLayout->addWidget(_projectLabel, 1);
    projectLayout->addWidget(_chooseBoardButton);

    // Sidebar.
    auto* sidebarPanel = new QWidget;
    auto* sidebarLayout = new QVBoxLayout(sidebarPanel);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    _sidebar = new QTreeWidget;
    _sidebar->setHeaderHidden(true);
    _sidebar->setColumnCount(1);
    _sidebar->setMinimumWidth(240);
    _sidebar->setIconSize(QSize(14, 14));
    _addButton = new QPushButton("+");
    _removeButton = new QPushButton(QString::fromUtf8("\xE2\x88\x92"));
    for (QPushButton* button : {_addButton, _removeButton}) button->setFixedWidth(32);
    auto* sidebarButtons = new QHBoxLayout;
    sidebarButtons->addStretch();
    sidebarButtons->addWidget(_addButton);
    sidebarButtons->addWidget(_removeButton);
    sidebarLayout->addWidget(_sidebar, 1);
    sidebarLayout->addLayout(sidebarButtons);

    // Pages.
    _pages = new QStackedWidget;
    _emptyPage = new QLabel(tr("Open or create a simulation document (File menu) to begin."));
    static_cast<QLabel*>(_emptyPage)->setAlignment(Qt::AlignCenter);
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
    _pages->addWidget(_emptyPage);
    _pages->addWidget(_boardPage);
    _pages->addWidget(_resultsView);
    _pages->addWidget(_fieldViewer);

    _inspector = new Inspector(&_document);
    _inspector->setMinimumWidth(300);

    auto* splitter = new QSplitter;
    splitter->addWidget(sidebarPanel);
    splitter->addWidget(_pages);
    splitter->addWidget(_inspector);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 900, 320});

    auto* central = new QWidget;
    auto* centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(projectBar);
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
    _jobList->setHeaderLabels({tr("Simulation"), tr("Status"), tr("Progress")});
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
    _statusLabel = new QLabel;
    _progress = new QProgressBar;
    _progress->setMaximumWidth(260);
    _progress->setTextVisible(false);
    _progress->hide();
    _stopButton = new QPushButton(tr("Stop"));
    _stopButton->setToolTip(tr("Stop the running simulation and empty the queue."));
    _stopButton->hide();
    // Which engine runs use, and a click away from changing it.
    _engineButton = new QPushButton;
    _engineButton->setFlat(true);
    _engineButton->setToolTip(tr("The engine simulations run on. Click to change it."));
    statusBar()->addWidget(_statusLabel, 1);
    statusBar()->addPermanentWidget(_engineButton);
    statusBar()->addPermanentWidget(_progress);
    statusBar()->addPermanentWidget(_stopButton);

    createActions();

    connect(&_document, &Document::changed, this, [this] {
        rebuildSidebar();
        updateHighlights();
        updateActions();
    });
    connect(&_document, &Document::modifiedChanged, this, &MainWindow::updateTitle);
    connect(_chooseBoardButton, &QPushButton::clicked, this, &MainWindow::chooseBoard);
    connect(_addButton, &QPushButton::clicked, this, &MainWindow::addSimulation);
    connect(_removeButton, &QPushButton::clicked, this, &MainWindow::removeSimulation);
    connect(_stopButton, &QPushButton::clicked, this, &MainWindow::stopRun);
    connect(_fieldViewer, &FieldViewer::captureRequested, this, [this] { runSimulation(true); });
    connect(_engineButton, &QPushButton::clicked, this, &MainWindow::showEngineSettings);
    connect(&_gpuDevices, &GpuDeviceList::changed, this, &MainWindow::updateEngineButton);
    connect(_sidebar, &QTreeWidget::currentItemChanged, this, &MainWindow::selectionChanged);
    connect(_boardView, &BoardView::picked, _inspector, &Inspector::setPick);
    connect(_layerList, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        _boardView->setLayerVisible(item->text(), item->checkState() == Qt::Checked);
    });

    connect(&_boardModel, &BoardModel::loadingStarted, this, [this] {
        _statusLabel->setText(tr("Loading the board through KiCad..."));
    });
    connect(&_boardModel, &BoardModel::loaded, this, [this] {
        _statusLabel->setText(_boardModel.data() ? tr("Board loaded.") : QString());
        _boardView->setBoard(_boardModel.data());
        _fieldViewer->setBoard(_boardModel.data());
        _inspector->setBoard(_boardModel.data());
        populateLayerList();
        updateHighlights();
    });
    connect(&_boardModel, &BoardModel::failed, this, [this](const QString& error) {
        _statusLabel->setText(tr("The board couldn't be loaded."));
        QMessageBox::warning(this, tr("KiEMS"), tr("The KiCad board couldn't be loaded:\n\n%1").arg(error));
    });

    connect(&_runner, &PipelineRunner::line, this, [this](const QString& text) { _log->appendPlainText(text); });
    connect(&_runner, &PipelineRunner::progress, &_jobs, &JobQueue::reportProgress);
    connect(&_runner, &PipelineRunner::finished, &_jobs, &JobQueue::reportFinished);
    _jobs.setHooks({[this](const Job& job, QString* error) { return startJob(job, error); }, [this] { _runner.stop(); }});
    connect(&_jobs, &JobQueue::changed, this, [this] {
        updateJobList();
        updateRunStatus();
        updateActions();
    });
    connect(&_jobs, &JobQueue::jobStarted, this, [this] { rebuildSidebar(); });
    connect(&_jobs, &JobQueue::jobFinished, this, &MainWindow::jobFinished);
    connect(&_jobs, &JobQueue::drained, this, [this] {
        if (_batchStarted > 1 && _batchFailed > 0) {
            _statusLabel->setText(tr("%1 of %2 runs failed; the Jobs list says why.").arg(_batchFailed).arg(_batchStarted));
        } else if (_batchStarted > 1 && _batchStopped == 0) {
            _statusLabel->setText(tr("All %1 runs finished.").arg(_batchStarted));
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
    _statusLabel->setText(tr("Writing the report..."));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool written = writeReport(_document, path, &error);
    QApplication::restoreOverrideCursor();
    if (!written) {
        _statusLabel->clear();
        QMessageBox::warning(this, tr("KiEMS"), error);
        return;
    }
    _statusLabel->setText(tr("Saved the report as %1.").arg(QDir::toNativeSeparators(path)));
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
        _statusLabel->setText(tr("The linked board %1 doesn't exist.").arg(QDir::toNativeSeparators(board)));
        _boardModel.clear();
        return;
    }
    _boardModel.load(board);
}

void MainWindow::addSimulation() {
    if (!_document.isOpen()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Add Simulation"), tr("Name:"), QLineEdit::Normal,
                                               _document.uniqueSimulationName(tr("Simulation")), &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) return;
    QString ground = "GND";
    if (auto board = _boardModel.data(); board && !board->nets.contains(ground)) {
        for (const QString& net : board->nets) {
            if (net.contains("GND", Qt::CaseInsensitive)) {
                ground = net;
                break;
            }
        }
    }
    const int index = _document.addSimulation(_document.uniqueSimulationName(name), ground);
    // Board Setup's ground, when chosen, replaces the guess above.
    SimulationEditor(_document, _boardModel.data().get(), index).applyBoardSetup();
    showPage(index, Page::Board);
}

void MainWindow::removeSimulation() {
    const int simulation = currentSimulation();
    if (simulation < 0) return;
    if (QMessageBox::question(this, tr("KiEMS"),
                              tr("Remove the simulation \"%1\"?").arg(_document.simulationName(simulation))) !=
        QMessageBox::Yes) {
        return;
    }
    _document.removeSimulation(simulation);
}

void MainWindow::runSelected() {
    runSimulation(false);
}

void MainWindow::runSimulation(bool storeFields) {
    const int simulation = currentSimulation();
    if (simulation < 0) return;
    if (const QString problem = _document.validate(); !problem.isEmpty()) {
        QMessageBox::warning(this, tr("KiEMS"), tr("The simulation can't run yet:\n\n%1").arg(problem));
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
        _statusLabel->setText(running && running->simulation == name ? tr("%1 is already running.").arg(name)
                                                                     : tr("%1 is already queued.").arg(name));
    } else if (_jobs.running() && _jobs.running()->simulation != name) {
        _statusLabel->setText(tr("%1 is queued.").arg(name));
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
        _statusLabel->setText(tr("Skipped %n simulation(s) with no pin to drive.", nullptr, static_cast<int>(skipped.size())));
    }
}

bool MainWindow::startJob(const Job& job, QString* error) {
    ++_batchStarted;
    const int simulation = _document.simulationIndex(job.simulation);
    if (simulation < 0) {
        *error = tr("The simulation no longer exists.");
        return false;
    }
    if (!_document.mainExcitation(simulation)) {
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
    if (!_runner.start(_document.configPath(), _document.kicadPcbPath(), job.simulation, arguments, error)) return false;
    _logDock->show();
    return true;
}

void MainWindow::jobFinished(const Job& job) {
    if (job.status == Job::Status::Failed) ++_batchFailed;
    if (job.status == Job::Status::Stopped) ++_batchStopped;
    switch (job.status) {
    case Job::Status::Succeeded: _statusLabel->setText(tr("%1 finished.").arg(job.simulation)); break;
    case Job::Status::Stopped: _statusLabel->setText(tr("%1 stopped.").arg(job.simulation)); break;
    default: _statusLabel->setText(tr("%1: %2").arg(job.simulation, job.message)); break;
    }
    rebuildSidebar();
    // A lone run shows its results (or why it failed) straight away. In a batch, the page in view
    // is only refreshed, so the next result doesn't pull the window away from what's being read.
    const bool lone = _batchStarted <= 1 && _jobs.queuedCount() == 0;
    const int simulation = _document.simulationIndex(job.simulation);
    if (job.status == Job::Status::Succeeded && simulation >= 0) {
        if (lone) showPage(simulation, job.storeFields ? Page::Fields : Page::Results);
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
                _jobList, {job.storeFields ? tr("%1 (with fields)").arg(job.simulation) : job.simulation, status, progress});
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
    _cancelJobButton->setEnabled(canCancel);
    _clearJobsButton->setEnabled(anyFinished);
}

void MainWindow::updateRunStatus() {
    const Job* job = _jobs.running();
    if (!job) {
        _progress->hide();
        _stopButton->hide();
        return;
    }
    const int queued = _jobs.queuedCount();
    QString text = tr("%1: %2").arg(job->simulation, job->stage);
    if (queued > 0) text += tr(" (%n more queued)", nullptr, queued);
    _statusLabel->setText(text);
    if (job->fraction < 0) {
        _progress->setRange(0, 0);
    } else {
        _progress->setRange(0, 1000);
        _progress->setValue(static_cast<int>(job->fraction * 1000));
    }
    _progress->show();
    _stopButton->show();
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

int MainWindow::currentSimulation() const {
    const QTreeWidgetItem* item = _sidebar->currentItem();
    return item ? item->data(0, kSimulationRole).toInt() : -1;
}

void MainWindow::rebuildSidebar() {
    _rebuildingSidebar = true;
    const int selectedSimulation = currentSimulation();
    const int selectedPage = _sidebar->currentItem() ? _sidebar->currentItem()->data(0, kPageRole).toInt() : -1;
    _sidebar->clear();
    const QColor accent = palette().color(QPalette::Highlight);
    for (int i = 0; i < _document.simulationCount(); ++i) {
        auto* top = new QTreeWidgetItem(_sidebar, {_document.simulationName(i)});
        top->setData(0, kSimulationRole, i);
        top->setData(0, kPageRole, -1);
        QFont font = top->font(0);
        font.setBold(true);
        top->setFont(0, font);
        const Job* job = _jobs.running();
        const bool running = job && job->simulation == _document.simulationName(i);
        const bool runningStoresFields = running && job->storeFields;
        if (running) top->setToolTip(0, tr("Running"));
        else if (_jobs.contains(_document.simulationName(i))) top->setToolTip(0, tr("Queued"));
        auto* board = new QTreeWidgetItem(top, {tr("Board")});
        board->setData(0, kSimulationRole, i);
        board->setData(0, kPageRole, static_cast<int>(Page::Board));
        board->setIcon(0, statusDot(_document.hasGeometry(i), running ? QColor(0xf2, 0xd0, 0x4b) : accent));
        auto* results = new QTreeWidgetItem(top, {tr("Simulation Results")});
        results->setData(0, kSimulationRole, i);
        results->setData(0, kPageRole, static_cast<int>(Page::Results));
        results->setIcon(0, statusDot(_document.hasResults(i), running ? QColor(0xf2, 0xd0, 0x4b) : accent));
        auto* fields = new QTreeWidgetItem(top, {tr("Field Viewer")});
        fields->setData(0, kSimulationRole, i);
        fields->setData(0, kPageRole, static_cast<int>(Page::Fields));
        const bool haveFields = !FieldViewer::findSeries(_document.simulationDir(i)).empty();
        fields->setIcon(0, statusDot(haveFields, runningStoresFields ? QColor(0xf2, 0xd0, 0x4b) : accent));
        top->setExpanded(true);
        if (i == selectedSimulation) {
            _sidebar->setCurrentItem(selectedPage == static_cast<int>(Page::Results)  ? results
                                     : selectedPage == static_cast<int>(Page::Fields) ? fields
                                     : selectedPage == static_cast<int>(Page::Board)  ? board
                                                                                      : top);
        }
    }
    _rebuildingSidebar = false;
}

void MainWindow::selectionChanged() {
    if (_rebuildingSidebar) return;
    const QTreeWidgetItem* item = _sidebar->currentItem();
    const int simulation = item ? item->data(0, kSimulationRole).toInt() : -1;
    const int page = item ? item->data(0, kPageRole).toInt() : -1;
    _inspector->setSimulation(simulation);
    updateHighlights();
    updateActions();
    if (page == static_cast<int>(Page::Results)) {
        _resultsView->showResults(_document.simulationName(simulation),
                                  SimulationResults::load(_document.resultsDir(simulation),
                                                          _document.simulationDir(simulation)));
        _pages->setCurrentWidget(_resultsView);
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
    for (int i = 0; i < _sidebar->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = _sidebar->topLevelItem(i);
        if (top->data(0, kSimulationRole).toInt() != simulation) continue;
        _sidebar->setCurrentItem(top->child(page == Page::Board ? 0 : page == Page::Results ? 1 : 2));
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
    const QStringList nets = _document.involvedNets(simulation);
    _boardView->setHighlightedNets(QSet<QString>(nets.begin(), nets.end()));
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
        _projectLabel->setText(tr("No document open"));
        _chooseBoardButton->setEnabled(false);
        return;
    }
    setWindowTitle(QString("%1[*] - KiEMS").arg(_document.displayName()));
    setWindowModified(_document.isModified());
    const QString board = _document.kicadPcbPath();
    _projectLabel->setText(board.isEmpty() ? tr("No KiCad board linked") : QDir::toNativeSeparators(board));
    _chooseBoardButton->setEnabled(true);
}

void MainWindow::updateActions() {
    const bool open = _document.isOpen();
    const int simulation = currentSimulation();
    const bool haveSimulation = simulation >= 0;
    // The selected simulation can't be removed while its run is queued or under way.
    const bool busy = haveSimulation && _jobs.contains(_document.simulationName(simulation));
    const bool canRun = !PipelineRunner::cliPath().isEmpty();
    _saveAction->setEnabled(open);
    _addAction->setEnabled(open);
    _addButton->setEnabled(open);
    _removeAction->setEnabled(haveSimulation && !busy);
    _removeButton->setEnabled(haveSimulation && !busy);
    _runAction->setEnabled(haveSimulation && canRun);
    _runFieldsAction->setEnabled(_runAction->isEnabled());
    _runAllAction->setEnabled(open && _document.simulationCount() > 0 && canRun);
    _stopAction->setEnabled(_jobs.isBusy());
}

} // namespace kiems_gui
