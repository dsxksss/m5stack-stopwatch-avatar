// SPDX-License-Identifier: AGPL-3.0-or-later

#include "wifi_pairing.h"

namespace {

const IPAddress kPortalAddress(192, 168, 4, 1);
const IPAddress kPortalMask(255, 255, 255, 0);

bool reached(uint32_t now, uint32_t deadline) {
  return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0;
}

}  // namespace

constexpr char WifiPairing::kAccessPointPassword[];
constexpr uint16_t WifiPairing::kDnsPort;

void WifiPairing::begin(const WifiProfile* profiles, uint8_t profileCount,
                        uint32_t nowMs) {
  const uint16_t suffix = static_cast<uint16_t>(ESP.getEfuseMac());
  char code[5];
  snprintf(code, sizeof(code), "%04X", suffix);
  accessPointCode_ = code;
  accessPointName_ = "KK-" + accessPointCode_;
  profileCount_ = 0;
  for (uint8_t index = 0;
       index < profileCount && index < kMaxProfiles; ++index) {
    String ssid = profiles[index].ssid;
    ssid.trim();
    if (ssid.isEmpty() || ssid.length() > 32 ||
        profiles[index].password.length() > 63 ||
        findProfile(ssid) >= 0) {
      continue;
    }
    profiles_[profileCount_].ssid = ssid;
    profiles_[profileCount_].password = profiles[index].password;
    ++profileCount_;
  }
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  if (hasCredentials()) {
    startKnownNetworkSearch(nowMs);
  } else {
    WiFi.mode(WIFI_OFF);
    setState(State::Offline);
  }
}

void WifiPairing::setState(State next) {
  if (state_ == next) return;
  state_ = next;
  stateChanged_ = true;
}

int8_t WifiPairing::findProfile(const String& ssid) const {
  for (uint8_t index = 0; index < profileCount_; ++index) {
    if (profiles_[index].ssid == ssid) return static_cast<int8_t>(index);
  }
  return -1;
}

bool WifiPairing::upsertProfile(const String& ssid, const String& password) {
  const int8_t existing = findProfile(ssid);
  if (existing >= 0) {
    profiles_[existing].password = password;
    profilesChanged_ = true;
    return true;
  }
  if (profileCount_ >= kMaxProfiles) return false;
  profiles_[profileCount_].ssid = ssid;
  profiles_[profileCount_].password = password;
  ++profileCount_;
  profilesChanged_ = true;
  return true;
}

void WifiPairing::removeProfile(uint8_t profileIndex) {
  if (profileIndex >= profileCount_) return;
  const String removedSsid = profiles_[profileIndex].ssid;
  for (uint8_t index = profileIndex; index + 1 < profileCount_; ++index) {
    profiles_[index] = profiles_[index + 1];
  }
  --profileCount_;
  profiles_[profileCount_].ssid = "";
  profiles_[profileCount_].password = "";
  profilesChanged_ = true;
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == removedSsid) {
    WiFi.disconnect(false, false);
  }
}

