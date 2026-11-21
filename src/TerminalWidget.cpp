#include "TerminalWidget.h"
#include "KeyMap.h"
#include "Theme.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCursor>
#include <QDBusInterface>
#include <QDir>
#include <QDragEnterEvent> // IWYU pragma: keep
#include <QDropEvent> // IWYU pragma: keep
#include <QEasingCurve>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent> // IWYU pragma: keep
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent> // IWYU pragma: keep
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTransform>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QVariantList>
#include <QVariantMap>
#include <QWheelEvent> // IWYU pragma: keep

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

QColor resolveCellColor(bool hasExplicit, const GhosttyColorRgb &rgb,
                        const GhosttyColorRgb &fallback, bool faint) {
    QColor col = hasExplicit ? GhosttyUnionTheme::toQColor(rgb)
                             : GhosttyUnionTheme::toQColor(fallback);
    if (faint) {
        col.setAlphaF(col.alphaF() * 0.65f);
    }
    return col;
}

QColor resolveStyleColor(const GhosttyStyleColor &sc, const GhosttyColorRgb *palette,
                         const QColor &fallback) {
    switch (sc.tag) {
    case GHOSTTY_STYLE_COLOR_PALETTE:
        return GhosttyUnionTheme::toQColor(palette[sc.value.palette]);
    case GHOSTTY_STYLE_COLOR_RGB:
        return GhosttyUnionTheme::toQColor(sc.value.rgb);
    case GHOSTTY_STYLE_COLOR_NONE:
    default:
        return fallback;
    }
}

void drawUnderlineStyle(QPainter &p, int x, int baselineY, int width, int cellH,
                        int style, const QColor &color) {
    const int y = std::min(baselineY + 2, cellH - 2);
    QPen pen(color, 1.0);
    switch (style) {
    case GHOSTTY_SGR_UNDERLINE_DOUBLE:
        p.setPen(pen);
        p.drawLine(x, y - 1, x + width - 1, y - 1);
        p.drawLine(x, std::min(y + 1, cellH - 1), x + width - 1, std::min(y + 1, cellH - 1));
        break;
    case GHOSTTY_SGR_UNDERLINE_DOTTED:
        pen.setStyle(Qt::DotLine);
        p.setPen(pen);
        p.drawLine(x, y, x + width - 1, y);
        break;
    case GHOSTTY_SGR_UNDERLINE_DASHED:
        pen.setStyle(Qt::DashLine);
        p.setPen(pen);
        p.drawLine(x, y, x + width - 1, y);
        break;
    case GHOSTTY_SGR_UNDERLINE_CURLY: {
        p.setPen(pen);
        for (int cx = x; cx < x + width - 1; cx += 2) {
            constexpr int amp = 1;
            const int y0 = y + (((cx - x) / 2) % 2 == 0 ? 0 : amp);
            const int y1 = y + (((cx - x) / 2) % 2 == 0 ? amp : 0);
            p.drawLine(cx, y0, std::min(cx + 2, x + width - 1), y1);
        }
        break;
    }
    case GHOSTTY_SGR_UNDERLINE_SINGLE:
    default:
        p.setPen(pen);
        p.drawLine(x, y, x + width - 1, y);
        break;
    }
}

struct PasteContext {
    QByteArray text;
};

bool pasteMimeReader(void *userdata, GhosttyString /*mime*/, GhosttyWriter writer) {
    const auto *ctx = static_cast<PasteContext *>(userdata);
    if (!ctx || ctx->text.isEmpty()) {
        return true;
    }
    return writer.write(writer.userdata,
                        reinterpret_cast<const uint8_t *>(ctx->text.constData()),
                        static_cast<size_t>(ctx->text.size()));
}

bool isBlockOrShade(uint32_t cp) {
    return cp >= 0x2580U && cp <= 0x259FU;
}

bool isBoxDrawing(uint32_t cp) {
    return cp >= 0x2500U && cp <= 0x257FU;
}

constexpr uint32_t kKittyUnicodePlaceholder = 0x10EEEEU;

bool isPowerlineSymbol(uint32_t cp) {
    return cp >= 0xE0B0U && cp <= 0xE0BFU;
}

bool isNerdFontPua(uint32_t cp) {
    if (isPowerlineSymbol(cp) || cp == kKittyUnicodePlaceholder) {
        return false;
    }
    return (cp >= 0xE000U && cp <= 0xF8FFU) ||
           (cp >= 0xF0000U && cp <= 0xFFFFDU) ||
           (cp >= 0x100000U && cp <= 0x10FFFDU);
}

bool decodeKittyImagePayload(void * /*userdata*/,
                             const GhosttyAllocator *allocator,
                             const uint8_t *data,
                             size_t dataLen,
                             GhosttySysImage *out) {
    if (!data || dataLen == 0 || dataLen > 128ULL * 1024ULL * 1024ULL || !out) {
        return false;
    }

    const QByteArray raw = QByteArray::fromRawData(reinterpret_cast<const char *>(data),
                                                   static_cast<qsizetype>(dataLen));
    QBuffer buffer;
    buffer.setData(raw);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return false;
    }

    QImageReader reader(&buffer);
    reader.setAllocationLimit(256);
    QImage decoded = reader.read();
    if (decoded.isNull() || decoded.width() <= 0 || decoded.height() <= 0) {
        return false;
    }
    if (decoded.format() != QImage::Format_RGBA8888) {
        decoded = decoded.convertToFormat(QImage::Format_RGBA8888);
    }
    if (decoded.isNull()) {
        return false;
    }

    const auto w = static_cast<uint64_t>(decoded.width());
    const auto h = static_cast<uint64_t>(decoded.height());
    if (w == 0 || h == 0 || w > 16384ULL || h > 16384ULL || w * h > 64ULL * 1024ULL * 1024ULL) {
        return false;
    }

    const size_t rowBytes = static_cast<size_t>(w * 4ULL);
    const size_t totalBytes = static_cast<size_t>(w * h * 4ULL);
    uint8_t *dst = ghostty_alloc(allocator, totalBytes);
    if (!dst) {
        return false;
    }

    if (static_cast<size_t>(decoded.bytesPerLine()) == rowBytes) {
        std::memcpy(dst, decoded.constBits(), totalBytes);
    } else {
        for (int y = 0; y < decoded.height(); ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * rowBytes, decoded.constScanLine(y), rowBytes);
        }
    }

    out->width = static_cast<uint32_t>(w);
    out->height = static_cast<uint32_t>(h);
    out->data = dst;
    out->data_len = totalBytes;
    return true;
}

void ensureGhosttySysImageDecoderInstalled() {
    static std::once_flag once;
    std::call_once(once, []() {
        ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG,
                        reinterpret_cast<const void *>(&decodeKittyImagePayload));
    });
}

class SolarForgeImageLightbox final : public QWidget {
public:
    SolarForgeImageLightbox(const QImage &image, uint32_t imageId, QWidget *returnFocus, QWidget *parentWindow)
        : QWidget(parentWindow)
        , m_image(image.convertToFormat(QImage::Format_RGBA8888_Premultiplied))
        , m_exportImage(image.convertToFormat(QImage::Format_RGBA8888))
        , m_imageId(imageId)
        , m_returnFocus(returnFocus) {
        setObjectName(QStringLiteral("imagePreviewLightbox"));
        setAttribute(Qt::WA_DeleteOnClose, true);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        if (parentWindow) {
            setGeometry(parentWindow->rect());
            parentWindow->installEventFilter(this);
        }

        auto *rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(28, 28, 28, 28);

        m_card = new QWidget(this);
        m_card->setObjectName(QStringLiteral("imagePreviewCard"));
        auto *cardLayout = new QVBoxLayout(m_card);
        cardLayout->setContentsMargins(16, 14, 16, 14);
        cardLayout->setSpacing(10);

        auto *headerRow = new QHBoxLayout();
        headerRow->setSpacing(8);

        m_titleLabel = new QLabel(m_card);
        m_titleLabel->setStyleSheet(QStringLiteral(
            "color: #F1F0F6; font-weight: 700; font-size: 13px; background: transparent;"));
        headerRow->addWidget(m_titleLabel);
        headerRow->addStretch(1);

        auto makeToolBtn = [this](const QString &text, const QString &objName, bool accent = false) {
            auto *btn = new QPushButton(text, m_card);
            btn->setObjectName(objName);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setFocusPolicy(Qt::NoFocus);
            btn->setStyleSheet(accent
                ? QStringLiteral(
                      "QPushButton { background: rgba(255, 211, 0, 36); color: #FFD300;"
                      " border: 1px solid rgba(255, 211, 0, 130); border-radius: 6px;"
                      " padding: 4px 10px; font-size: 12px; font-weight: 600; }"
                      "QPushButton:hover { background: rgba(255, 211, 0, 68); }")
                : QStringLiteral(
                      "QPushButton { background: rgba(255, 255, 255, 16); color: #E5E4EE;"
                      " border: 1px solid #45413B; border-radius: 6px;"
                      " padding: 4px 9px; font-size: 12px; }"
                      "QPushButton:hover { border-color: #FFD300; color: #FFD300; }"));
            return btn;
        };

        auto *zoomOutBtn = makeToolBtn(QStringLiteral("−"), QStringLiteral("lightboxZoomOutButton"));
        auto *zoomResetBtn = makeToolBtn(QStringLiteral("1:1"), QStringLiteral("lightboxResetZoomButton"));
        auto *fitBtn = makeToolBtn(QStringLiteral("Fit"), QStringLiteral("lightboxFitButton"));
        auto *zoomInBtn = makeToolBtn(QStringLiteral("+"), QStringLiteral("lightboxZoomInButton"));
        auto *gridBtn = makeToolBtn(QStringLiteral("Alpha Grid"), QStringLiteral("lightboxGridButton"));
        auto *copyBtn = makeToolBtn(QStringLiteral("Copy"), QStringLiteral("lightboxCopyButton"), true);
        auto *saveBtn = makeToolBtn(QStringLiteral("Save As…"), QStringLiteral("lightboxSaveButton"));
        auto *closeBtn = makeToolBtn(QStringLiteral("Close"), QStringLiteral("lightboxCloseButton"));

        connect(zoomOutBtn, &QPushButton::clicked, this, [this]() { stepZoom(1.0 / 1.25); });
        connect(zoomResetBtn, &QPushButton::clicked, this, [this]() {
            m_fitMode = false;
            m_zoom = 1.0;
            m_panOffset = {0.0, 0.0};
            refreshHeader();
            update();
        });
        connect(fitBtn, &QPushButton::clicked, this, [this]() {
            m_fitMode = true;
            m_panOffset = {0.0, 0.0};
            recomputeFitZoom();
            refreshHeader();
            update();
        });
        connect(zoomInBtn, &QPushButton::clicked, this, [this]() { stepZoom(1.25); });
        connect(gridBtn, &QPushButton::clicked, this, [this]() {
            m_checkerboard = !m_checkerboard;
            update();
        });
        connect(copyBtn, &QPushButton::clicked, this, [this, copyBtn]() {
            if (QClipboard *cb = QApplication::clipboard()) {
                cb->setImage(m_exportImage, QClipboard::Clipboard);
                copyBtn->setText(QStringLiteral("Copied"));
            }
        });
        connect(saveBtn, &QPushButton::clicked, this, [this]() {
            const QString picturesDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
            const QString baseDir = picturesDir.isEmpty() ? QDir::homePath() : picturesDir;
            const QString suggested = QDir(baseDir).filePath(
                QStringLiteral("ghostty-union-image-%1.png").arg(m_imageId));
            const QString target = QFileDialog::getSaveFileName(
                this,
                QStringLiteral("Save Terminal Image"),
                suggested,
                QStringLiteral("PNG Image (*.png);;WebP Image (*.webp);;JPEG Image (*.jpg *.jpeg)"));
            if (!target.isEmpty()) {
                m_exportImage.save(target);
            }
        });
        connect(closeBtn, &QPushButton::clicked, this, &SolarForgeImageLightbox::dismiss);

        headerRow->addWidget(zoomOutBtn);
        headerRow->addWidget(zoomResetBtn);
        headerRow->addWidget(fitBtn);
        headerRow->addWidget(zoomInBtn);
        headerRow->addWidget(gridBtn);
        headerRow->addWidget(copyBtn);
        headerRow->addWidget(saveBtn);
        headerRow->addWidget(closeBtn);
        cardLayout->addLayout(headerRow);

        m_canvas = new QWidget(m_card);
        m_canvas->setObjectName(QStringLiteral("imagePreviewCanvas"));
        m_canvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_canvas->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        cardLayout->addWidget(m_canvas, 1);

        rootLayout->addWidget(m_card, 1);
        recomputeFitZoom();
        refreshHeader();
    }

