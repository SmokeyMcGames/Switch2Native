# Changelog

## 0.3.9.3 — 2026-10-31

- Rename the app and repository to **Switch2NativeControllerDriver**. Internal hardware IDs, service names and profile paths remain compatible.
- Add a standalone Windows 11 x64 development-preview setup with Windows light/dark styling, explicit signing consent, offline connection guide, update detection and an Installed apps uninstaller.
- Replace the install-time WDK/devcon dependency with a narrowly scoped native root-device helper.
- Package corresponding source and license notices, SHA-256 checksums and build metadata; build a downloadable preview artifact in CI.
- Preserve controller behavior, gyro rates, pairings and per-controller settings. No production signing or universal clean-PC compatibility is claimed.

## 0.3.9.2 — 2026-10-31

First public source snapshot of the accumulated development project.

- Physical button highlights on rotating Settings models, including stick clicks, shoulders/triggers, Joy-Con SL/SR and Pro GL/GR.
- Brief-tap flashes, physical button names and stale/disconnect clearing.
- Individual dead zones/gyro response, live sticks and motion diagnostics.
- Pro USB/Bluetooth and native motion; Joy-Con separate/combined input, LEDs and mouse modes.
- Relative gyro mouse, click stabilization, optical input and controller shortcuts.
- Windows-owned pairing, custom captions and compatible targeted updates.

Earlier versions were local iterations, not public releases. See [validation](docs/VALIDATION.md). This snapshot is experimental and source-only.
