# Privacy and local data

Switch2NativeControllerDriver has no account system, cloud service, advertising, analytics or automatic crash uploads. Windows and GitHub have separate privacy policies. Bluetooth discovery/communication stays local through Windows.

Factory serials identify individual profiles; a Bluetooth address is fallback. These records are not sent to a project server. The manager uses supported controllers Windows records as paired.

## Local storage

- Machine configuration/profiles: HKLM\SOFTWARE\Switch2Native.
- Ownership record/update backups: %ProgramData%\Switch2Native.
- Installed helpers: %ProgramFiles%\Switch2Native.
- Tray preferences: console user's Windows registry.
- Explicit development captures/build/install logs: usually the checkout's captures directory.

Logs can contain hardware identities, Windows paths, process IDs and usernames. A GitHub issue is public unless explicitly private: redact before sharing.

Uninstall removes recorded devices, service/helpers, driver packages and recorded certificate trust. Windows pairings, profiles/preferences and some logs/backups can remain. Settings reset restores a connected controller's profile. Review remaining project-specific data before removing it; never delete unrelated registry/device files.

No remote deletion service exists because the project does not receive these local records.
