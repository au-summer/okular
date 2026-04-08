/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vibecontroller.h"

#include <QDebug>
#include <QMessageBox>
#include <QScrollBar>

#include "llmclient.h"
#include "mineruclient.h"
#include "summarycard.h"
#include "pointscard.h"

#include <QSettings>

#include "core/document.h"
#include "core/page.h"
#include "part/pageview.h"
#include "part/pageviewutils.h"

using namespace Vibe;

VibeController::VibeController(Okular::Document *doc, PageView *pageView, QObject *parent)
    : QObject(parent)
    , m_document(doc)
    , m_pageView(pageView)
    , m_llmClient(new LlmClient(this))
    , m_mineruClient(new MinerUClient(this))
{
    connect(m_llmClient, &LlmClient::pageSummaryReady, this, &VibeController::onPageSummaryReady);
    connect(m_llmClient, &LlmClient::errorOccurred, this, &VibeController::onLlmError);

    connect(m_mineruClient, &MinerUClient::documentReady, this, &VibeController::onDocumentReady);
    connect(m_mineruClient, &MinerUClient::errorOccurred, this, &VibeController::onMinerUError);
    connect(m_mineruClient, &MinerUClient::progressChanged, this, &VibeController::onMinerUProgress);

    m_db.open();
}

VibeController::~VibeController()
{
    m_db.close();
}

void VibeController::toggleCardsVisible(bool visible)
{
    m_cardsVisible = visible;
    const auto items = m_pageView->items();
    for (auto *item : items) {
        item->setVibeCardsVisible(visible);
    }
}

void VibeController::reloadConfig()
{
    QSettings settings(QStringLiteral("okular-vibe"), QStringLiteral("okular-vibe"));

    // Reload LLM config
    LlmClient::Config llmConfig;
    llmConfig.apiKey = settings.value(QStringLiteral("apiKey")).toString().trimmed();
    const QString model = settings.value(QStringLiteral("model")).toString().trimmed();
    if (!model.isEmpty()) {
        llmConfig.model = model;
    }
    const QString baseUrl = settings.value(QStringLiteral("baseUrl")).toString().trimmed();
    if (!baseUrl.isEmpty()) {
        llmConfig.baseUrl = QUrl(baseUrl);
    }
    m_llmClient->setConfig(llmConfig);

    // Reload MinerU config
    MinerUClient::Config mineruConfig;
    mineruConfig.apiToken = settings.value(QStringLiteral("mineruToken")).toString().trimmed();
    m_mineruClient->setConfig(mineruConfig);

    qDebug() << "[VibeController] Config reloaded, model:" << llmConfig.model;
}

void VibeController::parseCurrentPage()
{
    if (!m_document || m_document->pages() == 0) {
        return;
    }

    if (m_parsingAllPages) {
        m_pageView->displayMessage(QStringLiteral("Please wait, parsing all pages..."));
        return;
    }

    const int pageIdx = m_document->currentPage();

    // Ensure paper is registered in DB
    const QString filePath = m_document->currentDocument().toLocalFile();
    if (!filePath.isEmpty() && m_currentPaperId < 0) {
        m_currentPaperId = m_db.getOrCreatePaper(filePath);
    }

    // Check if we already have summary cards for this page (fully processed)
    if (m_currentPaperId >= 0) {
        QList<SummaryCardData> existingCards = m_db.getSummaryCards(m_currentPaperId, pageIdx);
        if (!existingCards.isEmpty()) {
            // Rebuild from DB
            QList<ParagraphData> paragraphs;
            for (const auto &card : existingCards) {
                ParagraphData p;
                p.paragraphIdx = card.paragraphIdx;
                p.summary = card.paragraphSummary;
                p.points = card.points;
                p.bbox = card.anchorRect;
                p.isLeftColumn = card.isLeftColumn;
                paragraphs.append(p);
            }
            clearCardsForPage(pageIdx);
            createCardsForPage(pageIdx, paragraphs);
            qDebug() << "[VibeController] Loaded" << existingCards.size() << "existing cards for page" << pageIdx;
            return;
        }
    }

    // Check if we have MinerU paragraphs for this page (parsed but not yet LLM-processed)
    if (m_currentPaperId >= 0) {
        QList<ParagraphData> dbParagraphs = m_db.getParagraphs(m_currentPaperId, pageIdx);
        if (!dbParagraphs.isEmpty()) {
            // We have paragraphs from MinerU, send to LLM
            processPageWithLlm(pageIdx, dbParagraphs);
            return;
        }

        // Check if other pages have MinerU data (document was already parsed)
        // If so, this page simply has no text content
        bool anyPageHasData = false;
        for (uint p = 0; p < m_document->pages(); ++p) {
            if (!m_db.getParagraphs(m_currentPaperId, p).isEmpty()) {
                anyPageHasData = true;
                break;
            }
        }
        if (anyPageHasData) {
            qDebug() << "[VibeController] Page" << pageIdx << "has no text content from MinerU";
            m_pageView->displayMessage(QStringLiteral("No text content on this page."));
            return;
        }
    }

    // No MinerU data at all — need to parse the whole document
    if (filePath.isEmpty()) {
        qWarning() << "[VibeController] Cannot get local file path for MinerU parsing";
        return;
    }

    m_pendingLlmPageIdx = pageIdx;
    m_pageView->displayMessage(QStringLiteral("Starting MinerU document parsing..."));
    m_mineruClient->parseDocument(filePath);
}

