// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

class WifiPairing {
 public:
  enum class State : uint8_t {
    Offline,
    Connecting,
    Portal,
    Connected,
    Failed,
  };

  void begin(const String& savedSsid, const String& savedPassword,
             uint32_t nowMs);
  void update(uint32_t nowMs);
  void startPortal(uint32_t nowMs);
  void cancelPortal();
  void retry(uint32_t nowMs);
  void forget();

  bool takeNewCredentials(String& ssid, String& password);
  bool consumeStateChanged();
  bool hasCredentials() const { return !ssid_.isEmpty(); }
  bool portalActive() const { return state_ == State::Portal; }
  bool connected() const { return state_ == State::Connected; }
  State state() const { return state_; }
  const char* stateName() const;
  const String& accessPointName() const { return accessPointName_; }
  const String& accessPointCode() const { return accessPointCode_; }
  const char* accessPointPassword() const { return kAccessPointPassword; }
  String localIp() const;
  float signalLevel() const;

 private:
  static constexpr uint16_t kDnsPort = 53;
  static constexpr uint32_t kConnectTimeoutMs = 15000;
  static constexpr uint32_t kPortalTimeoutMs = 5UL * 60UL * 1000UL;
  static constexpr char kAccessPointPassword[] = "kkfriend";

  void configureServer();
  void beginConnection(const String& ssid, const String& password,
                       uint32_t nowMs, bool candidate = false);
  void stopPortalServices();
  void setState(State next);
  void sendPortalPage(const String& message = "");
  void handleCredentialSave();
  void redirectToPortal();
  static String escapeHtml(const String& value);

  DNSServer dnsServer_;
  WebServer webServer_{80};
  State state_ = State::Offline;
  bool stateChanged_ = true;
  bool serverConfigured_ = false;
  bool portalServicesActive_ = false;
  bool credentialsReady_ = false;
  bool candidateConnection_ = false;
  String ssid_;
  String password_;
  String pendingSsid_;
  String pendingPassword_;
  String accessPointName_;
  String accessPointCode_;
  uint32_t connectionStartedMs_ = 0;
  uint32_t portalStartedMs_ = 0;
  uint32_t submitConnectAtMs_ = 0;
};
