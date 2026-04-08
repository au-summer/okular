/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef VIBE_TYPES_H
#define VIBE_TYPES_H

#include <QList>
#include <QString>
#include "core/area.h"

namespace Vibe
{

struct PointData {
    int pointIdx = 0;
    QString summary;
    QList<int> sentenceIndices;
    int importanceLevel = 1;
};

struct ParagraphData {
    int paragraphIdx = 0;
    QString text;
    QString summary;
    QList<PointData> points;
    Okular::NormalizedRect bbox;
    bool isLeftColumn = true;
};

struct SummaryCardData {
    int id = -1;
    int paragraphId = -1;
    int pageIdx = 0;
    int paragraphIdx = 0;
    QString paragraphSummary;
    QList<PointData> points;
    Okular::NormalizedRect anchorRect;
    bool isLeftColumn = true;
};

struct ParagraphLlmResult {
    int paragraphIdx = 0;
    QString paragraphSummary;
    QList<PointData> points;
};

} // namespace Vibe

#endif // VIBE_TYPES_H
