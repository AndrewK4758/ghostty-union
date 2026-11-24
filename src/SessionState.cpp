#include "SessionState.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace GhosttyUnionSession {

namespace {

constexpr int kCurrentVersion = 1;

QList<double> normalizeRatios(const QList<double> &raw, int count) {
    QList<double> result;
    if (count <= 0) {
        return result;
    }
    result.reserve(count);

    double sum = 0.0;
    bool allValid = (raw.size() == count);
    if (allValid) {
        for (double r : raw) {
            if (!std::isfinite(r) || r <= 0.0) {
                allValid = false;
                break;
            }
            sum += r;
        }
    }

    if (!allValid || sum <= 0.0) {
        const double eq = 1.0 / static_cast<double>(count);
        for (int i = 0; i < count; ++i) {
            result.append(eq);
        }
        return result;
    }

    for (double r : raw) {
        result.append(r / sum);
    }
    return result;
}

QJsonObject splitNodeToJson(const SessionSplitNode &node) {
    QJsonObject obj;
    if (node.type == SessionSplitNode::Type::Terminal) {
        obj.insert(QStringLiteral("type"), QStringLiteral("terminal"));
        obj.insert(QStringLiteral("cwd"), node.cwd);
        if (node.active) {
            obj.insert(QStringLiteral("active"), true);
        }
        return obj;
    }

    obj.insert(QStringLiteral("type"), QStringLiteral("split"));
    obj.insert(QStringLiteral("orientation"),
               node.orientation == Qt::Vertical ? QStringLiteral("vertical") : QStringLiteral("horizontal"));

    const QList<double> norm = normalizeRatios(node.ratios, static_cast<int>(node.children.size()));
    QJsonArray ratiosArr;
    for (double r : norm) {
        ratiosArr.append(r);
    }
    obj.insert(QStringLiteral("ratios"), ratiosArr);

    QJsonArray childrenArr;
    for (const SessionSplitNode &child : node.children) {
        childrenArr.append(splitNodeToJson(child));
    }
    obj.insert(QStringLiteral("children"), childrenArr);
    return obj;
}

std::optional<SessionSplitNode> splitNodeFromJson(const QJsonObject &obj) {
    const QString typeStr = obj.value(QStringLiteral("type")).toString();
    if (typeStr == QStringLiteral("terminal")) {
        SessionSplitNode node;
        node.type = SessionSplitNode::Type::Terminal;
        node.cwd = obj.value(QStringLiteral("cwd")).toString();
        node.active = obj.value(QStringLiteral("active")).toBool(false);
        return node;
    }
    if (typeStr != QStringLiteral("split")) {
        return std::nullopt;
    }

    SessionSplitNode node;
    node.type = SessionSplitNode::Type::Split;
    const QString orientStr = obj.value(QStringLiteral("orientation")).toString();
    node.orientation = (orientStr == QStringLiteral("vertical")) ? Qt::Vertical : Qt::Horizontal;

    const QJsonArray childrenArr = obj.value(QStringLiteral("children")).toArray();
    const QJsonArray ratiosArr = obj.value(QStringLiteral("ratios")).toArray();

    QList<double> keptRatios;
    for (int i = 0; i < childrenArr.size(); ++i) {
        if (!childrenArr[i].isObject()) {
            continue;
        }
        auto child = splitNodeFromJson(childrenArr[i].toObject());
        if (child && child->isValid()) {
            node.children.append(std::move(*child));
            if (i < ratiosArr.size() && ratiosArr[i].isDouble()) {
                keptRatios.append(ratiosArr[i].toDouble());
            } else {
                keptRatios.append(1.0);
            }
        }
    }

    if (node.children.isEmpty()) {
        return std::nullopt;
    }
    if (node.children.size() == 1) {
        return node.children.first();
    }

    node.ratios = normalizeRatios(keptRatios, static_cast<int>(node.children.size()));
    return node;
}

QJsonObject tabStateToJson(const SessionTabState &tab) {
    QJsonObject obj;
    if (!tab.customTitle.isEmpty()) {
        obj.insert(QStringLiteral("customTitle"), tab.customTitle);
    }
    if (tab.zoomed) {
        obj.insert(QStringLiteral("zoomed"), true);
    }
    obj.insert(QStringLiteral("splitTree"), splitNodeToJson(tab.root));
    return obj;
}

std::optional<SessionTabState> tabStateFromJson(const QJsonObject &obj) {
    if (!obj.value(QStringLiteral("splitTree")).isObject()) {
        return std::nullopt;
    }
    auto root = splitNodeFromJson(obj.value(QStringLiteral("splitTree")).toObject());
    if (!root || !root->isValid()) {
        return std::nullopt;
    }
    SessionTabState tab;
    tab.customTitle = obj.value(QStringLiteral("customTitle")).toString().trimmed();
    tab.zoomed = obj.value(QStringLiteral("zoomed")).toBool(false);
    tab.root = std::move(*root);
    return tab;
}

QJsonObject windowStateToJson(const SessionWindowState &win) {
    QJsonObject obj;
    if (!win.geometry.isEmpty()) {
        obj.insert(QStringLiteral("geometry"), QString::fromLatin1(win.geometry.toBase64()));
    }
    if (win.maximized) {
        obj.insert(QStringLiteral("maximized"), true);
    }
    obj.insert(QStringLiteral("activeTabIndex"), win.activeTabIndex);

    QJsonArray tabsArr;
    for (const SessionTabState &tab : win.tabs) {
        if (tab.isValid()) {
            tabsArr.append(tabStateToJson(tab));
        }
    }
    obj.insert(QStringLiteral("tabs"), tabsArr);
    return obj;
}

std::optional<SessionWindowState> windowStateFromJson(const QJsonObject &obj) {
    const QJsonArray tabsArr = obj.value(QStringLiteral("tabs")).toArray();
    SessionWindowState win;
    for (const QJsonValue &val : tabsArr) {
        if (!val.isObject()) {
            continue;
        }
        auto tab = tabStateFromJson(val.toObject());
        if (tab && tab->isValid()) {
            win.tabs.append(std::move(*tab));
        }
    }
    if (win.tabs.isEmpty()) {
        return std::nullopt;
    }

    const QString geomB64 = obj.value(QStringLiteral("geometry")).toString();
    if (!geomB64.isEmpty()) {
        win.geometry = QByteArray::fromBase64(geomB64.toLatin1());
    }
    win.maximized = obj.value(QStringLiteral("maximized")).toBool(false);
    const int rawIdx = obj.value(QStringLiteral("activeTabIndex")).toInt(0);
    win.activeTabIndex = std::clamp(rawIdx, 0, static_cast<int>(win.tabs.size()) - 1);
    return win;
}

QString effectivePath(const QString &filePath) {
    return filePath.isEmpty() ? defaultSessionFilePath() : filePath;
}

} // namespace

