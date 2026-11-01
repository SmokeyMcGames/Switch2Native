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
