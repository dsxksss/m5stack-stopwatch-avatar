// SPDX-License-Identifier: AGPL-3.0-or-later

#include "avatar_engine.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979323846f;
// All pose dimensions use a device-independent 1000 x 1000 design space.
// A 466 x 466 StopWatch therefore uses a 0.466 render scale.
constexpr float kDesignSize = 1000.0f;
constexpr uint32_t kFrameIntervalUs = 16667;
constexpr uint32_t kMetricsReportIntervalMs = 5000;
constexpr uint8_t kTearingEffectPin = 38;
constexpr uint32_t kVsyncTimeoutUs = 22000;
constexpr uint16_t kBackground = TFT_BLACK;
constexpr uint16_t kEyeColor = TFT_WHITE;
// A mid-gray moving contour caused visible RGB sub-pixel breakup on the
// AMOLED. Keep moving eye geometry neutral white. Eye text is rendered in one
// pass so font rasterization cannot consume the animation frame budget.
constexpr uint16_t kEyeEdgeColor = kEyeColor;
constexpr float kTouchTravelX = 70.0f;
constexpr float kTouchTravelY = 56.0f;
// Passive IMU motion should feel alive without pulling the expression away
// from the visual center. Touch and authored reactions retain their wider
// ranges; only gravity/gyro-driven gaze uses this compact safety ellipse.
constexpr float kTiltTravelX = 44.0f;
constexpr float kTiltTravelY = 34.0f;
constexpr float kTiltLeadTravelX = 8.0f;
constexpr float kTiltLeadTravelY = 6.0f;
constexpr float kTiltSafeLimitX = 47.0f;
constexpr float kTiltSafeLimitY = 36.0f;
constexpr float kShakeTravelX = 58.0f;
constexpr float kShakeTravelY = 44.0f;
constexpr uint32_t kEyeMessageFadeInMs = 420;
constexpr uint32_t kEyeMessageFadeOutMs = 260;
constexpr uint32_t kEyeMessageBreathPeriodMs = 1800;
constexpr float kEyeMessageBreathDepth = 0.10f;
constexpr float kEyeMessageFloatPixels = 0.75f;
constexpr uint32_t kNarrativeEyeCloseMs = 320;
constexpr uint32_t kNarrativeTextFadeMs = 260;
constexpr uint32_t kNarrativeEyeOpenMs = 360;
constexpr uint32_t kModeMenuFadeMs = 220;
constexpr int kNarrativeTextWidth = 310;
constexpr int kNarrativeLineHeight = 38;
constexpr int kNarrativeTextLeft = 78;
constexpr uint8_t kNarrativeTextLuminance = 214;
constexpr uint8_t kNarrativePageIndicatorLuminance = 96;
constexpr int kNarrativePageIndicatorBottomMargin = 66;

volatile uint32_t gTearingEffectEdges = 0;

void IRAM_ATTR handleTearingEffectEdge() {
  ++gTearingEffectEdges;
}

AvatarEngine::EyePose makeEye(
    float x, float y, float width, float height, float roundness = 1.0f,
    float upperLid = 0.0f, float upperLidTilt = 0.0f,
    float lowerLid = 0.0f, float browY = -230.0f, float browTilt = 0.0f,
    float browOpacity = 0.0f, float angle = 0.0f) {
  return {x,          y,          width,    height,
          roundness, upperLid,   upperLidTilt, lowerLid,
          browY,     browTilt,   browOpacity, angle};
}

AvatarEngine::EyePose makeAngledEye(float x, float y, float width,
                                    float height, float angle,
                                    float roundness = 1.0f) {
  return makeEye(x, y, width, height, roundness, 0.0f, 0.0f, 0.0f,
                 -230.0f, 0.0f, 0.0f, angle);
}

AvatarEngine::EyePose makeAngryEye(float x, float y, float width,
                                   float height, float angle, float browTilt,
                                   float browOpacity) {
  return makeEye(x, y, width, height, 1.0f, 0.0f, 0.0f, 0.0f,
                 -150.0f, browTilt, browOpacity, angle);
}

AvatarEngine::Pose makePose(const AvatarEngine::EyePose& left,
                            const AvatarEngine::EyePose& right,
                            float faceScale = 1.0f, float faceX = 0.0f,
                            float faceY = -8.0f, float headYaw = 0.0f,
                            float headPitch = 0.0f,
                            float headRoll = 0.0f) {
  return {left, right, faceScale, faceX, faceY, headYaw, headPitch,
          headRoll};
}

AvatarEngine::Oscillator oscillator(float offset, float amplitude,
                                    float angularFrequency,
                                    float phase = 0.0f) {
  return {offset, amplitude, angularFrequency, phase};
}

AvatarEngine::MotionProfile makeMotion(
    const AvatarEngine::Oscillator& faceX,
    const AvatarEngine::Oscillator& faceY,
    const AvatarEngine::Oscillator& eyeX,
    const AvatarEngine::Oscillator& eyeY,
    const AvatarEngine::Oscillator& scale, uint16_t blendInMs = 180,
    uint16_t blendOutMs = 240) {
  return {faceX, faceY, eyeX, eyeY, scale, blendInMs, blendOutMs};
}

const AvatarEngine::MotionProfile kIdleMotion = makeMotion(
    oscillator(0.0f, 0.0f, 0.0f), oscillator(0.0f, 3.2f, 1.15f),
    oscillator(0.0f, 0.0f, 0.0f), oscillator(0.0f, 0.0f, 0.0f),
    oscillator(1.0f, 0.0045f, 1.05f));

const AvatarEngine::MotionProfile kHappyMotion = makeMotion(
    oscillator(0.0f, 0.0f, 0.0f), oscillator(0.0f, 15.0f, 8.0f),
    oscillator(0.0f, 3.5f, 1.1f),
    oscillator(0.0f, 2.0f, 0.9f, kPi * 0.5f),
    oscillator(1.0f, 0.008f, 2.4f));

const AvatarEngine::MotionProfile kAngryMotion = makeMotion(
    oscillator(0.0f, 9.0f, 32.0f), oscillator(1.5f, 2.0f, 3.0f),
    oscillator(0.0f, 3.6f, 32.0f), oscillator(0.0f, 0.8f, 2.0f),
    oscillator(1.0f, 0.004f, 2.0f), 150, 220);

const AvatarEngine::MotionProfile kSurprisedMotion = makeMotion(
    oscillator(0.0f, 1.5f, 2.2f), oscillator(-1.0f, 3.0f, 2.0f),
    oscillator(0.0f, 1.5f, 1.4f),
    oscillator(0.0f, 1.0f, 1.2f, kPi * 0.5f),
    oscillator(1.0f, 0.021f, 9.0f), 120, 260);

const AvatarEngine::MotionProfile kSleepyMotion = makeMotion(
    oscillator(0.0f, 0.0f, 0.0f), oscillator(9.0f, 4.0f, 0.9f),
    oscillator(0.0f, 0.0f, 0.0f), oscillator(0.0f, 0.0f, 0.0f),
    oscillator(1.0f, 0.003f, 0.7f), 320, 420);

const AvatarEngine::MotionProfile kListeningMotion = makeMotion(
    oscillator(0.0f, 1.2f, 0.65f), oscillator(-1.0f, 2.5f, 0.85f),
    oscillator(0.0f, 2.4f, 0.72f), oscillator(0.0f, 1.5f, 0.58f),
    oscillator(1.0f, 0.0035f, 0.95f), 280, 360);

const AvatarEngine::MotionProfile kThinkingMotion = makeMotion(
    oscillator(0.0f, 2.8f, 0.52f), oscillator(-2.0f, 3.0f, 0.68f),
    oscillator(0.0f, 4.0f, 0.63f),
    oscillator(-2.0f, 2.6f, 0.49f, kPi * 0.5f),
    oscillator(1.0f, 0.003f, 0.72f), 320, 420);

const AvatarEngine::MotionProfile kCuriousMotion = makeMotion(
    oscillator(0.0f, 2.0f, 0.9f), oscillator(-1.0f, 3.5f, 1.15f),
    oscillator(0.0f, 3.0f, 0.8f), oscillator(0.0f, 2.0f, 0.7f),
    oscillator(1.0f, 0.006f, 1.25f), 160, 300);

const AvatarEngine::MotionProfile kConfusedMotion = makeMotion(
    oscillator(0.0f, 6.0f, 4.2f), oscillator(0.0f, 2.0f, 1.1f),
    oscillator(0.0f, 3.5f, 3.8f), oscillator(0.0f, 1.5f, 1.3f),
    oscillator(1.0f, 0.004f, 1.0f), 170, 320);

const AvatarEngine::MotionProfile kExcitedMotion = makeMotion(
    oscillator(0.0f, 3.5f, 6.8f), oscillator(0.0f, 15.0f, 8.5f),
    oscillator(0.0f, 4.0f, 5.2f), oscillator(0.0f, 3.0f, 4.8f),
    oscillator(1.0f, 0.014f, 6.2f), 110, 260);

const AvatarEngine::MotionProfile kSadMotion = makeMotion(
    oscillator(0.0f, 1.2f, 0.55f), oscillator(8.0f, 3.5f, 0.72f),
    oscillator(0.0f, 1.2f, 0.48f), oscillator(2.0f, 1.0f, 0.5f),
    oscillator(1.0f, 0.0025f, 0.62f), 300, 460);

constexpr AvatarEngine::BlinkSettings kNaturalBlink = {
    true, 2600, 3400, 6200, 112, 168};
constexpr AvatarEngine::BlinkSettings kExpressiveBlink = {
    true, 1200, 1800, 3600, 72, 112};
constexpr AvatarEngine::BlinkSettings kFocusedBlink = {
    true, 2100, 2800, 5000, 76, 116};
constexpr AvatarEngine::BlinkSettings kAttentiveBlink = {
    true, 1900, 2800, 5200, 95, 145};
constexpr AvatarEngine::BlinkSettings kThoughtfulBlink = {
    true, 2600, 4200, 6800, 120, 180};
constexpr AvatarEngine::BlinkSettings kSoftBlink = {
    true, 1800, 3200, 5600, 130, 190};
constexpr AvatarEngine::BlinkSettings kNoBlink = {
    false, 0, 0, 0, 0, 0};

