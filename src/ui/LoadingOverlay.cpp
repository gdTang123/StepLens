#include "ui/LoadingOverlay.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFont>
#include <QFrame>
#include <QLabel>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace steplens {
namespace {

constexpr int kCardMinimumWidth = 320;

QStringList localFiles(const QMimeData* mime) {
    QStringList files;
    if (mime == nullptr || !mime->hasUrls()) {
        return files;
    }
    const QList<QUrl> urls = mime->urls();
    for (const QUrl& url : urls) {
        if (url.isLocalFile()) {
            files.push_back(url.toLocalFile());
        }
    }
    return files;
}

} // namespace

LoadingOverlay::LoadingOverlay(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background-color: rgba(12, 16, 22, 150);"));
    setAcceptDrops(true);

    m_card = new QFrame(this);
    m_card->setAutoFillBackground(true);
    m_card->setStyleSheet(QStringLiteral("QFrame { background-color: #f4f6f8; }"
                                         "QLabel { background: transparent; color: #1c1f24; }"
                                         "QPushButton { color: #1c1f24; }"));

    auto* layout = new QVBoxLayout(m_card);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(10);

    m_title = new QLabel(m_card);
    QFont titleFont = font();
    if (titleFont.pointSizeF() > 0.0) {
        titleFont.setPointSizeF(titleFont.pointSizeF() + 2.0);
    }
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    m_title->setWordWrap(true);

    m_detail = new QLabel(m_card);
    m_detail->setWordWrap(true);

    m_bar = new QProgressBar(m_card);
    m_bar->setRange(0, 1000);
    m_bar->setTextVisible(false);

    m_cancel = new QPushButton(tr("取消"), m_card);
    connect(m_cancel, &QPushButton::clicked, this, &LoadingOverlay::cancelRequested);

    layout->addWidget(m_title);
    layout->addWidget(m_detail);
    layout->addWidget(m_bar);
    layout->addWidget(m_cancel);
    m_card->setMinimumWidth(kCardMinimumWidth);
}

void LoadingOverlay::setStatus(const QString& title, const QString& detail, float fraction,
                               bool indeterminate, bool cancelEnabled) {
    m_title->setText(title);
    m_detail->setText(detail);
    m_detail->setVisible(!detail.isEmpty());
    m_cancel->setEnabled(cancelEnabled);
    m_cancel->setVisible(cancelEnabled);
    if (indeterminate) {
        m_bar->setRange(0, 0);
    } else {
        float clamped = fraction;
        if (!(clamped > 0.f)) {
            clamped = 0.f;
        }
        if (clamped > 1.f) {
            clamped = 1.f;
        }
        m_bar->setRange(0, 1000);
        m_bar->setValue(static_cast<int>(clamped * 1000.f));
    }
    m_card->adjustSize();
    placeCard();
}

void LoadingOverlay::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    placeCard();
}

void LoadingOverlay::placeCard() {
    m_card->adjustSize();
    const int cardWidth = std::max(m_card->sizeHint().width(), kCardMinimumWidth);
    const int cardHeight = std::max(m_card->sizeHint().height(), m_card->height());
    m_card->resize(cardWidth, cardHeight);
    const int x = std::max(0, (width() - m_card->width()) / 2);
    const int y = std::max(0, (height() - m_card->height()) / 2);
    m_card->move(x, y);
}

void LoadingOverlay::dragEnterEvent(QDragEnterEvent* event) {
    if (!localFiles(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void LoadingOverlay::dropEvent(QDropEvent* event) {
    const QStringList files = localFiles(event->mimeData());
    if (files.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    emit filesDropped(files);
}

} // namespace steplens
