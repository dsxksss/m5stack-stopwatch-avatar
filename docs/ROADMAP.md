# Roadmap

This roadmap separates implemented firmware from future hardware experiments.

## Current foundation

- 11 procedural expressions and reusable animation timelines;
- persistent base states with temporary reactions that return to the active base;
- touch following, click, double-click, long-press and continuous swipe;
- accelerometer/gyroscope tilt following with a centered safe-motion area;
- expression-aware A/B eye-menu controls, vibration feedback and hardware diagnostics;
- serial semantic commands for external controllers;
- RTC-aware quiet hours, battery/charging telemetry and a compact status page;
- expression-led Wi-Fi pairing through a temporary captive portal;
- synthesized UI audio with persistent volume levels and quiet-hours muting;
- NVS-backed brightness and idle-timeout settings;
- inactivity dimming, 20 fps idle rendering and AMOLED sleep with touch/button/motion wake;
- 60 fps target renderer with dynamic dirty rectangles.

## Near-term work

- add a short real-device demo video and interaction test matrix;
- expose touch, tilt and shake thresholds as a small calibration profile;
- add regression checks for expression catalogue and playback transitions;
- package signed firmware binaries and checksums in GitHub Releases;
- measure power consumption and decide whether to add true deep sleep beyond the current AMOLED-only sleep strategy;
- add automatic network time synchronization and a safe RTC setup flow.

## Exploratory work

- microphone capture and offline command-word recognition;
- microphone capture, push-to-talk and speech recognition;
- authenticated voice/LLM relay and external-controller integrations;
- a compact authoring/export path for new procedural expressions.

These items are not shipped capabilities until they are implemented, compiled, uploaded and observed on a physical device.
