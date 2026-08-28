# KK — M5Stack StopWatch Avatar

[English](README.md) | [简体中文](README.zh-CN.md)

[![Build firmware](https://github.com/dsxksss/m5stack-stopwatch-avatar/actions/workflows/build.yml/badge.svg)](https://github.com/dsxksss/m5stack-stopwatch-avatar/actions/workflows/build.yml)
[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](LICENSE)

Meet **KK** — a tiny expressive face living inside the M5Stack StopWatch.

KK is a procedural avatar built for the M5Stack StopWatch's circular AMOLED display. Its eyes, eyelids, brows, keyframes and transitions are drawn in real time with C++, without image-frame animation. The pure-black visual system is optimized for the 466 × 466 circular screen, partial updates and direct interaction.

> Community project. Not affiliated with or endorsed by M5Stack.

## Current firmware

The current firmware version is **0.12.3**. Reading mode includes an on-device bookshelf alongside the existing public HTTPS source. The bundled Chinese novel *Zero Lamp* is split into 42 validated reading units, and KK persists the current chapter, content block and page in NVS. Direction controls are now consistent at both levels: A selects the previous chapter/page and B selects the next chapter/page. The B-release isolation introduced in 0.12.2 remains in place, so dismissing reading cannot reopen the mode menu with the same hold.

Version 0.12.3 was built and flashed to real M5Stack StopWatch hardware with the existing 42-entry SPIFFS library retained. The previous 0.12.2 run confirmed B advancing pages, A returning from `9/22` to `8/22`, and a 1,507 ms B hold dismissing reading without reopening the mode menu. The new chapter-order mapping compiles and runs on-device; its final button feel and public-image backward navigation still need user-facing physical acceptance.

## Highlights

- 11 procedural expressions: `idle`, `listening`, `thinking`, `happy`, `excited`, `curious`, `confused`, `angry`, `surprised`, `sad` and `sleepy`;
- hardware vertical sync from the CO5300 TE signal on GPIO 38, rendering at about 60 fps with dynamic dirty rectangles to reduce AMOLED transfer work;
- tap, double tap, long press, continuous touch tracking, and horizontal/vertical swipes;
- accelerometer and gyroscope fusion for tilt tracking, with the eyes leading, the head following and a centered safe-motion area;
- quick movement adds a brief inertial response while keeping the current expression;
- expressions are selected by touch, motion, battery, charging and network context rather than a user-controlled expression catalogue;
- original soft pop, boop and blip effects are synthesized for expressions, navigation, energy status, brightness and wake events, with automatic quiet-hours muting;
- double-click A+B together to reveal the hidden birthday greeting arranged vertically inside both eyes, or hold A+B to enter hardware diagnostics;
- show long UTF-8 content as full-screen, six-line pages with a Unicode-aware typewriter effect and a procedural transition back to the previous expression;
- hold B to close the current expression into a separate mode menu, then choose the on-device bookshelf or public reading and return smoothly to the expression;
- read the bundled 42-entry *Zero Lamp* library without Wi-Fi, with automatic next-chapter flow and NVS-backed chapter/block/page resume;
- store up to five Wi-Fi profiles, manage them from the captive portal, and select the strongest visible saved network automatically;
- hold A to open a minimal menu made entirely from KK's eyes; inside the menu, click A to browse brightness, sound, motion sensitivity, scheduled quiet mute, network and firmware version, double-click A to confirm, and press B to go back. Short A/B presses on the normal face do not select expressions; swipe down to check battery, while eye opening directly conveys the selected levels;
- dim after 45 seconds of inactivity and clear/switch the AMOLED off after 60 seconds, with touch, button and motion wake;
- persist brightness, motion sensitivity, idle timeouts and quiet hours in NVS;
- synchronize China Standard Time over NTP after Wi-Fi connects, write it to the RX8130 RTC, and use the RTC for quiet-hour sleepy behavior;
- semantic serial commands provide a stable input boundary for future voice recognition or external control.

## Interaction guide

The display gestures and the physical A/B buttons have separate roles. Short A/B presses on the normal face intentionally do nothing, so they cannot accidentally change an expression or erase a saved network.

### Normal face and display gestures

| Input | Result |
| --- | --- |
| Tap the display | Brief `happy` reaction |
| Double-tap the display | Brief `surprised` reaction |
| Hold the display in place for about 650 ms | Brief `angry` reaction |
| Press without crossing the swipe threshold | Eyes and head follow the touch point |
| Swipe right | Brief `curious` reaction |
| Swipe left | Brief `confused` reaction |
| Swipe up | Brief `surprised` reaction |
| Swipe down | Open the immersive battery expression |
| Slowly tilt the device | Gaze follows the tilt inside a restricted centered motion area |
| Move the device quickly | Add a short inertial eye/head response without changing expression |

`idle`, `listening` and `thinking` are persistent base states. Tap, hold, swipe, charging and low-battery reactions are temporary and restore the previous base state automatically.

### Physical buttons and modes

| Current mode | A | B | A+B |
| --- | --- | --- | --- |
| Normal face | Hold for about 800 ms to open the eye menu; a short press does nothing | Hold for about 800 ms to open the mode menu; a short press does nothing | Double-click both together for the vertical birthday greeting; hold both for about one second to enter diagnostics |
| Reading source menu | Single-click to switch between on-device and public sources; double-click to enter | Close the menu and restore the previous face | Hold both for about one second to enter diagnostics |
| On-device chapter menu | Single-click for the previous chapter; double-click to read/resume | Single-click for the next chapter; hold to return to reading sources | — |
| Public source page | Fetch/read the configured source or wait for LAN delivery | Return to reading sources; hold to close | — |
| Root eye menu | Single-click to move to the next item; double-click to open the selected item | Close the menu and restore the previous face | — |
| Brightness page | Single-click to cycle through four levels | Return to the root menu | — |
| Sound page | Single-click to cycle through mute plus four volume levels | Return to the root menu | — |
| Motion sensitivity page | Single-click to cycle through low, medium and high | Return to the root menu | — |
| Scheduled quiet page | Single-click to toggle scheduled mute | Return to the root menu | — |
| Network page | Hold for about 1.8 seconds to open saved-network management; a short press only refreshes status | Cancel an active portal and return to the root menu | — |
| Version page | Read-only; a click only gives haptic feedback | Return to the root menu | — |
| Battery view | No setting action | Close early | — |
| Full-screen narrative text | Previous page or content block | Reveal/next page or block; hold to dismiss | Hold both for about one second to enter diagnostics |
| Public image page | Previous content block | Next content block; hold to dismiss | — |
| Diagnostics | Test vibration | Redraw the diagnostic screen | Hold both for about one second to return to the face |

A single A click in the root menu is committed after the 420 ms double-click window. This small delay lets a second click open the current item instead of advancing it. Touch remains available inside eye-menu pages for gaze following, but it does not alter settings.

### Eye menu pages

The menu stays within KK's expression: the item name is drawn in the left eye and its value or position is drawn in the right eye.

1. **Brightness (`1/6`)** — A cycles display brightness through `60`, `100`, `150` and `220`. The right-eye opening represents the selected `1/4–4/4` level.
2. **Sound (`2/6`)** — A cycles `0/4–4/4`; `0/4` is mute. The right-eye opening represents the volume level, and non-muted selections play a short preview.
3. **Motion sensitivity (`3/6`)** — A cycles `low`, `medium` and `high`, shown as `低`, `中` or `高` in the right eye. The saved profile controls tilt and quick-movement response; the screen-wake threshold remains conservative and unchanged.
4. **Scheduled quiet (`4/6`)** — A toggles the saved `22:00–07:00` sound mute. It is disabled by default and affects sound only; the RTC-driven sleepy night expression remains independent.
5. **Network (`5/6`)** — Hold A for about 1.8 seconds to start the temporary `KK-XXXX` access point. The captive portal lists saved networks and lets you add/update or delete them, up to five profiles. New credentials are saved only after a successful connection; passwords are never echoed. During normal startup or recovery KK scans and selects the strongest visible saved profile.
6. **Version (`6/6`)** — Displays firmware version `0.12.3`; it does not change a setting.

### Battery, automatic reactions and screen power

- Swipe down to show a battery-aware expression. The left eye reports charging or a short energy description, the right eye shows the measured percentage, and eye opening also represents the level.
- After about 3.4 seconds the battery text exits with a blink and head shake; the previous centered face returns about 0.9 seconds later. Tap the display or press B to close it early.
- Connecting power triggers `excited`; disconnecting power triggers `curious`. At 15% or below, KK gives a periodic `sleepy` reminder while not charging.
- After 45 seconds without activity, the display dims to at most `24/255` and rendering drops to 20 fps. After 60 seconds, the AMOLED is cleared and switched off completely.
- Touch, either physical button or confirmed device movement wakes the display. The eye menu, battery view and diagnostics stay awake while in use.
- In the configured night window, entering the dim state may use the sleepy expression. The eye-menu scheduled-mute switch controls only sound during that window and is off by default.

### On-device library, network reading and full-screen narrative text

Hold B on the normal face. In Reading mode, single-click A to switch between the on-device bookshelf and the public source, then double-click A to enter. The bundled *Zero Lamp* library works offline. Inside its chapter list, single-click A moves to the previous chapter, single-click B moves to the next chapter, and double-click A reads or resumes; holding B returns to the source list. KK saves the chapter, content block and page, advances to the next chapter after the current unit, and resumes at the saved page after leaving or rebooting.

After KK connects to Wi-Fi, open `http://<device-ip>/read` from another device on the same local network. You can still send one volatile article, or save the public HTTPS URL of a `KKREAD/1` manifest. Hold B and press A in Reading mode to let KK fetch that source itself. The local configuration endpoint has no authentication, so use it only on a trusted LAN and never expose port 80; remote manifests and image blocks are fetched with certificate-validated HTTPS.

One manifest may contain up to 12 ordered blocks. Text blocks keep the 1,800-character/96-line limits and the existing typewriter renderer. Image blocks must be absolute HTTPS JPEG/PNG URLs and are capped at 768 KiB; they are aspect-fitted onto the black round display. A moves to the previous page or block, short B moves forward, and a display tap reveals or advances; holding B for 1.5 seconds exits. Backward navigation crosses content-block and local-chapter boundaries and opens the last page of the preceding text block. See [`docs/READING_SOURCES.md`](docs/READING_SOURCES.md) for the format.

Send `say <UTF-8 text>` over the serial monitor to enter narrative mode. The command preserves the message's original case, supports Chinese and explicit newlines, and accepts up to 768 bytes from the serial boundary. The renderer wraps by Unicode character into the circular screen's central safe area, uses at most six lines per page, and automatically creates additional pages.

The current expression first closes its eyes and brows into black. Each character then brightens into place at a 62 ms interval. Multi-page content shows a small gray `current/total` indicator at the bottom center; single-page content omits it. A completed page remains indefinitely. A moves to the previous page with a right-to-left transition; at the first page of reading content it opens the preceding block or local chapter at its last page. Short B reveals the rest of a page while typing, then advances with a left-to-right transition; a display tap follows the same reveal/advance behavior. Standalone serial text still cycles from its final page to its first because it has no surrounding content blocks. Holding B for 1.5 seconds dismisses the sequence and reopens the eyes into the expression that continued running underneath. Narrative text keeps the display awake and blocks charging or low-battery reactions until the expression has returned.

## Hardware

- [M5Stack StopWatch Dev Kit (C152)](https://docs.m5stack.com/en/core/StopWatch)
- ESP32-S3R8, 16 MB Flash, 8 MB PSRAM
- 1.75-inch 466 × 466 circular AMOLED touch display
- BMI270 six-axis IMU
- CST820B touch controller
- Two programmable buttons and an internal vibration motor

See [Hardware baseline](docs/HARDWARE_BASELINE.md) for interfaces, addresses and the current verification boundary.

## Build

Requirements:

- [PlatformIO Core](https://platformio.org/) 6.1.18
- USB-C data cable
- M5Stack StopWatch

The library commits used by the verified build are pinned in [`platformio.ini`](platformio.ini).

```sh
pio run
```

## Upload and monitor

Connect the StopWatch over USB-C. If automatic upload does not start, hold reset for about two seconds and release it when the green LED turns on.

```sh
pio run --target upload
pio device monitor --baud 115200
```

The monitor accepts expression names such as `happy`, `thinking` or `sleepy`. Playback testing also supports:

```text
once <expression>
loop <expression>
pingpong <expression>
sound
volume 0-160
```

Companion features also accept:

```text
status
time
time 2026-08-26 20:00:00
brightness 150
motion low
motion medium
motion high
dim 60
screenoff 300
quiet 22 7
wifi
wifi pair
wifi retry
wifi forget
screen on
screen off
say 小谷宝贝，今天也要好好休息。
```

Network time uses China Standard Time (`UTC+8`, `Asia/Shanghai`, no daylight saving). KK synchronizes after Wi-Fi connects, retries after one minute on failure, and rewrites the RTC every six hours. Brightness, motion sensitivity, timeouts, quiet settings, up to five Wi-Fi profiles and the public reading-source URL persist in NVS. The temporary `KK-XXXX` portal runs for at most five minutes and never echoes or logs router passwords; a candidate is saved only after a successful connection.

## Repository map

| Path | Purpose |
| --- | --- |
| `src/avatar_engine.*` | Expression catalogue, timelines, easing, drawing and interaction physics |
| `src/main.cpp` | Device setup, touch/IMU/buttons, vibration, diagnostics and serial commands |
| `src/wifi_pairing.*` | Non-blocking station connection, temporary captive portal and credential handoff |
| `src/reading_service.*` | Local reading/source configuration page and validation |
| `src/public_reader.*` | Public HTTPS manifest, text-block and JPEG/PNG retrieval |
| `src/local_library.*` | SPIFFS bookshelf catalogue and local chapter loading |
| `docs/HARDWARE_BASELINE.md` | Hardware capabilities and verification boundary |
| `docs/ENGINEERING_NOTES.md` | Rendering experiments, measurements and implementation decisions |
| `docs/ROADMAP.md` | Planned work and intentionally unsupported features |

## Known limitations

- The microphone and speech/LLM service are not connected yet. Wi-Fi pairing is only the transport foundation; serial commands still simulate semantic voice events.
- KK does not scrape arbitrary web pages or directly execute Legado source rules; public reading uses the documented `KKREAD/1` interchange format.
- Deep sleep and external expansion ports are not integrated. The current power strategy switches off only the AMOLED and keeps input sampling active for quick wake-up.
- The network time zone is currently fixed to China Standard Time; other regions still need a time-zone setting.
- Long-term battery life has not been measured; the default timeouts are a conservative starting point.
- Subjective motion and gesture tuning may vary with how the device is held.

## Inspiration and provenance

This project was inspired by the expression/animation/playback layering of [Bible Strong Avatar Lab](https://github.com/smontlouis/bible-strong-avatar-lab). It is an independent C++ implementation rebuilt for ESP32 hardware and does not bundle the upstream web application, TypeScript source, exported avatar data or visual assets.

The concise relationship is: **Inspired by the architecture, rebuilt for completely different hardware.**

Hardware initialization, pin mapping and IMU screen-axis handling reference M5Stack's official [StopWatch User Demo](https://github.com/m5stack/M5StopWatch-UserDemo). See [Third-party notices](THIRD_PARTY_NOTICES.md) for details.

## Contributing

Issues and pull requests are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) and run `pio run` before submitting a change. Hardware-dependent claims should include real-device evidence when possible.

## License

This project is licensed under the [GNU Affero General Public License v3.0 or later](LICENSE).
