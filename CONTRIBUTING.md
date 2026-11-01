# Contributing

Focused patches, documentation and reproducible hardware reports are welcome.

## Report a problem

Use the issue forms. Include version/commit, Windows build, adapter/controller model, transport, exact steps and expected/actual results. Distinguish physical tests from build checks. Redact hardware addresses, serials, personal paths and private signing material. Report vulnerabilities under [SECURITY.md](SECURITY.md).

## Send a patch

1. Fork and create a focused branch.
2. Preserve behavior outside the change.
3. Run tools/Build.ps1 and relevant tests.
4. Explain the problem, new behavior, validation and remaining hardware checks.
5. Sign off commits with `git commit -s`.

Sign-off certifies the [Developer Certificate of Origin 1.1](https://developercertificate.org/); it is not copyright assignment. Use a public-safe identity/address, including a GitHub no-reply address if preferred. Maintainers check sign-offs during review.

## Licensing

Contributions are accepted under **GPL-3.0-only**, subject to existing upstream notices. Contribute only material you have the right to submit. Identify third-party source, exact revision, license and modifications.

Publicly readable code is not automatically open-source. Do not copy or translate unlicensed code and assume attribution is permission. Cite protocol facts and distinguish independent implementation from adapted code. Do not include Nintendo firmware/keys, proprietary SDKs, commercial motion libraries or third-party installers.

## Review expectations

Use C++20/MSVC, focused functions and clear names. Preserve calibration, identity isolation, timestamps and input cleanup. Avoid unsolicited telemetry or global Windows security/input changes. Add meaningful behavioral tests; do not claim hardware success from CI. Discuss incompatible HID/IPC changes first.

Follow the [Code of Conduct](CODE_OF_CONDUCT.md).
