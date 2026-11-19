#include "KeyMap.h"
#include "MainWindow.h"
#include "StatsBarWidget.h"
#include "TerminalTabPane.h"
#include "TerminalWidget.h"

#include <QImage>
#include <QKeyEvent>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <algorithm>
#include <cmath>
#include <ghostty/vt.h>

#ifdef __linux__
#include <linux/input-event-codes.h>
#endif

class GhosttyUnionSmokeTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void testKeyEncoderSequences() {
        GhosttyKeyEncoder enc = nullptr;
        GhosttyKeyEvent ev = nullptr;
        QCOMPARE(ghostty_key_encoder_new(nullptr, &enc), GHOSTTY_SUCCESS);
        QCOMPARE(ghostty_key_event_new(nullptr, &ev), GHOSTTY_SUCCESS);

        auto encodeEvent = [&](GhosttyKittyKeyFlags kittyFlags,
                               const QKeyEvent &qev,
                               GhosttyKeyAction action = GHOSTTY_KEY_ACTION_PRESS) -> QByteArray {
            const bool altEsc = true;
            ghostty_key_encoder_setopt(enc, GHOSTTY_KEY_ENCODER_OPT_ALT_ESC_PREFIX, &altEsc);
            ghostty_key_encoder_setopt(enc, GHOSTTY_KEY_ENCODER_OPT_KITTY_FLAGS, &kittyFlags);

            QByteArray utf8Storage;
            GhosttyUnionKeyMap::populateGhosttyKeyEvent(ev, &qev, action, utf8Storage);

            char buf[128];
            size_t len = 0;
            if (ghostty_key_encoder_encode(enc, ev, buf, sizeof(buf), &len) != GHOSTTY_SUCCESS) {
                return QByteArrayLiteral("<ENCODE_ERROR>");
            }
            return QByteArray(buf, static_cast<qsizetype>(len));
        };

        auto encodeQt = [&](GhosttyKittyKeyFlags kittyFlags,
                            int qtKey,
                            Qt::KeyboardModifiers mods,
                            const QString &text,
                            quint32 nativeScanCode = 0,
                            quint32 nativeVirtualKey = 0,
                            quint32 nativeModifiers = 0) -> QByteArray {
            const QKeyEvent qev(QEvent::KeyPress, qtKey, mods,
                                nativeScanCode, nativeVirtualKey, nativeModifiers, text);
            return encodeEvent(kittyFlags, qev);
        };

        const GhosttyKittyKeyFlags kfLegacy = GHOSTTY_KITTY_KEY_DISABLED;
        const GhosttyKittyKeyFlags kfDisamb = GHOSTTY_KITTY_KEY_DISAMBIGUATE;
        const GhosttyKittyKeyFlags kfAlt = GHOSTTY_KITTY_KEY_DISAMBIGUATE | GHOSTTY_KITTY_KEY_REPORT_ALTERNATES;
        const GhosttyKittyKeyFlags kfAll = GHOSTTY_KITTY_KEY_ALL;

