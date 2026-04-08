/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "mineruclient.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTimer>
#include <QTemporaryFile>

// MiniZip or Qt's zip support isn't universally available,
// so we use a simple approach: download zip, use command-line unzip
#include <QProcess>
#include <QTemporaryDir>

using namespace Vibe;

static const QUrl MINERU_BASE_URL(QStringLiteral("https://mineru.net"));

MinerUClient::MinerUClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_pollTimer(new QTimer(this))
{
    m_pollTimer->setInterval(POLL_INTERVAL_MS);
    connect(m_pollTimer, &QTimer::timeout, this, &MinerUClient::pollOnce);

    // Load config from environment or QSettings
    const QString envToken = QProcessEnvironment::systemEnvironment().value(QStringLiteral("VIBE_MINERU_TOKEN")).trimmed();
    if (!envToken.isEmpty()) {
        m_config.apiToken = envToken;
    } else {
        QSettings settings(QStringLiteral("okular-vibe"), QStringLiteral("okular-vibe"));
        m_config.apiToken = settings.value(QStringLiteral("mineruToken")).toString().trimmed();
    }
}

MinerUClient::~MinerUClient()
{
    m_pollTimer->stop();
}

void MinerUClient::setConfig(const Config &config)
{
    m_config = config;
}

void MinerUClient::parseDocument(const QString &filePath)
{
    if (m_config.apiToken.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("No MinerU API token. Set VIBE_MINERU_TOKEN environment variable."));
        return;
    }

    if (!QFile::exists(filePath)) {
        Q_EMIT errorOccurred(QStringLiteral("File not found: %1").arg(filePath));
        return;
    }

    m_filePath = filePath;
    qDebug() << "[MinerU] parseDocument called for:" << filePath;
    Q_EMIT progressChanged(QStringLiteral("Requesting upload URL..."));
    requestUploadUrl();
}

void MinerUClient::requestUploadUrl()
{
    QUrl url(MINERU_BASE_URL);
    url.setPath(QStringLiteral("/api/v4/file-urls/batch"));

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(m_config.apiToken).toUtf8());

    QFileInfo fi(m_filePath);
    QJsonObject fileObj;
    fileObj[QStringLiteral("name")] = fi.fileName();
    fileObj[QStringLiteral("data_id")] = fi.baseName();

    QJsonObject body;
    body[QStringLiteral("files")] = QJsonArray{fileObj};
    body[QStringLiteral("enable_formula")] = true;
    body[QStringLiteral("enable_table")] = true;
    body[QStringLiteral("language")] = QStringLiteral("en");

    qDebug() << "[MinerU] Requesting upload URL from:" << url.toString();
    qDebug() << "[MinerU] Request body:" << QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply *reply = m_nam->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { onUploadUrlReply(reply); });
}

void MinerUClient::onUploadUrlReply(QNetworkReply *reply)
{
    reply->deleteLater();

    qDebug() << "[MinerU] onUploadUrlReply: HTTP status" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError) {
        Q_EMIT errorOccurred(QStringLiteral("MinerU upload URL request failed: %1").arg(reply->errorString()));
        return;
    }

    QByteArray responseData = reply->readAll();
    qDebug() << "[MinerU] Upload URL response:" << responseData.left(500);

    QJsonDocument doc = QJsonDocument::fromJson(responseData);
    QJsonObject root = doc.object();

    if (root[QStringLiteral("code")].toInt() != 0) {
        Q_EMIT errorOccurred(QStringLiteral("MinerU API error: %1").arg(root[QStringLiteral("msg")].toString()));
        return;
    }

    QJsonObject data = root[QStringLiteral("data")].toObject();
    m_currentTaskId = data[QStringLiteral("batch_id")].toString();
    qDebug() << "[MinerU] Got batch_id:" << m_currentTaskId;

    QJsonArray fileUrls = data[QStringLiteral("file_urls")].toArray();
    if (fileUrls.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("No upload URL returned from MinerU."));
        return;
    }

    QUrl uploadUrl(fileUrls[0].toString());
    qDebug() << "[MinerU] Upload URL:" << uploadUrl.toString().left(120) << "...";
    Q_EMIT progressChanged(QStringLiteral("Uploading PDF..."));
    uploadFile(uploadUrl);
}

