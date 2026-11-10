// A simulation's results: S-parameters, impedance, a Smith chart, trace delays, the time-domain
// response at each port and the eye diagrams (EyeView), read from the
// Port_<n>_data.csv files the pipeline writes into ems/results/<simulation>/ and labelled with the
// port names it records in ems/simulation/<simulation>/<n>/ports.txt.
#pragma once

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

    static SimulationResults load(const QString& resultsDir, const QString& simulationDir);
};

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
