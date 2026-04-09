/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef VIBE_LLM_CLIENT_H
#define VIBE_LLM_CLIENT_H

#include <QMap>
#include <QObject>
#include <QUrl>

#include "vibetypes.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace Vibe
{

class LlmClient : public QObject
{
    Q_OBJECT

public:
    struct Config {
        QString apiKey;
        QString model = QStringLiteral("gpt-4o-mini");
        QUrl baseUrl = QUrl(QStringLiteral("https://api.openai.com/v1"));
        QString language = QStringLiteral("en");
        QString processingMode = QStringLiteral("batch");
    };

    explicit LlmClient(QObject *parent = nullptr);
    ~LlmClient() override;

    void setConfig(const Config &config);
    void setPdfData(const QByteArray &pdfBase64);
    QByteArray pdfData() const;

    void requestPageSummary(int pageIdx, const QList<ParagraphData> &paragraphs);
    void requestAllPagesSummary(const QMap<int, QList<ParagraphData>> &allParagraphs);

Q_SIGNALS:
    void pageSummaryReady(int pageIdx, const QList<ParagraphLlmResult> &results);
    void allPagesSummaryReady(const QMap<int, QList<ParagraphLlmResult>> &results);
    void errorOccurred(const QString &msg);

private Q_SLOTS:
    void onReplyFinished(QNetworkReply *reply);

private:
    QString buildUserPrompt(int targetPageIdx, const QList<ParagraphData> &paragraphs) const;
    QString buildBatchPrompt(const QMap<int, QList<ParagraphData>> &allParagraphs) const;
    QJsonArray buildUserContent(const QString &textPrompt) const;
    QString buildSystemContent() const;

    QList<ParagraphLlmResult> parseResponse(const QByteArray &data) const;
    QMap<int, QList<ParagraphLlmResult>> parseBatchResponse(const QByteArray &data) const;
    QString extractContentFromResponse(const QByteArray &data) const;

    QNetworkAccessManager *m_nam;
    Config m_config;
    QByteArray m_pdfBase64;
    int m_pendingPageIdx = -1;
    bool m_batchMode = false;
};

} // namespace Vibe

#endif // VIBE_LLM_CLIENT_H
