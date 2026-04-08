/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef VIBE_CONTROLLER_H
#define VIBE_CONTROLLER_H

#include <QMap>
#include <QObject>

#include "vibedb.h"
#include "vibetypes.h"

class PageView;

namespace Okular
{
class Document;
class Page;
}

namespace Vibe
{

class LlmClient;
class MinerUClient;
class SummaryCard;
class PointsCard;

class VibeController : public QObject
{
    Q_OBJECT

public:
    VibeController(Okular::Document *doc, PageView *pageView, QObject *parent = nullptr);
    ~VibeController() override;

public Q_SLOTS:
    void parseCurrentPage();
    void reloadConfig();

private Q_SLOTS:
    void onDocumentReady(const QMap<int, QList<ParagraphData>> &pagesParagraphs);
    void onMinerUError(const QString &error);
    void onMinerUProgress(const QString &status);
    void onPageSummaryReady(int pageIdx, const QList<ParagraphLlmResult> &results);
    void onLlmError(const QString &error);

private:
    void processPageWithLlm(int pageIdx, const QList<ParagraphData> &paragraphs);
    void createCardsForPage(int pageIdx, const QList<ParagraphData> &paragraphs);
    void clearCardsForPage(int pageIdx);

    Okular::Document *m_document;
    PageView *m_pageView;
    VibeDB m_db;
    LlmClient *m_llmClient;
    MinerUClient *m_mineruClient;
    int m_currentPaperId = -1;
    int m_pendingLlmPageIdx = -1;

    // Paragraphs waiting for LLM results
    QList<ParagraphData> m_pendingParagraphs;
};

} // namespace Vibe

#endif // VIBE_CONTROLLER_H