void WifiPairing::startKnownNetworkSearch(uint32_t nowMs) {
  stopPortalServices();
  candidateConnection_ = false;
  connectingProfileIndex_ = -1;
  pendingSsid_ = "";
  pendingPassword_ = "";
  submitConnectAtMs_ = 0;
  connectionStartedMs_ = 0;
  if (!hasCredentials()) {
    WiFi.scanDelete();
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    scanActive_ = false;
    scanStartedMs_ = 0;
    setState(State::Offline);
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  WiFi.scanDelete();
  const int16_t scanResult =
      WiFi.scanNetworks(true, true, false, 300);
  if (scanResult == WIFI_SCAN_FAILED) {
    scanActive_ = false;
    setState(State::Failed);
    Serial.println("Wi-Fi saved-network scan failed to start");
    return;
  }
  scanActive_ = true;
  scanStartedMs_ = nowMs;
  setState(State::Connecting);
  Serial.printf("Wi-Fi scanning for %u saved profile(s)\n", profileCount_);
  if (scanResult >= 0) handleScan(nowMs);
}

void WifiPairing::beginProfileConnection(uint8_t profileIndex,
                                         uint32_t nowMs) {
  if (profileIndex >= profileCount_) {
    setState(State::Failed);
    return;
  }
  scanActive_ = false;
  candidateConnection_ = false;
  connectingProfileIndex_ = static_cast<int8_t>(profileIndex);
  WiFi.mode(WIFI_STA);
  WiFi.begin(profiles_[profileIndex].ssid.c_str(),
             profiles_[profileIndex].password.c_str());
  connectionStartedMs_ = nowMs;
  setState(State::Connecting);
  Serial.printf("Wi-Fi connecting with saved profile %u/%u\n",
                profileIndex + 1, profileCount_);
}

void WifiPairing::beginCandidateConnection(const String& ssid,
                                           const String& password,
                                           uint32_t nowMs) {
  stopPortalServices();
  WiFi.scanDelete();
  scanActive_ = false;
  candidateConnection_ = true;
  connectingProfileIndex_ = -1;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  connectionStartedMs_ = nowMs;
  submitConnectAtMs_ = 0;
  setState(State::Connecting);
  Serial.println("Wi-Fi connecting with candidate profile");
}

void WifiPairing::handleScan(uint32_t nowMs) {
  if (!scanActive_) return;
  const int16_t result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) {
    if (nowMs - scanStartedMs_ < kScanTimeoutMs) return;
    WiFi.scanDelete();
    scanActive_ = false;
    setState(State::Failed);
    Serial.println("Wi-Fi saved-network scan timed out");
    return;
  }
  if (result == WIFI_SCAN_FAILED) {
    scanActive_ = false;
    setState(State::Failed);
    Serial.println("Wi-Fi saved-network scan failed");
    return;
  }

  int8_t bestProfile = -1;
  int32_t bestRssi = INT32_MIN;
  for (int16_t network = 0; network < result; ++network) {
    const int8_t profile = findProfile(WiFi.SSID(network));
    if (profile >= 0 && WiFi.RSSI(network) > bestRssi) {
      bestProfile = profile;
      bestRssi = WiFi.RSSI(network);
    }
  }
  WiFi.scanDelete();
  scanActive_ = false;
  scanStartedMs_ = 0;
  if (bestProfile < 0) {
    setState(State::Failed);
    Serial.println("No saved Wi-Fi profile is currently visible");
    return;
  }
  beginProfileConnection(static_cast<uint8_t>(bestProfile), nowMs);
}

void WifiPairing::configureServer() {
  if (serverConfigured_) return;
  serverConfigured_ = true;
  webServer_.on("/", HTTP_GET, [this]() { sendPortalPage(); });
  webServer_.on("/save", HTTP_POST,
                [this]() { handleCredentialSave(); });
  webServer_.on("/delete", HTTP_POST,
                [this]() { handleProfileDelete(); });
  webServer_.on("/generate_204", HTTP_GET,
                [this]() { redirectToPortal(); });
  webServer_.on("/hotspot-detect.html", HTTP_GET,
                [this]() { redirectToPortal(); });
  webServer_.on("/connecttest.txt", HTTP_GET,
                [this]() { redirectToPortal(); });
  webServer_.on("/ncsi.txt", HTTP_GET,
                [this]() { redirectToPortal(); });
  webServer_.onNotFound([this]() { redirectToPortal(); });
}

void WifiPairing::startPortal(uint32_t nowMs) {
  stopPortalServices();
  WiFi.scanDelete();
  scanActive_ = false;
  configureServer();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(kPortalAddress, kPortalAddress, kPortalMask);
  if (!WiFi.softAP(accessPointName_.c_str(), kAccessPointPassword, 1, 0, 2)) {
    setState(State::Failed);
    Serial.println("Wi-Fi management hotspot failed to start");
    return;
  }
  dnsServer_.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer_.start(kDnsPort, "*", kPortalAddress);
  webServer_.begin();
  portalServicesActive_ = true;
  portalStartedMs_ = nowMs;
  submitConnectAtMs_ = 0;
  setState(State::Portal);
  Serial.printf(
      "Wi-Fi management: connect to %s, then open http://192.168.4.1\n",
      accessPointName_.c_str());
}

void WifiPairing::stopPortalServices() {
  if (!portalServicesActive_) return;
  dnsServer_.stop();
  webServer_.stop();
  WiFi.softAPdisconnect(false);
  portalServicesActive_ = false;
  portalStartedMs_ = 0;
}

void WifiPairing::cancelPortal() {
  if (!portalServicesActive_) return;
  stopPortalServices();
  if (WiFi.status() == WL_CONNECTED && findProfile(WiFi.SSID()) >= 0) {
    WiFi.mode(WIFI_STA);
    setState(State::Connected);
  } else if (hasCredentials()) {
    startKnownNetworkSearch(millis());
  } else {
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    setState(State::Offline);
  }
  Serial.println("Wi-Fi management closed");
}

void WifiPairing::retry(uint32_t nowMs) {
  if (hasCredentials()) {
    startKnownNetworkSearch(nowMs);
  } else {
    startPortal(nowMs);
  }
}

