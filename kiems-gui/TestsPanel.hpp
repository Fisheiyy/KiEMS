// The inspector's Tests tab: the eye mask every eye in a simulation's results is tested against,
// picked from a standard (EyeTestPreset) or entered by hand, with the bit rate and signal level the
// eye is built at. Picking a standard fills its numbers in; editing any of them away from the
// standard's makes the test Custom. The macOS app's EyeTestSectionView.
#pragma once

#include <QWidget>

class QPushButton;

namespace kiems_gui {

class Document;
class MaskShapeView;
class QuantityEdit;

class TestsPanel : public QWidget {
    Q_OBJECT

public:
    explicit TestsPanel(Document* document, QWidget* parent = nullptr);

    void setSimulation(int simulation);
    void refresh();

private:
    void pickStandard(const QString& standard);
    /// Sets one eye setting, switching the test to Custom if that leaves its standard's numbers.
    void edit(const QString& pointer, double value);

    Document* _document;
    int _simulation = -1;

    QPushButton* _standard;
    QuantityEdit* _bitRate;
    QuantityEdit* _signalLevel;
    MaskShapeView* _shape;
    QuantityEdit* _x1;
    QuantityEdit* _x2;
    QuantityEdit* _y1;
    QuantityEdit* _y2;
};

} // namespace kiems_gui
