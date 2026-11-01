# Installer development

The setup project is `installer/Setup.iss`, built with Inno Setup 6.7.4. The bootstrap is pinned by SHA-256 and Authenticode publisher. The generated executable uses the system's light/dark mode, Segoe UI and DPI-aware native controls. It is an offline **development preview**, not a production-signed release.

From a clean committed Windows checkout:

```powershell
.\tools\Build.ps1
.\tests\installer_tests.ps1
.\tools\Prepare-Package.ps1
$compiler = .\tools\Get-InnoCompiler.ps1
.\tools\Build-Installer.ps1 -IsccPath $compiler
```

Complete scripts are in [tools](../tools) and [installer](../installer). Build products live in ignored `out/installer`. `Build-Installer.ps1 -PreviewUiOnly` makes a separately named UI preview that refuses to install; never distribute it. Production setup requires administrator access, Windows 11 x64 and explicit certificate consent. It never changes global PowerShell execution policy; its child process uses a process-scoped policy to run the embedded scripts. Organization-enforced policy still takes precedence.

## Transaction boundaries

Inno stages the reviewed payload privately, then calls `Invoke-Setup.ps1` during PrepareToInstall. It cannot report success or register its uninstaller before driver setup succeeds. The backend serializes setup/uninstall, verifies payload hashes/signatures/certificate lifetime, checks recorded virtual-device/service ownership and chooses fresh install, update or same-version maintenance. The same-version path refreshes setup documents only; damaged installations need journal-guided recovery.

Fresh setup journals staged INF packages before registering roots. A failure attempts removal of only this fresh transaction. Existing-install update failures preserve the driver updater's journal and backup. A failure retains recovery scripts in a separate Program Files directory. Inno cancellation is not allowed during mutation. Restart results are accumulated and reported; setup defaults to restart later. An uninstall failure stops before management files are removed.

`Switch2DeviceSetup` only accepts the 17 project hardware IDs, verifies the INF's class and model entry, refuses duplicate roots and uses Windows SetupAPI/Newdev. It does not bundle devcon, replace physical USB drivers, force a driver downgrade or suppress Windows security. Its `check` command is read only.

All original `Switch2Native` hardware IDs, instance journals, service/registry/profile names and executable filenames remain for update compatibility. Display branding and repository URLs use `Switch2NativeControllerDriver`.

## Validation and release gate

CI compiles the driver, runs seven controller suites, validates both INFs, runs installer policy/script/helper checks, signs a temporary development payload, compiles setup and attaches it with the corresponding source, build metadata and hashes. It does **not** install a Windows 11 driver on the Windows Server CI runner. It has read-only repository permissions, needs no signing secret, and deletes the private development key after package signing.

Before promoting any installer beyond a tester preview, run these on a clean Windows 11 x64 VM/test PC with stock security:

- Cancel before certificate consent: no driver/trust/service changes.
- Fresh install with no SDK/WDK/VC runtime installed; check all 17 roots, service, Bluetooth/USB input and native sensors.
- Reject a corrupted payload, expired certificate, unsupported architecture/OS and downgrade.
- Re-run the same installer and update an older driver; check pairing, profiles, device identities and actual restart result.
- Induce an installation failure and inspect rollback/recovery; do not report a successful install on failure.
- Uninstall through Installed apps, including with a controller connected; confirm held input release, removal of owned nodes/packages/trust/service, preserved pairings/profiles, and no unrelated changes.
- Verify 100/150/200% DPI, keyboard navigation, dark/light and high-contrast appearance.

Record physical/VM acceptance separately from policy/unit tests. No clean-machine acceptance can be inferred from compiling, previewing the UI or updating an existing development PC.

Source is bundled inside the executable and published alongside it, tied to `build-info.json`'s exact commit. Installer bootstrap/runtime notices remain included. A stable publicly trusted application signature and Microsoft driver production signing must be arranged before a normal production release; see [Microsoft test-signing guidance](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/how-to-test-sign-a-driver-package).