    void dismiss() {
        const QPointer<QWidget> target = m_returnFocus;
        close();
        if (target) {
            target->setFocus(Qt::OtherFocusReason);
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == parentWidget() && event->type() == QEvent::Resize && parentWidget()) {
            setGeometry(parentWidget()->rect());
            if (m_fitMode) {
                recomputeFitZoom();
                refreshHeader();
            }
        }
        return QWidget::eventFilter(watched, event);
    }

    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        if (m_fitMode) {
            recomputeFitZoom();
            refreshHeader();
        }
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Q) {
            dismiss();
            event->accept();
            return;
        }
        if (event->modifiers().testFlag(Qt::ControlModifier) && event->key() == Qt::Key_C) {
            if (QClipboard *cb = QApplication::clipboard()) {
                cb->setImage(m_exportImage, QClipboard::Clipboard);
            }
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) {
            stepZoom(1.25);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Minus) {
            stepZoom(1.0 / 1.25);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_0 || event->key() == Qt::Key_1) {
            m_fitMode = false;
            m_zoom = 1.0;
            m_panOffset = {0.0, 0.0};
            refreshHeader();
            update();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_F) {
            m_fitMode = true;
            m_panOffset = {0.0, 0.0};
            recomputeFitZoom();
            refreshHeader();
            update();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_G) {
            m_checkerboard = !m_checkerboard;
            update();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override {
        const int dy = event->angleDelta().y();
        if (dy != 0) {
            stepZoom(dy > 0 ? 1.18 : (1.0 / 1.18));
            event->accept();
            return;
        }
        QWidget::wheelEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            if (m_card && !m_card->geometry().contains(event->position().toPoint())) {
                dismiss();
                event->accept();
                return;
            }
            m_panning = true;
            m_lastPanPos = event->position();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (m_panning) {
            const QPointF delta = event->position() - m_lastPanPos;
            m_lastPanPos = event->position();
            m_panOffset += delta;
            update();
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && m_panning) {
            m_panning = false;
            unsetCursor();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent * /*event*/) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(15, 14, 19, 224));

        if (!m_card || !m_canvas) {
            return;
        }

        const QRectF cardRect = QRectF(m_card->geometry()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath cardClip;
        cardClip.addRoundedRect(cardRect, 14.0, 14.0);
        p.save();
        p.setClipPath(cardClip);

        QLinearGradient cardBg(cardRect.topLeft(), cardRect.bottomLeft());
        cardBg.setColorAt(0.0, QColor(0x2B, 0x29, 0x33, 250));
        cardBg.setColorAt(1.0, QColor(0x1B, 0x1A, 0x21, 252));
        p.fillRect(cardRect, cardBg);

        QLinearGradient topBar(cardRect.topLeft(), cardRect.topRight());
        topBar.setColorAt(0.0, QColor(GhosttyUnionTheme::kGoldHex));
        topBar.setColorAt(0.6, QColor(GhosttyUnionTheme::kAmberHex));
        topBar.setColorAt(1.0, QColor(GhosttyUnionTheme::kRedOrangeHex));
        p.fillRect(QRectF(cardRect.left(), cardRect.top(), cardRect.width(), 2.5), topBar);
        p.restore();

        p.setPen(QPen(QColor(GhosttyUnionTheme::kChromeBorderHex), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(cardRect, 14.0, 14.0);

        const QRect canvasInSelf = m_canvas->geometry().translated(m_card->pos());
        const QRectF viewRect = QRectF(canvasInSelf).adjusted(2.0, 2.0, -2.0, -2.0);
        if (viewRect.width() <= 4.0 || viewRect.height() <= 4.0 || m_image.isNull()) {
            return;
        }

        p.save();
        QPainterPath viewClip;
        viewClip.addRoundedRect(viewRect, 8.0, 8.0);
        p.setClipPath(viewClip);
        p.fillRect(viewRect, QColor(0x14, 0x13, 0x18));

        const qreal drawW = m_image.width() * m_zoom;
        const qreal drawH = m_image.height() * m_zoom;
        const QPointF center = viewRect.center() + m_panOffset;
        const QRectF imgRect(center.x() - drawW * 0.5, center.y() - drawH * 0.5, drawW, drawH);

        if (m_checkerboard) {
            const QRectF visImg = imgRect.intersected(viewRect);
            constexpr int tile = 10;
            const int x0 = static_cast<int>(std::floor(visImg.left() / tile)) * tile;
            const int y0 = static_cast<int>(std::floor(visImg.top() / tile)) * tile;
            p.save();
            p.setClipRect(visImg);
            p.setRenderHint(QPainter::Antialiasing, false);
            for (int cy = y0; cy < visImg.bottom(); cy += tile) {
                for (int cx = x0; cx < visImg.right(); cx += tile) {
                    const bool dark = (((cx / tile) + (cy / tile)) & 1) == 0;
                    p.fillRect(cx, cy, tile, tile, dark ? QColor(0x22, 0x21, 0x28) : QColor(0x2E, 0x2C, 0x36));
                }
            }
            p.restore();
        }

        p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 2.0);
        p.drawImage(imgRect, m_image);

        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(QColor(255, 211, 0, 90), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(imgRect);
        p.restore();
    }

private:
    void recomputeFitZoom() {
        if (m_image.isNull()) {
            m_zoom = 1.0;
            return;
        }
        const QSize avail = (m_canvas && m_canvas->width() > 32 && m_canvas->height() > 32)
                                ? m_canvas->size() - QSize(24, 24)
                                : QSize(std::max(120, width() - 120), std::max(100, height() - 140));
        const qreal sx = static_cast<qreal>(avail.width()) / static_cast<qreal>(m_image.width());
        const qreal sy = static_cast<qreal>(avail.height()) / static_cast<qreal>(m_image.height());
        m_zoom = std::clamp(std::min({sx, sy, 4.0}), 0.10, 16.0);
    }

    void stepZoom(qreal factor) {
        m_fitMode = false;
        m_zoom = std::clamp(m_zoom * factor, 0.10, 16.0);
        refreshHeader();
        update();
    }

    void refreshHeader() {
        if (!m_titleLabel) {
            return;
        }
        const int pct = static_cast<int>(std::lround(m_zoom * 100.0));
        m_titleLabel->setText(QStringLiteral("IMAGE INSPECTOR  ·  #%1  ·  %2 × %3 px  ·  %4%")
                                  .arg(m_imageId)
                                  .arg(m_image.width())
                                  .arg(m_image.height())
                                  .arg(pct));
    }

    QImage m_image;
    QImage m_exportImage;
    uint32_t m_imageId = 0;
    QPointer<QWidget> m_returnFocus;
    QWidget *m_card = nullptr;
    QWidget *m_canvas = nullptr;
    QLabel *m_titleLabel = nullptr;
    qreal m_zoom = 1.0;
    bool m_fitMode = true;
    bool m_checkerboard = true;
    QPointF m_panOffset{0.0, 0.0};
    bool m_panning = false;
    QPointF m_lastPanPos;
};

void drawBlockElement(QPainter &p, uint32_t cp, int x, int w, int h, const QColor &fg) {
    p.setRenderHint(QPainter::Antialiasing, false);
    if (cp == 0x2580U) {
        p.fillRect(QRectF(x, 0.0, w, h * 0.5), fg);
        return;
    }
    if (cp >= 0x2581U && cp <= 0x2587U) {
        const qreal frac = static_cast<qreal>(cp - 0x2580U) / 8.0;
        const qreal yTop = h * (1.0 - frac);
        p.fillRect(QRectF(x, yTop, w, h - yTop), fg);
        return;
    }
    if (cp == 0x2588U) {
        p.fillRect(x, 0, w, h, fg);
        return;
    }
    if (cp >= 0x2589U && cp <= 0x258FU) {
        const qreal frac = static_cast<qreal>(0x2590U - cp) / 8.0;
        p.fillRect(QRectF(x, 0.0, w * frac, h), fg);
        return;
    }
    if (cp == 0x2590U) {
        p.fillRect(QRectF(x + w * 0.5, 0.0, w - w * 0.5, h), fg);
        return;
    }
    if (cp >= 0x2591U && cp <= 0x2593U) {
        const float alphaFactor = (cp == 0x2591U) ? 0.25f : ((cp == 0x2592U) ? 0.50f : 0.75f);
        QColor shade = fg;
        shade.setAlphaF(fg.alphaF() * alphaFactor);
        p.fillRect(x, 0, w, h, shade);
        return;
    }
    if (cp == 0x2594U) {
        p.fillRect(QRectF(x, 0.0, w, std::max(1.0, h / 8.0)), fg);
        return;
    }
    if (cp == 0x2595U) {
        const qreal barW = std::max(1.0, w / 8.0);
        p.fillRect(QRectF(x + w - barW, 0.0, barW, h), fg);
        return;
    }
    // Quadrants U+2596..U+259F: bitmask LL=1, LR=2, UL=4, UR=8
    static constexpr std::array<uint8_t, 10> kQuadMasks = {
        1,  // 2596 ▖ LL
        2,  // 2597 ▗ LR
        4,  // 2598 ▘ UL
        7,  // 2599 ▙ UL|LL|LR
        6,  // 259A ▚ UL|LR
        13, // 259B ▛ UL|UR|LL
        14, // 259C ▜ UL|UR|LR
        8,  // 259D ▝ UR
        9,  // 259E ▞ UR|LL
        11, // 259F ▟ UR|LL|LR
    };
    const uint8_t mask = kQuadMasks[cp - 0x2596U];
    const qreal halfW = w * 0.5;
    const qreal halfH = h * 0.5;
    if (mask & 4U) {
        p.fillRect(QRectF(x, 0.0, halfW, halfH), fg);
    }
    if (mask & 8U) {
        p.fillRect(QRectF(x + halfW, 0.0, w - halfW, halfH), fg);
    }
    if (mask & 1U) {
        p.fillRect(QRectF(x, halfH, halfW, h - halfH), fg);
    }
    if (mask & 2U) {
        p.fillRect(QRectF(x + halfW, halfH, w - halfW, h - halfH), fg);
    }
}

bool drawBoxDrawing(QPainter &p, uint32_t cp, int x, int w, int h, const QColor &fg) {
    const int midXi = x + w / 2;
    const int midYi = h / 2;

    // Rounded arcs U+256D..U+2570
    if (cp >= 0x256DU && cp <= 0x2570U) {
        p.save();
        p.setClipRect(QRect(x, 0, w, h));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(fg, 1.0, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);

        const qreal cx = midXi + 0.5;
        const qreal cy = midYi + 0.5;
        const qreal x0 = x;
        const qreal x1 = x + w;
        constexpr qreal y0 = 0.0;
        const qreal y1 = h;
        const qreal r = std::max(0.5, std::min<qreal>({4.0, (w - 1) * 0.5, (h - 1) * 0.5}));
        const qreal k = 0.55228475 * r;

        QPainterPath path;
        if (cp == 0x256DU) { // ╭ down + right
            path.moveTo(cx, y1);
            path.lineTo(cx, cy + r);
            path.cubicTo(cx, cy + r - k, cx + r - k, cy, cx + r, cy);
            path.lineTo(x1, cy);
        } else if (cp == 0x256EU) { // ╮ down + left
            path.moveTo(cx, y1);
            path.lineTo(cx, cy + r);
            path.cubicTo(cx, cy + r - k, cx - r + k, cy, cx - r, cy);
            path.lineTo(x0, cy);
        } else if (cp == 0x256FU) { // ╯ up + left
            path.moveTo(cx, y0);
            path.lineTo(cx, cy - r);
            path.cubicTo(cx, cy - r + k, cx - r + k, cy, cx - r, cy);
            path.lineTo(x0, cy);
        } else { // 0x2570 ╰ up + right
            path.moveTo(cx, y0);
            path.lineTo(cx, cy - r);
            path.cubicTo(cx, cy - r + k, cx + r - k, cy, cx + r, cy);
            path.lineTo(x1, cy);
        }
        p.drawPath(path);
        p.restore();
        return true;
    }

    // Diagonals U+2571..U+2573
    if (cp >= 0x2571U && cp <= 0x2573U) {
        p.save();
        p.setClipRect(QRect(x, 0, w, h));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(fg, 1.0, Qt::SolidLine, Qt::SquareCap));
        if (cp == 0x2571U || cp == 0x2573U) {
            p.drawLine(QPointF(x, h), QPointF(x + w, 0.0));
        }
        if (cp == 0x2572U || cp == 0x2573U) {
            p.drawLine(QPointF(x, 0.0), QPointF(x + w, h));
        }
        p.restore();
        return true;
    }

    // Dashed horizontals / verticals
    auto drawDashedH = [&](int count, int weight) {
        p.setRenderHint(QPainter::Antialiasing, false);
        const qreal step = static_cast<qreal>(w) / count;
        const qreal dashLen = std::max(1.0, step * 0.6);
        for (int i = 0; i < count; ++i) {
            p.fillRect(QRectF(x + i * step, midYi, dashLen, weight), fg);
        }
    };
    auto drawDashedV = [&](int count, int weight) {
        p.setRenderHint(QPainter::Antialiasing, false);
        const qreal step = static_cast<qreal>(h) / count;
        const qreal dashLen = std::max(1.0, step * 0.6);
        for (int i = 0; i < count; ++i) {
            p.fillRect(QRectF(midXi, i * step, weight, dashLen), fg);
        }
    };
    switch (cp) {
    case 0x2504U: drawDashedH(3, 1); return true;
    case 0x2505U: drawDashedH(3, 2); return true;
    case 0x2506U: drawDashedV(3, 1); return true;
    case 0x2507U: drawDashedV(3, 2); return true;
    case 0x2508U: drawDashedH(4, 1); return true;
    case 0x2509U: drawDashedH(4, 2); return true;
    case 0x250AU: drawDashedV(4, 1); return true;
    case 0x250BU: drawDashedV(4, 2); return true;
    case 0x254CU: drawDashedH(2, 1); return true;
    case 0x254DU: drawDashedH(2, 2); return true;
    case 0x254EU: drawDashedV(2, 1); return true;
    case 0x254FU: drawDashedV(2, 2); return true;
    default: break;
    }

    // Arm weights: 0 = none, 1 = light, 2 = heavy, 3 = double
    struct BoxArms {
        uint8_t left;
        uint8_t right;
        uint8_t up;
        uint8_t down;
    };
    static constexpr std::array<BoxArms, 128> kTable = {{
        {1, 1, 0, 0}, // 2500 ─
        {2, 2, 0, 0}, // 2501 ━
        {0, 0, 1, 1}, // 2502 │
        {0, 0, 2, 2}, // 2503 ┃
        {0, 0, 0, 0}, // 2504 (dashed)
        {0, 0, 0, 0}, // 2505 (dashed)
        {0, 0, 0, 0}, // 2506 (dashed)
        {0, 0, 0, 0}, // 2507 (dashed)
        {0, 0, 0, 0}, // 2508 (dashed)
        {0, 0, 0, 0}, // 2509 (dashed)
        {0, 0, 0, 0}, // 250A (dashed)
        {0, 0, 0, 0}, // 250B (dashed)
        {0, 1, 0, 1}, // 250C ┌
        {0, 2, 0, 1}, // 250D ┍
        {0, 1, 0, 2}, // 250E ┎
        {0, 2, 0, 2}, // 250F ┏
        {1, 0, 0, 1}, // 2510 ┐
        {2, 0, 0, 1}, // 2511 ┑
        {1, 0, 0, 2}, // 2512 ┒
        {2, 0, 0, 2}, // 2513 ┓
        {0, 1, 1, 0}, // 2514 └
        {0, 2, 1, 0}, // 2515 ┕
        {0, 1, 2, 0}, // 2516 ┖
        {0, 2, 2, 0}, // 2517 ┗
        {1, 0, 1, 0}, // 2518 ┘
        {2, 0, 1, 0}, // 2519 ┙
        {1, 0, 2, 0}, // 251A ┚
        {2, 0, 2, 0}, // 251B ┛
        {0, 1, 1, 1}, // 251C ├
        {0, 2, 1, 1}, // 251D ┝
        {0, 1, 2, 1}, // 251E ┞
        {0, 1, 1, 2}, // 251F ┟
        {0, 1, 2, 2}, // 2520 ┠
        {0, 2, 2, 1}, // 2521 ┡
        {0, 2, 1, 2}, // 2522 ┢
        {0, 2, 2, 2}, // 2523 ┣
        {1, 0, 1, 1}, // 2524 ┤
        {2, 0, 1, 1}, // 2525 ┥
        {1, 0, 2, 1}, // 2526 ┦
        {1, 0, 1, 2}, // 2527 ┧
        {1, 0, 2, 2}, // 2528 ┨
        {2, 0, 2, 1}, // 2529 ┩
        {2, 0, 1, 2}, // 252A ┪
        {2, 0, 2, 2}, // 252B ┫
        {1, 1, 0, 1}, // 252C ┬
        {2, 1, 0, 1}, // 252D ┭
        {1, 2, 0, 1}, // 252E ┮
        {2, 2, 0, 1}, // 252F ┯
        {1, 1, 0, 2}, // 2530 ┰
        {2, 1, 0, 2}, // 2531 ┱
        {1, 2, 0, 2}, // 2532 ┲
        {2, 2, 0, 2}, // 2533 ┳
        {1, 1, 1, 0}, // 2534 ┴
        {2, 1, 1, 0}, // 2535 ┵
        {1, 2, 1, 0}, // 2536 ┶
        {2, 2, 1, 0}, // 2537 ┷
        {1, 1, 2, 0}, // 2538 ┸
        {2, 1, 2, 0}, // 2539 ┹
        {1, 2, 2, 0}, // 253A ┺
        {2, 2, 2, 0}, // 253B ┻
        {1, 1, 1, 1}, // 253C ┼
        {2, 1, 1, 1}, // 253D ┽
        {1, 2, 1, 1}, // 253E ┾
        {2, 2, 1, 1}, // 253F ┿
        {1, 1, 2, 1}, // 2540 ╀
        {1, 1, 1, 2}, // 2541 ╁
        {1, 1, 2, 2}, // 2542 ╂
        {2, 1, 2, 1}, // 2543 ╃
        {1, 2, 2, 1}, // 2544 ╄
        {2, 1, 1, 2}, // 2545 ╅
        {1, 2, 1, 2}, // 2546 ╆
        {2, 2, 2, 1}, // 2547 ╇
        {2, 2, 1, 2}, // 2548 ╈
        {2, 1, 2, 2}, // 2549 ╉
        {1, 2, 2, 2}, // 254A ╊
        {2, 2, 2, 2}, // 254B ╋
        {0, 0, 0, 0}, // 254C (dashed)
        {0, 0, 0, 0}, // 254D (dashed)
        {0, 0, 0, 0}, // 254E (dashed)
        {0, 0, 0, 0}, // 254F (dashed)
        {3, 3, 0, 0}, // 2550 ═
        {0, 0, 3, 3}, // 2551 ║
        {0, 3, 0, 1}, // 2552 ╒
        {0, 1, 0, 3}, // 2553 ╓
        {0, 3, 0, 3}, // 2554 ╔
        {3, 0, 0, 1}, // 2555 ╕
        {1, 0, 0, 3}, // 2556 ╖
        {3, 0, 0, 3}, // 2557 ╗
        {0, 3, 1, 0}, // 2558 ╘
        {0, 1, 3, 0}, // 2559 ╙
        {0, 3, 3, 0}, // 255A ╚
        {3, 0, 1, 0}, // 255B ╛
        {1, 0, 3, 0}, // 255C ╜
        {3, 0, 3, 0}, // 255D ╝
        {0, 3, 1, 1}, // 255E ╞
        {0, 1, 3, 3}, // 255F ╟
        {0, 3, 3, 3}, // 2560 ╠
        {3, 0, 1, 1}, // 2561 ╡
        {1, 0, 3, 3}, // 2562 ╢
        {3, 0, 3, 3}, // 2563 ╣
        {3, 3, 0, 1}, // 2564 ╤
        {1, 1, 0, 3}, // 2565 ╥
        {3, 3, 0, 3}, // 2566 ╦
        {3, 3, 1, 0}, // 2567 ╧
        {1, 1, 3, 0}, // 2568 ╨
        {3, 3, 3, 0}, // 2569 ╩
        {3, 3, 1, 1}, // 256A ╪
        {1, 1, 3, 3}, // 256B ╫
        {3, 3, 3, 3}, // 256C ╬
        {0, 0, 0, 0}, // 256D (arc)
        {0, 0, 0, 0}, // 256E (arc)
        {0, 0, 0, 0}, // 256F (arc)
        {0, 0, 0, 0}, // 2570 (arc)
        {0, 0, 0, 0}, // 2571 (diag)
        {0, 0, 0, 0}, // 2572 (diag)
        {0, 0, 0, 0}, // 2573 (diag)
        {1, 0, 0, 0}, // 2574 ╴
        {0, 0, 1, 0}, // 2575 ╵
        {0, 1, 0, 0}, // 2576 ╶
        {0, 0, 0, 1}, // 2577 ╷
        {2, 0, 0, 0}, // 2578 ╸
        {0, 0, 2, 0}, // 2579 ╹
        {0, 2, 0, 0}, // 257A ╺
        {0, 0, 0, 2}, // 257B ╻
        {1, 2, 0, 0}, // 257C ╼
        {0, 0, 1, 2}, // 257D ╽
        {2, 1, 0, 0}, // 257E ╾
        {0, 0, 2, 1}, // 257F ╿
    }};

    const BoxArms arms = kTable[cp - 0x2500U];
    if (arms.left == 0 && arms.right == 0 && arms.up == 0 && arms.down == 0) {
        return false;
    }

    p.setRenderHint(QPainter::Antialiasing, false);

    // Light / heavy arms (weights 1 or 2)
    if (arms.left == 1 || arms.left == 2) {
        int stopX = midXi + (arms.up == 2 || arms.down == 2 ? 2 : 1);
        if (arms.up == 3 || arms.down == 3) {
            stopX = midXi + (arms.right == 0 ? 2 : 1);
        }
        stopX = std::min(x + w, stopX);
        p.fillRect(x, midYi, stopX - x, arms.left, fg);
    }
    if (arms.right == 1 || arms.right == 2) {
        int startX = midXi;
        if ((arms.up == 3 || arms.down == 3) && arms.left == 0) {
            startX = std::max(x, midXi - 1);
        }
        p.fillRect(startX, midYi, (x + w) - startX, arms.right, fg);
    }
    if (arms.up == 1 || arms.up == 2) {
        int stopY = midYi + (arms.left == 2 || arms.right == 2 ? 2 : 1);
        if (arms.left == 3 || arms.right == 3) {
            stopY = midYi + (arms.down == 0 ? 2 : 1);
        }
        stopY = std::min(h, stopY);
        p.fillRect(midXi, 0, arms.up, stopY, fg);
    }
    if (arms.down == 1 || arms.down == 2) {
        int startY = midYi;
        if ((arms.left == 3 || arms.right == 3) && arms.up == 0) {
            startY = std::max(0, midYi - 1);
        }
        p.fillRect(midXi, startY, arms.down, h - startY, fg);
    }

    // Double horizontal rails (y = midYi - 1 and midYi + 1)
    if (arms.left == 3) {
        const int topEnd = (arms.up == 3)
                               ? midXi
                               : ((arms.down == 3 && arms.right == 0) ? (midXi + 2) : (midXi + 1));
        const int botEnd = (arms.down == 3)
                               ? midXi
                               : ((arms.up == 3 && arms.right == 0) ? (midXi + 2) : (midXi + 1));
        p.fillRect(x, midYi - 1, std::min(x + w, topEnd) - x, 1, fg);
        p.fillRect(x, midYi + 1, std::min(x + w, botEnd) - x, 1, fg);
    }
    if (arms.right == 3) {
        const int topStart = (arms.up == 3)
                                 ? (midXi + 1)
                                 : ((arms.down == 3 && arms.left == 0) ? (midXi - 1) : midXi);
        const int botStart = (arms.down == 3)
                                 ? (midXi + 1)
                                 : ((arms.up == 3 && arms.left == 0) ? (midXi - 1) : midXi);
        p.fillRect(topStart, midYi - 1, (x + w) - topStart, 1, fg);
        p.fillRect(botStart, midYi + 1, (x + w) - botStart, 1, fg);
    }

    // Double vertical rails (x = midXi - 1 and midXi + 1)
    if (arms.up == 3) {
        const int leftEnd = (arms.left == 3)
                                ? midYi
                                : ((arms.right == 3 && arms.down == 0) ? (midYi + 2) : (midYi + 1));
        const int rightEnd = (arms.right == 3)
                                 ? midYi
                                 : ((arms.left == 3 && arms.down == 0) ? (midYi + 2) : (midYi + 1));
        p.fillRect(midXi - 1, 0, 1, std::min(h, leftEnd), fg);
        p.fillRect(midXi + 1, 0, 1, std::min(h, rightEnd), fg);
    }
    if (arms.down == 3) {
        const int leftStart = (arms.left == 3)
                                  ? (midYi + 1)
                                  : ((arms.right == 3 && arms.up == 0) ? (midYi - 1) : midYi);
        const int rightStart = (arms.right == 3)
                                   ? (midYi + 1)
                                   : ((arms.left == 3 && arms.up == 0) ? (midYi - 1) : midYi);
        p.fillRect(midXi - 1, leftStart, 1, h - leftStart, fg);
        p.fillRect(midXi + 1, rightStart, 1, h - rightStart, fg);
    }

    return true;
}

void drawPowerlineSymbol(QPainter &p, uint32_t cp, int x, int w, int h, const QColor &fg) {
    p.save();
    p.setClipRect(QRect(x, 0, w, h));
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal x0 = x;
    const qreal x1 = x + w;
    constexpr qreal y0 = 0.0;
    const qreal y1 = h;
    const qreal ym = h * 0.5;

    auto fillPoly = [&](std::initializer_list<QPointF> pts) {
        QPainterPath path;
        auto it = pts.begin();
        if (it != pts.end()) {
            path.moveTo(*it++);
            while (it != pts.end()) {
                path.lineTo(*it++);
            }
            path.closeSubpath();
        }
        p.fillPath(path, fg);
    };

    const QPen thinPen(fg, 1.25, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);

    switch (cp) {
    case 0xE0B0U: //  solid right triangle
        fillPoly({{x0 - 1.0, y0}, {x0, y0}, {x1, ym}, {x0, y1}, {x0 - 1.0, y1}});
        break;
    case 0xE0B1U: { //  thin right chevron
        p.setPen(thinPen);
        p.setBrush(Qt::NoBrush);
        QPainterPath path;
        path.moveTo(x0 + 0.5, y0);
        path.lineTo(x1 - 0.5, ym);
        path.lineTo(x0 + 0.5, y1);
        p.drawPath(path);
        break;
    }
    case 0xE0B2U: //  solid left triangle
        fillPoly({{x1 + 1.0, y0}, {x1, y0}, {x0, ym}, {x1, y1}, {x1 + 1.0, y1}});
        break;
    case 0xE0B3U: { //  thin left chevron
        p.setPen(thinPen);
        p.setBrush(Qt::NoBrush);
        QPainterPath path;
        path.moveTo(x1 - 0.5, y0);
        path.lineTo(x0 + 0.5, ym);
        path.lineTo(x1 - 0.5, y1);
        p.drawPath(path);
        break;
    }
    case 0xE0B4U: { //  solid right pill cap
        QPainterPath path;
        path.moveTo(x0 - 1.0, y0);
        path.lineTo(x0, y0);
        path.arcTo(QRectF(x0 - w, y0, 2.0 * w, h), 90.0, -180.0);
        path.lineTo(x0 - 1.0, y1);
        path.closeSubpath();
        p.fillPath(path, fg);
        break;
    }
    case 0xE0B5U: { //  thin right arc
        p.setPen(thinPen);
        p.setBrush(Qt::NoBrush);
        const QRectF oval(x0 - w + 0.5, 0.5, 2.0 * w - 1.25, h - 1.0);
        QPainterPath path;
        path.arcMoveTo(oval, 90.0);
        path.arcTo(oval, 90.0, -180.0);
        p.drawPath(path);
        break;
    }
    case 0xE0B6U: { //  solid left pill cap
        QPainterPath path;
        path.moveTo(x1 + 1.0, y0);
        path.lineTo(x1, y0);
        path.arcTo(QRectF(x0, y0, 2.0 * w, h), 90.0, 180.0);
        path.lineTo(x1 + 1.0, y1);
        path.closeSubpath();
        p.fillPath(path, fg);
        break;
    }
    case 0xE0B7U: { //  thin left arc
        p.setPen(thinPen);
        p.setBrush(Qt::NoBrush);
        const QRectF oval(x0 + 0.75, 0.5, 2.0 * w - 1.25, h - 1.0);
        QPainterPath path;
        path.arcMoveTo(oval, 90.0);
        path.arcTo(oval, 90.0, 180.0);
        p.drawPath(path);
        break;
    }
    case 0xE0B8U: //  lower-left triangle
        fillPoly({{x0 - 1.0, y0}, {x0, y0}, {x1, y1}, {x0 - 1.0, y1}});
        break;
    case 0xE0B9U: //  backslash separator
    case 0xE0BFU: //  backslash separator
        p.setPen(thinPen);
        p.drawLine(QPointF(x0, y0), QPointF(x1, y1));
        break;
    case 0xE0BAU: //  lower-right triangle
        fillPoly({{x1 + 1.0, y0}, {x1, y0}, {x0, y1}, {x1 + 1.0, y1}});
        break;
    case 0xE0BBU: //  forward-slash separator
    case 0xE0BDU: //  forward-slash separator
        p.setPen(thinPen);
        p.drawLine(QPointF(x1, y0), QPointF(x0, y1));
        break;
    case 0xE0BCU: //  upper-left triangle
        fillPoly({{x0 - 1.0, y0}, {x1, y0}, {x0, y1}, {x0 - 1.0, y1}});
        break;
    case 0xE0BEU: //  upper-right triangle
        fillPoly({{x0, y0}, {x1 + 1.0, y0}, {x1 + 1.0, y1}, {x1, y1}});
        break;
    default:
        break;
    }

    p.restore();
}

QString sanitizeNotificationText(const QString &s) {
    QString out;
    out.reserve(s.size());
    for (const QChar ch : s) {
        const ushort uc = ch.unicode();
        if (uc < 0x20U || uc == 0x7fU) {
            if (uc == '\n' || uc == '\t') {
                out.append(QLatin1Char(' '));
            }
            continue;
        }
        // Strip directional override / isolate formatting characters
        if ((uc >= 0x202AU && uc <= 0x202EU) ||
            (uc >= 0x2066U && uc <= 0x2069U) ||
            uc == 0x200EU || uc == 0x200FU) {
            continue;
        }
        out.append(ch);
    }
    return out;
}

Qt::CursorShape mapGhosttyMouseShape(GhosttyMouseShape shape) {
    switch (shape) {
    case GHOSTTY_MOUSE_SHAPE_DEFAULT:
    case GHOSTTY_MOUSE_SHAPE_CONTEXT_MENU:
    case GHOSTTY_MOUSE_SHAPE_ZOOM_IN:
    case GHOSTTY_MOUSE_SHAPE_ZOOM_OUT:
        return Qt::ArrowCursor;
    case GHOSTTY_MOUSE_SHAPE_HELP:
        return Qt::WhatsThisCursor;
    case GHOSTTY_MOUSE_SHAPE_POINTER:
        return Qt::PointingHandCursor;
    case GHOSTTY_MOUSE_SHAPE_PROGRESS:
        return Qt::BusyCursor;
    case GHOSTTY_MOUSE_SHAPE_WAIT:
        return Qt::WaitCursor;
    case GHOSTTY_MOUSE_SHAPE_CELL:
    case GHOSTTY_MOUSE_SHAPE_CROSSHAIR:
        return Qt::CrossCursor;
    case GHOSTTY_MOUSE_SHAPE_TEXT:
    case GHOSTTY_MOUSE_SHAPE_VERTICAL_TEXT:
        return Qt::IBeamCursor;
    case GHOSTTY_MOUSE_SHAPE_ALIAS:
        return Qt::DragLinkCursor;
    case GHOSTTY_MOUSE_SHAPE_COPY:
        return Qt::DragCopyCursor;
    case GHOSTTY_MOUSE_SHAPE_MOVE:
    case GHOSTTY_MOUSE_SHAPE_ALL_SCROLL:
        return Qt::SizeAllCursor;
    case GHOSTTY_MOUSE_SHAPE_NO_DROP:
    case GHOSTTY_MOUSE_SHAPE_NOT_ALLOWED:
        return Qt::ForbiddenCursor;
    case GHOSTTY_MOUSE_SHAPE_GRAB:
        return Qt::OpenHandCursor;
    case GHOSTTY_MOUSE_SHAPE_GRABBING:
        return Qt::ClosedHandCursor;
    case GHOSTTY_MOUSE_SHAPE_COL_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_E_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_W_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_EW_RESIZE:
        return Qt::SizeHorCursor;
    case GHOSTTY_MOUSE_SHAPE_ROW_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_N_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_S_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_NS_RESIZE:
        return Qt::SizeVerCursor;
    case GHOSTTY_MOUSE_SHAPE_NE_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_SW_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_NESW_RESIZE:
        return Qt::SizeBDiagCursor;
    case GHOSTTY_MOUSE_SHAPE_NW_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_SE_RESIZE:
    case GHOSTTY_MOUSE_SHAPE_NWSE_RESIZE:
        return Qt::SizeFDiagCursor;
    default:
        return Qt::IBeamCursor;
    }
}

} // namespace

TerminalWidget::TerminalWidget(const QString &workingDirectory, QWidget *parent, bool startShell)
    : QWidget(parent)
    , m_startShellPending(startShell)
    , m_pwd(workingDirectory.isEmpty() ? QDir::homePath() : workingDirectory) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_InputMethodEnabled, false);
    setMouseTracking(true);
    setAcceptDrops(true);

    m_clock.start();

    m_renderHoldTimer.setSingleShot(true);
    m_renderHoldTimer.setInterval(1000);
    connect(&m_renderHoldTimer, &QTimer::timeout, this, &TerminalWidget::onRenderHoldTimeout);

    m_blinkTimer.setInterval(530);
    connect(&m_blinkTimer, &QTimer::timeout, this, &TerminalWidget::onCursorBlink);
    m_blinkTimer.start();

    m_resizeCoalesceTimer.setSingleShot(true);
    m_resizeCoalesceTimer.setInterval(20);
    connect(&m_resizeCoalesceTimer, &QTimer::timeout, this, &TerminalWidget::resizeTerminalToWidget);

    m_kittyAnimTimer.setSingleShot(true);
    connect(&m_kittyAnimTimer, &QTimer::timeout, this, &TerminalWidget::onKittyAnimationTimer);

    m_scrollbarHoverAnim = new QVariantAnimation(this);
    m_scrollbarHoverAnim->setDuration(140);
    m_scrollbarHoverAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_scrollbarHoverAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_scrollbarHoverProgress = v.toReal();
        update();
    });

    m_splitGripAnim = new QVariantAnimation(this);
    m_splitGripAnim->setDuration(150);
    m_splitGripAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_splitGripAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_splitGripProgress = v.toReal();
        update();
    });

    ensureGhosttySysImageDecoderInstalled();
    updateFontMetrics();
    if (!initGhostty()) {
        return;
    }
    updateRenderState(true);
    refreshMouseCursor();
}

TerminalWidget::~TerminalWidget() {
    if (m_ptyNotifier) {
        m_ptyNotifier->setEnabled(false);
        delete m_ptyNotifier;
        m_ptyNotifier = nullptr;
    }
    if (m_ptyMasterFd >= 0) {
        ::close(m_ptyMasterFd);
        m_ptyMasterFd = -1;
    }
    if (m_childPid > 0) {
        ::kill(m_childPid, SIGHUP);
        int status = 0;
        ::waitpid(m_childPid, &status, WNOHANG);
        m_childPid = -1;
    }
    destroyGhostty();
}

void TerminalWidget::updateFontMetrics() {
    m_font = QFont(QStringLiteral("FiraCode Nerd Font SemBd"));
    m_font.setPointSizeF(m_basePointSize);
    m_font.setStyleHint(QFont::Monospace);

    m_boldFont = m_font;
    m_boldFont.setBold(true);

    m_italicFont = m_font;
    m_italicFont.setItalic(true);

    m_boldItalicFont = m_boldFont;
    m_boldItalicFont.setItalic(true);

    const QFontMetricsF fm(m_font);
    m_cellWidth = std::max(1, static_cast<int>(std::ceil(fm.horizontalAdvance(QLatin1Char('M')))));
    const int baseCellH = std::max(1, static_cast<int>(std::ceil(fm.height())));
    const int baseAscent = static_cast<int>(std::ceil(fm.ascent()));
    m_capHeight = (fm.capHeight() > 0.0) ? fm.capHeight() : (fm.ascent() * 0.75);

    const qreal dpr = std::max(1.0, devicePixelRatioF());
    m_metricsDpr = dpr;
    int snappedH = baseCellH;
    for (int delta = 0; delta <= 3; ++delta) {
        const int candidate = baseCellH + delta;
        const qreal devPx = candidate * dpr;
        if (std::abs(devPx - std::round(devPx)) < 0.01) {
            snappedH = candidate;
            break;
        }
    }
    m_cellHeight = snappedH;
    m_ascent = baseAscent + (m_cellHeight - baseCellH) / 2;
    m_puaPathCache.clear();
    m_rowCache.clear();
}