const AvatarEngine::Keyframe kIdleFrames[] = {
    {makePose(makeEye(-225.0f, -15.0f, 155.0f, 360.0f),
              makeEye(225.0f, -15.0f, 155.0f, 360.0f)),
     260, 0, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kListeningFrames[] = {
    {makePose(makeAngledEye(-220.0f, -18.0f, 160.0f, 325.0f, -1.0f),
              makeAngledEye(220.0f, -18.0f, 160.0f, 325.0f, 1.0f),
              1.0f, 0.0f, -9.0f, 0.0f, -3.0f, 0.0f),
     360, 900, AvatarEngine::Easing::Smooth},
    {makePose(makeAngledEye(-218.0f, -22.0f, 152.0f, 315.0f, -3.0f),
              makeAngledEye(218.0f, -20.0f, 174.0f, 340.0f, 4.0f),
              1.01f, 0.0f, -10.0f, 12.0f, -5.0f, 2.0f),
     560, 1600, AvatarEngine::Easing::Smooth},
    {makePose(makeAngledEye(-220.0f, -18.0f, 172.0f, 338.0f, -4.0f),
              makeAngledEye(220.0f, -20.0f, 154.0f, 318.0f, 2.0f),
              1.0f, 0.0f, -9.0f, -9.0f, -4.0f, -1.5f),
     540, 1450, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kThinkingFrames[] = {
    {makePose(makeAngledEye(-220.0f, -30.0f, 178.0f, 305.0f, -5.0f),
              makeAngledEye(220.0f, -34.0f, 150.0f, 250.0f, 8.0f),
              1.0f, 0.0f, -12.0f, 18.0f, -14.0f, 4.0f),
     620, 1750, AvatarEngine::Easing::Smooth},
    {makePose(makeAngledEye(-218.0f, -28.0f, 152.0f, 245.0f, -9.0f),
              makeAngledEye(218.0f, -32.0f, 205.0f, 320.0f, 5.0f),
              1.0f, 0.0f, -12.0f, -15.0f, -11.0f, -5.0f),
     640, 1600, AvatarEngine::Easing::Smooth},
    {makePose(makeAngledEye(-220.0f, -38.0f, 188.0f, 280.0f, -3.0f),
              makeAngledEye(220.0f, -38.0f, 148.0f, 245.0f, 7.0f),
              1.005f, 0.0f, -14.0f, 7.0f, -17.0f, 3.0f),
     600, 1900, AvatarEngine::Easing::Smooth},
    {makePose(makeAngledEye(-218.0f, -26.0f, 148.0f, 250.0f, -8.0f),
              makeAngledEye(218.0f, -28.0f, 195.0f, 310.0f, 4.0f),
              1.0f, 0.0f, -11.0f, -18.0f, -8.0f, -4.0f),
     620, 1700, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kHappyFrames[] = {
    {makePose(makeEye(-225.0f, 5.0f, 180.0f, 285.0f),
              makeEye(225.0f, 5.0f, 180.0f, 285.0f), 0.98f, 0.0f, 4.0f),
     115, 0, AvatarEngine::Easing::Snappy},
    {makePose(makeEye(-220.0f, -4.0f, 290.0f, 72.0f, 1.0f, 0.0f, 0.0f,
                           0.0f, -112.0f, -8.0f, 0.28f),
              makeEye(220.0f, -4.0f, 290.0f, 72.0f, 1.0f, 0.0f, 0.0f,
                           0.0f, -112.0f, 8.0f, 0.28f),
              1.035f, 0.0f, -12.0f),
     225, 920, AvatarEngine::Easing::Spring},
    {makePose(makeEye(-220.0f, -20.0f, 270.0f, 86.0f),
              makeEye(220.0f, -20.0f, 270.0f, 86.0f), 1.015f, 0.0f,
              -16.0f),
     175, 420, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kExcitedFrames[] = {
    {makePose(makeAngledEye(-220.0f, 18.0f, 205.0f, 112.0f, -3.0f),
              makeAngledEye(220.0f, 18.0f, 205.0f, 112.0f, 3.0f),
              0.965f, 0.0f, 10.0f, 0.0f, 10.0f, 0.0f),
     90, 25, AvatarEngine::Easing::Snappy},
    {makePose(makeAngledEye(-225.0f, -34.0f, 225.0f, 365.0f, -6.0f),
              makeAngledEye(225.0f, -34.0f, 225.0f, 365.0f, 6.0f),
              1.075f, 0.0f, -24.0f, 0.0f, -11.0f, 0.0f),
     155, 180, AvatarEngine::Easing::Spring},
    {makePose(makeAngledEye(-220.0f, -16.0f, 205.0f, 330.0f, -10.0f),
              makeAngledEye(220.0f, -16.0f, 205.0f, 330.0f, 2.0f),
              1.035f, 0.0f, -12.0f, -7.0f, -5.0f, -4.5f),
     135, 120, AvatarEngine::Easing::Spring},
    {makePose(makeEye(-220.0f, -12.0f, 285.0f, 82.0f),
              makeEye(220.0f, -12.0f, 285.0f, 82.0f), 1.035f, 0.0f,
              -16.0f, 7.0f, -7.0f, 4.0f),
     170, 720, AvatarEngine::Easing::Spring},
};

const AvatarEngine::Keyframe kCuriousFrames[] = {
    {makePose(makeAngledEye(-220.0f, -16.0f, 160.0f, 320.0f, -2.0f),
              makeAngledEye(220.0f, -16.0f, 160.0f, 320.0f, 2.0f),
              0.99f, 0.0f, -6.0f, 0.0f, 2.0f, 0.0f),
     120, 40, AvatarEngine::Easing::Snappy},
    {makePose(makeAngledEye(-218.0f, -28.0f, 195.0f, 360.0f, -8.0f),
              makeAngledEye(218.0f, -18.0f, 142.0f, 270.0f, 4.0f),
              1.025f, 0.0f, -14.0f, 16.0f, -8.0f, 7.0f),
     230, 850, AvatarEngine::Easing::Spring},
    {makePose(makeAngledEye(-220.0f, -22.0f, 182.0f, 342.0f, -6.0f),
              makeAngledEye(220.0f, -18.0f, 150.0f, 292.0f, 3.0f),
              1.01f, 0.0f, -11.0f, 10.0f, -5.0f, 5.0f),
     260, 520, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kConfusedFrames[] = {
    {makePose(makeAngledEye(-220.0f, -12.0f, 188.0f, 290.0f, 9.0f),
              makeAngledEye(220.0f, -26.0f, 145.0f, 250.0f, -7.0f),
              0.99f, 0.0f, -5.0f, -12.0f, 5.0f, -5.0f),
     165, 300, AvatarEngine::Easing::Snappy},
    {makePose(makeAngledEye(-218.0f, -26.0f, 145.0f, 250.0f, 7.0f),
              makeAngledEye(218.0f, -12.0f, 188.0f, 290.0f, -9.0f),
              1.005f, 0.0f, -8.0f, 12.0f, 3.0f, 5.0f),
     280, 420, AvatarEngine::Easing::Spring},
    {makePose(makeEye(-220.0f, -20.0f, 178.0f, 245.0f, 0.9f, 0.14f,
                           -0.32f, 0.0f, -195.0f, -10.0f, 0.62f),
              makeEye(220.0f, -20.0f, 178.0f, 245.0f, 0.9f, 0.14f,
                           0.32f, 0.0f, -195.0f, 10.0f, 0.62f),
              1.0f, 0.0f, -6.0f, 0.0f, 4.0f, 0.0f),
     260, 720, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kAngryFrames[] = {
    {makePose(makeAngryEye(-220.0f, -18.0f, 220.0f, 160.0f, 13.0f,
                           15.0f, 0.62f),
              makeAngryEye(220.0f, -18.0f, 220.0f, 160.0f, -13.0f,
                           -15.0f, 0.62f),
              0.985f, 0.0f, 4.0f),
     145, 80, AvatarEngine::Easing::Snappy},
    {makePose(makeAngryEye(-212.0f, -28.0f, 238.0f, 150.0f, 16.0f,
                           18.0f, 0.82f),
              makeAngryEye(212.0f, -28.0f, 238.0f, 150.0f, -16.0f,
                           -18.0f, 0.82f),
              1.015f, 0.0f, 4.0f),
     230, 1160, AvatarEngine::Easing::Spring},
    {makePose(makeAngryEye(-218.0f, -20.0f, 226.0f, 156.0f, 14.0f,
                           16.0f, 0.72f),
              makeAngryEye(218.0f, -20.0f, 226.0f, 156.0f, -14.0f,
                           -16.0f, 0.72f),
              1.0f, 0.0f, 3.0f),
     190, 350, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kSurprisedFrames[] = {
    {makePose(makeEye(-225.0f, 5.0f, 205.0f, 92.0f),
              makeEye(225.0f, 5.0f, 205.0f, 92.0f), 0.97f, 0.0f, 2.0f),
     105, 0, AvatarEngine::Easing::Snappy},
    {makePose(makeEye(-230.0f, -15.0f, 255.0f, 255.0f),
              makeEye(230.0f, -15.0f, 255.0f, 255.0f), 1.06f, 0.0f,
              -12.0f),
     215, 810, AvatarEngine::Easing::Spring},
    {makePose(makeEye(-228.0f, -14.0f, 225.0f, 275.0f),
              makeEye(228.0f, -14.0f, 225.0f, 275.0f), 1.025f, 0.0f,
              -10.0f),
     180, 430, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kSadFrames[] = {
    {makePose(makeEye(-220.0f, -5.0f, 180.0f, 275.0f, 0.95f, 0.08f,
                           -0.20f, 0.0f, -210.0f, -12.0f, 0.55f),
              makeEye(220.0f, -5.0f, 180.0f, 275.0f, 0.95f, 0.08f,
                           0.20f, 0.0f, -210.0f, 12.0f, 0.55f),
              0.995f, 0.0f, 4.0f, 0.0f, 5.0f, 0.0f),
     280, 260, AvatarEngine::Easing::Smooth},
    {makePose(makeEye(-215.0f, 18.0f, 205.0f, 205.0f, 0.9f, 0.26f,
                           -0.38f, 0.06f, -205.0f, -17.0f, 0.92f),
              makeEye(215.0f, 18.0f, 205.0f, 205.0f, 0.9f, 0.26f,
                           0.38f, 0.06f, -205.0f, 17.0f, 0.92f),
              0.985f, 0.0f, 20.0f, 0.0f, 15.0f, 0.0f),
     430, 1250, AvatarEngine::Easing::Smooth},
    {makePose(makeEye(-215.0f, 22.0f, 215.0f, 165.0f, 0.92f, 0.30f,
                           -0.42f, 0.08f, -190.0f, -18.0f, 1.0f),
              makeEye(215.0f, 22.0f, 215.0f, 165.0f, 0.92f, 0.30f,
                           0.42f, 0.08f, -190.0f, 18.0f, 1.0f),
              0.98f, 0.0f, 24.0f, -5.0f, 18.0f, -2.5f),
     360, 760, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::Keyframe kSleepyFrames[] = {
    {makePose(makeEye(-215.0f, 18.0f, 245.0f, 165.0f, 1.0f, 0.28f),
              makeEye(215.0f, 18.0f, 245.0f, 165.0f, 1.0f, 0.28f),
              0.99f, 0.0f, 18.0f),
     320, 380, AvatarEngine::Easing::Smooth},
    {makePose(makeEye(-210.0f, 48.0f, 305.0f, 48.0f),
              makeEye(210.0f, 48.0f, 305.0f, 48.0f), 0.98f, 0.0f,
              26.0f),
     520, 1380, AvatarEngine::Easing::Smooth},
};

const AvatarEngine::ExpressionSpec kExpressions[] = {
    {"IDLE", kIdleFrames, 1, AvatarEngine::PlaybackMode::Loop,
     kNaturalBlink, kIdleMotion, true},
    {"LISTENING", kListeningFrames, 3, AvatarEngine::PlaybackMode::Loop,
     kAttentiveBlink, kListeningMotion, true},
    {"THINKING", kThinkingFrames, 4, AvatarEngine::PlaybackMode::Loop,
     kThoughtfulBlink, kThinkingMotion, true},
    {"HAPPY", kHappyFrames, 3, AvatarEngine::PlaybackMode::Once,
     kExpressiveBlink, kHappyMotion, false},
    {"EXCITED", kExcitedFrames, 4, AvatarEngine::PlaybackMode::Once,
     kExpressiveBlink, kExcitedMotion, false},
    {"CURIOUS", kCuriousFrames, 3, AvatarEngine::PlaybackMode::Once,
     kAttentiveBlink, kCuriousMotion, false},
    {"CONFUSED", kConfusedFrames, 3, AvatarEngine::PlaybackMode::Once,
     kFocusedBlink, kConfusedMotion, false},
    {"ANGRY", kAngryFrames, 3, AvatarEngine::PlaybackMode::Once,
     kFocusedBlink, kAngryMotion, false},
    {"SURPRISED", kSurprisedFrames, 3, AvatarEngine::PlaybackMode::Once,
     kNoBlink, kSurprisedMotion, false},
    {"SAD", kSadFrames, 3, AvatarEngine::PlaybackMode::Once,
     kSoftBlink, kSadMotion, false},
    {"SLEEPY", kSleepyFrames, 2, AvatarEngine::PlaybackMode::Once,
     kNoBlink, kSleepyMotion, false},
};

static_assert(sizeof(kExpressions) / sizeof(kExpressions[0]) ==
                  static_cast<size_t>(ExpressionId::Count),
              "Expression catalog must match ExpressionId");

float clamp01(float value) {
  return std::max(0.0f, std::min(1.0f, value));
}

float smootherStep(float value) {
  const float progress = clamp01(value);
  return progress * progress * progress *
         (progress * (progress * 6.0f - 15.0f) + 10.0f);
}

struct AttentionPose {
  float yaw;
  float pitch;
  float roll;
  float eyeX;
  float eyeY;
  uint16_t holdMs;
  uint16_t transitionMs;
};

const AttentionPose kIdleAttention[] = {
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 4200, 620},
    {24.0f, -10.0f, 4.8f, 12.0f, -5.0f, 5200, 560},
    {-20.0f, 14.0f, -5.5f, -10.0f, 7.0f, 3600, 520},
    {11.0f, -16.0f, 2.2f, 6.0f, -8.0f, 4500, 500},
    {-27.0f, -4.0f, -4.0f, -12.0f, 1.0f, 5000, 640},
};

float mixFloat(float from, float to, float amount) {
  return from + (to - from) * amount;
}

AttentionPose mixAttention(const AttentionPose& from,
                           const AttentionPose& to, float amount) {
  return {mixFloat(from.yaw, to.yaw, amount),
          mixFloat(from.pitch, to.pitch, amount),
          mixFloat(from.roll, to.roll, amount),
          mixFloat(from.eyeX, to.eyeX, amount),
          mixFloat(from.eyeY, to.eyeY, amount), 0, 0};
}

AttentionPose sampleIdleAttention(uint32_t nowMs) {
  uint32_t cycleMs = 0;
  for (const auto& pose : kIdleAttention) {
    cycleMs += pose.holdMs + pose.transitionMs;
  }
  uint32_t cursor = cycleMs == 0 ? 0 : nowMs % cycleMs;
  const size_t count = sizeof(kIdleAttention) / sizeof(kIdleAttention[0]);
  for (size_t index = 0; index < count; ++index) {
    const AttentionPose& pose = kIdleAttention[index];
    if (cursor < pose.holdMs) return pose;
    cursor -= pose.holdMs;
    if (cursor < pose.transitionMs) {
      const float progress = smootherStep(
          cursor / static_cast<float>(std::max<uint16_t>(1,
                                                         pose.transitionMs)));
      return mixAttention(pose, kIdleAttention[(index + 1) % count], progress);
    }
    cursor -= pose.transitionMs;
  }
  return kIdleAttention[0];
}

float attentionHash(uint32_t value, float seed) {
  const float raw = sinf(value * 127.1f + seed * 311.7f) * 43758.5453f;
  return (raw - floorf(raw)) * 2.0f - 1.0f;
}

float microSaccade(uint32_t nowMs, float seed) {
  constexpr uint32_t kIntervalMs = 1100;
  constexpr uint32_t kMoveMs = 145;
  const uint32_t step = nowMs / kIntervalMs;
  const uint32_t elapsed = nowMs % kIntervalMs;
  const float previous = step == 0 ? 0.0f : attentionHash(step - 1, seed);
  const float next = attentionHash(step, seed);
  const float progress = smootherStep(
      std::min(1.0f, elapsed / static_cast<float>(kMoveMs)));
  return mixFloat(previous, next, progress);
}

float sampleOscillator(const AvatarEngine::Oscillator& oscillator,
                       float time) {
  if (oscillator.amplitude == 0.0f || oscillator.angularFrequency == 0.0f) {
    return oscillator.offset;
  }
  return oscillator.offset +
         sinf(time * oscillator.angularFrequency + oscillator.phase) *
             oscillator.amplitude;
}

struct MotionSample {
  float faceX;
  float faceY;
  float eyeX;
  float eyeY;
  float scale;
};

MotionSample sampleMotion(const AvatarEngine::MotionProfile& profile,
                          float time) {
  return {sampleOscillator(profile.faceX, time),
          sampleOscillator(profile.faceY, time),
          sampleOscillator(profile.eyeX, time),
          sampleOscillator(profile.eyeY, time),
          sampleOscillator(profile.scale, time)};
}

MotionSample mixMotion(const MotionSample& from, const MotionSample& to,
                       float amount) {
  const auto mix = [amount](float a, float b) {
    return a + (b - a) * amount;
  };
  return {mix(from.faceX, to.faceX), mix(from.faceY, to.faceY),
          mix(from.eyeX, to.eyeX), mix(from.eyeY, to.eyeY),
          mix(from.scale, to.scale)};
}

void projectEyeOntoHead(AvatarEngine::EyePose& eye, float side,
                        float headYaw, float headPitch, float headRoll) {
  const float yaw = std::max(-1.0f, std::min(1.0f, headYaw / 35.0f));
  const float pitch =
      std::max(-1.0f, std::min(1.0f, headPitch / 28.0f));
  const float horizontalCompression = 1.0f - fabsf(yaw) * 0.16f;
  const float depthScale =
      std::max(0.74f, 1.0f + side * yaw * 0.16f - fabsf(pitch) * 0.03f);

  eye.x = eye.x * horizontalCompression + yaw * 82.0f;
  eye.y += pitch * 62.0f;
  eye.width *= depthScale * (1.0f - fabsf(yaw) * 0.10f);
  eye.height *= depthScale * (1.0f - fabsf(pitch) * 0.05f);

  const float rollRadians = headRoll * kPi / 180.0f;
  const float rotatedX =
      eye.x * cosf(rollRadians) - eye.y * sinf(rollRadians);
  const float rotatedY =
      eye.x * sinf(rollRadians) + eye.y * cosf(rollRadians);
  eye.x = rotatedX;
  eye.y = rotatedY;
  eye.angle += headRoll + yaw * 7.0f + side * yaw * 3.5f;
}

bool reached(uint32_t now, uint32_t deadline) {
  return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0;
}

size_t utf8GlyphBytes(const String& text, size_t byteIndex) {
  if (byteIndex >= text.length()) return 0;
  const uint8_t lead = static_cast<uint8_t>(text[byteIndex]);
  size_t glyphBytes = 1;
  if ((lead & 0xE0) == 0xC0) {
    glyphBytes = 2;
  } else if ((lead & 0xF0) == 0xE0) {
    glyphBytes = 3;
  } else if ((lead & 0xF8) == 0xF0) {
    glyphBytes = 4;
  }
  return std::min(glyphBytes, text.length() - byteIndex);
}

}  // namespace

bool AvatarEngine::begin() {
  ready_ = M5.Display.width() > 0 && M5.Display.height() > 0;
  targetExpression_ = ExpressionId::Idle;
  baseExpression_ = ExpressionId::Idle;
  browseExpression_ = ExpressionId::Idle;
  activePlaybackMode_ = spec(ExpressionId::Idle).defaultPlaybackMode;
  currentPose_ = spec(ExpressionId::Idle).keyframes[0].pose;
  fromPose_ = currentPose_;
  targetPose_ = currentPose_;
  const uint32_t nowMs = millis();
  expressionStartedMs_ = nowMs;
  scheduleNextBlink(nowMs, true);
  metricsStartedMs_ = nowMs;
  pinMode(kTearingEffectPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(kTearingEffectPin),
                  handleTearingEffectEdge, RISING);
  forceRender_ = true;
  requiresFullClear_ = true;
  Serial.printf(
      "Avatar renderer: %d x %d, normalized scale %.3f, dirty rects, TE vsync\n",
                M5.Display.width(), M5.Display.height(),
                std::min(M5.Display.width(), M5.Display.height()) / kDesignSize);
  return ready_;
}

const AvatarEngine::ExpressionSpec& AvatarEngine::spec(ExpressionId expression) {
  const size_t index = std::min(static_cast<size_t>(expression),
                                static_cast<size_t>(ExpressionId::Count) - 1);
  return kExpressions[index];
}

const char* AvatarEngine::activeName() const {
  return spec(targetExpression_).name;
}

float AvatarEngine::ease(Easing easingMode, float progress) {
  const float value = clamp01(progress);
  if (easingMode == Easing::Snappy) {
    const float inverse = 1.0f - value;
    return 1.0f - inverse * inverse * inverse;
  }
  if (easingMode == Easing::Spring) {
    if (value >= 1.0f) return 1.0f;
    return 1.0f - expf(-6.5f * value) * cosf(10.0f * value);
  }
  return smootherStep(value);
}

AvatarEngine::EyePose AvatarEngine::interpolateEye(const EyePose& from,
                                                    const EyePose& to,
                                                    float amount) {
  const auto mix = [amount](float a, float b) { return a + (b - a) * amount; };
  return {mix(from.x, to.x),
          mix(from.y, to.y),
          mix(from.width, to.width),
          mix(from.height, to.height),
          mix(from.roundness, to.roundness),
          mix(from.upperLid, to.upperLid),
          mix(from.upperLidTilt, to.upperLidTilt),
          mix(from.lowerLid, to.lowerLid),
          mix(from.browY, to.browY),
          mix(from.browTilt, to.browTilt),
          mix(from.browOpacity, to.browOpacity),
          mix(from.angle, to.angle)};
}

AvatarEngine::Pose AvatarEngine::interpolate(const Pose& from, const Pose& to,
                                              float progress,
                                              Easing easingMode) {
  const float amount = ease(easingMode, progress);
  const auto mix = [amount](float a, float b) { return a + (b - a) * amount; };
  return {interpolateEye(from.leftEye, to.leftEye, amount),
          interpolateEye(from.rightEye, to.rightEye, amount),
          mix(from.faceScale, to.faceScale), mix(from.faceX, to.faceX),
          mix(from.faceY, to.faceY), mix(from.headYaw, to.headYaw),
          mix(from.headPitch, to.headPitch),
          mix(from.headRoll, to.headRoll)};
}

void AvatarEngine::show(ExpressionId expression, uint32_t nowMs,
                        bool autoReturn, uint16_t firstTransitionMs) {
  if (!ready_) return;

  browseExpression_ = expression;
  const ExpressionSpec& expressionSpec = spec(expression);
  if (expressionSpec.persistent) {
    baseExpression_ = expression;
    play(expression, nowMs, expressionSpec.defaultPlaybackMode, false,
         firstTransitionMs);
    return;
  }

  const PlaybackMode mode =
      autoReturn ? PlaybackMode::Once : expressionSpec.defaultPlaybackMode;
  play(expression, nowMs, mode, autoReturn, firstTransitionMs);
}

void AvatarEngine::play(ExpressionId expression, uint32_t nowMs,
                        PlaybackMode mode, bool returnToBase,
                        uint16_t firstTransitionMs) {
  if (!ready_) return;

  targetExpression_ = expression;
  returnToBase_ = returnToBase && !spec(expression).persistent;
  activePlaybackMode_ = returnToBase_ ? PlaybackMode::Once : mode;
  playbackDirection_ = 1;
  playbackComplete_ = false;
  expressionStartedMs_ = nowMs;
  expressionEndsAtMs_ = 0;
  if (returnToBase_) {
    const ExpressionSpec& expressionSpec = spec(expression);
    expressionEndsAtMs_ = nowMs;
    for (uint8_t index = 0; index < expressionSpec.keyframeCount; ++index) {
      expressionEndsAtMs_ += expressionSpec.keyframes[index].transitionMs +
                             expressionSpec.keyframes[index].holdMs;
    }
  }
  scheduleNextBlink(nowMs, true);
  startKeyframe(0, nowMs, firstTransitionMs);
  forceRender_ = true;
}

void AvatarEngine::startKeyframe(uint8_t index, uint32_t nowMs,
                                 uint16_t transitionOverrideMs) {
  const ExpressionSpec& expression = spec(targetExpression_);
  activeKeyframeIndex_ = std::min<uint8_t>(index, expression.keyframeCount - 1);
  const Keyframe& keyframe = expression.keyframes[activeKeyframeIndex_];
  fromPose_ = currentPose_;
  targetPose_ = keyframe.pose;
  transitionStartedMs_ = nowMs;
  transitionDurationMs_ = transitionOverrideMs == 0
                              ? keyframe.transitionMs
                              : transitionOverrideMs;
  transitionEasing_ = keyframe.easing;
}

void AvatarEngine::advanceTimeline(uint32_t nowMs) {
  if (playbackComplete_) return;

  const ExpressionSpec& expression = spec(targetExpression_);
  const Keyframe& keyframe = expression.keyframes[activeKeyframeIndex_];
  const uint32_t keyframeEndsAt =
      transitionStartedMs_ + transitionDurationMs_ + keyframe.holdMs;
  if (!reached(nowMs, keyframeEndsAt)) return;

  const int nextIndex =
      static_cast<int>(activeKeyframeIndex_) + playbackDirection_;
  if (nextIndex >= 0 && nextIndex < expression.keyframeCount) {
    startKeyframe(static_cast<uint8_t>(nextIndex), nowMs);
    return;
  }

  if (activePlaybackMode_ == PlaybackMode::Loop) {
    playbackDirection_ = 1;
    startKeyframe(0, nowMs);
  } else if (activePlaybackMode_ == PlaybackMode::PingPong &&
             expression.keyframeCount > 1) {
    playbackDirection_ = -playbackDirection_;
    const int reflectedIndex =
        static_cast<int>(activeKeyframeIndex_) + playbackDirection_;
    startKeyframe(static_cast<uint8_t>(reflectedIndex), nowMs);
  } else {
    completePlayback(nowMs);
  }
}

void AvatarEngine::completePlayback(uint32_t nowMs) {
  playbackComplete_ = true;
  if (returnToBase_) {
    Serial.printf("Return to base: %s\n", spec(baseExpression_).name);
    play(baseExpression_, nowMs, spec(baseExpression_).defaultPlaybackMode,
         false);
  }
}

ExpressionId AvatarEngine::adjacentExpression(int8_t direction) const {
  int index = static_cast<int>(browseExpression_) + (direction < 0 ? -1 : 1);
  if (index >= static_cast<int>(ExpressionId::Count)) index = 1;
  if (index <= 0) index = static_cast<int>(ExpressionId::Count) - 1;
  return static_cast<ExpressionId>(index);
}

void AvatarEngine::next(uint32_t nowMs, uint16_t firstTransitionMs) {
  show(adjacentExpression(1), nowMs, true, firstTransitionMs);
}

void AvatarEngine::previous(uint32_t nowMs, uint16_t firstTransitionMs) {
  show(adjacentExpression(-1), nowMs, true, firstTransitionMs);
}

bool AvatarEngine::showFromCommand(const String& rawCommand, uint32_t nowMs) {
  String command = rawCommand;
  command.trim();
  command.toLowerCase();

  PlaybackMode requestedMode = PlaybackMode::Once;
  bool hasPlaybackOverride = false;
  if (command.startsWith("loop ")) {
    requestedMode = PlaybackMode::Loop;
    hasPlaybackOverride = true;
    command.remove(0, 5);
    command.trim();
  } else if (command.startsWith("pingpong ")) {
    requestedMode = PlaybackMode::PingPong;
    hasPlaybackOverride = true;
    command.remove(0, 9);
    command.trim();
  } else if (command.startsWith("once ")) {
    requestedMode = PlaybackMode::Once;
    hasPlaybackOverride = true;
    command.remove(0, 5);
    command.trim();
  }

  ExpressionId expression = ExpressionId::Idle;
  if (command == "idle" || command == "neutral") {
    expression = ExpressionId::Idle;
  } else if (command == "listening" || command == "listen") {
    expression = ExpressionId::Listening;
  } else if (command == "thinking" || command == "think") {
    expression = ExpressionId::Thinking;
  } else if (command == "happy" || command == "smile") {
    expression = ExpressionId::Happy;
  } else if (command == "excited" || command == "excite") {
    expression = ExpressionId::Excited;
  } else if (command == "curious" || command == "curiosity") {
    expression = ExpressionId::Curious;
  } else if (command == "confused" || command == "confuse") {
    expression = ExpressionId::Confused;
  } else if (command == "angry") {
    expression = ExpressionId::Angry;
  } else if (command == "surprised" || command == "surprise") {
    expression = ExpressionId::Surprised;
  } else if (command == "sad") {
    expression = ExpressionId::Sad;
  } else if (command == "sleepy" || command == "sleep") {
    expression = ExpressionId::Sleepy;
  } else {
    return false;
  }

  if (hasPlaybackOverride) {
    play(expression, nowMs, requestedMode, false);
  } else {
    show(expression, nowMs, !spec(expression).persistent);
  }
  return true;
}

void AvatarEngine::setTouchTarget(int16_t screenX, int16_t screenY) {
  if (!ready_) return;
  const float halfWidth = std::max(1.0f, M5.Display.width() * 0.5f);
  const float halfHeight = std::max(1.0f, M5.Display.height() * 0.5f);
  const float normalizedX = std::max(
      -1.0f, std::min(1.0f, (screenX - halfWidth) / halfWidth));
  const float normalizedY = std::max(
      -1.0f, std::min(1.0f, (screenY - halfHeight) / halfHeight));
  touchTargetX_ = normalizedX * kTouchTravelX;
  touchTargetY_ = normalizedY * kTouchTravelY;
  touchActive_ = true;
}

void AvatarEngine::releaseTouch() {
  touchActive_ = false;
}

void AvatarEngine::setSwipeOffset(float screenDeltaX, float screenDeltaY,
                                  int8_t previewDirection) {
  swipeActive_ = true;
  swipeTargetX_ = std::max(-130.0f, std::min(130.0f, screenDeltaX * 0.96f));
  swipeTargetY_ = std::max(-115.0f, std::min(115.0f, screenDeltaY * 0.90f));
  swipePreviewTarget_ = previewDirection == 0
                            ? 0.0f
                            : std::min(0.34f,
                                       fabsf(screenDeltaX) / 52.0f * 0.34f);
  swipePreviewDirection_ = previewDirection;
}

void AvatarEngine::releaseSwipe() {
  swipeActive_ = false;
  swipeTargetX_ = 0.0f;
  swipeTargetY_ = 0.0f;
  swipePreviewTarget_ = 0.0f;
}

void AvatarEngine::commitSwipe(int8_t direction, uint32_t nowMs,
                               uint16_t firstTransitionMs) {
  if (!ready_ || direction == 0) return;

  const ExpressionId target = adjacentExpression(direction);
  const Pose& previewPose = spec(target).keyframes[0].pose;
  if (swipePreviewAmount_ > 0.01f) {
    currentPose_ = interpolate(currentPose_, previewPose,
                               swipePreviewAmount_, Easing::Smooth);
  }
  // Keep the swipe offset active after the expression changes. The same
  // finger may continue moving, so the new face must remain attached to it
  // until releaseSwipe() is called on finger-up.
  swipePreviewAmount_ = 0.0f;
  swipePreviewTarget_ = 0.0f;
  swipePreviewDirection_ = 0;
  show(target, nowMs, true, firstTransitionMs);
}

void AvatarEngine::setTiltTarget(float normalizedX, float normalizedY,
                                 float motionLeadX, float motionLeadY) {
  const auto shapeTilt = [](float value) {
    constexpr float kDeadZone = 0.11f;
    const float clamped = std::max(-1.0f, std::min(1.0f, value));
    const float magnitude = fabsf(clamped);
    if (magnitude <= kDeadZone) return 0.0f;
    const float normalized = (magnitude - kDeadZone) / (1.0f - kDeadZone);
    // Keep small wrist noise calm and approach the edge progressively.
    const float shaped = normalized * (1.15f - 0.15f * normalized * normalized);
    return copysignf(shaped, clamped);
  };
  tiltTargetX_ = shapeTilt(normalizedX) * kTiltTravelX;
  tiltTargetY_ = shapeTilt(normalizedY) * kTiltTravelY;
  tiltLeadTargetX_ =
      std::max(-1.0f, std::min(1.0f, motionLeadX)) * kTiltLeadTravelX;
  tiltLeadTargetY_ =
      std::max(-1.0f, std::min(1.0f, motionLeadY)) * kTiltLeadTravelY;
}

void AvatarEngine::setShakeTarget(float normalizedX, float normalizedY,
                                  float intensity) {
  shakeIntensityTarget_ = clamp01(intensity);
  shakeTargetX_ = std::max(-1.0f, std::min(1.0f, normalizedX)) *
                  kShakeTravelX * shakeIntensityTarget_;
  shakeTargetY_ = std::max(-1.0f, std::min(1.0f, normalizedY)) *
                  kShakeTravelY * shakeIntensityTarget_;
}

void AvatarEngine::updateInteraction(uint32_t nowMs) {
  if (lastInteractionUpdateMs_ == 0) {
    lastInteractionUpdateMs_ = nowMs;
    return;
  }

  const float elapsedSeconds = std::min(
      0.033f, std::max(0.001f, (nowMs - lastInteractionUpdateMs_) / 1000.0f));
  lastInteractionUpdateMs_ = nowMs;

  float targetX = touchActive_ ? touchTargetX_
                               : tiltTargetX_ + tiltLeadTargetX_;
  float targetY = touchActive_ ? touchTargetY_
                               : tiltTargetY_ + tiltLeadTargetY_;
  if (!touchActive_) {
    // Keep diagonal tilt inside one centered ellipse instead of allowing the
    // independent X/Y limits to add up into a large corner displacement.
    const float ellipseRadius =
        sqrtf((targetX * targetX) / (kTiltSafeLimitX * kTiltSafeLimitX) +
              (targetY * targetY) / (kTiltSafeLimitY * kTiltSafeLimitY));
    if (ellipseRadius > 1.0f) {
      targetX /= ellipseRadius;
      targetY /= ellipseRadius;
    }
  }
  // A damped spring gives direct tracking while pressed and a short, visible
  // rebound when the finger leaves the glass. The values are in design space,
  // so the same feel is retained if the display resolution changes.
  const float stiffness = touchActive_ ? 150.0f : 185.0f;
  const float damping = touchActive_ ? 21.0f : 25.5f;
  interactionVelocityX_ +=
      ((targetX - interactionX_) * stiffness -
       interactionVelocityX_ * damping) * elapsedSeconds;
  interactionVelocityY_ +=
      ((targetY - interactionY_) * stiffness -
       interactionVelocityY_ * damping) * elapsedSeconds;
  interactionX_ += interactionVelocityX_ * elapsedSeconds;
  interactionY_ += interactionVelocityY_ * elapsedSeconds;
  if (!touchActive_) {
    if (fabsf(interactionX_) > kTiltSafeLimitX) {
      interactionX_ = std::max(-kTiltSafeLimitX,
                               std::min(kTiltSafeLimitX, interactionX_));
      interactionVelocityX_ *= 0.22f;
    }
    if (fabsf(interactionY_) > kTiltSafeLimitY) {
      interactionY_ = std::max(-kTiltSafeLimitY,
                               std::min(kTiltSafeLimitY, interactionY_));
      interactionVelocityY_ *= 0.22f;
    }
  }

  // High-pass IMU acceleration drives a faster spring than ordinary tilt.
  // The small overshoot is intentional: the face appears to have mass instead
  // of merely copying the device position at the instant a shake begins.
  constexpr float kShakeStiffness = 235.0f;
  constexpr float kShakeDamping = 18.0f;
  shakeVelocityX_ +=
      ((shakeTargetX_ - shakeOffsetX_) * kShakeStiffness -
       shakeVelocityX_ * kShakeDamping) * elapsedSeconds;
  shakeVelocityY_ +=
      ((shakeTargetY_ - shakeOffsetY_) * kShakeStiffness -
       shakeVelocityY_ * kShakeDamping) * elapsedSeconds;
  shakeOffsetX_ += shakeVelocityX_ * elapsedSeconds;
  shakeOffsetY_ += shakeVelocityY_ * elapsedSeconds;
  shakeIntensity_ +=
      (shakeIntensityTarget_ - shakeIntensity_) *
      std::min(1.0f, elapsedSeconds * 13.0f);

  const float normalizedX = touchActive_
                                ? touchTargetX_ / kTouchTravelX
                                : tiltTargetX_ / kTiltTravelX;
  const float normalizedY = touchActive_
                                ? touchTargetY_ / kTouchTravelY
                                : tiltTargetY_ / kTiltTravelY;
  const float targetYaw = normalizedX * (touchActive_ ? 32.0f : 8.0f);
  const float targetPitch = normalizedY * (touchActive_ ? 24.0f : 6.0f);
  const float targetRoll = -normalizedX * (touchActive_ ? 7.0f : 1.5f);
  const float headStiffness = touchActive_ ? 46.0f : 15.0f;
  const float headDamping = touchActive_ ? 11.5f : 7.0f;

  const auto updateHeadAxis = [elapsedSeconds, headStiffness, headDamping](
                                  float target, float& value,
                                  float& velocity) {
    velocity += ((target - value) * headStiffness - velocity * headDamping) *
                elapsedSeconds;
    value += velocity * elapsedSeconds;
  };
  updateHeadAxis(targetYaw, headYaw_, headYawVelocity_);
  updateHeadAxis(targetPitch, headPitch_, headPitchVelocity_);
  updateHeadAxis(targetRoll, headRoll_, headRollVelocity_);

  if (swipeActive_) {
    swipeVelocityX_ = std::max(
        -900.0f, std::min(900.0f,
                          (swipeTargetX_ - swipeOffsetX_) / elapsedSeconds));
    swipeVelocityY_ = std::max(
        -900.0f, std::min(900.0f,
                          (swipeTargetY_ - swipeOffsetY_) / elapsedSeconds));
    swipeOffsetX_ = swipeTargetX_;
    swipeOffsetY_ = swipeTargetY_;
    swipePreviewAmount_ = swipePreviewTarget_;
  } else {
    constexpr float kSwipeReturnStiffness = 190.0f;
    constexpr float kSwipeReturnDamping = 24.0f;
    swipeVelocityX_ +=
        ((swipeTargetX_ - swipeOffsetX_) * kSwipeReturnStiffness -
         swipeVelocityX_ * kSwipeReturnDamping) * elapsedSeconds;
    swipeVelocityY_ +=
        ((swipeTargetY_ - swipeOffsetY_) * kSwipeReturnStiffness -
         swipeVelocityY_ * kSwipeReturnDamping) * elapsedSeconds;
    swipeOffsetX_ += swipeVelocityX_ * elapsedSeconds;
    swipeOffsetY_ += swipeVelocityY_ * elapsedSeconds;
    swipePreviewAmount_ +=
        (swipePreviewTarget_ - swipePreviewAmount_) *
        std::min(1.0f, elapsedSeconds * 18.0f);
    if (swipePreviewAmount_ < 0.005f) swipePreviewDirection_ = 0;
  }

  const float tiltMagnitude =
      std::min(1.0f, sqrtf(normalizedX * normalizedX +
                           normalizedY * normalizedY));
  const float tiltControl = clamp01((tiltMagnitude - 0.06f) / 0.40f);
  const float attentionTarget =
      touchActive_ ? 0.08f : 1.0f - tiltControl * 0.90f;
  attentionWeight_ +=
      (attentionTarget - attentionWeight_) *
      std::min(1.0f, elapsedSeconds * (touchActive_ ? 8.0f : 3.5f));
}

void AvatarEngine::invalidate() {
  forceRender_ = true;
  requiresFullClear_ = true;
  previousLeftBounds_.valid = false;
  previousRightBounds_.valid = false;
  nextFrameUs_ = 0;
}

void AvatarEngine::setEnergyUi(float normalizedLevel, bool charging,
                               bool affectMood) {
  energyUiEnabled_ = true;
  energyCharging_ = charging;
  energyMoodEnabled_ = affectMood;
  energyLevel_ = clamp01(normalizedLevel);
  energyDismissStartedMs_ = 0;
  forceRender_ = true;
}

void AvatarEngine::setEyeMessage(const String& leftText,
                                 const String& rightText, uint32_t nowMs,
                                 uint32_t holdMs, bool vertical) {
  leftEyeMessage_ = leftText;
  rightEyeMessage_ = rightText;
  eyeMessageVertical_ = vertical;
  eyeMessageStartedMs_ = nowMs;
  eyeMessageEndsAtMs_ = nowMs + holdMs;
  forceRender_ = true;
}

bool AvatarEngine::showNarrativeText(const String& text, uint32_t nowMs,
                                     uint16_t glyphIntervalMs,
                                     bool skipEyeClose) {
  narrativeText_ = text;
  narrativeText_.trim();
  narrativeText_.replace("\r\n", "\n");
  narrativeText_.replace('\r', '\n');
  if (narrativeText_.isEmpty()) return false;

  narrativeGlyphIntervalMs_ =
      std::max<uint16_t>(24, std::min<uint16_t>(glyphIntervalMs, 240));
  layoutNarrativeText();
  if (narrativeLineCount_ == 0 || narrativeGlyphCount_ == 0) return false;

  releaseTouch();
  releaseSwipe();
  narrativeActive_ = true;
  narrativeDismissAfterFade_ = false;
  narrativePhase_ = skipEyeClose ? NarrativePhase::Typing
                                 : NarrativePhase::ClosingEyes;
  narrativePageIndex_ = 0;
  narrativeCanvasPrepared_ = false;
  narrativeRenderedGlyphs_ = 0;
  narrativeFadeErasedWidth_ = 0;
  narrativePhaseStartedMs_ = nowMs;
  forceRender_ = true;
  requiresFullClear_ = true;
  previousLeftBounds_.valid = false;
  previousRightBounds_.valid = false;
  Serial.printf("Narrative text started: glyphs=%u lines=%u pages=%u\n",
                narrativeGlyphCount_, narrativeLineCount_,
                narrativePageCount_);
  return true;
}

void AvatarEngine::layoutNarrativeText() {
  narrativeGlyphCount_ = 0;
  narrativeLineCount_ = 0;
  narrativePageCount_ = 0;

  size_t byteIndex = 0;
  while (byteIndex < narrativeText_.length() &&
         narrativeGlyphCount_ < kNarrativeMaxGlyphs) {
    narrativeGlyphOffsets_[narrativeGlyphCount_++] = byteIndex;
    byteIndex += utf8GlyphBytes(narrativeText_, byteIndex);
  }
  narrativeGlyphOffsets_[narrativeGlyphCount_] = byteIndex;
  if (byteIndex < narrativeText_.length()) {
    narrativeText_.remove(byteIndex);
  }

  M5.Display.setFont(&fonts::efontCN_24_b);
  M5.Display.setTextSize(1);
  uint16_t lineStartGlyph = 0;
  int lineWidth = 0;
  for (uint16_t glyphIndex = 0; glyphIndex < narrativeGlyphCount_;
       ++glyphIndex) {
    const String glyph = narrativeText_.substring(
        narrativeGlyphOffsets_[glyphIndex],
        narrativeGlyphOffsets_[glyphIndex + 1]);
    if (glyph == "\n") {
      if (narrativeLineCount_ < kNarrativeMaxLines) {
        narrativeLineStartGlyph_[narrativeLineCount_] = lineStartGlyph;
        narrativeLineEndGlyph_[narrativeLineCount_] = glyphIndex;
        ++narrativeLineCount_;
      }
      lineStartGlyph = glyphIndex + 1;
      lineWidth = 0;
      continue;
    }

    const int glyphWidth = std::max(1, M5.Display.textWidth(glyph));
    if (lineWidth > 0 && lineWidth + glyphWidth > kNarrativeTextWidth) {
      if (narrativeLineCount_ < kNarrativeMaxLines) {
        narrativeLineStartGlyph_[narrativeLineCount_] = lineStartGlyph;
        narrativeLineEndGlyph_[narrativeLineCount_] = glyphIndex;
        ++narrativeLineCount_;
      }
      lineStartGlyph = glyphIndex;
      lineWidth = 0;
    }
    lineWidth += glyphWidth;
  }

  if (lineStartGlyph < narrativeGlyphCount_ &&
      narrativeLineCount_ < kNarrativeMaxLines) {
    narrativeLineStartGlyph_[narrativeLineCount_] = lineStartGlyph;
    narrativeLineEndGlyph_[narrativeLineCount_] = narrativeGlyphCount_;
    ++narrativeLineCount_;
  }
  narrativePageCount_ =
      (narrativeLineCount_ + kNarrativeLinesPerPage - 1) /
      kNarrativeLinesPerPage;
}

uint16_t AvatarEngine::narrativePageGlyphCount(uint16_t page) const {
  const uint16_t firstLine = page * kNarrativeLinesPerPage;
  const uint16_t lastLine = std::min<uint16_t>(
      narrativeLineCount_, firstLine + kNarrativeLinesPerPage);
  uint16_t total = 0;
  for (uint16_t line = firstLine; line < lastLine; ++line) {
    total += narrativeLineEndGlyph_[line] - narrativeLineStartGlyph_[line];
  }
  return total;
}

void AvatarEngine::beginNarrativeFade(uint32_t nowMs,
                                      bool dismissAfterFade) {
  narrativeDismissAfterFade_ = dismissAfterFade;
  narrativePhase_ = NarrativePhase::FadingText;
  narrativePhaseStartedMs_ = nowMs;
  narrativeFadeErasedWidth_ = 0;
  forceRender_ = true;
}

void AvatarEngine::advanceNarrativeText(uint32_t nowMs) {
  if (!narrativeActive_) return;
  switch (narrativePhase_) {
    case NarrativePhase::ClosingEyes:
      narrativePhase_ = NarrativePhase::Typing;
      narrativePhaseStartedMs_ = nowMs;
      narrativeCanvasPrepared_ = false;
      narrativeRenderedGlyphs_ = 0;
      requiresFullClear_ = true;
      Serial.println("Narrative advance: skipped closing transition");
      break;
    case NarrativePhase::Typing:
      narrativePhase_ = NarrativePhase::Holding;
      narrativePhaseStartedMs_ = nowMs;
      Serial.printf("Narrative advance: page %u/%u revealed\n",
                    narrativePageIndex_ + 1, narrativePageCount_);
      break;
    case NarrativePhase::Holding:
      beginNarrativeFade(nowMs, false);
      Serial.printf("Narrative advance: page %u/%u turning\n",
                    narrativePageIndex_ + 1, narrativePageCount_);
      break;
    case NarrativePhase::FadingText:
      narrativePhaseStartedMs_ = nowMs - kNarrativeTextFadeMs;
      break;
    case NarrativePhase::OpeningEyes:
      cancelNarrativeText();
      break;
    case NarrativePhase::Inactive:
      break;
  }
  forceRender_ = true;
}

void AvatarEngine::dismissNarrativeText(uint32_t nowMs) {
  if (!narrativeActive_) return;
  if (narrativePhase_ == NarrativePhase::ClosingEyes) {
    narrativePhase_ = NarrativePhase::OpeningEyes;
    narrativePhaseStartedMs_ = nowMs;
    requiresFullClear_ = true;
  } else if (narrativePhase_ != NarrativePhase::OpeningEyes) {
    beginNarrativeFade(nowMs, true);
  }
  Serial.printf("Narrative dismiss requested: page %u/%u\n",
                narrativePageIndex_ + 1, narrativePageCount_);
  forceRender_ = true;
}

void AvatarEngine::cancelNarrativeText() {
  if (!narrativeActive_) return;
  narrativeActive_ = false;
  narrativeDismissAfterFade_ = false;
  narrativePhase_ = NarrativePhase::Inactive;
  narrativeText_ = "";
  narrativeGlyphCount_ = 0;
  narrativeLineCount_ = 0;
  narrativePageCount_ = 0;
  narrativePageIndex_ = 0;
  narrativeCanvasPrepared_ = false;
  narrativeRenderedGlyphs_ = 0;
  narrativeFadeErasedWidth_ = 0;
  forceRender_ = true;
  requiresFullClear_ = true;
  previousLeftBounds_.valid = false;
  previousRightBounds_.valid = false;
}

bool AvatarEngine::modeMenuReady() const {
  return modeMenuActive_ && modeMenuPhase_ == ModeMenuPhase::Visible;
}

void AvatarEngine::showModeMenu(const String& title, const String& item,
                                const String& status, const String& detail,
                                uint32_t nowMs) {
  releaseTouch();
  releaseSwipe();
  modeMenuTitle_ = title;
  modeMenuItem_ = item;
  modeMenuStatus_ = status;
  modeMenuDetail_ = detail;
  modeMenuActive_ = true;
  modeMenuCanvasPrepared_ = false;
  modeMenuPhase_ = ModeMenuPhase::ClosingEyes;
  modeMenuPhaseStartedMs_ = nowMs;
  forceRender_ = true;
  previousLeftBounds_.valid = false;
  previousRightBounds_.valid = false;
  Serial.println("Mode menu opening");
}

void AvatarEngine::setModeMenuContent(const String& item,
                                      const String& status,
                                      const String& detail) {
  if (!modeMenuActive_) return;
  modeMenuItem_ = item;
  modeMenuStatus_ = status;
  modeMenuDetail_ = detail;
  modeMenuCanvasPrepared_ = false;
  forceRender_ = true;
}

void AvatarEngine::dismissModeMenu(uint32_t nowMs) {
  if (!modeMenuActive_) return;
  if (modeMenuPhase_ == ModeMenuPhase::ClosingEyes) {
    modeMenuPhase_ = ModeMenuPhase::OpeningEyes;
  } else if (modeMenuPhase_ != ModeMenuPhase::OpeningEyes) {
    modeMenuPhase_ = ModeMenuPhase::FadingOut;
  }
  modeMenuPhaseStartedMs_ = nowMs;
  modeMenuCanvasPrepared_ = false;
  forceRender_ = true;
  Serial.println("Mode menu closing");
}

void AvatarEngine::cancelModeMenu() {
  if (!modeMenuActive_) return;
  modeMenuActive_ = false;
  modeMenuCanvasPrepared_ = false;
  modeMenuPhase_ = ModeMenuPhase::Inactive;
  modeMenuTitle_ = "";
  modeMenuItem_ = "";
  modeMenuStatus_ = "";
  modeMenuDetail_ = "";
  modeMenuPhaseStartedMs_ = 0;
  forceRender_ = true;
  requiresFullClear_ = true;
  previousLeftBounds_.valid = false;
  previousRightBounds_.valid = false;
}

void AvatarEngine::updateModeMenu(uint32_t nowMs) {
  if (!modeMenuActive_) return;
  const uint32_t elapsed = nowMs - modeMenuPhaseStartedMs_;
  switch (modeMenuPhase_) {
    case ModeMenuPhase::ClosingEyes:
      if (elapsed >= kNarrativeEyeCloseMs) {
        modeMenuPhase_ = ModeMenuPhase::FadingIn;
        modeMenuPhaseStartedMs_ = nowMs;
        modeMenuCanvasPrepared_ = false;
        requiresFullClear_ = true;
      }
      break;
    case ModeMenuPhase::FadingIn:
      if (elapsed >= kModeMenuFadeMs) {
        modeMenuPhase_ = ModeMenuPhase::Visible;
        modeMenuPhaseStartedMs_ = nowMs;
        modeMenuCanvasPrepared_ = false;
      }
      break;
    case ModeMenuPhase::Visible:
      break;
    case ModeMenuPhase::FadingOut:
      if (elapsed >= kModeMenuFadeMs) {
        modeMenuPhase_ = ModeMenuPhase::OpeningEyes;
        modeMenuPhaseStartedMs_ = nowMs;
        modeMenuCanvasPrepared_ = false;
        requiresFullClear_ = true;
      }
      break;
    case ModeMenuPhase::OpeningEyes:
      if (elapsed >= kNarrativeEyeOpenMs) {
        cancelModeMenu();
        Serial.println("Mode menu closed; expression restored");
      }
      break;
    case ModeMenuPhase::Inactive:
      break;
  }
}

void AvatarEngine::drawModeMenu(uint32_t nowMs) {
  if (!modeMenuActive_) return;
  float alpha = 1.0f;
  if (modeMenuPhase_ == ModeMenuPhase::FadingIn) {
    alpha = smootherStep((nowMs - modeMenuPhaseStartedMs_) /
                         static_cast<float>(kModeMenuFadeMs));
  } else if (modeMenuPhase_ == ModeMenuPhase::FadingOut) {
    alpha = 1.0f - smootherStep((nowMs - modeMenuPhaseStartedMs_) /
                                static_cast<float>(kModeMenuFadeMs));
  }
  alpha = clamp01(alpha);
  const uint8_t white = static_cast<uint8_t>(lroundf(238.0f * alpha));
  const uint8_t gray = static_cast<uint8_t>(lroundf(132.0f * alpha));
  const uint8_t dim = static_cast<uint8_t>(lroundf(78.0f * alpha));
  const uint16_t whiteColor = M5.Display.color565(white, white, white);
  const uint16_t grayColor = M5.Display.color565(gray, gray, gray);
  const uint16_t dimColor = M5.Display.color565(dim, dim, dim);
  const int centerX = M5.Display.width() / 2;
  const int bookHalfWidth = 52;
  const int bookHalfHeight = 34;
  const int bookCenterY = 180;

  if (!modeMenuCanvasPrepared_) M5.Display.fillScreen(kBackground);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::efontCN_16);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(grayColor, kBackground);
  M5.Display.drawString(modeMenuTitle_, centerX, 82);

  for (int offset = 0; offset < 2; ++offset) {
    M5.Display.drawLine(centerX, bookCenterY - bookHalfHeight + offset,
                        centerX, bookCenterY + bookHalfHeight, whiteColor);
    M5.Display.drawLine(centerX - bookHalfWidth,
                        bookCenterY - bookHalfHeight + 7 + offset, centerX,
                        bookCenterY - bookHalfHeight + offset, whiteColor);
    M5.Display.drawLine(centerX + bookHalfWidth,
                        bookCenterY - bookHalfHeight + 7 + offset, centerX,
                        bookCenterY - bookHalfHeight + offset, whiteColor);
    M5.Display.drawLine(centerX - bookHalfWidth,
                        bookCenterY - bookHalfHeight + 7 + offset,
                        centerX - bookHalfWidth, bookCenterY + bookHalfHeight,
                        whiteColor);
    M5.Display.drawLine(centerX + bookHalfWidth,
                        bookCenterY - bookHalfHeight + 7 + offset,
                        centerX + bookHalfWidth, bookCenterY + bookHalfHeight,
                        whiteColor);
    M5.Display.drawLine(centerX - bookHalfWidth,
                        bookCenterY + bookHalfHeight, centerX,
                        bookCenterY + bookHalfHeight + 8 + offset, whiteColor);
    M5.Display.drawLine(centerX + bookHalfWidth,
                        bookCenterY + bookHalfHeight, centerX,
                        bookCenterY + bookHalfHeight + 8 + offset, whiteColor);
  }

  M5.Display.setFont(&fonts::efontCN_24_b);
  M5.Display.setTextColor(whiteColor, kBackground);
  M5.Display.drawString(modeMenuItem_, centerX, 274);
  M5.Display.setFont(&fonts::efontCN_16);
  M5.Display.setTextColor(grayColor, kBackground);
  M5.Display.drawString(modeMenuStatus_, centerX, 316);
  M5.Display.setTextColor(dimColor, kBackground);
  M5.Display.drawString(modeMenuDetail_, centerX, 347);
  M5.Display.drawString(modeMenuStatus_ == "等待长文"
                            ? "等待投送   B 返回"
                            : "A 进入   B 返回",
                        centerX, 397);
  M5.Display.setTextDatum(top_left);
}

void AvatarEngine::updateNarrativeText(uint32_t nowMs) {
  if (!narrativeActive_) return;
  const uint32_t elapsed = nowMs - narrativePhaseStartedMs_;
  switch (narrativePhase_) {
    case NarrativePhase::ClosingEyes:
      if (elapsed >= kNarrativeEyeCloseMs) {
        narrativePhase_ = NarrativePhase::Typing;
        narrativePhaseStartedMs_ = nowMs;
        narrativeCanvasPrepared_ = false;
        narrativeRenderedGlyphs_ = 0;
        narrativeFadeErasedWidth_ = 0;
        requiresFullClear_ = true;
      }
      break;
    case NarrativePhase::Typing: {
      const uint16_t pageGlyphs =
          narrativePageGlyphCount(narrativePageIndex_);
      if (elapsed / narrativeGlyphIntervalMs_ >= pageGlyphs) {
        narrativePhase_ = NarrativePhase::Holding;
        narrativePhaseStartedMs_ = nowMs;
      }
      break;
    }
    case NarrativePhase::Holding:
      break;
    case NarrativePhase::FadingText:
      if (elapsed >= kNarrativeTextFadeMs) {
        if (!narrativeDismissAfterFade_) {
          narrativePageIndex_ =
              narrativePageIndex_ + 1 < narrativePageCount_
                  ? narrativePageIndex_ + 1
                  : 0;
          narrativePhase_ = NarrativePhase::Typing;
          narrativePhaseStartedMs_ = nowMs;
          narrativeCanvasPrepared_ = false;
          narrativeRenderedGlyphs_ = 0;
          narrativeFadeErasedWidth_ = 0;
        } else {
          narrativePhase_ = NarrativePhase::OpeningEyes;
          narrativePhaseStartedMs_ = nowMs;
        }
        requiresFullClear_ = true;
      }
      break;
    case NarrativePhase::OpeningEyes:
      if (elapsed >= kNarrativeEyeOpenMs) {
        cancelNarrativeText();
        Serial.println("Narrative text closed; expression restored");
      }
      break;
    case NarrativePhase::Inactive:
      break;
  }
}

void AvatarEngine::drawNarrativeGlyph(uint16_t pageGlyphIndex,
                                      uint8_t luminance) {
  if (narrativePageIndex_ >= narrativePageCount_) return;
  const uint16_t firstLine = narrativePageIndex_ * kNarrativeLinesPerPage;
  const uint16_t lastLine = std::min<uint16_t>(
      narrativeLineCount_, firstLine + kNarrativeLinesPerPage);
  const uint16_t pageLineCount = lastLine - firstLine;
  const int firstY = M5.Display.height() / 2 -
                     static_cast<int>(pageLineCount - 1) *
                         kNarrativeLineHeight / 2;

  M5.Display.setFont(&fonts::efontCN_24_b);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(middle_left);
  M5.Display.setTextColor(M5.Display.color565(luminance, luminance, luminance));

  uint16_t remainingGlyph = pageGlyphIndex;
  for (uint16_t lineOffset = 0; lineOffset < pageLineCount; ++lineOffset) {
    const uint16_t line = firstLine + lineOffset;
    const uint16_t lineStart = narrativeLineStartGlyph_[line];
    const uint16_t lineGlyphs =
        narrativeLineEndGlyph_[line] - lineStart;
    if (remainingGlyph >= lineGlyphs) {
      remainingGlyph -= lineGlyphs;
      continue;
    }

    const uint16_t glyphIndex = lineStart + remainingGlyph;
    const String prefix = narrativeText_.substring(
        narrativeGlyphOffsets_[lineStart],
        narrativeGlyphOffsets_[glyphIndex]);
    const String glyph = narrativeText_.substring(
        narrativeGlyphOffsets_[glyphIndex],
        narrativeGlyphOffsets_[glyphIndex + 1]);
    const int glyphX = kNarrativeTextLeft + M5.Display.textWidth(prefix);
    const int lineY = firstY + lineOffset * kNarrativeLineHeight;
    M5.Display.drawString(glyph, glyphX, lineY);
    break;
  }
  M5.Display.setTextDatum(top_left);
}

void AvatarEngine::drawNarrativePageIndicator() {
  if (narrativePageCount_ <= 1 ||
      narrativePageIndex_ >= narrativePageCount_) {
    return;
  }

  const String pageLabel = String(narrativePageIndex_ + 1) + "/" +
                           String(narrativePageCount_);
  const uint8_t luminance = kNarrativePageIndicatorLuminance;
  M5.Display.setFont(&fonts::efontCN_16);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(
      M5.Display.color565(luminance, luminance, luminance));
  M5.Display.drawString(pageLabel, M5.Display.width() / 2,
                        M5.Display.height() -
                            kNarrativePageIndicatorBottomMargin);
  M5.Display.setTextDatum(top_left);
}

void AvatarEngine::drawNarrativePage(uint32_t nowMs) {
  if (narrativePageIndex_ >= narrativePageCount_) return;
  if (!narrativeCanvasPrepared_) {
    M5.Display.fillScreen(kBackground);
    drawNarrativePageIndicator();
    narrativeCanvasPrepared_ = true;
    narrativeRenderedGlyphs_ = 0;
          narrativeFadeErasedWidth_ = 0;
          Serial.printf("Narrative page opened: %u/%u\n",
                        narrativePageIndex_ + 1, narrativePageCount_);
  }

  const uint16_t pageGlyphs =
      narrativePageGlyphCount(narrativePageIndex_);
  if (narrativePhase_ == NarrativePhase::Typing) {
    const uint32_t elapsed = nowMs - narrativePhaseStartedMs_;
    const uint16_t completedGlyphs = std::min<uint16_t>(
        pageGlyphs, elapsed / narrativeGlyphIntervalMs_);
    while (narrativeRenderedGlyphs_ < completedGlyphs) {
      drawNarrativeGlyph(narrativeRenderedGlyphs_,
                         kNarrativeTextLuminance);
      ++narrativeRenderedGlyphs_;
    }
    if (completedGlyphs < pageGlyphs) {
      const float progress =
          (elapsed % narrativeGlyphIntervalMs_) /
          static_cast<float>(narrativeGlyphIntervalMs_);
      const uint8_t luminance = static_cast<uint8_t>(
          lroundf(72.0f + smootherStep(progress) * 183.0f));
      drawNarrativeGlyph(completedGlyphs, luminance);
    }
    return;
  }

  if (narrativePhase_ == NarrativePhase::Holding) {
    const bool completedThisFrame = narrativeRenderedGlyphs_ < pageGlyphs;
    while (narrativeRenderedGlyphs_ < pageGlyphs) {
      drawNarrativeGlyph(narrativeRenderedGlyphs_,
                         kNarrativeTextLuminance);
      ++narrativeRenderedGlyphs_;
    }
    if (completedThisFrame) {
      Serial.printf("Narrative page rendered complete: %u/%u glyphs=%u\n",
                    narrativePageIndex_ + 1, narrativePageCount_,
                    pageGlyphs);
    }
    return;
  }

  if (narrativePhase_ == NarrativePhase::FadingText) {
    const float progress = smootherStep(
        (nowMs - narrativePhaseStartedMs_) /
        static_cast<float>(kNarrativeTextFadeMs));
    const uint16_t eraseWidth = static_cast<uint16_t>(
        lroundf(kNarrativeTextWidth * progress));
    if (eraseWidth <= narrativeFadeErasedWidth_) return;

    const uint16_t firstLine = narrativePageIndex_ * kNarrativeLinesPerPage;
    const uint16_t lastLine = std::min<uint16_t>(
        narrativeLineCount_, firstLine + kNarrativeLinesPerPage);
    const uint16_t pageLineCount = lastLine - firstLine;
    const int firstY = M5.Display.height() / 2 -
                       static_cast<int>(pageLineCount - 1) *
                           kNarrativeLineHeight / 2;
    const int eraseX = kNarrativeTextLeft + narrativeFadeErasedWidth_;
    const int eraseDelta = eraseWidth - narrativeFadeErasedWidth_;
    for (uint16_t lineOffset = 0; lineOffset < pageLineCount; ++lineOffset) {
      const int lineY = firstY + lineOffset * kNarrativeLineHeight;
      M5.Display.fillRect(eraseX, lineY - 17, eraseDelta + 1, 35,
                          kBackground);
    }
    narrativeFadeErasedWidth_ = eraseWidth;
  }
}

void AvatarEngine::beginEnergyDismiss(uint32_t nowMs) {
  energyDismissStartedMs_ = nowMs;
  eyeMessageEndsAtMs_ = nowMs + 180;
  forceRender_ = true;
}

void AvatarEngine::clearEnergyUi() {
  energyUiEnabled_ = false;
  energyCharging_ = false;
  energyMoodEnabled_ = true;
  eyeMessageVertical_ = false;
  leftEyeMessage_ = "";
  rightEyeMessage_ = "";
  eyeMessageStartedMs_ = 0;
  eyeMessageEndsAtMs_ = 0;
  energyDismissStartedMs_ = 0;
  forceRender_ = true;
}

void AvatarEngine::scheduleNextBlink(uint32_t nowMs, bool useInitialDelay) {
  const BlinkSettings& blink = spec(targetExpression_).blink;
  blinkStartedMs_ = 0;
  if (!blink.enabled) {
    nextBlinkAtMs_ = 0;
    return;
  }

  uint32_t delayMs = blink.initialDelayMs;
  if (!useInitialDelay) {
    delayMs = blink.minIntervalMs;
    if (blink.maxIntervalMs > blink.minIntervalMs) {
      delayMs += random(0, blink.maxIntervalMs - blink.minIntervalMs + 1);
    }
  }
  nextBlinkAtMs_ = nowMs + delayMs;
}

float AvatarEngine::blinkScale(uint32_t nowMs) {
  const BlinkSettings& blink = spec(targetExpression_).blink;
  if (!blink.enabled) return 1.0f;

  if (blinkStartedMs_ == 0 && reached(nowMs, nextBlinkAtMs_)) {
    blinkStartedMs_ = nowMs;
  }
  if (blinkStartedMs_ == 0) return 1.0f;

  const uint32_t elapsed = nowMs - blinkStartedMs_;
  const uint32_t durationMs = blink.closeMs + blink.openMs;
  if (durationMs == 0 || elapsed >= durationMs) {
    scheduleNextBlink(nowMs, false);
    return 1.0f;
  }
  constexpr float kClosedScale = 0.08f;
  if (elapsed < blink.closeMs) {
    const float progress = smootherStep(
        elapsed / static_cast<float>(std::max<uint16_t>(1, blink.closeMs)));
    return 1.0f + (kClosedScale - 1.0f) * progress;
  }
  const float progress = smootherStep(
      (elapsed - blink.closeMs) /
      static_cast<float>(std::max<uint16_t>(1, blink.openMs)));
  return kClosedScale + (1.0f - kClosedScale) * progress;
}

void AvatarEngine::drawEye(const EyePose& eye, float centerX, float centerY,
                           float blink) {
  const float eyeX = centerX + eye.x;
  const float eyeY = centerY + eye.y;
  const int width = std::max(4, static_cast<int>(lroundf(eye.width)));
  const int height =
      std::max(4, static_cast<int>(lroundf(eye.height * blink)));
  const int left = lroundf(eyeX - width * 0.5f);
  const int top = lroundf(eyeY - height * 0.5f);
  const int radius = std::max(
      2, static_cast<int>(lroundf(std::min(width, height) * 0.5f *
                                  clamp01(eye.roundness))));

  const bool canRotate = fabsf(eye.angle) > 0.25f &&
                         eye.upperLid < 0.01f && eye.lowerLid < 0.01f &&
                         eye.roundness > 0.92f;
  if (canRotate) {
    const float angle = eye.angle * kPi / 180.0f;
    const bool vertical = height >= width;
    const float major = vertical ? height : width;
    const float minor = vertical ? width : height;
    const float lineLength = std::max(0.0f, major - minor);
    const float axisX = vertical ? -sinf(angle) : cosf(angle);
    const float axisY = vertical ? cosf(angle) : sinf(angle);
    if (lineLength < 1.0f) {
      const int circleRadius =
          std::max(2, static_cast<int>(lroundf(minor * 0.5f)));
      M5.Display.fillCircle(lroundf(eyeX), lroundf(eyeY), circleRadius + 1,
                            kEyeEdgeColor);
      M5.Display.fillCircle(lroundf(eyeX), lroundf(eyeY), circleRadius,
                            kEyeColor);
    } else {
      const float halfLine = lineLength * 0.5f;
      const float radius = std::max(2.0f, minor * 0.5f);
      const float startX = eyeX - axisX * halfLine;
      const float startY = eyeY - axisY * halfLine;
      const float endX = eyeX + axisX * halfLine;
      const float endY = eyeY + axisY * halfLine;
      const auto fillCapsule = [&](float capsuleRadius, uint16_t color) {
        const float perpendicularX = -axisY * capsuleRadius;
        const float perpendicularY = axisX * capsuleRadius;
        const int16_t startLeftX = lroundf(startX + perpendicularX);
        const int16_t startLeftY = lroundf(startY + perpendicularY);
        const int16_t startRightX = lroundf(startX - perpendicularX);
        const int16_t startRightY = lroundf(startY - perpendicularY);
        const int16_t endLeftX = lroundf(endX + perpendicularX);
        const int16_t endLeftY = lroundf(endY + perpendicularY);
        const int16_t endRightX = lroundf(endX - perpendicularX);
        const int16_t endRightY = lroundf(endY - perpendicularY);
        M5.Display.fillTriangle(startLeftX, startLeftY, startRightX,
                                startRightY, endLeftX, endLeftY, color);
        M5.Display.fillTriangle(startRightX, startRightY, endRightX,
                                endRightY, endLeftX, endLeftY, color);
        const int capRadius =
            std::max(2, static_cast<int>(lroundf(capsuleRadius)));
        M5.Display.fillCircle(lroundf(startX), lroundf(startY), capRadius,
                              color);
        M5.Display.fillCircle(lroundf(endX), lroundf(endY), capRadius, color);
      };
      fillCapsule(radius + 1.2f, kEyeEdgeColor);
      fillCapsule(radius, kEyeColor);
    }
  } else {
    M5.Display.fillRoundRect(left - 1, top - 1, width + 2, height + 2,
                             radius + 1, kEyeEdgeColor);
    M5.Display.fillRoundRect(left, top, width, height, radius, kEyeColor);
  }

  const int upperCover = lroundf(clamp01(eye.upperLid) * height);
  if (upperCover > 0) {
    M5.Display.fillRect(left - 1, top - 1, width + 2, upperCover + 1,
                        kBackground);
  }
  const int upperTilt =
      lroundf(fabsf(eye.upperLidTilt) * height * 0.34f);
  if (upperTilt > 0) {
    const int lidY = top + upperCover;
    if (eye.upperLidTilt > 0.0f) {
      M5.Display.fillTriangle(left, lidY, left + width, lidY,
                              left + width, lidY + upperTilt, kBackground);
    } else {
      M5.Display.fillTriangle(left, lidY + upperTilt, left, lidY,
                              left + width, lidY, kBackground);
    }
  }

  const int lowerCover = lroundf(clamp01(eye.lowerLid) * height);
  if (lowerCover > 0) {
    M5.Display.fillRect(left - 1, top + height - lowerCover, width + 2,
                        lowerCover + 1, kBackground);
  }

  const float browOpacity = clamp01(eye.browOpacity);
  if (browOpacity > 0.01f) {
    const float browLength = eye.width * 0.88f;
    const float radians = eye.browTilt * kPi / 180.0f;
    const float dx = cosf(radians) * browLength * 0.5f;
    const float dy = sinf(radians) * browLength * 0.5f;
    const float browCenterY = eyeY + eye.browY;
    const float browRadius =
        std::max(2.0f, std::min(eye.width, eye.height) * 0.04f);
    const uint8_t brightness =
        static_cast<uint8_t>(160.0f + 95.0f * browOpacity);
    const uint16_t browColor =
        M5.Display.color565(brightness, brightness, brightness);
    const float perpendicularX = -sinf(radians) * browRadius;
    const float perpendicularY = cosf(radians) * browRadius;
    const float startX = eyeX - dx;
    const float startY = browCenterY - dy;
    const float endX = eyeX + dx;
    const float endY = browCenterY + dy;
    const int16_t startLeftX = lroundf(startX + perpendicularX);
    const int16_t startLeftY = lroundf(startY + perpendicularY);
    const int16_t startRightX = lroundf(startX - perpendicularX);
    const int16_t startRightY = lroundf(startY - perpendicularY);
    const int16_t endLeftX = lroundf(endX + perpendicularX);
    const int16_t endLeftY = lroundf(endY + perpendicularY);
    const int16_t endRightX = lroundf(endX - perpendicularX);
    const int16_t endRightY = lroundf(endY - perpendicularY);
    M5.Display.fillTriangle(startLeftX, startLeftY, startRightX,
                            startRightY, endLeftX, endLeftY, browColor);
    M5.Display.fillTriangle(startRightX, startRightY, endRightX, endRightY,
                            endLeftX, endLeftY, browColor);
    const int browCapRadius =
        std::max(2, static_cast<int>(lroundf(browRadius)));
    M5.Display.fillCircle(lroundf(startX), lroundf(startY), browCapRadius,
                          browColor);
    M5.Display.fillCircle(lroundf(endX), lroundf(endY), browCapRadius,
                          browColor);
  }
}

void AvatarEngine::drawEyeMessage(const EyePose& eye, float centerX,
                                  float centerY, float blink,
                                  const String& text, float opacity,
                                  bool vertical) {
  if (text.isEmpty() || opacity < 0.01f || blink < 0.38f) return;

  const float eyeX = centerX + eye.x;
  const float eyeY = centerY + eye.y;
  const int height =
      std::max(4, static_cast<int>(lroundf(eye.height * blink)));
  const int top = lroundf(eyeY - height * 0.5f);
  const int upperCover = lroundf(clamp01(eye.upperLid) * height);
  const int upperTilt =
      lroundf(fabsf(eye.upperLidTilt) * height * 0.34f);
  const int lowerCover = lroundf(clamp01(eye.lowerLid) * height);
  const int visibleTop = top + upperCover + upperTilt / 2;
  const int visibleBottom = top + height - lowerCover;
  if (visibleBottom - visibleTop < 24) return;

  const uint8_t coreLuminance = static_cast<uint8_t>(
      lroundf(255.0f * (1.0f - clamp01(opacity))));
  const int textX = lroundf(eyeX);
  const int textY = (visibleTop + visibleBottom) / 2;
  M5.Display.setFont(&fonts::efontCN_24_b);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(
      M5.Display.color565(coreLuminance, coreLuminance, coreLuminance));
  if (!vertical) {
    M5.Display.drawString(text, textX, textY);
  } else {
    constexpr uint8_t kMaxVerticalGlyphs = 4;
    constexpr int kVerticalGlyphAdvance = 28;
    String glyphs[kMaxVerticalGlyphs];
    uint8_t glyphCount = 0;
    for (size_t byteIndex = 0;
         byteIndex < text.length() && glyphCount < kMaxVerticalGlyphs;) {
      const uint8_t lead = static_cast<uint8_t>(text[byteIndex]);
      size_t glyphBytes = 1;
      if ((lead & 0xE0) == 0xC0) {
        glyphBytes = 2;
      } else if ((lead & 0xF0) == 0xE0) {
        glyphBytes = 3;
      } else if ((lead & 0xF8) == 0xF0) {
        glyphBytes = 4;
      }
      glyphBytes = std::min(glyphBytes, text.length() - byteIndex);
      glyphs[glyphCount++] =
          text.substring(byteIndex, byteIndex + glyphBytes);
      byteIndex += glyphBytes;
    }

    const int firstY =
        textY - static_cast<int>(glyphCount - 1) * kVerticalGlyphAdvance / 2;
    for (uint8_t index = 0; index < glyphCount; ++index) {
      M5.Display.drawString(glyphs[index], textX,
                            firstY + index * kVerticalGlyphAdvance);
    }
  }
  M5.Display.setTextDatum(top_left);
}

AvatarEngine::DirtyRect AvatarEngine::mergeRects(const DirtyRect& first,
                                                  const DirtyRect& second) {
  if (!first.valid) return second;
  if (!second.valid) return first;
  const int left = std::min<int>(first.x, second.x);
  const int top = std::min<int>(first.y, second.y);
  const int right = std::max<int>(first.x + first.width,
                                  second.x + second.width);
  const int bottom = std::max<int>(first.y + first.height,
                                   second.y + second.height);
  return {static_cast<int16_t>(left), static_cast<int16_t>(top),
          static_cast<int16_t>(right - left),
          static_cast<int16_t>(bottom - top), true};
}

AvatarEngine::DirtyRect AvatarEngine::eyeBounds(const EyePose& eye,
                                                 float centerX, float centerY,
                                                 float blink) const {
  const float eyeX = centerX + eye.x;
  const float eyeY = centerY + eye.y;
  const float radians = eye.angle * kPi / 180.0f;
  const float baseHalfWidth = eye.width * 0.5f;
  const float baseHalfHeight = eye.height * blink * 0.5f;
  float halfWidth = fabsf(cosf(radians)) * baseHalfWidth +
                    fabsf(sinf(radians)) * baseHalfHeight;
  float halfHeight = fabsf(sinf(radians)) * baseHalfWidth +
                     fabsf(cosf(radians)) * baseHalfHeight;
  float topExtent = halfHeight;

  if (eye.browOpacity > 0.01f) {
    const float browRadians = eye.browTilt * kPi / 180.0f;
    const float halfBrowWidth = fabsf(cosf(browRadians)) * eye.width * 0.44f;
    const float halfBrowHeight = fabsf(sinf(browRadians)) * eye.width * 0.44f +
                                 std::min(eye.width, eye.height) * 0.04f;
    halfWidth = std::max(halfWidth, halfBrowWidth);
    topExtent = std::max(topExtent, -eye.browY + halfBrowHeight);
  }

  constexpr int kPadding = 7;
  const int left = std::max(0, static_cast<int>(floorf(eyeX - halfWidth)) - kPadding);
  const int top = std::max(0, static_cast<int>(floorf(eyeY - topExtent)) - kPadding);
  const int right = std::min(M5.Display.width(),
                             static_cast<int>(ceilf(eyeX + halfWidth)) + kPadding);
  const int bottom = std::min(M5.Display.height(),
                              static_cast<int>(ceilf(eyeY + halfHeight)) + kPadding);
  return {static_cast<int16_t>(left), static_cast<int16_t>(top),
          static_cast<int16_t>(std::max(0, right - left)),
          static_cast<int16_t>(std::max(0, bottom - top)), true};
}

void AvatarEngine::clearDirtyRect(const DirtyRect& rect) {
  if (!rect.valid || rect.width <= 0 || rect.height <= 0) return;
  M5.Display.fillRect(rect.x, rect.y, rect.width, rect.height, kBackground);
}

bool AvatarEngine::waitForVSync(uint32_t nowMs) {
  if (vsyncRetryAtMs_ != 0 && !reached(nowMs, vsyncRetryAtMs_)) return false;

  const uint32_t initialEdge = gTearingEffectEdges;
  const uint32_t waitStartedUs = micros();
  while (gTearingEffectEdges == initialEdge) {
    if (micros() - waitStartedUs >= kVsyncTimeoutUs) {
      ++vsyncTimeoutCount_;
      if (++consecutiveVsyncTimeouts_ >= 2) {
        // A disconnected or sleeping TE line must never stall animation.
        vsyncRetryAtMs_ = nowMs + 1000;
        consecutiveVsyncTimeouts_ = 0;
      }
      return false;
    }
    delayMicroseconds(20);
  }

  const uint32_t waitedUs = micros() - waitStartedUs;
  ++vsyncWaitCount_;
  totalVsyncWaitUs_ += waitedUs;
  maximumVsyncWaitUs_ = std::max(maximumVsyncWaitUs_, waitedUs);
  consecutiveVsyncTimeouts_ = 0;
  vsyncRetryAtMs_ = 0;
  return true;
}

void AvatarEngine::render(uint32_t nowMs) {
  const uint32_t renderStartedUs = micros();
  updateInteraction(nowMs);
  updateNarrativeText(nowMs);
  updateModeMenu(nowMs);
  const bool modeMenuSurfaceVisible =
      modeMenuActive_ &&
      (modeMenuPhase_ == ModeMenuPhase::FadingIn ||
       modeMenuPhase_ == ModeMenuPhase::Visible ||
       modeMenuPhase_ == ModeMenuPhase::FadingOut);
  if (modeMenuSurfaceVisible) {
    if (modeMenuPhase_ == ModeMenuPhase::Visible &&
        modeMenuCanvasPrepared_) {
      previousRenderStartedUs_ = 0;
      return;
    }
    waitForVSync(nowMs);
    M5.Display.startWrite();
    drawModeMenu(nowMs);
    M5.Display.endWrite();
    modeMenuCanvasPrepared_ = true;
    previousLeftBounds_.valid = false;
    previousRightBounds_.valid = false;
    recordRenderMetrics(nowMs, renderStartedUs, micros());
    return;
  }
  const int width = M5.Display.width();
  const int height = M5.Display.height();
  const float designScale = std::min(width, height) / kDesignSize;
  const float time = nowMs / 1000.0f;
  const ExpressionSpec& expression = spec(targetExpression_);
  const MotionSample lifeMotion = sampleMotion(kIdleMotion, time);
  const MotionSample emotionMotion = sampleMotion(expression.motion, time);

  float emotionBlend = 0.0f;
  if (targetExpression_ != ExpressionId::Idle) {
    emotionBlend = smootherStep(
        (nowMs - expressionStartedMs_) /
        static_cast<float>(std::max<uint16_t>(1, expression.motion.blendInMs)));
    if (returnToBase_ && expressionEndsAtMs_ != 0) {
      const int32_t remainingMs =
          static_cast<int32_t>(expressionEndsAtMs_ - nowMs);
      emotionBlend *= smootherStep(
          remainingMs /
          static_cast<float>(std::max<uint16_t>(1,
                                                expression.motion.blendOutMs)));
    }
  }

  const MotionSample motion =
      mixMotion(lifeMotion, emotionMotion, emotionBlend);
  Pose renderedPose = currentPose_;
  if (swipePreviewDirection_ != 0 && swipePreviewAmount_ > 0.005f) {
    const Pose& previewPose =
        spec(adjacentExpression(swipePreviewDirection_)).keyframes[0].pose;
    renderedPose = interpolate(renderedPose, previewPose,
                               swipePreviewAmount_, Easing::Smooth);
  }
  if (energyUiEnabled_) {
    float visibleEnergy = energyLevel_;
    if (energyCharging_) {
      const float chargePhase = fmodf(nowMs / 1800.0f, 1.0f);
      const float chargePulse = sinf(chargePhase * kPi);
      // Keep the right-eye gauge locked to the measured percentage. Only the
      // message-bearing left eye breathes to indicate incoming power.
      renderedPose.leftEye.width *= 1.0f + chargePulse * 0.018f;
      renderedPose.leftEye.height *= 1.0f + chargePulse * 0.018f;
    }

    // The right eye is the gauge: its remaining white opening directly shows
    // battery, brightness or sound level. Battery mode additionally makes the
    // whole head heavier as energy runs down.
    const float fatigue = 1.0f - clamp01(visibleEnergy);
    const float energyLid = fatigue * 0.88f;
    renderedPose.rightEye.upperLid =
        std::max(renderedPose.rightEye.upperLid, energyLid);
    if (energyMoodEnabled_) {
      renderedPose.faceY += fatigue * 26.0f;
      renderedPose.headPitch += fatigue * 5.0f;
    }

    if (energyDismissStartedMs_ != 0) {
      const uint32_t elapsed = nowMs - energyDismissStartedMs_;
      if (elapsed < 900) {
        if (elapsed < 140) {
          // A small anticipatory lean gives the gesture a readable wind-up.
          const float windUp = smootherStep(elapsed / 140.0f);
          renderedPose.faceX -= windUp * 11.0f;
          renderedPose.faceY += windUp * 4.0f;
          renderedPose.headYaw -= windUp * 6.0f;
          renderedPose.headRoll -= windUp * 4.0f;
          renderedPose.faceScale *= 1.0f - windUp * 0.018f;
        } else if (elapsed < 690) {
          // Two decisive side-to-side beats, with a small upward bounce.
          const float progress = (elapsed - 140) / 550.0f;
          const float envelope = sinf(progress * kPi);
          const float shake = sinf(progress * kPi * 4.0f) * envelope;
          renderedPose.faceX += shake * 38.0f;
          renderedPose.faceY -= sinf(progress * kPi) * 9.0f;
          renderedPose.headYaw += shake * 19.0f;
          renderedPose.headRoll += shake * 12.0f;
          renderedPose.faceScale *=
              1.0f + sinf(progress * kPi) * 0.035f;
        } else {
          // A damped overshoot prevents the face from stopping mechanically.
          const float settle = (elapsed - 690) / 210.0f;
          const float overshoot =
              sinf(settle * kPi * 2.0f) * (1.0f - settle);
          renderedPose.faceX += overshoot * 15.0f;
          renderedPose.faceY -= sinf(settle * kPi) * 4.0f;
          renderedPose.headYaw += overshoot * 8.0f;
          renderedPose.headRoll += overshoot * 6.0f;
        }
      }
    }
  }
  const float swipeTravel = std::min(
      1.0f, (fabsf(swipeOffsetX_) + fabsf(swipeOffsetY_) * 0.6f) / 110.0f);
  const float scale = renderedPose.faceScale * motion.scale *
                      (1.0f - swipeTravel * 0.025f) *
                      (1.0f - shakeIntensity_ * 0.018f);
  AttentionPose attention = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0};
  if (targetExpression_ == ExpressionId::Idle) {
    attention = sampleIdleAttention(nowMs);
    attention.eyeX += microSaccade(nowMs, 17.29f) * 3.8f;
    attention.eyeY += microSaccade(nowMs, 31.73f) * 2.3f;
  }

  const float gazeX = motion.eyeX + interactionX_ + shakeOffsetX_ +
                      attention.eyeX * attentionWeight_;
  const float gazeY = motion.eyeY + interactionY_ + shakeOffsetY_ +
                      attention.eyeY * attentionWeight_;
  renderedPose.leftEye.x += gazeX;
  renderedPose.leftEye.y += gazeY;
  renderedPose.rightEye.x += gazeX;
  renderedPose.rightEye.y += gazeY;

  const float headYaw = renderedPose.headYaw + headYaw_ +
                        attention.yaw * attentionWeight_ +
                        shakeOffsetX_ / kShakeTravelX * 8.0f;
  const float headPitch = renderedPose.headPitch + headPitch_ +
                          attention.pitch * attentionWeight_ +
                          swipeOffsetY_ / 110.0f * 6.0f +
                          shakeOffsetY_ / kShakeTravelY * 6.0f;
  const float headRoll = renderedPose.headRoll + headRoll_ +
                         attention.roll * attentionWeight_ -
                         swipeOffsetX_ / 120.0f * 7.0f -
                         shakeOffsetX_ / kShakeTravelX * 10.0f;
  projectEyeOntoHead(renderedPose.leftEye, -1.0f, headYaw, headPitch,
                     headRoll);
  projectEyeOntoHead(renderedPose.rightEye, 1.0f, headYaw, headPitch,
                     headRoll);

  const float eyeScale = scale * designScale;
  const auto scaleEye = [eyeScale](EyePose& eye) {
    eye.x *= eyeScale;
    eye.y *= eyeScale;
    eye.width *= eyeScale;
    eye.height *= eyeScale;
    eye.browY *= eyeScale;
  };
  scaleEye(renderedPose.leftEye);
  scaleEye(renderedPose.rightEye);

  const int centerX =
      width / 2 +
      lroundf((renderedPose.faceX + motion.faceX) * designScale +
              swipeOffsetX_ + shakeOffsetX_ * designScale * 0.30f);
  const int centerY =
      height / 2 +
      lroundf((renderedPose.faceY + motion.faceY) * designScale +
              swipeOffsetY_ + shakeOffsetY_ * designScale * 0.30f);

  float blink = blinkScale(nowMs);
  if (energyDismissStartedMs_ != 0) {
    const uint32_t elapsed = nowMs - energyDismissStartedMs_;
    constexpr float kDismissClosedScale = 0.06f;
    if (elapsed >= 90 && elapsed < 245) {
      const float progress = smootherStep((elapsed - 90) / 155.0f);
      blink = std::min(
          blink, 1.0f + (kDismissClosedScale - 1.0f) * progress);
    } else if (elapsed >= 245 && elapsed < 410) {
      const float progress = smootherStep((elapsed - 245) / 165.0f);
      blink = std::min(
          blink, kDismissClosedScale + (1.0f - kDismissClosedScale) * progress);
    } else if (elapsed >= 610 && elapsed < 690) {
      const float progress = smootherStep((elapsed - 610) / 80.0f);
      blink = std::min(blink, 1.0f + (0.24f - 1.0f) * progress);
    } else if (elapsed >= 690 && elapsed < 790) {
      const float progress = smootherStep((elapsed - 690) / 100.0f);
      blink = std::min(blink, 0.24f + (1.0f - 0.24f) * progress);
    }
  }

  if (narrativeActive_ &&
      (narrativePhase_ == NarrativePhase::ClosingEyes ||
       narrativePhase_ == NarrativePhase::OpeningEyes)) {
    const uint32_t elapsed = nowMs - narrativePhaseStartedMs_;
    const bool closing = narrativePhase_ == NarrativePhase::ClosingEyes;
    const float progress = smootherStep(
        elapsed / static_cast<float>(closing ? kNarrativeEyeCloseMs
                                             : kNarrativeEyeOpenMs));
    constexpr float kNarrativeClosedScale = 0.035f;
    const float eyeVisibility =
        closing ? 1.0f - progress : progress;
    const float narrativeBlink =
        kNarrativeClosedScale +
        (1.0f - kNarrativeClosedScale) * eyeVisibility;
    blink = std::min(blink, narrativeBlink);
    renderedPose.leftEye.browOpacity *= eyeVisibility;
    renderedPose.rightEye.browOpacity *= eyeVisibility;
  }
  if (modeMenuActive_ &&
      (modeMenuPhase_ == ModeMenuPhase::ClosingEyes ||
       modeMenuPhase_ == ModeMenuPhase::OpeningEyes)) {
    const uint32_t elapsed = nowMs - modeMenuPhaseStartedMs_;
    const bool closing = modeMenuPhase_ == ModeMenuPhase::ClosingEyes;
    const float progress = smootherStep(
        elapsed / static_cast<float>(closing ? kNarrativeEyeCloseMs
                                             : kNarrativeEyeOpenMs));
    constexpr float kModeMenuClosedScale = 0.035f;
    const float eyeVisibility = closing ? 1.0f - progress : progress;
    const float menuBlink =
        kModeMenuClosedScale +
        (1.0f - kModeMenuClosedScale) * eyeVisibility;
    blink = std::min(blink, menuBlink);
    renderedPose.leftEye.browOpacity *= eyeVisibility;
    renderedPose.rightEye.browOpacity *= eyeVisibility;
  }

  float eyeMessageOpacity = 0.0f;
  float eyeMessageOffsetY = 0.0f;
  if (energyUiEnabled_ && eyeMessageStartedMs_ != 0 &&
      !reached(nowMs, eyeMessageEndsAtMs_)) {
    const uint32_t messageElapsedMs = nowMs - eyeMessageStartedMs_;
    const float enter = smootherStep(
        messageElapsedMs / static_cast<float>(kEyeMessageFadeInMs));
    const uint32_t remaining = eyeMessageEndsAtMs_ - nowMs;
    const float exit =
        remaining < kEyeMessageFadeOutMs
            ? smootherStep(remaining /
                           static_cast<float>(kEyeMessageFadeOutMs))
            : 1.0f;
    const float breathPhase =
        messageElapsedMs * (2.0f * kPi / kEyeMessageBreathPeriodMs) -
        kPi * 0.5f;
    const float breath =
        1.0f - kEyeMessageBreathDepth * (0.5f + 0.5f * sinf(breathPhase));
    eyeMessageOpacity = std::min(enter, exit) * breath;
    eyeMessageOffsetY = (1.0f - enter) * 2.5f - (1.0f - exit) * 1.5f +
                        sinf(breathPhase) * kEyeMessageFloatPixels;
  }
  const bool narrativeTextVisible =
      narrativeActive_ &&
      (narrativePhase_ == NarrativePhase::Typing ||
       narrativePhase_ == NarrativePhase::Holding ||
       narrativePhase_ == NarrativePhase::FadingText);
  if (narrativeTextVisible) {
    waitForVSync(nowMs);
    M5.Display.startWrite();
    drawNarrativePage(nowMs);
    M5.Display.endWrite();
    previousLeftBounds_.valid = false;
    previousRightBounds_.valid = false;
    recordRenderMetrics(nowMs, renderStartedUs, micros());
    return;
  }

  DirtyRect leftBounds =
      eyeBounds(renderedPose.leftEye, centerX, centerY, blink);
  DirtyRect rightBounds =
      eyeBounds(renderedPose.rightEye, centerX, centerY, blink);
  waitForVSync(nowMs);
  M5.Display.startWrite();
  if (requiresFullClear_) {
    M5.Display.fillScreen(kBackground);
    requiresFullClear_ = false;
  } else {
    clearDirtyRect(mergeRects(previousLeftBounds_, leftBounds));
    clearDirtyRect(mergeRects(previousRightBounds_, rightBounds));
  }
  drawEye(renderedPose.leftEye, centerX, centerY, blink);
  drawEye(renderedPose.rightEye, centerX, centerY, blink);
  drawEyeMessage(renderedPose.leftEye, centerX, centerY + eyeMessageOffsetY,
                  blink,
                  leftEyeMessage_, eyeMessageOpacity, eyeMessageVertical_);
  drawEyeMessage(renderedPose.rightEye, centerX, centerY + eyeMessageOffsetY,
                  blink,
                  rightEyeMessage_, eyeMessageOpacity, eyeMessageVertical_);
  M5.Display.endWrite();

  previousLeftBounds_ = leftBounds;
  previousRightBounds_ = rightBounds;
  recordRenderMetrics(nowMs, renderStartedUs, micros());
}

void AvatarEngine::recordRenderMetrics(uint32_t nowMs,
                                       uint32_t renderStartedUs,
                                       uint32_t renderFinishedUs) {
  const uint32_t renderTimeUs = renderFinishedUs - renderStartedUs;
  totalRenderTimeUs_ += renderTimeUs;
  maximumRenderTimeUs_ = std::max(maximumRenderTimeUs_, renderTimeUs);
  ++metricsFrameCount_;

  if (previousRenderStartedUs_ != 0) {
    const uint32_t intervalUs = renderStartedUs - previousRenderStartedUs_;
    totalFrameIntervalUs_ += intervalUs;
    maximumFrameIntervalUs_ = std::max(maximumFrameIntervalUs_, intervalUs);
  }
  previousRenderStartedUs_ = renderStartedUs;

  const uint32_t windowMs = nowMs - metricsStartedMs_;
  if (windowMs < kMetricsReportIntervalMs || metricsFrameCount_ == 0) return;

  const float fps = metricsFrameCount_ * 1000.0f / windowMs;
  const float averageRenderMs =
      totalRenderTimeUs_ / (metricsFrameCount_ * 1000.0f);
  const uint32_t intervalCount = metricsFrameCount_ > 1 ? metricsFrameCount_ - 1 : 1;
  const float averageIntervalMs =
      totalFrameIntervalUs_ / (intervalCount * 1000.0f);
  const uint32_t vsyncAttempts = vsyncWaitCount_ + vsyncTimeoutCount_;
  const float vsyncLockRate =
      vsyncAttempts == 0 ? 0.0f : vsyncWaitCount_ * 100.0f / vsyncAttempts;
  const float averageVsyncWaitMs =
      vsyncWaitCount_ == 0
          ? 0.0f
          : totalVsyncWaitUs_ / (vsyncWaitCount_ * 1000.0f);
  Serial.printf(
      "[avatar perf] fps=%.1f render_avg=%.2fms render_max=%.2fms "
      "frame_avg=%.2fms frame_max=%.2fms vsync=%.1f%% "
      "wait_avg=%.2fms wait_max=%.2fms timeouts=%lu\n",
      fps, averageRenderMs, maximumRenderTimeUs_ / 1000.0f,
      averageIntervalMs, maximumFrameIntervalUs_ / 1000.0f, vsyncLockRate,
      averageVsyncWaitMs, maximumVsyncWaitUs_ / 1000.0f,
      static_cast<unsigned long>(vsyncTimeoutCount_));

  metricsStartedMs_ = nowMs;
  metricsFrameCount_ = 0;
  totalRenderTimeUs_ = 0;
  maximumRenderTimeUs_ = 0;
  totalFrameIntervalUs_ = 0;
  maximumFrameIntervalUs_ = 0;
  previousRenderStartedUs_ = 0;
  vsyncWaitCount_ = 0;
  vsyncTimeoutCount_ = 0;
  totalVsyncWaitUs_ = 0;
  maximumVsyncWaitUs_ = 0;
}

void AvatarEngine::update(uint32_t nowMs) {
  if (!ready_) return;

  const uint32_t elapsed = nowMs - transitionStartedMs_;
  if (transitionDurationMs_ > 0 && elapsed < transitionDurationMs_) {
    currentPose_ = interpolate(fromPose_, targetPose_,
                               elapsed / static_cast<float>(transitionDurationMs_),
                               transitionEasing_);
  } else {
    currentPose_ = targetPose_;
  }

  advanceTimeline(nowMs);

  const uint32_t nowUs = micros();
  const bool frameDue =
      nextFrameUs_ == 0 || static_cast<int32_t>(nowUs - nextFrameUs_) >= 0;
  if (!forceRender_ && !frameDue) return;

  if (forceRender_ || nextFrameUs_ == 0) {
    nextFrameUs_ = nowUs + kFrameIntervalUs;
  } else {
    nextFrameUs_ += kFrameIntervalUs;
    if (static_cast<int32_t>(nowUs - nextFrameUs_) >=
        static_cast<int32_t>(kFrameIntervalUs * 2)) {
      nextFrameUs_ = nowUs + kFrameIntervalUs;
    }
  }
  forceRender_ = false;
  render(nowMs);
}
