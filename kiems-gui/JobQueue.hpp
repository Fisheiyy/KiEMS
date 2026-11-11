// The window's queue of simulation runs, the counterpart of the macOS app's JobScheduler: Run and
// Run All add jobs, one runs at a time (an FDTD run already has the GPU to itself), and the rest
// wait their turn. The queue only keeps order and status; starting and stopping a run are hooks,
// which MainWindow points at its PipelineRunner and which reports back through reportProgress()
// and reportFinished(). Finished jobs stay listed, with their outcome, until cleared.
#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include <functional>

namespace kiems_gui {

struct Job {
    enum class Status { Queued, Running, Succeeded, Failed, Stopped };

    int id = 0;
    QString simulation;
    bool storeFields = false;
    /// Builds only the simulation's geometry (and its mesh), for the Geometry page.
    bool geometryOnly = false;
    Status status = Status::Queued;
    QString stage;
    /// In [0, 1], or negative while the stage gives no measure of progress.
    double fraction = -1;
    /// Why the job failed or stopped; empty otherwise.
    QString message;

    bool finished() const { return status != Status::Queued && status != Status::Running; }
};

class JobQueue : public QObject {
    Q_OBJECT

public:
    struct Hooks {
        /// Starts `job`'s run; false, with a reason, fails the job and moves on to the next.
        std::function<bool(const Job& job, QString* error)> start;
        /// Asks the running job to stop; it ends when reportFinished() is called.
        std::function<void()> stop;
    };

    explicit JobQueue(QObject* parent = nullptr);

    void setHooks(Hooks hooks) { _hooks = std::move(hooks); }

    /// Queues a run of `simulation` and starts it if nothing is running. A simulation that's
    /// already queued isn't queued twice; asking for its fields as well, or for a full run where only
    /// its geometry was queued, upgrades the queued job. Returns false if it was already queued or
    /// running (a running job isn't restarted).
    bool enqueue(const QString& simulation, bool storeFields, bool geometryOnly = false);
    /// Removes a queued job, or stops the running one.
    void cancel(int id);
    /// Empties the queue and stops the running job.
    void stopAll();
    /// Forgets the jobs that have finished.
    void clearFinished();
    /// Follows a simulation's rename, so its queued job still finds it. A running job keeps the
    /// name its run was started under.
    void renameSimulation(const QString& from, const QString& to);

    void reportProgress(const QString& stage, double fraction);
    void reportFinished(bool succeeded, const QString& message);

    const QList<Job>& jobs() const { return _jobs; }
    const Job* running() const;
    int queuedCount() const;
    /// Whether `simulation` is queued or running.
    bool contains(const QString& simulation) const;
    bool isBusy() const { return running() || queuedCount() > 0; }

signals:
    void changed();
    void jobStarted(const Job& job);
    void jobFinished(const Job& job);
    /// The last queued job has finished.
    void drained();

private:
    void startNext();
    Job* find(int id);

    Hooks _hooks;
    QList<Job> _jobs;
    int _nextId = 1;
    bool _starting = false;
};

QString statusText(Job::Status status);

} // namespace kiems_gui
