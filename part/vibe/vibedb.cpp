/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vibedb.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QDir>

using namespace Vibe;

VibeDB::VibeDB()
{
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    m_dbPath = dataDir + QStringLiteral("/vibe.db");
}

VibeDB::~VibeDB()
{
    close();
}

bool VibeDB::open()
{
    if (m_open) {
        return true;
    }

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("vibe_connection"));
    m_db.setDatabaseName(m_dbPath);

    if (!m_db.open()) {
        qWarning() << "[VibeDB] Failed to open database:" << m_db.lastError().text();
        return false;
    }

    createTables();
    m_open = true;
    return true;
}

void VibeDB::close()
{
    if (m_open) {
        m_db.close();
        m_open = false;
    }
    QSqlDatabase::removeDatabase(QStringLiteral("vibe_connection"));
}

void VibeDB::createTables()
{
    QSqlQuery q(m_db);

    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS papers ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  file_hash TEXT UNIQUE NOT NULL,"
        "  created_at TEXT DEFAULT (datetime('now')),"
        "  updated_at TEXT DEFAULT (datetime('now'))"
        ")"
    ));

    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS paragraphs ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  paper_id INTEGER NOT NULL,"
        "  page_idx INTEGER NOT NULL,"
        "  paragraph_idx INTEGER NOT NULL,"
        "  text TEXT,"
        "  paragraph_summary TEXT,"
        "  bbox_json TEXT,"
        "  is_left_column INTEGER DEFAULT 1,"
        "  UNIQUE(paper_id, page_idx, paragraph_idx),"
        "  FOREIGN KEY(paper_id) REFERENCES papers(id)"
        ")"
    ));

    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS points ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  paragraph_id INTEGER NOT NULL,"
        "  point_idx INTEGER NOT NULL,"
        "  point_summary TEXT,"
        "  sentence_indices_json TEXT,"
        "  importance_level INTEGER DEFAULT 1,"
        "  language TEXT DEFAULT 'en',"
        "  UNIQUE(paragraph_id, point_idx, language),"
        "  FOREIGN KEY(paragraph_id) REFERENCES paragraphs(id)"
        ")"
    ));

    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS summary_cards ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  paper_id INTEGER NOT NULL,"
        "  page_idx INTEGER NOT NULL,"
        "  paragraph_id INTEGER NOT NULL,"
        "  position_json TEXT,"
        "  is_left_column INTEGER DEFAULT 1,"
        "  language TEXT DEFAULT 'en',"
        "  paragraph_summary TEXT,"
        "  created_at TEXT DEFAULT (datetime('now')),"
        "  updated_at TEXT DEFAULT (datetime('now')),"
        "  FOREIGN KEY(paper_id) REFERENCES papers(id),"
        "  FOREIGN KEY(paragraph_id) REFERENCES paragraphs(id)"
        ")"
    ));
}

int VibeDB::getOrCreatePaper(const QString &fileHash)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM papers WHERE file_hash = ?"));
    q.addBindValue(fileHash);
    if (q.exec() && q.next()) {
        return q.value(0).toInt();
    }

    q.prepare(QStringLiteral("INSERT INTO papers (file_hash) VALUES (?)"));
    q.addBindValue(fileHash);
    if (q.exec()) {
        return q.lastInsertId().toInt();
    }

    qWarning() << "[VibeDB] Failed to create paper:" << q.lastError().text();
    return -1;
}

void VibeDB::saveParagraphs(int paperId, int pageIdx, const QList<ParagraphData> &paragraphs)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO paragraphs (paper_id, page_idx, paragraph_idx, text, paragraph_summary, bbox_json, is_left_column) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)"
    ));

    for (const auto &p : paragraphs) {
        q.addBindValue(paperId);
        q.addBindValue(pageIdx);
        q.addBindValue(p.paragraphIdx);
        q.addBindValue(p.text);
        q.addBindValue(p.summary);

        QJsonObject bboxObj;
        bboxObj[QStringLiteral("left")] = p.bbox.left;
        bboxObj[QStringLiteral("top")] = p.bbox.top;
        bboxObj[QStringLiteral("right")] = p.bbox.right;
        bboxObj[QStringLiteral("bottom")] = p.bbox.bottom;
        q.addBindValue(QString::fromUtf8(QJsonDocument(bboxObj).toJson(QJsonDocument::Compact)));

        q.addBindValue(p.isLeftColumn ? 1 : 0);

        if (!q.exec()) {
            qWarning() << "[VibeDB] Failed to save paragraph:" << q.lastError().text();
        }
    }
}

QList<ParagraphData> VibeDB::getParagraphs(int paperId, int pageIdx) const
{
    QList<ParagraphData> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT paragraph_idx, text, paragraph_summary, bbox_json, is_left_column "
        "FROM paragraphs WHERE paper_id = ? AND page_idx = ? ORDER BY paragraph_idx"
    ));
    q.addBindValue(paperId);
    q.addBindValue(pageIdx);

    if (!q.exec()) {
        return result;
    }

    while (q.next()) {
        ParagraphData p;
        p.paragraphIdx = q.value(0).toInt();
        p.text = q.value(1).toString();
        p.summary = q.value(2).toString();

        QJsonObject bboxObj = QJsonDocument::fromJson(q.value(3).toString().toUtf8()).object();
        p.bbox = Okular::NormalizedRect(
            bboxObj[QStringLiteral("left")].toDouble(),
            bboxObj[QStringLiteral("top")].toDouble(),
            bboxObj[QStringLiteral("right")].toDouble(),
            bboxObj[QStringLiteral("bottom")].toDouble()
        );

        p.isLeftColumn = q.value(4).toInt() != 0;

        result.append(p);
    }
    return result;
}