QRectF TerminalWidget::viewportGridRect() const {
    return {
        static_cast<qreal>(kPadX),
        static_cast<qreal>(kPadY),
        static_cast<qreal>(m_cols) * static_cast<qreal>(m_cellWidth),
        static_cast<qreal>(m_rows) * static_cast<qreal>(m_cellHeight),
    };
}

int TerminalWidget::deviceCellWidth() const {
    const qreal dpr = std::max(1.0, m_metricsDpr > 0.0 ? m_metricsDpr : devicePixelRatioF());
    return std::max(1, static_cast<int>(std::lround(static_cast<qreal>(m_cellWidth) * dpr)));
}

int TerminalWidget::deviceCellHeight() const {
    const qreal dpr = std::max(1.0, m_metricsDpr > 0.0 ? m_metricsDpr : devicePixelRatioF());
    return std::max(1, static_cast<int>(std::lround(static_cast<qreal>(m_cellHeight) * dpr)));
}

bool TerminalWidget::initGhostty() {
    if (ghostty_terminal_new(nullptr, &m_terminal, m_cols, m_rows) != GHOSTTY_SUCCESS) {
        return false;
    }
    m_appliedVtCols = m_cols;
    m_appliedVtRows = m_rows;
    m_appliedCellWidth = m_cellWidth;
    m_appliedCellHeight = m_cellHeight;
    m_appliedVtCellWidth = deviceCellWidth();
    m_appliedVtCellHeight = deviceCellHeight();
    ghostty_terminal_resize(m_terminal, m_cols, m_rows,
                            static_cast<uint32_t>(m_appliedVtCellWidth),
                            static_cast<uint32_t>(m_appliedVtCellHeight));
    if (ghostty_render_state_new(nullptr, &m_renderState) != GHOSTTY_SUCCESS ||
        ghostty_render_state_row_iterator_new(nullptr, &m_rowIter) != GHOSTTY_SUCCESS ||
        ghostty_render_state_row_cells_new(nullptr, &m_cellsIter) != GHOSTTY_SUCCESS ||
        ghostty_render_state_image_iterator_new(nullptr, &m_imageIter) != GHOSTTY_SUCCESS ||
        ghostty_key_encoder_new(nullptr, &m_keyEncoder) != GHOSTTY_SUCCESS ||
        ghostty_key_event_new(nullptr, &m_keyEvent) != GHOSTTY_SUCCESS ||
        ghostty_mouse_encoder_new(nullptr, &m_mouseEncoder) != GHOSTTY_SUCCESS ||
        ghostty_mouse_event_new(nullptr, &m_mouseEvent) != GHOSTTY_SUCCESS ||
        ghostty_selection_gesture_new(nullptr, &m_gesture) != GHOSTTY_SUCCESS ||
        ghostty_selection_gesture_event_new(nullptr, &m_pressEvent, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_PRESS) != GHOSTTY_SUCCESS ||
        ghostty_selection_gesture_event_new(nullptr, &m_dragEvent, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_DRAG) != GHOSTTY_SUCCESS ||
        ghostty_selection_gesture_event_new(nullptr, &m_releaseEvent, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_RELEASE) != GHOSTTY_SUCCESS) {
        destroyGhostty();
        return false;
    }

    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_USERDATA, this);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY, reinterpret_cast<const void *>(&TerminalWidget::onWritePty));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED, reinterpret_cast<const void *>(&TerminalWidget::onTitleChanged));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_PWD_CHANGED, reinterpret_cast<const void *>(&TerminalWidget::onPwdChanged));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_BELL, reinterpret_cast<const void *>(&TerminalWidget::onBell));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SIZE, reinterpret_cast<const void *>(&TerminalWidget::onSizeQuery));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_SCHEME, reinterpret_cast<const void *>(&TerminalWidget::onColorSchemeQuery));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES, reinterpret_cast<const void *>(&TerminalWidget::onDeviceAttributesQuery));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_XTVERSION, reinterpret_cast<const void *>(&TerminalWidget::onXtversionQuery));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE, reinterpret_cast<const void *>(&TerminalWidget::onClipboardWrite));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_RENDER_HOLD, reinterpret_cast<const void *>(&TerminalWidget::onRenderHold));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_RESET, reinterpret_cast<const void *>(&TerminalWidget::onReset));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SEMANTIC_PROMPT, reinterpret_cast<const void *>(&TerminalWidget::onSemanticPrompt));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_DESKTOP_NOTIFICATION, reinterpret_cast<const void *>(&TerminalWidget::onDesktopNotification));

    // Match Ghostty 1.4 Kitty Graphics defaults (320 MB budget + direct/file/temp/shm mediums)
    constexpr uint64_t kittyStorageLimitBytes = 320000000ULL;
    constexpr bool enableMedium = true;
    constexpr size_t maxKittyApcBytes = 128ULL * 1024ULL * 1024ULL;
    const QByteArray tempDirBytes = QDir::tempPath().toUtf8();
    const GhosttyString tempDirStr{
        reinterpret_cast<const uint8_t *>(tempDirBytes.constData()),
        static_cast<size_t>(tempDirBytes.size()),
    };
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &kittyStorageLimitBytes);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_FILE, &enableMedium);
    if (!tempDirBytes.isEmpty()) {
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_TEMP_FILE, &tempDirStr);
    }
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_SHARED_MEM, &enableMedium);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_APC_MAX_BYTES_KITTY, &maxKittyApcBytes);

    const bool hasGhosttyTerminfo = QFileInfo::exists(QStringLiteral("/usr/share/terminfo/x/xterm-ghostty"));
    const char *terminfoName = hasGhosttyTerminfo ? "xterm-ghostty" : "xterm-256color";
    const GhosttyString ti{reinterpret_cast<const uint8_t *>(terminfoName), std::strlen(terminfoName)};
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_TERMINFO_NAME, &ti);

    constexpr size_t maxScrollBytes = 50ULL * 1024ULL * 1024ULL;
    constexpr size_t maxScrollLines = 50000;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &maxScrollBytes);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &maxScrollLines);

    constexpr bool trackLastCell = true;
    ghostty_mouse_encoder_set(m_mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL, &trackLastCell);

    applyTerminalPalette();
    return true;
}

void TerminalWidget::applyTerminalPalette() const {
    constexpr GhosttyColorRgb bg = GhosttyUnionTheme::kOllieBackground;
    constexpr GhosttyColorRgb fg = GhosttyUnionTheme::kOllieForeground;
    constexpr GhosttyColorRgb cur = GhosttyUnionTheme::kOllieCursor;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &bg);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &fg);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &cur);

    GhosttyColorRgb palette[256];
    ghostty_color_palette_default(palette);
    for (size_t i = 0; i < GhosttyUnionTheme::kOllieAnsi16.size(); ++i) {
        palette[i] = GhosttyUnionTheme::kOllieAnsi16[i];
    }
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, palette);
}

void TerminalWidget::destroyGhostty() {
    if (m_gesture) {
        ghostty_selection_gesture_free(m_gesture, m_terminal);
        m_gesture = nullptr;
    }
    ghostty_selection_gesture_event_free(m_pressEvent);
    ghostty_selection_gesture_event_free(m_dragEvent);
    ghostty_selection_gesture_event_free(m_releaseEvent);
    m_pressEvent = m_dragEvent = m_releaseEvent = nullptr;

    ghostty_mouse_event_free(m_mouseEvent);
    ghostty_mouse_encoder_free(m_mouseEncoder);
    m_mouseEvent = nullptr;
    m_mouseEncoder = nullptr;

    ghostty_key_event_free(m_keyEvent);
    ghostty_key_encoder_free(m_keyEncoder);
    m_keyEvent = nullptr;
    m_keyEncoder = nullptr;

    ghostty_render_state_image_iterator_free(m_imageIter);
    ghostty_render_state_row_cells_free(m_cellsIter);
    ghostty_render_state_row_iterator_free(m_rowIter);
    ghostty_render_state_free(m_renderState);
    m_imageIter = nullptr;
    m_cellsIter = nullptr;
    m_rowIter = nullptr;
    m_renderState = nullptr;

    m_kittyAnimTimer.stop();
    m_visiblePlacements.clear();
    m_textureCache.clear();
    m_pendingOsc1337Seqs.clear();

    ghostty_terminal_free(m_terminal);
    m_terminal = nullptr;
}

bool TerminalWidget::spawnShell(const QString &workingDirectory) {
    const int devCw = deviceCellWidth();
    const int devCh = deviceCellHeight();
    winsize ws{
        .ws_row = m_rows,
        .ws_col = m_cols,
        .ws_xpixel = static_cast<unsigned short>(std::min<int>(65535, static_cast<int>(m_cols) * devCw)),
        .ws_ypixel = static_cast<unsigned short>(std::min<int>(65535, static_cast<int>(m_rows) * devCh)),
    };

    int masterFd = -1;
    int slaveFd = -1;
    if (::openpty(&masterFd, &slaveFd, nullptr, nullptr, &ws) != 0) {
        return false;
    }

    ::fcntl(masterFd, F_SETFD, FD_CLOEXEC);
    ::fcntl(slaveFd, F_SETFD, FD_CLOEXEC);

    const QByteArray cwdBytes = workingDirectory.toUtf8();
    const QByteArray shellEnv = qgetenv("SHELL");
    const char *shellPath = shellEnv.isEmpty() ? "/bin/zsh" : shellEnv.constData();

    const bool hasGhosttyTerminfo = QFileInfo::exists(QStringLiteral("/usr/share/terminfo/x/xterm-ghostty"));
    const QByteArray termEnv = hasGhosttyTerminfo ? QByteArrayLiteral("xterm-ghostty")
                                                  : QByteArrayLiteral("xterm-256color");
    const bool hasGhosttyResources = QFileInfo(QStringLiteral("/usr/share/ghostty")).isDir();
    const QString shellBase = QFileInfo(QString::fromLocal8Bit(shellPath)).fileName();
    const bool hasGhosttyZshIntegration =
        (shellBase == QStringLiteral("zsh")) &&
        QFileInfo(QStringLiteral("/usr/share/ghostty/shell-integration/zsh/.zshenv")).isReadable();
    const QByteArray existingZshDotDir = qgetenv("ZDOTDIR");

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(masterFd);
        ::close(slaveFd);
        return false;
    }

    if (pid == 0) {
        ::close(masterFd);
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        ::signal(SIGCHLD, SIG_DFL);
        ::signal(SIGPIPE, SIG_DFL);

        ::setsid();
        ::ioctl(slaveFd, TIOCSCTTY, 0);

        ::dup2(slaveFd, STDIN_FILENO);
        ::dup2(slaveFd, STDOUT_FILENO);
        ::dup2(slaveFd, STDERR_FILENO);
        if (slaveFd > STDERR_FILENO) {
            ::close(slaveFd);
        }

        if (!cwdBytes.isEmpty()) {
            ::chdir(cwdBytes.constData());
        }

        ::setenv("TERM", termEnv.constData(), 1);
        if (hasGhosttyTerminfo) {
            ::setenv("TERMINFO", "/usr/share/terminfo", 0);
        }
        ::setenv("COLORTERM", "truecolor", 1);
        ::setenv("TERM_PROGRAM", "ghostty", 1);
        ::setenv("TERM_PROGRAM_VERSION", "1.4.0", 1);
        if (hasGhosttyResources) {
            ::setenv("GHOSTTY_RESOURCES_DIR", "/usr/share/ghostty", 1);
            ::setenv("GHOSTTY_BIN_DIR", "/usr/bin", 1);
            ::setenv("GHOSTTY_SHELL_FEATURES", "cursor,sudo,title,path", 1);
        }
        if (hasGhosttyZshIntegration) {
            static constexpr const char kGhosttyZshDir[] = "/usr/share/ghostty/shell-integration/zsh";
            if (!existingZshDotDir.isEmpty() && existingZshDotDir != kGhosttyZshDir) {
                ::setenv("GHOSTTY_ZSH_ZDOTDIR", existingZshDotDir.constData(), 1);
            } else {
                ::unsetenv("GHOSTTY_ZSH_ZDOTDIR");
            }
            ::setenv("ZDOTDIR", kGhosttyZshDir, 1);
        }
        ::unsetenv("LINES");
        ::unsetenv("COLUMNS");

        char *const argv[] = {const_cast<char *>(shellPath), nullptr};
        ::execvp(shellPath, argv);
        ::_exit(127);
    }

    ::close(slaveFd);
    m_childPid = pid;
    m_ptyMasterFd = masterFd;
    m_appliedPtyCols = m_cols;
    m_appliedPtyRows = m_rows;
    m_appliedCellWidth = m_cellWidth;
    m_appliedCellHeight = m_cellHeight;
    const int flags = ::fcntl(m_ptyMasterFd, F_GETFL, 0);
    ::fcntl(m_ptyMasterFd, F_SETFL, flags | O_NONBLOCK);

    m_ptyNotifier = new QSocketNotifier(m_ptyMasterFd, QSocketNotifier::Read, this);
    connect(m_ptyNotifier, &QSocketNotifier::activated, this, &TerminalWidget::onPtyReadable);
    return true;
}

void TerminalWidget::onPtyReadable() {
    if (m_ptyMasterFd < 0 || !m_terminal) {
        return;
    }

    uint8_t buf[32768];
    bool hadData = false;
    for (;;) {
        const ssize_t n = ::read(m_ptyMasterFd, buf, sizeof(buf));
        if (n > 0) {
            hadData = true;
            feedVtStream(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            // Child closed PTY (shell exited)
            if (m_ptyNotifier) {
                m_ptyNotifier->setEnabled(false);
            }
            if (!m_exitedEmitted) {
                m_exitedEmitted = true;
                int status = 0;
                if (m_childPid > 0) {
                    ::waitpid(m_childPid, &status, WNOHANG);
                    m_childPid = -1;
                }
                Q_EMIT exited(this, WIFEXITED(status) ? WEXITSTATUS(status) : 0);
            }
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        // EIO when slave closed
        if (m_ptyNotifier) {
            m_ptyNotifier->setEnabled(false);
        }
        if (!m_exitedEmitted) {
            m_exitedEmitted = true;
            int status = 0;
            if (m_childPid > 0) {
                ::waitpid(m_childPid, &status, WNOHANG);
                m_childPid = -1;
            }
            Q_EMIT exited(this, WIFEXITED(status) ? WEXITSTATUS(status) : 0);
        }
        return;
    }

    if (hadData) {
        m_blinkVisible = true;
        if (!m_renderHeld) {
            updateRenderState(false);
        }
        update();
    }
}

void TerminalWidget::flushPtyBytes(const uint8_t *data, size_t len) const {
    if (!data || len == 0) {
        return;
    }
    if (m_ptyOutputSpy) {
        m_ptyOutputSpy(QByteArray(reinterpret_cast<const char *>(data), static_cast<qsizetype>(len)));
    }
    if (m_ptyMasterFd < 0) {
        return;
    }
    size_t sent = 0;
    while (sent < len) {
        const ssize_t w = ::write(m_ptyMasterFd, data + sent, len - sent);
        if (w > 0) {
            sent += static_cast<size_t>(w);
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ::usleep(1000);
        } else {
            break;
        }
    }
}

void TerminalWidget::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (m_startShellPending && m_ptyMasterFd < 0) {
        QTimer::singleShot(0, this, &TerminalWidget::ensureShellSpawned);
    } else if (m_ptyMasterFd >= 0) {
        m_resizeCoalesceTimer.start();
    }
}

void TerminalWidget::ensureShellSpawned() {
    if (!m_startShellPending || m_ptyMasterFd >= 0 || !m_terminal) {
        return;
    }
    m_startShellPending = false;
    resizeTerminalToWidget();
    spawnShell(m_pwd);
}

void TerminalWidget::appendOscCaptureBytes(const uint8_t *bytes, size_t count) {
    if (!bytes || count == 0 || m_oscCaptureState == OscCaptureState::Idle) {
        return;
    }
    static constexpr char kOsc1337Prefix[] = "1337;file=";
    static constexpr qsizetype kPrefixLen = sizeof(kOsc1337Prefix) - 1;
    static constexpr size_t kMaxOsc1337Bytes = 32ULL * 1024ULL * 1024ULL;

    size_t idx = 0;
    if (m_oscCaptureState == OscCaptureState::CheckingPrefix) {
        while (idx < count && m_osc1337Buffer.size() < kPrefixLen) {
            const char ch = static_cast<char>(bytes[idx]);
            const char lower = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
            if (lower != kOsc1337Prefix[m_osc1337Buffer.size()]) {
                m_oscCaptureState = OscCaptureState::Idle;
                m_osc1337Buffer.clear();
                return;
            }
            m_osc1337Buffer.append(ch);
            ++idx;
        }
        if (m_osc1337Buffer.size() == kPrefixLen) {
            m_oscCaptureState = OscCaptureState::Capturing1337File;
        }
    }

    if (m_oscCaptureState == OscCaptureState::Capturing1337File && idx < count) {
        const size_t rem = count - idx;
        if (static_cast<size_t>(m_osc1337Buffer.size()) + rem > kMaxOsc1337Bytes) {
            m_oscCaptureState = OscCaptureState::Idle;
            m_osc1337Buffer.clear();
            return;
        }
        m_osc1337Buffer.append(reinterpret_cast<const char *>(bytes + idx),
                               static_cast<qsizetype>(rem));
    }
}

void TerminalWidget::feedVtStream(const uint8_t *data, size_t len) {
    if (!m_terminal || !data || len == 0) {
        return;
    }

    auto hasPendingGroundWork = [this]() {
        return m_pendingCommandEndSanitize || !m_pendingOsc1337Seqs.isEmpty();
    };

    auto tryFlushAtGround = [this, &hasPendingGroundWork]() {
        if (!hasPendingGroundWork()) {
            return;
        }
        bool atGround = false;
        if (ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_VT_GROUND, &atGround) != GHOSTTY_SUCCESS || !atGround) {
            return;
        }
        if (!m_pendingOsc1337Seqs.isEmpty()) {
            const QVector<QByteArray> queued = std::move(m_pendingOsc1337Seqs);
            m_pendingOsc1337Seqs.clear();
            for (const QByteArray &seq : queued) {
                if (!seq.isEmpty()) {
                    ghostty_terminal_vt_write(m_terminal,
                                              reinterpret_cast<const uint8_t *>(seq.constData()),
                                              static_cast<size_t>(seq.size()));
                }
            }
        }
        if (m_pendingCommandEndSanitize) {
            m_pendingCommandEndSanitize = false;
            applySoftModeSanitization();
        }
    };

    size_t off = 0;
    GhosttyTerminalWriteStop stop = GHOSTTY_INIT_SIZED(GhosttyTerminalWriteStop);
    while (off < len) {
        tryFlushAtGround();
        const uint32_t boundaries = (hasPendingGroundWork() && m_oscCaptureState == OscCaptureState::Idle)
                                        ? GHOSTTY_TERMINAL_BOUNDARY_GROUND
                                        : (GHOSTTY_TERMINAL_BOUNDARY_OSC_START | GHOSTTY_TERMINAL_BOUNDARY_OSC_END);
        const GhosttyResult res = ghostty_terminal_vt_write_until(
            m_terminal, data + off, len - off, boundaries, &stop);
        if (res == GHOSTTY_NO_VALUE) {
            if (m_oscCaptureState != OscCaptureState::Idle) {
                appendOscCaptureBytes(data + off, len - off);
            }
            break;
        }
        if (res != GHOSTTY_SUCCESS) {
            m_oscCaptureState = OscCaptureState::Idle;
            m_osc1337Buffer.clear();
            ghostty_terminal_vt_write(m_terminal, data + off, len - off);
            break;
        }
        if (stop.consumed == 0) {
            if ((stop.reached & GHOSTTY_TERMINAL_BOUNDARY_GROUND) != 0 && hasPendingGroundWork()) {
                tryFlushAtGround();
                continue;
            }
            m_oscCaptureState = OscCaptureState::Idle;
            m_osc1337Buffer.clear();
            ghostty_terminal_vt_write(m_terminal, data + off, len - off);
            break;
        }

        if (m_oscCaptureState != OscCaptureState::Idle) {
            if ((stop.reached & GHOSTTY_TERMINAL_BOUNDARY_OSC_END) != 0) {
                if (stop.consumed > 1) {
                    appendOscCaptureBytes(data + off, stop.consumed - 1);
                }
                if ((stop.end == GHOSTTY_TERMINAL_STRING_END_BEL ||
                     stop.end == GHOSTTY_TERMINAL_STRING_END_ST_ESC) &&
                    m_oscCaptureState == OscCaptureState::Capturing1337File) {
                    enqueueOsc1337ImageSequence(
                        reinterpret_cast<const uint8_t *>(m_osc1337Buffer.constData()),
                        static_cast<size_t>(m_osc1337Buffer.size()));
                }
                m_oscCaptureState = OscCaptureState::Idle;
                m_osc1337Buffer.clear();
            } else {
                appendOscCaptureBytes(data + off, stop.consumed);
            }
        }
        if ((stop.reached & GHOSTTY_TERMINAL_BOUNDARY_OSC_START) != 0) {
            m_oscCaptureState = OscCaptureState::CheckingPrefix;
            m_osc1337Buffer.clear();
        }

        off += stop.consumed;
        tryFlushAtGround();
    }
    tryFlushAtGround();
}

void TerminalWidget::applySoftModeSanitization() {
    if (!m_terminal) {
        return;
    }
    GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
    if (ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen) == GHOSTTY_SUCCESS &&
        screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE) {
        static constexpr char kExitAltSeq[] =
            "\x1b[<99u\x1b[=0u"
            "\x1b[?1049l\x1b[?1047l\x1b[?47l";
        ghostty_terminal_vt_write(m_terminal,
                                  reinterpret_cast<const uint8_t *>(kExitAltSeq),
                                  sizeof(kExitAltSeq) - 1);
    }

    static constexpr char kSanitizePrimarySeq[] =
        "\x1b[<99u\x1b[=0u"
        "\x1b[>4m"
        "\x1b[2l"
        "\x1b[?1l\x1b>"
        "\x1b[?9l\x1b[?1000l\x1b[?1002l\x1b[?1003l"
        "\x1b[?1005l\x1b[?1006l\x1b[?1015l\x1b[?1016l"
        "\x1b[?1004l"
        "\x1b[?2004l"
        "\x1b[?2026l"
        "\x1b[?25h";
    ghostty_terminal_vt_write(m_terminal,
                              reinterpret_cast<const uint8_t *>(kSanitizePrimarySeq),
                              sizeof(kSanitizePrimarySeq) - 1);

    m_hoveredLink = {};
    m_pressedLink = {};
    m_hoveredImage = {};
    m_pressedImage = {};
    m_linkClickPending = false;
    m_imageClickPending = false;
    m_renderHeld = false;
    m_renderHoldTimer.stop();
    m_blinkVisible = true;
    refreshMouseCursor();
}

void TerminalWidget::feedVtBytes(const QByteArray &bytes) {
    if (!m_terminal || bytes.isEmpty()) {
        return;
    }
    feedVtStream(reinterpret_cast<const uint8_t *>(bytes.constData()),
                 static_cast<size_t>(bytes.size()));
    if (!m_renderHeld) {
        updateRenderState(false);
    }
    update();
}

void TerminalWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (m_ptyMasterFd >= 0) {
        // Coalesce transient reparent/layout geometries so zsh ZLE doesn't receive a SIGWINCH storm.
        m_resizeCoalesceTimer.start();
    } else {
        resizeTerminalToWidget();
    }
}

void TerminalWidget::resizeTerminalToWidget() {
    if (!m_terminal) {
        return;
    }
    const qreal dpr = std::max(1.0, devicePixelRatioF());
    if (std::abs(dpr - m_metricsDpr) > 0.001) {
        updateFontMetrics();
    }
    if (m_ptyMasterFd >= 0) {
        if (!isVisible() ||
            width() < kPadX + kPadRight + 8 * std::max(1, m_cellWidth) ||
            height() < 2 * kPadY + 3 * std::max(1, m_cellHeight)) {
            return;
        }
    }
    const int availW = std::max(m_cellWidth, width() - kPadX - kPadRight);
    const int availH = std::max(m_cellHeight, height() - 2 * kPadY);
    const int minCols = (m_ptyMasterFd >= 0) ? 8 : 2;
    const int minRows = (m_ptyMasterFd >= 0) ? 3 : 1;
    const uint16_t newCols = static_cast<uint16_t>(std::clamp(availW / m_cellWidth, minCols, 1000));
    const uint16_t newRows = static_cast<uint16_t>(std::clamp(availH / m_cellHeight, minRows, 1000));

    if (newCols != m_cols || newRows != m_rows) {
        m_cols = newCols;
        m_rows = newRows;
        m_rowCache.clear();
    }

    const int devCw = deviceCellWidth();
    const int devCh = deviceCellHeight();
    const bool cellMetricsChanged =
        (m_cellWidth != m_appliedCellWidth ||
         m_cellHeight != m_appliedCellHeight ||
         devCw != m_appliedVtCellWidth ||
         devCh != m_appliedVtCellHeight);
    if (m_cols != m_appliedVtCols || m_rows != m_appliedVtRows || cellMetricsChanged) {
        ghostty_terminal_resize(m_terminal, m_cols, m_rows,
                                static_cast<uint32_t>(devCw),
                                static_cast<uint32_t>(devCh));
        m_appliedVtCols = m_cols;
        m_appliedVtRows = m_rows;
        m_appliedVtCellWidth = devCw;
        m_appliedVtCellHeight = devCh;
    }

    if (m_ptyMasterFd >= 0 &&
        (m_cols != m_appliedPtyCols || m_rows != m_appliedPtyRows || cellMetricsChanged)) {
        winsize ws{
            .ws_row = m_rows,
            .ws_col = m_cols,
            .ws_xpixel = static_cast<unsigned short>(std::min<int>(65535, static_cast<int>(m_cols) * devCw)),
            .ws_ypixel = static_cast<unsigned short>(std::min<int>(65535, static_cast<int>(m_rows) * devCh)),
        };
        ::ioctl(m_ptyMasterFd, TIOCSWINSZ, &ws);
        m_appliedPtyCols = m_cols;
        m_appliedPtyRows = m_rows;
    }
    m_appliedCellWidth = m_cellWidth;
    m_appliedCellHeight = m_cellHeight;

    const GhosttyMouseEncoderSize msize{
        .size = sizeof(GhosttyMouseEncoderSize),
        .screen_width = static_cast<uint32_t>(std::max(1, width())),
        .screen_height = static_cast<uint32_t>(std::max(1, height())),
        .cell_width = static_cast<uint32_t>(m_cellWidth),
        .cell_height = static_cast<uint32_t>(m_cellHeight),
        .padding_top = kPadY,
        .padding_bottom = kPadY,
        .padding_right = kPadRight,
        .padding_left = kPadX,
    };
    ghostty_mouse_encoder_set(m_mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &msize);

    updateRenderState(true);
    update();
}

