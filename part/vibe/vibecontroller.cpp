/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vibecontroller.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QFile>
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
    connect(m_llmClient, &LlmClient::allPagesSummaryReady, this, &VibeController::onAllPagesSummaryReady);
    connect(m_llmClient, &LlmClient::errorOccurred, this, &VibeController::onLlmError);
    connect(m_llmClient, &LlmClient::pageErrorOccurred, this, &VibeController::onPageLlmError);

    connect(m_mineruClient, &MinerUClient::documentReady, this, &VibeController::onDocumentReady);
    connect(m_mineruClient, &MinerUClient::errorOccurred, this, &VibeController::onMinerUError);
    connect(m_mineruClient, &MinerUClient::progressChanged, this, &VibeController::onMinerUProgress);

    m_db.open();

    QSettings settings(QStringLiteral("okular-vibe"), QStringLiteral("okular-vibe"));
    m_language = settings.value(QStringLiteral("summaryLanguage"), QStringLiteral("en")).toString();
    m_processingMode = settings.value(QStringLiteral("processingMode"), QStringLiteral("batch")).toString();
}

VibeController::~VibeController()
{
    m_db.close();
}

void VibeController::toggleCardsVisible()
{
    const bool visible = !m_cardsVisible;
    m_cardsVisible = visible;

    if (!m_document || m_document->pages() == 0) {
        return;
    }

    ensurePaperId();

    const auto items = m_pageView->items();
    for (auto *item : items) {
        if (visible && item->vibeCards().isEmpty() && m_currentPaperId >= 0) {
            loadCachedCardsForPage(item->pageNumber());
        }
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
    const QString newLanguage = settings.value(QStringLiteral("summaryLanguage"), QStringLiteral("en")).toString();
    const bool languageChanged = (newLanguage != m_language);
    if (languageChanged && (m_parsingAllPages || m_activeRequestCount > 0)) {
        qWarning() << "[VibeController] Cannot change language while parsing is in progress";
        m_pageView->displayMessage(QStringLiteral("Cannot change language while parsing is in progress."));
        // Keep old language
        llmConfig.language = m_language;
    } else {
        m_language = newLanguage;
        llmConfig.language = m_language;
    }
    m_processingMode = settings.value(QStringLiteral("processingMode"), QStringLiteral("batch")).toString();
    llmConfig.processingMode = m_processingMode;
    m_llmClient->setConfig(llmConfig);

    // Reload MinerU config
    MinerUClient::Config mineruConfig;
    mineruConfig.apiToken = settings.value(QStringLiteral("mineruToken")).toString().trimmed();
    m_mineruClient->setConfig(mineruConfig);

    // If language actually changed, clear all cards and reload for new language
    const bool didChangeLanguage = (m_language == newLanguage && languageChanged);
    if (didChangeLanguage) {
        const auto items = m_pageView->items();
        for (auto *item : items) {
            item->clearVibeCards();
            if (m_cardsVisible && m_currentPaperId >= 0) {
                loadCachedCardsForPage(item->pageNumber());
            }
        }
    } else {
        // Just reload font config on existing cards
        const auto items = m_pageView->items();
        for (auto *item : items) {
            for (auto &pair : item->vibeCards()) {
                if (pair.summary) {
                    pair.summary->reloadFontConfig();
                }
                if (pair.points) {
                    pair.points->reloadFontConfig();
                }
            }
        }
    }

    qDebug() << "[VibeController] Config reloaded, model:" << llmConfig.model << "language:" << m_language;
}

bool VibeController::loadCachedCardsForPage(int pageIdx)
{
    if (m_currentPaperId < 0) {
        return false;
    }

    QList<SummaryCardData> existingCards = m_db.getSummaryCards(m_currentPaperId, pageIdx, m_language);
    if (existingCards.isEmpty()) {
        return false;
    }

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
    qDebug() << "[VibeController] Loaded" << existingCards.size() << "cached cards for page" << pageIdx;
    return true;
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

    if (m_inFlightPages.contains(pageIdx)) {
        m_pageView->displayMessage(QStringLiteral("Page %1 is already being processed...").arg(pageIdx + 1));
        return;
    }

    // Ensure paper is registered in DB
    ensurePaperId();

    // Check if we already have summary cards for this page (fully processed)
    if (m_currentPaperId >= 0 && loadCachedCardsForPage(pageIdx)) {
        m_cardsVisible = true;
        return;
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
    const QString filePath = m_document->currentDocument().toLocalFile();
    if (filePath.isEmpty()) {
        qWarning() << "[VibeController] Cannot get local file path for MinerU parsing";
        return;
    }

    m_pendingMinerUPageIdx = pageIdx;
    m_pageView->displayMessage(QStringLiteral("Starting MinerU document parsing..."));
    m_mineruClient->parseDocument(filePath);
}

void VibeController::retryCurrentPage()
{
    if (!m_document || m_document->pages() == 0) {
        return;
    }

    if (m_parsingAllPages) {
        m_pageView->displayMessage(QStringLiteral("Please wait, parsing all pages..."));
        return;
    }

    const int pageIdx = m_document->currentPage();

    ensurePaperId();

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
    m_db.deleteLlmDataForPage(m_currentPaperId, pageIdx, m_language);

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

    ensurePaperId();

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
        const QString filePath = m_document->currentDocument().toLocalFile();
        if (filePath.isEmpty()) {
            qWarning() << "[VibeController] Cannot get local file path for MinerU parsing";
            return;
        }
        m_parsingAllPages = true;
        m_pendingMinerUPageIdx = -1;
        m_pageView->displayMessage(QStringLiteral("Starting MinerU document parsing (all pages)..."));
        m_mineruClient->parseDocument(filePath);
        return;
    }

    // Gather pages that need LLM processing
    QMap<int, QList<ParagraphData>> unprocessed;
    for (uint p = 0; p < m_document->pages(); ++p) {
        auto paras = m_db.getParagraphs(m_currentPaperId, p);
        if (!paras.isEmpty() && m_db.getSummaryCards(m_currentPaperId, p, m_language).isEmpty()) {
            unprocessed[p] = paras;
        }
    }

    if (unprocessed.isEmpty()) {
        m_pageView->displayMessage(QStringLiteral("All pages already processed."));
        return;
    }

    m_parsingAllPages = true;
    m_cardsVisible = true;

    if (m_processingMode == QLatin1String("batch")) {
        // Batch mode: single LLM request for all pages
        ensurePdfData();
        m_pageView->displayMessage(QStringLiteral("Sending %1 pages to LLM (batch)...").arg(unprocessed.size()));
        // Show loading cards for all pages
        for (auto it = unprocessed.constBegin(); it != unprocessed.constEnd(); ++it) {
            clearCardsForPage(it.key());
            createCardsForPage(it.key(), it.value());
        }
        m_llmClient->requestAllPagesSummary(unprocessed);
    } else {
        // Per-page mode: queue pages
        m_allPagesQueue.clear();
        for (auto it = unprocessed.constBegin(); it != unprocessed.constEnd(); ++it) {
            m_allPagesQueue.append(it.key());
        }
        m_pageView->displayMessage(QStringLiteral("Parsing all pages: %1 pages remaining...").arg(m_allPagesQueue.size()));
        dispatchQueuedPages();
    }
}

void VibeController::retryAllPages()
{
    if (!m_document || m_document->pages() == 0) {
        return;
    }

    if (m_parsingAllPages) {
        m_pageView->displayMessage(QStringLiteral("Already parsing all pages..."));
        return;
    }

    ensurePaperId();

    if (m_currentPaperId < 0) {
        m_pageView->displayMessage(QStringLiteral("No parsed data. Run Parse All Pages first."));
        return;
    }

    // Delete LLM data for all pages that have MinerU paragraphs
    QMap<int, QList<ParagraphData>> toRetry;
    for (uint p = 0; p < m_document->pages(); ++p) {
        QList<ParagraphData> paragraphs = m_db.getParagraphs(m_currentPaperId, p);
        if (!paragraphs.isEmpty()) {
            m_db.deleteLlmDataForPage(m_currentPaperId, p, m_language);
            clearCardsForPage(p);
            toRetry[p] = paragraphs;
        }
    }

    if (toRetry.isEmpty()) {
        m_pageView->displayMessage(QStringLiteral("No pages with parsed data to retry."));
        return;
    }

    m_parsingAllPages = true;
    m_cardsVisible = true;

    if (m_processingMode == QLatin1String("batch")) {
        ensurePdfData();
        m_pageView->displayMessage(QStringLiteral("Retrying %1 pages (batch)...").arg(toRetry.size()));
        for (auto it = toRetry.constBegin(); it != toRetry.constEnd(); ++it) {
            createCardsForPage(it.key(), it.value());
        }
        m_llmClient->requestAllPagesSummary(toRetry);
    } else {
        m_allPagesQueue.clear();
        for (auto it = toRetry.constBegin(); it != toRetry.constEnd(); ++it) {
            m_allPagesQueue.append(it.key());
        }
        m_pageView->displayMessage(QStringLiteral("Retrying all pages: %1 pages remaining...").arg(m_allPagesQueue.size()));
        dispatchQueuedPages();
    }
}

void VibeController::dispatchQueuedPages()
{
    while (m_activeRequestCount < MAX_CONCURRENCY && !m_allPagesQueue.isEmpty()) {
        int pageIdx = m_allPagesQueue.takeFirst();
        QList<ParagraphData> paragraphs = m_db.getParagraphs(m_currentPaperId, pageIdx);
        if (!paragraphs.isEmpty()) {
            m_retryCount[pageIdx] = MAX_RETRIES;
            m_pageView->displayMessage(QStringLiteral("Parsing pages... (%1 remaining)")
                .arg(m_allPagesQueue.size()));
            processPageWithLlm(pageIdx, paragraphs);
        }
    }

    // Check if all work is done
    if (m_activeRequestCount == 0 && m_allPagesQueue.isEmpty() && m_parsingAllPages) {
        m_parsingAllPages = false;
        m_retryCount.clear();
        m_pageView->displayMessage(QStringLiteral("All pages processed."));
    }
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

    // If parsing all pages, start LLM processing
    if (m_parsingAllPages) {
        // Filter to unprocessed pages
        QMap<int, QList<ParagraphData>> unprocessed;
        for (auto it = pagesParagraphs.constBegin(); it != pagesParagraphs.constEnd(); ++it) {
            if (!it.value().isEmpty() && m_db.getSummaryCards(m_currentPaperId, it.key(), m_language).isEmpty()) {
                unprocessed[it.key()] = it.value();
            }
        }

        m_cardsVisible = true;

        if (m_processingMode == QLatin1String("batch")) {
            ensurePdfData();
            m_pageView->displayMessage(QStringLiteral("Sending %1 pages to LLM (batch)...").arg(unprocessed.size()));
            for (auto it = unprocessed.constBegin(); it != unprocessed.constEnd(); ++it) {
                clearCardsForPage(it.key());
                createCardsForPage(it.key(), it.value());
            }
            m_llmClient->requestAllPagesSummary(unprocessed);
        } else {
            m_allPagesQueue.clear();
            for (auto it = unprocessed.constBegin(); it != unprocessed.constEnd(); ++it) {
                m_allPagesQueue.append(it.key());
            }
            std::sort(m_allPagesQueue.begin(), m_allPagesQueue.end());
            m_pageView->displayMessage(QStringLiteral("Parsing all pages: %1 pages to process...").arg(m_allPagesQueue.size()));
            dispatchQueuedPages();
        }
        return;
    }

    // Otherwise process the single page the user originally requested
    if (m_pendingMinerUPageIdx >= 0 && pagesParagraphs.contains(m_pendingMinerUPageIdx)) {
        processPageWithLlm(m_pendingMinerUPageIdx, pagesParagraphs[m_pendingMinerUPageIdx]);
    } else if (m_pendingMinerUPageIdx >= 0) {
        qDebug() << "[VibeController] Page" << m_pendingMinerUPageIdx << "has no text content from MinerU";
        m_pageView->displayMessage(QStringLiteral("No text content on this page."));
    }
    m_pendingMinerUPageIdx = -1;
}

void VibeController::onMinerUError(const QString &error)
{
    qWarning() << "[VibeController] MinerU error:" << error;
    QMessageBox::warning(m_pageView, tr("MinerU Error"), error);
    m_pendingMinerUPageIdx = -1;
    if (m_parsingAllPages) {
        m_parsingAllPages = false;
        m_allPagesQueue.clear();
    }
}

void VibeController::onMinerUProgress(const QString &status)
{
    m_pageView->displayMessage(status);
}

void VibeController::ensurePaperId()
{
    if (m_currentPaperId >= 0) {
        return;
    }
    const QString filePath = m_document->currentDocument().toLocalFile();
    if (filePath.isEmpty()) {
        return;
    }
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    hasher.addData(&f);
    const QString fileHash = QString::fromLatin1(hasher.result().toHex());
    m_currentPaperId = m_db.getOrCreatePaper(fileHash);
}

void VibeController::ensurePdfData()
{
    if (!m_llmClient->pdfData().isEmpty()) {
        return;
    }
    const QString filePath = m_document->currentDocument().toLocalFile();
    if (filePath.isEmpty()) {
        return;
    }
    QFile f(filePath);
    if (f.open(QIODevice::ReadOnly)) {
        m_llmClient->setPdfData(f.readAll().toBase64());
        qDebug() << "[VibeController] PDF data cached for LLM requests";
    }
}

QMap<int, QList<ParagraphData>> VibeController::getAllParagraphs() const
{
    QMap<int, QList<ParagraphData>> result;
    if (m_currentPaperId < 0 || !m_document) {
        return result;
    }
    for (uint p = 0; p < m_document->pages(); ++p) {
        auto paras = m_db.getParagraphs(m_currentPaperId, p);
        if (!paras.isEmpty()) {
            result[p] = paras;
        }
    }
    return result;
}

void VibeController::processPageWithLlm(int pageIdx, const QList<ParagraphData> &paragraphs)
{
    m_cardsVisible = true;
    qDebug() << "[VibeController] Sending" << paragraphs.size() << "paragraphs from page" << pageIdx << "to LLM";

    m_inFlightPages[pageIdx] = paragraphs;
    m_activeRequestCount++;

    // Initialize retry count if not already set
    if (!m_retryCount.contains(pageIdx)) {
        m_retryCount[pageIdx] = MAX_RETRIES;
    }

    // Show loading cards
    clearCardsForPage(pageIdx);
    createCardsForPage(pageIdx, paragraphs); // will show "Summarizing..." since summary is empty

    // Ensure PDF data is cached for the LLM request
    ensurePdfData();

    // Send to LLM
    m_llmClient->requestPageSummary(pageIdx, paragraphs);
}

void VibeController::onPageSummaryReady(int pageIdx, const QList<ParagraphLlmResult> &results)
{
    qDebug() << "[VibeController] Received LLM results for page" << pageIdx << ":" << results.size() << "paragraphs";

    if (!m_inFlightPages.contains(pageIdx)) {
        qWarning() << "[VibeController] Received results for unknown page" << pageIdx;
        return;
    }

    QList<ParagraphData> paragraphs = m_inFlightPages.take(pageIdx);
    m_activeRequestCount--;
    m_retryCount.remove(pageIdx);

    // Merge LLM results with paragraphs
    for (auto &para : paragraphs) {
        for (const auto &r : results) {
            if (r.paragraphIdx == para.paragraphIdx) {
                para.summary = r.paragraphSummary;
                para.points = r.points;
                break;
            }
        }
    }

    // Save LLM results to DB (paragraphs already saved from MinerU in onDocumentReady)
    if (m_currentPaperId >= 0) {
        for (const auto &para : paragraphs) {
            if (!para.summary.isEmpty()) {
                int paragraphDbId = m_db.getParagraphDbId(m_currentPaperId, pageIdx, para.paragraphIdx);
                if (paragraphDbId >= 0) {
                    m_db.savePoints(paragraphDbId, m_language, para.points);
                    m_db.saveSummaryCard(m_currentPaperId, pageIdx, paragraphDbId, para.bbox, para.isLeftColumn, m_language, para.summary);
                }
            }
        }
    }

    // Update the cards
    clearCardsForPage(pageIdx);
    createCardsForPage(pageIdx, paragraphs);

    // Dispatch more pages if in batch mode
    if (m_parsingAllPages) {
        dispatchQueuedPages();
    }
}

void VibeController::onAllPagesSummaryReady(const QMap<int, QList<ParagraphLlmResult>> &results)
{
    qDebug() << "[VibeController] Received batch LLM results for" << results.size() << "pages";

    for (auto it = results.constBegin(); it != results.constEnd(); ++it) {
        int pageIdx = it.key();
        const auto &pageResults = it.value();

        // Load paragraphs from DB for this page
        QList<ParagraphData> paragraphs = m_db.getParagraphs(m_currentPaperId, pageIdx);

        // Merge LLM results
        for (auto &para : paragraphs) {
            for (const auto &r : pageResults) {
                if (r.paragraphIdx == para.paragraphIdx) {
                    para.summary = r.paragraphSummary;
                    para.points = r.points;
                    break;
                }
            }
        }

        // Save to DB
        if (m_currentPaperId >= 0) {
            for (const auto &para : paragraphs) {
                if (!para.summary.isEmpty()) {
                    int paragraphDbId = m_db.getParagraphDbId(m_currentPaperId, pageIdx, para.paragraphIdx);
                    if (paragraphDbId >= 0) {
                        m_db.savePoints(paragraphDbId, m_language, para.points);
                        m_db.saveSummaryCard(m_currentPaperId, pageIdx, paragraphDbId, para.bbox, para.isLeftColumn, m_language, para.summary);
                    }
                }
            }
        }

        // Update cards
        clearCardsForPage(pageIdx);
        createCardsForPage(pageIdx, paragraphs);
    }

    m_parsingAllPages = false;
    m_pageView->displayMessage(QStringLiteral("All %1 pages processed.").arg(results.size()));
}

void VibeController::onPageLlmError(int pageIdx, const QString &error)
{
    qWarning() << "[VibeController] LLM error for page" << pageIdx << ":" << error;

    m_inFlightPages.remove(pageIdx);
    m_activeRequestCount--;

    int retriesLeft = m_retryCount.value(pageIdx, 0);
    if (retriesLeft > 0) {
        m_retryCount[pageIdx] = retriesLeft - 1;
        qDebug() << "[VibeController] Retrying page" << pageIdx
                 << "(" << retriesLeft - 1 << "retries left)";

        QList<ParagraphData> paragraphs = m_db.getParagraphs(m_currentPaperId, pageIdx);
        if (!paragraphs.isEmpty()) {
            processPageWithLlm(pageIdx, paragraphs);
        }
    } else {
        qWarning() << "[VibeController] Page" << pageIdx << "failed after all retries:" << error;
        clearCardsForPage(pageIdx);
    }

    if (m_parsingAllPages) {
        dispatchQueuedPages();
    } else if (m_activeRequestCount == 0 && retriesLeft <= 0) {
        QMessageBox::warning(m_pageView, tr("AI Parse Error"),
            QStringLiteral("Failed to parse page %1: %2").arg(pageIdx + 1).arg(error));
    }
}

void VibeController::onLlmError(const QString &error)
{
    qWarning() << "[VibeController] LLM error:" << error;
    QMessageBox::warning(m_pageView, tr("AI Parse Error"), error);

    m_inFlightPages.clear();
    m_activeRequestCount = 0;
    m_retryCount.clear();

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
    m_pageView->relayoutForVibeCards();
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
    m_pageView->relayoutForVibeCards();
}