void WifiPairing::forget() {
  stopPortalServices();
  WiFi.scanDelete();
  WiFi.disconnect(true, true);
  for (uint8_t index = 0; index < profileCount_; ++index) {
    profiles_[index].ssid = "";
    profiles_[index].password = "";
  }
  profileCount_ = 0;
  profilesChanged_ = true;
  pendingSsid_ = "";
  pendingPassword_ = "";
  candidateConnection_ = false;
  scanActive_ = false;
  connectingProfileIndex_ = -1;
  connectionStartedMs_ = 0;
  scanStartedMs_ = 0;
  submitConnectAtMs_ = 0;
  WiFi.mode(WIFI_OFF);
  setState(State::Offline);
  Serial.println("All saved Wi-Fi profiles cleared");
}

void WifiPairing::update(uint32_t nowMs) {
  if (portalServicesActive_) {
    dnsServer_.processNextRequest();
    webServer_.handleClient();
    if (reached(nowMs, submitConnectAtMs_)) {
      beginCandidateConnection(pendingSsid_, pendingPassword_, nowMs);
      return;
    }
    if (portalStartedMs_ != 0 &&
        nowMs - portalStartedMs_ >= kPortalTimeoutMs) {
      cancelPortal();
      Serial.println("Wi-Fi management timed out after 5 minutes");
    }
    return;
  }

  if (scanActive_) {
    handleScan(nowMs);
    return;
  }

  const wl_status_t link = WiFi.status();
  if (link == WL_CONNECTED) {
    if (state_ != State::Connected) {
      if (candidateConnection_) {
        upsertProfile(pendingSsid_, pendingPassword_);
        pendingSsid_ = "";
        pendingPassword_ = "";
        candidateConnection_ = false;
      }
      connectingProfileIndex_ = findProfile(WiFi.SSID());
      setState(State::Connected);
      Serial.printf("Wi-Fi connected: profile=%d/%u ip=%s rssi=%ld\n",
                    connectingProfileIndex_ + 1, profileCount_,
                    WiFi.localIP().toString().c_str(),
                    static_cast<long>(WiFi.RSSI()));
    }
    return;
  }

  if (state_ == State::Connected) {
    Serial.println("Wi-Fi link lost; searching saved profiles");
    startKnownNetworkSearch(nowMs);
  } else if (state_ == State::Connecting && connectionStartedMs_ != 0 &&
             nowMs - connectionStartedMs_ >= kConnectTimeoutMs) {
    if (candidateConnection_) {
      Serial.println("Wi-Fi candidate timed out; restoring saved profiles");
      startKnownNetworkSearch(nowMs);
      return;
    }
    connectionStartedMs_ = 0;
    connectingProfileIndex_ = -1;
    setState(State::Failed);
    Serial.println("Wi-Fi saved profile connection timed out");
  }
}

bool WifiPairing::takeProfilesChanged(WifiProfile* profiles,
                                      uint8_t& profileCount) {
  if (!profilesChanged_) return false;
  profilesChanged_ = false;
  profileCount = profileCount_;
  for (uint8_t index = 0; index < profileCount_; ++index) {
    profiles[index] = profiles_[index];
  }
  return true;
}

bool WifiPairing::consumeStateChanged() {
  const bool changed = stateChanged_;
  stateChanged_ = false;
  return changed;
}

const char* WifiPairing::stateName() const {
  switch (state_) {
    case State::Offline:
      return "offline";
    case State::Connecting:
      return "connecting";
    case State::Portal:
      return "managing";
    case State::Connected:
      return "connected";
    case State::Failed:
      return "failed";
  }
  return "unknown";
}

String WifiPairing::currentSsid() const {
  return WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String();
}

String WifiPairing::localIp() const {
  if (state_ == State::Portal) return WiFi.softAPIP().toString();
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return "--";
}

float WifiPairing::signalLevel() const {
  if (WiFi.status() != WL_CONNECTED) return 0.0f;
  const int32_t rssi = WiFi.RSSI();
  if (rssi >= -50) return 1.0f;
  if (rssi <= -90) return 0.12f;
  return 0.12f + (rssi + 90) / 40.0f * 0.88f;
}

String WifiPairing::escapeHtml(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 12);
  for (size_t index = 0; index < value.length(); ++index) {
    switch (value[index]) {
      case '&':
        escaped += F("&amp;");
        break;
      case '<':
        escaped += F("&lt;");
        break;
      case '>':
        escaped += F("&gt;");
        break;
      case '"':
        escaped += F("&quot;");
        break;
      case '\'':
        escaped += F("&#39;");
        break;
      default:
        escaped += value[index];
        break;
    }
  }
  return escaped;
}