void TerminalWidget::updateRenderState(bool forceAllDirty) {
    if (!m_terminal || !m_renderState) {
        return;
    }

    const qreal dpr = std::max(1.0, devicePixelRatioF());
    if (std::abs(dpr - m_metricsDpr) > 0.001) {
        updateFontMetrics();
        const int devCw = deviceCellWidth();
        const int devCh = deviceCellHeight();
        if (devCw != m_appliedVtCellWidth || devCh != m_appliedVtCellHeight) {
            ghostty_terminal_resize(m_terminal, m_cols, m_rows,
                                    static_cast<uint32_t>(devCw),
                                    static_cast<uint32_t>(devCh));
            m_appliedVtCellWidth = devCw;
            m_appliedVtCellHeight = devCh;
        }
        forceAllDirty = true;
    }

    bool hasActiveAnim = false;
    uint64_t animNowMs = 0;
    uint64_t nextAnimMs = 0;
    {
        GhosttyKittyGraphics kg = nullptr;
        if (ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &kg) == GHOSTTY_SUCCESS && kg) {
            animNowMs = static_cast<uint64_t>(std::max<qint64>(0, m_clock.elapsed()));
            hasActiveAnim = (ghostty_kitty_graphics_animation_tick(kg, animNowMs, &nextAnimMs) == GHOSTTY_SUCCESS);
        }
    }

    if (ghostty_render_state_update(m_renderState, m_terminal) != GHOSTTY_SUCCESS) {
        return;
    }

    m_cursor = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR, &m_cursor);

    m_colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_COLORS, &m_colors);

    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &m_scrollbar);
    if (!hasScrollbar() && (m_scrollbarHovered || m_scrollbarDragging)) {
        m_scrollbarHovered = false;
        m_scrollbarDragging = false;
    }

    uint16_t rsRows = 0;
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_ROWS, &rsRows);
    if (m_rowCache.size() != rsRows) {
        m_rowCache.resize(rsRows);
        forceAllDirty = true;
    }

    GhosttyRenderStateDirty dirtyState = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirtyState);
    if (forceAllDirty) {
        dirtyState = GHOSTTY_RENDER_STATE_DIRTY_FULL;
    }

    if (dirtyState != GHOSTTY_RENDER_STATE_DIRTY_FALSE) {
        ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &m_rowIter);
        int y = 0;
        while (ghostty_render_state_row_iterator_next(m_rowIter)) {
            if (y >= m_rowCache.size()) {
                break;
            }
            bool rowDirty = false;
            ghostty_render_state_row_get(m_rowIter, GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &rowDirty);

            GhosttyRenderStateRowId rowId{};
            ghostty_render_state_row_get(m_rowIter, GHOSTTY_RENDER_STATE_ROW_DATA_ID, &rowId);

            GhosttyRenderStateRowSelection rowSel = GHOSTTY_INIT_SIZED(GhosttyRenderStateRowSelection);
            int selStart = -1;
            int selEnd = -1;
            if (ghostty_render_state_row_get(m_rowIter, GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION, &rowSel) == GHOSTTY_SUCCESS) {
                selStart = rowSel.start_x;
                selEnd = rowSel.end_x;
            }

            const CachedRow &cached = m_rowCache[y];
            const bool needRedraw = forceAllDirty ||
                                    dirtyState == GHOSTTY_RENDER_STATE_DIRTY_FULL ||
                                    rowDirty ||
                                    !cached.valid ||
                                    cached.dpr != dpr ||
                                    cached.idBits0 != rowId.bits[0] ||
                                    cached.idBits1 != rowId.bits[1] ||
                                    cached.selStart != selStart ||
                                    cached.selEnd != selEnd;
            if (needRedraw) {
                renderRowToCache(y, m_rowIter, m_colors, selStart, selEnd, dpr);
                m_rowCache[y].idBits0 = rowId.bits[0];
                m_rowCache[y].idBits1 = rowId.bits[1];
                m_rowCache[y].selStart = selStart;
                m_rowCache[y].selEnd = selEnd;
                m_rowCache[y].dpr = dpr;
                m_rowCache[y].valid = true;
            }
            ++y;
        }
    }

    const uint64_t prevPlacementsGen = m_lastPlacementsGen;
    refreshImagePlacements(forceAllDirty);
    const bool placementsChanged = (m_lastPlacementsGen != prevPlacementsGen);
    scheduleKittyAnimationTick(hasActiveAnim, animNowMs, nextAnimMs);

    ghostty_render_state_clean(m_renderState);
    if ((dirtyState != GHOSTTY_RENDER_STATE_DIRTY_FALSE || placementsChanged) &&
        (m_hoveredLink.isValid() || m_hoveredImage.isValid() || m_mouseInside)) {
        if (m_mouseInside) {
            updateHoveredLink(m_lastMousePos, QApplication::keyboardModifiers());
        } else {
            clearHoveredLink();
        }
    }
    refreshMouseCursor();
}

void TerminalWidget::renderRowToCache(int y, GhosttyRenderStateRowIterator rowIter,
                                      const GhosttyRenderStateColors &colors,
                                      int selStart, int selEnd, qreal dpr) {
    const int rowPxW = std::max(1, static_cast<int>(m_cols) * m_cellWidth);
    const int rowPxH = m_cellHeight;

    m_rowCache[y].bgRuns.clear();
    QPixmap &pm = m_rowCache[y].pixmap;
    const QSize targetSize(static_cast<int>(std::ceil(rowPxW * dpr)),
                           static_cast<int>(std::ceil(rowPxH * dpr)));
    if (pm.size() != targetSize || pm.devicePixelRatio() != dpr) {
        pm = QPixmap(targetSize);
        pm.setDevicePixelRatio(dpr);
    }

    const QColor defaultBg = GhosttyUnionTheme::toQColor(colors.background);
    const QColor selBg = GhosttyUnionTheme::toQColor(GhosttyUnionTheme::kOllieSelectionBg);
    const QColor selFg = GhosttyUnionTheme::toQColor(GhosttyUnionTheme::kOllieSelectionFg);

    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    if (ghostty_render_state_row_get(rowIter, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &m_cellsIter) != GHOSTTY_SUCCESS) {
        return;
    }

    struct CellDrawItem {
        int col = 0;
        int spanCols = 1;
        uint32_t cp = 0;
        bool hasCluster = false;
        GhosttyStyle style{};
        QColor fg;
        QString glyph;
    };

    std::vector<QColor> colBgs(m_cols, defaultBg);
    std::vector<CellDrawItem> fgItems;
    fgItems.reserve(m_cols);

    std::vector<char> utf8Heap;
    int col = 0;

    while (ghostty_render_state_row_cells_next(m_cellsIter) && col < m_cols) {
        GhosttyCell rawCell{};
        ghostty_render_state_row_cells_get(m_cellsIter, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &rawCell);

        GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
        ghostty_cell_get(rawCell, GHOSTTY_CELL_DATA_WIDE, &wide);
        if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
            ++col;
            continue;
        }

        bool hasStyling = false;
        ghostty_render_state_row_cells_get(m_cellsIter, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_HAS_STYLING, &hasStyling);

        GhosttyStyle style{};
        if (hasStyling) {
            ghostty_render_state_row_cells_get(m_cellsIter, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
        } else {
            ghostty_style_default(&style);
        }

        GhosttyColorRgb bgRgb{};
        const bool hasBg = ghostty_render_state_row_cells_get(
                               m_cellsIter, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bgRgb) == GHOSTTY_SUCCESS;
        GhosttyColorRgb fgRgb{};
        const bool hasFg = ghostty_render_state_row_cells_get(
                               m_cellsIter, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fgRgb) == GHOSTTY_SUCCESS;

        QColor cellBg = hasBg ? GhosttyUnionTheme::toQColor(bgRgb) : defaultBg;
        const bool faint = (style.flags & GHOSTTY_STYLE_FLAG_FAINT) != 0;
        const bool inverse = (style.flags & GHOSTTY_STYLE_FLAG_INVERSE) != 0;
        QColor cellFg = resolveCellColor(hasFg, fgRgb, colors.foreground, faint);
        if (inverse) {
            std::swap(cellBg, cellFg);
        }

        if (wide == GHOSTTY_CELL_WIDE_SPACER_HEAD) {
            const bool isSel = (selStart >= 0 && col >= selStart && col <= selEnd);
            colBgs[col] = isSel ? selBg : cellBg;
            ++col;
            continue;
        }

        const int spanCols = (wide == GHOSTTY_CELL_WIDE_WIDE) ? 2 : 1;
        for (int s = 0; s < spanCols && (col + s) < m_cols; ++s) {
            const bool isSelCol = (selStart >= 0 && (col + s) >= selStart && (col + s) <= selEnd);
            colBgs[col + s] = isSelCol ? selBg : cellBg;
        }
        if (selStart >= 0 && col >= selStart && col <= selEnd) {
            cellFg = selFg;
        }

        GhosttyCellContentTag contentTag = GHOSTTY_CELL_CONTENT_CODEPOINT;
        ghostty_cell_get(rawCell, GHOSTTY_CELL_DATA_CONTENT_TAG, &contentTag);

        uint32_t cp = 0;
        const bool hasCluster = (contentTag == GHOSTTY_CELL_CONTENT_CODEPOINT_GRAPHEME);
        if (contentTag == GHOSTTY_CELL_CONTENT_CODEPOINT || hasCluster) {
            ghostty_cell_get(rawCell, GHOSTTY_CELL_DATA_CODEPOINT, &cp);
        }

        // Suppress glyph & underline metadata on Kitty Graphics virtual placeholder cells (U+10EEEE).
        // Their foreground/underline RGB and combining diacritics encode (image_id, placement_id, row, col)
        // for GhosttyRenderStateImageIterator and must never be rasterized as visible text.
        if (cp == kKittyUnicodePlaceholder) {
            ++col;
            continue;
        }

        const bool hasDecorations = (GHOSTTY_STYLE_UNDERLINE(style) != GHOSTTY_SGR_UNDERLINE_NONE) ||
                                    (style.flags & GHOSTTY_STYLE_FLAG_STRIKETHROUGH) != 0 ||
                                    (style.flags & GHOSTTY_STYLE_FLAG_OVERLINE) != 0;
        const bool hasText = (style.flags & GHOSTTY_STYLE_FLAG_INVISIBLE) == 0 &&
                             (hasCluster || (contentTag == GHOSTTY_CELL_CONTENT_CODEPOINT && cp != 0U && cp != 0x20U));
        if (!hasText && !hasDecorations) {
            ++col;
            continue;
        }

        QString glyph;
        const bool procedural = !hasCluster && hasText &&
                                (isBlockOrShade(cp) || isBoxDrawing(cp) || isPowerlineSymbol(cp));
        if (hasText && !procedural) {
            if (!hasCluster) {
                const char32_t ucs4 = cp;
                glyph = QString::fromUcs4(&ucs4, 1);
            } else {
                char utf8Stack[128];
                GhosttyBuffer buf{reinterpret_cast<uint8_t *>(utf8Stack), sizeof(utf8Stack), 0};
                GhosttyResult cellResult = ghostty_render_state_row_cells_get(
                    m_cellsIter, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buf);
                if (cellResult == GHOSTTY_OUT_OF_SPACE && buf.len > 0) {
                    utf8Heap.resize(buf.len);
                    buf.ptr = reinterpret_cast<uint8_t *>(utf8Heap.data());
                    buf.cap = utf8Heap.size();
                    buf.len = 0;
                    cellResult = ghostty_render_state_row_cells_get(
                        m_cellsIter, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buf);
                }
                if (cellResult == GHOSTTY_SUCCESS && buf.len > 0) {
                    glyph = QString::fromUtf8(reinterpret_cast<const char *>(buf.ptr),
                                              static_cast<qsizetype>(buf.len));
                }
            }
        }

        CellDrawItem item;
        item.col = col;
        item.spanCols = spanCols;
        item.cp = cp;
        item.hasCluster = hasCluster;
        item.style = style;
        item.fg = cellFg;
        item.glyph = std::move(glyph);
        fgItems.push_back(std::move(item));

        ++col;
    }

    // Pass 1: record merged contiguous non-default background runs for 5-layer compositing in paintEvent()
    int c = 0;
    while (c < m_cols) {
        if (colBgs[c] == defaultBg) {
            ++c;
            continue;
        }
        const QColor &runBg = colBgs[c];
        int end = c + 1;
        while (end < m_cols && colBgs[end] == runBg) {
            ++end;
        }
        m_rowCache[y].bgRuns.push_back(BgRun{c, end, runBg});
        c = end;
    }

    // Pass 2: procedural box/shade/Powerline geometry, constrained Nerd Font PUA icons, text, decorations
    auto buildFittedPath = [&](const QFont &f, const QString &glyph, int cellSpanW) -> QPainterPath {
        QPainterPath raw;
        raw.addText(0.0, 0.0, f, glyph);
        const QRectF br = raw.boundingRect();
        if (br.isEmpty()) {
            return {};
        }
        const qreal maxW = std::max(1.0, static_cast<qreal>(cellSpanW) - 0.5);
        const qreal maxH = std::max(1.0, static_cast<qreal>(rowPxH) - 2.0);
        const qreal scale = std::min({1.0, maxW / br.width(), maxH / br.height()});
        const qreal scaledW = br.width() * scale;
        const qreal scaledH = br.height() * scale;
        const qreal targetX = (cellSpanW - scaledW) * 0.5 - br.left() * scale;
        const qreal visualMidY = std::clamp(static_cast<qreal>(m_ascent) - m_capHeight * 0.5,
                                            rowPxH * 0.35, rowPxH * 0.65);
        const qreal clampedTop = std::clamp(visualMidY - scaledH * 0.5,
                                            0.5,
                                            std::max(0.5, rowPxH - scaledH - 0.5));
        const qreal targetY = clampedTop - br.top() * scale;

        QTransform xf;
        xf.translate(targetX, targetY);
        xf.scale(scale, scale);
        return xf.map(raw);
    };

    for (const CellDrawItem &item : fgItems) {
        const int cellX = item.col * m_cellWidth;
        const int cellSpanW = item.spanCols * m_cellWidth;
        const bool itemInvisible = (item.style.flags & GHOSTTY_STYLE_FLAG_INVISIBLE) != 0;
        const bool itemBold = (item.style.flags & GHOSTTY_STYLE_FLAG_BOLD) != 0;
        const bool itemItalic = (item.style.flags & GHOSTTY_STYLE_FLAG_ITALIC) != 0;

        if (!itemInvisible && item.cp != 0U && item.cp != 0x20U) {
            if (!item.hasCluster && isBlockOrShade(item.cp)) {
                drawBlockElement(p, item.cp, cellX, cellSpanW, rowPxH, item.fg);
            } else if (!item.hasCluster && isBoxDrawing(item.cp) &&
                       drawBoxDrawing(p, item.cp, cellX, cellSpanW, rowPxH, item.fg)) {
                // Handled procedurally
            } else if (!item.hasCluster && isPowerlineSymbol(item.cp)) {
                drawPowerlineSymbol(p, item.cp, cellX, cellSpanW, rowPxH, item.fg);
            } else if (!item.glyph.isEmpty()) {
                const QFont &f = (itemBold && itemItalic)
                                     ? m_boldItalicFont
                                     : (itemBold ? m_boldFont : (itemItalic ? m_italicFont : m_font));
                const QFontMetricsF fm(f);
                const qreal adv = fm.horizontalAdvance(item.glyph);

                bool useFittedPath = isNerdFontPua(item.cp);
                if (!useFittedPath && item.cp >= 0x2000U) {
                    const QRectF br = fm.boundingRect(item.glyph);
                    const qreal shift = (cellSpanW - adv) * 0.5;
                    const qreal leftEdge = br.left() + shift;
                    const qreal rightEdge = br.right() + shift;
                    useFittedPath = (br.width() > cellSpanW - 0.5) ||
                                    (leftEdge < -0.5) ||
                                    (rightEdge > cellSpanW + 0.5) ||
                                    (br.height() > rowPxH - 1.0);
                }

                if (useFittedPath) {
                    QPainterPath norm;
                    if (!item.hasCluster) {
                        const uint64_t key = (static_cast<uint64_t>(item.cp) << 4) |
                                             (static_cast<uint64_t>(item.spanCols & 0x3) << 2) |
                                             (itemBold ? 2ULL : 0ULL) |
                                             (itemItalic ? 1ULL : 0ULL);
                        auto it = m_puaPathCache.constFind(key);
                        if (it != m_puaPathCache.constEnd()) {
                            norm = it.value();
                        } else {
                            norm = buildFittedPath(f, item.glyph, cellSpanW);
                            m_puaPathCache.insert(key, norm);
                        }
                    } else {
                        norm = buildFittedPath(f, item.glyph, cellSpanW);
                    }
                    if (!norm.isEmpty()) {
                        p.setRenderHint(QPainter::Antialiasing, true);
                        p.translate(cellX, 0.0);
                        p.fillPath(norm, item.fg);
                        p.translate(-cellX, 0.0);
                    }
                } else {
                    p.setFont(f);
                    p.setPen(item.fg);
                    const qreal gx = cellX + (cellSpanW - adv) * 0.5;
                    p.drawText(QPointF(gx, m_ascent), item.glyph);
                }
            }
        }

        p.setRenderHint(QPainter::Antialiasing, false);
        const int itemUnderline = GHOSTTY_STYLE_UNDERLINE(item.style);
        if (itemUnderline != GHOSTTY_SGR_UNDERLINE_NONE) {
            const QColor uCol = resolveStyleColor(item.style.underline_color, colors.palette, item.fg);
            drawUnderlineStyle(p, cellX, m_ascent, cellSpanW, rowPxH, itemUnderline, uCol);
        }
        if ((item.style.flags & GHOSTTY_STYLE_FLAG_STRIKETHROUGH) != 0) {
            p.setPen(item.fg);
            const int midY = rowPxH / 2;
            p.drawLine(cellX, midY, cellX + cellSpanW - 1, midY);
        }
        if ((item.style.flags & GHOSTTY_STYLE_FLAG_OVERLINE) != 0) {
            p.setPen(item.fg);
            p.drawLine(cellX, 1, cellX + cellSpanW - 1, 1);
        }
    }
}

