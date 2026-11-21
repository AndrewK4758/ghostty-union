#include "TerminalLink.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <vector>

namespace GhosttyUnionLink {

namespace {

struct CellCoord {
    uint16_t row = 0;
    uint16_t startCol = 0;
    uint16_t endCol = 0;
};

bool hasControlChars(const QString &s) {
    return std::ranges::any_of(s, [](const QChar ch) {
        const ushort uc = ch.unicode();
        return uc < 0x20U || uc == 0x7fU;
    });
}

bool isLocalHost(const QString &host, const QString &localHostName) {
    if (host.isEmpty()) {
        return true;
    }
    if (host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0 ||
        host.compare(QStringLiteral("localhost.localdomain"), Qt::CaseInsensitive) == 0 ||
        host == QStringLiteral("127.0.0.1") ||
        host == QStringLiteral("::1")) {
        return true;
    }
    if (!localHostName.isEmpty()) {
        if (host.compare(localHostName, Qt::CaseInsensitive) == 0) {
            return true;
        }
        const QString shortLocal = localHostName.section(QLatin1Char('.'), 0, 0);
        if (!shortLocal.isEmpty() && host.compare(shortLocal, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString readOsc8Uri(const GhosttyGridRef &ref) {
    GhosttyCell cell{};
    if (ghostty_grid_ref_cell(&ref, &cell) != GHOSTTY_SUCCESS) {
        return {};
    }
    bool hasLink = false;
    if (ghostty_cell_get(cell, GHOSTTY_CELL_DATA_HAS_HYPERLINK, &hasLink) != GHOSTTY_SUCCESS || !hasLink) {
        return {};
    }
    uint8_t stackBuf[512];
    size_t outLen = 0;
    GhosttyResult res = ghostty_grid_ref_hyperlink_uri(&ref, stackBuf, sizeof(stackBuf), &outLen);
    if (res == GHOSTTY_SUCCESS && outLen > 0) {
        return QString::fromUtf8(reinterpret_cast<const char *>(stackBuf), static_cast<qsizetype>(outLen));
    }
    if (res == GHOSTTY_OUT_OF_SPACE && outLen > 0 && outLen <= 16384) {
        std::vector<uint8_t> heapBuf(outLen);
        res = ghostty_grid_ref_hyperlink_uri(&ref, heapBuf.data(), heapBuf.size(), &outLen);
        if (res == GHOSTTY_SUCCESS && outLen > 0) {
            return QString::fromUtf8(reinterpret_cast<const char *>(heapBuf.data()), static_cast<qsizetype>(outLen));
        }
    }
    return {};
}

QString readRefText(const GhosttyGridRef &ref) {
    uint32_t stackCps[16];
    size_t len = 0;
    GhosttyResult res = ghostty_grid_ref_graphemes(&ref, stackCps, 16, &len);
    if (res == GHOSTTY_SUCCESS) {
        if (len == 0 || stackCps[0] == 0U) {
            return {};
        }
        return QString::fromUcs4(reinterpret_cast<const char32_t *>(stackCps), static_cast<qsizetype>(len));
    }
    if (res == GHOSTTY_OUT_OF_SPACE && len > 0 && len <= 128) {
        std::vector<uint32_t> heapCps(len);
        res = ghostty_grid_ref_graphemes(&ref, heapCps.data(), heapCps.size(), &len);
        if (res == GHOSTTY_SUCCESS && len > 0 && heapCps[0] != 0U) {
            return QString::fromUcs4(reinterpret_cast<const char32_t *>(heapCps.data()), static_cast<qsizetype>(len));
        }
    }
    return {};
}

bool rowHasWrapOut(GhosttyTerminal terminal, uint16_t row) {
    const GhosttyPoint pt{
        .tag = GHOSTTY_POINT_TAG_VIEWPORT,
        .value = {.coordinate = {.x = 0, .y = row}},
    };
    GhosttyGridRef ref{};
    if (ghostty_terminal_grid_ref(terminal, pt, &ref) != GHOSTTY_SUCCESS) {
        return false;
    }
    GhosttyRow grow{};
    if (ghostty_grid_ref_row(&ref, &grow) != GHOSTTY_SUCCESS) {
        return false;
    }
    bool wrap = false;
    ghostty_row_get(grow, GHOSTTY_ROW_DATA_WRAP, &wrap);
    return wrap;
}

bool rowIsWrapContinuation(GhosttyTerminal terminal, uint16_t row) {
    const GhosttyPoint pt{
        .tag = GHOSTTY_POINT_TAG_VIEWPORT,
        .value = {.coordinate = {.x = 0, .y = row}},
    };
    GhosttyGridRef ref{};
    if (ghostty_terminal_grid_ref(terminal, pt, &ref) != GHOSTTY_SUCCESS) {
        return false;
    }
    GhosttyRow grow{};
    if (ghostty_grid_ref_row(&ref, &grow) != GHOSTTY_SUCCESS) {
        return false;
    }
    bool cont = false;
    ghostty_row_get(grow, GHOSTTY_ROW_DATA_WRAP_CONTINUATION, &cont);
    return cont;
}

bool rowsSoftConnected(GhosttyTerminal terminal, uint16_t upperRow, uint16_t lowerRow) {
    return lowerRow == upperRow + 1 &&
           (rowHasWrapOut(terminal, upperRow) || rowIsWrapContinuation(terminal, lowerRow));
}

QVector<LinkCellSpan> mergeSpans(const std::vector<CellCoord> &coords) {
    QVector<LinkCellSpan> spans;
    for (const CellCoord &cc : coords) {
        if (!spans.isEmpty() && spans.back().row == cc.row) {
            spans.back().startCol = std::min(spans.back().startCol, cc.startCol);
            spans.back().endCol = std::max(spans.back().endCol, cc.endCol);
        } else {
            spans.push_back(LinkCellSpan{cc.row, cc.startCol, cc.endCol});
        }
    }
    return spans;
}

QString expandUserAndEnvPrefix(const QString &path) {
    if (path == QStringLiteral("~")) {
        return QDir::homePath();
    }
    if (path.startsWith(QStringLiteral("~/"))) {
        return QDir::homePath() + path.mid(1);
    }
    static const QRegularExpression kEnvPrefixRe(
        QStringLiteral("^((?:[\\w][\\w\\-.]*/)*)\\$([A-Za-z_]\\w*)(/.*)$"));
    const QRegularExpressionMatch m = kEnvPrefixRe.match(path);
    if (m.hasMatch()) {
        const QString prefix = m.captured(1);
        const QString varName = m.captured(2);
        const QString suffix = m.captured(3);
        if (qEnvironmentVariableIsSet(varName.toUtf8().constData())) {
            const QString val = qEnvironmentVariable(varName.toUtf8().constData());
            if (!val.isEmpty()) {
                return prefix + val + suffix;
            }
        }
    }
    return path;
}

bool resolveExistingLocalPath(const QString &candidate,
                              const QString &cwd,
                              QString &resolvedPathOut,
                              int &lineOut,
                              int &colOut) {
    lineOut = 0;
    colOut = 0;
    resolvedPathOut.clear();

    if (candidate.isEmpty() || hasControlChars(candidate)) {
        return false;
    }

    auto tryCandidate = [&](const QString &pathStr, int ln, int col) -> bool {
        if (pathStr.isEmpty()) {
            return false;
        }
        const QString expanded = expandUserAndEnvPrefix(pathStr);
        QString joined;
        if (QDir::isAbsolutePath(expanded)) {
            joined = QDir::cleanPath(expanded);
        } else if (!cwd.isEmpty()) {
            joined = QDir::cleanPath(QDir(cwd).filePath(expanded));
        } else {
            return false;
        }
        const QFileInfo fi(joined);
        if (!fi.exists()) {
            return false;
        }
        resolvedPathOut = fi.absoluteFilePath();
        lineOut = ln;
        colOut = col;
        return true;
    };

    if (tryCandidate(candidate, 0, 0)) {
        return true;
    }

    static const QRegularExpression kHashLineColRe(
        QStringLiteral("^(.+?)#L([1-9]\\d{0,6})(?:C([1-9]\\d{0,5})|-L?[1-9]\\d{0,6})?$"));
    if (const QRegularExpressionMatch m = kHashLineColRe.match(candidate); m.hasMatch()) {
        const int ln = m.captured(2).toInt();
        const int col = m.captured(3).isEmpty() ? 0 : m.captured(3).toInt();
        if (tryCandidate(m.captured(1), ln, col)) {
            return true;
        }
    }

    static const QRegularExpression kColonLineColRe(
        QStringLiteral("^(.+?):([1-9]\\d{0,6}):([1-9]\\d{0,5}):?$"));
    if (const QRegularExpressionMatch m = kColonLineColRe.match(candidate); m.hasMatch()) {
        if (tryCandidate(m.captured(1), m.captured(2).toInt(), m.captured(3).toInt())) {
            return true;
        }
    }

    static const QRegularExpression kColonLineRe(
        QStringLiteral("^(.+?):([1-9]\\d{0,6}):?$"));
    if (const QRegularExpressionMatch m = kColonLineRe.match(candidate); m.hasMatch()) {
        if (tryCandidate(m.captured(1), m.captured(2).toInt(), 0)) {
            return true;
        }
    }

    static const QRegularExpression kParenLineColRe(
        QStringLiteral("^(.+?)\\(([1-9]\\d{0,6})(?:,([1-9]\\d{0,5}))?\\)$"));
    if (const QRegularExpressionMatch m = kParenLineColRe.match(candidate); m.hasMatch()) {
        const int ln = m.captured(2).toInt();
        const int col = m.captured(3).isEmpty() ? 0 : m.captured(3).toInt();
        if (tryCandidate(m.captured(1), ln, col)) {
            return true;
        }
    }

    return false;
}

bool parseFileUriToLocal(const QString &uri,
                         const QString &cwd,
                         const QString &localHostName,
                         bool requireExists,
                         QString &resolvedPathOut,
                         int &lineOut,
                         int &colOut) {
    lineOut = 0;
    colOut = 0;
    resolvedPathOut.clear();

    if (!uri.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive) || hasControlChars(uri)) {
        return false;
    }

    QString pathPart;
    QString fragment;
    const qsizetype hashIdx = uri.indexOf(QLatin1Char('#'));
    const QString mainUri = (hashIdx >= 0) ? uri.left(hashIdx) : uri;
    if (hashIdx >= 0) {
        fragment = uri.mid(hashIdx + 1);
    }

    if (mainUri.startsWith(QStringLiteral("file://"), Qt::CaseInsensitive)) {
        const QString rest = mainUri.mid(7);
        const qsizetype slashIdx = rest.indexOf(QLatin1Char('/'));
        if (slashIdx < 0) {
            return false;
        }
        const QString host = QUrl::fromPercentEncoding(rest.left(slashIdx).toUtf8());
        if (!isLocalHost(host, localHostName)) {
            return false;
        }
        pathPart = QUrl::fromPercentEncoding(rest.mid(slashIdx).toUtf8());
    } else if (mainUri.startsWith(QStringLiteral("file:/"), Qt::CaseInsensitive)) {
        pathPart = QUrl::fromPercentEncoding(mainUri.mid(5).toUtf8());
    } else {
        return false;
    }

    int fragLine = 0;
    int fragCol = 0;
    if (!fragment.isEmpty()) {
        static const QRegularExpression kFragRe(
            QStringLiteral("^L?([1-9]\\d{0,6})(?:C([1-9]\\d{0,5})|-L?[1-9]\\d{0,6})?$"));
        if (const QRegularExpressionMatch fm = kFragRe.match(fragment); fm.hasMatch()) {
            fragLine = fm.captured(1).toInt();
            fragCol = fm.captured(2).isEmpty() ? 0 : fm.captured(2).toInt();
        }
    }

    if (resolveExistingLocalPath(pathPart, cwd, resolvedPathOut, lineOut, colOut)) {
        if (lineOut == 0 && fragLine > 0) {
            lineOut = fragLine;
            colOut = fragCol;
        }
        return true;
    }

    if (!requireExists && pathPart.startsWith(QLatin1Char('/'))) {
        resolvedPathOut = QDir::cleanPath(pathPart);
        lineOut = fragLine;
        colOut = fragCol;
        return true;
    }
    return false;
}

int unmatchedClosingTrimLength(const QString &s, qsizetype start, qsizetype end) {
    auto countBalanced = [&](QChar openCh, QChar closeCh) -> bool {
        int balance = 0;
        for (qsizetype i = start; i < end; ++i) {
            if (s[i] == openCh) {
                ++balance;
            } else if (s[i] == closeCh) {
                --balance;
            }
        }
        return balance < 0;
    };
    const QChar last = s[end - 1];
    if (last == QLatin1Char(')') && countBalanced(QLatin1Char('('), QLatin1Char(')'))) {
        return 1;
    }
    if (last == QLatin1Char(']') && countBalanced(QLatin1Char('['), QLatin1Char(']'))) {
        return 1;
    }
    if (last == QLatin1Char('}') && countBalanced(QLatin1Char('{'), QLatin1Char('}'))) {
        return 1;
    }
    if (last == QLatin1Char('>') || last == QLatin1Char(',') || last == QLatin1Char('.') ||
        last == QLatin1Char(';') || last == QLatin1Char(':') || last == QLatin1Char('\'') ||
        last == QLatin1Char('"')) {
        return 1;
    }
    return 0;
}

qsizetype trimCandidateEnd(const QString &text, qsizetype start, qsizetype end) {
    while (end > start) {
        const int trim = unmatchedClosingTrimLength(text, start, end);
        if (trim == 0) {
            break;
        }
        end -= trim;
    }
    return end;
}

bool isExplicitNonFileScheme(const QString &s) {
    static const QRegularExpression kNonFileSchemeRe(
        QStringLiteral("^(?:https?://|mailto:|ftp://|ssh://|ssh:|git://|tel:|magnet:|ipfs://|ipns://|gemini://|gopher://|news:)"),
        QRegularExpression::CaseInsensitiveOption);
    return kNonFileSchemeRe.match(s).hasMatch();
}

const QRegularExpression &compiledLinkRegex() {
    static const QRegularExpression kRegex(QStringLiteral(
        "(?:https?://|mailto:|ftp://|file:|ssh:|git://|ssh://|tel:|magnet:|ipfs://|ipns://|gemini://|gopher://|news:)"
        "(?:(?:\\[[0-9a-fA-F:]+\\](?::[0-9]+)?)|[\\w\\-.~:/?#@!$&*+,;=%]+(?:[\\(\\[]\\w*[\\)\\]])?)+(?<![,.])"
        "|(?:\\.\\./|\\./|(?<!\\w)~/|(?:[\\w][\\w\\-.]*/)*(?<!\\w)\\$[A-Za-z_]\\w*/|\\.[\\w][\\w\\-.]*/|(?<![\\w~/])/(?!/))"
        "(?:(?=[\\w\\-.~:/?#@!$&*+;=%]*\\.)[\\w\\-.~:/?#@!$&*+;=%]+(?:(?<!:) (?!\\w+://)(?!\\.{0,2}/)(?!~/)[\\w\\-.~:/?#@!$&*+;=%]*[/.])*(?<!:)"
        "|(?![\\w\\-.~:/?#@!$&*+;=%]*\\.)[\\w\\-.~:/?#@!$&*+;=%]+(?:(?<!:) (?!\\w+://)(?!\\.{0,2}/)(?!~/)[\\w\\-.~:/?#@!$&*+;=%]+)*(?<!:))"
        "|(?=[\\w\\-.~:/?#@!$&*+;=%]*\\.)(?<!\\$)(?<!\\$\\d)(?<!\\$\\d\\d)(?<!\\w)[\\w][\\w\\-.]*/[\\w\\-.~:/?#@!$&*+;=%]+(?<!:)"));
    return kRegex;
}

bool isBareTokenDelimiter(QChar ch) {
    if (ch.isSpace()) {
        return true;
    }
    switch (ch.unicode()) {
    case '\'':
    case '"':
    case '<':
    case '>':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case '|':
    case ';':
    case ',':
    case '`':
        return true;
    default:
        return false;
    }
}

bool isPureDigits(const QString &s) {
    if (s.isEmpty()) {
        return false;
    }
    return std::ranges::all_of(s, [](const QChar c) { return c.isDigit(); });
}

} // namespace

QString ResolvedLink::previewText() const {
    if (!isLocalFile || line <= 0) {
        return target;
    }
    if (column > 0) {
        return QStringLiteral("%1:%2:%3").arg(target).arg(line).arg(column);
    }
    return QStringLiteral("%1:%2").arg(target).arg(line);
}

bool ResolvedLink::sameDestination(const ResolvedLink &other) const {
    return isValid() && other.isValid() &&
           target == other.target &&
           line == other.line &&
           column == other.column &&
           isLocalFile == other.isLocalFile;
}

QString parseOsc7Pwd(const QString &raw, const QString &localHostName) {
    if (raw.isEmpty() || hasControlChars(raw)) {
        return {};
    }
    if (raw.startsWith(QStringLiteral("kitty-shell-cwd://"))) {
        const QString rest = raw.mid(18);
        const qsizetype slashIdx = rest.indexOf(QLatin1Char('/'));
        if (slashIdx < 0) {
            return {};
        }
        const QString host = rest.left(slashIdx);
        const QString path = rest.mid(slashIdx);
        return isLocalHost(host, localHostName) ? path : QString();
    }
    if (raw.startsWith(QStringLiteral("file://"), Qt::CaseInsensitive)) {
        const QString rest = raw.mid(7);
        const qsizetype slashIdx = rest.indexOf(QLatin1Char('/'));
        if (slashIdx < 0) {
            return {};
        }
        const QString host = QUrl::fromPercentEncoding(rest.left(slashIdx).toUtf8());
        const QString path = QUrl::fromPercentEncoding(rest.mid(slashIdx).toUtf8());
        return (isLocalHost(host, localHostName) && path.startsWith(QLatin1Char('/'))) ? path : QString();
    }
    if (raw.startsWith(QLatin1Char('/'))) {
        return raw;
    }
    return {};
}

ResolvedLink resolveAt(GhosttyTerminal terminal,
                       uint16_t cols,
                       uint16_t rows,
                       uint16_t hoverCol,
                       uint16_t hoverRow,
                       const QString &cwd,
                       bool allowBareCwdFiles,
                       const QString &localHostName) {
    if (!terminal || cols == 0 || rows == 0 || hoverCol >= cols || hoverRow >= rows) {
        return {};
    }

    auto refAt = [&](uint16_t col, uint16_t row, GhosttyGridRef &outRef) -> bool {
        const GhosttyPoint pt{
            .tag = GHOSTTY_POINT_TAG_VIEWPORT,
            .value = {.coordinate = {.x = col, .y = row}},
        };
        outRef = {};
        return ghostty_terminal_grid_ref(terminal, pt, &outRef) == GHOSTTY_SUCCESS;
    };

    uint16_t effectiveCol = hoverCol;
    GhosttyGridRef hoverRef;
    if (!refAt(effectiveCol, hoverRow, hoverRef)) {
        return {};
    }

    GhosttyCell hoverCell{};
    if (ghostty_grid_ref_cell(&hoverRef, &hoverCell) != GHOSTTY_SUCCESS) {
        return {};
    }
    GhosttyCellWide hoverWide = GHOSTTY_CELL_WIDE_NARROW;
    ghostty_cell_get(hoverCell, GHOSTTY_CELL_DATA_WIDE, &hoverWide);
    if (hoverWide == GHOSTTY_CELL_WIDE_SPACER_TAIL && effectiveCol > 0) {
        --effectiveCol;
        if (!refAt(effectiveCol, hoverRow, hoverRef) ||
            ghostty_grid_ref_cell(&hoverRef, &hoverCell) != GHOSTTY_SUCCESS) {
            return {};
        }
    } else if (hoverWide == GHOSTTY_CELL_WIDE_SPACER_HEAD) {
        return {};
    }

    // Priority 1: Explicit OSC 8 hyperlink
    const QString osc8Uri = readOsc8Uri(hoverRef);
    if (!osc8Uri.isEmpty()) {
        auto stepBackward = [&](uint16_t r, uint16_t c, uint16_t &prevR, uint16_t &prevC) -> bool {
            if (c > 0) {
                prevR = r;
                prevC = static_cast<uint16_t>(c - 1);
                return true;
            }
            if (r > 0 && rowsSoftConnected(terminal, static_cast<uint16_t>(r - 1), r)) {
                prevR = static_cast<uint16_t>(r - 1);
                prevC = static_cast<uint16_t>(cols - 1);
                return true;
            }
            return false;
        };
        auto stepForward = [&](uint16_t r, uint16_t c, uint16_t &nextR, uint16_t &nextC) -> bool {
            if (c + 1 < cols) {
                nextR = r;
                nextC = static_cast<uint16_t>(c + 1);
                return true;
            }
            if (r + 1 < rows && rowsSoftConnected(terminal, r, static_cast<uint16_t>(r + 1))) {
                nextR = static_cast<uint16_t>(r + 1);
                nextC = 0;
                return true;
            }
            return false;
        };

        uint16_t startR = hoverRow;
        uint16_t startC = effectiveCol;
        for (;;) {
            uint16_t pr = 0;
            uint16_t pc = 0;
            if (!stepBackward(startR, startC, pr, pc)) {
                break;
            }
            GhosttyGridRef pRef;
            if (!refAt(pc, pr, pRef)) {
                break;
            }
            GhosttyCell pCell{};
            if (ghostty_grid_ref_cell(&pRef, &pCell) != GHOSTTY_SUCCESS) {
                break;
            }
            GhosttyCellWide pw = GHOSTTY_CELL_WIDE_NARROW;
            ghostty_cell_get(pCell, GHOSTTY_CELL_DATA_WIDE, &pw);
            if (pw == GHOSTTY_CELL_WIDE_SPACER_TAIL && pc > 0) {
                GhosttyGridRef headRef;
                if (refAt(static_cast<uint16_t>(pc - 1), pr, headRef) && readOsc8Uri(headRef) == osc8Uri) {
                    startR = pr;
                    startC = static_cast<uint16_t>(pc - 1);
                    continue;
                }
                break;
            }
            if (pw == GHOSTTY_CELL_WIDE_SPACER_HEAD) {
                if (pr > 0 && readOsc8Uri(pRef) == osc8Uri) {
                    startR = pr;
                    startC = pc;
                    continue;
                }
                break;
            }
            if (readOsc8Uri(pRef) != osc8Uri) {
                break;
            }
            startR = pr;
            startC = pc;
        }

        uint16_t endR = hoverRow;
        uint16_t endC = effectiveCol;
        for (;;) {
            uint16_t nr = 0;
            uint16_t nc = 0;
            if (!stepForward(endR, endC, nr, nc)) {
                break;
            }
            GhosttyGridRef nRef;
            if (!refAt(nc, nr, nRef)) {
                break;
            }
            GhosttyCell nCell{};
            if (ghostty_grid_ref_cell(&nRef, &nCell) != GHOSTTY_SUCCESS) {
                break;
            }
            GhosttyCellWide nw = GHOSTTY_CELL_WIDE_NARROW;
            ghostty_cell_get(nCell, GHOSTTY_CELL_DATA_WIDE, &nw);
            if (nw == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
                endR = nr;
                endC = nc;
                continue;
            }
            if (nw == GHOSTTY_CELL_WIDE_SPACER_HEAD) {
                if (nr + 1 < rows && rowsSoftConnected(terminal, nr, static_cast<uint16_t>(nr + 1))) {
                    endR = nr;
                    endC = nc;
                    continue;
                }
                break;
            }
            if (readOsc8Uri(nRef) != osc8Uri) {
                break;
            }
            endR = nr;
            endC = nc;
        }

        std::vector<CellCoord> oscCoords;
        QString rawText;
        uint16_t cr = startR;
        uint16_t cc = startC;
        for (;;) {
            GhosttyGridRef cRef;
            if (refAt(cc, cr, cRef)) {
                GhosttyCell cCell{};
                GhosttyCellWide cw = GHOSTTY_CELL_WIDE_NARROW;
                if (ghostty_grid_ref_cell(&cRef, &cCell) == GHOSTTY_SUCCESS) {
                    ghostty_cell_get(cCell, GHOSTTY_CELL_DATA_WIDE, &cw);
                }
                if (cw != GHOSTTY_CELL_WIDE_SPACER_HEAD) {
                    oscCoords.push_back(CellCoord{cr, cc, cc});
                    if (cw != GHOSTTY_CELL_WIDE_SPACER_TAIL) {
                        const QString txt = readRefText(cRef);
                        rawText.append(txt.isEmpty() ? QStringLiteral(" ") : txt);
                    }
                }
            }
            if (cr == endR && cc == endC) {
                break;
            }
            uint16_t nr = 0;
            uint16_t nc = 0;
            if (!stepForward(cr, cc, nr, nc)) {
                break;
            }
            cr = nr;
            cc = nc;
        }

        ResolvedLink link;
        link.rawText = rawText.trimmed();
        link.isOsc8 = true;
        link.spans = mergeSpans(oscCoords);

        if (osc8Uri.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
            QString localPath;
            int ln = 0;
            int col = 0;
            if (parseFileUriToLocal(osc8Uri, cwd, localHostName, /*requireExists=*/false, localPath, ln, col)) {
                link.target = localPath;
                link.line = ln;
                link.column = col;
                link.isLocalFile = true;
                return link;
            }
        } else if (!hasControlChars(osc8Uri)) {
            const QUrl url(osc8Uri, QUrl::TolerantMode);
            if (url.isValid() && !url.scheme().isEmpty()) {
                link.target = osc8Uri;
                link.isLocalFile = false;
                return link;
            }
        }
        return {};
    }

    // Priority 2 & 3 operate over the soft-wrapped logical row cluster containing hoverRow
    uint16_t clusterStart = hoverRow;
    while (clusterStart > 0 && rowsSoftConnected(terminal, static_cast<uint16_t>(clusterStart - 1), clusterStart)) {
        --clusterStart;
    }
    uint16_t clusterEnd = hoverRow;
    while (clusterEnd + 1 < rows && rowsSoftConnected(terminal, clusterEnd, static_cast<uint16_t>(clusterEnd + 1))) {
        ++clusterEnd;
    }

    QString logicalText;
    std::vector<CellCoord> cellMap;
    logicalText.reserve(static_cast<qsizetype>(clusterEnd - clusterStart + 1) * cols);
    cellMap.reserve(static_cast<size_t>(clusterEnd - clusterStart + 1) * cols);
    qsizetype hoverCharIdx = -1;

    for (uint16_t r = clusterStart; r <= clusterEnd; ++r) {
        int lastNonZeroCol = -1;
        if (r == clusterEnd) {
            for (int c = static_cast<int>(cols) - 1; c >= 0; --c) {
                GhosttyGridRef ref;
                GhosttyCell cell{};
                if (!refAt(static_cast<uint16_t>(c), r, ref) ||
                    ghostty_grid_ref_cell(&ref, &cell) != GHOSTTY_SUCCESS) {
                    continue;
                }
                GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
                ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &wide);
                if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
                    lastNonZeroCol = c;
                    break;
                }
                const QString ch = readRefText(ref);
                if (!ch.isEmpty() && ch != QStringLiteral(" ")) {
                    lastNonZeroCol = c;
                    break;
                }
            }
            if (lastNonZeroCol < 0) {
                continue;
            }
        }

        const uint16_t limitCol = (r == clusterEnd)
                                      ? static_cast<uint16_t>(lastNonZeroCol + 1)
                                      : cols;
        for (uint16_t c = 0; c < limitCol; ++c) {
            GhosttyGridRef ref;
            GhosttyCell cell{};
            if (!refAt(c, r, ref) || ghostty_grid_ref_cell(&ref, &cell) != GHOSTTY_SUCCESS) {
                continue;
            }
            GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
            ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &wide);
            if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD) {
                continue;
            }

            QString glyph = readRefText(ref);
            if (glyph.isEmpty()) {
                glyph = QStringLiteral(" ");
            }
            const uint16_t spanEndCol = (wide == GHOSTTY_CELL_WIDE_WIDE && c + 1 < cols)
                                            ? static_cast<uint16_t>(c + 1)
                                            : c;
            if (r == hoverRow && c == effectiveCol) {
                hoverCharIdx = logicalText.size();
            }
            for (qsizetype k = 0; k < glyph.size(); ++k) {
                logicalText.append(glyph.at(k));
                cellMap.push_back(CellCoord{r, c, spanEndCol});
            }
        }
    }

