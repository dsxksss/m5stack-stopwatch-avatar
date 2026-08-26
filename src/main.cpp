// SPDX-License-Identifier: AGPL-3.0-or-later

#include <Arduino.h>
#include <M5IOE1.h>
#include <M5Unified.h>
#include <Preferences.h>

#include "avatar_engine.h"
#include "ui_sound.h"
#include "wifi_pairing.h"

namespace {

constexpr uint8_t kIoeAddress = 0x4F;
constexpr uint8_t kVibrationPwmRegister = 0x1B;
constexpr uint32_t kIoeBusFrequency = M5IOE1_I2C_FREQ_100K;
constexpr uint32_t kDiagnosticRefreshIntervalMs = 100;
constexpr uint32_t kImuInteractionIntervalMs = 20;
constexpr uint16_t kImuCalibrationSamples = 30;
constexpr int16_t kGestureDirectionLockPx = 12;
constexpr int16_t kGestureCommitPx = 52;
constexpr uint16_t kSwipeTransitionMs = 160;
constexpr char kPreferencesNamespace[] = "kk-avatar";
constexpr uint8_t kSettingsSchemaVersion = 4;
constexpr uint8_t kDefaultBrightness = 150;
constexpr uint8_t kDefaultSoundVolume = 56;
constexpr uint8_t kSoundVolumeLevels[] = {0, 32, 56, 84, 112};
constexpr uint8_t kDimBrightness = 24;
constexpr uint32_t kDefaultDimAfterMs = 45UL * 1000UL;
constexpr uint32_t kDefaultScreenOffAfterMs = 60UL * 1000UL;
constexpr uint32_t kStatusMessageHoldMs = 3400;
constexpr uint32_t kStatusDismissAnimationMs = 900;
constexpr uint32_t kWifiPairingHoldMs = 1800;
constexpr uint32_t kEyeMenuClickWindowMs = 420;
constexpr uint32_t kPowerSampleIntervalMs = 5000;
constexpr uint32_t kRtcSampleIntervalMs = 30000;
constexpr uint32_t kDimRenderIntervalMs = 50;
constexpr uint32_t kMotionWakeGraceMs = 1500;
constexpr float kMotionWakeJerkThreshold = 0.30f;
constexpr uint8_t kMotionWakeRequiredSamples = 2;
constexpr uint32_t kLowBatteryReminderIntervalMs = 15UL * 60UL * 1000UL;
constexpr uint8_t kLowBatteryThreshold = 15;

enum class GestureAxis : uint8_t { None, Horizontal, Vertical };
enum class ScreenPowerState : uint8_t { Bright, Dimmed, Sleeping };
enum class EyeMenuPage : uint8_t {
  Root,
  Brightness,
  Sound,
  Wifi,
};

struct CompanionSettings {
  uint8_t brightness = kDefaultBrightness;
  uint8_t soundVolume = kDefaultSoundVolume;
  String wifiSsid;
  String wifiPassword;
  uint32_t dimAfterMs = kDefaultDimAfterMs;
  uint32_t screenOffAfterMs = kDefaultScreenOffAfterMs;
  uint8_t quietStartHour = 22;
  uint8_t quietEndHour = 7;
};

M5IOE1 ioe;
AvatarEngine avatar;
UiSoundEngine uiSounds;
WifiPairing wifiPairing;
Preferences preferences;
CompanionSettings settings;
bool vibrationReady = false;
bool diagnosticMode = false;
bool diagnosticToggleLatched = false;
bool statusMode = false;
bool wifiMode = false;
bool eyeMenuMode = false;
bool wifiPairingInputArmed = false;
bool wifiPairingHoldLatched = false;
bool eyeMenuInputArmed = false;
bool eyeMenuWifiHoldLatched = false;
bool rtcValid = false;
bool chargingStateKnown = false;
bool charging = false;
bool chargeReadingValid = false;
bool nightRestActive = false;
uint32_t vibrationStopsAtMs = 0;
uint32_t lastDiagnosticRefreshMs = 0;
uint32_t lastImuInteractionMs = 0;
uint32_t lastActivityMs = 0;
uint32_t statusDismissesAtMs = 0;
uint32_t statusReactionStartsAtMs = 0;
uint32_t eyeMenuAClickDeadlineMs = 0;
uint32_t eyeMenuPageReadyAtMs = 0;
uint32_t lastPowerSampleMs = 0;
uint32_t lastRtcSampleMs = 0;
uint32_t lastDimRenderMs = 0;
uint32_t screenSleptAtMs = 0;
uint32_t lastLowBatteryReminderMs = 0;
uint16_t imuCalibrationCount = 0;
float filteredAccelX = 0.0f;
float filteredAccelY = 0.0f;
float neutralAccelX = 0.0f;
float neutralAccelY = 0.0f;
float previousAccelX = 0.0f;
float previousAccelY = 0.0f;
float previousAccelZ = 0.0f;
float shakeEnergy = 0.0f;
bool statusDismissing = false;
uint8_t eyeMenuAClickCount = 0;
uint8_t eyeMenuIndex = 0;
uint8_t motionWakeSampleCount = 0;
String lastDiagnosticEvent = "Waiting for input";
String serialCommand;
GestureAxis gestureAxis = GestureAxis::None;
bool gestureCommitted = false;
int32_t batteryLevel = -1;
int16_t batteryVoltageMv = -1;
m5::rtc_datetime_t rtcDateTime;
ScreenPowerState screenPowerState = ScreenPowerState::Bright;
ExpressionId statusReturnExpression = ExpressionId::Idle;
ExpressionId wifiReturnExpression = ExpressionId::Idle;
ExpressionId eyeMenuReturnExpression = ExpressionId::Idle;
EyeMenuPage eyeMenuPage = EyeMenuPage::Root;

void setFont();
void startVibration(uint8_t strength, uint16_t durationMs);
void renderEyeMenu(uint32_t nowMs);
void closeEyeMenu(uint32_t nowMs);

bool reached(uint32_t now, uint32_t deadline) {
  return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0;
}

uint32_t clampTimeoutSeconds(int seconds, uint32_t fallbackMs) {
  if (seconds < 10 || seconds > 24 * 60 * 60) return fallbackMs;
  return static_cast<uint32_t>(seconds) * 1000UL;
}

void loadSettings() {
  if (!preferences.begin(kPreferencesNamespace, false)) {
    Serial.println("Settings storage unavailable; using defaults");
    return;
  }
  settings.brightness =
      preferences.getUChar("brightness", kDefaultBrightness);
  settings.soundVolume =
      preferences.getUChar("sound_vol", kDefaultSoundVolume);
  settings.wifiSsid = preferences.getString("wifi_ssid", "");
  settings.wifiPassword = preferences.getString("wifi_pass", "");
  settings.dimAfterMs = clampTimeoutSeconds(
      preferences.getUInt("dim_sec", kDefaultDimAfterMs / 1000UL),
      kDefaultDimAfterMs);
  settings.screenOffAfterMs = clampTimeoutSeconds(
      preferences.getUInt("off_sec", kDefaultScreenOffAfterMs / 1000UL),
      kDefaultScreenOffAfterMs);
  settings.quietStartHour = preferences.getUChar("quiet_start", 22);
  settings.quietEndHour = preferences.getUChar("quiet_end", 7);

  // Version 1 used 60 s for dimming and 300 s for panel sleep. Migrate that
  // exact old default so existing devices now become fully dark at 60 s.
  const uint8_t storedSchema = preferences.getUChar("schema", 1);
  if (storedSchema < kSettingsSchemaVersion &&
      settings.dimAfterMs == 60UL * 1000UL &&
      settings.screenOffAfterMs == 5UL * 60UL * 1000UL) {
    settings.dimAfterMs = kDefaultDimAfterMs;
    settings.screenOffAfterMs = kDefaultScreenOffAfterMs;
    preferences.putUInt("dim_sec", settings.dimAfterMs / 1000UL);
    preferences.putUInt("off_sec", settings.screenOffAfterMs / 1000UL);
    Serial.println("Power timeout migrated: dim 45s, screen off 60s");
  }
  preferences.putUChar("schema", kSettingsSchemaVersion);

  settings.brightness = constrain(settings.brightness, 20, 255);
  settings.soundVolume = constrain(settings.soundVolume, 0, 160);
  settings.quietStartHour = std::min<uint8_t>(settings.quietStartHour, 23);
  settings.quietEndHour = std::min<uint8_t>(settings.quietEndHour, 23);
  if (settings.screenOffAfterMs <= settings.dimAfterMs) {
    settings.screenOffAfterMs = settings.dimAfterMs + 60000UL;
  }
}

void saveSettings() {
  if (!preferences.isKey("brightness") ||
      preferences.getUChar("brightness") != settings.brightness) {
    preferences.putUChar("brightness", settings.brightness);
  }
  if (!preferences.isKey("sound_vol") ||
      preferences.getUChar("sound_vol") != settings.soundVolume) {
    preferences.putUChar("sound_vol", settings.soundVolume);
  }
  if (!preferences.isKey("wifi_ssid") ||
      preferences.getString("wifi_ssid") != settings.wifiSsid) {
    preferences.putString("wifi_ssid", settings.wifiSsid);
  }
  if (!preferences.isKey("wifi_pass") ||
      preferences.getString("wifi_pass") != settings.wifiPassword) {
    preferences.putString("wifi_pass", settings.wifiPassword);
  }
  preferences.putUInt("dim_sec", settings.dimAfterMs / 1000UL);
  preferences.putUInt("off_sec", settings.screenOffAfterMs / 1000UL);
  preferences.putUChar("quiet_start", settings.quietStartHour);
  preferences.putUChar("quiet_end", settings.quietEndHour);
  preferences.putUChar("schema", kSettingsSchemaVersion);
}

bool isValidRtcDateTime(const m5::rtc_datetime_t& value) {
  return value.date.year >= 2024 && value.date.year <= 2099 &&
         value.date.month >= 1 && value.date.month <= 12 &&
         value.date.date >= 1 && value.date.date <= 31 &&
         value.time.hours >= 0 && value.time.hours <= 23 &&
         value.time.minutes >= 0 && value.time.minutes <= 59 &&
         value.time.seconds >= 0 && value.time.seconds <= 59;
}

void sampleRtc(bool logResult = false) {
  rtcValid = M5.Rtc.isEnabled() && M5.Rtc.getDateTime(&rtcDateTime) &&
             isValidRtcDateTime(rtcDateTime) && !M5.Rtc.getVoltLow();
  if (logResult) {
    if (rtcValid) {
      Serial.printf("RTC %04d-%02d-%02d %02d:%02d:%02d\n",
                    rtcDateTime.date.year, rtcDateTime.date.month,
                    rtcDateTime.date.date, rtcDateTime.time.hours,
                    rtcDateTime.time.minutes, rtcDateTime.time.seconds);
    } else {
      Serial.println("RTC time is not set");
    }
  }
}

bool isQuietHour() {
  if (!rtcValid || settings.quietStartHour == settings.quietEndHour) {
    return false;
  }
  const uint8_t hour = rtcDateTime.time.hours;
  if (settings.quietStartHour < settings.quietEndHour) {
    return hour >= settings.quietStartHour && hour < settings.quietEndHour;
  }
  return hour >= settings.quietStartHour || hour < settings.quietEndHour;
}

void playUiSound(UiSound sound, uint8_t variant = 0) {
  if (!uiSounds.ready() || settings.soundVolume == 0 || isQuietHour() ||
      screenPowerState == ScreenPowerState::Sleeping) {
    return;
  }
  uiSounds.play(sound, variant);
}

void playExpressionSound(ExpressionId expression) {
  switch (expression) {
    case ExpressionId::Happy:
    case ExpressionId::Excited:
      playUiSound(UiSound::Confirm);
      break;
    case ExpressionId::Surprised:
      playUiSound(UiSound::Open);
      break;
    case ExpressionId::Angry:
      playUiSound(UiSound::Warning);
      break;
    case ExpressionId::Sad:
    case ExpressionId::Sleepy:
      playUiSound(UiSound::Previous);
      break;
    default:
      playUiSound(UiSound::Tap);
      break;
  }
}

void samplePower() {
  batteryLevel = M5.Power.getBatteryLevel();
  batteryVoltageMv = M5.Power.getBatteryVoltage();
  const auto chargeState = M5.Power.isCharging();
  chargeReadingValid =
      chargeState != m5::Power_Class::is_charging_t::charge_unknown;
  if (chargeReadingValid) {
    charging = chargeState == m5::Power_Class::is_charging_t::is_charging;
  }
}

String formattedTime() {
  if (!rtcValid) return "--:--";
  char value[8];
  snprintf(value, sizeof(value), "%02d:%02d", rtcDateTime.time.hours,
           rtcDateTime.time.minutes);
  return String(value);
}

String formattedDate() {
  if (!rtcValid) return "RTC not set";
  char value[16];
  snprintf(value, sizeof(value), "%04d-%02d-%02d", rtcDateTime.date.year,
           rtcDateTime.date.month, rtcDateTime.date.date);
  return String(value);
}

uint8_t soundVolumeLevelIndex() {
  uint8_t activeLevel = 0;
  uint8_t closestDistance = 255;
  for (uint8_t index = 0;
       index < sizeof(kSoundVolumeLevels) / sizeof(kSoundVolumeLevels[0]);
       ++index) {
    const uint8_t distance = abs(static_cast<int>(settings.soundVolume) -
                                 kSoundVolumeLevels[index]);
    if (distance < closestDistance) {
      closestDistance = distance;
      activeLevel = index;
    }
  }
  return activeLevel;
}

uint8_t brightnessLevelIndex() {
  constexpr uint8_t levels[] = {60, 100, 150, 220};
  uint8_t activeLevel = 0;
  uint8_t closestDistance = 255;
  for (uint8_t index = 0; index < 4; ++index) {
    const uint8_t distance =
        abs(static_cast<int>(settings.brightness) - levels[index]);
    if (distance < closestDistance) {
      closestDistance = distance;
      activeLevel = index;
    }
  }
  return activeLevel;
}

void setStatusEyeMessage(uint32_t nowMs) {
  String leftText;
  const String rightText =
      batteryLevel >= 0 ? String(batteryLevel) + "%" : "--";
  if (charging) {
    leftText = "充电";
  } else if (batteryLevel >= 80) {
    leftText = "充足";
  } else if (batteryLevel >= 45) {
    leftText = "不错";
  } else if (batteryLevel >= 20) {
    leftText = "累了";
  } else {
    leftText = "休息";
  }
  avatar.setEyeMessage(leftText, rightText, nowMs,
                       kStatusMessageHoldMs + 180);
}

void wakeDisplay(uint32_t nowMs) {
  const bool wasSleeping = screenPowerState == ScreenPowerState::Sleeping;
  if (wasSleeping) M5.Display.wakeup();
  M5.Display.setBrightness(settings.brightness);
  screenPowerState = ScreenPowerState::Bright;
  screenSleptAtMs = 0;
  motionWakeSampleCount = 0;
  lastActivityMs = nowMs;
  if (nightRestActive) {
    nightRestActive = false;
    avatar.show(avatar.baseExpression(), nowMs, false, 260);
  }
  if (wasSleeping) {
    avatar.invalidate();
    playUiSound(UiSound::Wake);
    Serial.println("Display woke");
  }
}

void noteActivity(uint32_t nowMs) {
  if (screenPowerState != ScreenPowerState::Bright) {
    wakeDisplay(nowMs);
  } else {
    lastActivityMs = nowMs;
  }
}

void showStatus(uint32_t nowMs) {
  noteActivity(nowMs);
  samplePower();
  sampleRtc();
  avatar.setEnergyUi(batteryLevel >= 0 ? batteryLevel / 100.0f : 1.0f,
                     charging, true);
  setStatusEyeMessage(nowMs);
  if (!statusMode) {
    playUiSound(UiSound::Open);
    statusReturnExpression = avatar.baseExpression();
    ExpressionId statusExpression = ExpressionId::Idle;
    if (charging) {
      statusExpression = ExpressionId::Listening;
    } else if (batteryLevel >= 70) {
      statusExpression = ExpressionId::Listening;
    } else if (batteryLevel >= 35) {
      statusExpression = ExpressionId::Curious;
    } else if (batteryLevel >= 15) {
      statusExpression = ExpressionId::Sad;
    } else if (batteryLevel >= 0) {
      statusExpression = ExpressionId::Sleepy;
    }
    avatar.play(statusExpression, nowMs, AvatarEngine::PlaybackMode::PingPong,
                false, 260);
    avatar.invalidate();
  }
  statusMode = true;
  statusDismissing = false;
  statusReactionStartsAtMs = nowMs + kStatusMessageHoldMs;
  statusDismissesAtMs =
      statusReactionStartsAtMs + kStatusDismissAnimationMs;
  Serial.println("Immersive energy expression opened");
}

void hideStatus() {
  if (!statusMode) return;
  statusMode = false;
  statusDismissing = false;
  statusReactionStartsAtMs = 0;
  statusDismissesAtMs = 0;
  avatar.releaseTouch();
  avatar.clearEnergyUi();
  avatar.show(statusReturnExpression, millis(), false, 260);
  avatar.invalidate();
  Serial.println("Immersive energy expression closed");
}

void renderWifiFace(uint32_t nowMs) {
  if (!wifiMode) return;
  avatar.clearEnergyUi();
  String leftText;
  String rightText;
  float eyeLevel = 1.0f;
  ExpressionId expression = ExpressionId::Curious;
  AvatarEngine::PlaybackMode playback = AvatarEngine::PlaybackMode::PingPong;

  switch (wifiPairing.state()) {
    case WifiPairing::State::Offline:
      leftText = "长按";
      rightText = "配网";
      expression = ExpressionId::Curious;
      break;
    case WifiPairing::State::Connecting:
      leftText = "连接";
      eyeLevel = 0.58f;
      expression = ExpressionId::Thinking;
      break;
    case WifiPairing::State::Portal:
      leftText = "KK";
      rightText = wifiPairing.accessPointCode();
      expression = ExpressionId::Curious;
      break;
    case WifiPairing::State::Connected:
      leftText = "已连";
      eyeLevel = wifiPairing.signalLevel();
      expression = ExpressionId::Listening;
      break;
    case WifiPairing::State::Failed:
      leftText = "失败";
      rightText = "重试";
      expression = ExpressionId::Sad;
      break;
  }

  avatar.setEnergyUi(eyeLevel, false, false);
  avatar.setEyeMessage(leftText, rightText, nowMs, 10UL * 60UL * 1000UL);
  avatar.play(expression, nowMs, playback, false, 240);
  avatar.invalidate();
}

void enterWifiMode(uint32_t nowMs) {
  noteActivity(nowMs);
  if (statusMode) hideStatus();
  if (!wifiMode) {
    wifiReturnExpression = avatar.baseExpression();
    wifiMode = true;
    wifiPairingInputArmed = false;
    wifiPairingHoldLatched = false;
    playUiSound(UiSound::Open);
    startVibration(120, 34);
  }
  wifiPairing.consumeStateChanged();
  renderWifiFace(nowMs);
  Serial.printf("Wi-Fi expression mode: state=%s ap=%s ip=%s\n",
                wifiPairing.stateName(),
                wifiPairing.accessPointName().c_str(),
                wifiPairing.localIp().c_str());
}

void leaveWifiMode(uint32_t nowMs) {
  if (!wifiMode) return;
  if (wifiPairing.portalActive()) wifiPairing.cancelPortal();
  wifiMode = false;
  wifiPairingInputArmed = false;
  wifiPairingHoldLatched = false;
  avatar.releaseTouch();
  avatar.clearEnergyUi();
  avatar.show(wifiReturnExpression, nowMs, false, 260);
  avatar.invalidate();
  playUiSound(UiSound::Close);
  Serial.println("Wi-Fi expression mode closed");
}

void updateWifiPairing(uint32_t nowMs) {
  wifiPairing.update(nowMs);
  String newSsid;
  String newPassword;
  if (wifiPairing.takeNewCredentials(newSsid, newPassword)) {
    settings.wifiSsid = newSsid;
    settings.wifiPassword = newPassword;
    saveSettings();
    Serial.printf("Wi-Fi credentials saved: ssid=%s\n",
                  settings.wifiSsid.c_str());
  }
  if (!wifiPairing.consumeStateChanged()) return;
  const bool menuWifiVisible =
      eyeMenuMode && eyeMenuPage == EyeMenuPage::Wifi;
  if (!wifiMode && !menuWifiVisible) return;
  if (menuWifiVisible) {
    renderEyeMenu(nowMs);
  } else {
    renderWifiFace(nowMs);
  }
  if (wifiPairing.connected()) {
    startVibration(150, 45);
    playUiSound(UiSound::Confirm);
  } else if (wifiPairing.state() == WifiPairing::State::Failed) {
    startVibration(90, 70);
    playUiSound(UiSound::Warning);
  }
}

void handleWifiInput(uint32_t nowMs) {
  const auto touch = M5.Touch.getDetail(0);
  if (touch.wasPressed()) noteActivity(nowMs);
  if (touch.isPressed()) avatar.setTouchTarget(touch.x, touch.y);
  if (touch.wasReleased()) avatar.releaseTouch();

  if (M5.BtnA.wasClicked()) {
    noteActivity(nowMs);
    leaveWifiMode(nowMs);
    return;
  }

  // The hold used to enter Wi-Fi mode must be released before pairing can arm.
  if (!wifiPairingInputArmed && !M5.BtnB.isPressed()) {
    wifiPairingInputArmed = true;
  }
  if (M5.BtnB.wasReleased()) wifiPairingHoldLatched = false;

  if (wifiPairingInputArmed && !wifiPairingHoldLatched &&
      M5.BtnB.pressedFor(kWifiPairingHoldMs)) {
    wifiPairingHoldLatched = true;
    noteActivity(nowMs);
    if (!wifiPairing.portalActive()) {
      wifiPairing.startPortal(nowMs);
      wifiPairing.consumeStateChanged();
      renderWifiFace(nowMs);
      playUiSound(UiSound::Open);
      startVibration(150, 42);
    }
    return;
  }

  if (M5.BtnB.wasClicked()) {
    noteActivity(nowMs);
    startVibration(95, 24);
    if (!wifiPairing.portalActive()) {
      avatar.setEyeMessage("长按", "配网", nowMs, 1500);
      avatar.invalidate();
    } else {
      renderWifiFace(nowMs);
    }
  }
}

void cycleBrightness(uint32_t nowMs, bool refreshStatus = true) {
  constexpr uint8_t levels[] = {60, 100, 150, 220};
  uint8_t next = levels[0];
  uint8_t nextIndex = 0;
  for (uint8_t index = 0; index < 4; ++index) {
    if (levels[index] > settings.brightness) {
      next = levels[index];
      nextIndex = index;
      break;
    }
  }
  settings.brightness = next;
  saveSettings();
  M5.Display.setBrightness(settings.brightness);
  startVibration(115, 30);
  if (refreshStatus) showStatus(nowMs);
  playUiSound(UiSound::Brightness, nextIndex);
  Serial.printf("Brightness saved: %u\n", settings.brightness);
}

void cycleSoundVolume(uint32_t nowMs, bool refreshStatus = true) {
  const uint8_t currentIndex = soundVolumeLevelIndex();
  const uint8_t nextIndex =
      (currentIndex + 1) %
      (sizeof(kSoundVolumeLevels) / sizeof(kSoundVolumeLevels[0]));
  settings.soundVolume = kSoundVolumeLevels[nextIndex];
  saveSettings();
  uiSounds.setVolume(settings.soundVolume);
  startVibration(105, 28);
  if (refreshStatus) showStatus(nowMs);
  if (settings.soundVolume > 0) {
    playUiSound(UiSound::Brightness,
                std::min<uint8_t>(nextIndex - 1, 3));
  }
  Serial.printf("Sound volume saved: %u (%s)\n", settings.soundVolume,
                nextIndex == 0 ? "muted" : String(nextIndex).c_str());
}

void renderEyeMenu(uint32_t nowMs) {
  if (!eyeMenuMode) return;

  avatar.clearEnergyUi();
  String leftText;
  String rightText;
  float eyeLevel = 1.0f;
  ExpressionId expression = ExpressionId::Curious;

  if (eyeMenuPage == EyeMenuPage::Root) {
    constexpr const char* labels[] = {"亮度", "音量", "网络"};
    leftText = labels[eyeMenuIndex % 3];
    rightText = String((eyeMenuIndex % 3) + 1) + "/3";
    // Eye messages are intentionally rendered only while the energy/status
    // layer is active. Keep that layer enabled for the otherwise pure root
    // menu so its labels remain visible inside the eyes.
    avatar.setEnergyUi(1.0f, false, false);
  } else if (eyeMenuPage == EyeMenuPage::Brightness) {
    leftText = "亮度";
    rightText = String(brightnessLevelIndex() + 1) + "/4";
    eyeLevel = (brightnessLevelIndex() + 1) / 4.0f;
    avatar.setEnergyUi(eyeLevel, false, false);
  } else if (eyeMenuPage == EyeMenuPage::Sound) {
    const uint8_t level = soundVolumeLevelIndex();
    leftText = level == 0 ? "静音" : "音量";
    rightText = String(level) + "/4";
    eyeLevel = level / 4.0f;
    avatar.setEnergyUi(eyeLevel, false, false);
  } else {
    switch (wifiPairing.state()) {
      case WifiPairing::State::Offline:
        leftText = "网络";
        rightText = "未连";
        break;
      case WifiPairing::State::Connecting:
        leftText = "网络";
        rightText = "连接";
        eyeLevel = 0.58f;
        expression = ExpressionId::Thinking;
        break;
      case WifiPairing::State::Portal:
        leftText = "KK";
        rightText = wifiPairing.accessPointCode();
        break;
      case WifiPairing::State::Connected:
        leftText = "网络";
        rightText = "已连";
        eyeLevel = wifiPairing.signalLevel();
        expression = ExpressionId::Listening;
        break;
      case WifiPairing::State::Failed:
        leftText = "网络";
        rightText = "失败";
        expression = ExpressionId::Sad;
        break;
    }
    avatar.setEnergyUi(eyeLevel, false, false);
  }

  avatar.setEyeMessage(leftText, rightText, nowMs, 10UL * 60UL * 1000UL);
  avatar.play(expression, nowMs, AvatarEngine::PlaybackMode::PingPong, false,
              220);
  avatar.invalidate();
}

void openEyeMenu(uint32_t nowMs) {
  noteActivity(nowMs);
  if (statusMode) hideStatus();
  if (wifiMode) leaveWifiMode(nowMs);
  if (!eyeMenuMode) {
    eyeMenuReturnExpression = avatar.baseExpression();
    eyeMenuMode = true;
    eyeMenuPage = EyeMenuPage::Root;
    eyeMenuIndex = 0;
    eyeMenuAClickCount = 0;
    eyeMenuAClickDeadlineMs = 0;
    eyeMenuPageReadyAtMs = 0;
    eyeMenuInputArmed = false;
    eyeMenuWifiHoldLatched = false;
    playUiSound(UiSound::Open);
    startVibration(120, 34);
  }
  renderEyeMenu(nowMs);
  Serial.println("Eye menu opened");
}

void closeEyeMenu(uint32_t nowMs) {
  if (!eyeMenuMode) return;
  if (wifiPairing.portalActive()) wifiPairing.cancelPortal();
  eyeMenuMode = false;
  eyeMenuPage = EyeMenuPage::Root;
  eyeMenuAClickCount = 0;
  eyeMenuAClickDeadlineMs = 0;
  eyeMenuPageReadyAtMs = 0;
  eyeMenuInputArmed = false;
  eyeMenuWifiHoldLatched = false;
  avatar.releaseTouch();
  avatar.clearEnergyUi();
  avatar.show(eyeMenuReturnExpression, nowMs, false, 240);
  avatar.invalidate();
  playUiSound(UiSound::Close);
  Serial.println("Eye menu closed");
}

void openSelectedEyeMenuPage(uint32_t nowMs) {
  eyeMenuPage = static_cast<EyeMenuPage>(eyeMenuIndex + 1);
  eyeMenuAClickCount = 0;
  eyeMenuAClickDeadlineMs = 0;
  eyeMenuPageReadyAtMs = nowMs + 350;
  eyeMenuWifiHoldLatched = false;
  startVibration(125, 34);
  playUiSound(UiSound::Confirm);
  renderEyeMenu(nowMs);
  Serial.printf("Eye menu page opened: %u\n",
                static_cast<unsigned>(eyeMenuPage));
}

void handleEyeMenuInput(uint32_t nowMs) {
  const auto touch = M5.Touch.getDetail(0);
  if (touch.wasPressed()) noteActivity(nowMs);
  if (touch.isPressed()) avatar.setTouchTarget(touch.x, touch.y);
  if (touch.wasReleased()) avatar.releaseTouch();

  if (!eyeMenuInputArmed) {
    if (!M5.BtnA.isPressed()) eyeMenuInputArmed = true;
    return;
  }

  if (M5.BtnB.wasClicked()) {
    noteActivity(nowMs);
    eyeMenuAClickCount = 0;
    eyeMenuAClickDeadlineMs = 0;
    if (eyeMenuPage == EyeMenuPage::Root) {
      closeEyeMenu(nowMs);
    } else {
      if (eyeMenuPage == EyeMenuPage::Wifi && wifiPairing.portalActive()) {
        wifiPairing.cancelPortal();
        wifiPairing.consumeStateChanged();
      }
      eyeMenuPage = EyeMenuPage::Root;
      eyeMenuPageReadyAtMs = nowMs + 180;
      eyeMenuWifiHoldLatched = false;
      startVibration(95, 24);
      playUiSound(UiSound::Close);
      renderEyeMenu(nowMs);
    }
    return;
  }

  if (!reached(nowMs, eyeMenuPageReadyAtMs) &&
      eyeMenuPage != EyeMenuPage::Root) {
    return;
  }

  if (eyeMenuPage == EyeMenuPage::Wifi) {
    if (M5.BtnA.wasPressed() && !wifiPairing.portalActive()) {
      noteActivity(nowMs);
      const bool connected = wifiPairing.connected();
      const bool failed = wifiPairing.state() == WifiPairing::State::Failed;
      avatar.setEyeMessage(connected ? "换网" : (failed ? "重试" : "配网"),
                           "按住", nowMs, kWifiPairingHoldMs + 600);
      avatar.invalidate();
    }
    if (M5.BtnA.wasReleased()) eyeMenuWifiHoldLatched = false;
    if (!eyeMenuWifiHoldLatched &&
        M5.BtnA.pressedFor(kWifiPairingHoldMs)) {
      eyeMenuWifiHoldLatched = true;
      noteActivity(nowMs);
      if (!wifiPairing.portalActive()) {
        wifiPairing.startPortal(nowMs);
        wifiPairing.consumeStateChanged();
        startVibration(150, 42);
        playUiSound(UiSound::Open);
        renderEyeMenu(nowMs);
      }
      return;
    }
    if (M5.BtnA.wasClicked()) {
      noteActivity(nowMs);
      startVibration(70, 18);
      renderEyeMenu(nowMs);
    }
    return;
  }

  if (!eyeMenuInputArmed || !M5.BtnA.wasClicked()) {
    if (eyeMenuPage == EyeMenuPage::Root && eyeMenuAClickCount == 1 &&
        reached(nowMs, eyeMenuAClickDeadlineMs)) {
      eyeMenuAClickCount = 0;
      eyeMenuAClickDeadlineMs = 0;
      eyeMenuIndex = (eyeMenuIndex + 1) % 3;
      startVibration(85, 22);
      playUiSound(UiSound::Next);
      renderEyeMenu(nowMs);
      Serial.printf("Eye menu selection: %u/3\n", eyeMenuIndex + 1);
    }
    return;
  }

  noteActivity(nowMs);
  if (eyeMenuPage == EyeMenuPage::Brightness) {
    cycleBrightness(nowMs, false);
    renderEyeMenu(nowMs);
    return;
  }
  if (eyeMenuPage == EyeMenuPage::Sound) {
    cycleSoundVolume(nowMs, false);
    renderEyeMenu(nowMs);
    return;
  }
  if (eyeMenuAClickCount == 0 ||
      !reached(nowMs, eyeMenuAClickDeadlineMs)) {
    ++eyeMenuAClickCount;
  } else {
    eyeMenuAClickCount = 1;
  }
  eyeMenuAClickDeadlineMs = nowMs + kEyeMenuClickWindowMs;

  if (eyeMenuAClickCount < 2) return;
  eyeMenuAClickCount = 0;
  eyeMenuAClickDeadlineMs = 0;
  if (eyeMenuPage == EyeMenuPage::Root) {
    openSelectedEyeMenuPage(nowMs);
  }
}

void setFont() {
  M5.Display.setFont(&fonts::efontCN_16);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(top_left);
}

void drawDiagnosticLine(int y, const String& label, const String& value,
                        uint16_t color = TFT_WHITE) {
  M5.Display.fillRect(46, y, 374, 22, TFT_BLACK);
  M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Display.drawString(label, 46, y);
  M5.Display.setTextColor(color, TFT_BLACK);
  M5.Display.drawString(value, 156, y);
}

void drawDiagnosticFrame() {
  M5.Display.fillScreen(TFT_BLACK);
  const int cx = M5.Display.width() / 2;
  const int cy = M5.Display.height() / 2;
  M5.Display.drawCircle(cx, cy, min(cx, cy) - 3, TFT_DARKGREY);
  M5.Display.drawCircle(cx, cy, min(cx, cy) - 12, TFT_NAVY);

  setFont();
  M5.Display.setTextDatum(top_center);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.drawString("Hardware Check", cx, 45);
  M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Display.drawString("Hold A+B to return", cx, 72);
  M5.Display.setTextDatum(top_left);

  drawDiagnosticLine(112, "Display",
                     String(M5.Display.width()) + " x " + String(M5.Display.height()),
                     TFT_GREEN);
  drawDiagnosticLine(146, "IMU", "initializing...");
  drawDiagnosticLine(180, "Touch", M5.Touch.isEnabled() ? "enabled" : "not detected");
  drawDiagnosticLine(214, "Vibration", vibrationReady ? "ready" : "not detected");
  drawDiagnosticLine(264, "Last event", lastDiagnosticEvent, TFT_YELLOW);

  M5.Display.setTextDatum(top_center);
  M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Display.drawString("A: vibrate", cx, 340);
  M5.Display.drawString("B: redraw", cx, 370);
  M5.Display.drawString("Touch: show coordinates", cx, 400);
  M5.Display.setTextDatum(top_left);
}

void setDiagnosticEvent(const String& event) {
  lastDiagnosticEvent = event;
  drawDiagnosticLine(264, "Last event", lastDiagnosticEvent, TFT_YELLOW);
  Serial.println(event);
}

void stopVibration() {
  if (!vibrationReady) return;
  const uint8_t pwmData[2] = {0, 0x80};
  M5.In_I2C.writeRegister(kIoeAddress, kVibrationPwmRegister, pwmData,
                          sizeof(pwmData), kIoeBusFrequency);
  vibrationStopsAtMs = 0;
}

void startVibration(uint8_t strength = 160, uint16_t durationMs = 45) {
  if (!vibrationReady) return;
  const uint16_t duty12 =
      static_cast<uint16_t>(strength) * 0x0FFF / 0xFF;
  const uint8_t pwmData[2] = {
      static_cast<uint8_t>(duty12 & 0xFF),
      static_cast<uint8_t>(0x80 | ((duty12 >> 8) & 0x0F)),
  };
  if (!M5.In_I2C.writeRegister(kIoeAddress, kVibrationPwmRegister, pwmData,
                               sizeof(pwmData), kIoeBusFrequency)) {
    Serial.println("Vibration PWM write failed");
    return;
  }
  vibrationStopsAtMs = millis() + durationMs;
}

void updateVibration(uint32_t nowMs) {
  if (reached(nowMs, vibrationStopsAtMs)) stopVibration();
}

void setupVibration() {
  const m5ioe1_err_t error =
      ioe.begin(&M5.In_I2C, kIoeAddress, M5IOE1_I2C_FREQ_100K);
  vibrationReady = error == M5IOE1_OK;
  if (vibrationReady) {
    ioe.setPwmFrequency(200);
    // Configure the PWM pin once through the verified driver. Runtime haptics
    // then need only one two-byte register write instead of a write/readback/
    // GPIO-mode sequence that can stall an animation frame.
    vibrationReady =
        ioe.setPwmDuty12bit(M5IOE1_PWM_CH1, 0, false, true) == M5IOE1_OK;
  }
}

void trigger(ExpressionId expression, uint32_t nowMs,
             uint16_t firstTransitionMs = 0) {
  avatar.show(expression, nowMs, true, firstTransitionMs);
  startVibration();
  playExpressionSound(expression);
  Serial.printf("Expression: %s\n", avatar.activeName());
}

void updateCompanionSensors(uint32_t nowMs) {
  if (nowMs - lastRtcSampleMs >= kRtcSampleIntervalMs) {
    lastRtcSampleMs = nowMs;
    sampleRtc();
  }

  if (nowMs - lastPowerSampleMs < kPowerSampleIntervalMs) return;
  lastPowerSampleMs = nowMs;
  const bool previousCharging = charging;
  samplePower();

  if (chargeReadingValid) {
    if (!chargingStateKnown) {
      chargingStateKnown = true;
      Serial.printf("Power ready: battery=%ld%% voltage=%dmV charging=%s\n",
                    static_cast<long>(batteryLevel), batteryVoltageMv,
                    charging ? "yes" : "no");
    } else if (charging != previousCharging) {
      Serial.printf("Charging state changed: %s\n",
                    charging ? "connected" : "disconnected");
      if (!diagnosticMode && !statusMode && !wifiMode && !eyeMenuMode &&
          screenPowerState != ScreenPowerState::Sleeping) {
        trigger(charging ? ExpressionId::Excited : ExpressionId::Curious,
                nowMs, 220);
      }
    }
  }

  const bool lowBattery = batteryLevel >= 0 &&
                          batteryLevel <= kLowBatteryThreshold && !charging;
  if (lowBattery && !diagnosticMode && !statusMode && !wifiMode &&
      !eyeMenuMode &&
      screenPowerState != ScreenPowerState::Sleeping &&
      (lastLowBatteryReminderMs == 0 ||
       nowMs - lastLowBatteryReminderMs >= kLowBatteryReminderIntervalMs)) {
    lastLowBatteryReminderMs = nowMs;
    trigger(ExpressionId::Sleepy, nowMs, 320);
    Serial.printf("Low battery reminder: %ld%%\n",
                  static_cast<long>(batteryLevel));
  }
}

void updateDisplayPower(uint32_t nowMs) {
  if (wifiMode || eyeMenuMode) {
    lastActivityMs = nowMs;
    return;
  }
  if (diagnosticMode || statusMode) return;
  const uint32_t inactiveMs = nowMs - lastActivityMs;

  if (inactiveMs >= settings.screenOffAfterMs) {
    if (screenPowerState != ScreenPowerState::Sleeping) {
      stopVibration();
      M5.Display.fillScreen(TFT_BLACK);
      M5.Display.setBrightness(0);
      M5.Display.sleep();
      screenPowerState = ScreenPowerState::Sleeping;
      screenSleptAtMs = nowMs;
      motionWakeSampleCount = 0;
      Serial.println("Display sleeping; touch, buttons, or motion to wake");
    }
    return;
  }

  if (inactiveMs >= settings.dimAfterMs) {
    if (screenPowerState == ScreenPowerState::Bright) {
      if (isQuietHour()) {
        avatar.show(ExpressionId::Sleepy, nowMs, false, 420);
        nightRestActive = true;
        Serial.println("Quiet-hour sleepy state");
      }
      M5.Display.setBrightness(
          std::min<uint8_t>(settings.brightness, kDimBrightness));
      screenPowerState = ScreenPowerState::Dimmed;
      Serial.println("Display dimmed; renderer reduced to 20 fps");
    }
  }
}

void printCompanionStatus() {
  samplePower();
  sampleRtc();
  Serial.printf(
      "Status: time=%s date=%s battery=%ld%% voltage=%dmV charging=%s "
      "brightness=%u volume=%u dim=%lus off=%lus quiet=%02u-%02u "
      "wifi=%s ip=%s\n",
      formattedTime().c_str(), formattedDate().c_str(),
      static_cast<long>(batteryLevel), batteryVoltageMv,
      chargeReadingValid ? (charging ? "yes" : "no") : "unknown",
      settings.brightness, settings.soundVolume,
      static_cast<unsigned long>(settings.dimAfterMs / 1000UL),
      static_cast<unsigned long>(settings.screenOffAfterMs / 1000UL),
      settings.quietStartHour, settings.quietEndHour,
      wifiPairing.stateName(), wifiPairing.localIp().c_str());
  Serial.printf("Wi-Fi detail: ssid=%s state=%s ip=%s\n",
                settings.wifiSsid.isEmpty() ? "--" : settings.wifiSsid.c_str(),
                wifiPairing.stateName(), wifiPairing.localIp().c_str());
}

bool setRtcFromCommand(const String& command) {
  int year, month, day, hour, minute, second;
  if (sscanf(command.c_str(), "time %d-%d-%d %d:%d:%d", &year, &month,
             &day, &hour, &minute, &second) != 6 || year < 2024 ||
      year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
      second > 59) {
    Serial.println("Usage: time YYYY-MM-DD HH:MM:SS");
    return true;
  }

  tm calendar = {};
  calendar.tm_year = year - 1900;
  calendar.tm_mon = month - 1;
  calendar.tm_mday = day;
  calendar.tm_hour = hour;
  calendar.tm_min = minute;
  calendar.tm_sec = second;
  mktime(&calendar);
  const m5::rtc_datetime_t value(
      m5::rtc_date_t(year, month, day, calendar.tm_wday),
      m5::rtc_time_t(hour, minute, second));
  M5.Rtc.setDateTime(value);
  delay(20);
  sampleRtc(true);
  return true;
}

bool handleCompanionCommand(const String& rawCommand, uint32_t nowMs) {
  String command = rawCommand;
  command.trim();
  command.toLowerCase();

  if (command == "status") {
    printCompanionStatus();
    showStatus(nowMs);
    return true;
  }
  if (command == "sound" || command == "sound test") {
    playUiSound(UiSound::Confirm);
    const char* result = !uiSounds.ready()      ? "unavailable"
                         : settings.soundVolume == 0 ? "muted"
                         : isQuietHour()            ? "quiet hours"
                                                    : "played";
    Serial.printf("Sound test: %s\n", result);
    return true;
  }
  if (command == "time") {
    sampleRtc(true);
    return true;
  }
  if (command.startsWith("time ")) return setRtcFromCommand(command);
  if (command == "wifi") {
    Serial.printf("Wi-Fi: state=%s ssid=%s ip=%s ap=%s\n",
                  wifiPairing.stateName(),
                  settings.wifiSsid.isEmpty() ? "--"
                                              : settings.wifiSsid.c_str(),
                  wifiPairing.localIp().c_str(),
                  wifiPairing.accessPointName().c_str());
    return true;
  }
  if (command == "wifi pair") {
    enterWifiMode(nowMs);
    if (!wifiPairing.portalActive()) {
      wifiPairing.startPortal(nowMs);
      wifiPairing.consumeStateChanged();
      renderWifiFace(nowMs);
    }
    return true;
  }
  if (command == "wifi retry") {
    enterWifiMode(nowMs);
    wifiPairing.retry(nowMs);
    wifiPairing.consumeStateChanged();
    renderWifiFace(nowMs);
    return true;
  }
  if (command == "wifi forget") {
    settings.wifiSsid = "";
    settings.wifiPassword = "";
    saveSettings();
    wifiPairing.forget();
    enterWifiMode(nowMs);
    return true;
  }

  int value = 0;
  if (sscanf(command.c_str(), "brightness %d", &value) == 1) {
    if (value < 20 || value > 255) {
      Serial.println("Brightness range: 20-255");
    } else {
      settings.brightness = static_cast<uint8_t>(value);
      saveSettings();
      noteActivity(nowMs);
      M5.Display.setBrightness(settings.brightness);
      Serial.printf("Brightness saved: %u\n", settings.brightness);
    }
    return true;
  }
  if (sscanf(command.c_str(), "volume %d", &value) == 1) {
    if (value < 0 || value > 160) {
      Serial.println("Volume range: 0-160");
    } else {
      settings.soundVolume = static_cast<uint8_t>(value);
      saveSettings();
      uiSounds.setVolume(settings.soundVolume);
      showStatus(nowMs);
      if (settings.soundVolume > 0) playUiSound(UiSound::Confirm);
      Serial.printf("Sound volume saved: %u\n", settings.soundVolume);
    }
    return true;
  }
  if (sscanf(command.c_str(), "dim %d", &value) == 1) {
    const uint32_t timeout = clampTimeoutSeconds(value, 0);
    if (timeout == 0 || timeout >= settings.screenOffAfterMs) {
      Serial.println("Dim timeout must be 10-86399 seconds and below off timeout");
    } else {
      settings.dimAfterMs = timeout;
      saveSettings();
      Serial.printf("Dim timeout saved: %ds\n", value);
    }
    return true;
  }
  if (sscanf(command.c_str(), "screenoff %d", &value) == 1) {
    const uint32_t timeout = clampTimeoutSeconds(value, 0);
    if (timeout == 0 || timeout <= settings.dimAfterMs) {
      Serial.println("Off timeout must be 10-86400 seconds and above dim timeout");
    } else {
      settings.screenOffAfterMs = timeout;
      saveSettings();
      Serial.printf("Screen-off timeout saved: %ds\n", value);
    }
    return true;
  }

  int startHour, endHour;
  if (sscanf(command.c_str(), "quiet %d %d", &startHour, &endHour) == 2) {
    if (startHour < 0 || startHour > 23 || endHour < 0 || endHour > 23) {
      Serial.println("Quiet hours range: 0-23");
    } else {
      settings.quietStartHour = static_cast<uint8_t>(startHour);
      settings.quietEndHour = static_cast<uint8_t>(endHour);
      saveSettings();
      Serial.printf("Quiet hours saved: %02d:00-%02d:00\n", startHour,
                    endHour);
    }
    return true;
  }

  if (command == "screen on") {
    noteActivity(nowMs);
    Serial.println("Screen forced on");
    return true;
  }
  if (command == "screen off") {
    lastActivityMs = nowMs - settings.screenOffAfterMs;
    updateDisplayPower(nowMs);
    return true;
  }
  return false;
}

void handleProductTouch(uint32_t nowMs) {
  if (!M5.Touch.isEnabled()) return;
  const auto touch = M5.Touch.getDetail(0);

  if (touch.wasPressed()) {
    noteActivity(nowMs);
    gestureAxis = GestureAxis::None;
    gestureCommitted = false;
  }

  if (touch.wasHold() && gestureAxis == GestureAxis::None &&
      !gestureCommitted) {
    gestureCommitted = true;
    avatar.releaseTouch();
    trigger(ExpressionId::Angry, nowMs);
    return;
  }

  if (touch.isPressed()) {
    const bool moving = touch.isFlicking() || touch.isDragging();
    const int deltaX = moving ? touch.distanceX() : 0;
    const int deltaY = moving ? touch.distanceY() : 0;

    if (gestureAxis == GestureAxis::None &&
        std::max(abs(deltaX), abs(deltaY)) >= kGestureDirectionLockPx) {
      gestureAxis = abs(deltaX) >= abs(deltaY) ? GestureAxis::Horizontal
                                               : GestureAxis::Vertical;
    }

    if (gestureAxis == GestureAxis::Horizontal) {
      avatar.releaseTouch();
      const int8_t direction = deltaX > 0 ? -1 : 1;
      avatar.setSwipeOffset(deltaX, 0.0f, 0);
      if (!gestureCommitted && abs(deltaX) >= kGestureCommitPx) {
        trigger(direction < 0 ? ExpressionId::Curious
                              : ExpressionId::Confused,
                nowMs, kSwipeTransitionMs);
        gestureCommitted = true;
        Serial.printf("Horizontal swipe reaction: %s dx=%d\n",
                      avatar.activeName(), deltaX);
      }
      return;
    }

    if (gestureAxis == GestureAxis::Vertical) {
      avatar.releaseTouch();
      avatar.setSwipeOffset(0.0f, deltaY, 0);
      if (!gestureCommitted && abs(deltaY) >= kGestureCommitPx) {
        if (deltaY > 0) {
          showStatus(nowMs);
        } else {
          trigger(ExpressionId::Surprised, nowMs, kSwipeTransitionMs);
        }
        gestureCommitted = true;
        Serial.printf("Vertical swipe action: %s dy=%d\n",
                      deltaY > 0 ? "battery" : avatar.activeName(), deltaY);
      }
      return;
    }

    if (!gestureCommitted) avatar.setTouchTarget(touch.x, touch.y);
  }

  if (touch.wasReleased()) {
    const bool consumed =
        gestureCommitted || gestureAxis != GestureAxis::None;
    avatar.releaseTouch();
    avatar.releaseSwipe();
    gestureAxis = GestureAxis::None;
    gestureCommitted = false;
    if (consumed) return;
  }

  if (gestureCommitted) {
    return;
  }

  if (touch.wasClicked()) {
    trigger(touch.getClickCount() >= 2 ? ExpressionId::Surprised
                                       : ExpressionId::Happy,
            nowMs);
  }
}

void handleImuInteraction(uint32_t nowMs) {
  if (nowMs - lastImuInteractionMs < kImuInteractionIntervalMs) return;
  lastImuInteractionMs = nowMs;
  if (!M5.Imu.update()) return;

  const auto imu = M5.Imu.getImuData();
  // The official StopWatch demo swaps the BMI270 sensor's raw X/Y values to
  // obtain screen coordinates. Keep all avatar interaction in that same
  // coordinate system: X is screen-left/right and Y is screen-up/down.
  const float screenAccelX = imu.accel.y;
  const float screenAccelY = imu.accel.x;
  const float screenAccelZ = imu.accel.z;
  const float screenGyroX = imu.gyro.y;
  const float screenGyroY = imu.gyro.x;
  constexpr float kFilterAmount = 0.32f;
  if (imuCalibrationCount == 0) {
    filteredAccelX = screenAccelX;
    filteredAccelY = screenAccelY;
    previousAccelX = screenAccelX;
    previousAccelY = screenAccelY;
    previousAccelZ = screenAccelZ;
  } else {
    filteredAccelX += (screenAccelX - filteredAccelX) * kFilterAmount;
    filteredAccelY += (screenAccelY - filteredAccelY) * kFilterAmount;
  }

  if (imuCalibrationCount < kImuCalibrationSamples) {
    neutralAccelX += screenAccelX / kImuCalibrationSamples;
    neutralAccelY += screenAccelY / kImuCalibrationSamples;
    ++imuCalibrationCount;
    if (imuCalibrationCount == kImuCalibrationSamples) {
      Serial.printf("IMU interaction ready: neutral x=%.2f y=%.2f\n",
                    neutralAccelX, neutralAccelY);
    }
  } else {
    // Roughly 0.28 g of tilt reaches full gaze travel. Neutral adaptation is
    // deliberately very slow so eyes keep looking in the chosen direction
    // while the user holds the device at an angle. Gyroscope feed-forward
    // makes the eyes lead during rotation; gravity keeps the final direction.
    neutralAccelX += (filteredAccelX - neutralAccelX) * 0.00015f;
    neutralAccelY += (filteredAccelY - neutralAccelY) * 0.00015f;
    avatar.setTiltTarget(-(filteredAccelX - neutralAccelX) / 0.28f,
                         -(filteredAccelY - neutralAccelY) / 0.28f,
                         -screenGyroY / 180.0f, screenGyroX / 180.0f);
  }

  const float deltaX = screenAccelX - previousAccelX;
  const float deltaY = screenAccelY - previousAccelY;
  const float deltaZ = screenAccelZ - previousAccelZ;
  const float jerk = fabsf(deltaX) + fabsf(deltaY) + fabsf(deltaZ);
  previousAccelX = screenAccelX;
  previousAccelY = screenAccelY;
  previousAccelZ = screenAccelZ;
  shakeEnergy = shakeEnergy * 0.72f + jerk * 0.28f;

  if (screenPowerState == ScreenPowerState::Sleeping) {
    const bool graceElapsed =
        screenSleptAtMs != 0 && nowMs - screenSleptAtMs >= kMotionWakeGraceMs;
    if (graceElapsed && jerk >= kMotionWakeJerkThreshold) {
      ++motionWakeSampleCount;
    } else {
      motionWakeSampleCount = 0;
    }
    if (motionWakeSampleCount >= kMotionWakeRequiredSamples) {
      noteActivity(nowMs);
      Serial.printf("Motion wake: jerk=%.2f confirmed=%u\n", jerk,
                    motionWakeSampleCount);
    }
    return;
  }
  motionWakeSampleCount = 0;

  if (imuCalibrationCount < kImuCalibrationSamples) return;

  const float shakeIntensity = std::max(
      0.0f, std::min(1.0f, (shakeEnergy - 0.055f) / 0.48f));
  const float shakeDirectionX =
      std::max(-1.0f, std::min(1.0f, -deltaX / 0.28f));
  const float shakeDirectionY =
      std::max(-1.0f, std::min(1.0f, -deltaY / 0.28f));
  avatar.setShakeTarget(shakeDirectionX, shakeDirectionY, shakeIntensity);
}

void handleSerialCommands(uint32_t nowMs) {
  while (Serial.available() > 0) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\n' || character == '\r') {
      if (serialCommand.length() == 0) continue;
      if (handleCompanionCommand(serialCommand, nowMs)) {
        // Companion commands report their own result.
      } else if (avatar.showFromCommand(serialCommand, nowMs)) {
        noteActivity(nowMs);
        startVibration();
        playExpressionSound(avatar.activeExpression());
        Serial.printf("Command accepted: %s\n", avatar.activeName());
      } else {
        Serial.printf("Unknown command: %s\n", serialCommand.c_str());
      }
      serialCommand = "";
    } else if (serialCommand.length() < 32) {
      serialCommand += character;
    }
  }
}