void VibeController::retryCurrentPageSummary()
{
    if (!m_document || m_document->pages() == 0) {
        return;
    }

    if (m_parsingAllPages) {
        m_pageView->displayMessage(QStringLiteral("Please wait, parsing all pages..."));
        return;
    }

    const int pageIdx = m_document->currentPage();

    const QString filePath = m_document->currentDocument().toLocalFile();
    if (!filePath.isEmpty() && m_currentPaperId < 0) {
        m_currentPaperId = m_db.getOrCreatePaper(filePath);
    }

    if (m_currentPaperId < 0) {
        m_pageView->displayMessage(QStringLiteral("No parsed data. Run Parse Current Page first."));
        return;
    }

    QList<ParagraphData> paragraphs = m_db.getParagraphs(m_currentPaperId, pageIdx);
    if (paragraphs.isEmpty()) {
        m_pageView->displayMessage(QStringLiteral("No parsed data for this page. Run Parse Current Page first."));
        return;
    }

    // Delete LLM data from DB, keep MinerU paragraphs
    m_db.deleteLlmDataForPage(m_currentPaperId, pageIdx);

    // Clear summaries and points on the loaded data
    for (auto &p : paragraphs) {
        p.summary.clear();
        p.points.clear();
    }

    clearCardsForPage(pageIdx);
    processPageWithLlm(pageIdx, paragraphs);
}

void VibeController::parseAllPages()
{
    if (!m_document || m_document->pages() == 0) {
        return;
    }

    if (m_parsingAllPages) {
        m_pageView->displayMessage(QStringLiteral("Already parsing all pages..."));
        return;
    }

    const QString filePath = m_document->currentDocument().toLocalFile();
    if (!filePath.isEmpty() && m_currentPaperId < 0) {
        m_currentPaperId = m_db.getOrCreatePaper(filePath);
    }

    // Check if MinerU data exists
    bool anyPageHasData = false;
    if (m_currentPaperId >= 0) {
        for (uint p = 0; p < m_document->pages(); ++p) {
            if (!m_db.getParagraphs(m_currentPaperId, p).isEmpty()) {
                anyPageHasData = true;
                break;
            }
        }
    }

    if (!anyPageHasData) {
        // Need MinerU parse first, then parse all pages
        if (filePath.isEmpty()) {
            qWarning() << "[VibeController] Cannot get local file path for MinerU parsing";
            return;
        }
        m_parsingAllPages = true;
        m_pendingLlmPageIdx = -1;
        m_pageView->displayMessage(QStringLiteral("Starting MinerU document parsing (all pages)..."));
        m_mineruClient->parseDocument(filePath);
        return;
    }

    // MinerU data exists, build queue of pages without summary cards
    m_allPagesQueue.clear();
    for (uint p = 0; p < m_document->pages(); ++p) {
        if (!m_db.getParagraphs(m_currentPaperId, p).isEmpty()
            && m_db.getSummaryCards(m_currentPaperId, p).isEmpty()) {
            m_allPagesQueue.append(p);
        }
    }

    if (m_allPagesQueue.isEmpty()) {
        m_pageView->displayMessage(QStringLiteral("All pages already processed."));
        return;
    }

    m_parsingAllPages = true;
    m_pageView->displayMessage(QStringLiteral("Parsing all pages: %1 pages remaining...").arg(m_allPagesQueue.size()));
    processNextQueuedPage();
}

