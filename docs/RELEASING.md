# Release procedure

1. Audit source, new assets and provenance; preserve upstream notices and resolve licensing gaps.
2. Scan for keys, credentials, hardware identities, captures, private notes and machine paths. Stage explicit files; never force-add ignored output.
3. Update version/release notes. Distinguish source development snapshots from signed installable packages.
4. Run Windows build, seven CTests and INF validation. Record physical acceptance separately.
5. Review HID/IPC compatibility and full versus targeted update requirements.
6. Create an annotated tag and prerelease for experimental versions, tied to the exact source commit.
7. Any future binary release must include complete corresponding source, build/install scripts, GPL and notices at the same release location, plus hashes/signing details. Meet all applicable GPL obligations, including installation information where required.
8. Never publish private signing keys or present local development certificates as production trust. Do not ask users to disable Windows security.
9. Inspect rendered documentation, issue forms, security reporting and CI.

Initial 0.3.9.2 publication is source-only. GitHub generates source archives from its tag; no development certificate or prebuilt installer is attached.

## Installer previews (0.3.9.3 onward)

See [installer development](INSTALLER-DEVELOPMENT.md). Build from a clean commit and attach all four products from `out/installer`: setup EXE, corresponding source ZIP, `build-info.json` and `SHA256SUMS.txt`. Exclude the UI-only preview. Use a clearly marked **prerelease** with `--latest=false`; do not replace the previous release or imply production certification. A preview from an open PR must identify its exact commit and unmerged status. Verify hashes after upload.

Before a normal release, complete the clean-Windows installation/update/uninstall matrix, arrange production signing, and replace the development-consent/signing path appropriately. Do not publish private keys or bypass Windows signing/security requirements. CI artifacts are short-lived test downloads, not a permanent release channel.
