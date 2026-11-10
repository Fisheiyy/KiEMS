#include "EngineSettings.hpp"

#include "PipelineRunner.hpp"

#include <algorithm>

#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

namespace kiems_gui {

namespace {

constexpr int kListTimeoutMs = 30000;

const QString kModeKey = QStringLiteral("engine/mode");
const QString kDeviceKey = QStringLiteral("engine/gpuDevice");
const QString kDeviceNameKey = QStringLiteral("engine/gpuDeviceName");

QString dash() { return QString::fromUtf8(" \xE2\x80\x94 "); }

QString describeDevice(const GpuDevice& device) {
    QStringList details;
    if (!device.kind.isEmpty() && device.kind != "other") details << QObject::tr("%1 GPU").arg(device.kind);
    if (device.memoryBytes != 0) {
        const QString size = QLocale().formattedDataSize(static_cast<qint64>(device.memoryBytes), 1,
                                                         QLocale::DataSizeTraditionalFormat);
        // An integrated GPU's "device-local" heap is system memory it shares with the CPU.
        details << (device.kind == "integrated" ? QObject::tr("%1 shared").arg(size) : size);
    }
    if (!device.api.isEmpty()) details << device.api;
    return details.join(", ");
}

} // namespace

// ---------------------------------------------------------------------------------------------
// GpuDeviceList

GpuDeviceList::GpuDeviceList(QObject* parent) : QObject(parent) {}

GpuDeviceList::~GpuDeviceList() {
    if (_process) {
        _process->disconnect(this);
        _process->kill();
        _process->waitForFinished(2000);
    }
}

void GpuDeviceList::refresh() {
    if (_process) return;
    const QString cli = PipelineRunner::cliPath();
    if (cli.isEmpty()) {
        _devices.clear();
        _error = tr("The kiems command-line tool wasn't found next to this application.");
        _loaded = true;
        emit changed();
        return;
    }
    _process = new QProcess(this);
    _process->setProgram(cli);
    _process->setArguments({"--list-gpus"});
    connect(_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit && exitCode == 0);
    });
    connect(_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(false);
    });
    // A wedged GPU driver shouldn't leave the list loading forever.
    QTimer::singleShot(kListTimeoutMs, _process, [process = _process] { process->kill(); });
    _process->start();
    emit changed();
}

void GpuDeviceList::finish(bool succeeded) {
    if (!_process) return;
    QProcess* process = _process;
    _process = nullptr;
    process->deleteLater();
    const QByteArray output = process->readAllStandardOutput();
    const QString diagnostics = QString::fromUtf8(process->readAllStandardError()).trimmed();

    _devices.clear();
    _error.clear();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(output, &parseError);
    if (!succeeded || !document.isArray()) {
        _error = diagnostics.isEmpty() ? tr("kiems --list-gpus failed: %1").arg(process->errorString()) : diagnostics;
    } else {
        for (const QJsonValue& value : document.array()) {
            const QJsonObject object = value.toObject();
            GpuDevice device;
            device.index = object.value("index").toInt();
            device.name = object.value("name").toString();
            device.kind = object.value("kind").toString();
            device.api = object.value("api").toString();
            device.memoryBytes = static_cast<quint64>(object.value("memory_bytes").toDouble());
            device.usable = object.value("usable").toBool();
            device.problem = object.value("problem").toString();
            device.isDefault = object.value("default").toBool();
            _devices.push_back(device);
        }
        if (_devices.isEmpty()) {
            _error = diagnostics.isEmpty() ? tr("No GPU was found.") : diagnostics;
        }
    }
    _loaded = true;
    emit changed();
}

const GpuDevice* GpuDeviceList::defaultDevice() const {
    for (const GpuDevice& device : _devices) {
        if (device.isDefault && device.usable) return &device;
    }
    return nullptr;
}

const GpuDevice* GpuDeviceList::find(const QString& key) const {
    if (key.isEmpty()) return defaultDevice();
    for (const GpuDevice& device : _devices) {
        if (keyFor(device) == key) return &device;
    }
    return nullptr;
}

QString GpuDeviceList::keyFor(const GpuDevice& device) const {
    const auto sameName = std::count_if(_devices.begin(), _devices.end(), [&](const GpuDevice& other) {
        return other.name.compare(device.name, Qt::CaseInsensitive) == 0;
    });
    return sameName > 1 ? QString::number(device.index) : device.name;
}

