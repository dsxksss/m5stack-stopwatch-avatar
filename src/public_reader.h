// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <Arduino.h>

enum class ReadingBlockType : uint8_t { Text, Image };

struct ReadingBlock {
  ReadingBlockType type = ReadingBlockType::Text;
  String content;
};

class PublicReader {
 public:
  static constexpr uint8_t kMaxBlocks = 12;
  static constexpr size_t kMaxSourceUrlBytes = 384;

  ~PublicReader();

  bool fetchDocument(const String& sourceUrl, String& error);
  bool downloadImage(uint8_t blockIndex, String& error);
  void clear();
  void releaseImage();

  uint8_t blockCount() const { return blockCount_; }
  const ReadingBlock& block(uint8_t index) const { return blocks_[index]; }
  const String& title() const { return title_; }
  const uint8_t* imageData() const { return imageData_; }
  size_t imageLength() const { return imageLength_; }
  bool imageIsPng() const { return imageIsPng_; }

  static bool validHttpsUrl(const String& url);

 private:
  static constexpr size_t kMaxManifestBytes = 24 * 1024;
  static constexpr size_t kMaxImageBytes = 768 * 1024;
  static constexpr uint16_t kMaxTextGlyphs = 1800;
  static constexpr uint16_t kMaxTextLines = 96;

  bool fetchBytes(const String& url, size_t maxBytes, uint8_t*& data,
                  size_t& length, String& error);
  bool parseManifest(const String& manifest, String& error);
  bool addBlock(ReadingBlockType type, const String& content, String& error);
  static bool validateText(const String& text, uint16_t& glyphCount,
                           uint16_t& explicitLines);

  ReadingBlock blocks_[kMaxBlocks];
  uint8_t blockCount_ = 0;
  String title_;
  uint8_t* imageData_ = nullptr;
  size_t imageLength_ = 0;
  bool imageIsPng_ = false;
};
