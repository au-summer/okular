/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vibeconfigdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

using namespace Vibe;

VibeConfigDialog::VibeConfigDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Vibe AI Settings"));
    setMinimumWidth(450);

    auto *layout = new QVBoxLayout(this);

    auto *form = new QFormLayout;

    // OpenAI API Key
    m_openAiKeyEdit = new QLineEdit(this);
    m_openAiKeyEdit->setEchoMode(QLineEdit::Password);
    m_openAiKeyEdit->setPlaceholderText(tr("sk-... (or set VIBE_OPENAI_API_KEY env var)"));
    form->addRow(tr("OpenAI API Key:"), m_openAiKeyEdit);

    // Model selection
    m_modelCombo = new QComboBox(this);
    m_modelCombo->setEditable(true);
    m_modelCombo->addItems({
        QStringLiteral("gpt-5.4"),
        QStringLiteral("gpt-5.4-mini"),
        QStringLiteral("gpt-5.4-nano"),
        QStringLiteral("gpt-5"),
        QStringLiteral("gpt-5-mini"),
        QStringLiteral("gpt-5-nano"),
        QStringLiteral("gpt-4.1"),
        QStringLiteral("gpt-4.1-mini"),
        QStringLiteral("gpt-4o-mini"),
        QStringLiteral("o3"),
        QStringLiteral("o3-mini"),
    });
    form->addRow(tr("Model:"), m_modelCombo);

    // Base URL
    m_baseUrlEdit = new QLineEdit(this);
    m_baseUrlEdit->setPlaceholderText(QStringLiteral("https://api.openai.com/v1"));
    form->addRow(tr("Base URL:"), m_baseUrlEdit);

    // MinerU Token
    m_mineruTokenEdit = new QLineEdit(this);
    m_mineruTokenEdit->setEchoMode(QLineEdit::Password);
    m_mineruTokenEdit->setPlaceholderText(tr("(or set VIBE_MINERU_TOKEN env var)"));
    form->addRow(tr("MinerU Token:"), m_mineruTokenEdit);

    // Card font sizes
    m_summaryFontSizeSpin = new QSpinBox(this);
    m_summaryFontSizeSpin->setRange(4, 32);
    m_summaryFontSizeSpin->setValue(8);
    m_summaryFontSizeSpin->setSuffix(QStringLiteral(" px"));
    form->addRow(tr("Summary card font size:"), m_summaryFontSizeSpin);

    m_pointsFontSizeSpin = new QSpinBox(this);
    m_pointsFontSizeSpin->setRange(4, 32);
    m_pointsFontSizeSpin->setValue(7);
    m_pointsFontSizeSpin->setSuffix(QStringLiteral(" px"));
    form->addRow(tr("Points card font size:"), m_pointsFontSizeSpin);

    // Summary language
    m_languageCombo = new QComboBox(this);
    m_languageCombo->addItem(QStringLiteral("English"), QStringLiteral("en"));
    m_languageCombo->addItem(QStringLiteral("Chinese"), QStringLiteral("zh"));
    form->addRow(tr("Summary language:"), m_languageCombo);

    layout->addLayout(form);

    auto *hint = new QLabel(tr("Environment variables take precedence over these settings."), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
    layout->addWidget(hint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        saveSettings();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    loadSettings();
}

void VibeConfigDialog::loadSettings()
{
    QSettings settings(QStringLiteral("okular-vibe"), QStringLiteral("okular-vibe"));
    m_openAiKeyEdit->setText(settings.value(QStringLiteral("apiKey")).toString());
    m_baseUrlEdit->setText(settings.value(QStringLiteral("baseUrl")).toString());
    m_mineruTokenEdit->setText(settings.value(QStringLiteral("mineruToken")).toString());

    const QString model = settings.value(QStringLiteral("model")).toString();
    if (!model.isEmpty()) {
        m_modelCombo->setCurrentText(model);
    }

    m_summaryFontSizeSpin->setValue(settings.value(QStringLiteral("summaryFontSize"), 8).toInt());
    m_pointsFontSizeSpin->setValue(settings.value(QStringLiteral("pointsFontSize"), 7).toInt());

    const QString lang = settings.value(QStringLiteral("summaryLanguage"), QStringLiteral("en")).toString();
    int langIdx = m_languageCombo->findData(lang);
    if (langIdx >= 0) {
        m_languageCombo->setCurrentIndex(langIdx);
    }
}

void VibeConfigDialog::saveSettings()
{
    QSettings settings(QStringLiteral("okular-vibe"), QStringLiteral("okular-vibe"));
    settings.setValue(QStringLiteral("apiKey"), m_openAiKeyEdit->text().trimmed());
    settings.setValue(QStringLiteral("model"), m_modelCombo->currentText().trimmed());
    settings.setValue(QStringLiteral("baseUrl"), m_baseUrlEdit->text().trimmed());
    settings.setValue(QStringLiteral("mineruToken"), m_mineruTokenEdit->text().trimmed());
    settings.setValue(QStringLiteral("summaryFontSize"), m_summaryFontSizeSpin->value());
    settings.setValue(QStringLiteral("pointsFontSize"), m_pointsFontSizeSpin->value());
    settings.setValue(QStringLiteral("summaryLanguage"), m_languageCombo->currentData().toString());
}

QString VibeConfigDialog::openAiApiKey() const
{
    return m_openAiKeyEdit->text();
}

QString VibeConfigDialog::openAiModel() const
{
    return m_modelCombo->currentText();
}

QString VibeConfigDialog::openAiBaseUrl() const
{
    return m_baseUrlEdit->text();
}

QString VibeConfigDialog::mineruToken() const
{
    return m_mineruTokenEdit->text();
}
