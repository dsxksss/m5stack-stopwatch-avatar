// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <Arduino.h>

#include "public_reader.h"

class LocalLibrary {
 public:
  static constexpr uint8_t kMaxChapters = 64;

  bool begin(const char* bookRoot = "/b/z");
  bool loadChapter(uint8_t index, PublicReader& reader, String& error) const;

  bool available() const { return available_; }
  uint8_t chapterCount() const { return chapterCount_; }
  const String& bookTitle() const { return bookTitle_; }
  const String& chapterTitle(uint8_t index) const;

 private:
  struct Chapter {
    String path;
    String title;
  };

  static constexpr size_t kMaxCatalogBytes = 12 * 1024;
  static constexpr size_t kMaxManifestBytes = 24 * 1024;

  bool readFile(const String& path, size_t maxBytes, String& output,
                String& error) const;
  bool parseCatalog(const String& catalog, String& error);

  Chapter chapters_[kMaxChapters];
  String bookRoot_;
  String bookTitle_;
  uint8_t chapterCount_ = 0;
  bool filesystemReady_ = false;
  bool available_ = false;
};
