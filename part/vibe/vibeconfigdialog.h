/*
    SPDX-FileCopyrightText: 2026 Vibe Contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef VIBE_CONFIG_DIALOG_H
#define VIBE_CONFIG_DIALOG_H

#include <QDialog>

class QLineEdit;
class QComboBox;
class QSpinBox;

namespace Vibe
{

class VibeConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit VibeConfigDialog(QWidget *parent = nullptr);

    QString openAiApiKey() const;
    QString openAiModel() const;
    QString openAiBaseUrl() const;
    QString mineruToken() const;

private:
    void loadSettings();
    void saveSettings();

    QLineEdit *m_openAiKeyEdit;
    QComboBox *m_modelCombo;
    QLineEdit *m_baseUrlEdit;
    QLineEdit *m_mineruTokenEdit;
    QSpinBox *m_summaryFontSizeSpin;
    QSpinBox *m_pointsFontSizeSpin;
};

} // namespace Vibe

#endif // VIBE_CONFIG_DIALOG_H
