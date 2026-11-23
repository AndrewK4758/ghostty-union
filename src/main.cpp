#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QStandardPaths>
#include <QTextStream>
#include <optional>
#include <signal.h>

namespace {

QIcon resolveAppWindowIcon() {
    for (const QString &themeName : {
             QStringLiteral("com.mitchellh.ghostty"),
             QStringLiteral("ghostty-union"),
         }) {
        const QIcon themed = QIcon::fromTheme(themeName);
        if (!themed.isNull()) {
            return themed;
        }
    }

    const QStringList relPaths = {
        QStringLiteral("icons/candy-icons/apps/scalable/com.mitchellh.ghostty.svg"),
        QStringLiteral("icons/hicolor/512x512/apps/com.mitchellh.ghostty.png"),
        QStringLiteral("icons/hicolor/256x256/apps/com.mitchellh.ghostty.png"),
        QStringLiteral("icons/hicolor/128x128/apps/com.mitchellh.ghostty.png"),
        QStringLiteral("icons/hicolor/scalable/apps/ghostty-union.svg"),
    };
    for (const QString &dataDir : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        for (const QString &rel : relPaths) {
            const QString fullPath = QDir(dataDir).filePath(rel);
            if (QFileInfo::exists(fullPath)) {
                const QIcon fileIcon(fullPath);
                if (!fileIcon.isNull()) {
                    return fileIcon;
                }
            }
        }
    }

    return QIcon::fromTheme(QStringLiteral("utilities-terminal"));
}

} // namespace

int main(int argc, char *argv[]) {
    ::signal(SIGPIPE, SIG_IGN);

    // Set application & desktop identity BEFORE constructing QApplication so Qt6 Wayland
    // binds xdg_toplevel app_id = "ghostty-union" cleanly on the initial surface commit.
    QCoreApplication::setApplicationName(QStringLiteral("ghostty-union"));
    QCoreApplication::setOrganizationName(QStringLiteral("ghostty-union"));
    QGuiApplication::setDesktopFileName(QStringLiteral("ghostty-union"));

    const QApplication app(argc, argv);
    const QIcon appIcon = resolveAppWindowIcon();
    QApplication::setWindowIcon(appIcon);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Union-styled KDE terminal powered by libghostty-vt with mounted system monitor bar"));
    parser.addHelpOption();
    const QCommandLineOption cwdOpt(QStringList{QStringLiteral("working-directory"), QStringLiteral("d")},
                              QStringLiteral("Initial working directory"),
                              QStringLiteral("dir"));
    const QCommandLineOption themeOpt(QStringList{QStringLiteral("theme"), QStringLiteral("t")},
                              QStringLiteral("Terminal color theme (solar-forge, ollie)"),
                              QStringLiteral("name"));
    parser.addOption(cwdOpt);
    parser.addOption(themeOpt);
    parser.process(app);

    std::optional<GhosttyUnionTheme::TerminalThemeId> themeOverride;
    if (parser.isSet(themeOpt)) {
        bool ok = false;
        const auto parsed = GhosttyUnionTheme::parseTerminalThemeId(parser.value(themeOpt), &ok);
        if (ok) {
            themeOverride = parsed;
        } else {
            QTextStream(stderr) << "ghostty-union: unknown theme '" << parser.value(themeOpt)
                                << "' (available: solar-forge, ollie); using default.\n";
        }
    }

    MainWindow win(parser.value(cwdOpt), nullptr, themeOverride);
    win.setWindowIcon(appIcon);
    win.show();
    return QApplication::exec();
}