void TerminalWidget::paintEvent(QPaintEvent * /*event*/) {
    QPainter p(this);
    const QColor bg = GhosttyUnionTheme::toQColor(m_colors.background);
    p.fillRect(rect(), bg);

    // Layer 1: Kitty images below cell backgrounds (z < -1,073,741,824)
    drawImagePlacementsLayer(p, GHOSTTY_IMAGE_LAYER_BELOW_BACKGROUND);

    // Layer 2: Non-default cell backgrounds & selection spans
    p.setRenderHint(QPainter::Antialiasing, false);
    for (int y = 0; y < m_rowCache.size(); ++y) {
        if (!m_rowCache[y].valid || m_rowCache[y].bgRuns.empty()) {
            continue;
        }
        const int rowY = kPadY + y * m_cellHeight;
        for (const BgRun &run : m_rowCache[y].bgRuns) {
            p.fillRect(kPadX + run.startCol * m_cellWidth,
                       rowY,
                       (run.endCol - run.startCol) * m_cellWidth,
                       m_cellHeight,
                       run.color);
        }
    }

    // Layer 3: Kitty images above cell backgrounds & below text (-1,073,741,824 <= z <= -1, incl. U+10EEEE virtual placements)
    drawImagePlacementsLayer(p, GHOSTTY_IMAGE_LAYER_BELOW_TEXT);

    // Layer 4: Foreground text, procedural box/block/Powerline, Nerd Font icons, underlines
    const qreal dpr = std::max(1.0, devicePixelRatioF());
    const bool exactRowHeight = std::abs(m_cellHeight * dpr - std::round(m_cellHeight * dpr)) < 0.01;
    for (int y = 0; y < m_rowCache.size(); ++y) {
        if (m_rowCache[y].valid) {
            if (exactRowHeight) {
                p.drawPixmap(kPadX, kPadY + y * m_cellHeight, m_rowCache[y].pixmap);
            } else {
                p.drawPixmap(QRect(kPadX, kPadY + y * m_cellHeight, m_cols * m_cellWidth, m_cellHeight),
                             m_rowCache[y].pixmap);
            }
        }
    }

    // Layer 5: Kitty images above text (z >= 0)
    drawImagePlacementsLayer(p, GHOSTTY_IMAGE_LAYER_ABOVE_TEXT);

    // Ctrl+Hover link underline overlay (or Solar Forge inline image highlight frame)
    if (m_hoveredLink.isValid()) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(QColor(GhosttyUnionTheme::kCyanHex), 1.0));
        const int underlineOffset = std::min(m_ascent + 2, m_cellHeight - 2);
        for (const auto &span : m_hoveredLink.spans) {
            if (span.row >= m_rows) {
                continue;
            }
            const int ux0 = kPadX + static_cast<int>(span.startCol) * m_cellWidth;
            const int ux1 = kPadX + static_cast<int>(span.endCol + 1) * m_cellWidth - 1;
            const int uy = kPadY + static_cast<int>(span.row) * m_cellHeight + underlineOffset;
            p.drawLine(ux0, uy, ux1, uy);
        }
        p.restore();
    } else if (m_hoveredImage.isValid()) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF frameRect = m_hoveredImage.unionRect.adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(QColor(GhosttyUnionTheme::kGoldHex), 1.4));
        p.setBrush(QColor(255, 211, 0, 18));
        p.drawRoundedRect(frameRect, 4.0, 4.0);
        p.restore();
    }

    // Draw cursor over rendered rows (blink applies only when focused)
    const bool showCursor = m_cursor.visible && m_cursor.viewport_has_value &&
                            (!hasFocus() || !m_cursor.blinking || m_blinkVisible);
    if (showCursor) {
        const int cx = kPadX + static_cast<int>(m_cursor.viewport_x) * m_cellWidth;
        const int cy = kPadY + static_cast<int>(m_cursor.viewport_y) * m_cellHeight;
        const QColor curCol = m_colors.cursor_has_value
                                  ? GhosttyUnionTheme::toQColor(m_colors.cursor)
                                  : GhosttyUnionTheme::toQColor(GhosttyUnionTheme::kOllieCursor);

        if (!hasFocus() || m_cursor.visual_style == GHOSTTY_CURSOR_STYLE_BLOCK_HOLLOW) {
            p.setPen(curCol);
            p.setBrush(Qt::NoBrush);
            p.drawRect(cx, cy, m_cellWidth - 1, m_cellHeight - 1);
        } else if (m_cursor.visual_style == GHOSTTY_CURSOR_STYLE_BAR) {
            p.fillRect(cx, cy, 2, m_cellHeight, curCol);
        } else if (m_cursor.visual_style == GHOSTTY_CURSOR_STYLE_UNDERLINE) {
            p.fillRect(cx, cy + m_cellHeight - 2, m_cellWidth, 2, curCol);
        } else {
            QColor blockCol = curCol;
            blockCol.setAlpha(185);
            p.fillRect(cx, cy, m_cellWidth, m_cellHeight, blockCol);
        }
    }

    // Ghostty `unfocused-split-opacity = 0.88` wash on inactive split panes
    if (m_unfocusedSplitDimmed) {
        p.fillRect(rect(), GhosttyUnionTheme::withAlpha(m_colors.background, 31));
    }

    // Subtle Solar Forge top luminous focus ribbon on the active pane when 2+ splits are visible
    if (m_activeSplitAccent && !m_unfocusedSplitDimmed) {
        QLinearGradient activeBar(0.0, 0.0, static_cast<qreal>(width()), 0.0);
        activeBar.setColorAt(0.0, QColor(255, 211, 0, 20));
        activeBar.setColorAt(0.3, QColor(255, 211, 0, 190));
        activeBar.setColorAt(0.7, QColor(255, 152, 0, 165));
        activeBar.setColorAt(1.0, QColor(255, 61, 0, 20));
        p.fillRect(0, 0, width(), 2, activeBar);
    }

    // Animated Solar Forge interactive scrollbar gutter when scrollback exists
    QRectF trackRect;
    QRectF thumbRect;
    if (computeScrollbarGeometry(trackRect, thumbRect)) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        const qreal hoverT = m_scrollbarDragging ? 1.0 : m_scrollbarHoverProgress;
        if (hoverT > 0.02) {
            p.setBrush(QColor(255, 255, 255, static_cast<int>(24.0 * hoverT)));
            p.drawRoundedRect(trackRect, 4.0, 4.0);

            // Soft Solar Forge halo behind active thumb
            QColor haloCol(GhosttyUnionTheme::kGoldHex);
            haloCol.setAlpha(static_cast<int>((m_scrollbarDragging ? 64.0 : 42.0) * hoverT));
            p.setBrush(haloCol);
            const QRectF haloRect = thumbRect.adjusted(-1.5, -1.0, 1.5, 1.0);
            p.drawRoundedRect(haloRect, haloRect.width() * 0.5, haloRect.width() * 0.5);
        }

        const int alpha = m_scrollbarDragging
                              ? 248
                              : static_cast<int>(125.0 + 100.0 * hoverT);
        QLinearGradient thumbGrad(thumbRect.topLeft(), thumbRect.bottomLeft());
        QColor topC(GhosttyUnionTheme::kGoldHex);
        topC.setAlpha(alpha);
        QColor botC(GhosttyUnionTheme::kAmberHex);
        botC.setAlpha(alpha);
        thumbGrad.setColorAt(0.0, topC);
        thumbGrad.setColorAt(1.0, botC);
        p.setBrush(thumbGrad);
        const qreal radius = thumbRect.width() * 0.5;
        p.drawRoundedRect(thumbRect, radius, radius);
        p.restore();
    }

    // Solar Forge glassmorphic `link-previews = true` pill overlay (links & inline images)
    if (m_hoveredLink.isValid() || m_hoveredImage.isValid()) {
        const bool isImgPill = !m_hoveredLink.isValid();
        const QString rawPreview = isImgPill
                                       ? QStringLiteral("Image #%1 · %2×%3 px · Ctrl+Click to inspect")
                                             .arg(m_hoveredImage.imageId)
                                             .arg(m_hoveredImage.sourcePixelWidth)
                                             .arg(m_hoveredImage.sourcePixelHeight)
                                       : m_hoveredLink.previewText();
        if (!rawPreview.isEmpty()) {
            p.save();
            QFont uiFont = QApplication::font();
            uiFont.setPointSizeF(std::max(8.5, uiFont.pointSizeF() - 0.5));
            p.setFont(uiFont);
            const QFontMetrics fm(uiFont);

            const int maxPillW = std::max(80, width() - 28);
            constexpr int hPad = 10;
            constexpr int vPad = 5;
            const QString badgePrefix = isImgPill ? QStringLiteral("◈ ") : QStringLiteral("↗ ");
            const int prefixW = fm.horizontalAdvance(badgePrefix);
            const QString elided = fm.elidedText(rawPreview, Qt::ElideMiddle,
                                                 std::max(24, maxPillW - 2 * hPad - prefixW));
            const int pillW = std::min(maxPillW, prefixW + fm.horizontalAdvance(elided) + 2 * hPad);
            const int pillH = fm.height() + 2 * vPad;

            bool bottomBlocked = false;
            if (isImgPill) {
                bottomBlocked = m_hoveredImage.unionRect.bottom() >= static_cast<qreal>(height() - pillH - 16);
            } else {
                const uint16_t bottomThreshold = (m_rows >= 2) ? static_cast<uint16_t>(m_rows - 2) : 0;
                for (const auto &span : m_hoveredLink.spans) {
                    if (span.row >= bottomThreshold) {
                        bottomBlocked = true;
                        break;
                    }
                }
            }
            const int pillY = bottomBlocked ? 8 : std::max(8, height() - pillH - 8);
            const QRectF pillRect(8.0, static_cast<qreal>(pillY),
                                  static_cast<qreal>(pillW), static_cast<qreal>(pillH));

            p.setRenderHint(QPainter::Antialiasing, true);
            QLinearGradient pillBg(pillRect.topLeft(), pillRect.bottomLeft());
            pillBg.setColorAt(0.0, QColor(0x2A, 0x28, 0x33, 245));
            pillBg.setColorAt(1.0, QColor(0x1B, 0x1A, 0x21, 248));
            p.setPen(QPen(QColor(255, 211, 0, 105), 1.0));
            p.setBrush(pillBg);
            p.drawRoundedRect(pillRect, 7.0, 7.0);

            p.setPen(QColor(GhosttyUnionTheme::kGoldHex));
            p.drawText(QRectF(pillRect.left() + hPad, pillRect.top(), prefixW, pillRect.height()),
                       Qt::AlignVCenter | Qt::AlignLeft, badgePrefix);

            p.setPen(QColor(isImgPill ? GhosttyUnionTheme::kAmberHex : GhosttyUnionTheme::kCyanHex));
            p.drawText(pillRect.adjusted(hPad + prefixW, 0, -hPad, 0), Qt::AlignVCenter | Qt::AlignLeft, elided);
            p.restore();
        }
    }

    // Animated Solar Forge borderless hover drag grip pill when 2+ splits are visible
    if (m_splitRearrangeable && (m_splitDragging || m_splitGripProgress > 0.01)) {
        const QRectF grip = splitGripRect();
        const qreal t = m_splitDragging ? 1.0 : m_splitGripProgress;
        const bool activeGrip = m_splitDragging || m_splitGripHovered;
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setOpacity(std::clamp(t, 0.0, 1.0));

        QLinearGradient gripBg(grip.topLeft(), grip.bottomLeft());
        gripBg.setColorAt(0.0, QColor(0x31, 0x2F, 0x3B, activeGrip ? 248 : 225));
        gripBg.setColorAt(1.0, QColor(0x21, 0x20, 0x27, activeGrip ? 248 : 225));
        const QColor borderCol = activeGrip ? QColor(GhosttyUnionTheme::kGoldHex)
                                            : QColor(GhosttyUnionTheme::kChromeBorderHex);
        p.setPen(QPen(borderCol, 1.0));
        p.setBrush(gripBg);
        p.drawRoundedRect(grip, 4.0, 4.0);

        const QColor dotCol(activeGrip ? GhosttyUnionTheme::kGoldHex : GhosttyUnionTheme::kDimTextHex);
        p.setPen(Qt::NoPen);
        p.setBrush(dotCol);
        const qreal cy = grip.center().y();
        const qreal cx = grip.center().x();
        for (int i = -1; i <= 1; ++i) {
            p.drawEllipse(QPointF(cx + i * 6.0, cy), activeGrip ? 1.3 : 1.1, activeGrip ? 1.3 : 1.1);
        }
        p.restore();
    }
}

void TerminalWidget::keyPressEvent(QKeyEvent *event) {
    if (m_splitDragging && event->key() == Qt::Key_Escape) {
        m_splitDragArmed = false;
        m_splitDragging = false;
        refreshMouseCursor();
        update();
        Q_EMIT splitDragCancelled(this);
        event->accept();
        return;
    }

    // Font zoom, prompt jump & copy/paste shortcuts
    const auto baseMods = event->modifiers() & ~Qt::KeypadModifier;
    if (baseMods == (Qt::ControlModifier | Qt::ShiftModifier)) {
        if (event->key() == Qt::Key_C) {
            copySelection();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_V) {
            pasteClipboard(false);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Plus) {
            zoomIn();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_K) {
            softResetTerminalModes();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
            if (m_terminal) {
                const GhosttyTerminalScrollViewport sb{
                    .tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_DELTA_PROMPT,
                    .value = {.delta_prompt = (event->key() == Qt::Key_Up) ? -1 : 1},
                };
                ghostty_terminal_scroll_viewport(m_terminal, sb);
                updateRenderState(false);
                update();
            }
            event->accept();
            return;
        }
    } else if (baseMods == Qt::ControlModifier) {
        if (event->key() == Qt::Key_Equal || event->key() == Qt::Key_Plus) {
            zoomIn();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Minus) {
            zoomOut();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_0) {
            zoomReset();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Backspace) {
            ensureShellSpawned();
            constexpr uint8_t wordErase = 0x17;
            flushPtyBytes(&wordErase, 1);
            event->accept();
            return;
        }
    } else if (baseMods == Qt::ShiftModifier) {
        if (event->key() == Qt::Key_PageUp) {
            const GhosttyTerminalScrollViewport sb{
                .tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_DELTA,
                .value = {.delta = -static_cast<intptr_t>(std::max<uint16_t>(1, m_rows / 2))},
            };
            ghostty_terminal_scroll_viewport(m_terminal, sb);
            updateRenderState(false);
            update();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_PageDown) {
            const GhosttyTerminalScrollViewport sb{
                .tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_DELTA,
                .value = {.delta = static_cast<intptr_t>(std::max<uint16_t>(1, m_rows / 2))},
            };
            ghostty_terminal_scroll_viewport(m_terminal, sb);
            updateRenderState(false);
            update();
            event->accept();
            return;
        }
    }

    if (m_mouseInside) {
        Qt::KeyboardModifiers hoverMods = event->modifiers();
        if (event->key() == Qt::Key_Control) {
            hoverMods |= Qt::ControlModifier;
        } else if (event->key() == Qt::Key_Shift) {
            hoverMods |= Qt::ShiftModifier;
        } else if (event->key() == Qt::Key_Alt) {
            hoverMods |= Qt::AltModifier;
        } else if (event->key() == Qt::Key_Meta) {
            hoverMods |= Qt::MetaModifier;
        }
        updateHoveredLink(m_lastMousePos, hoverMods);
    }

    sendKeyToPty(event, event->isAutoRepeat() ? GHOSTTY_KEY_ACTION_REPEAT : GHOSTTY_KEY_ACTION_PRESS);
    event->accept();
}

void TerminalWidget::keyReleaseEvent(QKeyEvent *event) {
    if (m_mouseInside) {
        Qt::KeyboardModifiers hoverMods = event->modifiers();
        if (!event->isAutoRepeat()) {
            if (event->key() == Qt::Key_Control) {
                hoverMods &= ~Qt::ControlModifier;
            } else if (event->key() == Qt::Key_Shift) {
                hoverMods &= ~Qt::ShiftModifier;
            } else if (event->key() == Qt::Key_Alt) {
                hoverMods &= ~Qt::AltModifier;
            } else if (event->key() == Qt::Key_Meta) {
                hoverMods &= ~Qt::MetaModifier;
            }
        }
        updateHoveredLink(m_lastMousePos, hoverMods);
    }
    sendKeyToPty(event, GHOSTTY_KEY_ACTION_RELEASE);
    event->accept();
}

void TerminalWidget::sendKeyToPty(const QKeyEvent *event, GhosttyKeyAction action) {
    ensureShellSpawned();
    if (!m_terminal || !m_keyEncoder || !m_keyEvent || (m_ptyMasterFd < 0 && !m_ptyOutputSpy)) {
        return;
    }

    // Snap viewport back to active area on key input (except pure modifiers)
    const GhosttyKey ghosttyKey = GhosttyUnionKeyMap::mapQtKey(event);
    if (action != GHOSTTY_KEY_ACTION_RELEASE && !GhosttyUnionKeyMap::isModifierKey(ghosttyKey)) {
        bool activeVp = true;
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE, &activeVp);
        if (!activeVp) {
            constexpr GhosttyTerminalScrollViewport bottom{.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_BOTTOM, .value = {}};
            ghostty_terminal_scroll_viewport(m_terminal, bottom);
            updateRenderState(false);
            update();
        }
    }

    ghostty_key_encoder_set_from_terminal(m_keyEncoder, m_terminal);

    QByteArray utf8Storage;
    GhosttyUnionKeyMap::populateGhosttyKeyEvent(m_keyEvent, event, action, utf8Storage);

    uint8_t outBuf[256];
    size_t written = 0;
    const GhosttyResult res = ghostty_key_encoder_encode(m_keyEncoder, m_keyEvent, outBuf, sizeof(outBuf), &written);
    if (res == GHOSTTY_SUCCESS && written > 0) {
        flushPtyBytes(outBuf, written);
    }
}

bool TerminalWidget::cellPointAt(const QPointF &pos, uint16_t &colOut, uint16_t &rowOut) const {
    if (m_cols == 0 || m_rows == 0) {
        return false;
    }
    const int xPx = static_cast<int>(pos.x()) - kPadX;
    const int yPx = static_cast<int>(pos.y()) - kPadY;
    colOut = static_cast<uint16_t>(std::clamp(xPx / m_cellWidth, 0, m_cols - 1));
    rowOut = static_cast<uint16_t>(std::clamp(yPx / m_cellHeight, 0, m_rows - 1));
    return true;
}

bool TerminalWidget::strictCellAt(const QPointF &pos, uint16_t &colOut, uint16_t &rowOut) const {
    if (m_cols == 0 || m_rows == 0 || m_cellWidth <= 0 || m_cellHeight <= 0) {
        return false;
    }
    const int xPx = static_cast<int>(std::floor(pos.x())) - kPadX;
    const int yPx = static_cast<int>(std::floor(pos.y())) - kPadY;
    if (xPx < 0 || yPx < 0 ||
        xPx >= static_cast<int>(m_cols) * m_cellWidth ||
        yPx >= static_cast<int>(m_rows) * m_cellHeight) {
        return false;
    }
    colOut = static_cast<uint16_t>(xPx / m_cellWidth);
    rowOut = static_cast<uint16_t>(yPx / m_cellHeight);
    return true;
}

bool TerminalWidget::hasScrollbar() const {
    return m_terminal != nullptr && m_scrollbar.total > m_scrollbar.len &&
           m_scrollbar.total > 0 && height() > 2 * kScrollbarMarginY;
}

bool TerminalWidget::isInScrollbarZone(const QPointF &pos) const {
    return hasScrollbar() &&
           pos.x() >= static_cast<qreal>(width() - kScrollbarHitWidth) &&
           pos.x() <= static_cast<qreal>(width()) &&
           pos.y() >= 0.0 &&
           pos.y() <= static_cast<qreal>(height());
}

QRectF TerminalWidget::splitGripRect() const {
    return {(static_cast<qreal>(width()) - 44.0) * 0.5, 3.0, 44.0, 7.0};
}

bool TerminalWidget::isInSplitGripZone(const QPointF &pos) const {
    if (!m_splitRearrangeable || width() < 96) {
        return false;
    }
    constexpr qreal half = 28.0;
    const qreal mid = static_cast<qreal>(width()) * 0.5;
    return pos.x() >= (mid - half) && pos.x() <= (mid + half) &&
           pos.y() >= 0.0 && pos.y() <= 14.0;
}

bool TerminalWidget::isNearSplitGripZone(const QPointF &pos) const {
    if (!m_splitRearrangeable || !m_mouseInside || width() < 96) {
        return false;
    }
    constexpr qreal half = 80.0;
    const qreal mid = static_cast<qreal>(width()) * 0.5;
    return pos.x() >= (mid - half) && pos.x() <= (mid + half) &&
           pos.y() >= 0.0 && pos.y() <= 32.0;
}

bool TerminalWidget::computeScrollbarGeometry(QRectF &trackRectOut, QRectF &thumbRectOut) const {
    if (!hasScrollbar()) {
        return false;
    }
    constexpr auto trackTop = static_cast<qreal>(kScrollbarMarginY);
    const qreal trackH = std::max(1.0, static_cast<qreal>(height() - 2 * kScrollbarMarginY));
    const uint64_t maxOffset = m_scrollbar.total - m_scrollbar.len;
    const qreal ratioLen = static_cast<qreal>(m_scrollbar.len) / static_cast<qreal>(m_scrollbar.total);
    const qreal minThumbH = std::min(trackH, 24.0);
    const qreal thumbH = std::clamp(trackH * ratioLen, minThumbH, trackH);
    const qreal travel = std::max(0.0, trackH - thumbH);
    const qreal frac = (maxOffset > 0)
                           ? std::clamp(static_cast<qreal>(m_scrollbar.offset) / static_cast<qreal>(maxOffset),
                                        0.0, 1.0)
                           : 0.0;
    const qreal thumbY = trackTop + travel * frac;

    const qreal hoverT = m_scrollbarDragging ? 1.0 : m_scrollbarHoverProgress;
    const qreal barW = 4.0 + 4.0 * hoverT;
    const qreal rightInset = 3.0 - 1.0 * hoverT;
    const qreal barX = static_cast<qreal>(width()) - rightInset - barW;

    trackRectOut = QRectF(static_cast<qreal>(width()) - 10.0, trackTop, 8.0, trackH);
    thumbRectOut = QRectF(barX, thumbY, barW, thumbH);
    return true;
}

void TerminalWidget::scrollViewportToThumbTop(qreal desiredThumbTop) {
    QRectF trackRect;
    QRectF thumbRect;
    if (!computeScrollbarGeometry(trackRect, thumbRect)) {
        return;
    }
    const uint64_t maxOffset = m_scrollbar.total - m_scrollbar.len;
    if (maxOffset == 0) {
        return;
    }

    const qreal travel = std::max(0.0, trackRect.height() - thumbRect.height());
    const uint64_t targetRow = [&]() -> uint64_t {
        if (travel <= 0.5) {
            return (desiredThumbTop <= trackRect.top()) ? 0 : maxOffset;
        }
        const qreal frac = std::clamp((desiredThumbTop - trackRect.top()) / travel, 0.0, 1.0);
        return static_cast<uint64_t>(std::llround(frac * static_cast<qreal>(maxOffset)));
    }();
    if (targetRow == m_scrollbar.offset) {
        return;
    }

    const GhosttyTerminalScrollViewport sb = [&]() {
        if (targetRow == 0) {
            return GhosttyTerminalScrollViewport{.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_TOP, .value = {}};
        }
        if (targetRow >= maxOffset) {
            return GhosttyTerminalScrollViewport{.tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_BOTTOM, .value = {}};
        }
        return GhosttyTerminalScrollViewport{
            .tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_ROW,
            .value = {.row = static_cast<size_t>(targetRow)},
        };
    }();

    ghostty_terminal_scroll_viewport(m_terminal, sb);
    updateRenderState(false);
    update();
}

void TerminalWidget::animateScrollbarHover(qreal target) {
    if (!m_scrollbarHoverAnim) {
        m_scrollbarHoverProgress = target;
        update();
        return;
    }
    m_scrollbarHoverAnim->stop();
    m_scrollbarHoverAnim->setStartValue(m_scrollbarHoverProgress);
    m_scrollbarHoverAnim->setEndValue(target);
    m_scrollbarHoverAnim->start();
}

void TerminalWidget::animateSplitGrip(qreal target) {
    if (!m_splitGripAnim) {
        m_splitGripProgress = target;
        update();
        return;
    }
    m_splitGripAnim->stop();
    m_splitGripAnim->setStartValue(m_splitGripProgress);
    m_splitGripAnim->setEndValue(target);
    m_splitGripAnim->start();
}

void TerminalWidget::updateScrollbarHoverState(const QPointF &pos) {
    const bool hovered = isInScrollbarZone(pos);
    if (hovered != m_scrollbarHovered) {
        m_scrollbarHovered = hovered;
        animateScrollbarHover((m_scrollbarHovered || m_scrollbarDragging) ? 1.0 : 0.0);
    }
    refreshMouseCursor();
}

void TerminalWidget::clearHoveredLink() {
    if (m_hoveredLink.isValid() || m_hoveredImage.isValid()) {
        m_hoveredLink = {};
        m_hoveredImage = {};
        refreshMouseCursor();
        update();
    }
}

void TerminalWidget::updateHoveredLink(const QPointF &pos, Qt::KeyboardModifiers mods) {
    const auto nonKeypadMods = mods & ~Qt::KeypadModifier;
    const bool ctrlOnly = (nonKeypadMods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) == Qt::ControlModifier;

    GhosttyUnionLink::ResolvedLink resolved;
    HoveredImageInfo resolvedImg;
    uint16_t col = 0;
    uint16_t row = 0;
    if (m_terminal && m_mouseInside && ctrlOnly && !m_selecting && !m_scrollbarDragging &&
        !isInScrollbarZone(pos)) {
        if (strictCellAt(pos, col, row)) {
            resolved = GhosttyUnionLink::resolveAt(
                m_terminal, m_cols, m_rows, col, row, m_pwd, /*allowBareCwdFiles=*/true);
        }
        if (!resolved.isValid()) {
            resolvedImg = resolveImageAt(pos);
        }
    }

    bool linkChanged = (resolved.isValid() != m_hoveredLink.isValid());
    if (!linkChanged && resolved.isValid()) {
        linkChanged = !resolved.sameDestination(m_hoveredLink) ||
                      resolved.spans.size() != m_hoveredLink.spans.size() ||
                      (!resolved.spans.isEmpty() && !m_hoveredLink.spans.isEmpty() &&
                       (resolved.spans.first().row != m_hoveredLink.spans.first().row ||
                        resolved.spans.first().startCol != m_hoveredLink.spans.first().startCol));
    }
    const bool imgChanged = (resolvedImg.isValid() != m_hoveredImage.isValid()) ||
                            (resolvedImg.isValid() && !resolvedImg.sameTarget(m_hoveredImage));
    if (linkChanged || imgChanged) {
        m_hoveredLink = std::move(resolved);
        m_hoveredImage = resolvedImg;
        refreshMouseCursor();
        update();
    }
}

void TerminalWidget::refreshMouseCursor() {
    Qt::CursorShape desired = Qt::IBeamCursor;
    if (m_splitDragging) {
        desired = Qt::ClosedHandCursor;
    } else if (m_splitGripHovered) {
        desired = Qt::OpenHandCursor;
    } else if (m_scrollbarDragging || m_scrollbarHovered) {
        desired = Qt::ArrowCursor;
    } else if (m_hoveredLink.isValid() || m_hoveredImage.isValid()) {
        desired = Qt::PointingHandCursor;
    } else if (m_terminal) {
        GhosttyMouseShape shape = GHOSTTY_MOUSE_SHAPE_TEXT;
        if (ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MOUSE_SHAPE, &shape) == GHOSTTY_SUCCESS) {
            desired = mapGhosttyMouseShape(shape);
        }
    }
    if (cursor().shape() != desired) {
        setCursor(desired);
    }
}

void TerminalWidget::activateLink(const GhosttyUnionLink::ResolvedLink &link) const {
    if (!link.isValid()) {
        return;
    }
    if (m_linkOpenHandler) {
        static_cast<void>(m_linkOpenHandler(link));
        return;
    }
    static_cast<void>(GhosttyUnionLink::openLink(link));
}

void TerminalWidget::setUnfocusedSplitDimmed(bool dimmed) {
    if (m_unfocusedSplitDimmed != dimmed) {
        m_unfocusedSplitDimmed = dimmed;
        update();
    }
}

void TerminalWidget::setActiveSplitAccent(bool active) {
    if (m_activeSplitAccent != active) {
        m_activeSplitAccent = active;
        update();
    }
}

void TerminalWidget::setSplitRearrangeable(bool rearrangeable) {
    if (m_splitRearrangeable == rearrangeable) {
        return;
    }
    m_splitRearrangeable = rearrangeable;
    if (!m_splitRearrangeable) {
        m_splitGripHovered = false;
        m_splitGripNear = false;
        m_splitDragArmed = false;
        m_splitDragging = false;
        animateSplitGrip(0.0);
        refreshMouseCursor();
    } else if (m_mouseInside) {
        m_splitGripHovered = isInSplitGripZone(m_lastMousePos);
        m_splitGripNear = isNearSplitGripZone(m_lastMousePos);
        animateSplitGrip((m_splitGripHovered || m_splitGripNear) ? 1.0 : 0.0);
        refreshMouseCursor();
    }
    update();
}

void TerminalWidget::setLinkOpenHandlerForTesting(LinkOpenHandler handler) {
    m_linkOpenHandler = std::move(handler);
}

void TerminalWidget::setPtyOutputSpyForTesting(std::function<void(const QByteArray &)> spy) {
    m_ptyOutputSpy = std::move(spy);
}

void TerminalWidget::softResetTerminalModes() {
    if (!m_terminal) {
        return;
    }
    bool atGround = true;
    if (ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_VT_GROUND, &atGround) == GHOSTTY_SUCCESS && !atGround) {
        constexpr uint8_t canByte = 0x18;
        ghostty_terminal_vt_write(m_terminal, &canByte, 1);
    }
    m_pendingCommandEndSanitize = false;
    m_oscCaptureState = OscCaptureState::Idle;
    m_osc1337Buffer.clear();
    m_selecting = false;
    applySoftModeSanitization();
    updateRenderState(true);
    update();
}

