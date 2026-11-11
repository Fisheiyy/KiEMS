// A simulation's results: S-parameters, impedance, a Smith chart, trace delays, the time-domain
// response at each port, the probed pins and the eye diagrams (EyeView), read from the
// Port_<n>_data.csv files the pipeline writes into ems/results/<simulation>/ and the
// Probe<n>.csv files in ems/simulation/<simulation>/, and labelled with the port names it records
// in ems/simulation/<simulation>/<n>/ports.txt.
#pragma once

#include "ChartStyle.hpp"

#include <QList>
#include <QString>
#include <QWidget>

#include <complex>
#include <map>
#include <vector>

class QLabel;
class QTabWidget;

namespace kiems_gui {

struct PortResults {
    int excitedPort = 0;
    std::vector<double> frequencyHz;
    std::map<int, std::vector<std::complex<double>>> s; // receiving port -> S(received, excited)
    std::map<int, std::vector<double>> delay;           // receiving port -> delay (s)
    std::vector<std::complex<double>> impedance;        // at the excited port
};

/// A passive (non-absorbing) probe's measurements, from its Probe<n>.csv: per excited port that
/// reached it, the probe's voltage per unit incident voltage (Postprocessor::getProbeTransfer()),
/// and, for a trace-impedance probe, the impedance it measured.
struct ProbeResults {
    int port = 0;
    std::vector<double> frequencyHz;
    std::map<int, std::vector<std::complex<double>>> transfer;  // excited port -> V / V_inc
    std::map<int, std::vector<std::complex<double>>> impedance; // excited port -> Z (ohm)
};

/// The voltage over time at one port with every excitation driving it together, from the run's
/// excitations/<port>_response.csv.
struct TimeResponse {
    QString name;
    std::vector<double> timeSeconds;
    std::vector<double> volts;
};

struct SimulationResults {
    QString error;
    QString resultsDir;
    std::map<int, QString> portNames;
    std::vector<PortResults> ports;
    std::vector<TimeResponse> responses;
    std::vector<ProbeResults> probes;

    static SimulationResults load(const QString& resultsDir, const QString& simulationDir);
};

/// Charts under one heading: an excited port's S-parameters, a probe's measurements.
struct ResultsSection {
    QString title; ///< empty for none
    QList<QWidget*> graphs;
};

/// What one of the Results view's tabs shows.
struct ResultsCategory {
    QString title;
    QList<ResultsSection> sections;
    /// Shown in place of the sections when there are none.
    QString emptyMessage;
    /// The tab is its one graph, filling the page (the Smith chart), rather than a scrolling column.
    bool fill = false;
};

/// The charts of every Results tab but Eyes, newly built in `style`; the caller owns the widgets.
/// Shared by the Results view and the PDF report, so both show the same things.
QList<ResultsCategory> makeResultsCategories(const SimulationResults& results, ChartStyle style);

class ResultsView : public QWidget {
    Q_OBJECT

public:
    explicit ResultsView(QWidget* parent = nullptr);
    void showResults(const QString& title, const SimulationResults& results);

private:
    QLabel* _title;
    QTabWidget* _tabs;
};

} // namespace kiems_gui
