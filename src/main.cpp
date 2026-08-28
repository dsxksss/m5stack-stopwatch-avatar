// SPDX-License-Identifier: AGPL-3.0-or-later

#include <Arduino.h>
#include <M5IOE1.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <time.h>

#include "avatar_engine.h"
#include "local_library.h"
#include "public_reader.h"
#include "reading_service.h"
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
constexpr char kFirmwareVersion[] = "0.12.1";
constexpr char kPreferencesNamespace[] = "kk-avatar";
constexpr uint8_t kSettingsSchemaVersion = 9;
constexpr uint8_t kDefaultBrightness = 150;
constexpr uint8_t kDefaultSoundVolume = 56;
constexpr uint8_t kSoundVolumeLevels[] = {0, 32, 56, 84, 112};
constexpr uint8_t kDimBrightness = 24;
constexpr uint32_t kDefaultDimAfterMs = 45UL * 1000UL;
constexpr uint32_t kDefaultScreenOffAfterMs = 60UL * 1000UL;
constexpr uint32_t kStatusMessageHoldMs = 3400;
constexpr uint32_t kStatusDismissAnimationMs = 900;
constexpr uint32_t kEyeMessageDismissAnimationMs = 900;
constexpr uint32_t kBirthdayMessageHoldMs = 6000;
constexpr uint32_t kBirthdayButtonSyncWindowMs = 180;
constexpr uint32_t kWifiPairingHoldMs = 1800;
constexpr uint32_t kEyeMenuClickWindowMs = 420;
constexpr uint8_t kEyeMenuItemCount = 6;
constexpr uint32_t kPowerSampleIntervalMs = 5000;
constexpr uint32_t kRtcSampleIntervalMs = 30000;
constexpr uint32_t kNetworkTimePollIntervalMs = 250;
constexpr uint32_t kNetworkTimeSyncTimeoutMs = 15UL * 1000UL;
constexpr uint32_t kNetworkTimeRetryIntervalMs = 60UL * 1000UL;
constexpr uint32_t kNetworkTimeResyncIntervalMs = 6UL * 60UL * 60UL * 1000UL;
constexpr char kNetworkTimeZone[] = "CST-8";
constexpr char kPrimaryNtpServer[] = "ntp.aliyun.com";
constexpr char kSecondaryNtpServer[] = "ntp.tencent.com";
constexpr char kFallbackNtpServer[] = "pool.ntp.org";
constexpr uint32_t kDimRenderIntervalMs = 50;
constexpr uint32_t kMotionWakeGraceMs = 1500;
constexpr float kMotionWakeJerkThreshold = 0.30f;
constexpr uint8_t kMotionWakeRequiredSamples = 2;
constexpr uint32_t kLowBatteryReminderIntervalMs = 15UL * 60UL * 1000UL;
constexpr uint8_t kLowBatteryThreshold = 15;
constexpr size_t kSerialCommandMaxBytes = 768;
constexpr uint32_t kNarrativeDismissHoldMs = 1500;
constexpr uint32_t kReadingImageFadeMs = 180;

enum class GestureAxis : uint8_t { None, Horizontal, Vertical };
enum class ScreenPowerState : uint8_t { Bright, Dimmed, Sleeping };
enum class MotionSensitivity : uint8_t { Low, Medium, High };
enum class ModeMenuPage : uint8_t { Sources, LocalChapters, PublicSource };

struct MotionSensitivityProfile {
  const char* name;
  const char* eyeLabel;
  float tiltFullScaleG;
  float gyroDivisor;
  float shakeFloor;
  float shakeRange;
  float shakeDirectionDivisor;
};

constexpr MotionSensitivityProfile kMotionSensitivityProfiles[] = {
    {"low", "低", 0.52f, 360.0f, 0.085f, 0.58f, 0.34f},
    {"medium", "中", 0.38f, 280.0f, 0.055f, 0.48f, 0.28f},
    {"high", "高", 0.28f, 220.0f, 0.035f, 0.36f, 0.22f},
};

enum class EyeMenuPage : uint8_t {
  Root,
  Brightness,
  Sound,
  MotionSensitivity,
  QuietMute,
  Wifi,
  Version,
};

struct CompanionSettings {
  uint8_t brightness = kDefaultBrightness;
  uint8_t soundVolume = kDefaultSoundVolume;
  MotionSensitivity motionSensitivity = MotionSensitivity::Medium;
  WifiProfile wifiProfiles[WifiPairing::kMaxProfiles];
  uint8_t wifiProfileCount = 0;
  String readingSourceUrl;
  uint8_t localChapter = 0;
  uint8_t localBlock = 0;
  uint16_t localPage = 0;
  uint32_t dimAfterMs = kDefaultDimAfterMs;
  uint32_t screenOffAfterMs = kDefaultScreenOffAfterMs;
  uint8_t quietStartHour = 22;
  uint8_t quietEndHour = 7;
  bool quietMuteEnabled = false;
};

struct EyeMessage {
  String left;
  String right;
  uint32_t holdMs = 3400;
  bool vertical = false;
};

M5IOE1 ioe;
AvatarEngine avatar;
ReadingService readingService;
PublicReader publicReader;
LocalLibrary localLibrary;
UiSoundEngine uiSounds;
WifiPairing wifiPairing;
Preferences preferences;
CompanionSettings settings;
bool vibrationReady = false;
bool diagnosticMode = false;
bool diagnosticToggleLatched = false;
bool statusMode = false;
bool eyeMessageMode = false;
bool wifiMode = false;
bool eyeMenuMode = false;
bool modeMenuMode = false;
bool modeMenuInputArmed = false;
bool readingModeWaiting = false;
bool publicFetchRequested = false;
bool publicDocumentActive = false;
bool localDocumentActive = false;
bool publicImageMode = false;
bool publicImageFading = false;
bool publicImageExitAfterFade = false;
bool publicImageRetreatAfterFade = false;
bool publicAdvancePending = false;
bool publicRetreatPending = false;
bool publicImageBPressTracking = false;
bool publicImageBLongTriggered = false;
bool wifiPairingInputArmed = false;
bool wifiPairingHoldLatched = false;
bool eyeMenuInputArmed = false;
bool eyeMenuWifiHoldLatched = false;
bool birthdayADoubleClickPending = false;
bool birthdayBDoubleClickPending = false;
bool serialCommandOverflow = false;
bool narrativeBPressTracking = false;
bool narrativeBLongTriggered = false;
bool rtcValid = false;
bool networkTimeConnected = false;
bool networkTimeAwaiting = false;
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
uint32_t eyeMessageReactionStartsAtMs = 0;
uint32_t eyeMessageDismissesAtMs = 0;
uint32_t birthdayADoubleClickAtMs = 0;
uint32_t birthdayBDoubleClickAtMs = 0;
uint32_t narrativeBPressedAtMs = 0;
uint32_t publicImageBPressedAtMs = 0;
uint32_t publicImageFadeStartedMs = 0;
uint32_t eyeMenuAClickDeadlineMs = 0;
uint32_t eyeMenuPageReadyAtMs = 0;
uint32_t modeMenuAClickDeadlineMs = 0;
uint32_t lastPowerSampleMs = 0;
uint32_t lastRtcSampleMs = 0;
uint32_t networkTimeAttemptStartedMs = 0;
uint32_t lastNetworkTimePollMs = 0;
uint32_t nextNetworkTimeAttemptMs = 0;
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
bool eyeMessageDismissing = false;
uint8_t eyeMenuAClickCount = 0;
uint8_t eyeMenuIndex = 0;
uint8_t modeMenuAClickCount = 0;
uint8_t modeMenuSourceIndex = 0;
uint8_t modeMenuChapterIndex = 0;
uint8_t motionWakeSampleCount = 0;
String lastDiagnosticEvent = "Waiting for input";
String serialCommand;
String queuedReadingText;
String publicImageError;
uint8_t publicBlockIndex = 0;
GestureAxis gestureAxis = GestureAxis::None;
bool gestureCommitted = false;
int32_t batteryLevel = -1;
int16_t batteryVoltageMv = -1;
m5::rtc_datetime_t rtcDateTime;
ScreenPowerState screenPowerState = ScreenPowerState::Bright;
ExpressionId statusReturnExpression = ExpressionId::Idle;
ExpressionId eyeMessageReturnExpression = ExpressionId::Idle;
ExpressionId wifiReturnExpression = ExpressionId::Idle;
ExpressionId eyeMenuReturnExpression = ExpressionId::Idle;
EyeMenuPage eyeMenuPage = EyeMenuPage::Root;
ModeMenuPage modeMenuPage = ModeMenuPage::Sources;

void setFont();
void startVibration(uint8_t strength, uint16_t durationMs);
void renderEyeMenu(uint32_t nowMs);
void closeEyeMenu(uint32_t nowMs);
void closeEyeMessage(uint32_t nowMs, bool restoreExpression = true);
void cancelModeMenuImmediate();
bool startLocalChapter(uint32_t nowMs);
bool startPublicBlock(uint8_t index, uint32_t nowMs,
                      bool openLastPage = false);

bool reached(uint32_t now, uint32_t deadline) {
  return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0;
}

uint32_t clampTimeoutSeconds(int seconds, uint32_t fallbackMs) {
  if (seconds < 10 || seconds > 24 * 60 * 60) return fallbackMs;
  return static_cast<uint32_t>(seconds) * 1000UL;
}

uint8_t motionSensitivityIndex() {
  return std::min<uint8_t>(static_cast<uint8_t>(settings.motionSensitivity),
                           2);
}

