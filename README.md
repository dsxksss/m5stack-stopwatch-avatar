# KK — M5Stack StopWatch Avatar

[English](README.md) | [简体中文](README.zh-CN.md)

[![Build firmware](https://github.com/dsxksss/m5stack-stopwatch-avatar/actions/workflows/build.yml/badge.svg)](https://github.com/dsxksss/m5stack-stopwatch-avatar/actions/workflows/build.yml)
[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](LICENSE)

Meet **KK** — a tiny expressive face living inside the M5Stack StopWatch.

KK is a procedural avatar built for the M5Stack StopWatch's circular AMOLED display. Its eyes, eyelids, brows, keyframes and transitions are drawn in real time with C++, without image-frame animation. The pure-black visual system is optimized for the 466 × 466 circular screen, partial updates and direct interaction.

> Community project. Not affiliated with or endorsed by M5Stack.

## Current firmware

The current firmware version is **0.6.2**. This release fixes the battery-view swipe lifecycle: after a downward swipe, KK now releases the drag offset as soon as the battery expression opens and defensively resets it again when the view closes. The eyes return to the centered base expression instead of remaining below the screen.

Version 0.6.2 was built, flashed and exercised on real M5Stack StopWatch hardware. The battery view completed repeated automatic dismissals at about 59–60 fps with 100% TE synchronization and no recurring frame timeout.

## Highlights

- 11 procedural expressions: `idle`, `listening`, `thinking`, `happy`, `excited`, `curious`, `confused`, `angry`, `surprised`, `sad` and `sleepy`;
- hardware vertical sync from the CO5300 TE signal on GPIO 38, rendering at about 60 fps with dynamic dirty rectangles to reduce AMOLED transfer work;
- tap, double tap, long press, continuous touch tracking, and horizontal/vertical swipes;
- accelerometer and gyroscope fusion for tilt tracking, with the eyes leading, the head following and a centered safe-motion area;
- quick movement adds a brief inertial response while keeping the current expression;
- expressions are selected by touch, motion, battery, charging and network context rather than a user-controlled expression catalogue;
- original soft pop, boop and blip effects are synthesized for expressions, navigation, energy status, brightness and wake events, with automatic quiet-hours muting;
- hold A+B to enter hardware diagnostics;
- hold A to open a minimal menu made entirely from KK's eyes; inside the menu, click A to browse brightness, sound, scheduled quiet mute, network and firmware version, double-click A to confirm, and press B to go back. Short A/B presses on the normal face do not select expressions; swipe down to check battery, while eye opening directly conveys brightness and volume;
- dim after 45 seconds of inactivity and clear/switch the AMOLED off after 60 seconds, with touch, button and motion wake;
- persist brightness, idle timeouts and quiet hours in NVS;
- synchronize China Standard Time over NTP after Wi-Fi connects, write it to the RX8130 RTC, and use the RTC for quiet-hour sleepy behavior;
- semantic serial commands provide a stable input boundary for future voice recognition or external control.

## Interaction map

| Input | Result |
| --- | --- |
| Tap | `happy` |
| Double tap | `surprised` |
| Hold and move | Eyes and head continuously follow the touch point |
| Long press | `angry` |
| Swipe left / right | Give a brief curious or confused reaction based on direction, then restore the current state |
| Swipe up | `surprised` |
| Swipe down | Show battery percentage and charging state, then restore automatically; B returns early |
| Slowly tilt the device | Gaze continuously follows the tilt direction |
| Quickly move the device | Add a brief inertial eye/head response without changing expression |
| A / B on the normal face | Hold A to open the eye menu; short A/B presses do not switch expressions |
| A / B in the root menu | Click A to browse, double-click A to confirm, and press B to close |
| Brightness / sound page | Click A to cycle levels quickly, B returns; right-eye opening follows the level |
| Scheduled quiet page | Disabled by default. Click A to toggle and save the `22:00–07:00` mute window, B returns; disabling mute does not disable the sleepy night expression |
| Network page | The resting view only reports Wi-Fi status. Hold A to reveal the pair/change-network prompt and keep holding for about two seconds to start pairing; B cancels or returns, and saved credentials remain until a new connection succeeds |
| Version page | Read-only eye view showing firmware version `0.6.2`; B returns |
| Hold A+B | Enter / exit hardware diagnostics |

`idle`, `listening` and `thinking` are persistent base states. Other reactions return to the previously active base state when their animation finishes instead of always returning to idle.

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
dim 60
screenoff 300
quiet 22 7
wifi
wifi pair
wifi retry
wifi forget
screen on
screen off
```

Network time uses China Standard Time (`UTC+8`, `Asia/Shanghai`, no daylight saving). KK synchronizes after Wi-Fi connects, retries after one minute on failure, and rewrites the RTC every six hours; serial time setting remains available. `dim` and `screenoff` use seconds; brightness, timeouts, the scheduled-mute switch, quiet hours and Wi-Fi credentials persist in NVS. In `KK-XXXX`, `XXXX` is the last four hexadecimal digits derived from this device's unique chip ID, used only to distinguish nearby KK devices. The pairing portal accepts 2.4 GHz networks, runs for at most five minutes and never prints the submitted password. A candidate network must connect successfully before it replaces the saved credentials; otherwise KK restores the previous network.

## Repository map

| Path | Purpose |
| --- | --- |
| `src/avatar_engine.*` | Expression catalogue, timelines, easing, drawing and interaction physics |
| `src/main.cpp` | Device setup, touch/IMU/buttons, vibration, diagnostics and serial commands |
| `src/wifi_pairing.*` | Non-blocking station connection, temporary captive portal and credential handoff |
| `docs/HARDWARE_BASELINE.md` | Hardware capabilities and verification boundary |
| `docs/ENGINEERING_NOTES.md` | Rendering experiments, measurements and implementation decisions |
| `docs/ROADMAP.md` | Planned work and intentionally unsupported features |

## Next implementation handoff: LAN eye messages

This feature is planned but not implemented in firmware `0.6.2`. Its goal is to let a phone on the same Wi-Fi send a short message that appears inside KK's eyes without introducing a conventional on-device panel.

### Required behavior

- Add a station-mode HTTP page at `/message` with two compact inputs: `left` and `right`. Each field represents the text placed in one eye.
- Add `POST /api/message` using URL-encoded form fields `left`, `right` and optional `hold_ms`. Clamp `hold_ms` to `1500–10000`; default to `3400`.
- Limit each eye to four visible UTF-8 characters and the complete request body to 128 bytes. Reject missing, malformed or oversized input with HTTP `400` or `413`.
- The HTTP handler must only validate and enqueue data. Rendering, sound, vibration and avatar state changes must stay in the main loop so network traffic cannot block TE-synchronized animation.
- Reuse `AvatarEngine::setEyeMessage()` and the existing fade, breathing text, blink/head-shake dismissal and base-expression restoration. B dismisses early.
- Diagnostic mode, pairing, the eye menu and battery status take priority. Hold one pending message while those modes are active; the newest pending message may replace the older one.
- An accepted message may wake the AMOLED and reset the idle timer. It must never manually select a permanent expression.

### Networking and safety boundaries

- Serve the message page only while station Wi-Fi is connected. Stop it before the captive pairing portal starts and restart it after station reconnection; only one service may own port 80 at a time.
- Do not expose Wi-Fi credentials, message content or request bodies in serial logs. Escape all text inserted into HTML and never treat received text as markup.
- Phase one is LAN-only: no cloud relay, port forwarding, authentication, microphone, speech recognition or LLM calls. Document that any client already on the local network can submit a message until authentication is added.
- Prefer a small `local_message_server.*` module, with explicit start/stop coordination from `main.cpp`, rather than drawing from `wifi_pairing.cpp` callbacks.

### Acceptance checks

1. Pairing and saved-network restoration still work, including cancelling the portal with B.
2. Twenty consecutive valid submissions display and dismiss without a reboot, leak-like slowdown or loss of the previous base expression.
3. Invalid UTF-8, empty messages, oversized fields and oversized bodies are rejected without rendering.
4. Menu/status activity defers the message; the newest pending message appears after the higher-priority mode exits.
5. Real hardware remains near 60 fps with TE synchronization at 100% and no recurring frame timeout while the page is loaded and messages are submitted.
6. Update both READMEs and engineering notes with the final endpoint contract, memory use and real-device measurements.

## Known limitations

- The microphone and speech/LLM service are not connected yet. Wi-Fi pairing is only the transport foundation; serial commands still simulate semantic voice events.
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
