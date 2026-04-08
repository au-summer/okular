/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef VIBE_MINERU_CLIENT_H
#define VIBE_MINERU_CLIENT_H

#include <QMap>
#include <QObject>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

#include "vibetypes.h"

namespace Vibe
{

class MinerUClient : public QObject
{
    Q_OBJECT

public:
    struct Config {
        QString apiToken;
    };

    explicit MinerUClient(QObject *parent = nullptr);
    ~MinerUClient() override;

    void setConfig(const Config &config);
    void parseDocument(const QString &filePath);

Q_SIGNALS:
    void documentReady(const QMap<int, QList<Vibe::ParagraphData>> &pagesParagraphs);
    void errorOccurred(const QString &msg);
    void progressChanged(const QString &status);

private Q_SLOTS:
    void onUploadUrlReply(QNetworkReply *reply);
    void onFileUploaded(QNetworkReply *reply);
    void onPollReply(QNetworkReply *reply);
    void onDownloadReply(QNetworkReply *reply);
    void pollOnce();

private:
    void requestUploadUrl();
    void uploadFile(const QUrl &signedUrl);
    void downloadAndParseResult(const QUrl &zipUrl);
    QMap<int, QList<ParagraphData>> parseContentList(const QByteArray &jsonData);
    QByteArray extractContentListFromZip(const QByteArray &zipData);

    QNetworkAccessManager *m_nam;
    Config m_config;
    QString m_filePath;
    QTimer *m_pollTimer;
    QString m_currentTaskId;
    int m_pollCount = 0;
    static constexpr int MAX_POLL_ATTEMPTS = 120;
    static constexpr int POLL_INTERVAL_MS = 5000;
};

} // namespace Vibe

#endif // VIBE_MINERU_CLIENT_H