void TerminalWidget::resetTerminal() {
    if (!m_terminal) {
        return;
    }
    m_hoveredLink = {};
    m_pressedLink = {};
    m_hoveredImage = {};
    m_pressedImage = {};
    m_linkClickPending = false;
    m_imageClickPending = false;
    m_selecting = false;
    m_renderHeld = false;
    m_pendingCommandEndSanitize = false;
    m_oscCaptureState = OscCaptureState::Idle;
    m_osc1337Buffer.clear();
    m_pendingOsc1337Seqs.clear();
    m_visiblePlacements.clear();
    m_textureCache.clear();
    m_lastPlacementsGen = 0;
    m_lastKittyStorageGen = 0;
    m_placementsInitialized = false;
    m_kittyAnimTimer.stop();
    m_renderHoldTimer.stop();
    ghostty_terminal_reset(m_terminal);
    updateRenderState(true);
    update();
}

void TerminalWidget::showContextMenu(const QPointF &localPos, const QPoint &globalPos) {
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("terminalContextMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    GhosttyUnionTheme::styleSolarForgeMenu(menu);

    const QColor goldCol(GhosttyUnionTheme::kGoldHex);
    const QColor cyanCol(GhosttyUnionTheme::kCyanHex);
    const QColor amberCol(GhosttyUnionTheme::kAmberHex);
    const QColor redCol(GhosttyUnionTheme::kRedOrangeHex);

    const HoveredImageInfo ctxImg = resolveImageAt(localPos);
    if (ctxImg.isValid()) {
        const uint32_t imgId = ctxImg.imageId;
        QAction *inspectImgAct = menu->addAction(
            GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::InspectImage, goldCol),
            QStringLiteral("Inspect Image (%1×%2)…\tCtrl+Click")
                .arg(ctxImg.sourcePixelWidth)
                .arg(ctxImg.sourcePixelHeight));
        connect(inspectImgAct, &QAction::triggered, this, [this, imgId]() {
            inspectKittyImage(imgId);
        });

        QAction *copyImgAct = menu->addAction(
            GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::CopyImage, cyanCol),
            QStringLiteral("Copy Image"));
        connect(copyImgAct, &QAction::triggered, this, [this, imgId]() {
            copyKittyImageToClipboard(imgId);
        });

        QAction *saveImgAct = menu->addAction(
            GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::SaveImage, amberCol),
            QStringLiteral("Save Image As…"));
        connect(saveImgAct, &QAction::triggered, this, [this, imgId]() {
            const QString picturesDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
            const QString baseDir = picturesDir.isEmpty() ? m_pwd : picturesDir;
            const QString suggested = QDir(baseDir).filePath(
                QStringLiteral("ghostty-union-image-%1.png").arg(imgId));
            const QString target = QFileDialog::getSaveFileName(
                this,
                QStringLiteral("Save Terminal Image"),
                suggested,
                QStringLiteral("PNG Image (*.png);;WebP Image (*.webp);;JPEG Image (*.jpg *.jpeg)"));
            if (!target.isEmpty()) {
                saveKittyImageToFile(imgId, target);
            }
        });
        menu->addSeparator();
    }

    uint16_t col = 0;
    uint16_t row = 0;
    GhosttyUnionLink::ResolvedLink ctxLink;
    if (m_terminal && strictCellAt(localPos, col, row)) {
        ctxLink = GhosttyUnionLink::resolveAt(
            m_terminal, m_cols, m_rows, col, row, m_pwd, /*allowBareCwdFiles=*/true);
    }

    if (ctxLink.isValid()) {
        const QString openLabel = ctxLink.isLocalFile ? QStringLiteral("Open File") : QStringLiteral("Open Link");
        QAction *openAct = menu->addAction(
            GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::OpenLink, cyanCol),
            openLabel);
        connect(openAct, &QAction::triggered, this, [this, ctxLink]() {
            activateLink(ctxLink);
        });

        const QString copyLabel = ctxLink.isLocalFile ? QStringLiteral("Copy File Path") : QStringLiteral("Copy Link Address");
        QAction *copyLinkAct = menu->addAction(
            GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::Copy, cyanCol),
            copyLabel);
        connect(copyLinkAct, &QAction::triggered, this, [ctxLink]() {
            if (QClipboard *cb = QApplication::clipboard()) {
                cb->setText(ctxLink.target, QClipboard::Clipboard);
            }
        });
        menu->addSeparator();
    }

    GhosttySelection activeSel{};
    const bool hasSelection = m_terminal &&
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_SELECTION, &activeSel) == GHOSTTY_SUCCESS;

    QAction *copyAct = menu->addAction(
        GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::Copy, goldCol),
        QStringLiteral("Copy\tCtrl+Shift+C"));
    copyAct->setEnabled(hasSelection);
    connect(copyAct, &QAction::triggered, this, &TerminalWidget::copySelection);

    const QClipboard *cb = QApplication::clipboard();
    const bool hasClipboardText = cb && !cb->text(QClipboard::Clipboard).isEmpty();
    QAction *pasteAct = menu->addAction(
        GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::Paste, goldCol),
        QStringLiteral("Paste\tCtrl+Shift+V"));
    pasteAct->setEnabled(hasClipboardText);
    connect(pasteAct, &QAction::triggered, this, [this]() { pasteClipboard(false); });

    menu->addSeparator();

    QAction *renameTabAct = menu->addAction(
        GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::Rename, goldCol),
        QStringLiteral("Rename Tab…\tCtrl+Shift+A"));
    connect(renameTabAct, &QAction::triggered, this, &TerminalWidget::renameTabRequested);

    menu->addSeparator();

    QAction *splitRightAct = menu->addAction(
        GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::SplitRight, amberCol),
        QStringLiteral("Split Right\tCtrl+Shift+}"));
    connect(splitRightAct, &QAction::triggered, this, [this]() {
        Q_EMIT splitRequested(Qt::Horizontal);
    });
    QAction *splitDownAct = menu->addAction(
        GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::SplitDown, amberCol),
        QStringLiteral("Split Down\tCtrl+Shift+{"));
    connect(splitDownAct, &QAction::triggered, this, [this]() {
        Q_EMIT splitRequested(Qt::Vertical);
    });

    if (m_splitRearrangeable) {
        QMenu *rearrangeMenu = menu->addMenu(
            GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::Rearrange, amberCol),
            QStringLiteral("Rearrange Split"));
        GhosttyUnionTheme::styleSolarForgeMenu(rearrangeMenu);
        QAction *mvLeft = rearrangeMenu->addAction(QStringLiteral("Move / Swap Left\tCtrl+Alt+Shift+Left"));
        connect(mvLeft, &QAction::triggered, this, [this]() { Q_EMIT moveSplitRequested(-1, 0); });
        QAction *mvRight = rearrangeMenu->addAction(QStringLiteral("Move / Swap Right\tCtrl+Alt+Shift+Right"));
        connect(mvRight, &QAction::triggered, this, [this]() { Q_EMIT moveSplitRequested(1, 0); });
        QAction *mvUp = rearrangeMenu->addAction(QStringLiteral("Move / Swap Up\tCtrl+Alt+Shift+Up"));
        connect(mvUp, &QAction::triggered, this, [this]() { Q_EMIT moveSplitRequested(0, -1); });
        QAction *mvDown = rearrangeMenu->addAction(QStringLiteral("Move / Swap Down\tCtrl+Alt+Shift+Down"));
        connect(mvDown, &QAction::triggered, this, [this]() { Q_EMIT moveSplitRequested(0, 1); });

        rearrangeMenu->addSeparator();
        QAction *rotAct = rearrangeMenu->addAction(QStringLiteral("Rotate Split Orientation\tCtrl+Shift+R"));
        connect(rotAct, &QAction::triggered, this, &TerminalWidget::rotateSplitRequested);
        QAction *eqAct = rearrangeMenu->addAction(QStringLiteral("Equalize Splits\tCtrl+Alt+Shift+E"));
        connect(eqAct, &QAction::triggered, this, &TerminalWidget::equalizeSplitsRequested);
        QAction *detachAct = rearrangeMenu->addAction(QStringLiteral("Move Split to New Tab"));
        connect(detachAct, &QAction::triggered, this, &TerminalWidget::moveToNewTabRequested);
    }

    menu->addSeparator();

    QAction *softResetAct = menu->addAction(
        GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::ResetName, cyanCol),
        QStringLiteral("Reset Input Modes\tCtrl+Shift+K"));
    connect(softResetAct, &QAction::triggered, this, &TerminalWidget::softResetTerminalModes);

    QAction *resetAct = menu->addAction(
        GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::ResetTerminal, redCol),
        QStringLiteral("Clear / Reset Terminal"));
    connect(resetAct, &QAction::triggered, this, &TerminalWidget::resetTerminal);

    GhosttyUnionTheme::popupSolarForgeMenuAnimated(menu, globalPos);
}

void TerminalWidget::mousePressEvent(QMouseEvent *event) {
    setFocus(Qt::MouseFocusReason);
    Q_EMIT focused(this);

    m_lastMousePos = event->position();
    m_mouseInside = true;

    if (isInScrollbarZone(event->position())) {
        if (event->button() == Qt::LeftButton) {
            QRectF trackRect;
            QRectF thumbRect;
            if (computeScrollbarGeometry(trackRect, thumbRect)) {
                const qreal py = event->position().y();
                m_scrollbarDragging = true;
                if (py >= thumbRect.top() - 2.0 && py <= thumbRect.bottom() + 2.0) {
                    m_scrollbarDragGrabOffset = py - thumbRect.top();
                } else {
                    m_scrollbarDragGrabOffset = thumbRect.height() * 0.5;
                    scrollViewportToThumbTop(py - m_scrollbarDragGrabOffset);
                }
                updateScrollbarHoverState(event->position());
                update();
            }
        }
        event->accept();
        return;
    }

    const auto nonKeypadMods = event->modifiers() & ~Qt::KeypadModifier;
    const bool ctrlAltDrag = (nonKeypadMods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) ==
                             (Qt::ControlModifier | Qt::AltModifier);
    if (m_splitRearrangeable && event->button() == Qt::LeftButton &&
        (isInSplitGripZone(event->position()) || ctrlAltDrag)) {
        m_splitDragArmed = true;
        m_splitDragging = false;
        m_splitDragStartPos = event->position();
        event->accept();
        return;
    }

    const bool ctrlOnly = (nonKeypadMods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) == Qt::ControlModifier;
    if (event->button() == Qt::LeftButton && ctrlOnly && m_terminal) {
        uint16_t col = 0;
        uint16_t row = 0;
        if (strictCellAt(event->position(), col, row)) {
            const auto link = GhosttyUnionLink::resolveAt(
                m_terminal, m_cols, m_rows, col, row, m_pwd, /*allowBareCwdFiles=*/true);
            if (link.isValid()) {
                m_linkClickPending = true;
                m_pressedLink = link;
                m_pressPos = event->position();
                m_hoveredLink = link;
                m_hoveredImage = {};
                refreshMouseCursor();
                update();
                event->accept();
                return;
            }
        }
        const HoveredImageInfo imgHit = resolveImageAt(event->position());
        if (imgHit.isValid()) {
            m_imageClickPending = true;
            m_pressedImage = imgHit;
            m_pressPos = event->position();
            m_hoveredImage = imgHit;
            refreshMouseCursor();
            update();
            event->accept();
            return;
        }
    }

    const bool shiftBypass = event->modifiers().testFlag(Qt::ShiftModifier);
    bool tracking = false;
    if (!shiftBypass && m_terminal) {
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &tracking);
    }

    if (event->button() == Qt::RightButton && (!tracking || shiftBypass)) {
        showContextMenu(event->position(), event->globalPosition().toPoint());
        event->accept();
        return;
    }

    if (tracking && sendMouseToPty(event, GHOSTTY_MOUSE_ACTION_PRESS, event->button())) {
        return;
    }

    if (event->button() == Qt::MiddleButton) {
        pasteClipboard(true);
        return;
    }

    if (event->button() == Qt::LeftButton) {
        handleSelectionMousePress(event);
    }
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event) {
    m_lastMousePos = event->position();

    if (m_splitDragArmed && event->button() == Qt::LeftButton) {
        const bool wasDragging = m_splitDragging;
        m_splitDragArmed = false;
        m_splitDragging = false;
        m_splitGripHovered = isInSplitGripZone(event->position());
        m_splitGripNear = isNearSplitGripZone(event->position());
        animateSplitGrip((m_splitGripHovered || m_splitGripNear) ? 1.0 : 0.0);
        refreshMouseCursor();
        update();
        if (wasDragging) {
            Q_EMIT splitDragReleased(this, event->globalPosition().toPoint());
        }
        event->accept();
        return;
    }

    if (m_scrollbarDragging && event->button() == Qt::LeftButton) {
        m_scrollbarDragging = false;
        updateScrollbarHoverState(event->position());
        animateScrollbarHover(m_scrollbarHovered ? 1.0 : 0.0);
        update();
        event->accept();
        return;
    }

    if (m_imageClickPending && event->button() == Qt::LeftButton) {
        const HoveredImageInfo pressed = m_pressedImage;
        m_imageClickPending = false;
        m_pressedImage = {};
        const HoveredImageInfo released = resolveImageAt(event->position());
        if (released.isValid() && released.imageId == pressed.imageId) {
            inspectKittyImage(pressed.imageId);
        }
        updateHoveredLink(event->position(), event->modifiers());
        event->accept();
        return;
    }

    if (m_linkClickPending && event->button() == Qt::LeftButton) {
        const auto pressed = m_pressedLink;
        m_linkClickPending = false;
        m_pressedLink = {};

        uint16_t col = 0;
        uint16_t row = 0;
        if (m_terminal && strictCellAt(event->position(), col, row)) {
            const auto released = GhosttyUnionLink::resolveAt(
                m_terminal, m_cols, m_rows, col, row, m_pwd, /*allowBareCwdFiles=*/true);
            if (released.sameDestination(pressed)) {
                activateLink(pressed);
            }
        }
        updateHoveredLink(event->position(), event->modifiers());
        event->accept();
        return;
    }

    const bool shiftBypass = event->modifiers().testFlag(Qt::ShiftModifier);
    bool tracking = false;
    if (!shiftBypass && m_terminal) {
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &tracking);
    }

    if (tracking && !m_selecting && sendMouseToPty(event, GHOSTTY_MOUSE_ACTION_RELEASE, event->button())) {
        return;
    }

    if (event->button() == Qt::LeftButton && m_selecting) {
        handleSelectionMouseRelease(event);
    }
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event) {
    m_lastMousePos = event->position();
    m_mouseInside = true;

    if (m_splitDragArmed) {
        if (!event->buttons().testFlag(Qt::LeftButton)) {
            const bool wasDragging = m_splitDragging;
            m_splitDragArmed = false;
            m_splitDragging = false;
            m_splitGripHovered = isInSplitGripZone(event->position());
            m_splitGripNear = isNearSplitGripZone(event->position());
            refreshMouseCursor();
            update();
            if (wasDragging) {
                Q_EMIT splitDragCancelled(this);
            }
            return;
        }
        if (!m_splitDragging && (event->position() - m_splitDragStartPos).manhattanLength() > 5.0) {
            m_splitDragging = true;
            clearHoveredLink();
            refreshMouseCursor();
            update();
        }
        if (m_splitDragging) {
            Q_EMIT splitDragMoved(this, event->globalPosition().toPoint());
        }
        event->accept();
        return;
    }

    const bool gripHovered = isInSplitGripZone(event->position());
    const bool gripNear = isNearSplitGripZone(event->position());
    if (gripHovered != m_splitGripHovered || gripNear != m_splitGripNear) {
        m_splitGripHovered = gripHovered;
        m_splitGripNear = gripNear;
        animateSplitGrip((m_splitGripHovered || m_splitGripNear || m_splitDragging) ? 1.0 : 0.0);
        refreshMouseCursor();
        update();
    }

    if (m_scrollbarDragging) {
        if (!event->buttons().testFlag(Qt::LeftButton)) {
            m_scrollbarDragging = false;
            updateScrollbarHoverState(event->position());
            update();
            return;
        }
        scrollViewportToThumbTop(event->position().y() - m_scrollbarDragGrabOffset);
        event->accept();
        return;
    }

    if (m_imageClickPending) {
        if ((event->position() - m_pressPos).manhattanLength() > 6.0) {
            m_imageClickPending = false;
            m_pressedImage = {};
        } else {
            event->accept();
            return;
        }
    }

    if (m_linkClickPending) {
        if ((event->position() - m_pressPos).manhattanLength() > 6.0) {
            m_linkClickPending = false;
            m_pressedLink = {};
        } else {
            event->accept();
            return;
        }
    }

    updateScrollbarHoverState(event->position());
    if (m_scrollbarHovered && !m_selecting && event->buttons() == Qt::NoButton) {
        clearHoveredLink();
        event->accept();
        return;
    }

    updateHoveredLink(event->position(), event->modifiers());

    const bool shiftBypass = event->modifiers().testFlag(Qt::ShiftModifier);
    bool tracking = false;
    if (!shiftBypass && m_terminal) {
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &tracking);
    }

    if (tracking && !m_selecting) {
        Qt::MouseButton pressedBtn = Qt::NoButton;
        if (event->buttons().testFlag(Qt::LeftButton)) {
            pressedBtn = Qt::LeftButton;
        } else if (event->buttons().testFlag(Qt::MiddleButton)) {
            pressedBtn = Qt::MiddleButton;
        } else if (event->buttons().testFlag(Qt::RightButton)) {
            pressedBtn = Qt::RightButton;
        }
        static_cast<void>(sendMouseToPty(event, GHOSTTY_MOUSE_ACTION_MOTION, pressedBtn));
        return;
    }

    if (m_selecting && event->buttons().testFlag(Qt::LeftButton)) {
        handleSelectionMouseMove(event);
    }
}

void TerminalWidget::leaveEvent(QEvent *event) {
    m_mouseInside = false;
    if (m_scrollbarHovered) {
        m_scrollbarHovered = false;
        animateScrollbarHover(m_scrollbarDragging ? 1.0 : 0.0);
    }
    if (m_splitGripHovered || m_splitGripNear) {
        m_splitGripHovered = false;
        m_splitGripNear = false;
        animateSplitGrip(m_splitDragging ? 1.0 : 0.0);
    }
    clearHoveredLink();
    refreshMouseCursor();
    QWidget::leaveEvent(event);
}

void TerminalWidget::wheelEvent(QWheelEvent *event) {
    const int deltaY = event->angleDelta().y();
    if (deltaY == 0 || !m_terminal) {
        return;
    }

    bool tracking = false;
    if (!event->modifiers().testFlag(Qt::ShiftModifier)) {
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &tracking);
    }

    if (tracking && m_mouseEncoder && m_mouseEvent) {
        ghostty_mouse_encoder_set_from_terminal(m_mouseEncoder, m_terminal);
        constexpr bool anyBtn = false;
        ghostty_mouse_encoder_set(m_mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &anyBtn);
        ghostty_mouse_event_set_action(m_mouseEvent, GHOSTTY_MOUSE_ACTION_PRESS);
        ghostty_mouse_event_set_button(m_mouseEvent, deltaY > 0 ? GHOSTTY_MOUSE_BUTTON_FOUR : GHOSTTY_MOUSE_BUTTON_FIVE);
        ghostty_mouse_event_set_mods(m_mouseEvent, GhosttyUnionKeyMap::mapQtModifiers(event->modifiers()));
        const QPointF p = event->position();
        ghostty_mouse_event_set_position(m_mouseEvent, GhosttyMousePosition{static_cast<float>(p.x()), static_cast<float>(p.y())});
        uint8_t buf[64];
        size_t len = 0;
        if (ghostty_mouse_encoder_encode(m_mouseEncoder, m_mouseEvent, buf, sizeof(buf), &len) == GHOSTTY_SUCCESS && len > 0) {
            flushPtyBytes(buf, len);
        }
        return;
    }

    const int lines = std::max(1, std::abs(deltaY) / 40);
    const GhosttyTerminalScrollViewport sb{
        .tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_DELTA,
        .value = {.delta = deltaY > 0 ? -lines : lines},
    };
    ghostty_terminal_scroll_viewport(m_terminal, sb);
    updateRenderState(false);
    update();
}

bool TerminalWidget::sendMouseToPty(const QMouseEvent *event, GhosttyMouseAction action, Qt::MouseButton qtBtn) const {
    if (!m_terminal || !m_mouseEncoder || !m_mouseEvent) {
        return false;
    }

    ghostty_mouse_encoder_set_from_terminal(m_mouseEncoder, m_terminal);
    const bool anyPressed = event->buttons() != Qt::NoButton;
    ghostty_mouse_encoder_set(m_mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &anyPressed);

    ghostty_mouse_event_set_action(m_mouseEvent, action);
    const GhosttyMouseButton gb = GhosttyUnionKeyMap::mapQtMouseButton(qtBtn);
    if (gb != GHOSTTY_MOUSE_BUTTON_UNKNOWN) {
        ghostty_mouse_event_set_button(m_mouseEvent, gb);
    } else {
        ghostty_mouse_event_clear_button(m_mouseEvent);
    }
    ghostty_mouse_event_set_mods(m_mouseEvent, GhosttyUnionKeyMap::mapQtModifiers(event->modifiers()));
    ghostty_mouse_event_set_position(m_mouseEvent, GhosttyMousePosition{
                                                       static_cast<float>(event->position().x()),
                                                       static_cast<float>(event->position().y())});

    uint8_t buf[64];
    size_t written = 0;
    if (ghostty_mouse_encoder_encode(m_mouseEncoder, m_mouseEvent, buf, sizeof(buf), &written) == GHOSTTY_SUCCESS && written > 0) {
        flushPtyBytes(buf, written);
        return true;
    }
    return false;
}

void TerminalWidget::handleSelectionMousePress(const QMouseEvent *event) {
    uint16_t col = 0;
    uint16_t row = 0;
    if (!cellPointAt(event->position(), col, row)) {
        return;
    }

    const GhosttyPoint pt{
        .tag = GHOSTTY_POINT_TAG_VIEWPORT,
        .value = {.coordinate = {.x = col, .y = row}},
    };

    GhosttyGridRef ref{};
    if (ghostty_terminal_grid_ref(m_terminal, pt, &ref) != GHOSTTY_SUCCESS) {
        clearSelection();
        return;
    }

    const GhosttySurfacePosition pos{event->position().x(), event->position().y()};
    const auto timeNs = static_cast<uint64_t>(m_clock.nsecsElapsed());
    const uint64_t repeatNs = static_cast<uint64_t>(QApplication::doubleClickInterval()) * 1000000ULL;
    constexpr double repeatDist = 6.0;

    ghostty_selection_gesture_event_set(m_pressEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &ref);
    ghostty_selection_gesture_event_set(m_pressEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &pos);
    ghostty_selection_gesture_event_set(m_pressEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_TIME_NS, &timeNs);
    ghostty_selection_gesture_event_set(m_pressEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_INTERVAL_NS, &repeatNs);
    ghostty_selection_gesture_event_set(m_pressEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_DISTANCE, &repeatDist);

    GhosttySelection sel{};
    if (ghostty_selection_gesture_event(m_gesture, m_terminal, m_pressEvent, &sel) == GHOSTTY_SUCCESS) {
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &sel);
    } else {
        clearSelection();
    }

    m_selecting = true;
    updateRenderState(false);
    update();
}

void TerminalWidget::handleSelectionMouseMove(const QMouseEvent *event) {
    uint16_t col = 0;
    uint16_t row = 0;
    if (!cellPointAt(event->position(), col, row)) {
        return;
    }

    const GhosttyPoint pt{
        .tag = GHOSTTY_POINT_TAG_VIEWPORT,
        .value = {.coordinate = {.x = col, .y = row}},
    };

    GhosttyGridRef ref{};
    if (ghostty_terminal_grid_ref(m_terminal, pt, &ref) != GHOSTTY_SUCCESS) {
        return;
    }

    const GhosttySurfacePosition pos{event->position().x(), event->position().y()};
    const GhosttySelectionGestureGeometry geom{
        static_cast<uint32_t>(m_cols),
        static_cast<uint32_t>(m_cellWidth),
        static_cast<uint32_t>(kPadX),
        static_cast<uint32_t>(std::max(1, height()))};
    const bool rectSel = event->modifiers().testFlag(Qt::AltModifier);

    ghostty_selection_gesture_event_set(m_dragEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &ref);
    ghostty_selection_gesture_event_set(m_dragEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &pos);
    ghostty_selection_gesture_event_set(m_dragEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_GEOMETRY, &geom);
    ghostty_selection_gesture_event_set(m_dragEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_RECTANGLE, &rectSel);

    GhosttySelection sel{};
    if (ghostty_selection_gesture_event(m_gesture, m_terminal, m_dragEvent, &sel) == GHOSTTY_SUCCESS) {
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &sel);
        updateRenderState(false);
        update();
    }
}

