// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui_sound.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979323846f;

float smootherStep(float value) {
  value = std::max(0.0f, std::min(1.0f, value));
  return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
}

}  // namespace

bool UiSoundEngine::begin(uint8_t volume) {
  ready_ = M5.Speaker.isEnabled();
  if (!ready_) return false;
  M5.Speaker.setVolume(volume);
  M5.Speaker.setAllChannelVolume(255);
  for (uint8_t soundIndex = 0; soundIndex < kSoundCount; ++soundIndex) {
    const UiSound sound = static_cast<UiSound>(soundIndex);
    const uint8_t variants =
        sound == UiSound::Brightness ? kVariantCount : 1;
    for (uint8_t variant = 0; variant < variants; ++variant) {
      if (!cache(sound, variant)) {
        ready_ = false;
        return false;
      }
    }
  }
  return true;
}

void UiSoundEngine::setVolume(uint8_t volume) {
  if (ready_) M5.Speaker.setVolume(volume);
}

uint32_t UiSoundEngine::nextNoise() {
  noiseState_ ^= noiseState_ << 13;
  noiseState_ ^= noiseState_ >> 17;
  noiseState_ ^= noiseState_ << 5;
  return noiseState_;
}

size_t UiSoundEngine::synthesize(const ToneStep* steps, size_t stepCount,
                                 int16_t* destination) {
  size_t outputIndex = 0;
  float primaryPhase = 0.0f;
  float sparklePhase = 0.0f;

  for (size_t stepIndex = 0; stepIndex < stepCount; ++stepIndex) {
    const ToneStep& step = steps[stepIndex];
    const size_t stepSamples =
        static_cast<size_t>(step.durationMs) * kSampleRate / 1000;
    if (stepSamples == 0) continue;

    for (size_t sampleIndex = 0;
         sampleIndex < stepSamples && outputIndex < kMaximumSamples;
         ++sampleIndex, ++outputIndex) {
      if (step.amplitude <= 0.0f || step.startFrequency <= 0.0f) {
        destination[outputIndex] = 0;
        continue;
      }

      const float progress =
          sampleIndex / static_cast<float>(std::max<size_t>(1, stepSamples - 1));
      const float curvedProgress = smootherStep(progress);
      const float frequency =
          step.startFrequency +
          (step.endFrequency - step.startFrequency) * curvedProgress;
      primaryPhase += 2.0f * kPi * frequency / kSampleRate;
      sparklePhase += 2.0f * kPi * frequency * 1.503f / kSampleRate;

      const float attack = smootherStep(std::min(1.0f, sampleIndex / 64.0f));
      const float release = 1.0f - smootherStep(progress);
      const float envelope = attack * release;
      const float noise =
          (static_cast<int32_t>(nextNoise() & 0xFFFFu) - 32768) / 32768.0f;
      const float transient =
          progress < 0.10f ? noise * (0.10f - progress) * 0.18f : 0.0f;
      const float sample =
          (sinf(primaryPhase) * 0.80f + sinf(sparklePhase) * 0.15f +
           sinf(primaryPhase * 2.0f + 0.35f) * 0.05f + transient) *
          envelope * step.amplitude;
      destination[outputIndex] = static_cast<int16_t>(
          lroundf(std::max(-0.92f, std::min(0.92f, sample)) * 32767.0f));
    }
  }

  const size_t fadeSamples = std::min<size_t>(48, outputIndex);
  for (size_t index = 0; index < fadeSamples; ++index) {
    const size_t target = outputIndex - fadeSamples + index;
    destination[target] = static_cast<int16_t>(
        destination[target] * (fadeSamples - index) /
        static_cast<float>(fadeSamples));
  }
  return outputIndex;
}

size_t UiSoundEngine::describe(UiSound sound, uint8_t variant,
                               ToneStep* steps) const {
  switch (sound) {
    case UiSound::Tap:
      steps[0] = {690.0f, 850.0f, 72, 0.30f};
      return 1;
    case UiSound::Previous:
      steps[0] = {760.0f, 535.0f, 92, 0.31f};
      return 1;
    case UiSound::Next:
      steps[0] = {535.0f, 790.0f, 92, 0.31f};
      return 1;
    case UiSound::Confirm:
      steps[0] = {610.0f, 760.0f, 74, 0.27f};
      steps[1] = {0.0f, 0.0f, 18, 0.0f};
      steps[2] = {865.0f, 1080.0f, 112, 0.32f};
      return 3;
    case UiSound::Open:
      steps[0] = {390.0f, 625.0f, 105, 0.25f};
      steps[1] = {570.0f, 910.0f, 138, 0.30f};
      return 2;
    case UiSound::Close:
      steps[0] = {850.0f, 610.0f, 96, 0.27f};
      steps[1] = {0.0f, 0.0f, 14, 0.0f};
      steps[2] = {515.0f, 355.0f, 118, 0.23f};
      return 3;
    case UiSound::Brightness: {
      const float base = 510.0f + std::min<uint8_t>(variant, 3) * 105.0f;
      steps[0] = {base * 0.82f, base, 58, 0.25f};
      steps[1] = {base, base * 1.18f, 78, 0.28f};
      return 2;
    }
    case UiSound::Warning:
      steps[0] = {390.0f, 305.0f, 128, 0.28f};
      steps[1] = {0.0f, 0.0f, 32, 0.0f};
      steps[2] = {345.0f, 270.0f, 152, 0.24f};
      return 3;
    case UiSound::Wake:
      steps[0] = {405.0f, 690.0f, 145, 0.27f};
      steps[1] = {735.0f, 925.0f, 82, 0.22f};
      return 2;
  }
  return 0;
}

bool UiSoundEngine::cache(UiSound sound, uint8_t variant) {
  ToneStep steps[4] = {};
  const size_t stepCount = describe(sound, variant, steps);
  if (stepCount == 0) return false;

  size_t maximumCount = 0;
  for (size_t index = 0; index < stepCount; ++index) {
    maximumCount += static_cast<size_t>(steps[index].durationMs) *
                    kSampleRate / 1000;
  }
  maximumCount = std::min(maximumCount, kMaximumSamples);
  int16_t* samples = static_cast<int16_t*>(heap_caps_malloc(
      maximumCount * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (samples == nullptr) return false;

  CachedSound& cached =
      sounds_[static_cast<uint8_t>(sound)][std::min<uint8_t>(variant, 3)];
  cached.samples = samples;
  cached.count = synthesize(steps, stepCount, samples);
  return cached.count > 0;
}

void UiSoundEngine::play(UiSound sound, uint8_t variant) {
  if (!ready_) return;
  const CachedSound& cached =
      sounds_[static_cast<uint8_t>(sound)]
             [sound == UiSound::Brightness ? std::min<uint8_t>(variant, 3) : 0];
  if (cached.samples != nullptr && cached.count > 0) {
    M5.Speaker.playRaw(cached.samples, cached.count, kSampleRate, false, 1, 0,
                       true);
  }
}
