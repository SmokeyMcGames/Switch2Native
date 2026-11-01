# Install Switch2NativeControllerDriver

## Download and install

**[Download the Windows installer](https://github.com/SmokeyMcGames/Switch2NativeControllerDriver/releases/download/v0.3.9.3-preview.1/Switch2NativeControllerDriver-0.3.9.3-Preview-Setup-x64.exe)** — development preview 0.3.9.3.

**No separate software prerequisites or manual commands are needed.** Setup includes the driver, tray app, background manager, device-registration helper, offline guide and uninstaller. It handles the installation steps for you, including adding the development certificate after your consent. You do not need Visual Studio, the SDK/WDK, CMake, devcon or a separate Visual C++ runtime.

You need a **Windows 11 x64 PC**, permission to approve Windows' administrator prompt, and a supported controller. Wireless use requires a Bluetooth LE adapter; the Pro Controller also supports a data-capable USB cable. Windows security policy must permit the development-signed package; see the preview note below.

1. Download the **Preview-Setup-x64.exe** linked above. The source ZIP is for developers and is not needed for installation.
2. Close games and other controller software. Double-click the downloaded installer and approve the Windows administrator prompt.
3. Read the license and development-signing notice. If you trust this project, check the certificate consent box and choose **Install**. Setup performs the certificate and driver installation automatically.
4. Select **Finish**, then connect your controller as described below. Restart only if setup reports that Windows requires it.

The installer works offline once downloaded. There are no additional runtime or tool downloads during setup. Use the project's [Releases page](https://github.com/SmokeyMcGames/Switch2NativeControllerDriver/releases) for downloads; checksums and exact corresponding source are available there too.

## Connect a controller

- **Pro Controller over USB:** connect it directly to the PC with a data-capable cable.
- **Bluetooth:** hold SYNC until the LEDs cycle, then open **Windows Settings → Bluetooth & devices → Add device → Bluetooth** and select the controller. If it initially says **Not connected**, press a normal button once. Joy-Con 2 uses Bluetooth only.

The player LED becomes steady and the tray icon appears when connected. Right-click **Switch2NativeControllerDriver** in the tray for Settings and mouse/controller options. The tray closes after the last controller disconnects. Setup's final page can open Bluetooth settings and the offline guide for you.

## Update or uninstall

Run a newer setup to update in place. Setup preserves your Windows pairings and per-controller settings. Running the same version refreshes setup files and documentation without forcing a driver replacement; it is not a damaged-driver repair. Setup reports an actual Windows restart requirement and defaults to restarting later.

To uninstall, open **Settings → Apps → Installed apps → Switch2NativeControllerDriver → Uninstall**, or use its Start menu entry. The uninstaller removes the recorded driver components and development certificate. Windows pairings and controller profiles remain.

## About this development preview

This preview is for development/testing and has not yet been validated for fresh installation, rollback and full uninstall on a clean Windows 11 PC. Existing-PC update and same-version maintenance have been tested. It is not a production-signed or Microsoft-certified release.

The outer setup executable is not signed by a publicly trusted publisher. The bundled driver uses a temporary development certificate; setup displays its expiry and asks before adding it to the PC's trusted certificate stores. You do not need to import it manually. Use a fresh package if the certificate has expired.

Windows or organization policy may block this preview. Do not disable Windows protections to bypass an installation failure. Production signing is a project release requirement, not a tool users need to install. See [Microsoft's test-signing guidance](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/how-to-test-sign-a-driver-package).

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

Setup logs are under `%ProgramData%\Switch2Native\logs`. Keep the installation record and any recovery files if setup fails; include a redacted error message when reporting an issue. Setup rejects downgrades, incomplete installs and unrecognized device ownership rather than overwriting another installation. Failed fresh installs attempt rollback; failed updates keep their recovery records.

Looking to modify or compile the project? The separate [developer build guide](BUILDING.md) covers the toolchain and manual scripts. Source, licenses and build metadata are also included under `%ProgramFiles%\Switch2Native Setup` after installation.