    if (hoverCharIdx < 0 || logicalText.isEmpty()) {
        return {};
    }

    auto buildSpansForRange = [&](qsizetype startIdx, qsizetype endIdx) -> QVector<LinkCellSpan> {
        std::vector<CellCoord> sub;
        sub.reserve(static_cast<size_t>(endIdx - startIdx));
        for (qsizetype i = startIdx; i < endIdx; ++i) {
            sub.push_back(cellMap[static_cast<size_t>(i)]);
        }
        return mergeSpans(sub);
    };

    // Priority 2: Ghostty 1.4.0 3-branch regex across logicalText
    const QRegularExpression &re = compiledLinkRegex();
    QRegularExpressionMatchIterator it = re.globalMatch(logicalText);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const qsizetype mStart = m.capturedStart();
        const qsizetype mEnd = m.capturedEnd();
        if (hoverCharIdx < mStart || hoverCharIdx >= mEnd) {
            continue;
        }

        const qsizetype trimmedEnd = trimCandidateEnd(logicalText, mStart, mEnd);
        if (hoverCharIdx >= trimmedEnd) {
            continue;
        }

        const QString rawMatch = logicalText.mid(mStart, trimmedEnd - mStart);
        if (isExplicitNonFileScheme(rawMatch)) {
            const QUrl url(rawMatch, QUrl::TolerantMode);
            if (url.isValid() && !url.scheme().isEmpty() && !hasControlChars(rawMatch)) {
                ResolvedLink link;
                link.rawText = rawMatch;
                link.target = rawMatch;
                link.isLocalFile = false;
                link.spans = buildSpansForRange(mStart, trimmedEnd);
                return link;
            }
            continue;
        }