const MotionSensitivityProfile& motionSensitivityProfile() {
  return kMotionSensitivityProfiles[motionSensitivityIndex()];
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
  settings.motionSensitivity = static_cast<MotionSensitivity>(
      std::min<uint8_t>(preferences.getUChar("motion_sens", 1), 2));
  const uint8_t storedSchema = preferences.getUChar("schema", 1);
  settings.wifiProfileCount = 0;
  for (uint8_t index = 0; index < WifiPairing::kMaxProfiles; ++index) {
    const String ssidKey = "wifi_s" + String(index);
    const String passwordKey = "wifi_p" + String(index);
    const String ssid = preferences.getString(ssidKey.c_str(), "");
    if (ssid.isEmpty()) continue;
    settings.wifiProfiles[settings.wifiProfileCount].ssid = ssid;
    settings.wifiProfiles[settings.wifiProfileCount].password =
        preferences.getString(passwordKey.c_str(), "");
    ++settings.wifiProfileCount;
  }
  if (settings.wifiProfileCount == 0) {
    const String legacySsid = preferences.getString("wifi_ssid", "");
    if (!legacySsid.isEmpty()) {
      settings.wifiProfiles[0].ssid = legacySsid;
      settings.wifiProfiles[0].password =
          preferences.getString("wifi_pass", "");
      settings.wifiProfileCount = 1;
      Serial.println("Migrated one saved Wi-Fi network into profile storage");
    }
  }
  settings.dimAfterMs = clampTimeoutSeconds(
      preferences.getUInt("dim_sec", kDefaultDimAfterMs / 1000UL),
      kDefaultDimAfterMs);
  settings.screenOffAfterMs = clampTimeoutSeconds(
      preferences.getUInt("off_sec", kDefaultScreenOffAfterMs / 1000UL),
      kDefaultScreenOffAfterMs);
  settings.quietStartHour = preferences.getUChar("quiet_start", 22);
  settings.quietEndHour = preferences.getUChar("quiet_end", 7);
  settings.quietMuteEnabled = preferences.getBool("quiet_mute", false);
  settings.readingSourceUrl = preferences.getString("read_source", "");
  if (!PublicReader::validHttpsUrl(settings.readingSourceUrl)) {
    settings.readingSourceUrl = "";
  }
  settings.localChapter = preferences.getUChar("book_ch", 0);
  settings.localBlock = preferences.getUChar("book_blk", 0);
  settings.localPage = preferences.getUShort("book_page", 0);

  // Version 1 used 60 s for dimming and 300 s for panel sleep. Migrate that
  // exact old default so existing devices now become fully dark at 60 s.
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
  if (!preferences.isKey("motion_sens") ||
      preferences.getUChar("motion_sens") != motionSensitivityIndex()) {
    preferences.putUChar("motion_sens", motionSensitivityIndex());
  }
  if (!preferences.isKey("wifi_count") ||
      preferences.getUChar("wifi_count") != settings.wifiProfileCount) {
    preferences.putUChar("wifi_count", settings.wifiProfileCount);
  }
  for (uint8_t index = 0; index < WifiPairing::kMaxProfiles; ++index) {
    const String ssidKey = "wifi_s" + String(index);
    const String passwordKey = "wifi_p" + String(index);
    const String ssid = index < settings.wifiProfileCount
                            ? settings.wifiProfiles[index].ssid
                            : String();
    const String password = index < settings.wifiProfileCount
                                ? settings.wifiProfiles[index].password
                                : String();
    if (!preferences.isKey(ssidKey.c_str()) ||
        preferences.getString(ssidKey.c_str()) != ssid) {
      preferences.putString(ssidKey.c_str(), ssid);
    }
    if (!preferences.isKey(passwordKey.c_str()) ||
        preferences.getString(passwordKey.c_str()) != password) {
      preferences.putString(passwordKey.c_str(), password);
    }
  }
  preferences.putString(
      "wifi_ssid", settings.wifiProfileCount > 0
                       ? settings.wifiProfiles[0].ssid
                       : String());
  preferences.putString(
      "wifi_pass", settings.wifiProfileCount > 0
                       ? settings.wifiProfiles[0].password
                       : String());
  preferences.putUInt("dim_sec", settings.dimAfterMs / 1000UL);
  preferences.putUInt("off_sec", settings.screenOffAfterMs / 1000UL);
  preferences.putUChar("quiet_start", settings.quietStartHour);
  preferences.putUChar("quiet_end", settings.quietEndHour);
  if (!preferences.isKey("quiet_mute") ||
      preferences.getBool("quiet_mute") != settings.quietMuteEnabled) {
    preferences.putBool("quiet_mute", settings.quietMuteEnabled);
  }
  if (!preferences.isKey("read_source") ||
      preferences.getString("read_source") != settings.readingSourceUrl) {
    preferences.putString("read_source", settings.readingSourceUrl);
  }
  if (!preferences.isKey("book_ch") ||
      preferences.getUChar("book_ch") != settings.localChapter) {
    preferences.putUChar("book_ch", settings.localChapter);
  }
  if (!preferences.isKey("book_blk") ||
      preferences.getUChar("book_blk") != settings.localBlock) {
    preferences.putUChar("book_blk", settings.localBlock);
  }
  if (!preferences.isKey("book_page") ||
      preferences.getUShort("book_page") != settings.localPage) {
    preferences.putUShort("book_page", settings.localPage);
  }
  preferences.putUChar("schema", kSettingsSchemaVersion);
}

