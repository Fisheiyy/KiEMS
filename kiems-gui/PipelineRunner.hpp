// Runs the simulation pipeline (geometry, FDTD, postprocessing) for one simulation by running the
// kiems command-line tool beside this executable, so the app and the CLI share one tested
// pipeline. Its log is passed on line by line and turned into a stage and a progress fraction.
#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace kiems_gui {

class PipelineRunner : public QObject {
    Q_OBJECT

public:
    explicit PipelineRunner(QObject* parent = nullptr);
    ~PipelineRunner() override;

    /// The kiems executable this runner uses; empty if it isn't beside the app.
    static QString cliPath();

    /// Runs every stage for `simulation` of the document whose simulation.json is `configPath`,
    /// importing the board from `kicadPcbPath` first. `engineArguments` choose the FDTD engine
    /// (EngineSettings::arguments()).
    bool start(const QString& configPath, const QString& kicadPcbPath, const QString& simulation,
               const QStringList& engineArguments, QString* error);
    void stop();
    bool isRunning() const;
    QString simulation() const { return _simulation; }

signals:
    void line(const QString& text);
    /// `fraction` is in [0, 1], or negative when the stage gives no measure of progress.
    void progress(const QString& stage, double fraction);
    void finished(bool succeeded, const QString& message);

private:
    void readOutput();
    void handleLine(const QString& text);

    QProcess* _process = nullptr;
    QString _simulation;
    QString _lastError;
    QByteArray _pending;
    bool _stopping = false;
};

} // namespace kiems_gui
