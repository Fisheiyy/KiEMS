#include "PipelineRunner.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

#if !defined(Q_OS_WIN)
#include <csignal>
#include <unistd.h>
#endif

namespace kiems_gui {

PipelineRunner::PipelineRunner(QObject* parent) : QObject(parent) {}

PipelineRunner::~PipelineRunner() {
    stop();
}

QString PipelineRunner::cliPath() {
#if defined(Q_OS_WIN)
    const QString name = "kiems.exe";
#else
    const QString name = "kiems";
#endif
    const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(name);
    return QFileInfo::exists(path) ? path : QString();
}

bool PipelineRunner::start(const QString& configPath, const QString& kicadPcbPath, const QString& simulation,
                           const QStringList& engineArguments, QString* error, bool geometryOnly,
                           double previousSeconds) {
    if (isRunning()) {
        if (error) *error = tr("A simulation is already running.");
        return false;
    }
    const QString cli = cliPath();
    if (cli.isEmpty()) {
        if (error) *error = tr("The kiems command-line tool wasn't found next to this application.");
        return false;
    }
    _simulation = simulation;
    _lastError.clear();
    _pending.clear();
    _stopping = false;
    _fraction = -1;
    _clock.start();
    _estimator.start(0, previousSeconds);
    _process = new QProcess(this);
    _process->setProgram(cli);
    QStringList arguments{"-c", configPath, "--simulation", simulation, geometryOnly ? "--geometry" : "--all"};
    if (!kicadPcbPath.isEmpty()) arguments << "--input" << kicadPcbPath;
    arguments << engineArguments;
    _process->setArguments(arguments);
    _process->setWorkingDirectory(QFileInfo(configPath).absolutePath());
    _process->setProcessChannelMode(QProcess::MergedChannels);
#if !defined(Q_OS_WIN)
    // Its own process group, so stop() reaches the FDTD worker processes it starts too.
    _process->setChildProcessModifier([] { ::setpgid(0, 0); });
#endif
    connect(_process, &QProcess::readyReadStandardOutput, this, &PipelineRunner::readOutput);
    connect(_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        readOutput();
        if (!_pending.isEmpty()) handleLine(QString::fromUtf8(_pending));
        _pending.clear();
        const bool ok = status == QProcess::NormalExit && exitCode == 0;
        QString message;
        if (_stopping) message = tr("Stopped.");
        else if (!ok) message = _lastError.isEmpty() ? tr("kiems exited with status %1.").arg(exitCode) : _lastError;
        _process->deleteLater();
        _process = nullptr;
        emit finished(ok && !_stopping, message);
    });
    _process->start();
    if (!_process->waitForStarted(10000)) {
        if (error) *error = _process->errorString();
        _process->deleteLater();
        _process = nullptr;
        return false;
    }
    emit progress(tr("Starting"), -1);
    return true;
}

RunTiming PipelineRunner::timing() const {
    return _estimator.timing(_clock.isValid() ? _clock.elapsed() / 1000.0 : 0.0);
}

bool PipelineRunner::isRunning() const {
    return _process && _process->state() != QProcess::NotRunning;
}

void PipelineRunner::stop() {
    if (!isRunning()) return;
    _stopping = true;
    QProcess* process = _process; // finished() clears _process (deleteLater keeps it alive here).
    const qint64 pid = process->processId();
#if defined(Q_OS_WIN)
    // The pipeline starts FDTD worker processes of its own; end the whole tree.
    QProcess::execute("taskkill", {"/PID", QString::number(pid), "/T", "/F"});
#else
    ::kill(-static_cast<pid_t>(pid), SIGTERM);
#endif
    if (!process->waitForFinished(5000)) process->kill();
}

void PipelineRunner::readOutput() {
    if (!_process) return;
    _pending += _process->readAllStandardOutput();
    qsizetype newline;
    while ((newline = _pending.indexOf('\n')) >= 0) {
        QByteArray raw = _pending.left(newline);
        _pending.remove(0, newline + 1);
        if (raw.endsWith('\r')) raw.chop(1);
        handleLine(QString::fromUtf8(raw));
    }
}

void PipelineRunner::handleLine(const QString& rawText) {
    static const QRegularExpression ansi(QStringLiteral("\x1b\\[[0-9;]*m"));
    static const QRegularExpression timestep(QStringLiteral(R"(timestep (\d+)/(\d+))"));
    static const QRegularExpression stageStep(QStringLiteral(R"(\[(Geometry|Simulation|Postprocess\w*)\] \[[^\]]*? (\d+)/(\d+)\])"));
    QString text = rawText;
    text.remove(ansi);
    emit line(text);

    if (text.contains("[ERROR]") || text.startsWith("terminating due to")) {
        // The message itself, without the log prefix or source location.
        QString message = text.section("] ", 1);
        message = message.section(" (", 0, 0).trimmed();
        _lastError = message.isEmpty() ? text : message;
    }
    const double now = _clock.isValid() ? _clock.elapsed() / 1000.0 : 0.0;
    _estimator.feed(text, now);
    if (auto match = timestep.match(text); match.hasMatch()) {
        // The whole simulation stage's progress, over every port, once there's an estimate to
        // measure it by; until then this port's steps.
        const RunTiming timing = _estimator.timing(now);
        const double done = match.captured(1).toDouble();
        const double total = match.captured(2).toDouble();
        double fraction = timing.simulatingFraction >= 0 ? timing.simulatingFraction
                          : total > 0 && timing.ports <= 1 ? done / total
                                                           : -1;
        // Never backwards, nor back to "no measure" between ports.
        if (fraction >= 0 || _fraction >= 0) {
            _fraction = std::max(_fraction, fraction);
            fraction = _fraction;
        }
        emit progress(tr("Simulating"), fraction);
    } else if (auto stage = stageStep.match(text); stage.hasMatch()) {
        emit progress(stage.captured(1) == "Geometry" ? tr("Building geometry") : stage.captured(1), -1);
    } else if (text.contains("Creating geometry")) {
        emit progress(tr("Building geometry"), -1);
    } else if (text.contains("Running simulation")) {
        emit progress(tr("Simulating"), 0);
    } else if (text.contains("Postprocessing") || text.contains("Rendering")) {
        emit progress(tr("Processing results"), -1);
    }
}

} // namespace kiems_gui
