/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "summarycard.h"
#include "pointscard.h"

#include <QLabel>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QtMath>

using namespace Vibe;

SummaryCard::SummaryCard(QWidget *parent)
    : QFrame(parent)
{
    setFrameStyle(QFrame::StyledPanel | QFrame::Plain);
    setStyleSheet(QStringLiteral(
        "Vibe--SummaryCard {"
        "  background-color: white;"
        "  border: 1px solid #e0e0e0;"
        "  border-radius: 6px;"
        "  padding: 4px 6px;"
        "}"
    ));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 3, 4, 3);
    layout->setSpacing(0);

    m_label = new QLabel(this);
    m_label->setWordWrap(true);
    m_label->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 11px; color: #333;"));
    m_label->setText(tr("Summarizing..."));
    layout->addWidget(m_label);

    setMaximumWidth(300);
    setMinimumWidth(80);
    setCursor(Qt::PointingHandCursor);
}

void SummaryCard::setSummary(const QString &summary)
{
    m_label->setText(summary);
    adjustSize();
}

void SummaryCard::setLoading(bool loading)
{
    if (loading) {
        m_label->setText(tr("Summarizing..."));
        m_label->setStyleSheet(QStringLiteral("font-style: italic; font-size: 11px; color: #999;"));
    } else {
        m_label->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 11px; color: #333;"));
    }
}

void SummaryCard::setAnchorRect(const Okular::NormalizedRect &rect)
{
    m_anchorRect = rect;
}

void SummaryCard::setLeftColumn(bool isLeft)
{
    m_isLeftColumn = isLeft;
}

void SummaryCard::setPointsCard(PointsCard *card)
{
    m_pointsCard = card;
}

void SummaryCard::updatePosition(const QRect &uncroppedGeometry, const QPoint &viewportOffset)
{
    const int gap = 4;

    adjustSize();
    int cardWidth = sizeHint().width();

    int y = uncroppedGeometry.y() + qRound(m_anchorRect.top * uncroppedGeometry.height()) - viewportOffset.y();

    int x;
    if (m_isLeftColumn) {
        x = uncroppedGeometry.x() + qRound(m_anchorRect.left * uncroppedGeometry.width()) - cardWidth - gap - viewportOffset.x();
    } else {
        x = uncroppedGeometry.x() + qRound(m_anchorRect.right * uncroppedGeometry.width()) + gap - viewportOffset.x();
    }

    move(x, y);
}

void SummaryCard::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_pointsCard) {
        m_pointsCard->setVisible(!m_pointsCard->isVisible());
    }
    QFrame::mousePressEvent(event);
}
