/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/Log.h"

#include <QObject>
#include <QTemporaryDir>

class ServerProxyTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void handleKeepAliveAlarm_timeout_queuesDisconnectRequest();
  void handleData_incompleteMessage_queuesDisconnectRequest();
  void handleData_largeBurst_yieldsAndPreservesMessageOrder();
  void parseHandshakeMessage_protocolError_queuesRefusalRequest();
  void macShortcuts_disabled_preservesServerMapping();
  void macShortcuts_settingEnablesWindowsClient();
  void macShortcuts_common_data();
  void macShortcuts_common();
  void macShortcuts_otherCombinationsKeepCommand();
  void macShortcuts_mixedCombinationsReleaseOriginalCommand();
  void macShortcuts_bareAndBothCommandKeys();
  void macShortcuts_shiftAndOtherModifiers();
  void macShortcuts_pointerAndLeave();
  void macShortcuts_nativeMappingRestoresCtrl();

private:
  Log m_log;
  QTemporaryDir m_settings;
};
