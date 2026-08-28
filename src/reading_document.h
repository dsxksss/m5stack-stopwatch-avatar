// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <Arduino.h>

struct ReadingBlock {
  String content;
};

class ReadingDocument {
 public:
  static constexpr uint8_t kMaxBlocks = 12;

  bool load(const String& manifest, String& error);
  void clear();

  uint8_t blockCount() const { return blockCount_; }
  const ReadingBlock& block(uint8_t index) const { return blocks_[index]; }
  const String& title() const { return title_; }

 private:
  static constexpr size_t kMaxManifestBytes = 24 * 1024;
  static constexpr uint16_t kMaxTextGlyphs = 1800;
  static constexpr uint16_t kMaxTextLines = 96;

  bool parseManifest(const String& manifest, String& error);
  bool addBlock(const String& content, String& error);
  static bool validateText(const String& text, uint16_t& glyphCount,
                           uint16_t& explicitLines);

  ReadingBlock blocks_[kMaxBlocks];
  uint8_t blockCount_ = 0;
  String title_;
};