void MinerUClient::uploadFile(const QUrl &signedUrl)
{
    QFile *file = new QFile(m_filePath, this);
    if (!file->open(QIODevice::ReadOnly)) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot open file: %1").arg(m_filePath));
        file->deleteLater();
        return;
    }

    qDebug() << "[MinerU] uploadFile: file size =" << file->size() << "bytes, URL =" << signedUrl.toString().left(120) << "...";

    QNetworkRequest request(signedUrl);

    QNetworkReply *reply = m_nam->put(request, file);

    // Track upload progress
    connect(reply, &QNetworkReply::uploadProgress, this, [](qint64 sent, qint64 total) {
        qDebug() << "[MinerU] Upload progress:" << sent << "/" << total << "bytes";
    });
    connect(reply, &QNetworkReply::errorOccurred, this, [](QNetworkReply::NetworkError code) {
        qDebug() << "[MinerU] Upload network error code:" << code;
    });
    connect(reply, &QNetworkReply::sslErrors, this, [](const QList<QSslError> &errors) {
        for (const auto &e : errors) {
            qDebug() << "[MinerU] SSL error:" << e.errorString();
        }
    });

    // Keep file alive until upload completes
    connect(reply, &QNetworkReply::finished, file, &QFile::deleteLater);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { onFileUploaded(reply); });
}

void MinerUClient::onFileUploaded(QNetworkReply *reply)
{
    reply->deleteLater();

    qDebug() << "[MinerU] onFileUploaded: HTTP status" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
             << "error:" << reply->error() << reply->errorString();

    if (reply->error() != QNetworkReply::NoError) {
        QByteArray body = reply->readAll();
        qDebug() << "[MinerU] Upload error response body:" << body.left(500);
        Q_EMIT errorOccurred(QStringLiteral("File upload failed: %1").arg(reply->errorString()));
        return;
    }

    qDebug() << "[MinerU] File uploaded successfully, starting to poll batch_id:" << m_currentTaskId;
    Q_EMIT progressChanged(QStringLiteral("PDF uploaded. Parsing..."));

    m_pollCount = 0;
    m_pollTimer->start();
}

void MinerUClient::pollOnce()
{
    m_pollCount++;
    qDebug() << "[MinerU] pollOnce: attempt" << m_pollCount << "/" << MAX_POLL_ATTEMPTS;
    if (m_pollCount > MAX_POLL_ATTEMPTS) {
        m_pollTimer->stop();
        Q_EMIT errorOccurred(QStringLiteral("MinerU parsing timed out after %1 attempts.").arg(MAX_POLL_ATTEMPTS));
        return;
    }

    QUrl url(MINERU_BASE_URL);
    url.setPath(QStringLiteral("/api/v4/extract-results/batch/%1").arg(m_currentTaskId));

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(m_config.apiToken).toUtf8());

    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { onPollReply(reply); });
}

void MinerUClient::onPollReply(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        // Network error during polling — retry
        qWarning() << "[MinerU] Poll network error:" << reply->errorString();
        return;
    }

    QByteArray responseData = reply->readAll();
    qDebug() << "[MinerU] onPollReply: HTTP" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
             << "body:" << responseData.left(300);

    QJsonDocument doc = QJsonDocument::fromJson(responseData);
    QJsonObject root = doc.object();
    QJsonObject data = root[QStringLiteral("data")].toObject();
    QJsonArray results = data[QStringLiteral("extract_result")].toArray();

    if (results.isEmpty()) {
        qDebug() << "[MinerU] Poll: no results yet, still pending";
        return; // still pending
    }

    QJsonObject firstResult = results[0].toObject();
    QString state = firstResult[QStringLiteral("state")].toString();
    qDebug() << "[MinerU] Poll: state =" << state;

    if (state == QLatin1String("done")) {
        m_pollTimer->stop();
        QString zipUrl = firstResult[QStringLiteral("full_zip_url")].toString();
        if (zipUrl.isEmpty()) {
            Q_EMIT errorOccurred(QStringLiteral("MinerU returned done but no zip URL."));
            return;
        }
        Q_EMIT progressChanged(QStringLiteral("Downloading results..."));
        downloadAndParseResult(QUrl(zipUrl));
    } else if (state == QLatin1String("failed")) {
        m_pollTimer->stop();
        QString errMsg = firstResult[QStringLiteral("err_msg")].toString();
        Q_EMIT errorOccurred(QStringLiteral("MinerU parsing failed: %1").arg(errMsg));
    } else {
        // still running or pending
        QJsonObject progress = firstResult[QStringLiteral("extract_progress")].toObject();
        int extracted = progress[QStringLiteral("extracted_pages")].toInt();
        int total = progress[QStringLiteral("total_pages")].toInt();
        if (total > 0) {
            Q_EMIT progressChanged(QStringLiteral("Parsing page %1/%2...").arg(extracted).arg(total));
        }
    }
}

void MinerUClient::downloadAndParseResult(const QUrl &zipUrl)
{
    qDebug() << "[MinerU] Downloading zip from:" << zipUrl.toString().left(120) << "...";
    QNetworkRequest request(zipUrl);
    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::downloadProgress, this, [](qint64 received, qint64 total) {
        qDebug() << "[MinerU] Download progress:" << received << "/" << total << "bytes";
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { onDownloadReply(reply); });
}

