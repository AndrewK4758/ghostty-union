#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <Qt>

#include <optional>

namespace GhosttyUnionSession {

struct SessionSplitNode {
    enum class Type {
        Terminal,
        Split,
    };

    Type type = Type::Terminal;

    // Terminal leaf fields
    QString cwd;
    bool active = false;

    // Split container fields
    Qt::Orientation orientation = Qt::Horizontal;
    QList<double> ratios;
    QList<SessionSplitNode> children;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] int terminalCount() const;
};

struct SessionTabState {
    QString customTitle;
    bool zoomed = false;
    SessionSplitNode root;

    [[nodiscard]] bool isValid() const { return root.isValid(); }
    [[nodiscard]] int terminalCount() const { return root.terminalCount(); }
};

struct SessionWindowState {
    QByteArray geometry;
    bool maximized = false;
    int activeTabIndex = 0;
    QList<SessionTabState> tabs;

    [[nodiscard]] bool isValid() const { return !tabs.isEmpty(); }
    [[nodiscard]] int terminalCount() const;
};

struct SessionState {
    int version = 1;
    QList<SessionWindowState> windows;

    [[nodiscard]] bool isValid() const { return !windows.isEmpty(); }
};

[[nodiscard]] QString defaultSessionFilePath();
[[nodiscard]] QString resolveRestoredCwd(const QString &savedCwd);

[[nodiscard]] QJsonObject toJson(const SessionState &state);
[[nodiscard]] std::optional<SessionState> fromJson(const QJsonObject &obj);

bool saveToFile(const SessionState &state, const QString &filePath = {});
[[nodiscard]] std::optional<SessionState> loadFromFile(const QString &filePath = {});
bool clearSessionFile(const QString &filePath = {});

} // namespace GhosttyUnionSession
