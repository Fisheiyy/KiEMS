#include "BoardModel.hpp"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include <expected>
#include <mutex>
#include <optional>

namespace kiems_gui {

namespace {

/// libkicad's process-wide runtime: created once, on first use, and kept for the app's lifetime.
std::expected<libkicad::Runtime*, std::string> runtime() {
    static std::mutex mutex;
    static std::optional<libkicad::Runtime> instance;
    static std::string error;
    std::lock_guard lock(mutex);
    if (!instance && error.empty()) {
        auto created = libkicad::Runtime::create();
        if (created) instance.emplace(std::move(*created));
        else error = created.error();
    }
    if (!instance) return std::unexpected(error);
    return &*instance;
}

struct LoadResult {
    std::shared_ptr<BoardData> data;
    QString error;
};

LoadResult loadBoard(const QString& boardPath) {
    LoadResult result;
    auto kicad = runtime();
    if (!kicad) {
        result.error = QString::fromStdString(kicad.error());
        return result;
    }
    const QString projectPath = QFileInfo(boardPath).path() + "/" + QFileInfo(boardPath).completeBaseName() + ".kicad_pro";
    const libkicad::Board board(**kicad, projectPath.toStdString(), boardPath.toStdString());

    auto data = std::make_shared<BoardData>();
    data->boardPath = boardPath;
    auto fail = [&](const std::string& what, const std::string& error) {
        result.error = QString::fromStdString(what + ": " + error);
        return result;
    };

    auto layers = board.boardLayers();
    if (!layers) return fail("Couldn't read the board's layers", layers.error());
    data->layers = std::move(*layers);
    if (auto colors = board.layerColors()) {
        for (const auto& color : *colors) data->layerColors[color.name] = color.hex;
    }
    // libkiems numbers metals from F.Cu down, as the board's own stackup lists them.
    if (auto stackup = board.stackup()) {
        for (const auto& layer : *stackup) {
            if (layer.kind == libkicad::StackupLayerKind::Copper) data->copperLayers << QString::fromStdString(layer.name);
        }
        data->stackup = std::move(*stackup);
    }
    // For the Geometry page's vias; a board without them still loads.
    if (auto holes = board.throughHoles()) data->throughHoles = std::move(*holes);
    if (auto tracks = board.allTracks()) data->tracks = std::move(*tracks);
    auto geometry = board.boardGeometry();
    if (!geometry) return fail("Couldn't read the board's geometry", geometry.error());
    data->geometry = std::move(*geometry);
    auto footprints = board.footprints();
    if (!footprints) return fail("Couldn't read the board's footprints", footprints.error());
    data->footprints = std::move(*footprints);
    auto pads = board.allPads();
    if (!pads) return fail("Couldn't read the board's pads", pads.error());
    data->pads = std::move(*pads);
    if (auto nets = board.allNets()) {
        for (const auto& net : *nets) {
            data->nets << QString::fromStdString(net);
            if (auto netClass = board.netClassForNet(net)) data->netClasses[net] = *netClass;
        }
    }
    result.data = std::move(data);
    return result;
}

} // namespace

const libkicad::PadPosition* BoardData::pad(const QString& footprint, const QString& number) const {
    const std::string ref = footprint.toStdString();
    const std::string num = number.toStdString();
    for (const auto& pad : pads) {
        if (pad.footprintRef == ref && pad.padNumber == num) return &pad;
    }
    return nullptr;
}

BoardModel::BoardModel(QObject* parent) : QObject(parent) {}

BoardModel::~BoardModel() = default;

void BoardModel::load(const QString& kicadPcbPath) {
    const unsigned generation = ++_generation;
    _loading = true;
    emit loadingStarted();
    auto* watcher = new QFutureWatcher<LoadResult>(this);
    connect(watcher, &QFutureWatcher<LoadResult>::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != _generation) return; // Superseded by a later load() or clear().
        _loading = false;
        LoadResult result = watcher->result();
        if (!result.data) {
            emit failed(result.error);
            return;
        }
        _data = std::move(result.data);
        emit loaded();
    });
    watcher->setFuture(QtConcurrent::run(loadBoard, kicadPcbPath));
}

void BoardModel::clear() {
    ++_generation;
    _loading = false;
    _data.reset();
    emit loaded();
}

} // namespace kiems_gui
