# rawform on macOS: CoreAudio audio

rawform's macOS audio backend is a CoreAudio client (an AUHAL output unit
bound to one device). This note explains what it does with the output
device, what bit-perfect means on this backend, and what the crash ledger
is for. It is the macOS counterpart of `linux-audio.md`; the two backends
reach the same goal by different means, and the differences are the point
of this page.

## Bit-perfect playback and the device's nominal rate

When bit-perfect output is enabled (Settings > Playback, the default),
rawform sets the output device's nominal sample rate to each track's rate
when the device supports it, with the nearest supported rate in the same
family as the fallback (a 192 kHz track plays at 96 kHz on a device that
tops out at 96 kHz). This is the same switch Audio MIDI Setup makes, and it
is device-wide: while a track plays at a switched rate, every other
application's audio is resampled by CoreAudio to that rate. There is no
per-stream rate on this backend, so there is nothing to scope the switch
to rawform alone.

The switch happens once per track boundary that needs it; a track at the
device's current rate causes no transition. The device is given its own
rate back when playback stops (Stop, or the end of the last track) and when
rawform quits. With bit-perfect off, rawform never touches the device rate
and every track is resampled to whatever the device is set to; turning the
policy off while a switched rate is in force restores the device at the
next track.

The player bar reports the measured outcome, not the request: "(Bit
Perfect)" when the device clocks the track's rate (read back from the unit
after initialization, because a device can accept a rate set and still
settle elsewhere), "(Resampled to N Hz)" when it does not. On this backend
the claim covers the rate, rawform's own gain (unity), and a source above
24 bits (reported as "(32-bit source, 24-bit output)", the float32
pipeline's limit); the system volume slider and the device's physical
stream format are not watched yet, so a macOS volume below full is not
reflected in the claim the way a PipeWire soft volume is on Linux.

## Output devices

The Settings > Playback device picker lists every output-capable device by
its unique identifier.

- "Follow system default" (the default choice) tracks the system's output
  selection. When the default changes (a device is plugged in or unplugged,
  or picked in System Settings), playback moves to the new default at the
  listening position; unplugging headphones mid-track continues on the
  speakers from the same spot.
- Picking a concrete device pins rawform's audio to it, even if the system
  default later moves. The pin is remembered across launches; a pinned
  device that is not connected at startup is shown as "(not connected)",
  and rawform plays on the default until it returns. A pinned device that
  disappears while playing ends the session with "output failed: output
  device '...' disconnected" in the console, and the pin is kept so the
  choice stays visible.

Channel layouts: mono reaches both speakers. A source with more channels
than the device (a 5.1 track on a stereo device) plays its first channels
only; the AUHAL does not downmix, and rawform does not add a mixer stage on
this backend.

## Crash recovery: `rate_ledger.yaml`

Because the rate switch is device-wide and is repaid only when the device
closes, a session that dies holding a switched rate would leave the device
at that rate for every other application. rawform therefore writes
`rate_ledger.yaml` (in the config directory, next to `playback.yaml`) the
moment a session borrows a rate, recording the device, its original rate
and the borrowed one, and deletes the file when the debt is repaid. The
file exists only while a rate change is in force, so on a healthy machine
it is rarely seen.

On the next launch, a ledger left behind by a crash is read before the
first open: if the device is still at the borrowed rate it is set back to
the original, and the console says so; if the device has since moved (or is
gone) the ledger is moot. The file is deleted in every case. A rate changed
by another application while rawform held it is treated as that
application's choice and is not undone. Linux never writes this file; the
PipeWire backend's rate force lifts with the stream.

## Diagnostics

- Audio MIDI Setup shows the device's current nominal rate, which is the
  ground truth for what the switch did.
- rawform's log console (the `>` prompt on the status line under the
  playlist opens it) narrates every rate decision, switch, confirmation and
  device event, and reports output failures, underruns and decode damage as
  they happen.
- `rawform_audio_cli rates` (built by `make core`) prints what the output
  device advertises, exactly as the rate policy sees it, and with a file
  argument the decision the policy would make for that file; it changes no
  device state.
