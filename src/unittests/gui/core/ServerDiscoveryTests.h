// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QTest>

class ServerDiscoveryTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void discoverOnlyBoundServer();
  void rejectUnsolicitedAndMalformedReplies();
  void findMovedServerWithoutBroadcast();
};
