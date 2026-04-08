/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef VIBE_POINTS_CARD_H
#define VIBE_POINTS_CARD_H

#include <QFrame>
#include "vibetypes.h"

class QVBoxLayout;

namespace Vibe
{

class SummaryCard;

class PointsCard : public QFrame
{
    Q_OBJECT

public:
    explicit PointsCard(QWidget *parent = nullptr);

    void setPoints(const QList<PointData> &points);
    void setLeftColumn(bool isLeft);

    void updatePosition(const QRect &uncroppedGeometry, const SummaryCard *summaryCard, double scaleFactor, const QPoint &viewportOffset = QPoint(0, 0));
    void reloadFontConfig();

    bool isLeftColumn() const { return m_isLeftColumn; }

private:
    QVBoxLayout *m_layout;
    bool m_isLeftColumn = true;
    int m_baseFontSize = 7;
    double m_lastScaleFactor = -1;
};

} // namespace Vibe

#endif // VIBE_POINTS_CARD_H
