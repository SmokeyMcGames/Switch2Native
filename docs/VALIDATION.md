# Validation and limitations

Results below are development-PC observations, not broad compatibility certification.

## Source release 0.3.9.2

| Area | Evidence |
| --- | --- |
| Pro USB/Bluetooth | Controls, native motion, handoffs and rumble exercised; user confirmed vibration on both transports. |
| Left/right Joy-Con 2 | Single sideways, separate/combined modes and LED order exercised. Four physical Joy-Cons not tested. |
| Windows pairing | Factory names display; unpaired advertising device remained unclaimed. Normal-button wake after pairing works; immediate first-pair connection is imperfect. |
| Home | User confirmed Game Bar stays closed on Pro USB/Bluetooth and Joy-Cons. |
| Optical mouse | Movement, clicks, scroll, lift gating, right priority and left fallback accepted. |
| Gyro mouse | Right aiming, normal-mouse coexistence, click stabilization and dragging accepted. Left gyro/shortcuts have less live coverage. |
| Disconnect | Held click releases, caption appears, tray exits. |
| Settings | Four stick plots and three model rotations accepted. Nondefault right profile survived reconnect without affecting others. Pro identity matched across transports. Defaults restored to 8% Circular /90 response. |
| Model buttons | User accepted matching green press/release highlights on all three models. Actual renderer inspected off-screen front/back/side. |
| Updates | Recent compatible targeted updates needed no restart; earlier incompatible updates did. No universal no-reboot guarantee. |

## Motion rate observations

Pro USB produced approximately 249–250 distinct/native samples/s. Settled Bluetooth runs measured 199.30 Pro, 199.78 left and 195.77 right with three connected. The 0.3.9.2 smoke window measured 161.56 Pro, 191.18 right and 185.32 left. Both sets had no backwards timestamps. The lower run's cause was not isolated.

Nominal 200 Hz Bluetooth is a target, not a guaranteed wall-clock rate. 250 Hz Bluetooth/500 Hz USB are unverified. Settings renders around 25 fps independently of sample processing.

Raw captures remain private because they can contain hardware identities and local paths. These values are reported observations, not independently reproducible published benchmark artifacts.

## Automated checks

Seven CTest suites cover profiles/dead zones/orientation/buttons, tray events, Joy-Con mouse, gyro/click stabilization, Joy-Con protocol, controller protocol, and packed motion. Both INFs are checked; desktop warning 2084 remains known.

CI cannot validate physical pairing, perceived latency, angular accuracy, rumble sensation or gameplay compatibility.

## Open validation

More adapters/Windows builds/firmware; four physical Joy-Cons; sleep/resume/radio toggles/long sessions; quantified angular accuracy/thermal drift/surfaces; Joy-Con USB/grips; battery/audio/NFC; production signing/security review; game-specific motion and anti-cheat compatibility.
