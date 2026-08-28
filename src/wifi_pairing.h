// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

struct WifiProfile {
  String ssid;
  String password;
};

class WifiPairing {
 public:
  static constexpr uint8_t kMaxProfiles = 5;

  enum class State : uint8_t {
    Offline,
    Connecting,
    Portal,
    Connected,
    Failed,
  };

  void begin(const WifiProfile* profiles, uint8_t profileCount,
             uint32_t nowMs);
  void update(uint32_t nowMs);
  void startPortal(uint32_t nowMs);
  void cancelPortal();
  void retry(uint32_t nowMs);
  void forget();

  bool takeProfilesChanged(WifiProfile* profiles, uint8_t& profileCount);
  bool consumeStateChanged();
  bool hasCredentials() const { return profileCount_ > 0; }
  bool portalActive() const { return state_ == State::Portal; }
  bool connected() const { return state_ == State::Connected; }
  State state() const { return state_; }
  const char* stateName() const;
  const String& accessPointName() const { return accessPointName_; }
  const String& accessPointCode() const { return accessPointCode_; }
  const char* accessPointPassword() const { return kAccessPointPassword; }
  uint8_t profileCount() const { return profileCount_; }
  String currentSsid() const;
  String localIp() const;
  float signalLevel() const;

 private:
  static constexpr uint16_t kDnsPort = 53;
  static constexpr uint32_t kConnectTimeoutMs = 15000;
  static constexpr uint32_t kScanTimeoutMs = 12000;
  static constexpr uint32_t kPortalTimeoutMs = 5UL * 60UL * 1000UL;
  static constexpr char kAccessPointPassword[] = "kkfriend";

  void configureServer();
  void startKnownNetworkSearch(uint32_t nowMs);
  void beginProfileConnection(uint8_t profileIndex, uint32_t nowMs);
  void beginCandidateConnection(const String& ssid, const String& password,
                                uint32_t nowMs);
  void handleScan(uint32_t nowMs);
  void stopPortalServices();
  void setState(State next);
  void sendPortalPage(const String& message = "");
  void handleCredentialSave();
  void handleProfileDelete();
  void redirectToPortal();
  int8_t findProfile(const String& ssid) const;
  bool upsertProfile(const String& ssid, const String& password);
  void removeProfile(uint8_t profileIndex);
  static String escapeHtml(const String& value);

  DNSServer dnsServer_;
  WebServer webServer_{80};
  WifiProfile profiles_[kMaxProfiles];
  State state_ = State::Offline;
  bool stateChanged_ = true;
  bool serverConfigured_ = false;
  bool portalServicesActive_ = false;
  bool profilesChanged_ = false;
  bool candidateConnection_ = false;
  bool scanActive_ = false;
  uint8_t profileCount_ = 0;
  int8_t connectingProfileIndex_ = -1;
  String pendingSsid_;
  String pendingPassword_;
  String accessPointName_;
  String accessPointCode_;
  uint32_t connectionStartedMs_ = 0;
  uint32_t scanStartedMs_ = 0;
  uint32_t portalStartedMs_ = 0;
  uint32_t submitConnectAtMs_ = 0;
};
