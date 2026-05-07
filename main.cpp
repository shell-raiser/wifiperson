#include "mainwindow.h"

#include <QApplication>
#include <QPalette>
#include <QProcess>
#include <QStyleFactory>
#include <QStyleHints>
#include <QtGlobal>

namespace {

bool processOutputContains(const QString &program, const QStringList &arguments, const QString &needle)
{
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForFinished(300)) {
        process.kill();
        return false;
    }

    const QString output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    return output.contains(needle, Qt::CaseInsensitive);
}

bool linuxDesktopPrefersDarkMode()
{
#ifdef Q_OS_LINUX
    const QString gtkTheme = QString::fromUtf8(qgetenv("GTK_THEME"));
    if (gtkTheme.contains("dark", Qt::CaseInsensitive)) {
        return true;
    }

    if (processOutputContains("gsettings", {"get", "org.gnome.desktop.interface", "color-scheme"}, "prefer-dark")) {
        return true;
    }

    if (processOutputContains("gsettings", {"get", "org.gnome.desktop.interface", "gtk-theme"}, "dark")) {
        return true;
    }

    if (processOutputContains("kreadconfig6", {"--group", "General", "--key", "ColorScheme"}, "dark")
        || processOutputContains("kreadconfig5", {"--group", "General", "--key", "ColorScheme"}, "dark")) {
        return true;
    }
#endif

    return false;
}

bool systemPrefersDarkMode(const QApplication &app)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme colorScheme = app.styleHints()->colorScheme();
    if (colorScheme == Qt::ColorScheme::Dark) {
        return true;
    }
    if (colorScheme == Qt::ColorScheme::Light) {
        return false;
    }
#endif

    if (linuxDesktopPrefersDarkMode()) {
        return true;
    }

    return app.palette().color(QPalette::Window).lightness() < 128;
}

void applyDarkPalette(QApplication *app)
{
    app->setStyle(QStyleFactory::create("Fusion"));

    QPalette palette;
    palette.setColor(QPalette::Window, QColor(30, 34, 39));
    palette.setColor(QPalette::WindowText, QColor(232, 236, 241));
    palette.setColor(QPalette::Base, QColor(18, 21, 25));
    palette.setColor(QPalette::AlternateBase, QColor(38, 43, 49));
    palette.setColor(QPalette::ToolTipBase, QColor(232, 236, 241));
    palette.setColor(QPalette::ToolTipText, QColor(18, 21, 25));
    palette.setColor(QPalette::Text, QColor(232, 236, 241));
    palette.setColor(QPalette::Button, QColor(43, 49, 56));
    palette.setColor(QPalette::ButtonText, QColor(232, 236, 241));
    palette.setColor(QPalette::BrightText, Qt::red);
    palette.setColor(QPalette::Link, QColor(88, 166, 255));
    palette.setColor(QPalette::Highlight, QColor(56, 139, 253));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(135, 143, 153));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(135, 143, 153));

    app->setPalette(palette);
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    if (systemPrefersDarkMode(app)) {
        applyDarkPalette(&app);
    }
    
    MainWindow window;
    window.show();
    
    return app.exec();
}
