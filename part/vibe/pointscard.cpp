/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "pointscard.h"
#include "summarycard.h"

#include <QLabel>
#include <QSettings>
#include <QVBoxLayout>

using namespace Vibe;

PointsCard::PointsCard(QWidget *parent)
    : QFrame(parent)
{
    setFrameStyle(QFrame::StyledPanel | QFrame::Plain);
    setStyleSheet(QStringLiteral(
        "Vibe--PointsCard {"
        "  background-color: white;"
        "  border: 1px solid #d0d0d0;"
        "  border-radius: 6px;"
        "  padding: 4px 6px;"
        "}"
    ));

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(4, 3, 4, 3);
    m_layout->setSpacing(2);

    setMaximumWidth(300);
    setMinimumWidth(80);
}

void PointsCard::setPoints(const QList<PointData> &points)
{
    // Clear existing labels
    QLayoutItem *item;
    while ((item = m_layout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    for (const auto &pt : points) {
        auto *label = new QLabel(this);
        label->setWordWrap(true);
        label->setText(QStringLiteral("\u00b7 ") + pt.summary);
        label->setStyleSheet(QStringLiteral("font-size: 10px; color: #555; padding: 1px 0;"));
        m_layout->addWidget(label);
    }

    adjustSize();
}

void PointsCard::setLeftColumn(bool isLeft)
{
    m_isLeftColumn = isLeft;
}

void PointsCard::updatePosition(const QRect &uncroppedGeometry, const SummaryCard *summaryCard, double scaleFactor, const QPoint &viewportOffset)
{
    Q_UNUSED(uncroppedGeometry)
    Q_UNUSED(viewportOffset)

    // Scale font and dimensions with zoom
    QSettings settings(QStringLiteral("okular-vibe"), QStringLiteral("okular-vibe"));
    const int baseFontSize = settings.value(QStringLiteral("pointsFontSize"), 7).toInt();
    const int fontSize = qMax(6, qRound(baseFontSize * scaleFactor));
    const int padding = qMax(2, qRound(4 * scaleFactor));
    const int maxW = qRound(300 * scaleFactor);
    const int minW = qMax(40, qRound(80 * scaleFactor));

    // Update all child labels' font size
    for (int i = 0; i < m_layout->count(); ++i) {
        if (auto *label = qobject_cast<QLabel *>(m_layout->itemAt(i)->widget())) {
            label->setStyleSheet(QStringLiteral("font-size: %1px; color: #555; padding: 1px 0;").arg(fontSize));
        }
    }
    setContentsMargins(padding, padding, padding, padding);
    setMaximumWidth(maxW);
    setMinimumWidth(minW);
    adjustSize();

    int cardWidth = sizeHint().width();
    const int gap = qRound(4 * scaleFactor);

    int y = summaryCard->y();

    int x;
    if (m_isLeftColumn) {
        x = summaryCard->x() - cardWidth - gap;
    } else {
        x = summaryCard->x() + summaryCard->width() + gap;
    }

    move(x, y);
}
