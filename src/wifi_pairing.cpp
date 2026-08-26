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

void WifiPairing::begin(const String& savedSsid, const String& savedPassword,
                        uint32_t nowMs) {
  const uint16_t suffix = static_cast<uint16_t>(ESP.getEfuseMac());
  char code[5];
  snprintf(code, sizeof(code), "%04X", suffix);
  accessPointCode_ = code;
  accessPointName_ = "KK-" + accessPointCode_;
  ssid_ = savedSsid;
  password_ = savedPassword;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  if (hasCredentials()) {
    beginConnection(ssid_, password_, nowMs);
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

void WifiPairing::beginConnection(const String& ssid,
                                  const String& password, uint32_t nowMs,
                                  bool candidate) {
  stopPortalServices();
  candidateConnection_ = candidate;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  connectionStartedMs_ = nowMs;
  submitConnectAtMs_ = 0;
  setState(State::Connecting);
  Serial.printf("Wi-Fi connecting: ssid=%s%s\n", ssid.c_str(),
                candidate ? " (candidate)" : "");
}

void WifiPairing::configureServer() {
  if (serverConfigured_) return;
  serverConfigured_ = true;
  webServer_.on("/", HTTP_GET, [this]() { sendPortalPage(); });
  webServer_.on("/save", HTTP_POST,
                [this]() { handleCredentialSave(); });
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
  configureServer();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(kPortalAddress, kPortalAddress, kPortalMask);
  if (!WiFi.softAP(accessPointName_.c_str(), kAccessPointPassword, 1, 0, 2)) {
    setState(State::Failed);
    Serial.println("Wi-Fi pairing hotspot failed to start");
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
      "Wi-Fi pairing: connect to %s (password %s), then open http://192.168.4.1\n",
      accessPointName_.c_str(), kAccessPointPassword);
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
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    setState(State::Connected);
  } else if (hasCredentials()) {
    WiFi.mode(WIFI_STA);
    setState(State::Failed);
  } else {
    WiFi.mode(WIFI_OFF);
    setState(State::Offline);
  }
  Serial.println("Wi-Fi pairing cancelled");
}

void WifiPairing::retry(uint32_t nowMs) {
  if (hasCredentials()) {
    beginConnection(ssid_, password_, nowMs);
  } else {
    startPortal(nowMs);
  }
}

void WifiPairing::forget() {
  stopPortalServices();
  WiFi.disconnect(true, true);
  ssid_ = "";
  password_ = "";
  pendingSsid_ = "";
  pendingPassword_ = "";
  credentialsReady_ = false;
  candidateConnection_ = false;
  connectionStartedMs_ = 0;
  submitConnectAtMs_ = 0;
  WiFi.mode(WIFI_OFF);
  setState(State::Offline);
  Serial.println("Saved Wi-Fi credentials cleared");
}

void WifiPairing::update(uint32_t nowMs) {
  if (portalServicesActive_) {
    dnsServer_.processNextRequest();
    webServer_.handleClient();
    if (reached(nowMs, submitConnectAtMs_)) {
      beginConnection(pendingSsid_, pendingPassword_, nowMs, true);
      return;
    }
    if (portalStartedMs_ != 0 &&
        nowMs - portalStartedMs_ >= kPortalTimeoutMs) {
      cancelPortal();
      Serial.println("Wi-Fi pairing timed out after 5 minutes");
    }
    return;
  }

  const wl_status_t link = WiFi.status();
  if (link == WL_CONNECTED) {
    if (state_ != State::Connected) {
      if (candidateConnection_) {
        ssid_ = pendingSsid_;
        password_ = pendingPassword_;
        credentialsReady_ = true;
        candidateConnection_ = false;
      }
      setState(State::Connected);
      Serial.printf("Wi-Fi connected: ssid=%s ip=%s rssi=%ld\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                    static_cast<long>(WiFi.RSSI()));
    }
    return;
  }

  if (state_ == State::Connected) {
    connectionStartedMs_ = nowMs;
    WiFi.reconnect();
    setState(State::Connecting);
    Serial.println("Wi-Fi link lost; reconnecting");
  } else if (state_ == State::Connecting && connectionStartedMs_ != 0 &&
             nowMs - connectionStartedMs_ >= kConnectTimeoutMs) {
    if (candidateConnection_ && hasCredentials()) {
      Serial.printf(
          "Wi-Fi candidate timed out: ssid=%s; restoring saved network\n",
          pendingSsid_.c_str());
      candidateConnection_ = false;
      pendingSsid_ = "";
      pendingPassword_ = "";
      beginConnection(ssid_, password_, nowMs);
      return;
    }
    const String failedSsid =
        candidateConnection_ ? pendingSsid_ : ssid_;
    candidateConnection_ = false;
    setState(State::Failed);
    Serial.printf("Wi-Fi connection timed out: ssid=%s\n",
                  failedSsid.c_str());
  }
}

bool WifiPairing::takeNewCredentials(String& ssid, String& password) {
  if (!credentialsReady_) return false;
  credentialsReady_ = false;
  ssid = ssid_;
  password = password_;
  pendingSsid_ = "";
  pendingPassword_ = "";
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
      return "pairing";
    case State::Connected:
      return "connected";
    case State::Failed:
      return "failed";
  }
  return "unknown";
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
  page.reserve(2400);
  page += F(
      "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>KK Wi-Fi</title><style>body{margin:0;background:#080808;color:#fff;"
      "font:16px system-ui,sans-serif;display:grid;place-items:center;min-height:100vh}"
      "main{width:min(86vw,360px)}h1{font-size:28px;margin:0 0 8px}p{color:#bbb;"
      "line-height:1.55}label{display:block;margin:18px 0 6px}input,button{box-sizing:"
      "border-box;width:100%;border-radius:16px;padding:14px;border:1px solid #444;"
      "background:#151515;color:#fff;font-size:16px}button{margin-top:22px;background:#fff;"
      "color:#000;border:0;font-weight:700}.msg{color:#fff}</style></head><body><main>"
      "<h1>让 KK 连上 Wi-Fi</h1><p>选择或输入 2.4 GHz 网络，凭据只保存在设备中。</p>");
  if (!message.isEmpty()) {
    page += F("<p class=\"msg\">");
    page += escapeHtml(message);
    page += F("</p>");
  }
  page += F(
      "<form method=\"post\" action=\"/save\"><label for=\"ssid\">网络名称</label>"
      "<input id=\"ssid\" name=\"ssid\" maxlength=\"32\" required>"
      "<label for=\"password\">密码</label><input id=\"password\" "
      "name=\"password\" type=\"password\" maxlength=\"63\" autocomplete=\"current-password\">"
      "<button type=\"submit\">连接</button></form></main></body></html>");
  webServer_.sendHeader("Cache-Control", "no-store");
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
  pendingSsid_ = submittedSsid;
  pendingPassword_ = submittedPassword;
  credentialsReady_ = false;
  submitConnectAtMs_ = millis() + 650;
  webServer_.send(
      200, "text/html; charset=utf-8",
      "<!doctype html><meta charset=\"utf-8\"><meta name=\"viewport\" "
      "content=\"width=device-width,initial-scale=1\"><style>body{background:#080808;"
      "color:white;font:20px system-ui;padding:12vh 10vw}</style><h1>KK 正在连接</h1>"
      "<p>现在可以回到手表查看表情。</p>");
  Serial.printf("Wi-Fi credentials received: ssid=%s\n",
                pendingSsid_.c_str());
}

void WifiPairing::redirectToPortal() {
  webServer_.sendHeader("Location", "http://192.168.4.1/", true);
  webServer_.send(302, "text/plain", "");
}