// ---------------------------------------------------------------------------------------------
// EngineSettings

EngineSettings EngineSettings::load() {
    QSettings store;
    EngineSettings settings;
    const QString mode = store.value(kModeKey).toString();
    settings.mode = mode == "cpu" ? Mode::CPU : mode == "gpu" ? Mode::GPU : Mode::Automatic;
    settings.gpuDevice = store.value(kDeviceKey).toString();
    settings.gpuDeviceName = store.value(kDeviceNameKey).toString();
    return settings;
}

void EngineSettings::save() const {
    QSettings store;
    store.setValue(kModeKey, mode == Mode::CPU ? "cpu" : mode == Mode::GPU ? "gpu" : "automatic");
    store.setValue(kDeviceKey, gpuDevice);
    store.setValue(kDeviceNameKey, gpuDeviceName);
}

bool EngineSettings::usesGPU(const GpuDeviceList& devices) const {
    switch (mode) {
    case Mode::CPU: return false;
    case Mode::GPU: return true;
    case Mode::Automatic: return devices.hasUsableDevice();
    }
    return false;
}

QStringList EngineSettings::arguments(const GpuDeviceList& devices) const {
    if (!usesGPU(devices)) return {"--backend", "cpu"};
    QStringList arguments{"--backend", "gpu"};
    // A saved GPU that has gone (unplugged, driver change) gives way to the engine's own choice
    // rather than sending the run to the CPU.
    const bool missing = devices.isLoaded() && devices.find(gpuDevice) == nullptr;
    if (!gpuDevice.isEmpty() && !missing) arguments << "--gpu-device" << gpuDevice;
    return arguments;
}

QString EngineSettings::describe(const GpuDeviceList& devices) const {
    if (mode == Mode::CPU) return QObject::tr("CPU");
    if (mode == Mode::Automatic && !devices.isLoaded()) return QObject::tr("Automatic");
    if (devices.isLoaded() && !devices.hasUsableDevice()) return QObject::tr("CPU (no usable GPU)");
    if (const GpuDevice* device = devices.find(gpuDevice)) return QObject::tr("GPU: %1").arg(device->name);
    if (!devices.isLoaded()) return gpuDeviceName.isEmpty() ? QObject::tr("GPU") : QObject::tr("GPU: %1").arg(gpuDeviceName);
    // The saved GPU is gone; arguments() lets the engine choose.
    const GpuDevice* fallback = devices.defaultDevice();
    return fallback ? QObject::tr("GPU: %1 (%2 not found)").arg(fallback->name, gpuDeviceName.isEmpty() ? gpuDevice : gpuDeviceName)
                    : QObject::tr("GPU");
}

// ---------------------------------------------------------------------------------------------
// EngineSettingsDialog

EngineSettingsDialog::EngineSettingsDialog(GpuDeviceList* devices, const EngineSettings& settings, QWidget* parent)
    : QDialog(parent), _devices(devices), _initial(settings) {
    setWindowTitle(tr("Simulation Engine"));

    auto* engineBox = new QGroupBox(tr("Run simulations on"));
    auto* engineLayout = new QVBoxLayout(engineBox);
    _modes = new QButtonGroup(this);
    const struct {
        EngineSettings::Mode mode;
        QString label;
    } modes[] = {
        {EngineSettings::Mode::Automatic, tr("Automatic") + dash() + tr("the GPU when there is one, otherwise the CPU")},
        {EngineSettings::Mode::GPU, tr("GPU")},
        {EngineSettings::Mode::CPU, tr("CPU")},
    };
    for (const auto& mode : modes) {
        auto* button = new QRadioButton(mode.label);
        _modes->addButton(button, static_cast<int>(mode.mode));
        engineLayout->addWidget(button);
        if (mode.mode == settings.mode) button->setChecked(true);
    }

    auto* gpuBox = new QGroupBox(tr("GPU"));
    auto* gpuLayout = new QVBoxLayout(gpuBox);
    auto* deviceRow = new QHBoxLayout;
    _device = new QComboBox;
    _device->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    _device->setMinimumContentsLength(32);
    _refresh = new QPushButton(tr("Refresh"));
    deviceRow->addWidget(_device, 1);
    deviceRow->addWidget(_refresh);
    _deviceInfo = new QLabel;
    _deviceInfo->setWordWrap(true);
    _deviceInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* note = new QLabel(tr("A GPU run that can't start on the chosen GPU runs on the CPU instead, and says so "
                               "in the log."));
    note->setWordWrap(true);
    note->setEnabled(false);
    gpuLayout->addLayout(deviceRow);
    gpuLayout->addWidget(_deviceInfo);
    gpuLayout->addWidget(note);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(engineBox);
    layout->addWidget(gpuBox);
    layout->addStretch();
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(_modes, &QButtonGroup::idClicked, this, &EngineSettingsDialog::updateState);
    connect(_device, &QComboBox::currentIndexChanged, this, &EngineSettingsDialog::updateState);
    connect(_refresh, &QPushButton::clicked, _devices, &GpuDeviceList::refresh);
    connect(_devices, &GpuDeviceList::changed, this, &EngineSettingsDialog::populateDevices);

    populateDevices();
    if (!_devices->isLoaded() && !_devices->isLoading()) _devices->refresh();
}

