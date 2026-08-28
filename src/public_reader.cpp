// SPDX-License-Identifier: AGPL-3.0-or-later

#include "public_reader.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

namespace {

extern const uint8_t kCertificateBundleStart[]
    asm("_binary_data_x509_crt_bundle_start");

bool isContinuation(uint8_t value) { return (value & 0xC0) == 0x80; }

class BoundedMemoryStream : public Stream {
 public:
  explicit BoundedMemoryStream(size_t capacity) : capacity_(capacity) {
    data_ = static_cast<uint8_t*>(
        heap_caps_malloc(capacity_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data_ && capacity_ <= 64 * 1024) {
      data_ = static_cast<uint8_t*>(malloc(capacity_));
    }
  }

  ~BoundedMemoryStream() override {
    if (data_) free(data_);
  }

  using Print::write;

  size_t write(uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!data_ || size > capacity_ - length_) {
      overflowed_ = true;
      return 0;
    }
    memcpy(data_ + length_, buffer, size);
    length_ += size;
    return size;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  bool ready() const { return data_ != nullptr; }
  bool overflowed() const { return overflowed_; }
  size_t length() const { return length_; }

  uint8_t* release() {
    uint8_t* result = data_;
    data_ = nullptr;
    return result;
  }

 private:
  uint8_t* data_ = nullptr;
  size_t capacity_ = 0;
  size_t length_ = 0;
  bool overflowed_ = false;
};

bool isRedirect(int status) {
  return status == HTTP_CODE_MOVED_PERMANENTLY ||
         status == HTTP_CODE_FOUND || status == HTTP_CODE_SEE_OTHER ||
         status == HTTP_CODE_TEMPORARY_REDIRECT || status == 308;
}

}  // namespace

PublicReader::~PublicReader() { clear(); }

bool PublicReader::validHttpsUrl(const String& url) {
  if (url.length() < 12 || url.length() > kMaxSourceUrlBytes ||
      !url.startsWith("https://")) {
    return false;
  }
  const int hostStart = 8;
  const int pathStart = url.indexOf('/', hostStart);
  const String authority =
      pathStart < 0 ? url.substring(hostStart)
                    : url.substring(hostStart, pathStart);
  return !authority.isEmpty() && authority.indexOf('@') < 0 &&
         authority.indexOf(' ') < 0 && url.indexOf('#') < 0;
}

bool PublicReader::fetchBytes(const String& initialUrl, size_t maxBytes,
                              uint8_t*& data, size_t& length,
                              String& error) {
  data = nullptr;
  length = 0;
  if (WiFi.status() != WL_CONNECTED) {
    error = "网络未连接";
    return false;
  }
  if (!validHttpsUrl(initialUrl)) {
    error = "仅支持有效的 HTTPS 地址";
    return false;
  }

  WiFiClientSecure secureClient;
  secureClient.setCACertBundle(kCertificateBundleStart);
  secureClient.setTimeout(12);

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(3);
  http.setUserAgent("KK-Avatar/0.11.0");
  if (!http.begin(secureClient, initialUrl)) {
    error = "无法打开 HTTPS 地址";
    return false;
  }

  const int status = http.GET();
  if (status < 0) {
    error = "HTTPS 请求失败";
    Serial.printf("Public reading fetch failed: transport=%d\n", status);
    http.end();
    return false;
  }
  if (isRedirect(status)) {
    error = "重定向次数过多";
    http.end();
    return false;
  }
  if (status != HTTP_CODE_OK) {
    error = "服务器返回 " + String(status);
    Serial.printf("Public reading fetch failed: HTTP %d\n", status);
    http.end();
    return false;
  }

  const int announcedLength = http.getSize();
  if (announcedLength > static_cast<int>(maxBytes)) {
    error = "下载内容超过大小限制";
    http.end();
    return false;
  }

  BoundedMemoryStream sink(maxBytes);
  if (!sink.ready()) {
    error = "可用内存不足";
    http.end();
    return false;
  }
  const int written = http.writeToStream(&sink);
  http.end();
  if (written < 0 || sink.overflowed()) {
    error = sink.overflowed() ? "下载内容超过大小限制" : "下载中断";
    return false;
  }
  if (announcedLength >= 0 && written != announcedLength) {
    error = "下载内容不完整";
    return false;
  }
  if (sink.length() == 0) {
    error = "下载内容为空";
    return false;
  }

  length = sink.length();
  data = sink.release();
  return true;
}

bool PublicReader::fetchDocument(const String& sourceUrl, String& error) {
  clear();
  uint8_t* bytes = nullptr;
  size_t length = 0;
  if (!fetchBytes(sourceUrl, kMaxManifestBytes, bytes, length, error)) {
    return false;
  }
  String manifest;
  manifest.reserve(length + 1);
  manifest.concat(reinterpret_cast<const char*>(bytes), length);
  free(bytes);
  if (!parseManifest(manifest, error)) {
    clear();
    return false;
  }
  Serial.printf("Public reading manifest ready: blocks=%u\n", blockCount_);
  return true;
}

bool PublicReader::downloadImage(uint8_t blockIndex, String& error) {
  releaseImage();
  if (blockIndex >= blockCount_ ||
      blocks_[blockIndex].type != ReadingBlockType::Image) {
    error = "图片页不存在";
    return false;
  }
  if (!fetchBytes(blocks_[blockIndex].content, kMaxImageBytes, imageData_,
                  imageLength_, error)) {
    return false;
  }
  const bool jpeg = imageLength_ >= 3 && imageData_[0] == 0xFF &&
                    imageData_[1] == 0xD8 && imageData_[2] == 0xFF;
  const bool png = imageLength_ >= 8 && imageData_[0] == 0x89 &&
                   imageData_[1] == 'P' && imageData_[2] == 'N' &&
                   imageData_[3] == 'G' && imageData_[4] == 0x0D &&
                   imageData_[5] == 0x0A && imageData_[6] == 0x1A &&
                   imageData_[7] == 0x0A;
  if (!jpeg && !png) {
    releaseImage();
    error = "图片必须是 JPEG 或 PNG";
    return false;
  }
  imageIsPng_ = png;
  Serial.printf("Public reading image ready: bytes=%u format=%s\n",
                static_cast<unsigned>(imageLength_), png ? "png" : "jpeg");
  return true;
}

bool PublicReader::parseManifest(const String& original, String& error) {
  String manifest = original;
  manifest.replace("\r\n", "\n");
  manifest.replace('\r', '\n');
  if (!manifest.startsWith("KKREAD/1\n") && manifest != "KKREAD/1") {
    error = "书源不是 KKREAD/1 格式";
    return false;
  }

  size_t cursor = manifest.indexOf('\n');
  if (cursor == static_cast<size_t>(-1)) {
    error = "阅读清单没有内容";
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
      if (!addBlock(ReadingBlockType::Text, text, error)) return false;
      continue;
    }
    if (line.startsWith("::IMAGE ")) {
      String imageUrl = line.substring(8);
      imageUrl.trim();
      if (!addBlock(ReadingBlockType::Image, imageUrl, error)) return false;
      continue;
    }
    line.trim();
    if (!line.isEmpty() && !line.startsWith("#")) {
      error = "阅读清单包含未知指令";
      return false;
    }
  }
  if (blockCount_ == 0) {
    error = "阅读清单没有文字页或图片页";
    return false;
  }
  if (title_.isEmpty()) title_ = "公网阅读";
  return true;
}

bool PublicReader::addBlock(ReadingBlockType type, const String& content,
                            String& error) {
  if (blockCount_ >= kMaxBlocks) {
    error = "阅读清单最多支持 12 个内容块";
    return false;
  }
  if (type == ReadingBlockType::Image) {
    if (!validHttpsUrl(content)) {
      error = "图片地址必须是 HTTPS";
      return false;
    }
  } else {
    uint16_t glyphCount = 0;
    uint16_t explicitLines = 0;
    if (!validateText(content, glyphCount, explicitLines)) {
      error = "文字块需为有效 UTF-8，最多 1800 字和 96 行";
      return false;
    }
  }
  blocks_[blockCount_].type = type;
  blocks_[blockCount_].content = content;
  ++blockCount_;
  return true;
}

bool PublicReader::validateText(const String& text, uint16_t& glyphCount,
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

void PublicReader::releaseImage() {
  if (imageData_) free(imageData_);
  imageData_ = nullptr;
  imageLength_ = 0;
  imageIsPng_ = false;
}

void PublicReader::clear() {
  releaseImage();
  for (uint8_t index = 0; index < blockCount_; ++index) {
    blocks_[index].content = "";
  }
  blockCount_ = 0;
  title_ = "";
}
