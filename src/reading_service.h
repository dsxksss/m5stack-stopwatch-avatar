// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <Arduino.h>
#include <WebServer.h>

class ReadingService {
 public:
  void update(bool shouldRun);
  void stop();

  bool active() const { return active_; }
  bool hasPendingText() const { return pendingReady_; }
  bool takePendingText(String& text);
  bool takePendingSourceUrl(String& sourceUrl);
  void setSourceUrl(const String& sourceUrl) { sourceUrl_ = sourceUrl; }

 private:
  static constexpr size_t kMaxBodyBytes = 8192;
  static constexpr uint16_t kMaxGlyphs = 1800;
  static constexpr uint16_t kMaxExplicitLines = 96;

  void configureServer();
  void start();
  void sendReadingPage();
  void handleReadingSubmit();
  void handleSourceSubmit();
  void sendJson(int statusCode, const String& body);
  static bool validateText(const String& text, uint16_t& glyphCount,
                           uint16_t& explicitLines);

  WebServer webServer_{80};
  bool configured_ = false;
  bool active_ = false;
  bool pendingReady_ = false;
  bool pendingSourceReady_ = false;
  String pendingText_;
  String pendingSourceUrl_;
  String sourceUrl_;
};