void refreshDiagnosticSensors() {
  if (M5.Imu.update()) {
    const auto imu = M5.Imu.getImuData();
    char values[64];
    snprintf(values, sizeof(values), "H %.2f  V %.2f  Z %.2f", imu.accel.y,
             imu.accel.x, imu.accel.z);
    drawDiagnosticLine(146, "IMU", values, TFT_GREEN);
  } else {
    drawDiagnosticLine(146, "IMU", "no data", TFT_RED);
  }

  if (M5.Touch.getCount() > 0) {
    const auto touch = M5.Touch.getDetail(0);
    drawDiagnosticLine(180, "Touch",
                       "x " + String(touch.x) + "  y " + String(touch.y),
                       TFT_GREEN);
  } else {
    drawDiagnosticLine(180, "Touch",
                       M5.Touch.isEnabled() ? "enabled" : "not detected",
                       M5.Touch.isEnabled() ? TFT_GREEN : TFT_RED);
  }
}

void setDiagnosticMode(bool enabled) {
  if (statusMode) hideStatus();
  if (wifiMode) leaveWifiMode(millis());
  if (eyeMenuMode) closeEyeMenu(millis());
  diagnosticMode = enabled;
  noteActivity(millis());
  stopVibration();
  if (diagnosticMode) {
    drawDiagnosticFrame();
    Serial.println("Diagnostic mode entered");
  } else {
    avatar.invalidate();
    Serial.println("Expression mode entered");
  }
}

