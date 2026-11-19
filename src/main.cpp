#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <signal.h>

int main(int argc, char *argv[]) {
    ::signal(SIGPIPE, SIG_IGN);

    const QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ghostty-union"));
    QApplication::setOrganizationName(QStringLiteral("ghostty-union"));
    QApplication::setDesktopFileName(QStringLiteral("ghostty-union"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("com.mitchellh.ghostty"),
                                       QIcon::fromTheme(QStringLiteral("utilities-terminal"))));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Union-styled KDE terminal powered by libghostty-vt with mounted system monitor bar"));
    parser.addHelpOption();
    const QCommandLineOption cwdOpt(QStringList{QStringLiteral("working-directory"), QStringLiteral("d")},
                              QStringLiteral("Initial working directory"),
                              QStringLiteral("dir"));
    parser.addOption(cwdOpt);
    parser.process(app);

    MainWindow win(parser.value(cwdOpt));
    win.show();
    return QApplication::exec();
}