void saveLocalReadingProgress() {
  if (!preferences.isKey("book_ch") ||
      preferences.getUChar("book_ch") != settings.localChapter) {
    preferences.putUChar("book_ch", settings.localChapter);
  }
  if (!preferences.isKey("book_blk") ||
      preferences.getUChar("book_blk") != settings.localBlock) {
    preferences.putUChar("book_blk", settings.localBlock);
  }
  if (!preferences.isKey("book_page") ||
      preferences.getUShort("book_page") != settings.localPage) {
    preferences.putUShort("book_page", settings.localPage);
  }
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

bool isQuietTimeWindow() {
  if (!rtcValid || settings.quietStartHour == settings.quietEndHour) {
    return false;
  }
  const uint8_t hour = rtcDateTime.time.hours;
  if (settings.quietStartHour < settings.quietEndHour) {
    return hour >= settings.quietStartHour && hour < settings.quietEndHour;
  }
  return hour >= settings.quietStartHour || hour < settings.quietEndHour;
}

bool isQuietMuteActive() {
  return settings.quietMuteEnabled && isQuietTimeWindow();
}

void playUiSound(UiSound sound, uint8_t variant = 0) {
  if (!uiSounds.ready() || settings.soundVolume == 0 || isQuietMuteActive() ||
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
  if (modeMenuMode || avatar.modeMenuActive()) cancelModeMenuImmediate();
  if (avatar.narrativeTextActive()) avatar.cancelNarrativeText();
  if (eyeMessageMode) closeEyeMessage(nowMs);
  noteActivity(nowMs);
  // A downward swipe switches the main loop into status mode before the
  // normal touch handler can observe finger-up. Release its drag target here
  // so the battery expression recenters instead of remaining below screen.
  avatar.releaseSwipe();
  gestureAxis = GestureAxis::None;
  gestureCommitted = false;
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
  avatar.releaseSwipe();
  gestureAxis = GestureAxis::None;
  gestureCommitted = false;
  avatar.clearEnergyUi();
  avatar.show(statusReturnExpression, millis(), false, 260);
  avatar.invalidate();
  Serial.println("Immersive energy expression closed");
}

void closeEyeMessage(uint32_t nowMs, bool restoreExpression) {
  if (!eyeMessageMode) return;
  eyeMessageMode = false;
  eyeMessageDismissing = false;
  eyeMessageReactionStartsAtMs = 0;
  eyeMessageDismissesAtMs = 0;
  avatar.releaseTouch();
  avatar.releaseSwipe();
  gestureAxis = GestureAxis::None;
  gestureCommitted = false;
  avatar.clearEnergyUi();
  if (restoreExpression) {
    avatar.show(eyeMessageReturnExpression, nowMs, false, 240);
  }
  Serial.println("Eye message closed");
}

void beginEyeMessageDismiss(uint32_t nowMs) {
  if (!eyeMessageMode || eyeMessageDismissing) return;
  eyeMessageDismissing = true;
  eyeMessageReactionStartsAtMs = 0;
  eyeMessageDismissesAtMs = nowMs + kEyeMessageDismissAnimationMs;
  avatar.beginEnergyDismiss(nowMs);
  startVibration(75, 22);
  playUiSound(UiSound::Close);
  Serial.println("Eye message dismissing");
}

void showEyeMessage(const EyeMessage& message, uint32_t nowMs) {
  noteActivity(nowMs);
  avatar.releaseSwipe();
  gestureAxis = GestureAxis::None;
  gestureCommitted = false;
  eyeMessageReturnExpression = avatar.baseExpression();
  eyeMessageMode = true;
  eyeMessageDismissing = false;
  eyeMessageReactionStartsAtMs = nowMs + message.holdMs;
  eyeMessageDismissesAtMs =
      eyeMessageReactionStartsAtMs + kEyeMessageDismissAnimationMs;
  avatar.setEnergyUi(1.0f, false, false);
  avatar.setEyeMessage(message.left, message.right, nowMs,
                       message.holdMs + 180, message.vertical);
  avatar.play(ExpressionId::Listening, nowMs,
              AvatarEngine::PlaybackMode::PingPong, false, 240);
  startVibration(90, 26);
  playUiSound(UiSound::Open);
  Serial.println("Eye message shown");
}

void showBirthdayEasterEgg(uint32_t nowMs) {
  EyeMessage message;
  message.left = "生日快乐";
  message.right = "小谷宝贝";
  message.holdMs = kBirthdayMessageHoldMs;
  message.vertical = true;
  showEyeMessage(message, nowMs);
  Serial.println("Birthday easter egg shown");
}

bool birthdayEasterEggAvailable() {
  return !diagnosticMode && !statusMode && !eyeMessageMode && !wifiMode &&
         !eyeMenuMode && !modeMenuMode && !avatar.modeMenuActive() &&
         !wifiPairing.portalActive() && !publicImageMode &&
         !avatar.narrativeTextActive();
}

bool narrativeTextAvailable() {
  return !diagnosticMode && !statusMode && !eyeMessageMode && !wifiMode &&
         !eyeMenuMode && !modeMenuMode && !avatar.modeMenuActive() &&
         !wifiPairing.portalActive() && !publicImageMode &&
         !avatar.narrativeTextActive();
}

bool beginNarrativeText(const String& text, uint32_t nowMs,
                        bool skipEyeClose, uint16_t initialPage = 0) {
  noteActivity(nowMs);
  if (!avatar.showNarrativeText(text, nowMs, 62, skipEyeClose,
                                initialPage)) {
    Serial.println("Narrative text is empty");
    return false;
  }
  narrativeBPressTracking = false;
  narrativeBLongTriggered = false;
  narrativeBPressedAtMs = 0;
  startVibration(80, 24);
  playUiSound(UiSound::Open);
  return true;
}

bool showNarrativeTextMode(const String& text, uint32_t nowMs) {
  if (!narrativeTextAvailable()) {
    Serial.println("Narrative text unavailable while UI is busy");
    return false;
  }
  return beginNarrativeText(text, nowMs, false);
}

String readingModeAddress() {
  if (!readingService.active()) return "请先连接 Wi-Fi";
  return wifiPairing.localIp() + "/read";
}

String conciseChapterTitle(const String& original) {
  String title = original;
  const String separator = " · ";
  const int separatorAt = title.lastIndexOf(separator);
  if (separatorAt >= 0) {
    title = title.substring(separatorAt + separator.length());
  }
  constexpr uint8_t kMaxGlyphs = 13;
  size_t cursor = 0;
  uint8_t glyphs = 0;
  while (cursor < title.length() && glyphs < kMaxGlyphs) {
    const uint8_t lead = static_cast<uint8_t>(title[cursor]);
    size_t bytes = 1;
    if (lead >= 0xC2 && lead <= 0xDF) bytes = 2;
    if (lead >= 0xE0 && lead <= 0xEF) bytes = 3;
    if (lead >= 0xF0 && lead <= 0xF4) bytes = 4;
    cursor = std::min<size_t>(title.length(), cursor + bytes);
    ++glyphs;
  }
  if (cursor < title.length()) return title.substring(0, cursor) + "…";
  return title;
}

void resetModeMenuClicks() {
  modeMenuAClickCount = 0;
  modeMenuAClickDeadlineMs = 0;
}

void refreshModeMenuContent() {
  if (!modeMenuMode || !avatar.modeMenuActive()) return;

  if (modeMenuPage == ModeMenuPage::Sources) {
    const bool localSelected = modeMenuSourceIndex == 0;
    avatar.setModeMenuContent(localSelected ? "内置书架" : "公网书源",
                              localSelected ? "1/2" : "2/2",
                              "单击 A 换项 · 双击 A 进入");
    return;
  }

  if (modeMenuPage == ModeMenuPage::LocalChapters) {
    if (!localLibrary.available()) {
      avatar.setModeMenuContent("内置书架", "未安装书籍", "按 B 返回");
      return;
    }
    modeMenuChapterIndex = std::min<uint8_t>(
        modeMenuChapterIndex, localLibrary.chapterCount() - 1);
    avatar.setModeMenuContent(
        conciseChapterTitle(localLibrary.chapterTitle(modeMenuChapterIndex)),
        String(modeMenuChapterIndex + 1) + "/" +
            String(localLibrary.chapterCount()),
        "A下 B上 · 双击A阅读");
    return;
  }

  if (publicFetchRequested) {
    avatar.setModeMenuContent("公网书源", "正在更新", "安全连接公网书源");
    return;
  }
  if (readingModeWaiting) {
    avatar.setModeMenuContent("公网书源", "等待长文", readingModeAddress());
    return;
  }
  if (!queuedReadingText.isEmpty()) {
    avatar.setModeMenuContent("公网书源", "已有长文", "按 A 开始阅读");
  } else if (!settings.readingSourceUrl.isEmpty() &&
             wifiPairing.connected()) {
    avatar.setModeMenuContent("公网书源", "已配置", "按 A 更新并阅读");
  } else if (!settings.readingSourceUrl.isEmpty()) {
    avatar.setModeMenuContent("公网书源", "网络未连接",
                              "联网后可更新书源");
  } else if (readingService.active()) {
    avatar.setModeMenuContent("公网书源", "配置书源",
                              wifiPairing.localIp() + "/read");
  } else {
    avatar.setModeMenuContent("公网书源", "网络未连接",
                              "请先在设置中连接 Wi-Fi");
  }
}

void openModeMenu(uint32_t nowMs) {
  noteActivity(nowMs);
  modeMenuMode = true;
  modeMenuInputArmed = false;
  readingModeWaiting = false;
  modeMenuPage = ModeMenuPage::Sources;
  modeMenuSourceIndex = localLibrary.available() ? 0 : 1;
  modeMenuChapterIndex = localLibrary.available()
                             ? std::min<uint8_t>(settings.localChapter,
                                                 localLibrary.chapterCount() - 1)
                             : 0;
  resetModeMenuClicks();
  avatar.showModeMenu("阅读模式", "内置书架", "", "", nowMs);
  refreshModeMenuContent();
  startVibration(120, 34);
  playUiSound(UiSound::Open);
  Serial.println("Mode menu opened: reading");
}

void closeModeMenu(uint32_t nowMs) {
  if (!modeMenuMode && !avatar.modeMenuActive()) return;
  modeMenuMode = false;
  modeMenuInputArmed = false;
  readingModeWaiting = false;
  modeMenuPage = ModeMenuPage::Sources;
  resetModeMenuClicks();
  avatar.dismissModeMenu(nowMs);
  startVibration(85, 22);
  playUiSound(UiSound::Close);
}

void cancelModeMenuImmediate() {
  modeMenuMode = false;
  modeMenuInputArmed = false;
  readingModeWaiting = false;
  modeMenuPage = ModeMenuPage::Sources;
  resetModeMenuClicks();
  avatar.cancelModeMenu();
}

bool startQueuedReading(uint32_t nowMs) {
  if (queuedReadingText.isEmpty()) return false;
  String text = queuedReadingText;
  queuedReadingText = "";
  cancelModeMenuImmediate();
  const bool started = beginNarrativeText(text, nowMs, true);
  if (started) Serial.println("Queued reading started");
  return started;
}

void handleModeMenuInput(uint32_t nowMs) {
  if (!modeMenuMode || !avatar.modeMenuReady()) return;
  if (!modeMenuInputArmed) {
    if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed()) {
      modeMenuInputArmed = true;
    }
    return;
  }
  if (M5.BtnB.wasHold()) {
    noteActivity(nowMs);
    resetModeMenuClicks();
    if (modeMenuPage == ModeMenuPage::Sources) {
      closeModeMenu(nowMs);
    } else {
      modeMenuPage = ModeMenuPage::Sources;
      readingModeWaiting = false;
      refreshModeMenuContent();
      startVibration(75, 20);
      playUiSound(UiSound::Close);
    }
    return;
  }
  if (M5.BtnB.wasClicked()) {
    noteActivity(nowMs);
    resetModeMenuClicks();
    if (modeMenuPage == ModeMenuPage::LocalChapters &&
        localLibrary.available()) {
      modeMenuChapterIndex =
          (modeMenuChapterIndex + localLibrary.chapterCount() - 1) %
          localLibrary.chapterCount();
      refreshModeMenuContent();
      startVibration(70, 18);
      playUiSound(UiSound::Previous);
    } else if (modeMenuPage == ModeMenuPage::Sources) {
      closeModeMenu(nowMs);
    } else {
      modeMenuPage = ModeMenuPage::Sources;
      readingModeWaiting = false;
      refreshModeMenuContent();
      startVibration(75, 20);
      playUiSound(UiSound::Close);
    }
    return;
  }

  const bool aClicked = M5.BtnA.wasClicked();
  if (modeMenuPage != ModeMenuPage::PublicSource) {
    if (!aClicked) {
      if (modeMenuAClickCount == 1 &&
          reached(nowMs, modeMenuAClickDeadlineMs)) {
        resetModeMenuClicks();
        if (modeMenuPage == ModeMenuPage::Sources) {
          modeMenuSourceIndex = (modeMenuSourceIndex + 1) % 2;
        } else if (localLibrary.available()) {
          modeMenuChapterIndex =
              (modeMenuChapterIndex + 1) % localLibrary.chapterCount();
        }
        refreshModeMenuContent();
        startVibration(70, 18);
        playUiSound(UiSound::Next);
      }
      return;
    }

    noteActivity(nowMs);
    if (modeMenuAClickCount == 0 ||
        !reached(nowMs, modeMenuAClickDeadlineMs)) {
      ++modeMenuAClickCount;
    } else {
      modeMenuAClickCount = 1;
    }
    modeMenuAClickDeadlineMs = nowMs + kEyeMenuClickWindowMs;
    if (modeMenuAClickCount < 2) return;
    resetModeMenuClicks();

    if (modeMenuPage == ModeMenuPage::Sources) {
      modeMenuPage = modeMenuSourceIndex == 0
                         ? ModeMenuPage::LocalChapters
                         : ModeMenuPage::PublicSource;
      refreshModeMenuContent();
      startVibration(95, 26);
      playUiSound(UiSound::Confirm);
      return;
    }
    if (startLocalChapter(nowMs)) return;
    refreshModeMenuContent();
    startVibration(80, 55);
    playUiSound(UiSound::Warning);
    return;
  }

  if (!aClicked) return;

  noteActivity(nowMs);
  if (startQueuedReading(nowMs)) return;
  if (!settings.readingSourceUrl.isEmpty()) {
    if (!wifiPairing.connected()) {
      avatar.setModeMenuContent("阅读模式", "网络未连接",
                                "请先在设置中连接 Wi-Fi");
      startVibration(80, 55);
      playUiSound(UiSound::Warning);
      return;
    }
    if (!rtcValid) {
      avatar.setModeMenuContent("阅读模式", "正在校时",
                                "稍后再按 A 更新书源");
      startVibration(80, 40);
      playUiSound(UiSound::Warning);
      return;
    }
    publicFetchRequested = true;
    refreshModeMenuContent();
    startVibration(95, 26);
    playUiSound(UiSound::Confirm);
    Serial.println("Public reading update requested");
    return;
  }
  if (!readingService.active()) {
    avatar.setModeMenuContent("阅读模式", "网络未连接",
                              "请先在设置中连接 Wi-Fi");
    startVibration(80, 55);
    playUiSound(UiSound::Warning);
    return;
  }
  readingModeWaiting = true;
  refreshModeMenuContent();
  startVibration(95, 26);
  playUiSound(UiSound::Confirm);
  Serial.printf("Reading mode waiting: http://%s/read\n",
                wifiPairing.localIp().c_str());
}

void updateReadingService(uint32_t nowMs) {
  const bool wasActive = readingService.active();
  readingService.update(wifiPairing.connected() &&
                        !wifiPairing.portalActive());
  if (wasActive != readingService.active() && modeMenuMode) {
    refreshModeMenuContent();
  }

  String sourceUrl;
  if (readingService.takePendingSourceUrl(sourceUrl)) {
    settings.readingSourceUrl = sourceUrl;
    saveSettings();
    readingService.setSourceUrl(settings.readingSourceUrl);
    noteActivity(nowMs);
    if (modeMenuMode) refreshModeMenuContent();
    startVibration(85, 24);
    playUiSound(UiSound::Confirm);
    Serial.println("Public reading source saved");
  }

  String text;
  if (!readingService.takePendingText(text)) return;
  queuedReadingText = text;
  noteActivity(nowMs);
  if (modeMenuMode && readingModeWaiting) {
    startQueuedReading(nowMs);
  } else if (modeMenuMode) {
    refreshModeMenuContent();
    startVibration(85, 24);
    playUiSound(UiSound::Confirm);
  }
}

void drawPublicReadingMessage(const String& title, const String& detail) {
  const int centerX = M5.Display.width() / 2;
  const int centerY = M5.Display.height() / 2;
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::efontCN_24_b);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString(title, centerX, centerY - 16);
  M5.Display.setFont(&fonts::efontCN_16);
  M5.Display.setTextColor(M5.Display.color565(150, 150, 150), TFT_BLACK);
  M5.Display.drawString(detail, centerX, centerY + 24);
  M5.Display.setTextDatum(top_left);
}