void handleDiagnosticInput(uint32_t nowMs) {
  if (M5.BtnA.wasClicked()) {
    noteActivity(nowMs);
    setDiagnosticEvent("Button A clicked");
    startVibration(180, 70);
  }
  if (M5.BtnB.wasClicked()) {
    noteActivity(nowMs);
    lastDiagnosticEvent = "Button B clicked";
    drawDiagnosticFrame();
    Serial.println(lastDiagnosticEvent);
  }

  const auto touch = M5.Touch.getDetail(0);
  if (touch.wasPressed()) {
    noteActivity(nowMs);
    setDiagnosticEvent("Touch " + String(touch.x) + "," + String(touch.y));
  }

  if (nowMs - lastDiagnosticRefreshMs >= kDiagnosticRefreshIntervalMs) {
    lastDiagnosticRefreshMs = nowMs;
    refreshDiagnosticSensors();
  }
}

void handleDiagnosticToggle() {
  const bool bothHeld = M5.BtnA.pressedFor(1000) && M5.BtnB.pressedFor(1000);
  if (bothHeld && !diagnosticToggleLatched) {
    diagnosticToggleLatched = true;
    setDiagnosticMode(!diagnosticMode);
  } else if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed()) {
    diagnosticToggleLatched = false;
  }
}