void WifiPairing::sendPortalPage(const String& message) {
  String page;
  page.reserve(5200);
  page += F(
      "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>KK 网络管理</title><style>*{box-sizing:border-box}body{margin:0;"
      "background:#080808;color:#fff;font:16px system-ui,sans-serif;min-height:100vh}"
      "main{width:min(90vw,440px);margin:auto;padding:40px 0}h1{font-size:28px;"
      "margin:0 0 8px}h2{font-size:17px;margin:30px 0 10px;color:#ddd}p{color:#aaa;"
      "line-height:1.55}.profile{display:flex;align-items:center;justify-content:space-between;"
      "gap:12px;border-top:1px solid #282828;padding:13px 0}.profile span{overflow-wrap:anywhere}"
      "label{display:block;margin:16px 0 6px}input,button{border-radius:14px;padding:13px;"
      "border:1px solid #444;background:#151515;color:#fff;font-size:16px}input{width:100%}"
      "button{font-weight:700}.primary{width:100%;margin-top:20px;background:#fff;color:#000;"
      "border:0}.delete{color:#bbb;background:#111;padding:8px 12px}.msg{color:#fff}</style>"
      "</head><body><main><h1>KK 网络管理</h1><p>最多保存 5 个 2.4 GHz 网络。KK 启动或"
      "断线后会扫描附近网络，并连接信号最强的已保存项。密码只保存在设备中且不会显示。</p>");
  if (!message.isEmpty()) {
    page += F("<p class=\"msg\">");
    page += escapeHtml(message);
    page += F("</p>");
  }
  page += F("<h2>已保存网络</h2>");
  if (profileCount_ == 0) {
    page += F("<p>还没有保存网络。</p>");
  } else {
    for (uint8_t index = 0; index < profileCount_; ++index) {
      page += F("<div class=\"profile\"><span>");
      page += escapeHtml(profiles_[index].ssid);
      page += F("</span><form method=\"post\" action=\"/delete\"><input type=\"hidden\" "
                "name=\"index\" value=\"");
      page += String(index);
      page += F("\"><button class=\"delete\" type=\"submit\">删除</button></form></div>");
    }
  }
  page += F(
      "<h2>添加或更新网络</h2><form method=\"post\" action=\"/save\">"
      "<label for=\"ssid\">网络名称</label><input id=\"ssid\" name=\"ssid\" maxlength=\"32\" "
      "required><label for=\"password\">密码</label><input id=\"password\" name=\"password\" "
      "type=\"password\" maxlength=\"63\" autocomplete=\"current-password\">"
      "<button class=\"primary\" type=\"submit\">保存并连接</button></form></main></body></html>");
  webServer_.sendHeader("Cache-Control", "no-store");
  webServer_.sendHeader("X-Content-Type-Options", "nosniff");
  webServer_.send(200, "text/html; charset=utf-8", page);
}

void WifiPairing::handleCredentialSave() {
  String submittedSsid = webServer_.arg("ssid");
  const String submittedPassword = webServer_.arg("password");
  submittedSsid.trim();
  if (submittedSsid.isEmpty() || submittedSsid.length() > 32 ||
      submittedPassword.length() > 63) {
    sendPortalPage("网络名称或密码长度不正确");
    return;
  }
  if (findProfile(submittedSsid) < 0 && profileCount_ >= kMaxProfiles) {
    sendPortalPage("已保存 5 个网络，请先删除一个");
    return;
  }
  pendingSsid_ = submittedSsid;
  pendingPassword_ = submittedPassword;
  submitConnectAtMs_ = millis() + 650;
  webServer_.send(
      200, "text/html; charset=utf-8",
      "<!doctype html><meta charset=\"utf-8\"><meta name=\"viewport\" "
      "content=\"width=device-width,initial-scale=1\"><style>body{background:#080808;"
      "color:white;font:20px system-ui;padding:12vh 10vw}</style><h1>KK 正在验证网络</h1>"
      "<p>只有连接成功后才会保存；失败时会恢复其他已保存网络。</p>");
  Serial.println("Wi-Fi candidate credentials received");
}

void WifiPairing::handleProfileDelete() {
  if (!webServer_.hasArg("index")) {
    sendPortalPage("缺少要删除的网络编号");
    return;
  }
  const String value = webServer_.arg("index");
  char* end = nullptr;
  const long index = strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0' || index < 0 ||
      index >= profileCount_) {
    sendPortalPage("网络编号无效");
    return;
  }
  removeProfile(static_cast<uint8_t>(index));
  sendPortalPage("已删除网络");
  Serial.printf("Wi-Fi profile removed; remaining=%u\n", profileCount_);
}

void WifiPairing::redirectToPortal() {
  webServer_.sendHeader("Location", "http://192.168.4.1/", true);
  webServer_.send(302, "text/plain", "");
}
