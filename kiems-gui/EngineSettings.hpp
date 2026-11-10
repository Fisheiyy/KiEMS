// Which FDTD engine the app's runs use -- the CPU, the GPU, or the GPU when there is one -- and on
// which GPU. It is a setting of this machine rather than of a document, so it lives in QSettings,
// and it reaches the pipeline as kiems's --backend and --gpu-device arguments. The GPUs come from
// `kiems --list-gpus`, so the app reports exactly the devices the engine itself sees.
#pragma once

#include <QDialog>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QButtonGroup;
class QComboBox;
class QLabel;
class QProcess;
class QPushButton;

namespace kiems_gui {

struct GpuDevice {
    int index = 0;
    QString name;
    QString kind; ///< "discrete", "integrated", "virtual", "cpu" or "other"
    QString api;  ///< e.g. "Vulkan 1.4" or "Metal"
    quint64 memoryBytes = 0;
    bool usable = false;
    QString problem; ///< why it isn't usable
    bool isDefault = false;
};

/// The GPUs `kiems --list-gpus` reports, fetched in the background.
class GpuDeviceList : public QObject {
    Q_OBJECT

public:
    explicit GpuDeviceList(QObject* parent = nullptr);
    ~GpuDeviceList() override;

    /// Lists the GPUs again; changed() follows when it is done.
    void refresh();
    bool isLoading() const { return _process != nullptr; }
    bool isLoaded() const { return _loaded; }
    const QList<GpuDevice>& devices() const { return _devices; }
    /// Why the list is empty, when it is.
    const QString& error() const { return _error; }

    /// The device the engine picks when none is chosen; null if there is none.
    const GpuDevice* defaultDevice() const;
    /// The device `key` (see keyFor()) selects; null if none does.
    const GpuDevice* find(const QString& key) const;
    bool hasUsableDevice() const { return defaultDevice() != nullptr; }

    /// What --gpu-device needs to select `device`: its name, or its index when another device has
    /// the same name.
    QString keyFor(const GpuDevice& device) const;

signals:
    void changed();

private:
    void finish(bool succeeded);

    QProcess* _process = nullptr;
    QList<GpuDevice> _devices;
    QString _error;
    bool _loaded = false;
};

struct EngineSettings {
    enum class Mode { Automatic, CPU, GPU };

    Mode mode = Mode::Automatic;
    /// --gpu-device's value; empty lets the engine pick.
    QString gpuDevice;
    /// The chosen GPU's name, for showing the choice while the device list is still loading or
    /// when the GPU is no longer present.
    QString gpuDeviceName;

    static EngineSettings load();
    void save() const;

    /// Whether a run now uses the GPU engine (Automatic: when there is a usable GPU).
    bool usesGPU(const GpuDeviceList& devices) const;
    /// kiems's arguments for this choice.
    QStringList arguments(const GpuDeviceList& devices) const;
    /// A short description for the status bar and the log, e.g. "GPU: NVIDIA GeForce RTX 4080".
    QString describe(const GpuDeviceList& devices) const;
};

class EngineSettingsDialog : public QDialog {
    Q_OBJECT

public:
    EngineSettingsDialog(GpuDeviceList* devices, const EngineSettings& settings, QWidget* parent = nullptr);

    EngineSettings settings() const;

private:
    void populateDevices();
    void updateState();

    GpuDeviceList* _devices;
    EngineSettings _initial;
    QButtonGroup* _modes;
    QComboBox* _device;
    QPushButton* _refresh;
    QLabel* _deviceInfo;
};

} // namespace kiems_gui
