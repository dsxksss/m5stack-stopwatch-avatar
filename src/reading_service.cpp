// SPDX-License-Identifier: AGPL-3.0-or-later

#include "reading_service.h"

#include <WiFi.h>

namespace {

bool isContinuation(uint8_t value) { return (value & 0xC0) == 0x80; }

}  // namespace

void ReadingService::configureServer() {
  if (configured_) return;
  configured_ = true;
  webServer_.on("/", HTTP_GET, [this]() {
    webServer_.sendHeader("Location", "/read", true);
    webServer_.send(302, "text/plain", "");
  });
  webServer_.on("/read", HTTP_GET, [this]() { sendReadingPage(); });
  webServer_.on("/api/read", HTTP_POST,
                [this]() { handleReadingSubmit(); });
  webServer_.onNotFound([this]() {
    webServer_.sendHeader("Cache-Control", "no-store");
    webServer_.send(404, "text/plain; charset=utf-8", "页面不存在");
  });
}

void ReadingService::start() {
  if (active_ || WiFi.status() != WL_CONNECTED) return;
  configureServer();
  webServer_.begin();
  active_ = true;
  Serial.printf("Reading service ready: http://%s/read\n",
                WiFi.localIP().toString().c_str());
}

void ReadingService::stop() {
  if (!active_) return;
  webServer_.stop();
  active_ = false;
  Serial.println("Reading service stopped");
}

void ReadingService::update(bool shouldRun) {
  if (shouldRun) {
    start();
  } else {
    stop();
  }
  if (active_) webServer_.handleClient();
}

bool ReadingService::takePendingText(String& text) {
  if (!pendingReady_) return false;
  pendingReady_ = false;
  text = pendingText_;
  pendingText_ = "";
  return true;
}

void ReadingService::sendReadingPage() {
  String page;
  page.reserve(5200);
  page += F(
      "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<meta name=\"color-scheme\" content=\"dark\"><title>KK 阅读模式</title>"
      "<style>*{box-sizing:border-box}body{margin:0;background:#050505;color:#f5f5f5;"
      "font:16px system-ui,-apple-system,sans-serif;min-height:100vh;display:grid;"
      "place-items:center}main{width:min(90vw,620px);padding:36px 0}h1{font-size:30px;"
      "margin:0 0 8px}p{color:#aaa;line-height:1.6;margin:0 0 20px}textarea{width:100%;"
      "min-height:48vh;resize:vertical;border:1px solid #3a3a3a;border-radius:20px;"
      "background:#111;color:#fff;padding:18px;font:17px/1.7 system-ui,sans-serif;"
      "outline:none}textarea:focus{border-color:#888}.row{display:flex;align-items:center;"
      "justify-content:space-between;gap:16px;margin-top:14px}.count{color:#777}button{"
      "border:0;border-radius:999px;background:#f2f2f2;color:#050505;padding:13px 24px;"
      "font-size:16px;font-weight:700}.msg{min-height:26px;margin-top:14px;color:#bbb}"
      "</style></head><body><main><h1>交给 KK 阅读</h1>"
      "<p>在 KK 上长按 B，进入“阅读模式”并选择“局域网投送”，然后从这里发送临时正文。</p>"
      "<textarea id=\"text\" maxlength=\"1800\" placeholder=\"在这里粘贴长文……\"></textarea>"
      "<div class=\"row\"><span class=\"count\" id=\"count\">0 / 1800</span>"
      "<button id=\"send\">发送给 KK</button></div><div class=\"msg\" id=\"msg\"></div>"
      "<script>const t=document.querySelector('#text'),c=document.querySelector('#count'),"
      "m=document.querySelector('#msg'),b=document.querySelector('#send');"
      "const n=()=>Array.from(t.value).length;const u=()=>c.textContent=n()+' / 1800';"
      "t.addEventListener('input',u);b.addEventListener('click',async()=>{const v=t.value.trim();"
      "if(!v){m.textContent='请先输入正文';return}if(n()>1800){m.textContent='正文超过 1800 字';return}"
      "b.disabled=true;m.textContent='正在发送……';try{const r=await fetch('/api/read',{method:'POST',"
      "headers:{'Content-Type':'text/plain;charset=utf-8'},body:v});const j=await r.json();"
      "m.textContent=j.message||'已发送'}catch(e){m.textContent='发送失败，请检查网络'}"
      "finally{b.disabled=false}});u()</script></main></body></html>");
  webServer_.sendHeader("Cache-Control", "no-store");
  webServer_.sendHeader("X-Content-Type-Options", "nosniff");
  webServer_.sendHeader("Content-Security-Policy",
                        "default-src 'none'; style-src 'unsafe-inline'; "
                        "script-src 'unsafe-inline'; connect-src 'self'");
  webServer_.send(200, "text/html; charset=utf-8", page);
}

