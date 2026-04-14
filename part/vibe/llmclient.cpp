/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "llmclient.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QSettings>

using namespace Vibe;

LlmClient::LlmClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    connect(m_nam, &QNetworkAccessManager::finished, this, &LlmClient::onReplyFinished);

    // Load config from environment or QSettings
    const QString envKey = QProcessEnvironment::systemEnvironment().value(QStringLiteral("VIBE_OPENAI_API_KEY")).trimmed();
    if (!envKey.isEmpty()) {
        m_config.apiKey = envKey;
    }

    const QString envModel = QProcessEnvironment::systemEnvironment().value(QStringLiteral("VIBE_OPENAI_MODEL")).trimmed();
    if (!envModel.isEmpty()) {
        m_config.model = envModel;
    }

    const QString envBase = QProcessEnvironment::systemEnvironment().value(QStringLiteral("VIBE_OPENAI_BASE_URL")).trimmed();
    if (!envBase.isEmpty()) {
        m_config.baseUrl = QUrl(envBase);
    }

    // Fallback to QSettings
    if (m_config.apiKey.isEmpty()) {
        QSettings settings(QStringLiteral("okular-vibe"), QStringLiteral("okular-vibe"));
        m_config.apiKey = settings.value(QStringLiteral("apiKey")).toString().trimmed();
        const QString settingsModel = settings.value(QStringLiteral("model")).toString().trimmed();
        if (!settingsModel.isEmpty()) {
            m_config.model = settingsModel;
        }
        const QString settingsBase = settings.value(QStringLiteral("baseUrl")).toString().trimmed();
        if (!settingsBase.isEmpty()) {
            m_config.baseUrl = QUrl(settingsBase);
        }
        m_config.language = settings.value(QStringLiteral("summaryLanguage"), QStringLiteral("en")).toString();
        m_config.processingMode = settings.value(QStringLiteral("processingMode"), QStringLiteral("batch")).toString();
    }
}

LlmClient::~LlmClient() = default;

void LlmClient::setConfig(const Config &config)
{
    m_config = config;
}

void LlmClient::setPdfData(const QByteArray &pdfBase64)
{
    m_pdfBase64 = pdfBase64;
}

QByteArray LlmClient::pdfData() const
{
    return m_pdfBase64;
}

QString LlmClient::buildSystemContent() const
{
    QString content = QStringLiteral("You are an academic paper analysis assistant. Always respond with valid JSON.");
    if (m_config.language == QLatin1String("zh")) {
        content += QStringLiteral(
            "\nIMPORTANT: Write all summaries and point descriptions in Chinese (简体中文). "
            "Keep technical terms, proper nouns, and abbreviations in English.");
    }
    return content;
}

QString LlmClient::buildUserPrompt(int targetPageIdx, const QList<ParagraphData> &paragraphs) const
{
    QString prompt = QStringLiteral(
        "The PDF of the full paper is attached above.\n"
        "Analyze ONLY the following paragraphs from page %1.\n\n"
        "For each paragraph, provide:\n"
        "1. paragraph_summary: A concise summary of the paragraph (10 words or less).\n"
        "2. point_summaries: An array of concise summaries (10 words or less each), one per logical point in the paragraph.\n"
        "   - Division principle: Aggressively merge content with similar semantics, same sub-topic, or cause-effect relationships. "
        "Only split when the paragraph addresses distinctly different sub-topics.\n"
        "   - Ideally 2-4 points. A single-meaning paragraph may have 1 point. Complex paragraphs may exceed 4.\n\n"
        "Example:\n"
        "Input paragraph:\n"
        "\"The goal of reducing sequential computation also forms the foundation of the Extended Neural GPU, "
        "ByteNet and ConvS2S, all of which use convolutional neural networks as basic building block. "
        "In these models, the number of operations required to relate signals from two arbitrary positions "
        "grows in the distance between positions. This makes it more difficult to learn dependencies between "
        "distant positions. In the Transformer this is reduced to a constant number of operations.\"\n\n"
        "Output:\n"
        "{\n"
        "  \"paragraphs\": [\n"
        "    {\n"
        "      \"id\": \"0_0\",\n"
        "      \"paragraph_summary\": \"Transformer vs CNN advantages\",\n"
        "      \"point_summaries\": [\n"
        "        \"CNNs reduce sequential computation\",\n"
        "        \"CNNs struggle with long-range dependencies\",\n"
        "        \"Transformer uses constant operations\"\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n\n"
        "Return a JSON object with this format:\n"
        "{\n"
        "  \"paragraphs\": [\n"
        "    {\n"
        "      \"id\": \"%1_<paragraphIdx>\",\n"
        "      \"paragraph_summary\": \"...\",\n"
        "      \"point_summaries\": [\"...\", \"...\"]\n"
        "    }\n"
        "  ]\n"
        "}\n\n"
        "Paragraphs:\n\n"
    ).arg(targetPageIdx);

    for (const auto &p : paragraphs) {
        prompt += QStringLiteral("--- Paragraph %1 ---\n%2\n\n").arg(p.paragraphIdx).arg(p.text);
    }

    return prompt;
}