void MinerUClient::onDownloadReply(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        Q_EMIT errorOccurred(QStringLiteral("Download failed: %1").arg(reply->errorString()));
        return;
    }

    qDebug() << "[MinerU] onDownloadReply: HTTP" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
             << "error:" << reply->error();

    QByteArray zipData = reply->readAll();
    qDebug() << "[MinerU] Downloaded zip size:" << zipData.size() << "bytes";
    Q_EMIT progressChanged(QStringLiteral("Extracting results..."));

    QByteArray contentListJson = extractContentListFromZip(zipData);
    qDebug() << "[MinerU] Extracted content_list.json size:" << contentListJson.size() << "bytes";
    if (contentListJson.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Could not find content_list.json in MinerU results."));
        return;
    }

    auto pagesParagraphs = parseContentList(contentListJson);
    qDebug() << "[MinerU] Parsed" << pagesParagraphs.size() << "pages";
    for (auto it = pagesParagraphs.constBegin(); it != pagesParagraphs.constEnd(); ++it) {
        qDebug() << "  Page" << it.key() << ":" << it.value().size() << "paragraphs";
    }

    Q_EMIT documentReady(pagesParagraphs);
}

QByteArray MinerUClient::extractContentListFromZip(const QByteArray &zipData)
{
    // Write zip to temp file, extract using system unzip
    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        qWarning() << "[MinerU] Cannot create temp directory";
        return {};
    }

    QString zipPath = tmpDir.path() + QStringLiteral("/result.zip");
    QFile zipFile(zipPath);
    if (!zipFile.open(QIODevice::WriteOnly)) {
        qWarning() << "[MinerU] Cannot write temp zip file";
        return {};
    }
    zipFile.write(zipData);
    zipFile.close();

    // Extract zip
    QProcess unzip;
    unzip.setWorkingDirectory(tmpDir.path());
    unzip.start(QStringLiteral("unzip"), {QStringLiteral("-o"), zipPath, QStringLiteral("-d"), tmpDir.path()});
    unzip.waitForFinished(30000);

    if (unzip.exitCode() != 0) {
        qWarning() << "[MinerU] unzip failed:" << unzip.readAllStandardError();
        return {};
    }

    // Find *content_list.json recursively
    QProcess find;
    find.setWorkingDirectory(tmpDir.path());
    find.start(QStringLiteral("find"), {tmpDir.path(), QStringLiteral("-name"), QStringLiteral("*content_list.json")});
    find.waitForFinished(5000);

    QString output = QString::fromUtf8(find.readAllStandardOutput()).trimmed();
    QStringList files = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    if (files.isEmpty()) {
        qWarning() << "[MinerU] No content_list.json found in zip";
        return {};
    }

    // Read the first content_list.json found
    QFile contentFile(files.first());
    if (!contentFile.open(QIODevice::ReadOnly)) {
        qWarning() << "[MinerU] Cannot read content_list.json";
        return {};
    }

    return contentFile.readAll();
}

QMap<int, QList<ParagraphData>> MinerUClient::parseContentList(const QByteArray &jsonData)
{
    QMap<int, QList<ParagraphData>> result;

    QJsonDocument doc = QJsonDocument::fromJson(jsonData);
    if (!doc.isArray()) {
        qWarning() << "[MinerU] content_list.json is not a JSON array";
        return result;
    }

    QJsonArray items = doc.array();
    // Track paragraph index per page
    QMap<int, int> pageParaCount;

    for (const auto &val : items) {
        QJsonObject item = val.toObject();
        QString type = item[QStringLiteral("type")].toString();

        // Only process text paragraphs
        if (type != QLatin1String("text") && type != QLatin1String("list")) {
            continue;
        }

        // Skip headings (text_level > 0) and short text
        int textLevel = item[QStringLiteral("text_level")].toInt(0);
        if (textLevel > 0) {
            continue;
        }

        QString text = item[QStringLiteral("text")].toString();
        if (text.length() < 30) {
            continue;
        }

        int pageIdx = item[QStringLiteral("page_idx")].toInt();
        QJsonArray bbox = item[QStringLiteral("bbox")].toArray();
        if (bbox.size() < 4) {
            continue;
        }

        // Convert MinerU bbox [x0,y0,x1,y1] (0-1000) to NormalizedRect (0-1)
        double left = bbox[0].toDouble() / 1000.0;
        double top = bbox[1].toDouble() / 1000.0;
        double right = bbox[2].toDouble() / 1000.0;
        double bottom = bbox[3].toDouble() / 1000.0;

        ParagraphData para;
        para.paragraphIdx = pageParaCount.value(pageIdx, 0);
        para.text = text;
        para.bbox = Okular::NormalizedRect(left, top, right, bottom);

        // Column detection
        double centerX = (left + right) / 2.0;
        para.isLeftColumn = (centerX < 0.5);

        result[pageIdx].append(para);
        pageParaCount[pageIdx] = para.paragraphIdx + 1;
    }

    return result;
}