void handleStatusInput(uint32_t nowMs) {
  if (!statusDismissing && reached(nowMs, statusReactionStartsAtMs)) {
    statusDismissing = true;
    avatar.beginEnergyDismiss(nowMs);
    startVibration(90, 25);
    playUiSound(UiSound::Close);
    Serial.println("Eye message dismissed with blink and head shake");
  }
  if (reached(nowMs, statusDismissesAtMs)) {
    hideStatus();
    return;
  }

  const auto touch = M5.Touch.getDetail(0);
  if (touch.wasPressed()) {
    noteActivity(nowMs);
    showStatus(nowMs);
  }
  if (touch.isPressed()) {
    avatar.setTouchTarget(touch.x, touch.y);
  }
  if (touch.wasReleased()) {
    avatar.releaseTouch();
  }

  if (touch.wasClicked() || M5.BtnB.wasClicked()) {
    noteActivity(nowMs);
    hideStatus();
  }
}

}  // namespace

void setup() {
  auto config = M5.config();
  config.serial_baudrate = 115200;
  config.internal_spk = true;
  M5.begin(config);
  Serial.begin(115200);
  loadSettings();
  const bool soundReady = uiSounds.begin(settings.soundVolume);

  M5.Display.setRotation(0);
  M5.Display.setBrightness(settings.brightness);
  M5.Touch.setHoldThresh(650);
  M5.Touch.setFlickThresh(10);
  M5.BtnA.setHoldThresh(800);
  M5.BtnB.setHoldThresh(800);
  setupVibration();

  if (!avatar.begin()) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("Avatar buffer failed", M5.Display.width() / 2,
                          M5.Display.height() / 2);
    Serial.println("Avatar sprite allocation failed");
  }
  wifiPairing.begin(settings.wifiSsid, settings.wifiPassword, millis());

  Serial.println("Expression device started");
  Serial.printf("Speaker: %s\n", soundReady ? "ready" : "unavailable");
  Serial.println(
      "Commands: idle listening thinking happy excited curious confused "
      "angry surprised sad sleepy");
  Serial.println("Playback test: once|loop|pingpong <expression>");
  Serial.println("Hold A+B for hardware diagnostics");
  Serial.println(
      "Hold A: eye menu; swipe down: battery; Wi-Fi page hold A: pair/change network; B: back");
  Serial.println("Sound test: sound");
  Serial.println(
      "Companion commands: status, time [YYYY-MM-DD HH:MM:SS], "
      "brightness 20-255, volume 0-160, dim seconds, screenoff seconds, quiet start end, "
      "wifi [pair|retry|forget], screen on|off");

  lastActivityMs = millis();
  lastPowerSampleMs = lastActivityMs - kPowerSampleIntervalMs;
  lastRtcSampleMs = lastActivityMs - kRtcSampleIntervalMs;
  updateCompanionSensors(lastActivityMs);
  sampleRtc(true);
  printCompanionStatus();
}

