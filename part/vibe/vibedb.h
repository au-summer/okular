/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef VIBE_DB_H
#define VIBE_DB_H

#include <QList>
#include <QString>
#include <QSqlDatabase>

#include "vibetypes.h"

namespace Vibe
{

class VibeDB
{
public:
    VibeDB();
    ~VibeDB();

    bool open();
    void close();

    int getOrCreatePaper(const QString &fileHash);
    void saveParagraphs(int paperId, int pageIdx, const QList<ParagraphData> &paragraphs);
    QList<ParagraphData> getParagraphs(int paperId, int pageIdx) const;

    void savePoints(int paragraphId, const QString &language, const QList<PointData> &points);
    QList<PointData> getPoints(int paragraphId, const QString &language) const;

    void saveSummaryCard(int paperId, int pageIdx, int paragraphId, const Okular::NormalizedRect &anchor, bool isLeftColumn, const QString &language, const QString &paragraphSummary);
    QList<SummaryCardData> getSummaryCards(int paperId, int pageIdx, const QString &language) const;

    int getParagraphDbId(int paperId, int pageIdx, int paragraphIdx) const;
    void deleteLlmDataForPage(int paperId, int pageIdx, const QString &language);

private:
    void createTables();

    QSqlDatabase m_db;
    QString m_dbPath;
    bool m_open = false;
};

} // namespace Vibe

#endif // VIBE_DB_H
