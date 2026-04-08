/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef VIBE_LLM_CLIENT_H
#define VIBE_LLM_CLIENT_H

#include <QObject>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

#include "vibetypes.h"

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
    };

    explicit LlmClient(QObject *parent = nullptr);
    ~LlmClient() override;

    void setConfig(const Config &config);
    void requestPageSummary(int pageIdx, const QList<ParagraphData> &paragraphs);

Q_SIGNALS:
    void pageSummaryReady(int pageIdx, const QList<ParagraphLlmResult> &results);
    void errorOccurred(const QString &msg);

private Q_SLOTS:
    void onReplyFinished(QNetworkReply *reply);

private:
    QString buildPrompt(const QList<ParagraphData> &paragraphs) const;
    QList<ParagraphLlmResult> parseResponse(const QByteArray &data) const;

    QNetworkAccessManager *m_nam;
    Config m_config;
    int m_pendingPageIdx = -1;
};

} // namespace Vibe

#endif // VIBE_LLM_CLIENT_H