        // Basic keys & legacy compatibility
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_A, Qt::NoModifier, QStringLiteral("a")), QByteArray("a"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_C, Qt::ControlModifier, QStringLiteral("\x03")), QByteArray("\x03"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r")), QByteArray("\r"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_Up, Qt::NoModifier, QString()), QByteArray("\x1b[A"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_Backtab, Qt::ShiftModifier, QString()), QByteArray("\x1b[Z"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_Backspace, Qt::NoModifier, QStringLiteral("\x7f")), QByteArray("\x7f"));

        // Shift+uppercase letters across legacy and Kitty keyboard protocol flags
        for (const GhosttyKittyKeyFlags kf : {kfLegacy, kfDisamb, kfAlt}) {
            QCOMPARE(encodeQt(kf, Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A")), QByteArray("A"));
            QCOMPARE(encodeQt(kf, Qt::Key_Z, Qt::ShiftModifier, QStringLiteral("Z")), QByteArray("Z"));
        }
        QCOMPARE(encodeQt(kfAll, Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A")),
                 QByteArray("\x1b[97:65;2;65u"));

        // Shifted digits & punctuation (regression for unshiftedCodepoint + consumed Shift)
        for (const GhosttyKittyKeyFlags kf : {kfLegacy, kfDisamb, kfAlt}) {
            QCOMPARE(encodeQt(kf, Qt::Key_Exclam, Qt::ShiftModifier, QStringLiteral("!")), QByteArray("!"));
            QCOMPARE(encodeQt(kf, Qt::Key_Underscore, Qt::ShiftModifier, QStringLiteral("_")), QByteArray("_"));
            QCOMPARE(encodeQt(kf, Qt::Key_Question, Qt::ShiftModifier, QStringLiteral("?")), QByteArray("?"));
            QCOMPARE(encodeQt(kf, Qt::Key_AsciiTilde, Qt::ShiftModifier, QStringLiteral("~")), QByteArray("~"));
        }
        QCOMPARE(encodeQt(kfAll, Qt::Key_Exclam, Qt::ShiftModifier, QStringLiteral("!")),
                 QByteArray("\x1b[49:33;2;33u"));

        // CapsLock & CapsLock+Shift (XKB Lock bit 0x02)
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_A, Qt::NoModifier, QStringLiteral("A"), 0, 0, 0x02u), QByteArray("A"));
        QCOMPARE(encodeQt(kfDisamb, Qt::Key_A, Qt::NoModifier, QStringLiteral("A"), 0, 0, 0x02u), QByteArray("A"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_A, Qt::ShiftModifier, QStringLiteral("a"), 0, 0, 0x02u), QByteArray("a"));
        QCOMPARE(encodeQt(kfDisamb, Qt::Key_A, Qt::ShiftModifier, QStringLiteral("a"), 0, 0, 0x02u), QByteArray("a"));

        // Shift+Space / Shift+Tab / Shift+Return
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_Space, Qt::ShiftModifier, QStringLiteral(" ")), QByteArray(" "));
        QCOMPARE(encodeQt(kfDisamb, Qt::Key_Space, Qt::ShiftModifier, QStringLiteral(" ")), QByteArray("\x1b[32;2u"));
        QCOMPARE(encodeQt(kfDisamb, Qt::Key_Backtab, Qt::ShiftModifier, QString()), QByteArray("\x1b[9;2u"));
        QCOMPARE(encodeQt(kfDisamb, Qt::Key_Return, Qt::ShiftModifier, QStringLiteral("\r")), QByteArray("\x1b[13;2u"));

        // Ctrl+Shift, Ctrl+Space, Alt+Shift, Super
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_AsciiCircum, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("\x1e")),
                 QByteArray("\x1e"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_Underscore, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("\x1f")),
                 QByteArray("\x1f"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_Space, Qt::ControlModifier, QString()), QByteArray("\x00", 1));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_A, Qt::AltModifier, QString()), QByteArray("\x1b" "a"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_A, Qt::AltModifier | Qt::ShiftModifier, QStringLiteral("A")),
                 QByteArray("\x1b" "A"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_Greater, Qt::AltModifier | Qt::ShiftModifier, QStringLiteral(">")),
                 QByteArray("\x1b>"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_A, Qt::MetaModifier, QStringLiteral("a")), QByteArray());
        QCOMPARE(encodeQt(kfDisamb, Qt::Key_A, Qt::MetaModifier, QStringLiteral("a")), QByteArray("\x1b[97;9u"));

        // Keypad mapping: NumLock ON vs NumLock OFF (even when evdev scancode is KEY_KP7)