void ReadingService::sendJson(int statusCode, const String& body) {
  webServer_.sendHeader("Cache-Control", "no-store");
  webServer_.sendHeader("X-Content-Type-Options", "nosniff");
  webServer_.send(statusCode, "application/json; charset=utf-8", body);
}

bool ReadingService::validateText(const String& text, uint16_t& glyphCount,
                                  uint16_t& explicitLines) {
  glyphCount = 0;
  explicitLines = 1;
  for (size_t index = 0; index < text.length();) {
    const uint8_t lead = static_cast<uint8_t>(text[index]);
    uint32_t codepoint = 0;
    size_t bytes = 0;
    if (lead <= 0x7F) {
      codepoint = lead;
      bytes = 1;
    } else if (lead >= 0xC2 && lead <= 0xDF) {
      bytes = 2;
      codepoint = lead & 0x1F;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      bytes = 3;
      codepoint = lead & 0x0F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      bytes = 4;
      codepoint = lead & 0x07;
    } else {
      return false;
    }
    if (index + bytes > text.length()) return false;
    for (size_t offset = 1; offset < bytes; ++offset) {
      const uint8_t continuation =
          static_cast<uint8_t>(text[index + offset]);
      if (!isContinuation(continuation)) return false;
      codepoint = (codepoint << 6) | (continuation & 0x3F);
    }
    if ((bytes == 2 && codepoint < 0x80) ||
        (bytes == 3 && codepoint < 0x800) ||
        (bytes == 4 && codepoint < 0x10000) || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
      return false;
    }
    if ((codepoint < 0x20 && codepoint != '\n' && codepoint != '\r' &&
         codepoint != '\t') || (codepoint >= 0x7F && codepoint <= 0x9F)) {
      return false;
    }
    if (codepoint == '\n' && ++explicitLines > kMaxExplicitLines) return false;
    if (++glyphCount > kMaxGlyphs) return false;
    index += bytes;
  }
  return glyphCount > 0;
}

void ReadingService::handleReadingSubmit() {
  if (!webServer_.hasArg("plain")) {
    sendJson(400, "{\"message\":\"请求中没有正文\"}");
    return;
  }
  String text = webServer_.arg("plain");
  if (text.length() == 0 || text.length() > kMaxBodyBytes) {
    sendJson(413, "{\"message\":\"正文为空或超过 8192 字节\"}");
    return;
  }
  text.trim();
  uint16_t glyphCount = 0;
  uint16_t explicitLines = 0;
  if (!validateText(text, glyphCount, explicitLines)) {
    sendJson(400, "{\"message\":\"正文需为有效 UTF-8，最多 1800 字和 96 行\"}");
    return;
  }
  pendingText_ = text;
  pendingReady_ = true;
  sendJson(202, "{\"message\":\"已发送，KK 将在局域网投送页面中打开\"}");
  Serial.printf("Reading text queued: bytes=%u glyphs=%u lines=%u\n",
                static_cast<unsigned>(text.length()), glyphCount,
                explicitLines);
}
