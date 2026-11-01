# Security policy

Use [GitHub private vulnerability reporting](https://github.com/SmokeyMcGames/Switch2NativeControllerDriver/security/advisories/new) for privilege boundaries, installation, IPC, unsafe input parsing or code execution. Avoid public exploitation details before assessment.

Include affected commit/version, impact, reproduction and a minimal redacted example. Do not send private keys, pairing keys, full device inventories or personal logs. If the private form is unavailable, request a private contact in an issue without disclosing vulnerability details.

Community maintenance has no guaranteed response SLA. The latest development version is assessed; older snapshots have no separately supported branch.

The manager is a Windows service; installation needs administrator privileges. The tray runs in the console user's session. Profile saves validate identity/session/generation. Updates verify package hashes and device ownership.

Development signing is not Microsoft certification. Trust reviewed packages only. Do not disable Windows protections, force-kill driver hosts or import certificates from untrusted reports.

CI uses read-only permissions and pinned actions, and does not sign/install drivers. Keys, local certificates and hardware captures must not be committed. Repository security features do not certify that the driver is vulnerability-free.
