// SPDX-License-Identifier: AGPL-3.0-or-later

#include "reading_document.h"

namespace {

bool isContinuation(uint8_t value) { return (value & 0xC0) == 0x80; }

}  // namespace

bool ReadingDocument::load(const String& manifest, String& error) {
  clear();
  if (manifest.isEmpty() || manifest.length() > kMaxManifestBytes) {
    error = manifest.isEmpty() ? "阅读文件为空" : "阅读文件超过大小限制";
    return false;
  }
  if (!parseManifest(manifest, error)) {
    clear();
    return false;
  }
  return true;
}

bool ReadingDocument::parseManifest(const String& original, String& error) {
  String manifest = original;
  manifest.replace("\r\n", "\n");
  manifest.replace('\r', '\n');
  if (!manifest.startsWith("KKREAD/1\n") && manifest != "KKREAD/1") {
    error = "内置章节格式错误";
    return false;
  }

  size_t cursor = manifest.indexOf('\n');
  if (cursor == static_cast<size_t>(-1)) {
    error = "内置章节没有内容";
    return false;
  }
  ++cursor;
  while (cursor < manifest.length()) {
    size_t lineEnd = manifest.indexOf('\n', cursor);
    if (lineEnd == static_cast<size_t>(-1)) lineEnd = manifest.length();
    String line = manifest.substring(cursor, lineEnd);
    cursor = lineEnd + 1;

    if (line.startsWith("TITLE: ") && title_.isEmpty()) {
      title_ = line.substring(7);
      title_.trim();
      if (title_.length() > 144) title_.remove(144);
      continue;
    }
    if (line == "::TEXT") {
      String text;
      while (cursor <= manifest.length()) {
        lineEnd = manifest.indexOf('\n', cursor);
        if (lineEnd == static_cast<size_t>(-1)) lineEnd = manifest.length();
        line = manifest.substring(cursor, lineEnd);
        cursor = lineEnd + 1;
        if (line == "::END") break;
        if (!text.isEmpty()) text += '\n';
        text += line;
        if (text.length() > 8192) {
          error = "单个文字块超过 8192 字节";
          return false;
        }
        if (lineEnd == manifest.length()) {
          error = "文字块缺少 ::END";
          return false;
        }
      }
      text.trim();
      if (!addBlock(text, error)) return false;
      continue;
    }
    line.trim();
    if (!line.isEmpty() && !line.startsWith("#")) {
      error = "内置章节包含未知指令";
      return false;
    }
  }
  if (blockCount_ == 0) {
    error = "内置章节没有文字内容";
    return false;
  }
  if (title_.isEmpty()) title_ = "内置阅读";
  return true;
}

bool ReadingDocument::addBlock(const String& content, String& error) {
  if (blockCount_ >= kMaxBlocks) {
    error = "单章最多支持 12 个文字块";
    return false;
  }
  uint16_t glyphCount = 0;
  uint16_t explicitLines = 0;
  if (!validateText(content, glyphCount, explicitLines)) {
    error = "文字块需为有效 UTF-8，最多 1800 字和 96 行";
    return false;
  }
  blocks_[blockCount_].content = content;
  ++blockCount_;
  return true;
}

bool ReadingDocument::validateText(const String& text, uint16_t& glyphCount,
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
    if ((codepoint < 0x20 && codepoint != '\n' && codepoint != '\t') ||
        (codepoint >= 0x7F && codepoint <= 0x9F)) {
      return false;
    }
    if (codepoint == '\n' && ++explicitLines > kMaxTextLines) return false;
    if (++glyphCount > kMaxTextGlyphs) return false;
    index += bytes;
  }
  return glyphCount > 0;
}

void ReadingDocument::clear() {
  for (uint8_t index = 0; index < blockCount_; ++index) {
    blocks_[index].content = "";
  }
  blockCount_ = 0;
  title_ = "";
}