void loop() {
  M5.update();
  const uint32_t nowMs = millis();
  updateVibration(nowMs);

  const auto touch = M5.Touch.getDetail(0);
  if (screenPowerState == ScreenPowerState::Sleeping &&
      (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || touch.wasPressed())) {
    noteActivity(nowMs);
  }

  handleDiagnosticToggle();
  updateWifiPairing(nowMs);
  updateCompanionSensors(nowMs);
  handleSerialCommands(nowMs);

  if (diagnosticMode) {
    handleDiagnosticInput(nowMs);
  } else if (statusMode) {
    handleStatusInput(nowMs);
    if (statusMode) {
      avatar.update(nowMs);
    }
  } else if (wifiMode) {
    handleWifiInput(nowMs);
    if (wifiMode) avatar.update(nowMs);
  } else if (eyeMenuMode) {
    handleEyeMenuInput(nowMs);
    if (eyeMenuMode) avatar.update(nowMs);
  } else {
    const bool showMenuRequested =
        M5.BtnA.wasHold() && !M5.BtnB.isPressed();

    if (showMenuRequested) {
      openEyeMenu(nowMs);
    }
    if (!statusMode && !wifiMode && !eyeMenuMode) {
      handleProductTouch(nowMs);
      handleImuInteraction(nowMs);

      if (screenPowerState == ScreenPowerState::Bright) {
        avatar.update(nowMs);
      } else if (screenPowerState == ScreenPowerState::Dimmed &&
                 nowMs - lastDimRenderMs >= kDimRenderIntervalMs) {
        lastDimRenderMs = nowMs;
        avatar.update(nowMs);
      }
    }
  }

  updateDisplayPower(nowMs);

  // A short cooperative yield keeps input responsive without quantizing the
  // 60 fps renderer onto a coarse 5 ms loop cadence.
  delay(1);
}
