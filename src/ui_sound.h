// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

enum class UiSound : uint8_t {
  Tap,
  Previous,
  Next,
  Confirm,
  Open,
  Close,
  Brightness,
  Warning,
  Wake,
};

class UiSoundEngine {
 public:
  bool begin(uint8_t volume = 56);
  void setVolume(uint8_t volume);
  void play(UiSound sound, uint8_t variant = 0);
  bool ready() const { return ready_; }

 private:
  struct ToneStep {
    float startFrequency;
    float endFrequency;
    uint16_t durationMs;
    float amplitude;
  };

  static constexpr uint32_t kSampleRate = 16000;
  static constexpr size_t kMaximumSamples = 5200;
  static constexpr size_t kSoundCount = 9;
  static constexpr size_t kVariantCount = 4;

  struct CachedSound {
    int16_t* samples = nullptr;
    size_t count = 0;
  };

  size_t synthesize(const ToneStep* steps, size_t stepCount,
                    int16_t* destination);
  size_t describe(UiSound sound, uint8_t variant, ToneStep* steps) const;
  bool cache(UiSound sound, uint8_t variant);
  uint32_t nextNoise();

  CachedSound sounds_[kSoundCount][kVariantCount] = {};
  uint32_t noiseState_ = 0x6D2B79F5u;
  bool ready_ = false;
};