QString LlmClient::buildBatchPrompt(const QMap<int, QList<ParagraphData>> &allParagraphs) const
{
    QString prompt = QStringLiteral(
        "The PDF of the full paper is attached above.\n"
        "Analyze the following paragraphs from each page.\n\n"
        "For each paragraph, provide:\n"
        "1. paragraph_summary: A concise summary of the paragraph (10 words or less).\n"
        "2. point_summaries: An array of concise summaries (10 words or less each), one per logical point in the paragraph.\n"
        "   - Division principle: Aggressively merge content with similar semantics, same sub-topic, or cause-effect relationships. "
        "Only split when the paragraph addresses distinctly different sub-topics.\n"
        "   - Ideally 2-4 points. A single-meaning paragraph may have 1 point. Complex paragraphs may exceed 4.\n\n"
        "Example:\n"
        "Input paragraph:\n"
        "\"The goal of reducing sequential computation also forms the foundation of the Extended Neural GPU, "
        "ByteNet and ConvS2S, all of which use convolutional neural networks as basic building block. "
        "In these models, the number of operations required to relate signals from two arbitrary positions "
        "grows in the distance between positions. This makes it more difficult to learn dependencies between "
        "distant positions. In the Transformer this is reduced to a constant number of operations.\"\n\n"
        "Output:\n"
        "{\n"
        "  \"paragraphs\": [\n"
        "    {\n"
        "      \"id\": \"0_0\",\n"
        "      \"paragraph_summary\": \"Transformer vs CNN advantages\",\n"
        "      \"point_summaries\": [\n"
        "        \"CNNs reduce sequential computation\",\n"
        "        \"CNNs struggle with long-range dependencies\",\n"
        "        \"Transformer uses constant operations\"\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n\n"
        "Return a JSON object with this format:\n"
        "{\n"
        "  \"pages\": [\n"
        "    {\n"
        "      \"page_idx\": 0,\n"
        "      \"paragraphs\": [\n"
        "        {\n"
        "          \"id\": \"0_<paragraphIdx>\",\n"
        "          \"paragraph_summary\": \"...\",\n"
        "          \"point_summaries\": [\"...\", \"...\"]\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n\n"
    );

    for (auto it = allParagraphs.constBegin(); it != allParagraphs.constEnd(); ++it) {
        prompt += QStringLiteral("=== Page %1 ===\n").arg(it.key());
        for (const auto &p : it.value()) {
            prompt += QStringLiteral("--- Paragraph %1 ---\n%2\n\n").arg(p.paragraphIdx).arg(p.text);
        }
    }

    return prompt;
}

QJsonArray LlmClient::buildUserContent(const QString &textPrompt) const
{
    QJsonArray content;

    // Attach PDF file
    if (!m_pdfBase64.isEmpty()) {
        QJsonObject filePart;
        filePart[QStringLiteral("type")] = QStringLiteral("file");

        QJsonObject fileObj;
        fileObj[QStringLiteral("filename")] = QStringLiteral("paper.pdf");
        fileObj[QStringLiteral("file_data")] = QStringLiteral("data:application/pdf;base64,%1").arg(QString::fromLatin1(m_pdfBase64));

        filePart[QStringLiteral("file")] = fileObj;
        content.append(filePart);
    }

    // Text prompt
    QJsonObject textPart;
    textPart[QStringLiteral("type")] = QStringLiteral("text");
    textPart[QStringLiteral("text")] = textPrompt;
    content.append(textPart);

    return content;
}

