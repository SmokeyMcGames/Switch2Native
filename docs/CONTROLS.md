# Controls and modes

## Gamepads and players

Pro face buttons map by physical position: Nintendo B→Xbox A, A→B, Y→X, X→Y. Triggers are digital. Sticks use calibrated travel and per-controller dead zones.

Hold a single Joy-Con sideways with SL/SR on top. Its axes rotate with that layout; SL/SR are shoulders. Opposite halves can combine upright, retaining independent physical profiles and Settings panels.

Home is ordinary assignable HID input, not System Main Menu/Guide. Extra physical buttons do not add XInput channels. The Settings models show raw physical presses even when Mouse Mode or shortcuts consume them.

Joy-Con logical controllers receive Player 1–4 in connection order, independently of the Pro. Sideways LEDs read left-to-right; combined halves share a logical controller. Windows XInput slots are separate from these LED numbers.

Four separate Joy-Cons displace Pro gamepad output until a slot is free; its physical transport remains connected. Four actual Joy-Con radio links still need hardware validation.

## Mouse ownership

Right Joy-Con takes priority; left is fallback when no right is connected, in combined or separate mode. The left remains a normal gamepad while the right owns mouse input.

Mouse Mode starts off, with Optical selected. Last-Joy-Con disconnect disables it, releases clicks and clears pause. Mode selection and optical sensitivity are session state, not saved per-controller profiles.

## Optical mouse

Surface motion moves the pointer. Lift-off stops movement/clicks/scrolling and restores gamepad actions. Contact detection uses a measured heuristic; surfaces vary.

| Action | Right | Left fallback |
| --- | --- | --- |
| Left click | R | L |
| Right click | ZR | ZL |
| Middle click | X | Up-arrow |
| Scroll | Stick up/down | Stick up/down |
| Optical sensitivity up | Home | — |
| Optical sensitivity down | C | Capture |

Sensitivity changes once per press in 25-percentage-point steps, 25%–400%. Captions show the result. It is shared within the session and resets on driver restart. Stick-click does not middle-click.

## Gyro air mouse

Relative HID movement follows wrist rotation from the current pointer location across displays. A normal mouse stays usable without snap-back. This is not absolute room-position tracking or a laser-like screen intersection.

Keep the controller still briefly for residual bias learning. Stick-click alone pauses movement; clicks/scrolling remain available. Settings saves 10–360 counts/degree per physical Joy-Con, default 90. Windows pointer settings can affect screen travel.

Left/right clicks hold gyro movement for up to 200 ms without delaying click edges. Net held movement over 0.4 degrees breaks out early for dragging. Discarded movement is not replayed. Optical movement and middle-click do not use that hold.

## Shortcuts

| Action | Right | Left alone |
| --- | --- | --- |
| Toggle Mouse Mode | Plus + Home | Minus + Capture |
| Switch optical / gyro | Plus + stick-click | Minus + stick-click |
| Pause/resume gyro movement | Stick-click | Stick-click |

Hold Plus/Minus, press the second button, then release both. Simultaneous presses work; reverse order has a 150 ms window. Chords work lifted or with Mouse Mode off, on the eligible mouse controller.

Recognized chords suppress their component gamepad/sensitivity actions. A modifier alone reaches the gamepad on release; short taps are buffered so games can observe them. Optical sensitivity buttons retain ordinary gamepad behavior in gyro mode unless used in a shortcut.

## Tray

Hover text: **Switch2Native Controller Driver**. Menu: Settings, grouping, mouse mode/source, optical sensitivity and **Always show in taskbar tray**. Changes use fading app captions rather than Windows notification banners.

The background manager remains available. The tray only runs with a supported controller connected and finishes its last-disconnect caption before exiting. [Settings guide](../SETTINGS.md).
