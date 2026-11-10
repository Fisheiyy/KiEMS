// KiEMS desktop app entry point: `kiems-gui [document]` opens a .g2esim package (or the
// simulation.json in one) if given.
#include "MainWindow.hpp"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace {

/// A dark palette close to the macOS app's appearance, on Qt's portable Fusion style.
void applyDarkTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette palette;
    const QColor window(0x26, 0x28, 0x2c), base(0x1e, 0x20, 0x24), text(0xe4, 0xe6, 0xea);
    const QColor disabled(0x7a, 0x7d, 0x84), accent(0x2f, 0x6f, 0xe0);
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, window);
    palette.setColor(QPalette::ToolTipBase, base);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, QColor(0x32, 0x35, 0x3a));
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Highlight, accent);
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, accent.lighter(130));
    palette.setColor(QPalette::PlaceholderText, disabled);
    for (QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        palette.setColor(QPalette::Disabled, role, disabled);
    }
    app.setPalette(palette);
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("KiEMS");
    QApplication::setOrganizationName("KiEMS");
    applyDarkTheme(app);

    kiems_gui::MainWindow window;
    window.show();
    const QStringList arguments = QApplication::arguments();
    if (arguments.size() > 1) window.openDocument(arguments.at(1));
    return app.exec();
}
