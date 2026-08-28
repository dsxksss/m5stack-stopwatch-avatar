# Roadmap

This roadmap separates implemented firmware from future hardware experiments.

## Current foundation

- 11 procedural expressions and reusable animation timelines;
- persistent base states with temporary reactions that return to the active base;
- touch following, click, double-click, long-press and continuous swipe;
- accelerometer/gyroscope tilt following with a centered safe-motion area and persistent low/medium/high motion profiles;
- expression-aware A/B eye-menu controls, vibration feedback and hardware diagnostics;
- serial semantic commands for external controllers;
- full-screen Unicode narrative text with circular-safe wrapping, typewriter reveal, manual cyclic paging and hold-B expression return;
- a full-screen hold-B mode menu with volatile LAN text delivery plus persistent public HTTPS `KKREAD/1` sources containing text and JPEG/PNG blocks;
- RTC-aware quiet hours, battery/charging telemetry and a compact status page;
- expression-led Wi-Fi management through a temporary captive portal, with up to five saved profiles and strongest-visible selection;
- non-blocking NTP synchronization for China Standard Time with periodic RX8130 updates;
- synthesized UI audio with persistent volume levels and quiet-hours muting;
- NVS-backed brightness, motion sensitivity and idle-timeout settings;
- inactivity dimming, 20 fps idle rendering and AMOLED sleep with touch/button/motion wake;
- 60 fps target renderer with dynamic dirty rectangles.

## Near-term work

- add a short real-device demo video and interaction test matrix;
- evaluate whether touch gestures also need user-selectable calibration after the motion-profile rollout;
- add regression checks for expression catalogue and playback transitions;
- add fixture-based parser tests and a broader real-device JPEG/PNG size/orientation matrix;
- package signed firmware binaries and checksums in GitHub Releases;
- measure power consumption and decide whether to add true deep sleep beyond the current AMOLED-only sleep strategy;
- add a user-facing time-zone setting for regions outside China Standard Time.

## Exploratory work

- microphone capture and offline command-word recognition;
- microphone capture, push-to-talk and speech recognition;
- authenticated voice/LLM relay and external-controller integrations;
- a compact authoring/export path for new procedural expressions.

These items are not shipped capabilities until they are implemented, compiled, uploaded and observed on a physical device.
