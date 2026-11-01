# Developer guide

| Path | Purpose |
| --- | --- |
| src/ | Transport, protocol, profiles, motion, mouse, service, tray, Settings and diagnostics |
| driver/ | UMDF virtual HID and SensorsCx drivers, descriptors, INFs |
| tests/ | Hardware-independent C++ regression tests |
| tools/ | Build, signing, install/update/uninstall and bounded capture helpers |
| assets/ | Maintainer-supplied tray artwork and Windows resource |
| docs/ | Public guides, validation, provenance and release procedure |

The service owns Windows-paired discovery and machine-local profiles. Dedicated Pro/Joy-Con coordinators own physical transport. Virtual consumers expose gamepad, auxiliary HID and native sensors. The tray runs in the console user's session and uses bounded service IPC.

## Build loop

```powershell
.\tools\Build.ps1
ctest --test-dir build -C Release --output-on-failure
.\build\Release\SettingsRenderCheck.exe model-preview.png
```

Build writes output but does not replace the installed driver. Do not sign/install for documentation changes.

Preserve original sample timestamps. Distinguish packets, distinct samples and Windows events; repeated snapshots are not higher-rate sensing. Keep UI work off sample paths and clear held state on disconnect/shutdown/failure.

## Diagnostics

- SettingsCheck: profiles, sticks, button masks and Settings IPC.
- SettingsRenderCheck: off-screen actual model renderer.
- XInputCheck: Windows gamepad input.
- SensorCheck: Gyrometer/Accelerometer rate, values and timestamp order.
- JoyCheck: manager/coordinator/mouse state.
- RateCheck: Pro counter deltas and optional link interval read.
- HandoffCheck: USB/Bluetooth timing.
- HomeInputCheck / MouseCheck: HID buttons and relative mouse output.

Direct USB/Bluetooth probes can require exclusive ownership or change volatile settings. Read their source/arguments first; do not compete with the installed owner casually. Redact captures before sharing.

## Compatibility and release

Settings reports are 64 bytes; physical button diagnostics occupy a capability-gated reserved tail. Preserve existing consumer reports and old IPC commands for targeted updates. Descriptor, consumer reader or incompatible IPC changes require a full package update.

Use a new driver version for binary changes. Never disguise changed binaries under an already installed version. [Release procedure](RELEASING.md).

Windows CI builds/tests/validates INFs only. It never signs, installs, changes trust or operates hardware.