void LlmClient::requestPageSummary(int pageIdx, const QList<ParagraphData> &paragraphs)
{
    if (m_config.apiKey.isEmpty()) {
        Q_EMIT pageErrorOccurred(pageIdx, QStringLiteral("No API key configured. Set VIBE_OPENAI_API_KEY environment variable."));
        return;
    }

    if (paragraphs.isEmpty()) {
        Q_EMIT pageErrorOccurred(pageIdx, QStringLiteral("No paragraphs to summarize."));
        return;
    }

    QUrl url = m_config.baseUrl;
    url.setPath(url.path() + QStringLiteral("/chat/completions"));

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(m_config.apiKey).toUtf8());
    request.setTransferTimeout(REQUEST_TIMEOUT_MS);

    QJsonObject body;
    body[QStringLiteral("model")] = m_config.model;

    QJsonArray messages;

    QJsonObject systemMsg;
    systemMsg[QStringLiteral("role")] = QStringLiteral("system");
    systemMsg[QStringLiteral("content")] = buildSystemContent();
    messages.append(systemMsg);

    QJsonObject userMsg;
    userMsg[QStringLiteral("role")] = QStringLiteral("user");
    userMsg[QStringLiteral("content")] = buildUserContent(buildUserPrompt(pageIdx, paragraphs));
    messages.append(userMsg);

    body[QStringLiteral("messages")] = messages;
    body[QStringLiteral("response_format")] = buildPageResponseFormat();

    body[QStringLiteral("temperature")] = 1.0;
    body[QStringLiteral("top_p")] = 0.95;

    QNetworkReply *reply = m_nam->post(request, QJsonDocument(body).toJson());
    reply->setProperty("vibe_page_idx", pageIdx);
    reply->setProperty("vibe_batch", false);
    reply->setProperty("vibe_para_count", paragraphs.size());
}

void LlmClient::requestAllPagesSummary(const QMap<int, QList<ParagraphData>> &allParagraphs)
{
    if (m_config.apiKey.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("No API key configured. Set VIBE_OPENAI_API_KEY environment variable."));
        return;
    }

    if (allParagraphs.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("No paragraphs to summarize."));
        return;
    }

    QUrl url = m_config.baseUrl;
    url.setPath(url.path() + QStringLiteral("/chat/completions"));

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(m_config.apiKey).toUtf8());
    request.setTransferTimeout(REQUEST_TIMEOUT_MS);

    QJsonObject body;
    body[QStringLiteral("model")] = m_config.model;

    QJsonArray messages;

    QJsonObject systemMsg;
    systemMsg[QStringLiteral("role")] = QStringLiteral("system");
    systemMsg[QStringLiteral("content")] = buildSystemContent();
    messages.append(systemMsg);

    QJsonObject userMsg;
    userMsg[QStringLiteral("role")] = QStringLiteral("user");
    userMsg[QStringLiteral("content")] = buildUserContent(buildBatchPrompt(allParagraphs));
    messages.append(userMsg);

    body[QStringLiteral("messages")] = messages;
    body[QStringLiteral("response_format")] = buildBatchResponseFormat();

    body[QStringLiteral("temperature")] = 1.0;
    body[QStringLiteral("top_p")] = 0.95;

    QNetworkReply *reply = m_nam->post(request, QJsonDocument(body).toJson());
    reply->setProperty("vibe_batch", true);
    reply->setProperty("vibe_page_idx", -1);
}

void LlmClient::onReplyFinished(QNetworkReply *reply)
{
    reply->deleteLater();

    const bool isBatch = reply->property("vibe_batch").toBool();
    const int pageIdx = reply->property("vibe_page_idx").toInt();

    if (reply->error() != QNetworkReply::NoError) {
        const QString errMsg = QStringLiteral("HTTP error: %1").arg(reply->errorString());
        if (isBatch) {
            Q_EMIT errorOccurred(errMsg);
        } else {
            Q_EMIT pageErrorOccurred(pageIdx, errMsg);
        }
        return;
    }

    const QByteArray data = reply->readAll();

    if (isBatch) {
        const auto results = parseBatchResponse(data);
        if (results.isEmpty()) {
            Q_EMIT errorOccurred(QStringLiteral("Failed to parse LLM batch response."));
            return;
        }
        Q_EMIT allPagesSummaryReady(results);
    } else {
        const auto results = parseResponse(data);
        if (!validatePageResults(results)) {
            Q_EMIT pageErrorOccurred(pageIdx, QStringLiteral("LLM returned invalid or incomplete results."));
            return;
        }
        Q_EMIT pageSummaryReady(pageIdx, results);
    }
}

QString LlmClient::extractContentFromResponse(const QByteArray &data) const
{
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        qWarning() << "[LlmClient] Response is not a JSON object";
        return {};
    }

    QJsonObject root = doc.object();
    QJsonArray choices = root[QStringLiteral("choices")].toArray();
    if (choices.isEmpty()) {
        qWarning() << "[LlmClient] No choices in response";
        return {};
    }

    return choices[0].toObject()[QStringLiteral("message")].toObject()[QStringLiteral("content")].toString();
}