#ifdef __linux__
        const quint32 kp7Scan = KEY_KP7 + 8;
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_7, kp7Scan, Qt::KeypadModifier), GHOSTTY_KEY_NUMPAD_7);
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_7, Qt::KeypadModifier, QStringLiteral("7"), kp7Scan, 0, 0x10u),
                 QByteArray("7"));
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Home, kp7Scan, Qt::KeypadModifier), GHOSTTY_KEY_NUMPAD_HOME);
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_Home, Qt::KeypadModifier, QString(), kp7Scan, 0, 0),
                 QByteArray("\x1b[H"));

        // Linux XKB/evdev scancodes (modifiers, international, F13-F24, system, media, clipboard)
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Shift, KEY_LEFTSHIFT + 8), GHOSTTY_KEY_SHIFT_LEFT);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Shift, KEY_RIGHTSHIFT + 8), GHOSTTY_KEY_SHIFT_RIGHT);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Control, KEY_LEFTCTRL + 8), GHOSTTY_KEY_CONTROL_LEFT);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Control, KEY_RIGHTCTRL + 8), GHOSTTY_KEY_CONTROL_RIGHT);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Alt, KEY_LEFTALT + 8), GHOSTTY_KEY_ALT_LEFT);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Alt, KEY_RIGHTALT + 8), GHOSTTY_KEY_ALT_RIGHT);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Meta, KEY_LEFTMETA + 8), GHOSTTY_KEY_META_LEFT);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Meta, KEY_RIGHTMETA + 8), GHOSTTY_KEY_META_RIGHT);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Less, KEY_102ND + 8), GHOSTTY_KEY_INTL_BACKSLASH);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_F13, KEY_F13 + 8), GHOSTTY_KEY_F13);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_F24, KEY_F24 + 8), GHOSTTY_KEY_F24);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Print, KEY_SYSRQ + 8), GHOSTTY_KEY_PRINT_SCREEN);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_ScrollLock, KEY_SCROLLLOCK + 8), GHOSTTY_KEY_SCROLL_LOCK);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Pause, KEY_PAUSE + 8), GHOSTTY_KEY_PAUSE);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_VolumeUp, KEY_VOLUMEUP + 8), GHOSTTY_KEY_AUDIO_VOLUME_UP);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Copy, KEY_COPY + 8), GHOSTTY_KEY_COPY);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Paste, KEY_PASTE + 8), GHOSTTY_KEY_PASTE);
#endif
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Enter, 0), GHOSTTY_KEY_NUMPAD_ENTER);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Plus, 0, Qt::KeypadModifier), GHOSTTY_KEY_NUMPAD_ADD);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_F13, 0), GHOSTTY_KEY_F13);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_F25, 0), GHOSTTY_KEY_F25);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Print, 0), GHOSTTY_KEY_PRINT_SCREEN);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_ScrollLock, 0), GHOSTTY_KEY_SCROLL_LOCK);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Pause, 0), GHOSTTY_KEY_PAUSE);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Help, 0), GHOSTTY_KEY_HELP);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_AltGr, 0), GHOSTTY_KEY_ALT_RIGHT);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Super_L, 0), GHOSTTY_KEY_META_LEFT);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Super_R, 0), GHOSTTY_KEY_META_RIGHT);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_MediaPlay, 0), GHOSTTY_KEY_MEDIA_PLAY_PAUSE);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Copy, 0), GHOSTTY_KEY_COPY);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Cut, 0), GHOSTTY_KEY_CUT);
        QCOMPARE(GhosttyUnionKeyMap::mapQtKey(Qt::Key_Paste, 0), GHOSTTY_KEY_PASTE);

        QVERIFY(GhosttyUnionKeyMap::isModifierKey(GHOSTTY_KEY_SHIFT_LEFT));
        QVERIFY(GhosttyUnionKeyMap::isModifierKey(GHOSTTY_KEY_SHIFT_RIGHT));
        QVERIFY(GhosttyUnionKeyMap::isModifierKey(GHOSTTY_KEY_CONTROL_RIGHT));
        QVERIFY(GhosttyUnionKeyMap::isModifierKey(GHOSTTY_KEY_ALT_RIGHT));
        QVERIFY(GhosttyUnionKeyMap::isModifierKey(GHOSTTY_KEY_META_RIGHT));
        QVERIFY(GhosttyUnionKeyMap::isModifierKey(GHOSTTY_KEY_CAPS_LOCK));
        QVERIFY(GhosttyUnionKeyMap::isModifierKey(GHOSTTY_KEY_NUM_LOCK));
        QVERIFY(!GhosttyUnionKeyMap::isModifierKey(GHOSTTY_KEY_A));

        ghostty_key_event_free(ev);
        ghostty_key_encoder_free(enc);
    }

    void testStatsBarLiveSensors() {
        StatsBarWidget bar;
        bar.show();
        QVERIFY(QTest::qWaitForWindowExposed(&bar));
        QCOMPARE(bar.height(), 40);

        QTRY_VERIFY_WITH_TIMEOUT(bar.summaryTexts().size() == 5, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!bar.summaryTexts()[0].contains(QLatin1String("--%")), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!bar.summaryTexts()[1].contains(QLatin1String("--%")), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(bar.summaryTexts()[1].contains(QLatin1Char('G')), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(bar.historySampleCount() >= 1, 3000);

        qInfo() << "StatsBar sample:" << bar.summaryTexts() << "samples=" << bar.historySampleCount();

        bar.setSensorsEnabled(false);
        bar.setSensorsEnabled(true);
    }

    void testMainWindowTerminalSplitsAndIo() {
        MainWindow win;
        win.resize(900, 600);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        const auto *tabs = win.findChild<QTabWidget *>();
        QVERIFY(tabs != nullptr);
        QCOMPARE(tabs->count(), 1);
        QVERIFY(!tabs->tabBar()->autoHide());

        auto *pane = qobject_cast<TerminalTabPane *>(tabs->currentWidget());
        QVERIFY(pane != nullptr);
        TerminalWidget *term = pane->activeTerminal();
        QVERIFY(term != nullptr);

        // Verify live shell responds to typed input through libghostty-vt
        QTest::keyClicks(term, QStringLiteral("echo __GHOSTTY_UNION_OK__"));
        QTest::keyClick(term, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(
            term->plainTextSnapshot().contains(QLatin1String("__GHOSTTY_UNION_OK__")),
            3000);

        // Verify horizontal split + zoom toggle
        pane->splitActive(Qt::Horizontal);
        QCOMPARE(pane->findChildren<TerminalWidget *>().size(), 2);
        QVERIFY(pane->activeTerminal() != term);

        pane->toggleZoom();
        QVERIFY(!pane->activeTerminal()->isHidden());
        QVERIFY(term->isHidden());

        pane->toggleZoom();
        QVERIFY(!term->isHidden());
    }

    void testTabDoesNotStealTerminalFocus() {
        MainWindow win;
        win.resize(900, 600);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        const auto *tabs = win.findChild<QTabWidget *>();
        QVERIFY(tabs != nullptr);
        auto *pane = qobject_cast<TerminalTabPane *>(tabs->currentWidget());
        QVERIFY(pane != nullptr);
        TerminalWidget *term = pane->activeTerminal();
        QVERIFY(term != nullptr);
        term->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(term->hasFocus(), 1000);

        QTest::keyClick(term, Qt::Key_Tab);
        QVERIFY(term->hasFocus());
        QTest::keyClick(term, Qt::Key_Backtab, Qt::ShiftModifier);
        QVERIFY(term->hasFocus());
    }

    void testP10kPromptSpacingAndGlyphs() {
        TerminalWidget term(QString(), nullptr, /*startShell=*/false);
        term.resize(900, 260);
        term.show();
        QVERIFY(QTest::qWaitForWindowExposed(&term));

        const QSize cs = term.cellSize();
        const int cw = cs.width();
        const int ch = cs.height();
        const QPoint pad = TerminalWidget::viewportOffset();
        QVERIFY(cw >= 4);
        QVERIFY(ch >= 8);

        // Construct a representative p10k multiline prompt matching ~/.p10k.zsh (cols 0..41 on row 0)
        const QString vtSeq =
            QStringLiteral("\x1b[38;2;118;131;144m\u256D\u2500"
                           "\x1b[38;2;33;136;255m\u2591\u2592\u2593"
                           "\x1b[48;2;33;136;255;38;2;255;255;255m \uF303 "
                           "\x1b[38;2;33;136;255;48;2;48;54;61m\uE0B0"
                           "\x1b[38;2;230;237;243;48;2;48;54;61m \uF489 ghostty-union "
                           "\x1b[38;2;48;54;61;49m\uE0B0"
                           "\x1b[38;2;118;131;144m\u2500\u2500\u2500\u2500\u2500\u2500"
                           "\x1b[38;2;48;54;61m\uE0B2"
                           "\x1b[48;2;48;54;61;38;2;88;166;255m \U000F10FE "
                           "\x1b[49;38;2;48;54;61m\u2593\u2592\u2591"
                           "\x1b[38;2;118;131;144m\u2500\u256E\r\n"
                           "\x1b[0;38;2;118;131;144m\u2570\u2500\x1b[0m \x1b[38;2;63;185;80m\u276F\x1b[0m ");
        term.feedVtBytes(vtSeq.toUtf8());

        const QString snap = term.plainTextSnapshot();
        QVERIFY(snap.contains(QStringLiteral("ghostty-union")));
        QVERIFY(snap.contains(QStringLiteral("\uF303")));
        QVERIFY(snap.contains(QStringLiteral("\U000F10FE")));
        QVERIFY(snap.contains(QStringLiteral("\u256D\u2500")));
        QVERIFY(snap.contains(QStringLiteral("\u2570\u2500")));

        const QImage img = term.grab().toImage();
        const qreal imgDpr = img.devicePixelRatio();
        QVERIFY(!img.isNull());

        auto colorDist = [](const QColor &a, const QColor &b) {
            return std::abs(a.red() - b.red()) +
                   std::abs(a.green() - b.green()) +
                   std::abs(a.blue() - b.blue());
        };

        auto cellMaxDistFromBg = [&](int col, int row, const QColor &bgCol, int insetX, int insetY) {
            const int xStart = std::max(0, static_cast<int>(std::ceil((pad.x() + col * cw + insetX) * imgDpr)));
            const int xEnd = std::min(img.width(), static_cast<int>(std::floor((pad.x() + (col + 1) * cw - insetX) * imgDpr)));
            const int yStart = std::max(0, static_cast<int>(std::ceil((pad.y() + row * ch + insetY) * imgDpr)));
            const int yEnd = std::min(img.height(), static_cast<int>(std::floor((pad.y() + (row + 1) * ch - insetY) * imgDpr)));
            int maxDist = 0;
            for (int y = yStart; y < yEnd; ++y) {
                for (int x = xStart; x < xEnd; ++x) {
                    maxDist = std::max(maxDist, colorDist(img.pixelColor(x, y), bgCol));
                }
            }
            return maxDist;
        };

        const QColor seg1Bg(0x21, 0x88, 0xff);
        const QColor seg2Bg(0x30, 0x36, 0x3d);

        // Icon cells (col 6 = U+F303, col 10 = U+F489, col 35 = U+F10FE) must render visible fg ink
        QVERIFY2(cellMaxDistFromBg(6, 0, seg1Bg, 0, 0) > 80, "U+F303 icon did not render visible ink");
        QVERIFY2(cellMaxDistFromBg(10, 0, seg2Bg, 0, 0) > 80, "U+F489 icon did not render visible ink");
        QVERIFY2(cellMaxDistFromBg(35, 0, seg2Bg, 0, 0) > 40, "U+F10FE Supplementary PUA-A icon did not render visible ink");

        // Adjacent right space cells (col 7, col 11, col 36) must stay pure segment background (zero overflow)
        QVERIFY2(cellMaxDistFromBg(7, 0, seg1Bg, 1, 1) <= 4, "U+F303 overflowed into right space padding cell");
        QVERIFY2(cellMaxDistFromBg(11, 0, seg2Bg, 1, 1) <= 4, "U+F489 overflowed into right space padding cell");
        QVERIFY2(cellMaxDistFromBg(36, 0, seg2Bg, 1, 1) <= 4, "U+F10FE overflowed into right space padding cell");

        // Horizontal box-drawing filler dashes (cols 28..30, U+2500) must connect with zero gaps
        const QColor gapLineCol(118, 131, 144);
        const int gx0 = static_cast<int>(std::round((pad.x() + 28 * cw) * imgDpr));
        const int gx1 = static_cast<int>(std::round((pad.x() + 31 * cw) * imgDpr));
        const int gy0 = static_cast<int>(std::floor(pad.y() * imgDpr));
        const int gy1 = static_cast<int>(std::ceil((pad.y() + ch) * imgDpr));
        for (int x = gx0; x < gx1; ++x) {
            bool hasLinePx = false;
            for (int y = gy0; y < gy1; ++y) {
                if (colorDist(img.pixelColor(x, y), gapLineCol) <= 20) {
                    hasLinePx = true;
                    break;
                }
            }
            QVERIFY2(hasLinePx, qPrintable(QStringLiteral("Horizontal gap discontinuity at x=%1").arg(x)));
        }

        // Powerline E0B0 left edge (col 8) and E0B2 right edge (col 33) must be flush top-to-bottom
        const int plRightX = static_cast<int>(std::round((pad.x() + 8 * cw) * imgDpr));
        const int plLeftEdgeX = static_cast<int>(std::round((pad.x() + 34 * cw) * imgDpr)) - 1;
        const int pyMin = static_cast<int>(std::ceil((pad.y() + 2) * imgDpr));
        const int pyMax = static_cast<int>(std::floor((pad.y() + ch - 2) * imgDpr));
        for (int y = pyMin; y < pyMax; ++y) {
            QVERIFY2(colorDist(img.pixelColor(plRightX, y), seg1Bg) <= 12,
                     qPrintable(QStringLiteral("Powerline E0B0 left seam at y=%1").arg(y)));
            QVERIFY2(colorDist(img.pixelColor(plLeftEdgeX, y), seg2Bg) <= 12,
                     qPrintable(QStringLiteral("Powerline E0B2 right seam at y=%1").arg(y)));
        }

        // Shade block ▓ (col 37, U+2593) must fill full row height without vertical step against col 36
        const QColor termBg = img.pixelColor(0, 0);
        const int shadeMidX = static_cast<int>(std::round((pad.x() + 37 * cw + cw / 2) * imgDpr));
        const int topRowY = static_cast<int>(std::floor(pad.y() * imgDpr));
        const int botRowY = static_cast<int>(std::ceil((pad.y() + ch) * imgDpr)) - 1;
        QVERIFY2(colorDist(img.pixelColor(shadeMidX, topRowY), termBg) > 8, "Shade U+2593 stepped down from row top");
        QVERIFY2(colorDist(img.pixelColor(shadeMidX, botRowY), termBg) > 8, "Shade U+2593 clipped above row bottom");
    }
};

QTEST_MAIN(GhosttyUnionSmokeTest)
#include "smoke_test.moc"
