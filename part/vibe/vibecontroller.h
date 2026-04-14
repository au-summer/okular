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
    void retryCurrentPage();
    void parseAllPages();
    void retryAllPages();
    void toggleCardsVisible();
    void reloadConfig();

private Q_SLOTS:
    void onDocumentReady(const QMap<int, QList<ParagraphData>> &pagesParagraphs);
    void onMinerUError(const QString &error);
    void onMinerUProgress(const QString &status);
    void onPageSummaryReady(int pageIdx, const QList<ParagraphLlmResult> &results);
    void onAllPagesSummaryReady(const QMap<int, QList<ParagraphLlmResult>> &results);
    void onLlmError(const QString &error);
    void onPageLlmError(int pageIdx, const QString &error);

private:
    bool loadCachedCardsForPage(int pageIdx);
    void ensurePaperId();
    void ensurePdfData();
    void processPageWithLlm(int pageIdx, const QList<ParagraphData> &paragraphs);
    QMap<int, QList<ParagraphData>> getAllParagraphs() const;
    void createCardsForPage(int pageIdx, const QList<ParagraphData> &paragraphs);
    void clearCardsForPage(int pageIdx);
    void dispatchQueuedPages();

    static constexpr int MAX_CONCURRENCY = 5;
    static constexpr int MAX_RETRIES = 2;

    Okular::Document *m_document;
    PageView *m_pageView;
    VibeDB m_db;
    LlmClient *m_llmClient;
    MinerUClient *m_mineruClient;
    int m_currentPaperId = -1;
    int m_pendingMinerUPageIdx = -1;

    // Concurrent LLM request tracking
    QMap<int, QList<ParagraphData>> m_inFlightPages;
    QMap<int, int> m_retryCount;
    int m_activeRequestCount = 0;

    // Parse-all-pages state
    QList<int> m_allPagesQueue;
    bool m_parsingAllPages = false;
    bool m_cardsVisible = false;
    QString m_language = QStringLiteral("en");
    QString m_processingMode = QStringLiteral("batch");
};

} // namespace Vibe

#endif // VIBE_CONTROLLER_H
