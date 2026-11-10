// Board data for the views, loaded through libkicad on a worker thread: libkicad's queries can
// take a second or more (its stock backend asks an installed KiCad), which mustn't stall the UI.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include "libkicad/libkicad.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace kiems_gui {

/// One immutable snapshot of a board, shared with the views.
struct BoardData {
    QString boardPath;
    std::vector<libkicad::BoardLayerInfo> layers;   // enabled layers, KiCad display order
    std::map<std::string, std::string> layerColors; // layer name -> "#RRGGBBAA"
    QStringList copperLayers;                       // F.Cu first, matching libkiems' metal indices
    libkicad::BoardGeometry geometry;
    std::vector<libkicad::FootprintInfo> footprints;
    std::vector<libkicad::PadPosition> pads;
    QStringList nets;
    std::map<std::string, std::string> netClasses;  // net -> its highest-priority class

    const libkicad::PadPosition* pad(const QString& footprint, const QString& number) const;
};

class BoardModel : public QObject {
    Q_OBJECT

public:
    explicit BoardModel(QObject* parent = nullptr);
    ~BoardModel() override;

    /// Starts loading `kicadPcbPath` (and its sibling .kicad_pro); loaded() or failed() follows.
    void load(const QString& kicadPcbPath);
    void clear();
    bool isLoading() const { return _loading; }
    std::shared_ptr<const BoardData> data() const { return _data; }

signals:
    void loadingStarted();
    void loaded();
    void failed(const QString& error);

private:
    std::shared_ptr<const BoardData> _data;
    bool _loading = false;
    unsigned _generation = 0;
};

} // namespace kiems_gui
