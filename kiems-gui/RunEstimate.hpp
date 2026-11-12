// How long a simulation run has left, read from the kiems pipeline's log as it runs.
//
// The FDTD stage is nearly all of a run, and it runs once per excited port. Each port's run ends at
// its step limit or when the field energy has decayed by the end criterion (60 dB), whichever comes
// first. So the estimate is: the steps the current port still needs -- to its limit, or to where
// the energy decay reaches the criterion, fitted as a straight line in dB against steps over the
// latter half of the steps so far -- at the recent stepping rate; plus, for each port still to run,
// the steps the ports so far took and their setup time. Before the first steps there's nothing to go on but a
// previous run of the same simulation, if there was one.
#pragma once

#include <QString>

#include <vector>

namespace kiems_gui {

struct RunTiming {
    double elapsed = 0;    // seconds since the run started
    double remaining = -1; // seconds; negative while there's nothing to go on
    int port = 0;          // the excited port being simulated, from 1; 0 before the FDTD stage
    int ports = 0;         // how many ports the simulation excites; 0 if it hasn't said yet
    /// Whether the current port is expected to stop at the energy criterion rather than its step limit.
    bool decayLimited = false;
    /// The FDTD stage's progress over all its ports, in [0, 1]; negative before there's an estimate.
    double simulatingFraction = -1;
};

class RunEstimator {
public:
    /// Starts timing a run, `now` seconds into some clock. `previousSeconds`, if positive, is how long
    /// the last run of the same simulation took: the estimate until the steps give a better one.
    void start(double now, double previousSeconds = -1);
    /// Reads one line of the log, logged at `now`. Returns whether it moved the estimate.
    bool feed(const QString& line, double now);
    /// The estimate at `now`, counting down from the last one between log lines.
    RunTiming timing(double now) const;

    /// The energy decay a port's run stops at (Copper's end criterion, 1e-6).
    static constexpr double kTargetDecayDB = 60;

private:
    struct Sample {
        double time = 0;
        double step = 0;
        double decay = -1; // dB; negative when the line didn't say
    };
    void addSample(double now, double step, double total, double decay, double target);
    void update(double now);
    double rate() const;
    double predictedEndStep(bool* decayLimited) const;

    double _started = 0;
    double _previous = -1;
    int _announcedPorts = 0;
    int _port = -1; // index of the port being stepped
    double _simulatingSince = -1;
    double _portSetupFrom = -1; // when the current port's setup began
    double _total = 0;
    double _target = kTargetDecayDB;
    std::vector<Sample> _samples; // the current port's
    std::vector<double> _finishedSteps;
    std::vector<double> _setupSeconds;
    double _lastRate = 0;
    bool _postprocessing = false;
    // The estimate at the last update.
    double _estimateAt = -1;
    double _estimate = -1;
    bool _decayLimited = false;
};

/// "42 s", "3:07" or "1:02:07".
QString formatDuration(double seconds);

} // namespace kiems_gui
