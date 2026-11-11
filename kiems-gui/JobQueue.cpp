#include "JobQueue.hpp"

#include <algorithm>

namespace kiems_gui {

namespace {
// Finished jobs kept for the Jobs list; older ones are dropped first.
constexpr int kFinishedKept = 50;
} // namespace

QString statusText(Job::Status status) {
    switch (status) {
    case Job::Status::Queued: return QObject::tr("Queued");
    case Job::Status::Running: return QObject::tr("Running");
    case Job::Status::Succeeded: return QObject::tr("Finished");
    case Job::Status::Failed: return QObject::tr("Failed");
    case Job::Status::Stopped: return QObject::tr("Stopped");
    }
    return {};
}

JobQueue::JobQueue(QObject* parent) : QObject(parent) {}

const Job* JobQueue::running() const {
    for (const Job& job : _jobs) {
        if (job.status == Job::Status::Running) return &job;
    }
    return nullptr;
}

Job* JobQueue::find(int id) {
    for (Job& job : _jobs) {
        if (job.id == id) return &job;
    }
    return nullptr;
}

int JobQueue::queuedCount() const {
    return static_cast<int>(
        std::count_if(_jobs.begin(), _jobs.end(), [](const Job& job) { return job.status == Job::Status::Queued; }));
}

bool JobQueue::contains(const QString& simulation) const {
    return std::any_of(_jobs.begin(), _jobs.end(),
                       [&](const Job& job) { return !job.finished() && job.simulation == simulation; });
}

bool JobQueue::enqueue(const QString& simulation, bool storeFields, bool geometryOnly) {
    for (Job& job : _jobs) {
        if (job.finished() || job.simulation != simulation) continue;
        if (job.status == Job::Status::Queued) {
            // A full run builds the geometry too.
            const bool upgrade = (storeFields && !job.storeFields) || (!geometryOnly && job.geometryOnly);
            job.storeFields = job.storeFields || storeFields;
            job.geometryOnly = job.geometryOnly && geometryOnly;
            if (upgrade) emit changed();
        }
        return false;
    }
    Job job;
    job.id = _nextId++;
    job.simulation = simulation;
    job.storeFields = storeFields && !geometryOnly;
    job.geometryOnly = geometryOnly;
    _jobs.append(job);
    emit changed();
    if (!running()) startNext();
    return true;
}

void JobQueue::cancel(int id) {
    Job* job = find(id);
    if (!job) return;
    if (job->status == Job::Status::Queued) {
        _jobs.removeIf([id](const Job& j) { return j.id == id; });
        emit changed();
        if (!isBusy()) emit drained();
    } else if (job->status == Job::Status::Running && _hooks.stop) {
        _hooks.stop();
    }
}

void JobQueue::stopAll() {
    const bool hadQueued = queuedCount() > 0;
    _jobs.removeIf([](const Job& job) { return job.status == Job::Status::Queued; });
    if (hadQueued) emit changed();
    if (running()) {
        if (_hooks.stop) _hooks.stop();
    } else if (hadQueued) {
        emit drained();
    }
}

void JobQueue::clearFinished() {
    const auto removed = _jobs.removeIf([](const Job& job) { return job.finished(); });
    if (removed > 0) emit changed();
}

void JobQueue::renameSimulation(const QString& from, const QString& to) {
    bool renamed = false;
    for (Job& job : _jobs) {
        if (job.simulation == from && job.status == Job::Status::Queued) {
            job.simulation = to;
            renamed = true;
        }
    }
    if (renamed) emit changed();
}

void JobQueue::reportProgress(const QString& stage, double fraction) {
    for (Job& job : _jobs) {
        if (job.status != Job::Status::Running) continue;
        job.stage = stage;
        job.fraction = fraction;
        emit changed();
        return;
    }
}

void JobQueue::reportFinished(bool succeeded, const QString& message) {
    for (Job& job : _jobs) {
        if (job.status != Job::Status::Running) continue;
        job.status = succeeded               ? Job::Status::Succeeded
                     : message == tr("Stopped.") ? Job::Status::Stopped
                                                 : Job::Status::Failed;
        job.message = message;
        job.fraction = succeeded ? 1 : job.fraction;
        const Job finished = job;
        emit changed();
        emit jobFinished(finished);
        break;
    }
    // Keep the list from growing without bound over a long session.
    int finishedCount = static_cast<int>(std::count_if(_jobs.begin(), _jobs.end(), [](const Job& j) { return j.finished(); }));
    for (auto it = _jobs.begin(); it != _jobs.end() && finishedCount > kFinishedKept;) {
        if (it->finished()) {
            it = _jobs.erase(it);
            --finishedCount;
        } else {
            ++it;
        }
    }
    startNext();
}

void JobQueue::startNext() {
    // A start hook that fails reports nothing back, so this loop moves on by itself; one that
    // succeeds may report progress -- or even its end -- before returning, which needs the job
    // already marked running, and the loop to look again rather than recurse.
    if (_starting) return;
    _starting = true;
    while (!running()) {
        auto next = std::find_if(_jobs.begin(), _jobs.end(), [](const Job& job) { return job.status == Job::Status::Queued; });
        if (next == _jobs.end()) {
            _starting = false;
            emit drained();
            return;
        }
        next->status = Job::Status::Running;
        next->stage = tr("Starting");
        next->fraction = -1;
        const int id = next->id;
        const Job started = *next;
        emit changed();
        emit jobStarted(started);
        QString error;
        if (_hooks.start && _hooks.start(started, &error)) continue;
        if (Job* job = find(id); job && job->status == Job::Status::Running) {
            job->status = Job::Status::Failed;
            job->message = error.isEmpty() ? tr("The run couldn't start.") : error;
            const Job failed = *job;
            emit changed();
            emit jobFinished(failed);
        }
    }
    _starting = false;
}

} // namespace kiems_gui