bool SessionSplitNode::isValid() const {
    if (type == Type::Terminal) {
        return true;
    }
    if (children.isEmpty()) {
        return false;
    }
    return std::ranges::all_of(children, [](const SessionSplitNode &c) { return c.isValid(); });
}

int SessionSplitNode::terminalCount() const {
    if (type == Type::Terminal) {
        return 1;
    }
    int total = 0;
    for (const SessionSplitNode &child : children) {
        total += child.terminalCount();
    }
    return total;
}

int SessionWindowState::terminalCount() const {
    int total = 0;
    for (const SessionTabState &tab : tabs) {
        total += tab.terminalCount();
    }
    return total;
}

QString defaultSessionFilePath() {
    const QString envOverride = qEnvironmentVariable("GHOSTTY_UNION_SESSION_PATH").trimmed();
    if (!envOverride.isEmpty()) {
        return envOverride;
    }
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (configDir.isEmpty()) {
        configDir = QDir::homePath() + QStringLiteral("/.config/ghostty-union");
    }
    return QDir(configDir).filePath(QStringLiteral("session.json"));
}

QString resolveRestoredCwd(const QString &savedCwd) {
    const QString trimmed = savedCwd.trimmed();
    if (!trimmed.isEmpty()) {
        const QFileInfo fi(trimmed);
        if (fi.exists() && fi.isDir()) {
            return fi.absoluteFilePath();
        }
    }
    return QDir::homePath();
}

QJsonObject toJson(const SessionState &state) {
    QJsonObject root;
    root.insert(QStringLiteral("version"), state.version > 0 ? state.version : kCurrentVersion);
    QJsonArray windowsArr;
    for (const SessionWindowState &win : state.windows) {
        if (win.isValid()) {
            windowsArr.append(windowStateToJson(win));
        }
    }
    root.insert(QStringLiteral("windows"), windowsArr);
    return root;
}

std::optional<SessionState> fromJson(const QJsonObject &obj) {
    const int ver = obj.value(QStringLiteral("version")).toInt(0);
    if (ver != kCurrentVersion) {
        return std::nullopt;
    }
    const QJsonArray windowsArr = obj.value(QStringLiteral("windows")).toArray();
    SessionState state;
    state.version = ver;
    for (const QJsonValue &val : windowsArr) {
        if (!val.isObject()) {
            continue;
        }
        auto win = windowStateFromJson(val.toObject());
        if (win && win->isValid()) {
            state.windows.append(std::move(*win));
        }
    }
    if (!state.isValid()) {
        return std::nullopt;
    }
    return state;
}

bool saveToFile(const SessionState &state, const QString &filePath) {
    if (!state.isValid()) {
        return false;
    }
    const QString path = effectivePath(filePath);
    const QFileInfo fi(path);
    if (!QDir().mkpath(fi.absolutePath())) {
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QJsonDocument doc(toJson(state));
    const QByteArray bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

std::optional<SessionState> loadFromFile(const QString &filePath) {
    const QString path = effectivePath(filePath);
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }
    return fromJson(doc.object());
}

bool clearSessionFile(const QString &filePath) {
    const QString path = effectivePath(filePath);
    if (!QFile::exists(path)) {
        return true;
    }
    return QFile::remove(path);
}

} // namespace GhosttyUnionSession
