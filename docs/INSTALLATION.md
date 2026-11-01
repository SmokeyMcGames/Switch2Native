# Install, update and build

## Offline setup (development preview)

Download `Switch2NativeControllerDriver-0.3.9.3-Preview-Setup-x64.exe` from the project's [releases](https://github.com/SmokeyMcGames/Switch2NativeControllerDriver/releases). PR validation builds are also available as GitHub Actions artifacts; those require a GitHub login. Use only official project downloads and compare the accompanying `SHA256SUMS.txt` if needed.

1. Close games and competing controller software, then run setup on a **Windows 11 x64 test PC** with administrator access.
2. Read the GPL license and development-signing page. The checkbox explicitly authorizes installing the bundled public development certificate into LocalMachine Root and TrustedPublisher. Cancel if you do not trust it.
3. Choose Install. Setup checks the package, installs virtual devices and the manager, and adds an entry to Windows Installed apps. No SDK, WDK, CMake, Visual Studio or runtime download is required on the target PC.
4. Pair through Windows Bluetooth settings, or connect a Pro Controller by USB. Press a normal button once if a newly paired controller sleeps. The tray appears while a controller is connected.

Setup follows Windows light/dark appearance. Its final page offers the offline guide and Bluetooth settings. The app's visible name is **Switch2NativeControllerDriver**; internal service names, hardware IDs and saved paths retain `Switch2Native` for compatibility.

Run a newer setup to update in place. This release uses the compatible transport/helper update path. Running the same version refreshes setup files and documentation without forcing a driver replacement; it is not a damaged-driver repair. Downgrades, unknown ownership, incomplete installs and unsafe mixed versions are rejected. A failed fresh install attempts rollback; an update failure preserves its journal and backup instead of uninstalling the existing driver.

Uninstall through **Settings → Apps → Installed apps → Switch2NativeControllerDriver**, or its Start menu entry. Physical Windows pairing and controller profiles remain. Logs live under `%ProgramData%\Switch2Native\logs`; failed transactions may retain recovery scripts under `%ProgramFiles%\Switch2Native Setup Recovery`. Do not delete the installation journal before recovery.

**Signing limitation:** setup's outer executable is not signed by a publicly trusted publisher. The payload uses an ephemeral development certificate, whose expiry is shown in setup and `build-info.json`. Replace an expired preview with a freshly built package. These packages are for development/testing, not production distribution. Windows/organization policy may block them; do not disable protections to bypass failures. Microsoft production signing is a separate release requirement. See [Microsoft's test-signing guidance](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/how-to-test-sign-a-driver-package).

Setup reports an actual Windows restart requirement and defaults to restarting later. For unattended testing only: `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TRUSTDEVCERT=1 /RESTARTEXITCODE=3010`. Explicit certificate consent is required; the default is no consent.

Complete corresponding source, build/install scripts, GPL and notices are bundled under `%ProgramFiles%\Switch2Native Setup`, with source also attached beside the executable. No private signing key is distributed.

## Before installing

The following instructions are for **building from source**. You need administrator access for installation and the SDK/WDK tools below on the build machine.

Local signing creates your own development certificate. Installation explicitly adds its public certificate to this PC's LocalMachine **Root** and **TrustedPublisher** stores. Only trust a package you built or reviewed. Do not install certificates or executables from issue attachments.

The scripts do not disable Secure Boot or Memory Integrity, enable global test-signing, replace physical Nintendo USB drivers, or restart Windows automatically. Windows may reject a package under a particular machine's security policy; do not weaken that policy to bypass an error.

## Requirements

- Windows 11 x64 and a Bluetooth LE adapter for wireless controllers.
- Visual Studio 2022 with Desktop development with C++ and MSVC tools.
- CMake 3.24 or newer on PATH.
- Windows SDK/WDK 10.0.26100.0, including UMDF 2.31 libraries.
- WDK utilities: InfVerif, Inf2Cat and SignTool. Root-device registration uses the included `Switch2DeviceSetup` helper.

Scripts currently use the standard Windows Kits directory and VS2022 generator. ARM64 and alternative layouts are not validated.

## Build and prepare

From a normal PowerShell in the repository:

```powershell
.\tools\Build.ps1
.\tools\Prepare-Package.ps1
```

Build runs seven test suites and both INF checks. Known desktop INF warning 2084 concerns reuse of inbox xinputhid.sys; passing checks do not imply Microsoft certification.

Prepare signs the driver/helper binaries and catalogs, exports the public certificate and writes SHA-256 hashes to `out/driver/manifest.json`. The temporary private key is deleted afterward. Preparation does not install trust/devices. Rebuilding invalidates signatures: prepare again before installation.

## Install

Close competing software if it holds the controller. In **administrator PowerShell**, from the repository:

```powershell
.\tools\Install-Driver.ps1 -TrustLocalDevelopmentCertificate
```

No Bluetooth address argument is required. The manager discovers supported controllers paired in Windows. Connect a Pro by USB or pair through Windows Settings. If newly paired but disconnected, press a normal button.

The installer creates virtual gamepad/motion/sensor devices, installs the manager service and tray under `%ProgramFiles%\Switch2Native`, and records owned IDs/packages/certificates in `%ProgramData%\Switch2Native\installation.json`. Keep that record for update/uninstall.

## Update

Build and prepare a **newer** package, then use administrator PowerShell:

```powershell
.\tools\Update-Driver.ps1 -TrustLocalDevelopmentCertificate
```

The updater validates hashes/ownership, pauses readers, updates existing nodes and restores operation while preserving instance IDs and Windows pairings. Recovery copies live under `%ProgramData%\Switch2Native\updates`.

For maintainer-designated transport/helper-only changes with compatible consumer HID/IPC contracts and consumer drivers at least 0.3.8.5:

```powershell
.\tools\Update-Driver.ps1 -TrustLocalDevelopmentCertificate -TransportOnly
```

Version 0.3.9.2's diagnostic button addition supports this path. It leaves consumers at their installed versions. Incompatible descriptors, sensor readers or contracts require a full update.

Windows can require a restart. Save work and follow the actual result; never kill WUDFHost or clear reboot flags to force replacement. `-Resume` is only for a recorded failed update after its cause is addressed.

## Uninstall

In administrator PowerShell:

```powershell
.\tools\Uninstall-Driver.ps1
```

Removes recorded project devices, service/helpers, owned packages and recorded development trust. Physical controller drivers and Windows pairing remain. Profiles/preferences, logs and backups may remain; see [Privacy](../PRIVACY.md). Do not delete installation metadata before uninstalling.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Newly paired but Not connected | Press a normal button once; avoid holding SYNC again. |
| Device absent | Check factory Windows name, Bluetooth LE support and Windows pairing. |
| USB initialization fails | Exit competing controller software and reconnect USB. |
| No tray | Wake a controller; the tray exits when all disconnect. |
| Duplicate game input | Check other remappers and physical versus virtual input. |
| No gyro in a game | Native sensors are separate from XInput; the game needs sensor support. |
| Update ownership/version failure | Preserve logs; never force-delete unrelated devices/packages. |
| Reboot required | Save work and complete the reported Windows restart. |

See [Support](../SUPPORT.md) for redacted bug reports.
