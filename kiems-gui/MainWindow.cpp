#include "MainWindow.hpp"

#include "BoardView.hpp"
#include "Inspector.hpp"
#include "ResultsView.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
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
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTreeWidget>
#include <QVBoxLayout>

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
    _boardPage = new QWidget;
    auto* boardLayout = new QHBoxLayout(_boardPage);
    boardLayout->setContentsMargins(0, 0, 0, 0);
    _layerList = new QListWidget;
    _layerList->setMaximumWidth(170);
    _boardView = new BoardView;
    boardLayout->addWidget(_layerList);
    boardLayout->addWidget(_boardView, 1);
    _resultsView = new ResultsView;
    _pages->addWidget(_emptyPage);
    _pages->addWidget(_boardPage);
    _pages->addWidget(_resultsView);

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
    _statusLabel = new QLabel;
    _progress = new QProgressBar;
    _progress->setMaximumWidth(260);
    _progress->setTextVisible(false);
    _progress->hide();
    _stopButton = new QPushButton(tr("Stop"));
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
        _inspector->setBoard(_boardModel.data());
        populateLayerList();
        updateHighlights();
    });
    connect(&_boardModel, &BoardModel::failed, this, [this](const QString& error) {
        _statusLabel->setText(tr("The board couldn't be loaded."));
        QMessageBox::warning(this, tr("KiEMS"), tr("The KiCad board couldn't be loaded:\n\n%1").arg(error));
    });

    connect(&_runner, &PipelineRunner::line, this, [this](const QString& text) { _log->appendPlainText(text); });
    connect(&_runner, &PipelineRunner::progress, this, [this](const QString& stage, double fraction) {
        _statusLabel->setText(tr("%1: %2").arg(_runningSimulation, stage));
        if (fraction < 0) _progress->setRange(0, 0);
        else {
            _progress->setRange(0, 1000);
            _progress->setValue(static_cast<int>(fraction * 1000));
        }
    });
    connect(&_runner, &PipelineRunner::finished, this, [this](bool succeeded, const QString& message) {
        _progress->hide();
        _stopButton->hide();
        _statusLabel->setText(succeeded ? tr("%1 finished.").arg(_runningSimulation)
                                        : tr("%1: %2").arg(_runningSimulation, message));
        const int simulation = _document.simulationIndex(_runningSimulation);
        _runningSimulation.clear();
        rebuildSidebar();
        updateActions();
        if (succeeded && simulation >= 0) showPage(simulation, Page::Results);
        else if (!succeeded && message != tr("Stopped.")) {
            QMessageBox::warning(this, tr("KiEMS"), tr("The simulation failed:\n\n%1").arg(message));
        }
    });

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
    file->addSeparator();
    file->addAction(tr("Change &Board..."), this, &MainWindow::chooseBoard);
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* simulation = menuBar()->addMenu(tr("&Simulation"));
    _addAction = simulation->addAction(tr("&Add Simulation..."), this, &MainWindow::addSimulation);
    _removeAction = simulation->addAction(tr("&Remove Simulation"), this, &MainWindow::removeSimulation);
    simulation->addSeparator();
    _runAction = simulation->addAction(tr("&Run"), QKeySequence(Qt::CTRL | Qt::Key_R), this, &MainWindow::runSelected);
    _stopAction = simulation->addAction(tr("S&top"), QKeySequence(Qt::CTRL | Qt::Key_Period), this, &MainWindow::stopRun);
    simulation->addSeparator();
    simulation->addAction(tr("&Engine..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), this,
                          &MainWindow::showEngineSettings);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("&Fit Board"), QKeySequence(Qt::CTRL | Qt::Key_0), this, [this] { _boardView->fitToBoard(); });
    view->addAction(_logDock->toggleViewAction());
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

void MainWindow::closeEvent(QCloseEvent* event) {
    if (_runner.isRunning()) {
        if (QMessageBox::question(this, tr("KiEMS"), tr("A simulation is running. Stop it and quit?")) !=
            QMessageBox::Yes) {
            event->ignore();
            return;
        }
        _runner.stop();
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
    const int simulation = currentSimulation();
    if (simulation < 0 || _runner.isRunning()) return;
    if (const QString problem = _document.validate(); !problem.isEmpty()) {
        QMessageBox::warning(this, tr("KiEMS"), tr("The simulation can't run yet:\n\n%1").arg(problem));
        return;
    }
    if (!_document.mainExcitation(simulation)) {
        QMessageBox::information(this, tr("KiEMS"),
                                 tr("Pick the pin to drive on the board, and tick \"Excited\" in the Selection tab."));
        return;
    }
    // The pipeline reads the document from disk.
    if (_document.isModified() && !saveDocument()) return;
    QString error;
    _runningSimulation = _document.simulationName(simulation);
    _log->clear();
    _log->appendPlainText(tr("Engine: %1").arg(_engine.describe(_gpuDevices)));
    if (!_runner.start(_document.configPath(), _document.kicadPcbPath(), _runningSimulation,
                       _engine.arguments(_gpuDevices), &error)) {
        _runningSimulation.clear();
        QMessageBox::warning(this, tr("KiEMS"), error);
        return;
    }
    _logDock->show();
    _progress->setRange(0, 0);
    _progress->show();
    _stopButton->show();
    updateActions();
}

void MainWindow::stopRun() {
    _runner.stop();
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
        const bool running = _document.simulationName(i) == _runningSimulation;
        auto* board = new QTreeWidgetItem(top, {tr("Board")});
        board->setData(0, kSimulationRole, i);
        board->setData(0, kPageRole, static_cast<int>(Page::Board));
        board->setIcon(0, statusDot(_document.hasGeometry(i), running ? QColor(0xf2, 0xd0, 0x4b) : accent));
        auto* results = new QTreeWidgetItem(top, {tr("Simulation Results")});
        results->setData(0, kSimulationRole, i);
        results->setData(0, kPageRole, static_cast<int>(Page::Results));
        results->setIcon(0, statusDot(_document.hasResults(i), running ? QColor(0xf2, 0xd0, 0x4b) : accent));
        top->setExpanded(true);
        if (i == selectedSimulation) {
            _sidebar->setCurrentItem(selectedPage == static_cast<int>(Page::Results) ? results
                                     : selectedPage == static_cast<int>(Page::Board) ? board
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
    } else if (_document.isOpen()) {
        _pages->setCurrentWidget(_boardPage);
        _inspector->show();
    }
}

void MainWindow::showPage(int simulation, Page page) {
    for (int i = 0; i < _sidebar->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = _sidebar->topLevelItem(i);
        if (top->data(0, kSimulationRole).toInt() != simulation) continue;
        _sidebar->setCurrentItem(top->child(page == Page::Board ? 0 : 1));
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
    const bool running = _runner.isRunning();
    const bool haveSimulation = currentSimulation() >= 0;
    _saveAction->setEnabled(open);
    _addAction->setEnabled(open);
    _addButton->setEnabled(open);
    _removeAction->setEnabled(haveSimulation && !running);
    _removeButton->setEnabled(haveSimulation && !running);
    _runAction->setEnabled(haveSimulation && !running && !PipelineRunner::cliPath().isEmpty());
    _stopAction->setEnabled(running);
}

} // namespace kiems_gui
