#include "KeyMap.h"
#include "MainWindow.h"
#include "StatsBarWidget.h"
#include "TerminalLink.h"
#include "TerminalTabPane.h"
#include "TerminalWidget.h"

#include <QAbstractButton>
#include <QApplication> // IWYU pragma: keep
#include <QClipboard>
#include <QCoreApplication>
#include <QDir> // IWYU pragma: keep
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent> // IWYU pragma: keep
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSysInfo>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir> // IWYU pragma: keep
#include <QTest>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <vector>
#include <ghostty/vt.h>

#ifdef __linux__
#include <linux/input-event-codes.h>
#endif

namespace {

class GhosttyUnionSmokeTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    [[maybe_unused]] static void testKeyEncoderSequences() {
        GhosttyKeyEncoder enc = nullptr;
        GhosttyKeyEvent ev = nullptr;
        QCOMPARE(ghostty_key_encoder_new(nullptr, &enc), GHOSTTY_SUCCESS);
        QCOMPARE(ghostty_key_event_new(nullptr, &ev), GHOSTTY_SUCCESS);

        auto encodeEvent = [&](GhosttyKittyKeyFlags kittyFlags,
                               const QKeyEvent &qev,
                               GhosttyKeyAction action = GHOSTTY_KEY_ACTION_PRESS) -> QByteArray {
            constexpr bool altEsc = true;
            ghostty_key_encoder_setopt(enc, GHOSTTY_KEY_ENCODER_OPT_ALT_ESC_PREFIX, &altEsc);
            ghostty_key_encoder_setopt(enc, GHOSTTY_KEY_ENCODER_OPT_KITTY_FLAGS, &kittyFlags);

            QByteArray utf8Storage;
            GhosttyUnionKeyMap::populateGhosttyKeyEvent(ev, &qev, action, utf8Storage);

            char buf[128];
            size_t len = 0;
            if (ghostty_key_encoder_encode(enc, ev, buf, sizeof(buf), &len) != GHOSTTY_SUCCESS) {
                return QByteArrayLiteral("<ENCODE_ERROR>");
            }
            return {buf, static_cast<qsizetype>(len)};
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

        constexpr GhosttyKittyKeyFlags kfLegacy = GHOSTTY_KITTY_KEY_DISABLED;
        constexpr GhosttyKittyKeyFlags kfDisambiguate = GHOSTTY_KITTY_KEY_DISAMBIGUATE;
        constexpr GhosttyKittyKeyFlags kfAlt = GHOSTTY_KITTY_KEY_DISAMBIGUATE | GHOSTTY_KITTY_KEY_REPORT_ALTERNATES;
        constexpr GhosttyKittyKeyFlags kfAll = GHOSTTY_KITTY_KEY_ALL;

        // Basic keys & legacy compatibility
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_A, Qt::NoModifier, QStringLiteral("a")), QByteArray("a"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_C, Qt::ControlModifier, QStringLiteral("\x03")), QByteArray("\x03"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r")), QByteArray("\r"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_Up, Qt::NoModifier, QString()), QByteArray("\x1b[A"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_Backtab, Qt::ShiftModifier, QString()), QByteArray("\x1b[Z"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_Backspace, Qt::NoModifier, QStringLiteral("\x7f")), QByteArray("\x7f"));

        // Shift+uppercase letters across legacy and Kitty keyboard protocol flags
        for (const GhosttyKittyKeyFlags kf : {kfLegacy, kfDisambiguate, kfAlt}) {
            QCOMPARE(encodeQt(kf, Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A")), QByteArray("A"));
            QCOMPARE(encodeQt(kf, Qt::Key_Z, Qt::ShiftModifier, QStringLiteral("Z")), QByteArray("Z"));
        }
        QCOMPARE(encodeQt(kfAll, Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A")),
                 QByteArray("\x1b[97:65;2;65u"));

        // Shifted digits & punctuation (regression for unshiftedCodepoint + consumed Shift)
        for (const GhosttyKittyKeyFlags kf : {kfLegacy, kfDisambiguate, kfAlt}) {
            QCOMPARE(encodeQt(kf, Qt::Key_Exclam, Qt::ShiftModifier, QStringLiteral("!")), QByteArray("!"));
            QCOMPARE(encodeQt(kf, Qt::Key_Underscore, Qt::ShiftModifier, QStringLiteral("_")), QByteArray("_"));
            QCOMPARE(encodeQt(kf, Qt::Key_Question, Qt::ShiftModifier, QStringLiteral("?")), QByteArray("?"));
            QCOMPARE(encodeQt(kf, Qt::Key_AsciiTilde, Qt::ShiftModifier, QStringLiteral("~")), QByteArray("~"));
        }
        QCOMPARE(encodeQt(kfAll, Qt::Key_Exclam, Qt::ShiftModifier, QStringLiteral("!")),
                 QByteArray("\x1b[49:33;2;33u"));

        // CapsLock & CapsLock+Shift (XKB Lock bit 0x02)
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_A, Qt::NoModifier, QStringLiteral("A"), 0, 0, 0x02u), QByteArray("A"));
        QCOMPARE(encodeQt(kfDisambiguate, Qt::Key_A, Qt::NoModifier, QStringLiteral("A"), 0, 0, 0x02u), QByteArray("A"));
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_A, Qt::ShiftModifier, QStringLiteral("a"), 0, 0, 0x02u), QByteArray("a"));
        QCOMPARE(encodeQt(kfDisambiguate, Qt::Key_A, Qt::ShiftModifier, QStringLiteral("a"), 0, 0, 0x02u), QByteArray("a"));

        // Shift+Space / Shift+Tab / Shift+Return
        QCOMPARE(encodeQt(kfLegacy, Qt::Key_Space, Qt::ShiftModifier, QStringLiteral(" ")), QByteArray(" "));
        QCOMPARE(encodeQt(kfDisambiguate, Qt::Key_Space, Qt::ShiftModifier, QStringLiteral(" ")), QByteArray("\x1b[32;2u"));
        QCOMPARE(encodeQt(kfDisambiguate, Qt::Key_Backtab, Qt::ShiftModifier, QString()), QByteArray("\x1b[9;2u"));
        QCOMPARE(encodeQt(kfDisambiguate, Qt::Key_Return, Qt::ShiftModifier, QStringLiteral("\r")), QByteArray("\x1b[13;2u"));

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
        QCOMPARE(encodeQt(kfDisambiguate, Qt::Key_A, Qt::MetaModifier, QStringLiteral("a")), QByteArray("\x1b[97;9u"));

        // Keypad mapping: NumLock ON vs NumLock OFF (even when evdev scancode is KEY_KP7)
#ifdef __linux__
        constexpr quint32 kp7Scan = KEY_KP7 + 8;
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

    [[maybe_unused]] static void testStatsBarLiveSensors() {
        StatsBarWidget bar;
        bar.show();
        QVERIFY(QTest::qWaitForWindowExposed(&bar));
        QCOMPARE(bar.height(), 40);

        const QStringList procNvidia = StatsBarWidget::readConnectedNvidiaModelsFromProc();
        if (!procNvidia.isEmpty()) {
            QTRY_COMPARE_WITH_TIMEOUT(bar.gpuCardCount(), static_cast<int>(procNvidia.size()), 3000);
            QVERIFY(!bar.gpuTags().contains(QLatin1String("GPU0")));
        } else {
            QVERIFY(bar.gpuCardCount() >= 1);
        }
        QTRY_VERIFY_WITH_TIMEOUT(bar.summaryTexts().size() == 4 + bar.gpuCardCount(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!bar.summaryTexts()[0].contains(QLatin1String("--%")), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!bar.summaryTexts()[1].contains(QLatin1String("--%")), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(bar.summaryTexts()[1].contains(QLatin1Char('G')), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(bar.historySampleCount() >= 1, 3000);

        qInfo() << "StatsBar GPUs:" << bar.gpuTags()
                << "sample:" << bar.summaryTexts()
                << "samples=" << bar.historySampleCount();

        bar.setSensorsEnabled(false);
        bar.setSensorsEnabled(true);
    }

    [[maybe_unused]] static void testStatsBarPrivateAndPublicIps() {
        QCOMPARE(StatsBarWidget::parsePublicIpv4Response(QByteArray("203.0.113.42\n")),
                 QStringLiteral("203.0.113.42"));
        QCOMPARE(StatsBarWidget::parsePublicIpv4Response(QByteArray(" 1.1.1.1 ")),
                 QStringLiteral("1.1.1.1"));
        QCOMPARE(StatsBarWidget::parsePublicIpv4Response(QByteArray("192.168.1.10")), QString());
        QCOMPARE(StatsBarWidget::parsePublicIpv4Response(QByteArray("10.0.0.1")), QString());
        QCOMPARE(StatsBarWidget::parsePublicIpv4Response(QByteArray("172.20.20.20")), QString());
        QCOMPARE(StatsBarWidget::parsePublicIpv4Response(QByteArray("127.0.0.1")), QString());
        QCOMPARE(StatsBarWidget::parsePublicIpv4Response(QByteArray("2001:db8::1")), QString());
        QCOMPARE(StatsBarWidget::parsePublicIpv4Response(QByteArray("<html>502</html>")), QString());
        QCOMPARE(StatsBarWidget::parsePublicIpv4Response(QByteArray(128, '1')), QString());

        QVERIFY(StatsBarWidget::isVirtualOrTunnelInterface(QStringLiteral("lo")));
        QVERIFY(StatsBarWidget::isVirtualOrTunnelInterface(QStringLiteral("docker0")));
        QVERIFY(StatsBarWidget::isVirtualOrTunnelInterface(QStringLiteral("br-154dd75b2740")));
        QVERIFY(StatsBarWidget::isVirtualOrTunnelInterface(QStringLiteral("veth123")));
        QVERIFY(StatsBarWidget::isVirtualOrTunnelInterface(QStringLiteral("home-lan")));
        QVERIFY(StatsBarWidget::isVirtualOrTunnelInterface(QStringLiteral("wg0")));
        QVERIFY(StatsBarWidget::isVirtualOrTunnelInterface(QStringLiteral("tun0")));
        QVERIFY(StatsBarWidget::isVirtualOrTunnelInterface(QStringLiteral("tailscale0")));
        QVERIFY(!StatsBarWidget::isVirtualOrTunnelInterface(QStringLiteral("wlan0")));
        QVERIFY(!StatsBarWidget::isVirtualOrTunnelInterface(QStringLiteral("wlp2s0")));
        QVERIFY(!StatsBarWidget::isVirtualOrTunnelInterface(QStringLiteral("eth0")));
        QVERIFY(!StatsBarWidget::isVirtualOrTunnelInterface(QStringLiteral("enp3s0")));

        StatsBarWidget bar;
        bar.resize(1200, 40);
        bar.show();
        QVERIFY(QTest::qWaitForWindowExposed(&bar));
        QCOMPARE(bar.height(), 40);

        bar.setPublicIpForTesting(QStringLiteral("203.0.113.42"));
        QCOMPARE(bar.publicIp(), QStringLiteral("203.0.113.42"));

        const int gpuCount = bar.gpuCardCount();
        QVERIFY(gpuCount >= 1);
        const QStringList summaries = bar.summaryTexts();
        QCOMPARE(summaries.size(), 4 + gpuCount);
        const int netIdx = 2 + gpuCount;
        QVERIFY(summaries[netIdx].contains(QLatin1String("PRI ")));
        QVERIFY(summaries[netIdx].contains(QLatin1String("PUB 203.0.113.42")));

        QVERIFY(bar.layout() != nullptr);
        QWidget *netCard = bar.layout()->itemAt(netIdx) ? bar.layout()->itemAt(netIdx)->widget() : nullptr;
        QVERIFY(netCard != nullptr);
        QCOMPARE(netCard->height(), 32);

        if (!bar.privateIp().isEmpty()) {
            QApplication::clipboard()->setText(QStringLiteral("stale"));
            QTest::mouseClick(netCard, Qt::LeftButton, Qt::NoModifier, QPoint(20, 6));
            QCOMPARE(QApplication::clipboard()->text(), bar.privateIp());
        } else {
            QVERIFY(summaries[netIdx].contains(QLatin1String("PRI offline")));
        }

        QApplication::clipboard()->setText(QStringLiteral("stale"));
        QTest::mouseClick(netCard, Qt::LeftButton, Qt::NoModifier, QPoint(20, 24));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("203.0.113.42"));
    }

    [[maybe_unused]] static void testStatsBarFullWidthEvenGrowthAndShrink() {
        StatsBarWidget bar;
        bar.resize(1220, 40);
        bar.show();
        QVERIFY(QTest::qWaitForWindowExposed(&bar));
        bar.setGpusForTesting({
            {QStringLiteral("gpu/gpu0"), QStringLiteral("NVIDIA GeForce RTX 5070 Ti Laptop GPU"), {}, true},
            {QStringLiteral("gpu/gpu1"), QStringLiteral("NVIDIA GeForce RTX 5080"), {}, true},
        });
        QVERIFY(bar.layout() != nullptr);
        const int itemCount = bar.layout()->count();
        QCOMPARE(itemCount, 7);

        std::vector<int> hintWidths(static_cast<size_t>(itemCount), 0);
        int sumHints = 0;
        for (int i = 0; i < itemCount; ++i) {
            QLayoutItem *li = bar.layout()->itemAt(i);
            QVERIFY(li != nullptr);
            QVERIFY(li->widget() != nullptr);
            hintWidths[static_cast<size_t>(i)] = li->sizeHint().width();
            QVERIFY(hintWidths[static_cast<size_t>(i)] > 0);
            sumHints += hintWidths[static_cast<size_t>(i)];
        }
        QCOMPARE(bar.layout()->itemAt(itemCount - 1)->widget()->objectName(), QStringLiteral("ClockPill"));

        auto snapshotWidths = [&](int targetW) -> std::vector<int> {
            bar.resize(targetW, 40);
            bar.layout()->invalidate();
            bar.layout()->activate();
            QApplication::processEvents();
            std::vector<int> widths(static_cast<size_t>(itemCount), 0);
            if (bar.width() != targetW) {
                QTest::qFail(qPrintable(QStringLiteral("bar width %1 != target %2").arg(bar.width()).arg(targetW)),
                             __FILE__, __LINE__);
                return widths;
            }
            if (bar.layout()->count() != itemCount) {
                QTest::qFail("unexpected layout item count", __FILE__, __LINE__);
                return widths;
            }
            std::vector<QWidget *> cur(static_cast<size_t>(itemCount), nullptr);
            for (int i = 0; i < itemCount; ++i) {
                QWidget *w = bar.layout()->itemAt(i)->widget();
                if (!w || w->height() != 32 || w->width() <= 0) {
                    QTest::qFail("invalid item geometry", __FILE__, __LINE__);
                    return widths;
                }
                cur[static_cast<size_t>(i)] = w;
                widths[static_cast<size_t>(i)] = w->width();
            }
            if (cur.front()->x() != 10) {
                QTest::qFail(qPrintable(QStringLiteral("left margin x=%1").arg(cur.front()->x())), __FILE__, __LINE__);
            }
            if (cur.back()->x() + cur.back()->width() != targetW - 10) {
                QTest::qFail(qPrintable(QStringLiteral("right edge=%1 expected=%2")
                                            .arg(cur.back()->x() + cur.back()->width())
                                            .arg(targetW - 10)),
                             __FILE__, __LINE__);
            }
            for (int i = 0; i < itemCount - 1; ++i) {
                const auto nextIdx = static_cast<size_t>(i) + 1;
                const int gap = cur[nextIdx]->x() -
                                (cur[static_cast<size_t>(i)]->x() + cur[static_cast<size_t>(i)]->width());
                if (gap != 8) {
                    QTest::qFail(qPrintable(QStringLiteral("gap[%1]=%2").arg(i).arg(gap)), __FILE__, __LINE__);
                }
            }
            return widths;
        };

        for (const int w : {360, 600, 900, 1220, 1600, 2200}) {
            const auto widths = snapshotWidths(w);
            QVERIFY(widths[0] > 0);
        }

        const int growBaseW = 20 + 8 * (itemCount - 1) + sumHints + 17;
        const auto wGrowBase = snapshotWidths(growBaseW);
        const auto wGrown = snapshotWidths(growBaseW + itemCount * 100);
        std::vector<int> delta1600;
        std::vector<int> delta2200;
        const auto w1600 = snapshotWidths(1600);
        const auto w2200 = snapshotWidths(2200);
        for (int i = 0; i < itemCount; ++i) {
            const auto idx = static_cast<size_t>(i);
            QCOMPARE(wGrown[idx] - wGrowBase[idx], 100);
            delta1600.push_back(w1600[idx] - hintWidths[idx]);
            delta2200.push_back(w2200[idx] - hintWidths[idx]);
            QVERIFY(delta2200.back() > 0);
        }
        QVERIFY(*std::ranges::max_element(delta1600) -
                    *std::ranges::min_element(delta1600) <= 1);
        QVERIFY(*std::ranges::max_element(delta2200) -
                    *std::ranges::min_element(delta2200) <= 1);

        const int shrinkTargetW = 20 + 8 * (itemCount - 1) + sumHints - itemCount * 40;
        const auto wShrink = snapshotWidths(shrinkTargetW);
        for (int i = 0; i < itemCount; ++i) {
            const auto idx = static_cast<size_t>(i);
            QCOMPARE(wShrink[idx] - hintWidths[idx], -40);
        }

        auto assertUnclampedEven = [&](const std::vector<int> &widths) {
            bool hasUnclamped = false;
            int minDelta = 0;
            int maxDelta = 0;
            for (int i = 0; i < itemCount; ++i) {
                const auto idx = static_cast<size_t>(i);
                QVERIFY(widths[idx] >= 72);
                if (widths[idx] > 72) {
                    const int d = widths[idx] - hintWidths[idx];
                    if (!hasUnclamped) {
                        minDelta = maxDelta = d;
                        hasUnclamped = true;
                    } else {
                        minDelta = std::min(minDelta, d);
                        maxDelta = std::max(maxDelta, d);
                    }
                }
            }
            QVERIFY(hasUnclamped);
            QVERIFY(maxDelta - minDelta <= 1);
        };

        const auto w900 = snapshotWidths(900);
        const auto w1220 = snapshotWidths(1220);
        assertUnclampedEven(w900);
        assertUnclampedEven(w1220);

        bar.setPublicIpForTesting(QStringLiteral("203.0.113.42"));
        for (const int w : {900, 1220, 2000}) {
            bar.resize(w, 40);
            bar.layout()->invalidate();
            bar.layout()->activate();
            QApplication::processEvents();
            const QImage img = bar.grab().toImage();
            QVERIFY(!img.isNull());
            QCOMPARE(img.width(), qRound(w * img.devicePixelRatio()));
        }

        QApplication::clipboard()->setText(QStringLiteral("stale"));
        QTest::mouseClick(bar.layout()->itemAt(2 + bar.gpuCardCount())->widget(),
                          Qt::LeftButton, Qt::NoModifier, QPoint(20, 24));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("203.0.113.42"));

        MainWindow win;
        win.resize(1100, 600);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        auto *winBar = win.findChild<StatsBarWidget *>();
        QVERIFY(winBar != nullptr);
        QCOMPARE(winBar->width(), win.width());
        QVERIFY(winBar->layout() != nullptr);
        const int winCount = winBar->layout()->count();
        QCOMPARE(winCount, 5 + winBar->gpuCardCount());
        winBar->layout()->invalidate();
        winBar->layout()->activate();
        QApplication::processEvents();

        std::vector<int> winW1100(static_cast<size_t>(winCount), 0);
        for (int i = 0; i < winCount; ++i) {
            winW1100[static_cast<size_t>(i)] = winBar->layout()->itemAt(i)->widget()->width();
        }

        const int grownWinW = 1100 + winCount * 100;
        win.resize(grownWinW, 700);
        QTRY_COMPARE(winBar->width(), grownWinW);
        winBar->layout()->invalidate();
        winBar->layout()->activate();
        QApplication::processEvents();

        for (int i = 0; i < winCount; ++i) {
            const int grown = winBar->layout()->itemAt(i)->widget()->width() - winW1100[static_cast<size_t>(i)];
            QVERIFY(std::abs(grown - 100) <= 1);
        }
    }

    [[maybe_unused]] static void testStatsBarMultiGpuEnumerationAndHotplug() {
        QCOMPARE(StatsBarWidget::formatGpuShortTag(QStringLiteral("NVIDIA GeForce RTX 5070 Ti Laptop GPU")),
                 QStringLiteral("5070Ti"));
        QCOMPARE(StatsBarWidget::formatGpuShortTag(QStringLiteral("NVIDIA GeForce RTX 5080")),
                 QStringLiteral("5080"));
        QCOMPARE(StatsBarWidget::formatGpuShortTag(QStringLiteral("NVIDIA GeForce RTX 4090 SUPER")),
                 QStringLiteral("4090S"));
        QCOMPARE(StatsBarWidget::formatGpuShortTag(QStringLiteral("AMD Radeon RX 7900 XTX")),
                 QStringLiteral("7900XTX"));
        QCOMPARE(StatsBarWidget::formatGpuShortTag(QStringLiteral("Intel(R) Arc(TM) A770 Graphics")),
                 QStringLiteral("A770"));
        QCOMPARE(StatsBarWidget::formatGpuShortTag(QStringLiteral("Granite Ridge [Radeon 610M]")),
                 QStringLiteral("610M"));
        QCOMPARE(StatsBarWidget::formatGpuShortTag(QString(), 2), QStringLiteral("GPU2"));

        QVERIFY(!StatsBarWidget::isIntegratedGpuName(QStringLiteral("NVIDIA GeForce RTX 5070 Ti Laptop GPU")));
        QVERIFY(!StatsBarWidget::isIntegratedGpuName(QStringLiteral("NVIDIA GeForce RTX 5080")));
        QVERIFY(!StatsBarWidget::isIntegratedGpuName(QStringLiteral("AMD Radeon RX 7900 XTX")));
        QVERIFY(!StatsBarWidget::isIntegratedGpuName(QStringLiteral("Intel(R) Arc(TM) A770 Graphics")));
        QVERIFY(StatsBarWidget::isIntegratedGpuName(QStringLiteral("Granite Ridge [Radeon 610M]")));
        QVERIFY(StatsBarWidget::isIntegratedGpuName(QStringLiteral("AMD Radeon 780M Graphics")));
        QVERIFY(StatsBarWidget::isIntegratedGpuName(QStringLiteral("Intel(R) UHD Graphics")));

        QTemporaryDir tmpProc;
        QVERIFY(tmpProc.isValid());
        auto writeProcGpu = [&](const QString &busId, const QString &modelName) {
            const QString busDir = tmpProc.filePath(busId);
            QVERIFY(QDir().mkpath(busDir));
            QFile f(busDir + QStringLiteral("/information"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write(QStringLiteral("Model: \t\t %1\nIRQ: \t\t 100\n").arg(modelName).toUtf8());
        };
        writeProcGpu(QStringLiteral("0000:01:00.0"), QStringLiteral("NVIDIA GeForce RTX 5070 Ti Laptop GPU"));

        const QList<StatsBarWidget::GpuDescriptor> mixedCandidates{
            {QStringLiteral("gpu/gpu0"), QStringLiteral("NVIDIA GeForce RTX 5070 Ti Laptop GPU"), {}, true},
            {QStringLiteral("gpu/gpu1"), QStringLiteral("NVIDIA GeForce RTX 5080"), {}, true},
            {QStringLiteral("gpu/gpu2"), QStringLiteral("Granite Ridge [Radeon 610M]"), {}, false},
        };

        // Unplugged RTX 5080 and integrated Radeon 610M are both filtered out
        const QStringList procSingle = StatsBarWidget::readConnectedNvidiaModelsFromProc(tmpProc.path());
        QCOMPARE(procSingle, QStringList{QStringLiteral("NVIDIA GeForce RTX 5070 Ti Laptop GPU")});
        const auto activeSingle = StatsBarWidget::selectActiveGpus(mixedCandidates, procSingle, true);
        QCOMPARE(activeSingle.size(), 1);
        QCOMPARE(activeSingle[0].tag, QStringLiteral("5070Ti"));

        // Docked RTX 5080 appears alongside RTX 5070 Ti while Radeon 610M stays hidden
        writeProcGpu(QStringLiteral("0000:09:00.0"), QStringLiteral("NVIDIA GeForce RTX 5080"));
        const QStringList procDual = StatsBarWidget::readConnectedNvidiaModelsFromProc(tmpProc.path());
        QCOMPARE(procDual.size(), 2);
        const auto activeDual = StatsBarWidget::selectActiveGpus(mixedCandidates, procDual, true);
        QCOMPARE(activeDual.size(), 2);
        QCOMPARE(activeDual[0].tag, QStringLiteral("5070Ti"));
        QCOMPARE(activeDual[1].tag, QStringLiteral("5080"));

        // iGPU-only fallback & duplicate model disambiguation
        const auto activeIntegratedOnly = StatsBarWidget::selectActiveGpus(
            {{QStringLiteral("gpu/gpu0"), QStringLiteral("Granite Ridge [Radeon 610M]"), {}, false}},
            {}, false);
        QCOMPARE(activeIntegratedOnly.size(), 1);
        QCOMPARE(activeIntegratedOnly[0].tag, QStringLiteral("610M"));

        const auto activeDup = StatsBarWidget::selectActiveGpus(
            {{QStringLiteral("gpu/gpu0"), QStringLiteral("NVIDIA GeForce RTX 5090"), {}, true},
             {QStringLiteral("gpu/gpu1"), QStringLiteral("NVIDIA GeForce RTX 5090"), {}, true}},
            {}, false);
        QCOMPARE(activeDup.size(), 2);
        QCOMPARE(activeDup[0].tag, QStringLiteral("5090#0"));
        QCOMPARE(activeDup[1].tag, QStringLiteral("5090#1"));

        // Live UI reconciliation across 1 -> 2 -> 3 -> 1 GPUs
        StatsBarWidget bar;
        bar.resize(1600, 40);
        bar.show();
        QVERIFY(QTest::qWaitForWindowExposed(&bar));

        auto verifyBarCount = [&](int expectedGpus, const QStringList &expectedTags) {
            QCOMPARE(bar.gpuCardCount(), expectedGpus);
            QCOMPARE(bar.gpuTags(), expectedTags);
            QCOMPARE(bar.layout()->count(), 5 + expectedGpus);
            QCOMPARE(bar.summaryTexts().size(), 4 + expectedGpus);
            bar.layout()->invalidate();
            bar.layout()->activate();
            QApplication::processEvents();
            QWidget *last = bar.layout()->itemAt(bar.layout()->count() - 1)->widget();
            QVERIFY(last != nullptr);
            QCOMPARE(last->objectName(), QStringLiteral("ClockPill"));
            QCOMPARE(last->x() + last->width(), bar.width() - 10);
            QVERIFY(!bar.grab().toImage().isNull());
        };

        bar.setGpusForTesting({mixedCandidates[0]});
        verifyBarCount(1, {QStringLiteral("5070Ti")});

        bar.setGpusForTesting({mixedCandidates[0], mixedCandidates[1]});
        verifyBarCount(2, {QStringLiteral("5070Ti"), QStringLiteral("5080")});

        bar.setGpusForTesting({
            mixedCandidates[0],
            mixedCandidates[1],
            {QStringLiteral("gpu/gpu2"), QStringLiteral("AMD Radeon RX 7900 XTX"), {}, true},
        });
        verifyBarCount(3, {QStringLiteral("5070Ti"), QStringLiteral("5080"), QStringLiteral("7900XTX")});

        bar.setGpusForTesting({mixedCandidates[0]});
        verifyBarCount(1, {QStringLiteral("5070Ti")});
    }

    [[maybe_unused]] static void testMainWindowTerminalSplitsAndIo() {
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

    [[maybe_unused]] static void testTabDoesNotStealTerminalFocus() {
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

    [[maybe_unused]] static void testP10kPromptSpacingAndGlyphs() {
        TerminalWidget term(QString(), nullptr, /*startShell=*/false);
        term.resize(900, 260);
        term.show();
        QVERIFY(QTest::qWaitForWindowExposed(&term));

        const QSize cs = term.cellSize();
        const int cw = cs.width();
        const int ch = cs.height();
        constexpr QPoint pad = TerminalWidget::viewportOffset();
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

        constexpr QColor seg1Bg(0x21, 0x88, 0xff);
        constexpr QColor seg2Bg(0x30, 0x36, 0x3d);

        // Icon cells (col 6 = U+F303, col 10 = U+F489, col 35 = U+F10FE) must render visible fg ink
        QVERIFY2(cellMaxDistFromBg(6, 0, seg1Bg, 0, 0) > 80, "U+F303 icon did not render visible ink");
        QVERIFY2(cellMaxDistFromBg(10, 0, seg2Bg, 0, 0) > 80, "U+F489 icon did not render visible ink");
        QVERIFY2(cellMaxDistFromBg(35, 0, seg2Bg, 0, 0) > 40, "U+F10FE Supplementary PUA-A icon did not render visible ink");

        // Adjacent right space cells (col 7, col 11, col 36) must stay pure segment background (zero overflow)
        QVERIFY2(cellMaxDistFromBg(7, 0, seg1Bg, 1, 1) <= 4, "U+F303 overflowed into right space padding cell");
        QVERIFY2(cellMaxDistFromBg(11, 0, seg2Bg, 1, 1) <= 4, "U+F489 overflowed into right space padding cell");
        QVERIFY2(cellMaxDistFromBg(36, 0, seg2Bg, 1, 1) <= 4, "U+F10FE overflowed into right space padding cell");

        // Horizontal box-drawing filler dashes (cols 28..30, U+2500) must connect with zero gaps
        constexpr QColor gapLineCol(118, 131, 144);
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
        const double midCellX = static_cast<double>(pad.x() + 37 * cw) + static_cast<double>(cw) * 0.5;
        const int shadeMidX = static_cast<int>(std::round(midCellX * imgDpr));
        const int topRowY = static_cast<int>(std::floor(pad.y() * imgDpr));
        const int botRowY = static_cast<int>(std::ceil((pad.y() + ch) * imgDpr)) - 1;
        QVERIFY2(colorDist(img.pixelColor(shadeMidX, topRowY), termBg) > 8, "Shade U+2593 stepped down from row top");
        QVERIFY2(colorDist(img.pixelColor(shadeMidX, botRowY), termBg) > 8, "Shade U+2593 clipped above row bottom");
    }

    [[maybe_unused]] static void testTerminalScrollbarMouseDragAndClick() {
        TerminalWidget term(QString(), nullptr, /*startShell=*/false);
        term.resize(640, 240);
        term.show();
        QVERIFY(QTest::qWaitForWindowExposed(&term));

        QVERIFY(term.scrollbarState().total <= term.scrollbarState().len);

        QByteArray lines;
        for (int i = 0; i < 200; ++i) {
            lines.append(QStringLiteral("Line %1\r\n").arg(i, 3, 10, QLatin1Char('0')).toUtf8());
        }
        term.feedVtBytes(lines);

        const auto sbInit = term.scrollbarState();
        QVERIFY(sbInit.total > sbInit.len);
        const uint64_t maxOffset = sbInit.total - sbInit.len;
        QCOMPARE(sbInit.offset, maxOffset);
        QVERIFY(term.plainTextSnapshot().contains(QStringLiteral("Line 199")));

        // Track click near top jumps viewport to the top of scrollback
        QTest::mouseClick(&term, Qt::LeftButton, Qt::NoModifier, QPoint(term.width() - 6, 8));
        QCOMPARE(term.scrollbarState().offset, uint64_t{0});
        QVERIFY(term.plainTextSnapshot().contains(QStringLiteral("Line 000")));

        // Drag thumb smoothly from top -> middle -> bottom (including horizontal drift outside gutter)
        const QPoint pressPt(term.width() - 6, 10);
        QTest::mousePress(&term, Qt::LeftButton, Qt::NoModifier, pressPt);

        const QPointF midLocal(static_cast<qreal>(term.width() - 6), static_cast<qreal>(term.height()) * 0.5);
        QMouseEvent midMove(QEvent::MouseMove, midLocal,
                            term.mapToGlobal(midLocal.toPoint()),
                            Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&term, &midMove);
        const uint64_t midOffset = term.scrollbarState().offset;
        QVERIFY(midOffset > 0);
        QVERIFY(midOffset < maxOffset);

        const QPointF botLocal(static_cast<qreal>(term.width() - 100), static_cast<qreal>(term.height() - 4));
        QMouseEvent botMove(QEvent::MouseMove, botLocal,
                            term.mapToGlobal(botLocal.toPoint()),
                            Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&term, &botMove);
        QCOMPARE(term.scrollbarState().offset, maxOffset);
        QVERIFY(term.plainTextSnapshot().contains(QStringLiteral("Line 199")));

        QTest::mouseRelease(&term, Qt::LeftButton, Qt::NoModifier, botLocal.toPoint());
        QVERIFY(!term.grab().toImage().isNull());

        // Scrollbar gutter keeps priority over PTY mouse tracking modes
        term.feedVtBytes(QByteArrayLiteral("\x1b[?1000h\x1b[?1006h"));
        QTest::mouseClick(&term, Qt::LeftButton, Qt::NoModifier, QPoint(term.width() - 6, 8));
        QCOMPARE(term.scrollbarState().offset, uint64_t{0});
    }

    [[maybe_unused]] static void testOsc7AndKittyPwdTracking() {
        QCOMPARE(GhosttyUnionLink::parseOsc7Pwd(
                     QStringLiteral("file://arch_laptop/home/ak/projects%20dir"),
                     QStringLiteral("arch_laptop")),
                 QStringLiteral("/home/ak/projects dir"));
        QCOMPARE(GhosttyUnionLink::parseOsc7Pwd(
                     QStringLiteral("kitty-shell-cwd://arch_laptop/home/ak/100%20literal"),
                     QStringLiteral("arch_laptop")),
                 QStringLiteral("/home/ak/100%20literal"));
        QCOMPARE(GhosttyUnionLink::parseOsc7Pwd(
                     QStringLiteral("file:///tmp"),
                     QStringLiteral("arch_laptop")),
                 QStringLiteral("/tmp"));
        QCOMPARE(GhosttyUnionLink::parseOsc7Pwd(
                     QStringLiteral("/var/tmp"),
                     QStringLiteral("arch_laptop")),
                 QStringLiteral("/var/tmp"));
        QCOMPARE(GhosttyUnionLink::parseOsc7Pwd(
                     QStringLiteral("file://prod_box/etc/hosts"),
                     QStringLiteral("arch_laptop")),
                 QString());

        TerminalWidget term(QStringLiteral("/"), nullptr, /*startShell=*/false);
        const QString host = QSysInfo::machineHostName();
        QVERIFY(!host.isEmpty());

        term.feedVtBytes(QStringLiteral("\x1b]7;file://%1/tmp\x1b\\").arg(host).toUtf8());
        QCOMPARE(term.currentWorkingDirectory(), QStringLiteral("/tmp"));

        term.feedVtBytes(QStringLiteral("\x1b]7;kitty-shell-cwd://%1/var/tmp\x07").arg(host).toUtf8());
        QCOMPARE(term.currentWorkingDirectory(), QStringLiteral("/var/tmp"));

        term.feedVtBytes(QByteArrayLiteral("\x1b]7;file://untrusted_remote_host/etc\x07"));
        QCOMPARE(term.currentWorkingDirectory(), QStringLiteral("/var/tmp"));
    }

    [[maybe_unused]] static void testCtrlHoverAndCtrlClickLinksAndFiles() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        QVERIFY(QDir().mkpath(tmpDir.filePath(QStringLiteral("src"))));
        {
            QFile f1(tmpDir.filePath(QStringLiteral("CMakeLists.txt")));
            QVERIFY(f1.open(QIODevice::WriteOnly));
            f1.write("cmake_minimum_required(VERSION 3.24)\n");
        }
        {
            QFile f2(tmpDir.filePath(QStringLiteral("src/Widget.cpp")));
            QVERIFY(f2.open(QIODevice::WriteOnly));
            f2.write("int widget() { return 0; }\n");
        }

        TerminalWidget term(tmpDir.path(), nullptr, /*startShell=*/false);
        term.resize(760, 360);
        term.show();
        QVERIFY(QTest::qWaitForWindowExposed(&term));

        const QSize cs = term.cellSize();
        const int cw = cs.width();
        const int ch = cs.height();
        constexpr QPoint pad = TerminalWidget::viewportOffset();
        const int cols = std::max(20, (term.width() - 6 - 12) / cw);
        auto cellCenter = [&](int col, int row) {
            return QPoint(pad.x() + col * cw + cw / 2, pad.y() + row * ch + ch / 2);
        };

        const QString host = QSysInfo::machineHostName();
        const QString canonicalWidget = QFileInfo(tmpDir.filePath(QStringLiteral("src/Widget.cpp"))).canonicalFilePath();
        const QString canonicalCmake = QFileInfo(tmpDir.filePath(QStringLiteral("CMakeLists.txt"))).canonicalFilePath();

        // Row 0: OSC 8 link
        // Row 1..2: soft-wrapped https:// URL
        // Row 3: src/Widget.cpp:128:9: error:
        // Row 4: ./CMakeLists.txt:15
        // Row 5: CMakeLists.txt
        // Row 6: origin/develop (non-existent, must not match)
        const int wrapSlugLen = std::max(20, cols - 10);
        const QString longUrl = QStringLiteral("https://example.com/") + QString(wrapSlugLen, QLatin1Char('a'));
        QByteArray vt;
        vt.append(QStringLiteral("\x1b]7;file://%1%2\x1b\\").arg(host, tmpDir.path()).toUtf8());
        vt.append(QStringLiteral("\x1b]8;;file://%1%2#L42\x1b\\Widget\x1b]8;;\x1b\\\r\n")
                      .arg(host, canonicalWidget).toUtf8());
        vt.append(longUrl.toUtf8());
        vt.append("\r\n");
        vt.append("src/Widget.cpp:128:9: error:\r\n");
        vt.append("./CMakeLists.txt:15\r\n");
        vt.append("CMakeLists.txt\r\n");
        vt.append("origin/develop\r\n");
        term.feedVtBytes(vt);

        QList<GhosttyUnionLink::ResolvedLink> openedLinks;
        term.setLinkOpenHandlerForTesting([&](const GhosttyUnionLink::ResolvedLink &lnk) {
            openedLinks.push_back(lnk);
            return true;
        });

        QByteArray ptyBytes;
        term.setPtyOutputSpyForTesting([&](const QByteArray &b) {
            ptyBytes.append(b);
        });

        auto sendMove = [&](const QPoint &pt, Qt::KeyboardModifiers mods) {
            QMouseEvent ev(QEvent::MouseMove, QPointF(pt), QPointF(term.mapToGlobal(pt)),
                           Qt::NoButton, Qt::NoButton, mods);
            QCoreApplication::sendEvent(&term, &ev);
        };

        // 1. Hover OSC 8 on row 0
        sendMove(cellCenter(2, 0), Qt::ControlModifier);
        QVERIFY(term.hoveredLink().isValid());
        QVERIFY(term.hoveredLink().isOsc8);
        QVERIFY(term.hoveredLink().isLocalFile);
        QCOMPARE(term.hoveredLink().target, canonicalWidget);
        QCOMPARE(term.hoveredLink().line, 42);
        QCOMPARE(term.cursor().shape(), Qt::PointingHandCursor);

        // 2. Hover soft-wrapped URL on row 2 (continuation of row 1)
        sendMove(cellCenter(4, 2), Qt::ControlModifier);
        QVERIFY(term.hoveredLink().isValid());
        QVERIFY(!term.hoveredLink().isLocalFile);
        QCOMPARE(term.hoveredLink().target, longUrl);
        QCOMPARE(term.hoveredLink().spans.size(), 2);

        // 3. Hover src/Widget.cpp:128:9 on row 3 -> line 128, col 9
        sendMove(cellCenter(4, 3), Qt::ControlModifier);
        QVERIFY(term.hoveredLink().isValid());
        QCOMPARE(term.hoveredLink().target, canonicalWidget);
        QCOMPARE(term.hoveredLink().line, 128);
        QCOMPARE(term.hoveredLink().column, 9);
        QVERIFY(!term.grab().toImage().isNull());

        // 4. Stationary Ctrl press & release updates hover state
        sendMove(cellCenter(4, 4), Qt::NoModifier);
        QVERIFY(!term.hoveredLink().isValid());
        QCOMPARE(term.cursor().shape(), Qt::IBeamCursor);

        QKeyEvent ctrlPress(QEvent::KeyPress, Qt::Key_Control, Qt::NoModifier);
        QCoreApplication::sendEvent(&term, &ctrlPress);
        QVERIFY(term.hoveredLink().isValid());
        QCOMPARE(term.hoveredLink().target, canonicalCmake);
        QCOMPARE(term.hoveredLink().line, 15);
        QCOMPARE(term.cursor().shape(), Qt::PointingHandCursor);

        QKeyEvent ctrlRelease(QEvent::KeyRelease, Qt::Key_Control, Qt::ControlModifier);
        QCoreApplication::sendEvent(&term, &ctrlRelease);
        QVERIFY(!term.hoveredLink().isValid());
        QCOMPARE(term.cursor().shape(), Qt::IBeamCursor);

        // 5. Bare CMakeLists.txt on row 5 resolves in cwd
        sendMove(cellCenter(3, 5), Qt::ControlModifier);
        QVERIFY(term.hoveredLink().isValid());
        QCOMPARE(term.hoveredLink().target, canonicalCmake);

        // 6. Non-existent origin/develop on row 6 must NOT match
        sendMove(cellCenter(3, 6), Qt::ControlModifier);
        QVERIFY(!term.hoveredLink().isValid());

        // 7. Ctrl+Click opens src/Widget.cpp:128:9 (normal mode)
        openedLinks.clear();
        QTest::mouseClick(&term, Qt::LeftButton, Qt::ControlModifier, cellCenter(4, 3));
        QCOMPARE(openedLinks.size(), 1);
        QCOMPARE(openedLinks.back().target, canonicalWidget);
        QCOMPARE(openedLinks.back().line, 128);
        QCOMPARE(openedLinks.back().column, 9);

        // 8. Under mouse tracking (?1000h + ?1006h), Ctrl+Click on link still opens link without leaking to PTY,
        //    whereas Ctrl+Click on non-link cell forwards SGR mouse sequence to PTY.
        term.feedVtBytes(QByteArrayLiteral("\x1b[?1000h\x1b[?1006h"));
        ptyBytes.clear();
        openedLinks.clear();
        QTest::mouseClick(&term, Qt::LeftButton, Qt::ControlModifier, cellCenter(4, 3));
        QCOMPARE(openedLinks.size(), 1);
        QVERIFY(ptyBytes.isEmpty());

        QTest::mouseClick(&term, Qt::LeftButton, Qt::ControlModifier, cellCenter(3, 6));
        QCOMPARE(openedLinks.size(), 1);
        QVERIFY(ptyBytes.contains("\x1b[<"));
    }

    [[maybe_unused]] static void testGhosttySurfaceParityFeatures() {
        // 1. Unfocused split dimming (`unfocused-split-opacity = 0.88`)
        TerminalTabPane pane;
        pane.resize(800, 400);
        pane.show();
        QVERIFY(QTest::qWaitForWindowExposed(&pane));

        TerminalWidget *t1 = pane.activeTerminal();
        QVERIFY(t1 != nullptr);
        QVERIFY(!t1->isUnfocusedSplitDimmed());

        pane.splitActive(Qt::Horizontal);
        TerminalWidget *t2 = pane.activeTerminal();
        QVERIFY(t2 != nullptr && t2 != t1);
        QVERIFY(t1->isUnfocusedSplitDimmed());
        QVERIFY(!t2->isUnfocusedSplitDimmed());

        QTest::mouseClick(t1, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QVERIFY(!t1->isUnfocusedSplitDimmed());
        QVERIFY(t2->isUnfocusedSplitDimmed());

        pane.toggleZoom();
        QVERIFY(!t1->isUnfocusedSplitDimmed());
        pane.toggleZoom();
        QVERIFY(!t1->isUnfocusedSplitDimmed());
        QVERIFY(t2->isUnfocusedSplitDimmed());

        // 2. DEC mode 1004 focus events, OSC 22 pointer shapes, OSC 133 click-to-move & prompt jump
        TerminalWidget term(QString(), nullptr, /*startShell=*/false);
        term.resize(640, 240);
        term.show();
        QVERIFY(QTest::qWaitForWindowExposed(&term));

        QByteArray ptyBytes;
        term.setPtyOutputSpyForTesting([&](const QByteArray &b) {
            ptyBytes.append(b);
        });

        term.feedVtBytes(QByteArrayLiteral("\x1b[?1004h"));
        {
            QFocusEvent fin(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
            QCoreApplication::sendEvent(&term, &fin);
            QFocusEvent focusOutEv(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
            QCoreApplication::sendEvent(&term, &focusOutEv);
        }
        QCOMPARE(ptyBytes, QByteArrayLiteral("\x1b[I\x1b[O"));

        ptyBytes.clear();
        term.feedVtBytes(QByteArrayLiteral("\x1b[?1004l"));
        {
            QFocusEvent fin(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
            QCoreApplication::sendEvent(&term, &fin);
        }
        QVERIFY(ptyBytes.isEmpty());

        // OSC 22 cursor shapes
        term.feedVtBytes(QByteArrayLiteral("\x1b]22;pointer\x07"));
        QCOMPARE(term.cursor().shape(), Qt::PointingHandCursor);
        term.feedVtBytes(QByteArrayLiteral("\x1b]22;text\x07"));
        QCOMPARE(term.cursor().shape(), Qt::IBeamCursor);

        // OSC 133 click-to-move
        const QSize cs = term.cellSize();
        const int cw = cs.width();
        const int ch = cs.height();
        constexpr QPoint pad = TerminalWidget::viewportOffset();
        auto cellCenter = [&](int col, int row) {
            return QPoint(pad.x() + col * cw + cw / 2, pad.y() + row * ch + ch / 2);
        };

        term.feedVtBytes(QByteArrayLiteral("\x1b]133;A\x07$ \x1b]133;B\x07" "hello!"));
        ptyBytes.clear();
        QTest::mouseClick(&term, Qt::LeftButton, Qt::NoModifier, cellCenter(3, 0));
        QCOMPARE(ptyBytes, QByteArrayLiteral("\x1b[D\x1b[D\x1b[D\x1b[D\x1b[D"));

        // Clicking prompt cells or alt-screen must not send arrow keys
        ptyBytes.clear();
        QTest::mouseClick(&term, Qt::LeftButton, Qt::NoModifier, cellCenter(0, 0));
        QVERIFY(ptyBytes.isEmpty());

        term.feedVtBytes(QByteArrayLiteral("\x1b[?1049h\x1b]133;A\x07$ \x1b]133;B\x07" "hello!"));
        QTest::mouseClick(&term, Qt::LeftButton, Qt::NoModifier, cellCenter(3, 0));
        QVERIFY(ptyBytes.isEmpty());
        term.feedVtBytes(QByteArrayLiteral("\x1b[?1049l"));

        // Prompt jump (Ctrl+Shift+Up)
        QByteArray promptLines = QByteArrayLiteral("\r\n\x1b]133;A\x07prompt1\r\n\x1b]133;C\x07");
        for (int i = 0; i < 60; ++i) {
            promptLines.append(QStringLiteral("output %1\r\n").arg(i).toUtf8());
        }
        promptLines.append("\x1b]133;A\x07prompt2 ");
        term.feedVtBytes(promptLines);
        const auto sbBefore = term.scrollbarState();
        QVERIFY(sbBefore.total > sbBefore.len);
        const uint64_t maxOffset = sbBefore.total - sbBefore.len;
        QCOMPARE(sbBefore.offset, maxOffset);
        QTest::keyClick(&term, Qt::Key_Up, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(term.scrollbarState().offset < maxOffset);

        // Shell quoting for drag-and-drop paths
        QCOMPARE(GhosttyUnionLink::shellQuotePaths({QUrl::fromLocalFile(QStringLiteral("/tmp/a b/c'd.txt"))}),
                 QStringLiteral("'/tmp/a b/c'\\''d.txt'"));

        // Live shell integration check: cd updates m_pwd via kitty-shell-cwd / OSC 7 on arch_laptop
        QTemporaryDir cdTarget;
        QVERIFY(cdTarget.isValid());
        const QString canonicalTarget = QFileInfo(cdTarget.path()).canonicalFilePath();

        MainWindow win;
        win.resize(900, 600);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));

        const auto *tabs = win.findChild<QTabWidget *>();
        QVERIFY(tabs != nullptr);
        auto *mainPane = qobject_cast<TerminalTabPane *>(tabs->currentWidget());
        QVERIFY(mainPane != nullptr);
        TerminalWidget *liveTerm = mainPane->activeTerminal();
        QVERIFY(liveTerm != nullptr);

        QTest::keyClicks(liveTerm, QStringLiteral("cd %1 && echo __ZSH_CD_DONE__")
                                       .arg(GhosttyUnionLink::shellQuoteSingle(cdTarget.path())));
        QTest::keyClick(liveTerm, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(
            liveTerm->plainTextSnapshot().contains(QLatin1String("__ZSH_CD_DONE__")),
            5000);
        QTRY_COMPARE_WITH_TIMEOUT(liveTerm->currentWorkingDirectory(), canonicalTarget, 5000);
    }

    [[maybe_unused]] static void testCtrlDFocusShiftAcrossNestedSplitsAndTabs() {
        MainWindow win;
        win.resize(1000, 640);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        win.activateWindow();

        auto *tabs = win.findChild<QTabWidget *>();
        QVERIFY(tabs != nullptr);
        QCOMPARE(tabs->focusPolicy(), Qt::NoFocus);
        QCOMPARE(tabs->tabBar()->focusPolicy(), Qt::NoFocus);

        auto waitShellReady = [](TerminalWidget *t, const QString &id) {
            const QString expected = QStringLiteral("__RDY_%1__").arg(id);
            QTest::keyClicks(t, QStringLiteral("printf '%s%s\\n' __RDY_ %1__").arg(id));
            QTest::keyClick(t, Qt::Key_Return);
            QTRY_VERIFY_WITH_TIMEOUT(t->plainTextSnapshot().contains(expected), 5000);
            QTest::qWait(80);
        };

        auto *pane = qobject_cast<TerminalTabPane *>(tabs->currentWidget());
        QVERIFY(pane != nullptr);
        TerminalWidget *tA = pane->activeTerminal();
        QVERIFY(tA != nullptr);
        waitShellReady(tA, QStringLiteral("A"));

        pane->splitActive(Qt::Horizontal);
        TerminalWidget *tB = pane->activeTerminal();
        QVERIFY(tB != nullptr && tB != tA);
        waitShellReady(tB, QStringLiteral("B"));

        pane->splitActive(Qt::Vertical);
        TerminalWidget *tC = pane->activeTerminal();
        QVERIFY(tC != nullptr && tC != tB && tC != tA);
        QTRY_VERIFY_WITH_TIMEOUT(tC->hasFocus(), 2000);
        waitShellReady(tC, QStringLiteral("C"));

        // Ctrl+D on C (layout A | (B / C)) must collapse nested splitter and return focus to MRU B
        const QPointer<TerminalWidget> guardA(tA);
        const QPointer<TerminalWidget> guardB(tB);
        QTest::keyClick(tC, Qt::Key_D, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(pane->findChildren<TerminalWidget *>().size(), 2, 5000);
        QVERIFY(guardB);
        QCOMPARE(pane->activeTerminal(), guardB.data());
        QTRY_VERIFY_WITH_TIMEOUT(guardB && guardB->hasFocus(), 2000);
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget *>(guardB.data()));

        // Typing into QApplication::focusWidget() reaches B directly
        QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("echo __AFTER_CTRL_D_B__"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(guardB && guardB->plainTextSnapshot().contains(QLatin1String("__AFTER_CTRL_D_B__")), 5000);
        QVERIFY(guardB);

        // MRU focus order: split -> click A -> click C2 -> Ctrl+D on C2 must return focus to A
        pane->splitActive(Qt::Vertical);
        TerminalWidget *tC2 = pane->activeTerminal();
        QVERIFY(tC2 != nullptr && tC2 != tB && tC2 != tA);
        waitShellReady(tC2, QStringLiteral("C2"));

        QVERIFY(guardA);
        QTest::mouseClick(guardA, Qt::LeftButton, Qt::NoModifier, QPoint(24, 24));
        QTRY_VERIFY_WITH_TIMEOUT(guardA && guardA->hasFocus(), 2000);
        QTest::mouseClick(tC2, Qt::LeftButton, Qt::NoModifier, QPoint(24, 24));
        QTRY_VERIFY_WITH_TIMEOUT(tC2->hasFocus(), 2000);

        QTest::keyClick(tC2, Qt::Key_D, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(pane->findChildren<TerminalWidget *>().size(), 2, 5000);
        QVERIFY(guardA);
        QCOMPARE(pane->activeTerminal(), guardA.data());
        QTRY_VERIFY_WITH_TIMEOUT(guardA && guardA->hasFocus(), 2000);
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget *>(guardA.data()));

        // MRU tab focus restoration: open 3 tabs, visit Tab 0 -> Tab 2, Ctrl+D Tab 2 -> restores Tab 0
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 3000);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 3, 3000);

        tabs->setCurrentIndex(0);
        QApplication::processEvents();
        tabs->setCurrentIndex(2);
        QApplication::processEvents();

        auto *pane2 = qobject_cast<TerminalTabPane *>(tabs->currentWidget());
        QVERIFY(pane2 != nullptr);
        TerminalWidget *tab2Term = pane2->activeTerminal();
        QVERIFY(tab2Term != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(tab2Term->hasFocus(), 2000);
        waitShellReady(tab2Term, QStringLiteral("TAB2"));

        QTest::keyClick(tab2Term, Qt::Key_D, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 5000);
        QCOMPARE(tabs->currentIndex(), 0);
        QCOMPARE(tabs->currentWidget(), static_cast<QWidget *>(pane));
        QTRY_VERIFY_WITH_TIMEOUT(tA->hasFocus(), 2000);
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget *>(tA));
    }

    [[maybe_unused]] static void testSplitRearrangementDragSwapDockAndDetach() {
        MainWindow win;
        win.resize(1000, 640);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        win.activateWindow();

        auto *tabs = win.findChild<QTabWidget *>();
        QVERIFY(tabs != nullptr);
        auto *pane = qobject_cast<TerminalTabPane *>(tabs->currentWidget());
        QVERIFY(pane != nullptr);

        TerminalWidget *tA = pane->activeTerminal();
        QVERIFY(tA != nullptr);
        QVERIFY(!tA->isSplitRearrangeable());

        pane->splitActive(Qt::Horizontal);
        TerminalWidget *tB = pane->activeTerminal();
        QVERIFY(tB != nullptr && tB != tA);
        QVERIFY(tA->isSplitRearrangeable());
        QVERIFY(tB->isSplitRearrangeable());
        QApplication::processEvents();
        QVERIFY(tA->mapTo(pane, QPoint(0, 0)).x() < tB->mapTo(pane, QPoint(0, 0)).x());

        auto dragBetweenWidgets = [](TerminalWidget *src,
                                     const QPoint &srcLocalStart,
                                     TerminalWidget *dst,
                                     const QPoint &dstLocalEnd,
                                     Qt::KeyboardModifiers mods) {
            const QPoint gStart = src->mapToGlobal(srcLocalStart);
            const QPoint gEnd = dst->mapToGlobal(dstLocalEnd);
            const QPoint dstInSrc = src->mapFromGlobal(gEnd);
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(srcLocalStart), QPointF(gStart),
                              Qt::LeftButton, Qt::LeftButton, mods);
            QCoreApplication::sendEvent(src, &press);
            QMouseEvent step(QEvent::MouseMove, QPointF(srcLocalStart + QPoint(10, 10)),
                             QPointF(gStart + QPoint(10, 10)),
                             Qt::NoButton, Qt::LeftButton, mods);
            QCoreApplication::sendEvent(src, &step);
            QMouseEvent move(QEvent::MouseMove, QPointF(dstInSrc), QPointF(gEnd),
                             Qt::NoButton, Qt::LeftButton, mods);
            QCoreApplication::sendEvent(src, &move);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(dstInSrc), QPointF(gEnd),
                                Qt::LeftButton, Qt::NoButton, mods);
            QCoreApplication::sendEvent(src, &release);
            QApplication::processEvents();
        };

        // 1. Hover-grip drag CenterSwap: drag tB onto center of tA -> tB moves left, tA moves right
        const QPoint gripB = tB->splitGripRect().center().toPoint();
        QCOMPARE(pane->previewDropZoneForTesting(tB, tA->mapToGlobal(tA->rect().center())),
                 TerminalTabPane::DropZone::CenterSwap);
        dragBetweenWidgets(tB, gripB, tA, tA->rect().center(), Qt::NoModifier);
        QVERIFY(tB->mapTo(pane, QPoint(0, 0)).x() < tA->mapTo(pane, QPoint(0, 0)).x());
        QCOMPARE(pane->activeTerminal(), tB);
        QTRY_VERIFY_WITH_TIMEOUT(tB->hasFocus(), 2000);

        // 2. Ctrl+Alt+LeftDrag Bottom edge re-dock: drag tB onto bottom band of tA -> tA above, tB below
        const QPoint botA(tA->width() / 2, tA->height() - 30);
        QCOMPARE(pane->previewDropZoneForTesting(tB, tA->mapToGlobal(botA)),
                 TerminalTabPane::DropZone::Bottom);
        dragBetweenWidgets(tB, tB->rect().center(), tA, botA, Qt::ControlModifier | Qt::AltModifier);
        QVERIFY(tB->mapTo(pane, QPoint(0, 0)).y() > tA->mapTo(pane, QPoint(0, 0)).y());
        QTRY_VERIFY_WITH_TIMEOUT(tB->hasFocus(), 2000);

        // 3. 3-pane RootRight full-height dock + keyboard moveSplit / rotate / equalize
        pane->splitActive(Qt::Horizontal);
        TerminalWidget *tC = pane->activeTerminal();
        QVERIFY(tC != nullptr && tC != tB && tC != tA);
        QApplication::processEvents();

        const QPoint rootRightGlobal = pane->mapToGlobal(QPoint(pane->width() - 6, pane->height() / 2));
        QCOMPARE(pane->previewDropZoneForTesting(tC, rootRightGlobal),
                 TerminalTabPane::DropZone::RootRight);
        QVERIFY(pane->dockTerminal(tC, nullptr, TerminalTabPane::DropZone::RootRight));
        QApplication::processEvents();
        QVERIFY(tC->mapTo(pane, QPoint(0, 0)).x() > tA->mapTo(pane, QPoint(0, 0)).x());
        QVERIFY(tC->height() >= pane->height() - 10);

        const int xBeforeMove = tC->mapTo(pane, QPoint(0, 0)).x();
        QVERIFY(pane->moveSplit(-1, 0));
        QApplication::processEvents();
        QVERIFY(tC->mapTo(pane, QPoint(0, 0)).x() < xBeforeMove);

        pane->rotateSplitOrientation();
        pane->equalizeSplits();
        QApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(tC->hasFocus(), 2000);

        // 4. Dragging split onto tab bar detaches live terminal into a new tab without losing PTY
        QTest::keyClicks(tC, QStringLiteral("echo __BEFORE_DETACH__"));
        QTest::keyClick(tC, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(tC->plainTextSnapshot().contains(QLatin1String("__BEFORE_DETACH__")), 5000);

        const QPoint tabBarGlobal = pane->mapToGlobal(QPoint(pane->width() / 2, -16));
        QCOMPARE(pane->previewDropZoneForTesting(tC, tabBarGlobal),
                 TerminalTabPane::DropZone::DetachTab);
        const QPoint dstInC = tC->mapFromGlobal(tabBarGlobal);
        const QPoint gripC = tC->splitGripRect().center().toPoint();
        QMouseEvent pressC(QEvent::MouseButtonPress, QPointF(gripC), QPointF(tC->mapToGlobal(gripC)),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(tC, &pressC);
        QMouseEvent stepC(QEvent::MouseMove, QPointF(gripC + QPoint(8, 8)),
                          QPointF(tC->mapToGlobal(gripC + QPoint(8, 8))),
                          Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(tC, &stepC);
        QMouseEvent moveC(QEvent::MouseMove, QPointF(dstInC), QPointF(tabBarGlobal),
                          Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(tC, &moveC);
        QMouseEvent relC(QEvent::MouseButtonRelease, QPointF(dstInC), QPointF(tabBarGlobal),
                         Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(tC, &relC);

        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 3000);
        auto *newPane = qobject_cast<TerminalTabPane *>(tabs->currentWidget());
        QVERIFY(newPane != nullptr && newPane != pane);
        QCOMPARE(newPane->activeTerminal(), tC);
        QVERIFY(!tC->isSplitRearrangeable());
        QTRY_VERIFY_WITH_TIMEOUT(tC->hasFocus(), 2000);
        QVERIFY(tC->plainTextSnapshot().contains(QLatin1String("__BEFORE_DETACH__")));
        QCOMPARE(pane->findChildren<TerminalWidget *>().size(), 2);

        // Verify live PTY still responds in detached tab
        QTest::keyClicks(tC, QStringLiteral("echo __AFTER_DETACH__"));
        QTest::keyClick(tC, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(tC->plainTextSnapshot().contains(QLatin1String("__AFTER_DETACH__")), 5000);
    }

    [[maybe_unused]] static void testTabCloseCustomModalAndMultiTabIsolation() {
        MainWindow win;
        win.resize(960, 620);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        win.activateWindow();

        auto *tabs = win.findChild<QTabWidget *>();
        QVERIFY(tabs != nullptr);
        QTabBar *bar = tabs->tabBar();
        QVERIFY(bar != nullptr);

        auto *modal = win.findChild<QWidget *>(QStringLiteral("tabCloseConfirmModal"));
        auto *card = win.findChild<QWidget *>(QStringLiteral("tabCloseConfirmCard"));
        auto *titleLbl = win.findChild<QLabel *>(QStringLiteral("tabCloseModalTitle"));
        auto *bodyLbl = win.findChild<QLabel *>(QStringLiteral("tabCloseModalBody"));
        auto *cancelBtn = win.findChild<QPushButton *>(QStringLiteral("tabCloseCancelButton"));
        auto *confirmBtn = win.findChild<QPushButton *>(QStringLiteral("tabCloseConfirmButton"));
        QVERIFY(modal != nullptr);
        QVERIFY(card != nullptr);
        QVERIFY(titleLbl != nullptr);
        QVERIFY(bodyLbl != nullptr);
        QVERIFY(cancelBtn != nullptr);
        QVERIFY(confirmBtn != nullptr);
        QVERIFY(!modal->isVisible());

        auto findCloseButtonForTab = [bar](int idx) -> QAbstractButton * {
            if (auto *btn = qobject_cast<QAbstractButton *>(bar->tabButton(idx, QTabBar::RightSide))) {
                return btn;
            }
            return qobject_cast<QAbstractButton *>(bar->tabButton(idx, QTabBar::LeftSide));
        };

        auto *pane0 = qobject_cast<TerminalTabPane *>(tabs->widget(0));
        QVERIFY(pane0 != nullptr);
        TerminalWidget *term0 = pane0->activeTerminal();
        QVERIFY(term0 != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(term0->hasFocus(), 2000);

        // Open a second tab
        QTest::keyClick(term0, Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 3000);
        auto *pane1 = qobject_cast<TerminalTabPane *>(tabs->widget(1));
        QVERIFY(pane1 != nullptr);
        QCOMPARE(pane1->terminalCount(), 1);
        TerminalWidget *term1 = pane1->activeTerminal();
        QVERIFY(term1 != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(term1->hasFocus(), 2000);
        tabs->setTabText(1, QStringLiteral("work-tab"));
        QApplication::processEvents();

        // 1. Clicking real QTabBar close button opens in-window custom modal (no OS QMessageBox)
        QAbstractButton *closeTab1Btn = findCloseButtonForTab(1);
        QVERIFY(closeTab1Btn != nullptr);
        QTest::mouseClick(closeTab1Btn, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(modal->isVisible(), 2000);
        QCOMPARE(QApplication::activeModalWidget(), static_cast<QWidget *>(nullptr));
        QCOMPARE(modal->geometry(), win.rect());
        QCOMPARE(titleLbl->text(), QStringLiteral("Close Tab?"));
        QVERIFY(bodyLbl->text().contains(QLatin1String("work-tab")));
        QTRY_VERIFY_WITH_TIMEOUT(cancelBtn->hasFocus(), 2000);

        // Window shortcuts must be blocked while modal is open
        QTest::keyClick(cancelBtn, Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
        QApplication::processEvents();
        QCOMPARE(tabs->count(), 2);
        QVERIFY(modal->isVisible());

        // Modal tracks window resize and paints cleanly; clicking inside card does NOT dismiss it
        win.resize(1040, 660);
        QTRY_COMPARE_WITH_TIMEOUT(modal->geometry(), win.rect(), 2000);
        QVERIFY(!win.grab().toImage().isNull());
        QTest::mouseClick(titleLbl, Qt::LeftButton);
        QApplication::processEvents();
        QVERIFY(modal->isVisible());

        // Accidental Enter/Return cancels safely (Cancel is focused by default)
        QTest::keyClick(cancelBtn, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!modal->isVisible(), 2000);
        QVERIFY(win.isVisible());
        QCOMPARE(tabs->count(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(term1->hasFocus(), 2000);

        // 2. Escape and clicking backdrop scrim outside the card both cancel safely
        QTest::mouseClick(closeTab1Btn, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(modal->isVisible(), 2000);
        QTest::keyClick(cancelBtn, Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!modal->isVisible(), 2000);
        QCOMPARE(tabs->count(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(term1->hasFocus(), 2000);

        QTest::mouseClick(closeTab1Btn, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(modal->isVisible(), 2000);
        QTest::mouseClick(modal, Qt::LeftButton, Qt::NoModifier, QPoint(8, 8));
        QTRY_VERIFY_WITH_TIMEOUT(!modal->isVisible(), 2000);
        QCOMPARE(tabs->count(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(term1->hasFocus(), 2000);

        // 3. Multi-split tab: tab close shows pane count; Ctrl+Shift+W closes active split without modal
        pane1->splitActive(Qt::Horizontal);
        QCOMPARE(pane1->terminalCount(), 2);
        QTest::mouseClick(findCloseButtonForTab(1), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(modal->isVisible(), 2000);
        QCOMPARE(titleLbl->text(), QStringLiteral("Close Tab with 2 Panes?"));
        QVERIFY(bodyLbl->text().contains(QLatin1String("2 split panes")));
        QTest::mouseClick(cancelBtn, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!modal->isVisible(), 2000);
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(pane1->terminalCount(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(pane1->activeTerminal() && pane1->activeTerminal()->hasFocus(), 2000);

        QTest::keyClick(pane1->activeTerminal(), Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(pane1->terminalCount(), 1, 2000);
        QVERIFY(!modal->isVisible());
        QCOMPARE(tabs->count(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(pane1->activeTerminal() && pane1->activeTerminal()->hasFocus(), 2000);

        // 4. Closing a non-active tab via its close button closes ONLY that tab, keeping active tab & app open
        tabs->setTabText(0, QStringLiteral("main-tab"));
        tabs->setTabText(1, QStringLiteral("build-tab"));
        tabs->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(term0->hasFocus(), 2000);
        const QPointer<TerminalTabPane> guardPane1(pane1);
        QTest::mouseClick(findCloseButtonForTab(1), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(modal->isVisible(), 2000);
        QVERIFY(bodyLbl->text().contains(QLatin1String("build-tab")));
        QVERIFY(!bodyLbl->text().contains(QLatin1String("main-tab")));

        // Arrow-key navigation inside modal: Right focuses Close, Left focuses Cancel, Tab toggles
        QTest::keyClick(cancelBtn, Qt::Key_Right);
        QTRY_VERIFY_WITH_TIMEOUT(confirmBtn->hasFocus(), 2000);
        QTest::keyClick(confirmBtn, Qt::Key_Left);
        QTRY_VERIFY_WITH_TIMEOUT(cancelBtn->hasFocus(), 2000);
        QTest::keyClick(cancelBtn, Qt::Key_Tab);
        QTRY_VERIFY_WITH_TIMEOUT(confirmBtn->hasFocus(), 2000);

        // Return while Close is focused confirms closing ONLY Tab 1
        QTest::keyClick(confirmBtn, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!modal->isVisible(), 2000);
        QVERIFY(win.isVisible());
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->currentWidget(), static_cast<QWidget *>(pane0));
        QTRY_VERIFY_WITH_TIMEOUT(guardPane1.isNull(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(term0->hasFocus(), 2000);
    }

    [[maybe_unused]] static void testClosingLastTabNeverClosesApplication() {
        MainWindow win;
        win.resize(960, 620);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        win.activateWindow();

        auto *tabs = win.findChild<QTabWidget *>();
        QVERIFY(tabs != nullptr);
        QTabBar *bar = tabs->tabBar();
        QVERIFY(bar != nullptr);

        auto *modal = win.findChild<QWidget *>(QStringLiteral("tabCloseConfirmModal"));
        auto *cancelBtn = win.findChild<QPushButton *>(QStringLiteral("tabCloseCancelButton"));
        auto *confirmBtn = win.findChild<QPushButton *>(QStringLiteral("tabCloseConfirmButton"));
        QVERIFY(modal != nullptr && cancelBtn != nullptr && confirmBtn != nullptr);

        auto findCloseButtonForTab = [bar](int idx) -> QAbstractButton * {
            if (auto *btn = qobject_cast<QAbstractButton *>(bar->tabButton(idx, QTabBar::RightSide))) {
                return btn;
            }
            return qobject_cast<QAbstractButton *>(bar->tabButton(idx, QTabBar::LeftSide));
        };

        // 1. Closing the only tab via the tab's close button -> Cancel keeps it and app open
        QCOMPARE(tabs->count(), 1);
        const QPointer<TerminalTabPane> origPane(qobject_cast<TerminalTabPane *>(tabs->widget(0)));
        QVERIFY(origPane);
        QTRY_VERIFY_WITH_TIMEOUT(origPane->activeTerminal() && origPane->activeTerminal()->hasFocus(), 2000);

        QAbstractButton *closeBtn0 = findCloseButtonForTab(0);
        QVERIFY(closeBtn0 != nullptr);
        QTest::mouseClick(closeBtn0, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(modal->isVisible(), 2000);
        QTest::mouseClick(cancelBtn, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!modal->isVisible(), 2000);
        QVERIFY(win.isVisible());
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->widget(0), static_cast<QWidget *>(origPane.data()));

        // 2. Confirming close on the ONLY tab must close that tab and open a fresh one, NOT close the window
        QTest::mouseClick(findCloseButtonForTab(0), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(modal->isVisible(), 2000);
        QTest::mouseClick(confirmBtn, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!modal->isVisible(), 2000);
        QVERIFY2(win.isVisible(), "Closing the last tab must NOT close the MainWindow/application");
        QCOMPARE(tabs->count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(origPane.isNull(), 2000);

        const QPointer<TerminalTabPane> freshPane(qobject_cast<TerminalTabPane *>(tabs->widget(0)));
        QVERIFY(freshPane);
        QTRY_VERIFY_WITH_TIMEOUT(freshPane->activeTerminal() && freshPane->activeTerminal()->hasFocus(), 2000);

        // 3. Confirming close via Ctrl+Shift+W on the last tab also keeps the app open with a fresh tab
        QTest::keyClick(freshPane->activeTerminal(), Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(modal->isVisible(), 2000);
        QTest::mouseClick(confirmBtn, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!modal->isVisible(), 2000);
        QVERIFY2(win.isVisible(), "Ctrl+Shift+W on the last tab must NOT close the MainWindow/application");
        QCOMPARE(tabs->count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(freshPane.isNull(), 2000);

        auto *paneA = qobject_cast<TerminalTabPane *>(tabs->widget(0));
        QVERIFY(paneA != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(paneA->activeTerminal() && paneA->activeTerminal()->hasFocus(), 2000);

        // 4. Stale index safety: open Tab 1, show close modal for Tab 1, let Tab 0 exit while modal is open
        QTest::keyClick(paneA->activeTerminal(), Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 3000);
        const QPointer<TerminalTabPane> paneB(qobject_cast<TerminalTabPane *>(tabs->widget(1)));
        QVERIFY(paneB);

        QTest::mouseClick(findCloseButtonForTab(1), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(modal->isVisible(), 2000);

        // Tab 0 empties while modal is targeting Tab 1 -> Tab 1 shifts to index 0, modal stays open
        Q_EMIT paneA->emptied(paneA);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 2000);
        QVERIFY(modal->isVisible());
        QCOMPARE(tabs->widget(0), static_cast<QWidget *>(paneB.data()));

        // Confirming now closes paneB cleanly without closing the application
        QTest::mouseClick(confirmBtn, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!modal->isVisible(), 2000);
        QVERIFY(win.isVisible());
        QCOMPARE(tabs->count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(paneB.isNull(), 2000);

        // 5. Auto-dismiss safety: if the pending tab itself empties while its modal is open, modal dismisses
        auto *paneC = qobject_cast<TerminalTabPane *>(tabs->widget(0));
        QVERIFY(paneC != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(paneC->activeTerminal() && paneC->activeTerminal()->hasFocus(), 2000);
        QTest::keyClick(paneC->activeTerminal(), Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 3000);
        auto *paneD = qobject_cast<TerminalTabPane *>(tabs->widget(1));
        QVERIFY(paneD != nullptr);

        QTest::mouseClick(findCloseButtonForTab(1), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(modal->isVisible(), 2000);
        Q_EMIT paneD->emptied(paneD);
        QTRY_VERIFY_WITH_TIMEOUT(!modal->isVisible(), 2000);
        QVERIFY(win.isVisible());
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->widget(0), static_cast<QWidget *>(paneC));
        QTRY_VERIFY_WITH_TIMEOUT(paneC->activeTerminal() && paneC->activeTerminal()->hasFocus(), 2000);
    }

    [[maybe_unused]] static void testAdaptiveTabBarSizing() {
        MainWindow win;
        win.resize(1000, 600);
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        win.activateWindow();

        auto *tabs = win.findChild<QTabWidget *>();
        QVERIFY(tabs != nullptr);
        QTabBar *bar = tabs->tabBar();
        QVERIFY(bar != nullptr);
        QCOMPARE(bar->elideMode(), Qt::ElideMiddle);

        // Short path title must NOT shrink tab below preferred width (200px)
        tabs->setTabText(0, QStringLiteral("~"));
        QApplication::processEvents();
        QCOMPARE(bar->tabRect(0).width(), 200);

        // Very long path title must stay capped at preferred width (200px)
        tabs->setTabText(0, QStringLiteral("/home/ak/projects/very/deeply/nested/workspace/directory/path"));
        QApplication::processEvents();
        QCOMPARE(bar->tabRect(0).width(), 200);

        // 3 tabs on 1000px window still have plenty of space -> all remain 200px
        for (int i = 0; i < 2; ++i) {
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
            QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), i + 2, 3000);
        }
        tabs->setTabText(1, QStringLiteral("/"));
        tabs->setTabText(2, QStringLiteral("zsh"));
        QApplication::processEvents();
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(bar->tabRect(i).width(), 200);
        }

        // Add 3 more tabs (total 6 -> 1200px > ~970px container): tabs shrink evenly to fit
        for (int i = 0; i < 3; ++i) {
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
            QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), i + 4, 3000);
        }
        QApplication::processEvents();
        const int shrunkWidth = bar->tabRect(0).width();
        QVERIFY2(shrunkWidth < 200 && shrunkWidth >= 90,
                 qPrintable(QStringLiteral("Expected shrunk width in [90, 200), got %1").arg(shrunkWidth)));
        for (int i = 1; i < 6; ++i) {
            QCOMPARE(bar->tabRect(i).width(), shrunkWidth);
        }
        QVERIFY(shrunkWidth * 6 <= tabs->width());

        // Widening window so 6 * 200 fits restores tabs back to 200px
        win.resize(1400, 600);
        QTRY_COMPARE_WITH_TIMEOUT(bar->tabRect(0).width(), 200, 2000);
        for (int i = 1; i < 6; ++i) {
            QCOMPARE(bar->tabRect(i).width(), 200);
        }

        // Narrowing window below 6 * 90 clamps tabs to minimum floor (90px)
        win.resize(450, 600);
        QTRY_COMPARE_WITH_TIMEOUT(bar->tabRect(0).width(), 90, 2000);
    }
};

#include "smoke_test.moc"

} // namespace

QTEST_MAIN(GhosttyUnionSmokeTest)
