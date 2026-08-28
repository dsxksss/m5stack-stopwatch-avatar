// SPDX-License-Identifier: AGPL-3.0-or-later

#include "local_library.h"

#include <SPIFFS.h>

namespace {

const String kEmptyString;

bool validRelativeManifestPath(const String& path) {
  return path.startsWith("manifests/") && path.endsWith(".kkread") &&
         path.indexOf("..") < 0 && path.indexOf('\\') < 0 &&
         path.indexOf('?') < 0 && path.indexOf('#') < 0;
}

}  // namespace

bool LocalLibrary::begin(const char* bookRoot) {
  available_ = false;
  chapterCount_ = 0;
  bookTitle_ = "";
  bookRoot_ = bookRoot ? String(bookRoot) : String();
  while (bookRoot_.endsWith("/")) bookRoot_.remove(bookRoot_.length() - 1);

  filesystemReady_ = SPIFFS.begin(false);
  if (!filesystemReady_) {
    Serial.println("Local library unavailable: SPIFFS mount failed");
    return false;
  }

  String catalog;
  String error;
  if (!readFile(bookRoot_ + "/catalog.kkbook", kMaxCatalogBytes, catalog,
                error) ||
      !parseCatalog(catalog, error)) {
    Serial.printf("Local library unavailable: %s\n", error.c_str());
    return false;
  }

  available_ = chapterCount_ > 0;
  Serial.printf("Local library ready: title=%s chapters=%u used=%u total=%u\n",
                bookTitle_.c_str(), static_cast<unsigned>(chapterCount_),
                static_cast<unsigned>(SPIFFS.usedBytes()),
                static_cast<unsigned>(SPIFFS.totalBytes()));
  return available_;
}

bool LocalLibrary::readFile(const String& path, size_t maxBytes,
                            String& output, String& error) const {
  output = "";
  if (!filesystemReady_) {
    error = "内置存储未就绪";
    return false;
  }
  File file = SPIFFS.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    error = "找不到内置阅读文件";
    return false;
  }
  const size_t length = file.size();
  if (length == 0 || length > maxBytes) {
    file.close();
    error = length == 0 ? "内置阅读文件为空" : "内置阅读文件过大";
    return false;
  }
  if (!output.reserve(length + 1)) {
    file.close();
    error = "读取内置书籍时内存不足";
    return false;
  }
  while (file.available()) output += static_cast<char>(file.read());
  file.close();
  if (output.length() != length) {
    output = "";
    error = "内置阅读文件不完整";
    return false;
  }
  return true;
}

bool LocalLibrary::parseCatalog(const String& original, String& error) {
  String catalog = original;
  catalog.replace("\r\n", "\n");
  catalog.replace('\r', '\n');
  if (!catalog.startsWith("KKBOOK/1\n")) {
    error = "内置书籍目录格式错误";
    return false;
  }

  size_t cursor = catalog.indexOf('\n') + 1;
  while (cursor < catalog.length()) {
    size_t lineEnd = catalog.indexOf('\n', cursor);
    if (lineEnd == static_cast<size_t>(-1)) lineEnd = catalog.length();
    String line = catalog.substring(cursor, lineEnd);
    cursor = lineEnd + 1;
    line.trim();
    if (line.isEmpty() || line.startsWith("#") ||
        line.startsWith("SOURCE_SHA256: ")) {
      continue;
    }
    if (line.startsWith("TITLE: ") && bookTitle_.isEmpty()) {
      bookTitle_ = line.substring(7);
      bookTitle_.trim();
      if (bookTitle_.length() > 96) bookTitle_.remove(96);
      continue;
    }
    if (!line.startsWith("::CHAPTER ")) {
      error = "内置书籍目录包含未知指令";
      return false;
    }
    if (chapterCount_ >= kMaxChapters) {
      error = "内置书籍章节超过 64 个";
      return false;
    }
    const int delimiter = line.indexOf('|', 10);
    if (delimiter < 0) {
      error = "内置书籍章节目录不完整";
      return false;
    }
    String relativePath = line.substring(10, delimiter);
    String title = line.substring(delimiter + 1);
    relativePath.trim();
    title.trim();
    if (!validRelativeManifestPath(relativePath) || title.isEmpty()) {
      error = "内置书籍章节目录无效";
      return false;
    }
    if (title.length() > 144) title.remove(144);
    chapters_[chapterCount_].path = bookRoot_ + "/" + relativePath;
    chapters_[chapterCount_].title = title;
    ++chapterCount_;
  }
  if (bookTitle_.isEmpty() || chapterCount_ == 0) {
    error = "内置书籍目录没有内容";
    return false;
  }
  return true;
}

bool LocalLibrary::loadChapter(uint8_t index, ReadingDocument& document,
                               String& error) const {
  document.clear();
  if (!available_ || index >= chapterCount_) {
    error = "内置章节不存在";
    return false;
  }
  String manifest;
  if (!readFile(chapters_[index].path, kMaxManifestBytes, manifest, error)) {
    return false;
  }
  if (!document.load(manifest, error)) return false;
  Serial.printf("Local chapter ready: index=%u blocks=%u\n",
                static_cast<unsigned>(index + 1),
                static_cast<unsigned>(document.blockCount()));
  return true;
}

const String& LocalLibrary::chapterTitle(uint8_t index) const {
  if (index >= chapterCount_) return kEmptyString;
  return chapters_[index].title;
}
