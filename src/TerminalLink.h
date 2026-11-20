#pragma once

#include <QSysInfo> // IWYU pragma: keep
#include <QUrl>
#include <QVector>

#include <ghostty/vt.h>

namespace GhosttyUnionLink {

struct LinkCellSpan {
    uint16_t row = 0;
    uint16_t startCol = 0; // inclusive
    uint16_t endCol = 0;   // inclusive
};

struct ResolvedLink {
    QString rawText;
    QString target;       // canonical local path (if isLocalFile) or full URL string
    int line = 0;         // 1-based line (>0 when :line or #L<line> present)
    int column = 0;       // 1-based column (>0 when :line:col present)
    bool isLocalFile = false;
    bool isOsc8 = false;
    QVector<LinkCellSpan> spans;

    [[nodiscard]] bool isValid() const { return !target.isEmpty() && !spans.isEmpty(); }
    [[nodiscard]] QString previewText() const;
    [[nodiscard]] bool sameDestination(const ResolvedLink &other) const;
};

/// Decode OSC 7 `file://host/path` and `kitty-shell-cwd://host/path` strings for local hosts.
QString parseOsc7Pwd(const QString &raw, const QString &localHostName = QSysInfo::machineHostName());

/// Resolve a clickable link or local file path under viewport coordinates (hoverCol, hoverRow).
ResolvedLink resolveAt(GhosttyTerminal terminal,
                       uint16_t cols,
                       uint16_t rows,
                       uint16_t hoverCol,
                       uint16_t hoverRow,
                       const QString &cwd,
                       bool allowBareCwdFiles = true,
                       const QString &localHostName = QSysInfo::machineHostName());

/// Open a resolved link using Kate (`--line`/`--column`) when line coords are present, or xdg-open.
bool openLink(const ResolvedLink &link);

/// POSIX single-quote escaping for pasted/dropped filesystem paths.
QString shellQuoteSingle(const QString &s);

/// Format dropped URLs/local file paths into a space-separated shell-safe string.
QString shellQuotePaths(const QList<QUrl> &urls);

} // namespace GhosttyUnionLink
