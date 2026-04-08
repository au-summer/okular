/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef VIBE_SUMMARY_CARD_H
#define VIBE_SUMMARY_CARD_H

#include <QFrame>
#include "vibetypes.h"

class QLabel;

namespace Vibe
{

class PointsCard;

class SummaryCard : public QFrame
{
    Q_OBJECT

public:
    explicit SummaryCard(QWidget *parent = nullptr);

    void setSummary(const QString &summary);
    void setLoading(bool loading);
    void setAnchorRect(const Okular::NormalizedRect &rect);
    void setLeftColumn(bool isLeft);
    void setPointsCard(PointsCard *card);

    void updatePosition(const QRect &uncroppedGeometry, double scaleFactor, const QPoint &viewportOffset = QPoint(0, 0));

    bool isLeftColumn() const { return m_isLeftColumn; }
    const Okular::NormalizedRect &anchorRect() const { return m_anchorRect; }

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    QLabel *m_label;
    PointsCard *m_pointsCard = nullptr;
    Okular::NormalizedRect m_anchorRect;
    bool m_isLeftColumn = true;
    bool m_loading = false;
};

} // namespace Vibe

#endif // VIBE_SUMMARY_CARD_H
