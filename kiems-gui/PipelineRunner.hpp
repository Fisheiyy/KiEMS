// Runs the simulation pipeline (geometry, FDTD, postprocessing) for one simulation by running the
// kiems command-line tool beside this executable, so the app and the CLI share one tested
// pipeline. Its log is passed on line by line and turned into a stage, a progress fraction, and an
// estimate of the time left (RunEstimator).
#pragma once

#include "RunEstimate.hpp"

#include <QElapsedTimer>
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
    /// importing the board from `kicadPcbPath` first -- or, with `geometryOnly`, just its geometry
    /// and mesh. `engineArguments` choose the FDTD engine (EngineSettings::arguments()).
    /// `previousSeconds`, if positive, is how long this simulation's last run took: the estimate of
    /// the time left until the run's own progress gives a better one.
    bool start(const QString& configPath, const QString& kicadPcbPath, const QString& simulation,
               const QStringList& engineArguments, QString* error, bool geometryOnly = false,
               double previousSeconds = -1);
    void stop();
    bool isRunning() const;
    QString simulation() const { return _simulation; }
    /// The current (or last) run's time so far and estimated time left.
    RunTiming timing() const;

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
    QElapsedTimer _clock;
    RunEstimator _estimator;
    double _fraction = -1; // the simulation stage's progress, kept from going backwards
};

} // namespace kiems_gui