void TerminalWidget::handleSelectionMouseRelease(const QMouseEvent *event) {
    m_selecting = false;
    uint16_t col = 0;
    uint16_t row = 0;
    if (cellPointAt(event->position(), col, row)) {
        const GhosttyPoint pt{
            .tag = GHOSTTY_POINT_TAG_VIEWPORT,
            .value = {.coordinate = {.x = col, .y = row}},
        };
        GhosttyGridRef ref{};
        if (ghostty_terminal_grid_ref(m_terminal, pt, &ref) == GHOSTTY_SUCCESS) {
            ghostty_selection_gesture_event_set(m_releaseEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &ref);
        } else {
            ghostty_selection_gesture_event_set(m_releaseEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, nullptr);
        }
    } else {
        ghostty_selection_gesture_event_set(m_releaseEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, nullptr);
    }

    ghostty_selection_gesture_event(m_gesture, m_terminal, m_releaseEvent, nullptr);

    uint8_t clicks = 0;
    bool dragged = false;
    ghostty_selection_gesture_get(m_gesture, m_terminal, GHOSTTY_SELECTION_GESTURE_DATA_CLICK_COUNT, &clicks);
    ghostty_selection_gesture_get(m_gesture, m_terminal, GHOSTTY_SELECTION_GESTURE_DATA_DRAGGED, &dragged);

    if (clicks <= 1 && !dragged) {
        clearSelection();
        if (event->modifiers() == Qt::NoModifier) {
            static_cast<void>(tryClickToMovePromptCursor(event->position()));
        }
    } else {
        updatePrimaryClipboardFromSelection();
    }
    updateRenderState(false);
    update();
}

bool TerminalWidget::tryClickToMovePromptCursor(const QPointF &pos) const {
    if (!m_terminal || !m_keyEncoder || !m_keyEvent || (m_ptyMasterFd < 0 && !m_ptyOutputSpy)) {
        return false;
    }
    GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
    if (ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen) != GHOSTTY_SUCCESS ||
        screen != GHOSTTY_TERMINAL_SCREEN_PRIMARY) {
        return false;
    }
    bool tracking = true;
    if (ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &tracking) != GHOSTTY_SUCCESS || tracking) {
        return false;
    }
    bool activeVp = false;
    if (ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE, &activeVp) != GHOSTTY_SUCCESS || !activeVp) {
        return false;
    }
    bool atPrompt = false;
    if (ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_AT_PROMPT, &atPrompt) != GHOSTTY_SUCCESS || !atPrompt) {
        return false;
    }
    if (!m_cursor.viewport_has_value) {
        return false;
    }

    uint16_t clickCol = 0;
    uint16_t clickRow = 0;
    if (!strictCellAt(pos, clickCol, clickRow)) {
        return false;
    }

    const uint16_t curCol = m_cursor.viewport_x;
    const uint16_t curRow = m_cursor.viewport_y;
    if (curCol >= m_cols || curRow >= m_rows) {
        return false;
    }

    auto rowWrapFlag = [&](uint16_t r, GhosttyRowData key) -> bool {
        const GhosttyPoint pt{
            .tag = GHOSTTY_POINT_TAG_VIEWPORT,
            .value = {.coordinate = {.x = 0, .y = r}},
        };
        GhosttyGridRef ref{};
        if (ghostty_terminal_grid_ref(m_terminal, pt, &ref) != GHOSTTY_SUCCESS) {
            return false;
        }
        GhosttyRow grow{};
        if (ghostty_grid_ref_row(&ref, &grow) != GHOSTTY_SUCCESS) {
            return false;
        }
        bool flag = false;
        ghostty_row_get(grow, key, &flag);
        return flag;
    };
    auto rowsWrapped = [&](uint16_t upper, uint16_t lower) -> bool {
        return lower == upper + 1 &&
               (rowWrapFlag(upper, GHOSTTY_ROW_DATA_WRAP) || rowWrapFlag(lower, GHOSTTY_ROW_DATA_WRAP_CONTINUATION));
    };

    uint16_t clusterStart = curRow;
    while (clusterStart > 0 && rowsWrapped(static_cast<uint16_t>(clusterStart - 1), clusterStart)) {
        --clusterStart;
    }
    uint16_t clusterEnd = curRow;
    while (clusterEnd + 1 < m_rows && rowsWrapped(clusterEnd, static_cast<uint16_t>(clusterEnd + 1))) {
        ++clusterEnd;
    }
    if (clickRow < clusterStart || clickRow > clusterEnd) {
        return false;
    }

    struct PromptCellInfo {
        bool isInput = false;
        bool hasText = false;
    };
    std::vector<PromptCellInfo> cells;
    cells.reserve(static_cast<size_t>(clusterEnd - clusterStart + 1) * m_cols);
    int cursorIdx = -1;
    int clickIdx = -1;

    for (uint16_t r = clusterStart; r <= clusterEnd; ++r) {
        for (uint16_t c = 0; c < m_cols; ++c) {
            const GhosttyPoint pt{
                .tag = GHOSTTY_POINT_TAG_VIEWPORT,
                .value = {.coordinate = {.x = c, .y = r}},
            };
            GhosttyGridRef ref{};
            if (ghostty_terminal_grid_ref(m_terminal, pt, &ref) != GHOSTTY_SUCCESS) {
                continue;
            }
            GhosttyCell cell{};
            if (ghostty_grid_ref_cell(&ref, &cell) != GHOSTTY_SUCCESS) {
                continue;
            }
            GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
            ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &wide);
            if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
                if (r == curRow && c == curCol && !cells.empty()) {
                    cursorIdx = static_cast<int>(cells.size()) - 1;
                }
                if (r == clickRow && c == clickCol && !cells.empty()) {
                    clickIdx = static_cast<int>(cells.size()) - 1;
                }
                continue;
            }
            if (wide == GHOSTTY_CELL_WIDE_SPACER_HEAD) {
                continue;
            }

            GhosttyCellSemanticContent semantic = GHOSTTY_CELL_SEMANTIC_OUTPUT;
            ghostty_cell_get(cell, GHOSTTY_CELL_DATA_SEMANTIC_CONTENT, &semantic);
            bool hasText = false;
            ghostty_cell_get(cell, GHOSTTY_CELL_DATA_HAS_TEXT, &hasText);

            const int idx = static_cast<int>(cells.size());
            if (r == curRow && c == curCol) {
                cursorIdx = idx;
            }
            if (r == clickRow && c == clickCol) {
                clickIdx = idx;
            }
            cells.push_back(PromptCellInfo{semantic == GHOSTTY_CELL_SEMANTIC_INPUT, hasText});
        }
    }

    if (cursorIdx < 0 || clickIdx < 0 || cursorIdx == clickIdx) {
        return false;
    }

    const int lo = std::min(cursorIdx, clickIdx);
    const int hi = std::max(cursorIdx, clickIdx);
    for (int i = lo; i < hi; ++i) {
        if (!cells[static_cast<size_t>(i)].isInput) {
            return false;
        }
    }
    const PromptCellInfo &hiCell = cells[static_cast<size_t>(hi)];
    if (!hiCell.isInput) {
        if (hiCell.hasText || hi == 0 || !cells[static_cast<size_t>(hi - 1)].isInput) {
            return false;
        }
    }

    const bool goLeft = (clickIdx < cursorIdx);
    const int steps = hi - lo;
    ghostty_key_encoder_set_from_terminal(m_keyEncoder, m_terminal);

    const int qtArrow = goLeft ? Qt::Key_Left : Qt::Key_Right;
    const QKeyEvent pressEv(QEvent::KeyPress, qtArrow, Qt::NoModifier);
    QByteArray utf8Storage;
    GhosttyUnionKeyMap::populateGhosttyKeyEvent(m_keyEvent, &pressEv, GHOSTTY_KEY_ACTION_PRESS, utf8Storage);

    uint8_t seqBuf[64];
    size_t seqLen = 0;
    if (ghostty_key_encoder_encode(m_keyEncoder, m_keyEvent, seqBuf, sizeof(seqBuf), &seqLen) != GHOSTTY_SUCCESS || seqLen == 0) {
        return false;
    }

    QByteArray out;
    out.reserve(steps * static_cast<int>(seqLen));
    for (int i = 0; i < steps; ++i) {
        out.append(reinterpret_cast<const char *>(seqBuf), static_cast<qsizetype>(seqLen));
    }
    flushPtyBytes(reinterpret_cast<const uint8_t *>(out.constData()), static_cast<size_t>(out.size()));
    return true;
}

void TerminalWidget::clearSelection() const {
    if (m_terminal) {
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SELECTION, nullptr);
    }
}

namespace {

bool isKittyPlaceholderMark(uint32_t cp) {
    return QChar::isMark(cp) ||
           (cp >= 0x0300U && cp <= 0x036FU) ||
           (cp >= 0x1AB0U && cp <= 0x1AFFU) ||
           (cp >= 0x1DC0U && cp <= 0x1DFFU) ||
           (cp >= 0x20D0U && cp <= 0x20FFU) ||
           (cp >= 0xFE20U && cp <= 0xFE2FU);
}

QString sanitizeKittyPlaceholdersFromPlainText(const QString &raw) {
    const char16_t hi = QChar::highSurrogate(kKittyUnicodePlaceholder);
    if (!raw.contains(QChar(hi))) {
        return raw;
    }
    const QList<uint> ucs4 = raw.toUcs4();
    std::vector<char32_t> cleaned;
    cleaned.reserve(static_cast<size_t>(ucs4.size()));
    bool lineHadPlaceholder = false;

    auto trimTrailingSpaces = [&]() {
        if (!lineHadPlaceholder) {
            return;
        }
        while (!cleaned.empty() && cleaned.back() == U' ') {
            cleaned.pop_back();
        }
        lineHadPlaceholder = false;
    };

    for (qsizetype i = 0; i < ucs4.size(); ++i) {
        const uint32_t cp = ucs4[i];
        if (cp == kKittyUnicodePlaceholder) {
            lineHadPlaceholder = true;
            cleaned.push_back(U' ');
            while (i + 1 < ucs4.size() && isKittyPlaceholderMark(ucs4[i + 1])) {
                ++i;
            }
            continue;
        }
        if (cp == U'\n') {
            trimTrailingSpaces();
            cleaned.push_back(U'\n');
            continue;
        }
        cleaned.push_back(static_cast<char32_t>(cp));
    }
    trimTrailingSpaces();
    return cleaned.empty()
               ? QString()
               : QString::fromUcs4(cleaned.data(), static_cast<qsizetype>(cleaned.size()));
}

QString formatTerminalPlainText(GhosttyTerminal terminal, const GhosttySelection *selection) {
    if (!terminal) {
        return {};
    }
    GhosttyFormatter formatter = nullptr;
    if (ghostty_formatter_terminal_new(nullptr, &formatter, terminal) != GHOSTTY_SUCCESS || !formatter) {
        return {};
    }
    constexpr GhosttyFormatterFormat emitFmt = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    constexpr bool unwrap = true;
    constexpr bool trim = true;
    ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_EMIT, &emitFmt);
    ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_UNWRAP, &unwrap);
    ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_TRIM, &trim);
    if (selection) {
        ghostty_formatter_set(formatter, GHOSTTY_FORMATTER_OPT_SELECTION, selection);
    }

    uint8_t *ptr = nullptr;
    size_t len = 0;
    QString out;
    if (ghostty_formatter_format_alloc(formatter, nullptr, &ptr, &len) == GHOSTTY_SUCCESS && ptr && len > 0) {
        out = sanitizeKittyPlaceholdersFromPlainText(
            QString::fromUtf8(reinterpret_cast<const char *>(ptr), static_cast<qsizetype>(len)));
    }
    if (ptr) {
        ghostty_free(nullptr, ptr, len);
    }
    ghostty_formatter_free(formatter);
    return out;
}

void copySelectionToClipboard(GhosttyTerminal terminal, QClipboard::Mode mode) {
    if (!terminal) {
        return;
    }
    GhosttySelection activeSel{};
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_SELECTION, &activeSel) != GHOSTTY_SUCCESS) {
        return;
    }
    const QString text = formatTerminalPlainText(terminal, &activeSel);
    if (!text.isEmpty()) {
        if (QClipboard *cb = QApplication::clipboard()) {
            cb->setText(text, mode);
        }
    }
}

} // namespace

void TerminalWidget::updatePrimaryClipboardFromSelection() const {
    copySelectionToClipboard(m_terminal, QClipboard::Selection);
}

void TerminalWidget::copySelection() const {
    copySelectionToClipboard(m_terminal, QClipboard::Clipboard);
}

QString TerminalWidget::plainTextSnapshot() const {
    if (!m_terminal) {
        return {};
    }
    GhosttySelection sel{};
    if (ghostty_terminal_select_all(m_terminal, &sel) != GHOSTTY_SUCCESS) {
        return {};
    }
    return formatTerminalPlainText(m_terminal, &sel);
}

void TerminalWidget::pasteClipboard(bool selectionOnly) {
    ensureShellSpawned();
    const QClipboard *cb = QApplication::clipboard();
    if (!cb) {
        return;
    }
    const QString text = cb->text(selectionOnly ? QClipboard::Selection : QClipboard::Clipboard);
    pasteText(text,
              selectionOnly ? GHOSTTY_CLIPBOARD_LOCATION_SELECTION : GHOSTTY_CLIPBOARD_LOCATION_STANDARD,
              GHOSTTY_PASTE_SOURCE_CLIPBOARD);
}

void TerminalWidget::pasteText(const QString &text,
                               GhosttyClipboardLocation location,
                               GhosttyPasteSource source) {
    ensureShellSpawned();
    if (!m_terminal || text.isEmpty()) {
        return;
    }

    PasteContext ctx{text.toUtf8()};
    const char *mimeStr = "text/plain";
    const GhosttyString mime{reinterpret_cast<const uint8_t *>(mimeStr), std::strlen(mimeStr)};

    GhosttyPaste req = GHOSTTY_INIT_SIZED(GhosttyPaste);
    req.location = location;
    req.source = source;
    req.mimes = &mime;
    req.mimes_len = 1;
    req.reader = GhosttyMimeReader{&pasteMimeReader, &ctx};
    req.allow_unsafe = false;

    bool written = false;
    const GhosttyResult res = ghostty_terminal_paste(m_terminal, &req, &written);
    if (res == GHOSTTY_REJECTED) {
        const auto choice = QMessageBox::warning(
            this,
            QStringLiteral("Confirm Multi-Line Paste"),
            QStringLiteral("The clipboard contains newlines or control sequences that could execute commands immediately.\n\nPaste anyway?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (choice == QMessageBox::Yes) {
            req.allow_unsafe = true;
            ghostty_terminal_paste(m_terminal, &req, &written);
        }
    }
}

void TerminalWidget::zoomIn() {
    m_basePointSize = std::min(28.0, m_basePointSize + 1.0);
    updateFontMetrics();
    m_resizeCoalesceTimer.stop();
    resizeTerminalToWidget();
}

void TerminalWidget::zoomOut() {
    m_basePointSize = std::max(6.0, m_basePointSize - 1.0);
    updateFontMetrics();
    m_resizeCoalesceTimer.stop();
    resizeTerminalToWidget();
}

void TerminalWidget::zoomReset() {
    m_basePointSize = 10.5;
    updateFontMetrics();
    m_resizeCoalesceTimer.stop();
    resizeTerminalToWidget();
}

void TerminalWidget::sendFocusEventToPty(GhosttyFocusEvent ev) const {
    if (!m_terminal || (m_ptyMasterFd < 0 && !m_ptyOutputSpy)) {
        return;
    }
    GhosttyTerminalModeConfig cfg{
        .mode = GHOSTTY_MODE_FOCUS_EVENT,
        .value = false,
    };
    if (ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MODE, &cfg) != GHOSTTY_SUCCESS || !cfg.value) {
        return;
    }
    uint8_t buf[16];
    size_t written = 0;
    if (ghostty_focus_encode(ev, buf, sizeof(buf), &written) == GHOSTTY_SUCCESS && written > 0) {
        flushPtyBytes(buf, written);
    }
}

void TerminalWidget::focusInEvent(QFocusEvent *event) {
    QWidget::focusInEvent(event);
    m_blinkVisible = true;
    sendFocusEventToPty(GHOSTTY_FOCUS_GAINED);
    Q_EMIT focused(this);
    update();
}

void TerminalWidget::focusOutEvent(QFocusEvent *event) {
    QWidget::focusOutEvent(event);
    m_linkClickPending = false;
    m_imageClickPending = false;
    m_pressedLink = {};
    m_pressedImage = {};
    if (m_splitDragArmed) {
        const bool wasDragging = m_splitDragging;
        m_splitDragArmed = false;
        m_splitDragging = false;
        if (wasDragging) {
            Q_EMIT splitDragCancelled(this);
        }
        refreshMouseCursor();
    }
    clearHoveredLink();
    sendFocusEventToPty(GHOSTTY_FOCUS_LOST);
    update();
}

void TerminalWidget::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData() && (event->mimeData()->hasUrls() || event->mimeData()->hasText())) {
        event->acceptProposedAction();
    } else {
        QWidget::dragEnterEvent(event);
    }
}

void TerminalWidget::dropEvent(QDropEvent *event) {
    const QMimeData *mime = event->mimeData();
    if (!mime) {
        QWidget::dropEvent(event);
        return;
    }
    QString payload;
    if (mime->hasUrls()) {
        payload = GhosttyUnionLink::shellQuotePaths(mime->urls());
    }
    if (payload.isEmpty() && mime->hasText()) {
        payload = mime->text();
    }
    if (!payload.isEmpty()) {
        pasteText(payload, GHOSTTY_CLIPBOARD_LOCATION_STANDARD, GHOSTTY_PASTE_SOURCE_TEXT);
        event->acceptProposedAction();
        return;
    }
    QWidget::dropEvent(event);
}

bool TerminalWidget::focusNextPrevChild(bool /*next*/) {
    return false;
}

void TerminalWidget::onRenderHoldTimeout() {
    if (!m_renderHeld || !m_terminal) {
        return;
    }
    GhosttyTerminalModeConfig mode{
        .mode = GHOSTTY_MODE_SYNCHRONIZED_OUTPUT,
        .value = false,
    };
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_MODE, &mode);
    m_renderHeld = false;
    updateRenderState(false);
    update();
}

void TerminalWidget::onCursorBlink() {
    if (hasFocus() && m_cursor.visible && m_cursor.blinking) {
        m_blinkVisible = !m_blinkVisible;
        update();
    }
}

void TerminalWidget::onKittyAnimationTimer() {
    if (!m_terminal || m_renderHeld) {
        return;
    }
    updateRenderState(false);
    update();
}

const TerminalWidget::CachedKittyTexture *TerminalWidget::ensureKittyTexture(
    GhosttyKittyGraphics kg,
    uint32_t imageId,
    uint64_t expectedGeneration) {
    if (!kg || imageId == 0) {
        return nullptr;
    }

    if (expectedGeneration != 0) {
        auto it = m_textureCache.constFind(expectedGeneration);
        if (it != m_textureCache.constEnd() && it->imageId == imageId && !it->premulImage.isNull()) {
            return &it.value();
        }
    }

    const GhosttyKittyGraphicsImage imgHandle = ghostty_kitty_graphics_image(kg, imageId);
    if (!imgHandle) {
        return nullptr;
    }

    uint64_t actualGen = 0;
    uint32_t imgW = 0;
    uint32_t imgH = 0;
    GhosttyKittyImageFormat fmt = GHOSTTY_KITTY_IMAGE_FORMAT_RGBA;
    GhosttyString pixels{};

    if (ghostty_kitty_graphics_image_get(imgHandle, GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_GENERATION, &actualGen) != GHOSTTY_SUCCESS ||
        ghostty_kitty_graphics_image_get(imgHandle, GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_WIDTH, &imgW) != GHOSTTY_SUCCESS ||
        ghostty_kitty_graphics_image_get(imgHandle, GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_HEIGHT, &imgH) != GHOSTTY_SUCCESS ||
        ghostty_kitty_graphics_image_get(imgHandle, GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_FORMAT, &fmt) != GHOSTTY_SUCCESS ||
        ghostty_kitty_graphics_image_get(imgHandle, GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_PIXELS, &pixels) != GHOSTTY_SUCCESS) {
        return nullptr;
    }

    if (actualGen == 0 || imgW == 0 || imgH == 0 || imgW > 16384U || imgH > 16384U || !pixels.ptr || pixels.len == 0) {
        return nullptr;
    }

    auto existing = m_textureCache.constFind(actualGen);
    if (existing != m_textureCache.constEnd() && existing->imageId == imageId && !existing->premulImage.isNull()) {
        return &existing.value();
    }

    QImage premul;
    const auto w = static_cast<int>(imgW);
    const auto h = static_cast<int>(imgH);
    const size_t pixelCount = static_cast<size_t>(imgW) * static_cast<size_t>(imgH);

    switch (fmt) {
    case GHOSTTY_KITTY_IMAGE_FORMAT_RGBA: {
        if (pixels.len < pixelCount * 4ULL) {
            return nullptr;
        }
        const QImage view(pixels.ptr, w, h, w * 4, QImage::Format_RGBA8888);
        premul = view.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        break;
    }
    case GHOSTTY_KITTY_IMAGE_FORMAT_RGB: {
        if (pixels.len < pixelCount * 3ULL) {
            return nullptr;
        }
        const QImage view(pixels.ptr, w, h, w * 3, QImage::Format_RGB888);
        premul = view.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        break;
    }
    case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY: {
        if (pixels.len < pixelCount) {
            return nullptr;
        }
        const QImage view(pixels.ptr, w, h, w, QImage::Format_Grayscale8);
        premul = view.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        break;
    }
    case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA: {
        if (pixels.len < pixelCount * 2ULL) {
            return nullptr;
        }
        QImage rgba(w, h, QImage::Format_RGBA8888);
        for (int y = 0; y < h; ++y) {
            uint8_t *dstRow = rgba.scanLine(y);
            const uint8_t *srcRow = pixels.ptr + static_cast<size_t>(y) * static_cast<size_t>(w) * 2ULL;
            for (int x = 0; x < w; ++x) {
                const uint8_t lum = srcRow[x * 2];
                const uint8_t alpha = srcRow[x * 2 + 1];
                dstRow[x * 4 + 0] = lum;
                dstRow[x * 4 + 1] = lum;
                dstRow[x * 4 + 2] = lum;
                dstRow[x * 4 + 3] = alpha;
            }
        }
        premul = rgba.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        break;
    }
    default:
        return nullptr;
    }

    if (premul.isNull()) {
        return nullptr;
    }

    CachedKittyTexture entry;
    entry.imageId = imageId;
    entry.imageGeneration = actualGen;
    entry.pixelWidth = w;
    entry.pixelHeight = h;
    entry.premulImage = std::move(premul);
    auto inserted = m_textureCache.insert(actualGen, std::move(entry));
    return &inserted.value();
}

void TerminalWidget::pruneKittyTextureCache(GhosttyKittyGraphics kg) {
    if (m_textureCache.isEmpty()) {
        return;
    }
    if (!kg) {
        m_textureCache.clear();
        return;
    }
    for (auto it = m_textureCache.begin(); it != m_textureCache.end();) {
        const GhosttyKittyGraphicsImage img = ghostty_kitty_graphics_image(kg, it->imageId);
        uint64_t liveGen = 0;
        if (!img ||
            ghostty_kitty_graphics_image_get(img, GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_GENERATION, &liveGen) != GHOSTTY_SUCCESS ||
            liveGen != it->imageGeneration) {
            it = m_textureCache.erase(it);
        } else {
            ++it;
        }
    }
}

