# Controller settings

Right-click the **Switch2NativeControllerDriver** tray icon and choose **Settings...**. Every active physical controller has its own panel. The Pro Controller shows both sticks; each Joy-Con shows its stick even when both halves share one gamepad. Scroll to reach additional panels.

## Sticks and dead zones

- The white dot is calibrated raw stick position, before dead zones and mouse-mode suppression. The blue ring is the gamepad output after those adjustments. A single sideways Joy-Con uses the same rotated axes as its gamepad.
- Each stick starts with an **8% Circular dead zone**, including after Reset. Circular uses the stick's distance from center. Its shaded circle is the ignored area. Travel outside that circle is rescaled smoothly to the full range, with diagonal output limited to the unit circle.
- The **Axis dead zone** option preserves the previous behavior. Both axes are processed independently; the shaded square marks positions where both outputs are zero. Choosing Axis is saved for that stick rather than overwritten at reconnect.
- The slider ranges from 0% to 40%. The preview follows the slider while dragging; release it to apply and save. Keyboard adjustments and shape selections save immediately.

## Gyro mouse response

Each Joy-Con has its own response setting, from **10 to 360 mouse counts per degree**. The default **90** retains the previously accepted air-mouse speed. Higher values move the pointer farther for the same turn. The percentage is relative to that default. Windows pointer settings can still affect the final distance on screen.

This setting applies to gyro mouse only. It does not change optical sensitivity, native sensor values, sample rates, or which Joy-Con controls the mouse. The right Joy-Con remains preferred, with left fallback. Click stabilization continues to break out after 0.4 degrees of deliberate held movement at every response setting.

## Motion diagnostics

Each panel shows angular velocity in degrees/second, acceleration including gravity in g, relative roll/pitch/yaw, a sample counter, and the delivered IMU rate measured over roughly one second. The display refreshes at about 25 frames/second; the driver integrates every real IMU sample independently of the window.

The model follows estimated **rotation**, with gravity correcting pitch and roll. It stays centered: these sensors cannot reliably locate the controller's position in the room. There is no absolute compass reference, so heading can drift. **Recenter heading** changes only the preview's heading reference, never mouse aiming or controller calibration. The model is a simplified diagnostic shape, not a dimensionally accurate product model.

## Button diagnostics

Buttons on each 3D model turn green while held. Brief taps flash for 120 ms so they remain visible between window refreshes. The line below the model lists held buttons, or the last brief tap. This diagnostic flash never lengthens gamepad or mouse input.

The preview uses physical button names and states before sideways mapping, mouse-mode suppression, or shortcut handling. It includes stick clicks, shoulders, triggers, Joy-Con SL/SR, and Pro GL/GR. Rotate the controller to see buttons on its side or back; their names also appear below the model when pressed. SYNC and the magnetic-release buttons are not game-input buttons.

Highlights clear when a controller disconnects or its input becomes stale. Older drivers without button telemetry show a waiting message instead of a false button-test result.

## Individual profiles

Profiles are keyed by controller type and factory serial, read from the controller's identity block. The same factory identity is used over USB and Bluetooth. A Bluetooth address is used only if factory identity is unavailable. USB without a usable unique identity is displayed, but saving is disabled rather than sharing another controller's profile.

The manager saves one validated binary `Profile` value under `HKLM\SOFTWARE\Switch2Native\Controllers\<type>-<identity-source>-<serial>`. These machine-local settings belong to the physical controller, not its player number, side, port, or pairing mode. Reset affects only that controller. Changes produce the existing custom tray caption. Profiles survive normal disconnects and updates.

## Implementation and compatibility

Hub feature reports F0-F3 carry physical controller identity, stick positions and the profile; F4-F7 carry motion and orientation. Every report is 64 bytes. The previously reserved F0-F3 tail now carries capability-gated physical button masks, recent taps and the last input tick. EA command 5 applies a profile only when the current physical identity matches. Existing E6/E7/E8/E9/EB/EC/EA layouts and report sizes are unchanged.

The existing v3 pipe retains its 24-byte request and 352-byte reply for older commands. Optional SettingsRead/SettingsSave commands return a separate 656-byte reply. Saves require the active console session and the current roster generation, preventing a delayed window from editing a replacement controller. Registry access stays in the service; sensor input paths only read a cached profile. The settings worker performs bounded IPC outside the UI thread.

This additive hub/helper change can use the targeted updater with the existing maintenance-aware 0.3.8.5 consumers. Changes to consumer reports or incompatible IPC layouts still require their corresponding update.

Factory identity offsets were cross-checked against the [controller memory layout research](https://github.com/ndeadly/switch2_controller_research/blob/master/memory_layout.md). Live identity reads and per-device persistence are verified separately during hardware validation.