int VibeDB::getParagraphDbId(int paperId, int pageIdx, int paragraphIdx) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id FROM paragraphs WHERE paper_id = ? AND page_idx = ? AND paragraph_idx = ?"
    ));
    q.addBindValue(paperId);
    q.addBindValue(pageIdx);
    q.addBindValue(paragraphIdx);
    if (q.exec() && q.next()) {
        return q.value(0).toInt();
    }
    return -1;
}

void VibeDB::savePoints(int paragraphId, const QString &language, const QList<PointData> &points)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO points (paragraph_id, point_idx, point_summary, sentence_indices_json, importance_level, language) "
        "VALUES (?, ?, ?, ?, ?, ?)"
    ));

    for (const auto &pt : points) {
        q.addBindValue(paragraphId);
        q.addBindValue(pt.pointIdx);
        q.addBindValue(pt.summary);

        QJsonArray indices;
        for (int idx : pt.sentenceIndices) {
            indices.append(idx);
        }
        q.addBindValue(QString::fromUtf8(QJsonDocument(indices).toJson(QJsonDocument::Compact)));
        q.addBindValue(pt.importanceLevel);
        q.addBindValue(language);

        if (!q.exec()) {
            qWarning() << "[VibeDB] Failed to save point:" << q.lastError().text();
        }
    }
}

QList<PointData> VibeDB::getPoints(int paragraphId, const QString &language) const
{
    QList<PointData> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT point_idx, point_summary, sentence_indices_json, importance_level "
        "FROM points WHERE paragraph_id = ? AND language = ? ORDER BY point_idx"
    ));
    q.addBindValue(paragraphId);
    q.addBindValue(language);

    if (!q.exec()) {
        return result;
    }

    while (q.next()) {
        PointData pt;
        pt.pointIdx = q.value(0).toInt();
        pt.summary = q.value(1).toString();

        QJsonArray indices = QJsonDocument::fromJson(q.value(2).toString().toUtf8()).array();
        for (const auto &v : indices) {
            pt.sentenceIndices.append(v.toInt());
        }
        pt.importanceLevel = q.value(3).toInt();
        result.append(pt);
    }
    return result;
}

void VibeDB::deleteLlmDataForPage(int paperId, int pageIdx, const QString &language)
{
    QSqlQuery q(m_db);

    // Delete summary_cards for this page and language
    q.prepare(QStringLiteral("DELETE FROM summary_cards WHERE paper_id = ? AND page_idx = ? AND language = ?"));
    q.addBindValue(paperId);
    q.addBindValue(pageIdx);
    q.addBindValue(language);
    q.exec();

    // Delete points for paragraphs on this page and language
    q.prepare(QStringLiteral(
        "DELETE FROM points WHERE language = ? AND paragraph_id IN "
        "(SELECT id FROM paragraphs WHERE paper_id = ? AND page_idx = ?)"));
    q.addBindValue(language);
    q.addBindValue(paperId);
    q.addBindValue(pageIdx);
    q.exec();

    qDebug() << "[VibeDB] Deleted LLM data for paper" << paperId << "page" << pageIdx << "language" << language;
}

void VibeDB::saveSummaryCard(int paperId, int pageIdx, int paragraphId, const Okular::NormalizedRect &anchor, bool isLeftColumn, const QString &language, const QString &paragraphSummary)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO summary_cards (paper_id, page_idx, paragraph_id, position_json, is_left_column, language, paragraph_summary) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)"
    ));
    q.addBindValue(paperId);
    q.addBindValue(pageIdx);
    q.addBindValue(paragraphId);

    QJsonObject posObj;
    posObj[QStringLiteral("left")] = anchor.left;
    posObj[QStringLiteral("top")] = anchor.top;
    posObj[QStringLiteral("right")] = anchor.right;
    posObj[QStringLiteral("bottom")] = anchor.bottom;
    q.addBindValue(QString::fromUtf8(QJsonDocument(posObj).toJson(QJsonDocument::Compact)));
    q.addBindValue(isLeftColumn ? 1 : 0);
    q.addBindValue(language);
    q.addBindValue(paragraphSummary);

    if (!q.exec()) {
        qWarning() << "[VibeDB] Failed to save summary card:" << q.lastError().text();
    }
}

QList<SummaryCardData> VibeDB::getSummaryCards(int paperId, int pageIdx, const QString &language) const
{
    QList<SummaryCardData> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT sc.id, sc.paragraph_id, p.paragraph_idx, sc.paragraph_summary, sc.position_json, sc.is_left_column, sc.language "
        "FROM summary_cards sc "
        "JOIN paragraphs p ON sc.paragraph_id = p.id "
        "WHERE sc.paper_id = ? AND sc.page_idx = ? AND sc.language = ? "
        "ORDER BY p.paragraph_idx"
    ));
    q.addBindValue(paperId);
    q.addBindValue(pageIdx);
    q.addBindValue(language);

    if (!q.exec()) {
        return result;
    }

    while (q.next()) {
        SummaryCardData card;
        card.id = q.value(0).toInt();
        card.paragraphId = q.value(1).toInt();
        card.pageIdx = pageIdx;
        card.paragraphIdx = q.value(2).toInt();
        card.paragraphSummary = q.value(3).toString();

        QJsonObject posObj = QJsonDocument::fromJson(q.value(4).toString().toUtf8()).object();
        card.anchorRect = Okular::NormalizedRect(
            posObj[QStringLiteral("left")].toDouble(),
            posObj[QStringLiteral("top")].toDouble(),
            posObj[QStringLiteral("right")].toDouble(),
            posObj[QStringLiteral("bottom")].toDouble()
        );

        card.isLeftColumn = q.value(5).toInt() != 0;
        card.points = getPoints(card.paragraphId, language);
        result.append(card);
    }
    return result;
}