        if (rawMatch.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
            QString localPath;
            int ln = 0;
            int col = 0;
            if (parseFileUriToLocal(rawMatch, cwd, localHostName, /*requireExists=*/true, localPath, ln, col)) {
                ResolvedLink link;
                link.rawText = rawMatch;
                link.target = localPath;
                link.line = ln;
                link.column = col;
                link.isLocalFile = true;
                link.spans = buildSpansForRange(mStart, trimmedEnd);
                return link;
            }
            continue;
        }

        // Filesystem path branch: try full trimmed match, then shorten at trailing spaces if needed
        qsizetype candidateEnd = trimmedEnd;
        while (candidateEnd > hoverCharIdx) {
            const QString sub = logicalText.mid(mStart, candidateEnd - mStart);
            QString localPath;
            int ln = 0;
            int col = 0;
            if (resolveExistingLocalPath(sub, cwd, localPath, ln, col)) {
                ResolvedLink link;
                link.rawText = sub;
                link.target = localPath;
                link.line = ln;
                link.column = col;
                link.isLocalFile = true;
                link.spans = buildSpansForRange(mStart, candidateEnd);
                return link;
            }
            const qsizetype lastSpace = sub.lastIndexOf(QLatin1Char(' '));
            if (lastSpace <= 0) {
                break;
            }
            candidateEnd = mStart + lastSpace;
        }
    }

    // Priority 3: Bare existing file/dir token in cwd (for non-prompt cells, e.g. `ls`, `rg`, `git status`)
    if (allowBareCwdFiles && !cwd.isEmpty()) {
        GhosttyCellSemanticContent semantic = GHOSTTY_CELL_SEMANTIC_OUTPUT;
        ghostty_cell_get(hoverCell, GHOSTTY_CELL_DATA_SEMANTIC_CONTENT, &semantic);
        if (semantic != GHOSTTY_CELL_SEMANTIC_PROMPT) {
            qsizetype tokStart = hoverCharIdx;
            while (tokStart > 0 && !isBareTokenDelimiter(logicalText.at(tokStart - 1))) {
                --tokStart;
            }
            qsizetype tokEnd = hoverCharIdx;
            while (tokEnd < logicalText.size() && !isBareTokenDelimiter(logicalText.at(tokEnd))) {
                ++tokEnd;
            }
            tokEnd = trimCandidateEnd(logicalText, tokStart, tokEnd);
            if (hoverCharIdx >= tokStart && hoverCharIdx < tokEnd) {
                const QString token = logicalText.mid(tokStart, tokEnd - tokStart);
                if (!token.isEmpty() &&
                    token != QStringLiteral(".") &&
                    token != QStringLiteral("..") &&
                    !token.startsWith(QLatin1Char('-')) &&
                    !isPureDigits(token) &&
                    !token.contains(QStringLiteral("://"))) {
                    QString localPath;
                    int ln = 0;
                    int col = 0;
                    if (resolveExistingLocalPath(token, cwd, localPath, ln, col)) {
                        ResolvedLink link;
                        link.rawText = token;
                        link.target = localPath;
                        link.line = ln;
                        link.column = col;
                        link.isLocalFile = true;
                        link.spans = buildSpansForRange(tokStart, tokEnd);
                        return link;
                    }
                }
            }
        }
    }

    return {};
}