void VibeController::processNextQueuedPage()
{
    while (!m_allPagesQueue.isEmpty()) {
        int pageIdx = m_allPagesQueue.takeFirst();
        QList<ParagraphData> paragraphs = m_db.getParagraphs(m_currentPaperId, pageIdx);
        if (!paragraphs.isEmpty()) {
            m_pageView->displayMessage(QStringLiteral("Parsing page %1 (%2 remaining)...")
                .arg(pageIdx + 1).arg(m_allPagesQueue.size()));
            processPageWithLlm(pageIdx, paragraphs);
            return;
        }
    }

    // Queue exhausted
    m_parsingAllPages = false;
    m_pageView->displayMessage(QStringLiteral("All pages processed."));
}

void VibeController::onDocumentReady(const QMap<int, QList<ParagraphData>> &pagesParagraphs)
{
    qDebug() << "[VibeController] MinerU parsed" << pagesParagraphs.size() << "pages";

    // Store all paragraphs in DB
    if (m_currentPaperId >= 0) {
        for (auto it = pagesParagraphs.constBegin(); it != pagesParagraphs.constEnd(); ++it) {
            m_db.saveParagraphs(m_currentPaperId, it.key(), it.value());
        }
    }

    // If parsing all pages, build the queue and start
    if (m_parsingAllPages) {
        m_allPagesQueue.clear();
        for (auto it = pagesParagraphs.constBegin(); it != pagesParagraphs.constEnd(); ++it) {
            if (!it.value().isEmpty() && m_db.getSummaryCards(m_currentPaperId, it.key()).isEmpty()) {
                m_allPagesQueue.append(it.key());
            }
        }
        std::sort(m_allPagesQueue.begin(), m_allPagesQueue.end());
        m_pageView->displayMessage(QStringLiteral("Parsing all pages: %1 pages to process...").arg(m_allPagesQueue.size()));
        processNextQueuedPage();
        return;
    }

    // Otherwise process the single page the user originally requested
    if (m_pendingLlmPageIdx >= 0 && pagesParagraphs.contains(m_pendingLlmPageIdx)) {
        processPageWithLlm(m_pendingLlmPageIdx, pagesParagraphs[m_pendingLlmPageIdx]);
    } else if (m_pendingLlmPageIdx >= 0) {
        qDebug() << "[VibeController] Page" << m_pendingLlmPageIdx << "has no text content from MinerU";
        m_pageView->displayMessage(QStringLiteral("No text content on this page."));
    }
    m_pendingLlmPageIdx = -1;
}

void VibeController::onMinerUError(const QString &error)
{
    qWarning() << "[VibeController] MinerU error:" << error;
    QMessageBox::warning(m_pageView, tr("MinerU Error"), error);
    m_pendingLlmPageIdx = -1;
    if (m_parsingAllPages) {
        m_parsingAllPages = false;
        m_allPagesQueue.clear();
    }
}

void VibeController::onMinerUProgress(const QString &status)
{
    m_pageView->displayMessage(status);
}

void VibeController::processPageWithLlm(int pageIdx, const QList<ParagraphData> &paragraphs)
{
    qDebug() << "[VibeController] Sending" << paragraphs.size() << "paragraphs from page" << pageIdx << "to LLM";

    m_pendingParagraphs = paragraphs;
    m_pendingLlmPageIdx = pageIdx;

    // Show loading cards
    clearCardsForPage(pageIdx);
    createCardsForPage(pageIdx, paragraphs); // will show "Summarizing..." since summary is empty

    // Send to LLM
    m_llmClient->requestPageSummary(pageIdx, paragraphs);
}