QList<ParagraphLlmResult> LlmClient::parseResponse(const QByteArray &data) const
{
    QList<ParagraphLlmResult> results;

    QString content = extractContentFromResponse(data);
    if (content.isEmpty()) {
        return results;
    }

    QJsonDocument contentDoc = QJsonDocument::fromJson(content.toUtf8());
    if (!contentDoc.isObject()) {
        qWarning() << "[LlmClient] Content is not valid JSON:" << content.left(200);
        return results;
    }

    QJsonArray paragraphs = contentDoc.object()[QStringLiteral("paragraphs")].toArray();
    for (const auto &pVal : paragraphs) {
        QJsonObject pObj = pVal.toObject();
        ParagraphLlmResult r;

        QString id = pObj[QStringLiteral("id")].toString();
        QStringList parts = id.split(QLatin1Char('_'));
        if (parts.size() >= 2) {
            r.paragraphIdx = parts[1].toInt();
        }

        r.paragraphSummary = pObj[QStringLiteral("paragraph_summary")].toString();

        QJsonArray pointSummaries = pObj[QStringLiteral("point_summaries")].toArray();

        for (int i = 0; i < pointSummaries.size(); ++i) {
            PointData pt;
            pt.pointIdx = i;
            pt.summary = pointSummaries[i].toString();
            r.points.append(pt);
        }

        results.append(r);
    }

    return results;
}

QMap<int, QList<ParagraphLlmResult>> LlmClient::parseBatchResponse(const QByteArray &data) const
{
    QMap<int, QList<ParagraphLlmResult>> results;

    QString content = extractContentFromResponse(data);
    if (content.isEmpty()) {
        return results;
    }

    QJsonDocument contentDoc = QJsonDocument::fromJson(content.toUtf8());
    if (!contentDoc.isObject()) {
        qWarning() << "[LlmClient] Content is not valid JSON:" << content.left(200);
        return results;
    }

    QJsonArray pages = contentDoc.object()[QStringLiteral("pages")].toArray();
    for (const auto &pageVal : pages) {
        QJsonObject pageObj = pageVal.toObject();
        int pageIdx = pageObj[QStringLiteral("page_idx")].toInt();

        QList<ParagraphLlmResult> pageResults;
        QJsonArray paragraphs = pageObj[QStringLiteral("paragraphs")].toArray();

        for (const auto &pVal : paragraphs) {
            QJsonObject pObj = pVal.toObject();
            ParagraphLlmResult r;

            QString id = pObj[QStringLiteral("id")].toString();
            QStringList parts = id.split(QLatin1Char('_'));
            if (parts.size() >= 2) {
                r.paragraphIdx = parts[1].toInt();
            }

            r.paragraphSummary = pObj[QStringLiteral("paragraph_summary")].toString();

            QJsonArray pointSummaries = pObj[QStringLiteral("point_summaries")].toArray();

            for (int i = 0; i < pointSummaries.size(); ++i) {
                PointData pt;
                pt.pointIdx = i;
                pt.summary = pointSummaries[i].toString();
                r.points.append(pt);
            }

            pageResults.append(r);
        }

        results[pageIdx] = pageResults;
    }

    return results;
}

bool LlmClient::validatePageResults(const QList<ParagraphLlmResult> &results) const
{
    if (results.isEmpty()) {
        return false;
    }
    for (const auto &r : results) {
        if (r.paragraphSummary.isEmpty()) {
            return false;
        }
        if (r.points.isEmpty()) {
            return false;
        }
    }
    return true;
}