bool openLink(const ResolvedLink &link) {
    if (!link.isValid() || hasControlChars(link.target)) {
        return false;
    }

    if (link.isLocalFile) {
        const QFileInfo fi(link.target);
        if (!fi.exists()) {
            return false;
        }
        const QString absPath = fi.absoluteFilePath();
        if (fi.isFile() && link.line > 0) {
            const QString kateExe = QStandardPaths::findExecutable(QStringLiteral("kate"));
            if (!kateExe.isEmpty()) {
                QStringList args;
                args << QStringLiteral("--line") << QString::number(link.line);
                if (link.column > 0) {
                    args << QStringLiteral("--column") << QString::number(link.column);
                }
                args << absPath;
                if (QProcess::startDetached(kateExe, args)) {
                    return true;
                }
            }
        }
        return QDesktopServices::openUrl(QUrl::fromLocalFile(absPath));
    }

    const QUrl url(link.target, QUrl::TolerantMode);
    if (!url.isValid() || url.scheme().isEmpty()) {
        return false;
    }
    return QDesktopServices::openUrl(url);
}

QString shellQuoteSingle(const QString &s) {
    if (s.isEmpty()) {
        return QStringLiteral("''");
    }
    bool allSafe = true;
    for (const QChar ch : s) {
        const ushort uc = ch.unicode();
        const bool safe = (uc >= 'A' && uc <= 'Z') ||
                          (uc >= 'a' && uc <= 'z') ||
                          (uc >= '0' && uc <= '9') ||
                          uc == '_' || uc == '.' || uc == '/' ||
                          uc == ':' || uc == '@' || uc == '+' || uc == '-';
        if (!safe) {
            allSafe = false;
            break;
        }
    }
    if (allSafe) {
        return s;
    }
    QString quoted = s;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

QString shellQuotePaths(const QList<QUrl> &urls) {
    QStringList parts;
    parts.reserve(urls.size());
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            parts.append(shellQuoteSingle(url.toLocalFile()));
        } else if (url.isValid()) {
            parts.append(shellQuoteSingle(url.toString()));
        }
    }
    return parts.join(QLatin1Char(' '));
}

} // namespace GhosttyUnionLink
