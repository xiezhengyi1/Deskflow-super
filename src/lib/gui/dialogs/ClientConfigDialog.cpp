/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClientConfigDialog.h"
#include "ui_ClientConfigDialog.h"

#include "common/Settings.h"
#include "common/PlatformInfo.h"
#include "gui/widgets/SettingsDialogButtonBox.h"

#include <QPushButton>
#include <QCheckBox>
#include <QLabel>
#include <QVBoxLayout>

ClientConfigDialog::ClientConfigDialog(QWidget *parent)
    : QDialog(parent),
      ui(new Ui::ClientConfigDialog),
      m_buttonBox{new SettingsDialogButtonBox(this)}
{
  ui->setupUi(this);
  ui->cbMacShortcuts->setVisible(deskflow::platform::isWindows());
  m_autoDiscover = new QCheckBox(tr("Automatically find the bound server on the local network"), this);
  m_bindingLabel = new QLabel(this);
  m_bindingLabel->setWordWrap(true);
  m_forget = new QPushButton(tr("Forget server binding"), this);
  m_forget->setAutoDefault(false);
  auto *box = qobject_cast<QVBoxLayout *>(layout());
  box->insertWidget(0, m_autoDiscover);
  box->insertWidget(1, m_bindingLabel);
  box->insertWidget(2, m_forget);
  connect(m_autoDiscover, &QCheckBox::toggled, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(m_forget, &QPushButton::clicked, this, [this] {
    m_forgetBinding = true;
    updateBindingText();
    setButtonBoxEnabledButtons();
  });
  layout()->addWidget(m_buttonBox);
  initConnections();
  load();
  updateControls();
  setButtonBoxEnabledButtons();
}

ClientConfigDialog::~ClientConfigDialog()
{
  delete ui;
}

void ClientConfigDialog::changeEvent(QEvent *e)
{
  QDialog::changeEvent(e);
  if (e->type() == QEvent::LanguageChange)
    ui->retranslateUi(this);
}

void ClientConfigDialog::updateControls() const
{
  const auto writable = Settings::isWritable();
  m_autoDiscover->setEnabled(writable);
  updateBindingText();
  ui->cbDynamicConnectTime->setEnabled(writable);
  ui->cbLanguageSync->setEnabled(writable);
  ui->cbMacShortcuts->setEnabled(writable);
  ui->cbXScrollInvert->setEnabled(writable);
  ui->sbXScrollScale->setEnabled(writable);
  ui->cbYScrollInvert->setEnabled(writable);
  ui->sbYScrollScale->setEnabled(writable);
}

void ClientConfigDialog::initConnections() const
{
  connect(m_buttonBox, &SettingsDialogButtonBox::accepted, this, &ClientConfigDialog::save);
  connect(m_buttonBox, &SettingsDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_buttonBox, &SettingsDialogButtonBox::reset, this, &ClientConfigDialog::load);
  connect(m_buttonBox, &SettingsDialogButtonBox::restoreDefault, this, &ClientConfigDialog::resetToDefault);

  connect(
      ui->cbDynamicConnectTime, &QCheckBox::checkStateChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons
  );
  connect(ui->cbLanguageSync, &QCheckBox::checkStateChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->cbMacShortcuts, &QCheckBox::checkStateChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->cbYScrollInvert, &QCheckBox::checkStateChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->sbYScrollScale, &QDoubleSpinBox::valueChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->cbXScrollInvert, &QCheckBox::checkStateChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->sbXScrollScale, &QDoubleSpinBox::valueChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(Settings::instance(), &Settings::settingsWritableChanged, this, &ClientConfigDialog::updateControls);
}

bool ClientConfigDialog::isModified() const
{
  return m_forgetBinding || (m_autoDiscover->isChecked() != Settings::value(Settings::Client::AutoDiscover).toBool()) ||
         (ui->cbDynamicConnectTime->isChecked() != Settings::value(Settings::Client::DynamicConnectionRetry).toBool()
         ) ||
         (ui->cbLanguageSync->isChecked() != Settings::value(Settings::Client::LanguageSync).toBool()) ||
         (ui->cbMacShortcuts->isChecked() != Settings::value(Settings::Client::MacShortcuts).toBool()) ||
         (ui->cbYScrollInvert->isChecked() != Settings::value(Settings::Client::InvertYScroll).toBool()) ||
         (ui->sbYScrollScale->value() != Settings::value(Settings::Client::YScrollScale).toDouble()) ||
         (ui->cbXScrollInvert->isChecked() != Settings::value(Settings::Client::InvertXScroll).toBool()) ||
         (ui->sbXScrollScale->value() != Settings::value(Settings::Client::XScrollScale).toDouble());
}

bool ClientConfigDialog::isDefault() const
{
  return m_autoDiscover->isChecked() == Settings::defaultValue(Settings::Client::AutoDiscover).toBool() &&
         (ui->cbDynamicConnectTime->isChecked() ==
          Settings::defaultValue(Settings::Client::DynamicConnectionRetry).toBool()) &&
         (ui->cbLanguageSync->isChecked() == Settings::defaultValue(Settings::Client::LanguageSync).toBool()) &&
         (ui->cbMacShortcuts->isChecked() == Settings::defaultValue(Settings::Client::MacShortcuts).toBool()) &&
         (ui->cbYScrollInvert->isChecked() == Settings::defaultValue(Settings::Client::InvertYScroll).toBool()) &&
         (ui->sbYScrollScale->value() == Settings::defaultValue(Settings::Client::YScrollScale).toDouble()) &&
         (ui->cbXScrollInvert->isChecked() == Settings::defaultValue(Settings::Client::InvertXScroll).toBool()) &&
         (ui->sbXScrollScale->value() == Settings::defaultValue(Settings::Client::XScrollScale).toDouble());
}

void ClientConfigDialog::setButtonBoxEnabledButtons() const
{
  const bool modified = isModified();
  m_buttonBox->enableSave(modified);
  m_buttonBox->enableReset(modified);
  m_buttonBox->enableRestoreDefaults(!isDefault());
}

void ClientConfigDialog::updateBindingText() const
{
  const auto fingerprint = Settings::value(Settings::Client::BoundServerFingerprint).toString();
  m_bindingLabel->setText(m_forgetBinding
      ? tr("Binding and saved address will be cleared when you save.")
      : fingerprint.isEmpty() ? tr("Not bound. The first successful secure connection binds the server automatically.")
                             : tr("Bound server certificate: %1").arg(fingerprint.left(16) + "…"));
  m_forget->setEnabled(Settings::isWritable() && !fingerprint.isEmpty() && !m_forgetBinding);
}

void ClientConfigDialog::load()
{
  m_forgetBinding = false;
  m_autoDiscover->setChecked(Settings::value(Settings::Client::AutoDiscover).toBool());
  updateBindingText();
  ui->cbDynamicConnectTime->setChecked(Settings::value(Settings::Client::DynamicConnectionRetry).toBool());
  ui->cbLanguageSync->setChecked(Settings::value(Settings::Client::LanguageSync).toBool());
  ui->cbMacShortcuts->setChecked(Settings::value(Settings::Client::MacShortcuts).toBool());
  ui->cbYScrollInvert->setChecked(Settings::value(Settings::Client::InvertYScroll).toBool());
  ui->sbYScrollScale->setValue(Settings::value(Settings::Client::YScrollScale).toDouble());
  ui->cbXScrollInvert->setChecked(Settings::value(Settings::Client::InvertXScroll).toBool());
  ui->sbXScrollScale->setValue(Settings::value(Settings::Client::XScrollScale).toDouble());
}

void ClientConfigDialog::resetToDefault()
{
  m_autoDiscover->setChecked(Settings::defaultValue(Settings::Client::AutoDiscover).toBool());
  ui->cbDynamicConnectTime->setChecked(Settings::defaultValue(Settings::Client::DynamicConnectionRetry).toBool());
  ui->cbLanguageSync->setChecked(Settings::defaultValue(Settings::Client::LanguageSync).toBool());
  ui->cbMacShortcuts->setChecked(Settings::defaultValue(Settings::Client::MacShortcuts).toBool());
  ui->cbYScrollInvert->setChecked(Settings::defaultValue(Settings::Client::InvertYScroll).toBool());
  ui->sbYScrollScale->setValue(Settings::defaultValue(Settings::Client::YScrollScale).toDouble());
  ui->cbXScrollInvert->setChecked(Settings::defaultValue(Settings::Client::InvertXScroll).toBool());
  ui->sbXScrollScale->setValue(Settings::defaultValue(Settings::Client::XScrollScale).toDouble());
}

void ClientConfigDialog::save()
{
  Settings::setValue(Settings::Client::AutoDiscover, m_autoDiscover->isChecked());
  if (m_forgetBinding) {
    Settings::setValue(Settings::Client::BindingInitialized, true);
    Settings::setValue(Settings::Client::BoundServerFingerprint, QString());
    Settings::setValue(Settings::Client::RemoteHost, QString());
  }
  Settings::setValue(Settings::Client::DynamicConnectionRetry, ui->cbDynamicConnectTime->isChecked());
  Settings::setValue(Settings::Client::LanguageSync, ui->cbLanguageSync->isChecked());
  Settings::setValue(Settings::Client::MacShortcuts, ui->cbMacShortcuts->isChecked());
  Settings::setValue(Settings::Client::InvertYScroll, ui->cbYScrollInvert->isChecked());
  Settings::setValue(Settings::Client::YScrollScale, ui->sbYScrollScale->value());
  Settings::setValue(Settings::Client::InvertXScroll, ui->cbXScrollInvert->isChecked());
  Settings::setValue(Settings::Client::XScrollScale, ui->sbXScrollScale->value());
  Settings::save();
  QDialog::accept();
}