QJsonObject LlmClient::buildPageResponseFormat() const
{
    QJsonObject idProp;
    idProp[QStringLiteral("type")] = QStringLiteral("string");

    QJsonObject summaryProp;
    summaryProp[QStringLiteral("type")] = QStringLiteral("string");

    QJsonObject pointItemSchema;
    pointItemSchema[QStringLiteral("type")] = QStringLiteral("string");

    QJsonObject pointsProp;
    pointsProp[QStringLiteral("type")] = QStringLiteral("array");
    pointsProp[QStringLiteral("items")] = pointItemSchema;

    QJsonObject paragraphProps;
    paragraphProps[QStringLiteral("id")] = idProp;
    paragraphProps[QStringLiteral("paragraph_summary")] = summaryProp;
    paragraphProps[QStringLiteral("point_summaries")] = pointsProp;

    QJsonObject paragraphItem;
    paragraphItem[QStringLiteral("type")] = QStringLiteral("object");
    paragraphItem[QStringLiteral("properties")] = paragraphProps;
    paragraphItem[QStringLiteral("required")] = QJsonArray({QStringLiteral("id"), QStringLiteral("paragraph_summary"), QStringLiteral("point_summaries")});
    paragraphItem[QStringLiteral("additionalProperties")] = false;

    QJsonObject paragraphsArray;
    paragraphsArray[QStringLiteral("type")] = QStringLiteral("array");
    paragraphsArray[QStringLiteral("items")] = paragraphItem;

    QJsonObject rootProps;
    rootProps[QStringLiteral("paragraphs")] = paragraphsArray;

    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");
    schema[QStringLiteral("properties")] = rootProps;
    schema[QStringLiteral("required")] = QJsonArray({QStringLiteral("paragraphs")});
    schema[QStringLiteral("additionalProperties")] = false;

    QJsonObject jsonSchema;
    jsonSchema[QStringLiteral("name")] = QStringLiteral("PageSummaryOutput");
    jsonSchema[QStringLiteral("strict")] = true;
    jsonSchema[QStringLiteral("schema")] = schema;

    QJsonObject responseFormat;
    responseFormat[QStringLiteral("type")] = QStringLiteral("json_schema");
    responseFormat[QStringLiteral("json_schema")] = jsonSchema;

    return responseFormat;
}

QJsonObject LlmClient::buildBatchResponseFormat() const
{
    QJsonObject idProp;
    idProp[QStringLiteral("type")] = QStringLiteral("string");

    QJsonObject summaryProp;
    summaryProp[QStringLiteral("type")] = QStringLiteral("string");

    QJsonObject pointItemSchema;
    pointItemSchema[QStringLiteral("type")] = QStringLiteral("string");

    QJsonObject pointsProp;
    pointsProp[QStringLiteral("type")] = QStringLiteral("array");
    pointsProp[QStringLiteral("items")] = pointItemSchema;

    QJsonObject paragraphProps;
    paragraphProps[QStringLiteral("id")] = idProp;
    paragraphProps[QStringLiteral("paragraph_summary")] = summaryProp;
    paragraphProps[QStringLiteral("point_summaries")] = pointsProp;

    QJsonObject paragraphItem;
    paragraphItem[QStringLiteral("type")] = QStringLiteral("object");
    paragraphItem[QStringLiteral("properties")] = paragraphProps;
    paragraphItem[QStringLiteral("required")] = QJsonArray({QStringLiteral("id"), QStringLiteral("paragraph_summary"), QStringLiteral("point_summaries")});
    paragraphItem[QStringLiteral("additionalProperties")] = false;

    QJsonObject paragraphsArray;
    paragraphsArray[QStringLiteral("type")] = QStringLiteral("array");
    paragraphsArray[QStringLiteral("items")] = paragraphItem;

    QJsonObject pageIdxProp;
    pageIdxProp[QStringLiteral("type")] = QStringLiteral("integer");

    QJsonObject pageProps;
    pageProps[QStringLiteral("page_idx")] = pageIdxProp;
    pageProps[QStringLiteral("paragraphs")] = paragraphsArray;

    QJsonObject pageItem;
    pageItem[QStringLiteral("type")] = QStringLiteral("object");
    pageItem[QStringLiteral("properties")] = pageProps;
    pageItem[QStringLiteral("required")] = QJsonArray({QStringLiteral("page_idx"), QStringLiteral("paragraphs")});
    pageItem[QStringLiteral("additionalProperties")] = false;

    QJsonObject pagesArray;
    pagesArray[QStringLiteral("type")] = QStringLiteral("array");
    pagesArray[QStringLiteral("items")] = pageItem;

    QJsonObject rootProps;
    rootProps[QStringLiteral("pages")] = pagesArray;

    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");
    schema[QStringLiteral("properties")] = rootProps;
    schema[QStringLiteral("required")] = QJsonArray({QStringLiteral("pages")});
    schema[QStringLiteral("additionalProperties")] = false;

    QJsonObject jsonSchema;
    jsonSchema[QStringLiteral("name")] = QStringLiteral("BatchSummaryOutput");
    jsonSchema[QStringLiteral("strict")] = true;
    jsonSchema[QStringLiteral("schema")] = schema;

    QJsonObject responseFormat;
    responseFormat[QStringLiteral("type")] = QStringLiteral("json_schema");
    responseFormat[QStringLiteral("json_schema")] = jsonSchema;

    return responseFormat;
}
