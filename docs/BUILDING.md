# Build and install from source (developers only)

**These tools and commands are only for compiling or modifying the project.** To use the controllers, download and run the [offline setup](INSTALLATION.md); none of the build tools below are needed on the destination PC.

## Before installing a source build

You need administrator access for installation and the SDK/WDK tools below on the build machine.

Local signing creates your own development certificate. Installation explicitly adds its public certificate to this PC's LocalMachine **Root** and **TrustedPublisher** stores. Only trust a package you built or reviewed. Do not install certificates or executables from issue attachments.

The scripts do not disable Secure Boot or Memory Integrity, enable global test-signing, replace physical Nintendo USB drivers, or restart Windows automatically. Windows may reject a package under a particular machine's security policy; do not weaken that policy to bypass an error.

## Build-machine requirements

- Windows 11 x64; a Bluetooth LE adapter for wireless controller testing.
- Visual Studio 2022 with Desktop development with C++ and MSVC tools.
- CMake 3.24 or newer on PATH.
- Windows SDK/WDK 10.0.26100.0, including UMDF 2.31 libraries.
- WDK utilities: InfVerif, Inf2Cat and SignTool. Root-device registration uses the included `Switch2DeviceSetup` helper.

Scripts currently use the standard Windows Kits directory and VS2022 generator. ARM64 and alternative layouts are not validated.

## Build and prepare

Clone the repository and use a normal PowerShell:

```powershell
git clone https://github.com/SmokeyMcGames/Switch2NativeControllerDriver.git
cd Switch2NativeControllerDriver
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

The 0.3.9.2 diagnostic button addition and 0.3.9.3 setup integration support this path. It leaves consumers at their installed versions. Incompatible descriptors, sensor readers or contracts require a full update.

Windows can require a restart. Save work and follow the actual result; never kill WUDFHost or clear reboot flags to force replacement. `-Resume` is only for a recorded failed update after its cause is addressed.

## Uninstall

In administrator PowerShell:

```powershell
.\tools\Uninstall-Driver.ps1
```

Removes recorded project devices, service/helpers, owned packages and recorded development trust. Physical controller drivers and Windows pairing remain. Profiles/preferences, logs and backups may remain; see [Privacy](../PRIVACY.md). Do not delete installation metadata before uninstalling.

## Build the offline installer

See [Installer development](INSTALLER-DEVELOPMENT.md) for packaging, signing and validation. The compiler and SDK/WDK are build-machine tools; they are not installed on end users' PCs. The project binaries use the static MSVC runtime, so setup does not require a separate Visual C++ redistributable download.

For unattended setup testing only: `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TRUSTDEVCERT=1 /RESTARTEXITCODE=3010`. Explicit certificate consent is required; the default is no consent.