void TerminalWidget::refreshImagePlacements(bool forceRebuild) {
    if (!m_terminal || !m_renderState || !m_imageIter) {
        return;
    }

    GhosttyKittyGraphics kg = nullptr;
    uint64_t storageGen = 0;
    if (ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &kg) == GHOSTTY_SUCCESS && kg) {
        ghostty_kitty_graphics_get(kg, GHOSTTY_KITTY_GRAPHICS_DATA_GENERATION, &storageGen);
    }

    uint64_t placementsGen = 0;
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_IMAGE_PLACEMENTS_GENERATION, &placementsGen);

    const bool storageChanged = (storageGen != m_lastKittyStorageGen);
    if (storageChanged) {
        pruneKittyTextureCache(kg);
        m_lastKittyStorageGen = storageGen;
    }

    if (placementsGen == 0 && storageGen == 0) {
        m_visiblePlacements.clear();
        m_lastPlacementsGen = 0;
        m_placementsInitialized = true;
        return;
    }

    if (!forceRebuild && m_placementsInitialized && !storageChanged && placementsGen == m_lastPlacementsGen) {
        // Refresh any active textures whose per-image generation updated (e.g. animation frames)
        if (kg) {
            for (const VisibleImagePlacement &pl : m_visiblePlacements) {
                ensureKittyTexture(kg, pl.imageId, pl.imageGeneration);
            }
        }
        return;
    }

    m_lastPlacementsGen = placementsGen;
    m_placementsInitialized = true;
    m_visiblePlacements.clear();

    if (ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_IMAGE_ITERATOR, &m_imageIter) != GHOSTTY_SUCCESS) {
        return;
    }

    const qreal scaleX = static_cast<qreal>(m_cellWidth) / static_cast<qreal>(std::max(1, m_appliedVtCellWidth));
    const qreal scaleY = static_cast<qreal>(m_cellHeight) / static_cast<qreal>(std::max(1, m_appliedVtCellHeight));

    size_t orderIdx = 0;
    while (ghostty_render_state_image_iterator_next(m_imageIter)) {
        VisibleImagePlacement pl;
        pl.orderIndex = orderIdx++;

        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_PROTOCOL, &pl.protocol);
        if (pl.protocol != GHOSTTY_IMAGE_PROTOCOL_KITTY) {
            continue;
        }
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_ID, &pl.imageId);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_GENERATION, &pl.imageGeneration);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_LAYER, &pl.layer);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_Z, &pl.z);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_COL, &pl.viewportCol);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_ROW, &pl.viewportRow);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_GRID_COLS, &pl.gridCols);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_GRID_ROWS, &pl.gridRows);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_X, &pl.cellOffsetX);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_Y, &pl.cellOffsetY);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_WIDTH, &pl.pixelWidth);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_HEIGHT, &pl.pixelHeight);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_X, &pl.sourceX);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_Y, &pl.sourceY);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_WIDTH, &pl.sourceWidth);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_HEIGHT, &pl.sourceHeight);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_KITTY_PLACEMENT_ID, &pl.placementId);
        ghostty_render_state_image_get(m_imageIter, GHOSTTY_RENDER_STATE_IMAGE_DATA_KITTY_KIND, &pl.kittyKind);

        if (pl.imageId == 0 || pl.pixelWidth == 0 || pl.pixelHeight == 0 ||
            pl.sourceWidth == 0 || pl.sourceHeight == 0) {
            continue;
        }

        const qreal dstX = static_cast<qreal>(kPadX) +
                           static_cast<qreal>(pl.viewportCol) * static_cast<qreal>(m_cellWidth) +
                           static_cast<qreal>(pl.cellOffsetX) * scaleX;
        const qreal dstY = static_cast<qreal>(kPadY) +
                           static_cast<qreal>(pl.viewportRow) * static_cast<qreal>(m_cellHeight) +
                           static_cast<qreal>(pl.cellOffsetY) * scaleY;
        const qreal dstW = static_cast<qreal>(pl.pixelWidth) * scaleX;
        const qreal dstH = static_cast<qreal>(pl.pixelHeight) * scaleY;
        pl.dstRect = QRectF(dstX, dstY, dstW, dstH);

        if (pl.kittyKind == GHOSTTY_RENDER_STATE_KITTY_PLACEMENT_KIND_VIRTUAL &&
            pl.gridCols > 0 && pl.gridRows > 0) {
            const QRectF cellBox(
                static_cast<qreal>(kPadX) + static_cast<qreal>(pl.viewportCol) * static_cast<qreal>(m_cellWidth),
                static_cast<qreal>(kPadY) + static_cast<qreal>(pl.viewportRow) * static_cast<qreal>(m_cellHeight),
                static_cast<qreal>(pl.gridCols) * static_cast<qreal>(m_cellWidth),
                static_cast<qreal>(pl.gridRows) * static_cast<qreal>(m_cellHeight));
            pl.hitRect = pl.dstRect.united(cellBox);
        } else {
            pl.hitRect = pl.dstRect;
        }

        if (kg) {
            ensureKittyTexture(kg, pl.imageId, pl.imageGeneration);
        }
        m_visiblePlacements.push_back(pl);
    }

    std::stable_sort(m_visiblePlacements.begin(), m_visiblePlacements.end(),
                     [](const VisibleImagePlacement &a, const VisibleImagePlacement &b) {
                         if (a.layer != b.layer) {
                             return static_cast<int>(a.layer) < static_cast<int>(b.layer);
                         }
                         if (a.z != b.z) {
                             return a.z < b.z;
                         }
                         return a.orderIndex < b.orderIndex;
                     });
}

void TerminalWidget::scheduleKittyAnimationTick(bool hasActiveAnim, uint64_t nowMs, uint64_t nextAnimMs) {
    if (!m_terminal || !hasActiveAnim || m_visiblePlacements.empty()) {
        m_kittyAnimTimer.stop();
        return;
    }
    const qint64 waitMs = std::clamp<qint64>(
        static_cast<qint64>(nextAnimMs) - static_cast<qint64>(nowMs), 8, 60000);
    m_kittyAnimTimer.start(static_cast<int>(waitMs));
}

void TerminalWidget::drawImagePlacementsLayer(QPainter &p, GhosttyImageLayer layer) {
    if (m_visiblePlacements.empty()) {
        return;
    }

    bool anyInLayer = false;
    for (const VisibleImagePlacement &pl : m_visiblePlacements) {
        if (pl.layer == layer) {
            anyInLayer = true;
            break;
        }
    }
    if (!anyInLayer) {
        return;
    }

    const QRectF gridClip = viewportGridRect();
    const qreal dpr = std::max(1.0, devicePixelRatioF());

    p.save();
    p.setClipRect(gridClip);

    for (const VisibleImagePlacement &pl : m_visiblePlacements) {
        if (pl.layer != layer) {
            continue;
        }
        auto texIt = m_textureCache.constFind(pl.imageGeneration);
        if (texIt == m_textureCache.constEnd()) {
            continue;
        }
        const QImage &tex = texIt->premulImage;
        if (tex.isNull()) {
            continue;
        }

        const QRectF srcRect(static_cast<qreal>(pl.sourceX),
                             static_cast<qreal>(pl.sourceY),
                             static_cast<qreal>(pl.sourceWidth),
                             static_cast<qreal>(pl.sourceHeight));
        const qreal devDstW = pl.dstRect.width() * dpr;
        const qreal devDstH = pl.dstRect.height() * dpr;
        const bool crispPixelArt = (texIt->pixelWidth <= 32 && texIt->pixelHeight <= 32 &&
                                    devDstW >= srcRect.width() * 2.0 && devDstH >= srcRect.height() * 2.0);
        p.setRenderHint(QPainter::SmoothPixmapTransform, !crispPixelArt);
        p.drawImage(pl.dstRect, tex, srcRect);
    }

    p.restore();
}

TerminalWidget::HoveredImageInfo TerminalWidget::resolveImageAt(const QPointF &pos) const {
    if (m_visiblePlacements.empty()) {
        return {};
    }
    const QRectF gridClip = viewportGridRect();
    if (!gridClip.contains(pos)) {
        return {};
    }

    const VisibleImagePlacement *hit = nullptr;
    for (auto it = m_visiblePlacements.rbegin(); it != m_visiblePlacements.rend(); ++it) {
        const QRectF clipped = it->hitRect.intersected(gridClip);
        if (!clipped.isEmpty() && clipped.contains(pos)) {
            hit = &(*it);
            break;
        }
    }
    if (!hit) {
        return {};
    }

    HoveredImageInfo info;
    info.imageId = hit->imageId;
    info.placementId = hit->placementId;
    info.kittyKind = hit->kittyKind;

    auto texIt = m_textureCache.constFind(hit->imageGeneration);
    if (texIt != m_textureCache.constEnd()) {
        info.sourcePixelWidth = texIt->pixelWidth;
        info.sourcePixelHeight = texIt->pixelHeight;
    } else {
        info.sourcePixelWidth = static_cast<int>(hit->sourceWidth);
        info.sourcePixelHeight = static_cast<int>(hit->sourceHeight);
    }

    if (hit->kittyKind == GHOSTTY_RENDER_STATE_KITTY_PLACEMENT_KIND_VIRTUAL) {
        QRectF united = hit->hitRect.intersected(gridClip);
        for (const VisibleImagePlacement &pl : m_visiblePlacements) {
            if (pl.kittyKind != GHOSTTY_RENDER_STATE_KITTY_PLACEMENT_KIND_VIRTUAL ||
                pl.imageId != hit->imageId ||
                pl.placementId != hit->placementId) {
                continue;
            }
            if (hit->placementId == 0 &&
                (pl.viewportCol != hit->viewportCol || pl.gridCols != hit->gridCols)) {
                continue;
            }
            united = united.united(pl.hitRect.intersected(gridClip));
        }
        info.unionRect = united;
    } else {
        info.unionRect = hit->dstRect.intersected(gridClip);
    }
    return info;
}

QImage TerminalWidget::kittyImageById(uint32_t imageId) const {
    if (!m_terminal || imageId == 0) {
        return {};
    }
    for (auto it = m_textureCache.constBegin(); it != m_textureCache.constEnd(); ++it) {
        if (it->imageId == imageId && !it->premulImage.isNull()) {
            return it->premulImage.convertToFormat(QImage::Format_RGBA8888);
        }
    }
    GhosttyKittyGraphics kg = nullptr;
    if (ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &kg) != GHOSTTY_SUCCESS || !kg) {
        return {};
    }
    auto *mutableSelf = const_cast<TerminalWidget *>(this);
    const CachedKittyTexture *tex = mutableSelf->ensureKittyTexture(kg, imageId, 0);
    return (tex && !tex->premulImage.isNull())
               ? tex->premulImage.convertToFormat(QImage::Format_RGBA8888)
               : QImage();
}

bool TerminalWidget::copyKittyImageToClipboard(uint32_t imageId) const {
    const QImage img = kittyImageById(imageId);
    if (img.isNull()) {
        return false;
    }
    QClipboard *cb = QApplication::clipboard();
    if (!cb) {
        return false;
    }
    cb->setImage(img, QClipboard::Clipboard);
    return true;
}

bool TerminalWidget::saveKittyImageToFile(uint32_t imageId, const QString &filePath) const {
    if (filePath.isEmpty()) {
        return false;
    }
    const QImage img = kittyImageById(imageId);
    if (img.isNull()) {
        return false;
    }
    return img.save(filePath);
}

void TerminalWidget::inspectKittyImage(uint32_t imageId) {
    const QImage img = kittyImageById(imageId);
    if (img.isNull()) {
        return;
    }
    QWidget *hostWindow = window();
    if (!hostWindow) {
        hostWindow = this;
    }
    if (auto *existing = hostWindow->findChild<QWidget *>(QStringLiteral("imagePreviewLightbox"))) {
        existing->close();
    }
    auto *lightbox = new SolarForgeImageLightbox(img, imageId, this, hostWindow);
    lightbox->show();
    lightbox->raise();
    lightbox->setFocus(Qt::PopupFocusReason);
}

void TerminalWidget::enqueueOsc1337ImageSequence(const uint8_t *content, size_t len) {
    if (!content || len < 11) {
        return;
    }
    const QByteArray raw = QByteArray::fromRawData(reinterpret_cast<const char *>(content),
                                                   static_cast<qsizetype>(len));
    if (!raw.startsWith("1337;")) {
        return;
    }
    const QByteArray afterCode = raw.mid(5);
    if (afterCode.size() < 6 || afterCode.left(5).toLower() != "file=") {
        return;
    }

    const qsizetype colonIdx = afterCode.indexOf(':', 5);
    if (colonIdx < 5 || colonIdx + 1 >= afterCode.size()) {
        return;
    }

    const QByteArray argsPart = afterCode.mid(5, colonIdx - 5);
    const QByteArray b64Part = afterCode.mid(colonIdx + 1);

    bool isInline = false;
    bool preserveAspect = true;
    bool doNotMoveCursor = false;
    QByteArray widthSpec;
    QByteArray heightSpec;

    for (const QByteArray &kv : argsPart.split(';')) {
        const qsizetype eq = kv.indexOf('=');
        if (eq <= 0) {
            continue;
        }
        const QByteArray k = kv.left(eq).trimmed();
        const QByteArray v = kv.mid(eq + 1).trimmed();
        if (k == "inline") {
            isInline = (v == "1");
        } else if (k == "width") {
            widthSpec = v;
        } else if (k == "height") {
            heightSpec = v;
        } else if (k == "preserveAspectRatio") {
            preserveAspect = (v != "0");
        } else if (k == "doNotMoveCursor") {
            doNotMoveCursor = (v == "1");
        }
    }
    if (!isInline) {
        return;
    }

    const QByteArray decodedBytes = QByteArray::fromBase64(b64Part);
    if (decodedBytes.isEmpty()) {
        return;
    }

    QBuffer buf;
    buf.setData(decodedBytes);
    if (!buf.open(QIODevice::ReadOnly)) {
        return;
    }
    QImageReader reader(&buf);
    reader.setAllocationLimit(256);
    QImage img = reader.read();
    if (img.isNull() || img.width() <= 0 || img.height() <= 0 ||
        img.width() > 16384 || img.height() > 16384 ||
        static_cast<uint64_t>(img.width()) * static_cast<uint64_t>(img.height()) > 64ULL * 1024ULL * 1024ULL) {
        return;
    }
    if (img.format() != QImage::Format_RGBA8888) {
        img = img.convertToFormat(QImage::Format_RGBA8888);
    }
    if (img.isNull()) {
        return;
    }

    auto parseDimCells = [&](const QByteArray &spec, int gridMax, int devCellPx) -> uint32_t {
        const QByteArray s = spec.trimmed().toLower();
        if (s.isEmpty() || s == "auto") {
            return 0;
        }
        if (s.endsWith("px")) {
            bool ok = false;
            const int px = s.left(s.size() - 2).toInt(&ok);
            if (!ok || px <= 0) {
                return 0;
            }
            return static_cast<uint32_t>(std::clamp((px + std::max(1, devCellPx) - 1) / std::max(1, devCellPx), 1, 1000));
        }
        if (s.endsWith('%')) {
            bool ok = false;
            const int pct = s.left(s.size() - 1).toInt(&ok);
            if (!ok || pct <= 0) {
                return 0;
            }
            return static_cast<uint32_t>(std::clamp((std::max(1, gridMax) * pct + 99) / 100, 1, 1000));
        }
        bool ok = false;
        const int cells = s.toInt(&ok);
        return (ok && cells > 0) ? static_cast<uint32_t>(std::clamp(cells, 1, 1000)) : 0U;
    };

    const int devCw = deviceCellWidth();
    const int devCh = deviceCellHeight();
    uint32_t reqCols = parseDimCells(widthSpec, m_cols, devCw);
    uint32_t reqRows = parseDimCells(heightSpec, m_rows, devCh);

    if (preserveAspect && reqCols > 0 && reqRows > 0) {
        const double boxPxW = static_cast<double>(reqCols) * static_cast<double>(devCw);
        const double boxPxH = static_cast<double>(reqRows) * static_cast<double>(devCh);
        const double imgAspect = static_cast<double>(img.width()) / static_cast<double>(img.height());
        if (boxPxW / boxPxH > imgAspect) {
            reqCols = static_cast<uint32_t>(std::max(1, static_cast<int>(std::lround((boxPxH * imgAspect) / devCw))));
        } else {
            reqRows = static_cast<uint32_t>(std::max(1, static_cast<int>(std::lround((boxPxW / imgAspect) / devCh))));
        }
    }

    const size_t rowBytes = static_cast<size_t>(img.width()) * 4ULL;
    QByteArray tightRgba(static_cast<qsizetype>(rowBytes * static_cast<size_t>(img.height())), Qt::Uninitialized);
    if (static_cast<size_t>(img.bytesPerLine()) == rowBytes) {
        std::memcpy(tightRgba.data(), img.constBits(), static_cast<size_t>(tightRgba.size()));
    } else {
        for (int y = 0; y < img.height(); ++y) {
            std::memcpy(tightRgba.data() + static_cast<qsizetype>(static_cast<size_t>(y) * rowBytes),
                        img.constScanLine(y),
                        rowBytes);
        }
    }

    const QByteArray rgbaB64 = tightRgba.toBase64();
    const uint32_t synthId = 0xF0000000U | (++m_nextOsc1337ImageSeq & 0x0FFFFFFFU);

    constexpr qsizetype kChunkSize = 4096;
    QByteArray apcStream;
    apcStream.reserve(rgbaB64.size() + 128);

    for (qsizetype offset = 0; offset < rgbaB64.size(); offset += kChunkSize) {
        const QByteArray slice = rgbaB64.mid(offset, kChunkSize);
        const bool more = (offset + kChunkSize) < rgbaB64.size();
        if (offset == 0) {
            apcStream.append("\x1b_Ga=T,f=32,s=");
            apcStream.append(QByteArray::number(img.width()));
            apcStream.append(",v=");
            apcStream.append(QByteArray::number(img.height()));
            apcStream.append(",i=");
            apcStream.append(QByteArray::number(synthId));
            apcStream.append(",q=2");
            if (reqCols > 0) {
                apcStream.append(",c=");
                apcStream.append(QByteArray::number(reqCols));
            }
            if (reqRows > 0) {
                apcStream.append(",r=");
                apcStream.append(QByteArray::number(reqRows));
            }
            if (doNotMoveCursor) {
                apcStream.append(",C=1");
            }
            apcStream.append(more ? ",m=1;" : ",m=0;");
        } else {
            apcStream.append(more ? "\x1b_Gm=1;" : "\x1b_Gm=0;");
        }
        apcStream.append(slice);
        apcStream.append("\x1b\\");
    }
    m_pendingOsc1337Seqs.push_back(std::move(apcStream));
}

// --- Static libghostty-vt C callbacks ---

void TerminalWidget::onWritePty(GhosttyTerminal /*terminal*/, void *userdata, const uint8_t *data, size_t len) {
    if (const auto *self = static_cast<TerminalWidget *>(userdata)) {
        self->flushPtyBytes(data, len);
    }
}

void TerminalWidget::onTitleChanged(GhosttyTerminal terminal, void *userdata) {
    auto *self = static_cast<TerminalWidget *>(userdata);
    if (!self) {
        return;
    }
    GhosttyString s{};
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_TITLE, &s) == GHOSTTY_SUCCESS && s.len > 0) {
        self->m_title = QString::fromUtf8(reinterpret_cast<const char *>(s.ptr), static_cast<qsizetype>(s.len));
        Q_EMIT self->titleChanged(self->m_title);
    }
}

void TerminalWidget::onPwdChanged(GhosttyTerminal terminal, void *userdata) {
    auto *self = static_cast<TerminalWidget *>(userdata);
    if (!self) {
        return;
    }
    GhosttyString s{};
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_PWD, &s) == GHOSTTY_SUCCESS && s.len > 0) {
        const QString raw = QString::fromUtf8(reinterpret_cast<const char *>(s.ptr), static_cast<qsizetype>(s.len));
        const QString parsed = GhosttyUnionLink::parseOsc7Pwd(raw);
        if (!parsed.isEmpty()) {
            self->m_pwd = parsed;
            Q_EMIT self->pwdChanged(self->m_pwd);
        }
    }
}

void TerminalWidget::onBell(GhosttyTerminal /*terminal*/, void *userdata) {
    if (const auto *self = static_cast<TerminalWidget *>(userdata)) {
        QApplication::beep();
        Q_UNUSED(self);
    }
}

bool TerminalWidget::onSizeQuery(GhosttyTerminal /*terminal*/, void *userdata, GhosttySizeReportSize *outSize) {
    const auto *self = static_cast<TerminalWidget *>(userdata);
    if (!self || !outSize) {
        return false;
    }
    outSize->rows = self->m_rows;
    outSize->columns = self->m_cols;
    outSize->cell_width = static_cast<uint32_t>(self->deviceCellWidth());
    outSize->cell_height = static_cast<uint32_t>(self->deviceCellHeight());
    return true;
}

bool TerminalWidget::onColorSchemeQuery(GhosttyTerminal /*terminal*/, void * /*userdata*/, GhosttyColorScheme *outScheme) {
    if (!outScheme) {
        return false;
    }
    *outScheme = GHOSTTY_COLOR_SCHEME_DARK;
    return true;
}

bool TerminalWidget::onDeviceAttributesQuery(GhosttyTerminal /*terminal*/, void * /*userdata*/, GhosttyDeviceAttributes *outAttrs) {
    if (!outAttrs) {
        return false;
    }
    *outAttrs = {};
    outAttrs->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT220;
    outAttrs->primary.features[0] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
    outAttrs->primary.features[1] = GHOSTTY_DA_FEATURE_ANSI_TEXT_LOCATOR;
    outAttrs->primary.features[2] = GHOSTTY_DA_FEATURE_CLIPBOARD;
    outAttrs->primary.num_features = 3;

    outAttrs->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT220;
    outAttrs->secondary.firmware_version = 100;
    outAttrs->secondary.rom_cartridge = 0;

    outAttrs->tertiary.unit_id = 0;
    return true;
}

GhosttyString TerminalWidget::onXtversionQuery(GhosttyTerminal /*terminal*/, void * /*userdata*/) {
    static constexpr const char kVer[] = "ghostty-union 0.1.0";
    return GhosttyString{reinterpret_cast<const uint8_t *>(kVer), sizeof(kVer) - 1};
}

void TerminalWidget::onClipboardWrite(GhosttyTerminal /*terminal*/, void * /*userdata*/, const GhosttyClipboardWrite *write) {
    if (!write) {
        return;
    }
    GhosttyClipboardWriteReply reply = GHOSTTY_INIT_SIZED(GhosttyClipboardWriteReply);
    reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_UNSUPPORTED;

    if (QClipboard *cb = QApplication::clipboard()) {
        const QClipboard::Mode mode = (write->location == GHOSTTY_CLIPBOARD_LOCATION_SELECTION ||
                                       write->location == GHOSTTY_CLIPBOARD_LOCATION_PRIMARY)
                                          ? QClipboard::Selection
                                          : QClipboard::Clipboard;
        if (write->contents_len == 0) {
            cb->clear(mode);
            reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS;
        } else {
            for (size_t i = 0; i < write->contents_len; ++i) {
                const QByteArray mime = QByteArray::fromRawData(
                    reinterpret_cast<const char *>(write->contents[i].mime.ptr),
                    static_cast<qsizetype>(write->contents[i].mime.len));
                if (mime.startsWith("text/")) {
                    const QString text = QString::fromUtf8(
                        reinterpret_cast<const char *>(write->contents[i].data.ptr),
                        static_cast<qsizetype>(write->contents[i].data.len));
                    cb->setText(text, mode);
                    reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS;
                    break;
                }
            }
        }
    }
    if (write->reply) {
        write->reply(write, &reply);
    }
}

void TerminalWidget::onRenderHold(GhosttyTerminal terminal, void *userdata, bool held) {
    auto *self = static_cast<TerminalWidget *>(userdata);
    if (!self) {
        return;
    }
    if (held) {
        ghostty_render_state_update(self->m_renderState, terminal);
        self->m_renderHeld = true;
        self->m_renderHoldTimer.start();
    } else {
        self->m_renderHeld = false;
        self->m_renderHoldTimer.stop();
    }
}

void TerminalWidget::onReset(GhosttyTerminal /*terminal*/, void *userdata) {
    auto *self = static_cast<TerminalWidget *>(userdata);
    if (!self) {
        return;
    }
    self->m_hoveredLink = {};
    self->m_pressedLink = {};
    self->m_hoveredImage = {};
    self->m_pressedImage = {};
    self->m_linkClickPending = false;
    self->m_imageClickPending = false;
    self->m_renderHeld = false;
    self->m_pendingCommandEndSanitize = false;
    self->m_oscCaptureState = OscCaptureState::Idle;
    self->m_osc1337Buffer.clear();
    self->m_pendingOsc1337Seqs.clear();
    self->m_visiblePlacements.clear();
    self->m_textureCache.clear();
    self->m_lastPlacementsGen = 0;
    self->m_lastKittyStorageGen = 0;
    self->m_placementsInitialized = false;
    self->m_kittyAnimTimer.stop();
    self->m_renderHoldTimer.stop();
    self->m_blinkVisible = true;
    self->m_rowCache.clear();
}

void TerminalWidget::onSemanticPrompt(GhosttyTerminal /*terminal*/,
                                      void *userdata,
                                      const GhosttyTerminalSemanticPrompt *event) {
    auto *self = static_cast<TerminalWidget *>(userdata);
    if (!self || !event) {
        return;
    }
    if (event->kind == GHOSTTY_SEMANTIC_PROMPT_COMMAND_END) {
        self->m_pendingCommandEndSanitize = true;
    }
}

void TerminalWidget::onDesktopNotification(GhosttyTerminal /*terminal*/,
                                           void * /*userdata*/,
                                           const GhosttyTerminalDesktopNotification *notification) {
    if (!notification) {
        return;
    }
    const QString title = sanitizeNotificationText(
        notification->title.len > 0
            ? QString::fromUtf8(reinterpret_cast<const char *>(notification->title.ptr),
                                static_cast<qsizetype>(notification->title.len))
            : QStringLiteral("ghostty-union"));
    const QString body = sanitizeNotificationText(
        notification->body.len > 0
            ? QString::fromUtf8(reinterpret_cast<const char *>(notification->body.ptr),
                                static_cast<qsizetype>(notification->body.len))
            : QString());
    if (body.isEmpty() && title.isEmpty()) {
        return;
    }

    QDBusInterface notifyInterface(QStringLiteral("org.freedesktop.Notifications"),
                                   QStringLiteral("/org/freedesktop/Notifications"),
                                   QStringLiteral("org.freedesktop.Notifications"),
                                   QDBusConnection::sessionBus());
    if (!notifyInterface.isValid()) {
        return;
    }
    notifyInterface.call(QDBus::NoBlock,
                         QStringLiteral("Notify"),
                         QStringLiteral("ghostty-union"),
                         static_cast<quint32>(0),
                         QStringLiteral("com.mitchellh.ghostty"),
                         title,
                         body,
                         QStringList{},
                         QVariantMap{},
                         static_cast<qint32>(-1));
}