void VibeController::onPageSummaryReady(int pageIdx, const QList<ParagraphLlmResult> &results)
{
    qDebug() << "[VibeController] Received LLM results for page" << pageIdx << ":" << results.size() << "paragraphs";

    // Merge LLM results with pending paragraphs
    for (auto &para : m_pendingParagraphs) {
        for (const auto &r : results) {
            if (r.paragraphIdx == para.paragraphIdx) {
                para.summary = r.paragraphSummary;
                para.points = r.points;
                break;
            }
        }
    }

    // Save to DB
    if (m_currentPaperId >= 0) {
        m_db.saveParagraphs(m_currentPaperId, pageIdx, m_pendingParagraphs);

        for (const auto &para : m_pendingParagraphs) {
            if (!para.summary.isEmpty()) {
                int paragraphDbId = m_db.getParagraphDbId(m_currentPaperId, pageIdx, para.paragraphIdx);
                if (paragraphDbId >= 0) {
                    m_db.savePoints(paragraphDbId, para.points);
                    m_db.saveSummaryCard(m_currentPaperId, pageIdx, paragraphDbId, para.bbox, para.isLeftColumn);
                }
            }
        }
    }

    // Update the cards
    clearCardsForPage(pageIdx);
    createCardsForPage(pageIdx, m_pendingParagraphs);
    m_pendingParagraphs.clear();
    m_pendingLlmPageIdx = -1;

    // Continue processing queued pages
    if (m_parsingAllPages) {
        processNextQueuedPage();
    }
}

void VibeController::onLlmError(const QString &error)
{
    qWarning() << "[VibeController] LLM error:" << error;
    QMessageBox::warning(m_pageView, tr("AI Parse Error"), error);
    m_pendingLlmPageIdx = -1;
    if (m_parsingAllPages) {
        m_parsingAllPages = false;
        m_allPagesQueue.clear();
    }
}

void VibeController::createCardsForPage(int pageIdx, const QList<ParagraphData> &paragraphs)
{
    const auto items = m_pageView->items();
    PageViewItem *targetItem = nullptr;
    for (const auto &item : items) {
        if (item->pageNumber() == pageIdx) {
            targetItem = item;
            break;
        }
    }

    if (!targetItem) {
        qWarning() << "[VibeController] No PageViewItem for page" << pageIdx;
        return;
    }

    QWidget *viewport = m_pageView->viewport();
    const QRect &uncroppedGeo = targetItem->uncroppedGeometry();
    const double zoom = targetItem->zoomFactor();
    const QPoint vpOffset(m_pageView->horizontalScrollBar()->value(), m_pageView->verticalScrollBar()->value());

    for (const auto &para : paragraphs) {
        auto *summaryCard = new SummaryCard(viewport);
        summaryCard->setAnchorRect(para.bbox);
        summaryCard->setLeftColumn(para.isLeftColumn);

        if (para.summary.isEmpty()) {
            summaryCard->setLoading(true);
        } else {
            summaryCard->setLoading(false);
            summaryCard->setSummary(para.summary);
        }

        auto *pointsCard = new PointsCard(viewport);
        pointsCard->setLeftColumn(para.isLeftColumn);

        if (!para.points.isEmpty()) {
            pointsCard->setPoints(para.points);
        } else {
            pointsCard->setVisible(false);
        }

        summaryCard->setPointsCard(pointsCard);

        summaryCard->updatePosition(uncroppedGeo, zoom, vpOffset);
        pointsCard->updatePosition(uncroppedGeo, summaryCard, zoom, vpOffset);

        targetItem->addVibeCardPair(summaryCard, pointsCard);

        summaryCard->show();
        if (!para.points.isEmpty()) {
            pointsCard->show();
        }
    }
}

void VibeController::clearCardsForPage(int pageIdx)
{
    const auto items = m_pageView->items();
    for (const auto &item : items) {
        if (item->pageNumber() == pageIdx) {
            item->clearVibeCards();
            break;
        }
    }
}