bool publicImageDimensions(const uint8_t* data, size_t length,
                           uint32_t& width, uint32_t& height) {
  width = 0;
  height = 0;
  if (length >= 24 && data[0] == 0x89 && data[1] == 'P' &&
      data[2] == 'N' && data[3] == 'G') {
    width = (static_cast<uint32_t>(data[16]) << 24) |
            (static_cast<uint32_t>(data[17]) << 16) |
            (static_cast<uint32_t>(data[18]) << 8) | data[19];
    height = (static_cast<uint32_t>(data[20]) << 24) |
             (static_cast<uint32_t>(data[21]) << 16) |
             (static_cast<uint32_t>(data[22]) << 8) | data[23];
    return width > 0 && height > 0;
  }
  if (length < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
  size_t offset = 2;
  while (offset + 8 < length) {
    if (data[offset] != 0xFF) {
      ++offset;
      continue;
    }
    while (offset < length && data[offset] == 0xFF) ++offset;
    if (offset >= length) break;
    const uint8_t marker = data[offset++];
    if (marker == 0xD8 || marker == 0xD9 ||
        (marker >= 0xD0 && marker <= 0xD7)) {
      continue;
    }
    if (offset + 1 >= length) break;
    const uint16_t segmentLength =
        (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
    if (segmentLength < 2 || offset + segmentLength > length) break;
    const bool startOfFrame =
        (marker >= 0xC0 && marker <= 0xC3) ||
        (marker >= 0xC5 && marker <= 0xC7) ||
        (marker >= 0xC9 && marker <= 0xCB) ||
        (marker >= 0xCD && marker <= 0xCF);
    if (startOfFrame && segmentLength >= 7) {
      height = (static_cast<uint16_t>(data[offset + 3]) << 8) |
               data[offset + 4];
      width = (static_cast<uint16_t>(data[offset + 5]) << 8) |
              data[offset + 6];
      return width > 0 && height > 0;
    }
    offset += segmentLength;
  }
  return false;
}

void drawPublicImagePage() {
  const uint8_t* data = publicReader.imageData();
  const size_t length = publicReader.imageLength();
  const int screenWidth = M5.Display.width();
  const int screenHeight = M5.Display.height();
  M5.Display.fillScreen(TFT_BLACK);

  bool drawn = false;
  uint32_t imageWidth = 0;
  uint32_t imageHeight = 0;
  if (data && publicImageDimensions(data, length, imageWidth, imageHeight)) {
    const float scale = std::min(
        1.0f, std::min((screenWidth - 28.0f) / imageWidth,
                       (screenHeight - 28.0f) / imageHeight));
    if (publicReader.imageIsPng()) {
      drawn = M5.Display.drawPng(data, length, screenWidth / 2,
                                 screenHeight / 2, 0, 0, 0, 0, scale, 0.0f,
                                 middle_center);
    } else {
      drawn = M5.Display.drawJpg(data, length, screenWidth / 2,
                                 screenHeight / 2, 0, 0, 0, 0, scale, 0.0f,
                                 middle_center);
    }
  }
  if (!drawn) {
    publicImageError = "图片解码失败";
    drawPublicReadingMessage("图片暂不可用", "轻按翻页，长按 B 退出");
    Serial.println("Public reading image decode failed");
  } else {
    Serial.printf("Public reading image displayed: width=%u height=%u\n",
                  static_cast<unsigned>(imageWidth),
                  static_cast<unsigned>(imageHeight));
  }

  M5.Display.setFont(&fonts::efontCN_16);
  M5.Display.setTextDatum(middle_center);
  const String page = String(publicBlockIndex + 1) + "/" +
                      String(publicReader.blockCount());
  const int labelWidth = M5.Display.textWidth(page) + 18;
  M5.Display.fillRoundRect((screenWidth - labelWidth) / 2,
                           screenHeight - 35, labelWidth, 25, 12, TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString(page, screenWidth / 2, screenHeight - 23);
  M5.Display.setTextDatum(top_left);
}

void finishPublicReadingFromBlack(uint32_t nowMs) {
  if (localDocumentActive && avatar.narrativeTextActive()) {
    settings.localPage = avatar.narrativePageIndex();
    saveLocalReadingProgress();
  }
  publicDocumentActive = false;
  localDocumentActive = false;
  publicImageMode = false;
  publicImageFading = false;
  publicImageExitAfterFade = false;
  publicImageRetreatAfterFade = false;
  publicAdvancePending = false;
  publicRetreatPending = false;
  publicImageBPressTracking = false;
  publicImageBLongTriggered = false;
  publicImageError = "";
  publicReader.clear();
  avatar.restoreExpressionFromBlack(nowMs);
  startVibration(55, 18);
  playUiSound(UiSound::Close);
  Serial.println("Public reading closed; expression restoring");
}

bool startPublicBlock(uint8_t index, uint32_t nowMs, bool openLastPage) {
  if (!publicDocumentActive || publicReader.blockCount() == 0) return false;
  publicBlockIndex = index % publicReader.blockCount();
  publicAdvancePending = false;
  publicRetreatPending = false;
  publicImageError = "";
  const ReadingBlock& block = publicReader.block(publicBlockIndex);
  uint16_t initialPage = 0;
  if (localDocumentActive) {
    if (settings.localBlock != publicBlockIndex) {
      settings.localBlock = publicBlockIndex;
      settings.localPage = 0;
      saveLocalReadingProgress();
    }
    initialPage = openLastPage ? UINT16_MAX : settings.localPage;
  } else if (openLastPage) {
    initialPage = UINT16_MAX;
  }
  if (block.type == ReadingBlockType::Text) {
    publicImageMode = false;
    publicReader.releaseImage();
    avatar.cancelNarrativeText();
    const bool started =
        beginNarrativeText(block.content, nowMs, true, initialPage);
    if (started && localDocumentActive && openLastPage) {
      settings.localPage = avatar.narrativePageIndex();
      saveLocalReadingProgress();
    }
    if (!started) finishPublicReadingFromBlack(nowMs);
    return started;
  }

  avatar.cancelNarrativeText();
  publicImageMode = true;
  publicImageFading = false;
  publicImageRetreatAfterFade = false;
  publicImageBPressTracking = false;
  publicImageBLongTriggered = false;
  drawPublicReadingMessage("正在加载图片", String(publicBlockIndex + 1) + "/" +
                                             String(publicReader.blockCount()));
  String error;
  if (!publicReader.downloadImage(publicBlockIndex, error)) {
    publicImageError = error;
    drawPublicReadingMessage("图片加载失败", error);
  } else {
    drawPublicImagePage();
  }
  startVibration(45, 16);
  return true;
}

bool startLocalChapter(uint32_t nowMs) {
  if (!localLibrary.available()) {
    avatar.setModeMenuContent("内置书架", "不可用", "请重新写入设备书库");
    return false;
  }
  const uint8_t chapter = std::min<uint8_t>(
      modeMenuChapterIndex, localLibrary.chapterCount() - 1);
  const bool resume = chapter == settings.localChapter;
  String error;
  if (!localLibrary.loadChapter(chapter, publicReader, error)) {
    avatar.setModeMenuContent("内置书架", "章节读取失败", error);
    Serial.printf("Local chapter rejected: %s\n", error.c_str());
    return false;
  }

  settings.localChapter = chapter;
  settings.localBlock = resume ? settings.localBlock : 0;
  settings.localPage = resume ? settings.localPage : 0;
  if (settings.localBlock >= publicReader.blockCount()) {
    settings.localBlock = 0;
    settings.localPage = 0;
  }
  saveLocalReadingProgress();
  cancelModeMenuImmediate();
  publicDocumentActive = true;
  localDocumentActive = true;
  publicBlockIndex = settings.localBlock;
  M5.Display.fillScreen(TFT_BLACK);
  const bool started = startPublicBlock(publicBlockIndex, nowMs);
  if (started) {
    Serial.printf("Local reading started: chapter=%u block=%u page=%u\n",
                  static_cast<unsigned>(settings.localChapter + 1),
                  static_cast<unsigned>(settings.localBlock + 1),
                  static_cast<unsigned>(settings.localPage + 1));
  }
  return started;
}

void advancePublicBlock(uint32_t nowMs) {
  if (!publicDocumentActive || publicReader.blockCount() == 0) return;
  if (!localDocumentActive) {
    startPublicBlock((publicBlockIndex + 1) % publicReader.blockCount(), nowMs);
    return;
  }

  const uint8_t nextBlock = publicBlockIndex + 1;
  if (nextBlock < publicReader.blockCount()) {
    settings.localBlock = nextBlock;
    settings.localPage = 0;
    saveLocalReadingProgress();
    startPublicBlock(nextBlock, nowMs);
    return;
  }

  const uint8_t nextChapter =
      (settings.localChapter + 1) % localLibrary.chapterCount();
  String error;
  if (!localLibrary.loadChapter(nextChapter, publicReader, error)) {
    Serial.printf("Local next chapter failed: %s\n", error.c_str());
    finishPublicReadingFromBlack(nowMs);
    return;
  }
  settings.localChapter = nextChapter;
  settings.localBlock = 0;
  settings.localPage = 0;
  modeMenuChapterIndex = nextChapter;
  saveLocalReadingProgress();
  startPublicBlock(0, nowMs);
  Serial.printf("Local reading advanced: chapter=%u\n",
                static_cast<unsigned>(nextChapter + 1));
}

void retreatPublicBlock(uint32_t nowMs) {
  if (!publicDocumentActive || publicReader.blockCount() == 0) return;
  if (publicBlockIndex > 0) {
    const uint8_t previousBlock = publicBlockIndex - 1;
    if (localDocumentActive) {
      settings.localBlock = previousBlock;
      settings.localPage = 0;
      saveLocalReadingProgress();
    }
    startPublicBlock(previousBlock, nowMs, true);
    return;
  }

  if (!localDocumentActive) {
    startPublicBlock(publicReader.blockCount() - 1, nowMs, true);
    return;
  }

  const uint8_t previousChapter =
      (settings.localChapter + localLibrary.chapterCount() - 1) %
      localLibrary.chapterCount();
  String error;
  if (!localLibrary.loadChapter(previousChapter, publicReader, error)) {
    Serial.printf("Local previous chapter failed: %s\n", error.c_str());
    finishPublicReadingFromBlack(nowMs);
    return;
  }
  settings.localChapter = previousChapter;
  settings.localBlock = publicReader.blockCount() - 1;
  settings.localPage = 0;
  modeMenuChapterIndex = previousChapter;
  saveLocalReadingProgress();
  startPublicBlock(settings.localBlock, nowMs, true);
  Serial.printf("Local reading retreated: chapter=%u block=%u\n",
                static_cast<unsigned>(previousChapter + 1),
                static_cast<unsigned>(settings.localBlock + 1));
}

void updatePublicReadingSource(uint32_t nowMs) {
  if (!publicFetchRequested) return;
  publicFetchRequested = false;
  if (!modeMenuMode || !wifiPairing.connected() ||
      settings.readingSourceUrl.isEmpty()) {
    refreshModeMenuContent();
    return;
  }
  String error;
  if (!publicReader.fetchDocument(settings.readingSourceUrl, error)) {
    avatar.setModeMenuContent("阅读模式", "更新失败", error);
    startVibration(80, 65);
    playUiSound(UiSound::Warning);
    Serial.println("Public reading manifest rejected");
    return;
  }

  cancelModeMenuImmediate();
  publicDocumentActive = true;
  localDocumentActive = false;
  publicBlockIndex = 0;
  M5.Display.fillScreen(TFT_BLACK);
  startPublicBlock(0, nowMs);
  Serial.println("Public reading started");
}

void beginPublicImageFade(uint32_t nowMs, bool exitAfterFade,
                          bool retreatAfterFade = false) {
  if (!publicImageMode || publicImageFading) return;
  publicImageFading = true;
  publicImageExitAfterFade = exitAfterFade;
  publicImageRetreatAfterFade = retreatAfterFade;
  publicImageFadeStartedMs = nowMs;
}

void handlePublicImageInput(uint32_t nowMs) {
  if (!publicImageMode) return;
  const auto touch = M5.Touch.getDetail(0);
  if (M5.BtnB.wasPressed()) {
    publicImageBPressTracking = true;
    publicImageBLongTriggered = false;
    publicImageBPressedAtMs = nowMs;
    noteActivity(nowMs);
  }
  if (publicImageBPressTracking && !publicImageBLongTriggered &&
      M5.BtnB.isPressed() &&
      nowMs - publicImageBPressedAtMs >= kNarrativeDismissHoldMs) {
    publicImageBLongTriggered = true;
    beginPublicImageFade(nowMs, true);
    startVibration(75, 24);
    playUiSound(UiSound::Close);
    return;
  }
  const bool bReleased = M5.BtnB.wasReleased();
  const bool bShortPress = bReleased && publicImageBPressTracking &&
                           !publicImageBLongTriggered;
  if (bReleased) {
    publicImageBPressTracking = false;
    publicImageBLongTriggered = false;
    publicImageBPressedAtMs = 0;
  }
  const bool aShortPress = M5.BtnA.wasClicked();
  if (!publicImageFading &&
      (aShortPress || bShortPress || touch.wasClicked())) {
    noteActivity(nowMs);
    beginPublicImageFade(nowMs, false, aShortPress);
    startVibration(45, 16);
    playUiSound(aShortPress ? UiSound::Previous : UiSound::Next);
  }
}

void updatePublicImage(uint32_t nowMs) {
  if (!publicImageMode || !publicImageFading) return;
  const uint32_t elapsed = nowMs - publicImageFadeStartedMs;
  const int width = M5.Display.width();
  const int eraseWidth = std::min<int>(
      width, static_cast<int>((static_cast<uint64_t>(width) * elapsed) /
                              kReadingImageFadeMs));
  const int eraseX = publicImageRetreatAfterFade ? width - eraseWidth : 0;
  M5.Display.fillRect(eraseX, 0, eraseWidth, M5.Display.height(), TFT_BLACK);
  if (elapsed < kReadingImageFadeMs) return;
  M5.Display.fillScreen(TFT_BLACK);
  if (publicImageExitAfterFade) {
    finishPublicReadingFromBlack(nowMs);
  } else {
    publicImageMode = false;
    publicImageFading = false;
    if (publicImageRetreatAfterFade) {
      retreatPublicBlock(nowMs);
    } else {
      advancePublicBlock(nowMs);
    }
  }
}

void updateBirthdayEasterEgg(uint32_t nowMs) {
  if (!birthdayEasterEggAvailable()) {
    birthdayADoubleClickPending = false;
    birthdayBDoubleClickPending = false;
    return;
  }

  if (M5.BtnA.wasDoubleClicked()) {
    birthdayADoubleClickPending = true;
    birthdayADoubleClickAtMs = nowMs;
  }
  if (M5.BtnB.wasDoubleClicked()) {
    birthdayBDoubleClickPending = true;
    birthdayBDoubleClickAtMs = nowMs;
  }

  if (birthdayADoubleClickPending && birthdayBDoubleClickPending &&
      nowMs - birthdayADoubleClickAtMs <= kBirthdayButtonSyncWindowMs &&
      nowMs - birthdayBDoubleClickAtMs <= kBirthdayButtonSyncWindowMs) {
    birthdayADoubleClickPending = false;
    birthdayBDoubleClickPending = false;
    showBirthdayEasterEgg(nowMs);
    return;
  }

  if (birthdayADoubleClickPending &&
      nowMs - birthdayADoubleClickAtMs > kBirthdayButtonSyncWindowMs) {
    birthdayADoubleClickPending = false;
  }
  if (birthdayBDoubleClickPending &&
      nowMs - birthdayBDoubleClickAtMs > kBirthdayButtonSyncWindowMs) {
    birthdayBDoubleClickPending = false;
  }
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
      rightText = "管理";
      expression = ExpressionId::Curious;
      break;
    case WifiPairing::State::Connecting:
      leftText = "连接";
      eyeLevel = 0.58f;
      expression = ExpressionId::Thinking;
      break;
    case WifiPairing::State::Portal:
      leftText = String(wifiPairing.accessPointPassword()).substring(0, 4);
      rightText = String(wifiPairing.accessPointPassword()).substring(4);
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
  if (modeMenuMode || avatar.modeMenuActive()) cancelModeMenuImmediate();
  if (avatar.narrativeTextActive()) avatar.cancelNarrativeText();
  if (eyeMessageMode) closeEyeMessage(nowMs);
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

void startWifiPairingPortal(uint32_t nowMs) {
  readingService.stop();
  wifiPairing.startPortal(nowMs);
}

void updateWifiPairing(uint32_t nowMs) {
  wifiPairing.update(nowMs);
  WifiProfile updatedProfiles[WifiPairing::kMaxProfiles];
  uint8_t updatedProfileCount = 0;
  if (wifiPairing.takeProfilesChanged(updatedProfiles,
                                      updatedProfileCount)) {
    settings.wifiProfileCount = updatedProfileCount;
    for (uint8_t index = 0; index < WifiPairing::kMaxProfiles; ++index) {
      settings.wifiProfiles[index] =
          index < updatedProfileCount ? updatedProfiles[index]
                                      : WifiProfile{};
    }
    saveSettings();
    Serial.printf("Wi-Fi profile storage updated: count=%u\n",
                  settings.wifiProfileCount);
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

void beginNetworkTimeSync(uint32_t nowMs) {
  configTzTime(kNetworkTimeZone, kPrimaryNtpServer, kSecondaryNtpServer,
               kFallbackNtpServer);
  networkTimeAwaiting = true;
  networkTimeAttemptStartedMs = nowMs;
  lastNetworkTimePollMs = nowMs - kNetworkTimePollIntervalMs;
  nextNetworkTimeAttemptMs = 0;
  Serial.println("Network time sync started: timezone=Asia/Shanghai (UTC+8)");
}

void updateNetworkTime(uint32_t nowMs) {
  if (!wifiPairing.connected()) {
    networkTimeConnected = false;
    networkTimeAwaiting = false;
    networkTimeAttemptStartedMs = 0;
    nextNetworkTimeAttemptMs = 0;
    return;
  }

  if (!networkTimeConnected) {
    networkTimeConnected = true;
    beginNetworkTimeSync(nowMs);
  }

  if (!networkTimeAwaiting) {
    if (reached(nowMs, nextNetworkTimeAttemptMs)) beginNetworkTimeSync(nowMs);
    return;
  }

  if (nowMs - lastNetworkTimePollMs < kNetworkTimePollIntervalMs) return;
  lastNetworkTimePollMs = nowMs;

  tm calendar = {};
  if (getLocalTime(&calendar, 0) && calendar.tm_year + 1900 >= 2024 &&
      calendar.tm_year + 1900 <= 2099) {
    M5.Rtc.setDateTime(&calendar);
    sampleRtc();
    if (rtcValid) {
      networkTimeAwaiting = false;
      networkTimeAttemptStartedMs = 0;
      nextNetworkTimeAttemptMs = nowMs + kNetworkTimeResyncIntervalMs;
      Serial.printf("Network time synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                    rtcDateTime.date.year, rtcDateTime.date.month,
                    rtcDateTime.date.date, rtcDateTime.time.hours,
                    rtcDateTime.time.minutes, rtcDateTime.time.seconds);
      return;
    }
  }

  if (nowMs - networkTimeAttemptStartedMs >= kNetworkTimeSyncTimeoutMs) {
    networkTimeAwaiting = false;
    networkTimeAttemptStartedMs = 0;
    nextNetworkTimeAttemptMs = nowMs + kNetworkTimeRetryIntervalMs;
    Serial.println("Network time sync timed out; retrying in 60 seconds");
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
      startWifiPairingPortal(nowMs);
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
      avatar.setEyeMessage("长按", "管理", nowMs, 1500);
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

void setMotionSensitivity(MotionSensitivity sensitivity, uint32_t nowMs,
                          bool refreshStatus = true) {
  settings.motionSensitivity = sensitivity;
  saveSettings();
  shakeEnergy = 0.0f;
  avatar.setShakeTarget(0.0f, 0.0f, 0.0f);
  const uint8_t index = motionSensitivityIndex();
  startVibration(85 + index * 15, 24 + index * 5);
  if (refreshStatus) showStatus(nowMs);
  playUiSound(UiSound::Brightness, index);
  const MotionSensitivityProfile& profile = motionSensitivityProfile();
  Serial.printf(
      "Motion sensitivity saved: %s (tilt=%.2fg gyro=%.0f shake=%.3f/%.2f dir=%.2f)\n",
      profile.name, profile.tiltFullScaleG, profile.gyroDivisor,
      profile.shakeFloor, profile.shakeRange, profile.shakeDirectionDivisor);
}

void cycleMotionSensitivity(uint32_t nowMs, bool refreshStatus = true) {
  const uint8_t nextIndex = (motionSensitivityIndex() + 1) % 3;
  setMotionSensitivity(static_cast<MotionSensitivity>(nextIndex), nowMs,
                       refreshStatus);
}

void toggleQuietMute(uint32_t nowMs) {
  settings.quietMuteEnabled = !settings.quietMuteEnabled;
  saveSettings();
  startVibration(settings.quietMuteEnabled ? 95 : 75,
                 settings.quietMuteEnabled ? 34 : 22);
  playUiSound(settings.quietMuteEnabled ? UiSound::Confirm : UiSound::Close);
  Serial.printf("Scheduled quiet mute saved: %s (%02u:00-%02u:00)\n",
                settings.quietMuteEnabled ? "on" : "off",
                settings.quietStartHour, settings.quietEndHour);
}

void renderEyeMenu(uint32_t nowMs) {
  if (!eyeMenuMode) return;

  avatar.clearEnergyUi();
  String leftText;
  String rightText;
  float eyeLevel = 1.0f;
  // Menu text should remain calm and readable. Curious uses asymmetric
  // ping-pong keyframes that make one eye repeatedly grow and shrink.
  ExpressionId expression = ExpressionId::Idle;

  if (eyeMenuPage == EyeMenuPage::Root) {
    constexpr const char* labels[] = {"亮度", "音量", "灵敏", "夜静", "网络",
                                      "版本"};
    leftText = labels[eyeMenuIndex % kEyeMenuItemCount];
    rightText = String((eyeMenuIndex % kEyeMenuItemCount) + 1) + "/" +
                String(kEyeMenuItemCount);
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
  } else if (eyeMenuPage == EyeMenuPage::MotionSensitivity) {
    const uint8_t level = motionSensitivityIndex();
    leftText = "灵敏";
    rightText = motionSensitivityProfile().eyeLabel;
    eyeLevel = (level + 1) / 3.0f;
    avatar.setEnergyUi(eyeLevel, false, false);
  } else if (eyeMenuPage == EyeMenuPage::QuietMute) {
    leftText = "夜静";
    rightText = settings.quietMuteEnabled ? "开" : "关";
    eyeLevel = settings.quietMuteEnabled ? 0.58f : 1.0f;
    avatar.setEnergyUi(eyeLevel, false, false);
  } else if (eyeMenuPage == EyeMenuPage::Wifi) {
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
        leftText = String(wifiPairing.accessPointPassword()).substring(0, 4);
        rightText = String(wifiPairing.accessPointPassword()).substring(4);
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
  } else {
    leftText = "版本";
    rightText = kFirmwareVersion;
    expression = ExpressionId::Listening;
    avatar.setEnergyUi(1.0f, false, false);
  }

  avatar.setEyeMessage(leftText, rightText, nowMs, 10UL * 60UL * 1000UL);
  avatar.play(expression, nowMs, AvatarEngine::PlaybackMode::PingPong, false,
              220);
  avatar.invalidate();
}

void openEyeMenu(uint32_t nowMs) {
  if (modeMenuMode || avatar.modeMenuActive()) cancelModeMenuImmediate();
  if (avatar.narrativeTextActive()) avatar.cancelNarrativeText();
  if (eyeMessageMode) closeEyeMessage(nowMs);
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
      avatar.setEyeMessage("网络", "管理", nowMs,
                           kWifiPairingHoldMs + 600);
      avatar.invalidate();
    }
    if (M5.BtnA.wasReleased()) eyeMenuWifiHoldLatched = false;
    if (!eyeMenuWifiHoldLatched &&
        M5.BtnA.pressedFor(kWifiPairingHoldMs)) {
      eyeMenuWifiHoldLatched = true;
      noteActivity(nowMs);
      if (!wifiPairing.portalActive()) {
        startWifiPairingPortal(nowMs);
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
      eyeMenuIndex = (eyeMenuIndex + 1) % kEyeMenuItemCount;
      startVibration(85, 22);
      playUiSound(UiSound::Next);
      renderEyeMenu(nowMs);
      Serial.printf("Eye menu selection: %u/%u\n", eyeMenuIndex + 1,
                    kEyeMenuItemCount);
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
  if (eyeMenuPage == EyeMenuPage::MotionSensitivity) {
    cycleMotionSensitivity(nowMs, false);
    renderEyeMenu(nowMs);
    return;
  }
  if (eyeMenuPage == EyeMenuPage::QuietMute) {
    toggleQuietMute(nowMs);
    renderEyeMenu(nowMs);
    return;
  }
  if (eyeMenuPage == EyeMenuPage::Version) {
    startVibration(55, 16);
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
      if (!diagnosticMode && !statusMode && !eyeMessageMode && !wifiMode &&
          !eyeMenuMode && !modeMenuMode && !avatar.modeMenuActive() &&
          !avatar.narrativeTextActive() && !publicImageMode &&
          screenPowerState != ScreenPowerState::Sleeping) {
        trigger(charging ? ExpressionId::Excited : ExpressionId::Curious,
                nowMs, 220);
      }
    }
  }

  const bool lowBattery = batteryLevel >= 0 &&
                          batteryLevel <= kLowBatteryThreshold && !charging;
  if (lowBattery && !diagnosticMode && !statusMode && !eyeMessageMode &&
      !wifiMode && !eyeMenuMode && !modeMenuMode &&
      !avatar.modeMenuActive() && !avatar.narrativeTextActive() &&
      !publicImageMode &&
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
  if (wifiMode || eyeMenuMode || modeMenuMode || avatar.modeMenuActive()) {
    lastActivityMs = nowMs;
    return;
  }
  if (diagnosticMode || statusMode || eyeMessageMode ||
      avatar.narrativeTextActive() || publicImageMode) {
    return;
  }
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
      if (isQuietTimeWindow()) {
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
      "Status: version=%s time=%s date=%s battery=%ld%% voltage=%dmV charging=%s "
      "brightness=%u volume=%u motion=%s dim=%lus off=%lus quiet=%s/%02u-%02u "
      "wifi=%s ip=%s\n",
      kFirmwareVersion, formattedTime().c_str(), formattedDate().c_str(),
      static_cast<long>(batteryLevel), batteryVoltageMv,
      chargeReadingValid ? (charging ? "yes" : "no") : "unknown",
      settings.brightness, settings.soundVolume,
      motionSensitivityProfile().name,
      static_cast<unsigned long>(settings.dimAfterMs / 1000UL),
      static_cast<unsigned long>(settings.screenOffAfterMs / 1000UL),
      settings.quietMuteEnabled ? "on" : "off",
      settings.quietStartHour, settings.quietEndHour,
      wifiPairing.stateName(), wifiPairing.localIp().c_str());
  Serial.printf("Wi-Fi detail: profiles=%u state=%s ip=%s\n",
                settings.wifiProfileCount, wifiPairing.stateName(),
                wifiPairing.localIp().c_str());
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
  String commandPrefix = command.substring(0, std::min<size_t>(4, command.length()));
  commandPrefix.toLowerCase();
  if (commandPrefix == "say ") {
    String text = command.substring(4);
    text.trim();
    if (text.isEmpty()) {
      Serial.println("Usage: say <UTF-8 text>");
    } else {
      showNarrativeTextMode(text, nowMs);
    }
    return true;
  }
  command.toLowerCase();

  if (command == "status") {
    printCompanionStatus();
    showStatus(nowMs);
    return true;
  }
  if (command == "birthday") {
    if (birthdayEasterEggAvailable()) {
      showBirthdayEasterEgg(nowMs);
    } else {
      Serial.println("Birthday easter egg unavailable while UI is busy");
    }
    return true;
  }
  if (command == "sound" || command == "sound test") {
    playUiSound(UiSound::Confirm);
    const char* result = !uiSounds.ready()      ? "unavailable"
                         : settings.soundVolume == 0 ? "muted"
                         : isQuietMuteActive()      ? "quiet hours"
                                                    : "played";
    Serial.printf("Sound test: %s\n", result);
    return true;
  }
  if (command == "motion") {
    const MotionSensitivityProfile& profile = motionSensitivityProfile();
    Serial.printf(
        "Motion sensitivity: %s (tilt=%.2fg gyro=%.0f shake=%.3f/%.2f dir=%.2f)\n",
        profile.name, profile.tiltFullScaleG, profile.gyroDivisor,
        profile.shakeFloor, profile.shakeRange,
        profile.shakeDirectionDivisor);
    return true;
  }
  if (command.startsWith("motion ")) {
    const String level = command.substring(7);
    if (level == "low") {
      setMotionSensitivity(MotionSensitivity::Low, nowMs);
    } else if (level == "medium") {
      setMotionSensitivity(MotionSensitivity::Medium, nowMs);
    } else if (level == "high") {
      setMotionSensitivity(MotionSensitivity::High, nowMs);
    } else {
      Serial.println("Usage: motion low|medium|high");
    }
    return true;
  }
  if (command == "time") {
    sampleRtc(true);
    return true;
  }
  if (command.startsWith("time ")) return setRtcFromCommand(command);
  if (command == "read library") {
    Serial.printf(
        "Local library: available=%s title=%s chapters=%u progress=%u/%u/%u\n",
        localLibrary.available() ? "yes" : "no",
        localLibrary.available() ? localLibrary.bookTitle().c_str() : "none",
        static_cast<unsigned>(localLibrary.chapterCount()),
        static_cast<unsigned>(settings.localChapter + 1),
        static_cast<unsigned>(settings.localBlock + 1),
        static_cast<unsigned>(settings.localPage + 1));
    return true;
  }
  if (command == "read local" || command.startsWith("read local ")) {
    if (!localLibrary.available()) {
      Serial.println("Local library is not available");
      return true;
    }
    if (modeMenuMode || avatar.modeMenuActive()) cancelModeMenuImmediate();
    if (statusMode) hideStatus();
    if (!narrativeTextAvailable()) {
      Serial.println("Local reading unavailable while UI is busy");
      return true;
    }
    uint8_t chapter = settings.localChapter;
    if (command.startsWith("read local ")) {
      const int requested = command.substring(11).toInt();
      if (requested < 1 || requested > localLibrary.chapterCount()) {
        Serial.println("Local chapter is out of range");
        return true;
      }
      chapter = requested - 1;
    }
    modeMenuChapterIndex = chapter;
    startLocalChapter(nowMs);
    return true;
  }
  if (command == "read source") {
    Serial.printf("Public reading source: %s\n",
                  settings.readingSourceUrl.isEmpty() ? "not configured"
                                                       : "configured");
    return true;
  }
  if (command == "read fetch") {
    if (settings.readingSourceUrl.isEmpty()) {
      Serial.println("Public reading source is not configured");
      return true;
    }
    if (!wifiPairing.connected()) {
      Serial.println("Public reading fetch requires Wi-Fi");
      return true;
    }
    if (!narrativeTextAvailable()) {
      Serial.println("Public reading fetch unavailable while UI is busy");
      return true;
    }
    String error;
    if (!publicReader.fetchDocument(settings.readingSourceUrl, error)) {
      Serial.printf("Public reading fetch rejected: %s\n", error.c_str());
      return true;
    }
    publicDocumentActive = true;
    localDocumentActive = false;
    publicBlockIndex = 0;
    M5.Display.fillScreen(TFT_BLACK);
    startPublicBlock(0, nowMs);
    Serial.println("Public reading started from serial command");
    return true;
  }
  if (command == "read next") {
    if (!publicDocumentActive) {
      Serial.println("Public reading is not active");
    } else if (publicImageMode) {
      beginPublicImageFade(nowMs, false);
      Serial.println("Public reading image advancing");
    } else if (avatar.narrativeHoldingLastPage()) {
      publicAdvancePending = true;
      publicRetreatPending = false;
      avatar.advanceNarrativeTextToBlack(nowMs);
    } else {
      avatar.advanceNarrativeText(nowMs);
    }
    return true;
  }
  if (command == "read previous") {
    if (!publicDocumentActive) {
      Serial.println("Public reading is not active");
    } else if (publicImageMode) {
      beginPublicImageFade(nowMs, false, true);
      Serial.println("Public reading image retreating");
    } else if (avatar.narrativePageIndex() > 0) {
      avatar.retreatNarrativeText(nowMs);
    } else {
      publicRetreatPending = true;
      publicAdvancePending = false;
      avatar.advanceNarrativeTextToBlack(nowMs, true);
    }
    return true;
  }
  if (command == "read close") {
    if (!publicDocumentActive) {
      Serial.println("Public reading is not active");
    } else if (publicImageMode) {
      beginPublicImageFade(nowMs, true);
    } else {
      if (localDocumentActive) {
        settings.localPage = avatar.narrativePageIndex();
        saveLocalReadingProgress();
      }
      publicDocumentActive = false;
      localDocumentActive = false;
      publicAdvancePending = false;
      publicRetreatPending = false;
      publicReader.clear();
      avatar.dismissNarrativeText(nowMs);
    }
    return true;
  }
  if (command == "wifi") {
    Serial.printf("Wi-Fi: state=%s profiles=%u ip=%s ap=%s\n",
                  wifiPairing.stateName(), settings.wifiProfileCount,
                  wifiPairing.localIp().c_str(),
                  wifiPairing.accessPointName().c_str());
    return true;
  }
  if (command == "wifi pair") {
    enterWifiMode(nowMs);
    if (!wifiPairing.portalActive()) {
      startWifiPairingPortal(nowMs);
      wifiPairing.consumeStateChanged();
      renderWifiFace(nowMs);
    }
    return true;
  }
  if (command == "wifi retry") {
    enterWifiMode(nowMs);
    readingService.stop();
    wifiPairing.retry(nowMs);
    wifiPairing.consumeStateChanged();
    renderWifiFace(nowMs);
    return true;
  }
  if (command == "wifi forget") {
    readingService.stop();
    wifiPairing.forget();
    settings.wifiProfileCount = 0;
    for (uint8_t index = 0; index < WifiPairing::kMaxProfiles; ++index) {
      settings.wifiProfiles[index] = WifiProfile{};
    }
    saveSettings();
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
  const MotionSensitivityProfile& motion = motionSensitivityProfile();
  constexpr float kFilterAmount = 0.24f;
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
    // The selected profile changes only input normalization. AvatarEngine still
    // owns the fixed travel limits and safe eye ellipse. Neutral adaptation is
    // deliberately very slow so eyes keep looking in the chosen direction.
    neutralAccelX += (filteredAccelX - neutralAccelX) * 0.00015f;
    neutralAccelY += (filteredAccelY - neutralAccelY) * 0.00015f;
    avatar.setTiltTarget(-(filteredAccelX - neutralAccelX) /
                             motion.tiltFullScaleG,
                         -(filteredAccelY - neutralAccelY) /
                             motion.tiltFullScaleG,
                         -screenGyroY / motion.gyroDivisor,
                         screenGyroX / motion.gyroDivisor);
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
      0.0f,
      std::min(1.0f,
               (shakeEnergy - motion.shakeFloor) / motion.shakeRange));
  const float shakeDirectionX =
      std::max(-1.0f,
               std::min(1.0f, -deltaX / motion.shakeDirectionDivisor));
  const float shakeDirectionY =
      std::max(-1.0f,
               std::min(1.0f, -deltaY / motion.shakeDirectionDivisor));
  avatar.setShakeTarget(shakeDirectionX, shakeDirectionY, shakeIntensity);
}

void handleSerialCommands(uint32_t nowMs) {
  while (Serial.available() > 0) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\n' || character == '\r') {
      if (serialCommand.length() == 0 && !serialCommandOverflow) continue;
      if (serialCommandOverflow) {
        Serial.printf("Command too long; maximum is %u bytes\n",
                      static_cast<unsigned>(kSerialCommandMaxBytes));
        serialCommand = "";
        serialCommandOverflow = false;
        continue;
      }
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
    } else if (serialCommand.length() < kSerialCommandMaxBytes) {
      serialCommand += character;
    } else {
      serialCommandOverflow = true;
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
  if (modeMenuMode || avatar.modeMenuActive()) cancelModeMenuImmediate();
  if (avatar.narrativeTextActive()) avatar.cancelNarrativeText();
  if (eyeMessageMode) closeEyeMessage(millis());
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
    avatar.releaseSwipe();
  }

  if (touch.wasClicked() || M5.BtnB.wasClicked()) {
    noteActivity(nowMs);
    hideStatus();
  }
}

void handleEyeMessageInput(uint32_t nowMs) {
  if (!eyeMessageDismissing &&
      reached(nowMs, eyeMessageReactionStartsAtMs)) {
    beginEyeMessageDismiss(nowMs);
  }
  if (reached(nowMs, eyeMessageDismissesAtMs)) {
    closeEyeMessage(nowMs);
    return;
  }

  if (M5.BtnA.wasHold() && !M5.BtnB.isPressed()) {
    closeEyeMessage(nowMs);
    openEyeMenu(nowMs);
    return;
  }
  if (M5.BtnB.wasClicked()) {
    noteActivity(nowMs);
    beginEyeMessageDismiss(nowMs);
    return;
  }

  const auto touch = M5.Touch.getDetail(0);
  if (touch.wasPressed()) {
    noteActivity(nowMs);
    gestureAxis = GestureAxis::None;
    gestureCommitted = false;
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
    if (gestureAxis == GestureAxis::Vertical) {
      avatar.releaseTouch();
      avatar.setSwipeOffset(0.0f, deltaY, 0);
      if (!gestureCommitted && deltaY >= kGestureCommitPx) {
        gestureCommitted = true;
        showStatus(nowMs);
        return;
      }
    } else if (!eyeMessageDismissing) {
      avatar.setTouchTarget(touch.x, touch.y);
    }
  }

  if (touch.wasReleased()) {
    avatar.releaseTouch();
    avatar.releaseSwipe();
    gestureAxis = GestureAxis::None;
    gestureCommitted = false;
  }
}

void handleNarrativeTextInput(uint32_t nowMs) {
  if (!avatar.narrativeTextActive()) return;
  const auto touch = M5.Touch.getDetail(0);

  if (M5.BtnB.wasPressed()) {
    narrativeBPressTracking = true;
    narrativeBLongTriggered = false;
    narrativeBPressedAtMs = nowMs;
    noteActivity(nowMs);
  }
  if (narrativeBPressTracking && !narrativeBLongTriggered &&
      M5.BtnB.isPressed() &&
      nowMs - narrativeBPressedAtMs >= kNarrativeDismissHoldMs) {
    narrativeBLongTriggered = true;
    noteActivity(nowMs);
    if (publicDocumentActive) {
      if (localDocumentActive) {
        settings.localPage = avatar.narrativePageIndex();
        saveLocalReadingProgress();
      }
      publicDocumentActive = false;
      localDocumentActive = false;
      publicAdvancePending = false;
      publicRetreatPending = false;
      publicReader.clear();
    }
    avatar.dismissNarrativeText(nowMs);
    startVibration(75, 24);
    playUiSound(UiSound::Close);
    Serial.printf("Narrative input: B held %lu ms, dismissing\n",
                  static_cast<unsigned long>(nowMs - narrativeBPressedAtMs));
    return;
  }

  const bool bReleased = M5.BtnB.wasReleased();
  const bool bShortPress = bReleased && narrativeBPressTracking &&
                           !narrativeBLongTriggered;
  if (bReleased) {
    narrativeBPressTracking = false;
    narrativeBLongTriggered = false;
    narrativeBPressedAtMs = 0;
  }

  const bool aShortPress = M5.BtnA.wasClicked();
  if (aShortPress) {
    noteActivity(nowMs);
    if (avatar.narrativePageIndex() > 0) {
      avatar.retreatNarrativeText(nowMs);
    } else if (publicDocumentActive) {
      publicRetreatPending = true;
      publicAdvancePending = false;
      avatar.advanceNarrativeTextToBlack(nowMs, true);
    } else {
      Serial.println("Narrative input: A short press, already at first page");
    }
    startVibration(45, 16);
    playUiSound(UiSound::Previous);
    Serial.println("Narrative input: A short press, retreating");
  }

  if (bShortPress || touch.wasClicked()) {
    noteActivity(nowMs);
    if (publicDocumentActive && avatar.narrativeHoldingLastPage()) {
      publicAdvancePending = true;
      publicRetreatPending = false;
      avatar.advanceNarrativeTextToBlack(nowMs);
    } else {
      avatar.advanceNarrativeText(nowMs);
    }
    startVibration(45, 16);
    playUiSound(UiSound::Next);
    Serial.printf("Narrative input: %s, advancing\n",
                  bShortPress ? "B short press" : "display tap");
  }
}

void syncLocalReadingProgress() {
  if (!localDocumentActive || !publicDocumentActive ||
      !avatar.narrativeTextActive()) {
    return;
  }
  const uint16_t page = avatar.narrativePageIndex();
  if (settings.localBlock == publicBlockIndex &&
      settings.localPage == page) {
    return;
  }
  settings.localBlock = publicBlockIndex;
  settings.localPage = page;
  saveLocalReadingProgress();
  Serial.printf("Local reading progress saved: chapter=%u block=%u page=%u/%u\n",
                static_cast<unsigned>(settings.localChapter + 1),
                static_cast<unsigned>(settings.localBlock + 1),
                static_cast<unsigned>(settings.localPage + 1),
                static_cast<unsigned>(avatar.narrativePageCount()));
}

}  // namespace

void setup() {
  auto config = M5.config();
  config.serial_baudrate = 115200;
  config.internal_spk = true;
  M5.begin(config);
  Serial.begin(115200);
  serialCommand.reserve(kSerialCommandMaxBytes);
  loadSettings();
  const bool localLibraryReady = localLibrary.begin();
  if (localLibraryReady) {
    if (settings.localChapter >= localLibrary.chapterCount()) {
      settings.localChapter = 0;
      settings.localBlock = 0;
      settings.localPage = 0;
      saveLocalReadingProgress();
    }
    modeMenuChapterIndex = settings.localChapter;
  }
  readingService.setSourceUrl(settings.readingSourceUrl);
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
  wifiPairing.begin(settings.wifiProfiles, settings.wifiProfileCount,
                    millis());

  Serial.println("Expression device started");
  Serial.printf("Firmware version: %s\n", kFirmwareVersion);
  Serial.printf("Speaker: %s\n", soundReady ? "ready" : "unavailable");
  Serial.println(
      "Commands: idle listening thinking happy excited curious confused "
      "angry surprised sad sleepy");
  Serial.println("Playback test: once|loop|pingpong <expression>");
  Serial.println("Hold A+B for hardware diagnostics");
  Serial.println(
      "Hold A: eye menu; swipe down: battery; Wi-Fi page hold A: manage saved networks; B: back");
  Serial.println(
      "Hold B: mode menu; reading/source page: http://<device-ip>/read");
  Serial.println(
      "Reading diagnostics: read library|local [chapter]|source|fetch|next|previous|close");
  Serial.println("Sound test: sound");
  Serial.println("Full-screen typewriter: say <UTF-8 text>");
  Serial.println(
      "Companion commands: status, time [YYYY-MM-DD HH:MM:SS], "
      "brightness 20-255, volume 0-160, motion [low|medium|high], dim seconds, "
      "screenoff seconds, quiet start end, "
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
  updateBirthdayEasterEgg(nowMs);
  updateWifiPairing(nowMs);
  updateReadingService(nowMs);
  updateNetworkTime(nowMs);
  updatePublicReadingSource(nowMs);
  updateCompanionSensors(nowMs);
  handleSerialCommands(nowMs);

  if (diagnosticMode) {
    handleDiagnosticInput(nowMs);
  } else if (publicImageMode) {
    handlePublicImageInput(nowMs);
    updatePublicImage(nowMs);
  } else if (avatar.narrativeTextActive()) {
    handleNarrativeTextInput(nowMs);
    const bool wasActive = avatar.narrativeTextActive();
    avatar.update(nowMs);
    syncLocalReadingProgress();
    if (publicRetreatPending && avatar.narrativeReadyForNextBlock()) {
      avatar.cancelNarrativeText();
      retreatPublicBlock(nowMs);
    } else if (publicAdvancePending && avatar.narrativeReadyForNextBlock()) {
      avatar.cancelNarrativeText();
      advancePublicBlock(nowMs);
    } else if (wasActive && !avatar.narrativeTextActive()) {
      startVibration(55, 18);
      playUiSound(UiSound::Close);
    }
  } else if (modeMenuMode || avatar.modeMenuActive()) {
    handleModeMenuInput(nowMs);
    avatar.update(nowMs);
  } else if (statusMode) {
    handleStatusInput(nowMs);
    if (statusMode) {
      avatar.update(nowMs);
    }
  } else if (eyeMessageMode) {
    handleEyeMessageInput(nowMs);
    if (eyeMessageMode) avatar.update(nowMs);
  } else if (wifiMode) {
    handleWifiInput(nowMs);
    if (wifiMode) avatar.update(nowMs);
  } else if (eyeMenuMode) {
    handleEyeMenuInput(nowMs);
    if (eyeMenuMode) avatar.update(nowMs);
  } else {
    const bool showEyeMenuRequested =
        M5.BtnA.wasHold() && !M5.BtnB.isPressed();
    const bool showModeMenuRequested =
        M5.BtnB.wasHold() && !M5.BtnA.isPressed();

    if (showEyeMenuRequested) {
      openEyeMenu(nowMs);
    } else if (showModeMenuRequested) {
      openModeMenu(nowMs);
    }
    if (!statusMode && !wifiMode && !eyeMenuMode && !modeMenuMode &&
        !avatar.modeMenuActive()) {
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