void EngineSettingsDialog::populateDevices() {
    // Keep the current choice across a refresh.
    const QString selected = _device->count() > 0 ? _device->currentData().toString() : _initial.gpuDevice;
    QString selectedName = _initial.gpuDeviceName;
    if (_device->count() > 0) selectedName = _device->currentData(Qt::UserRole + 1).toString();

    QSignalBlocker blocker(_device);
    _device->clear();
    auto* model = qobject_cast<QStandardItemModel*>(_device->model());

    const GpuDevice* automatic = _devices->defaultDevice();
    _device->addItem(automatic ? tr("Automatic (%1)").arg(automatic->name) : tr("Automatic"), QString());
    for (const GpuDevice& device : _devices->devices()) {
        _device->addItem(device.name, _devices->keyFor(device));
        _device->setItemData(_device->count() - 1, device.name, Qt::UserRole + 1);
        if (!device.usable) {
            _device->setItemText(_device->count() - 1, tr("%1 (can't be used)").arg(device.name));
            if (model) model->item(_device->count() - 1)->setEnabled(false);
            _device->setItemData(_device->count() - 1, device.problem, Qt::ToolTipRole);
        }
    }
    int index = _device->findData(selected);
    if (index < 0 && !selected.isEmpty()) {
        // The saved GPU isn't here now (or the list is still loading): keep it, marked.
        const QString name = selectedName.isEmpty() ? selected : selectedName;
        _device->addItem(_devices->isLoaded() ? tr("%1 (not found)").arg(name) : name, selected);
        _device->setItemData(_device->count() - 1, name, Qt::UserRole + 1);
        index = _device->count() - 1;
    }
    _device->setCurrentIndex(std::max(index, 0));
    updateState();
}

void EngineSettingsDialog::updateState() {
    const auto mode = static_cast<EngineSettings::Mode>(_modes->checkedId());
    const bool gpu = mode != EngineSettings::Mode::CPU;
    _device->setEnabled(gpu && !_devices->isLoading());
    _refresh->setEnabled(!_devices->isLoading());

    QString info;
    if (_devices->isLoading()) {
        info = tr("Looking for GPUs...");
    } else if (_devices->devices().isEmpty()) {
        info = tr("No GPU can be used: %1").arg(_devices->error());
        if (mode == EngineSettings::Mode::GPU) info += "\n" + tr("GPU runs will run on the CPU.");
    } else if (const GpuDevice* device = _devices->find(_device->currentData().toString())) {
        info = describeDevice(*device);
    } else if (_devices->isLoaded() && !_device->currentData().toString().isEmpty()) {
        info = tr("This GPU isn't present now; until it is, runs use the automatic choice.");
    } else if (!_devices->hasUsableDevice()) {
        info = tr("None of these GPUs can be used, so runs will use the CPU.");
    }
    _deviceInfo->setText(info);
}

EngineSettings EngineSettingsDialog::settings() const {
    EngineSettings settings;
    settings.mode = static_cast<EngineSettings::Mode>(_modes->checkedId());
    settings.gpuDevice = _device->currentData().toString();
    settings.gpuDeviceName = settings.gpuDevice.isEmpty() ? QString() : _device->currentData(Qt::UserRole + 1).toString();
    if (settings.gpuDevice.isEmpty() && _device->count() == 0) {
        settings.gpuDevice = _initial.gpuDevice;
        settings.gpuDeviceName = _initial.gpuDeviceName;
    }
    return settings;
}

} // namespace kiems_gui
