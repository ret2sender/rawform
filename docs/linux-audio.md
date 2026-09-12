# rawform on Linux: PipeWire audio

rawform's Linux audio backend is a native PipeWire client (`libpipewire-0.3`,
built when `pipewire-devel` or your distribution's equivalent is present at
configure time). This note explains what it does with your graph, what
bit-perfect means here, and which knobs matter. Nothing below requires
editing system configuration; one optional user-level drop-in is described
for a specific preference.

## Bit-perfect playback and the graph rate

A PipeWire graph runs at one clock rate at a time. When bit-perfect output is
enabled (Settings > Playback, the default), rawform asks the graph to clock
each track's native sample rate using a per-stream rate force
(`node.force-rate`). This works on a stock PipeWire configuration: the force
applies regardless of `clock.allowed-rates`, holds exactly as long as
rawform's stream exists, and lifts the moment the stream goes away, including
if rawform is killed. There is no state to clean up and no configuration to
restore; the daemon does it structurally.

The player bar tells the truth about the outcome: "(Bit Perfect)" when the
graph genuinely clocks the track's rate, "(Resampled to N Hz)" when PipeWire's
resampler is bridging. The rate shown is measured from the live graph clock,
not assumed from the request.

Notes on the mechanism:

- A graph rate switch affects all applications' audio while it is in force,
  exactly as it would in any bit-perfect setup.
- With bit-perfect disabled, rawform never touches the graph rate and
  PipeWire resamples to whatever the graph is doing.
- If something else asserts a rate (for example
  `pw-metadata -n settings 0 clock.force-rate 48000`, which overrides
  per-stream forces), rawform notices, reports the change in the console, and
  updates the player bar immediately.

### Optional: allowed-rates without forcing

If you prefer the graph to follow track rates through PipeWire's own rate
selection rather than rawform's force, extend the allowed list with a user
drop-in:

    mkdir -p ~/.config/pipewire/pipewire.conf.d
    cat > ~/.config/pipewire/pipewire.conf.d/99-rawform-rates.conf <<'EOF'
    context.properties = {
        default.clock.allowed-rates = [ 44100 48000 88200 96000 176400 192000 ]
    }
    EOF
    systemctl --user restart pipewire pipewire-pulse wireplumber

This is not required for rawform's bit-perfect playback; it is the standard
setup for making the whole graph rate-flexible.

## Output devices

The Settings > Playback device picker lists the graph's `Audio/Sink` nodes.

- "Follow system default" (the default choice) tracks the system's output
  selection; when you change the default output in your desktop's audio
  settings, the session manager migrates rawform's live stream and rawform
  follows without interruption.
- Picking a concrete device pins rawform's audio to it, by the node's
  persistent name, even if the system default later moves. The pin is
  remembered across restarts; if the device is not present at startup (for
  example a profile turned off), rawform says so in the console, plays on the
  default, and shows the remembered device as "(not connected)" until it
  returns.

What a device advertises is what its driver exposes. Some consumer cards
route output through a fixed-rate DSP and genuinely offer a single rate
(48000 is common); rawform reports resampling on such devices honestly. Any
USB Audio Class 2 DAC exposes its real converter rates and works bit-perfect
out of the box.

## The last mile: driver-format verification

The graph clock (the io position rawform measures) says what rate the graph
runs at; it does not by itself prove the DRIVER realized that rate. A node
that advertises a rate interval could, in principle, accept an interior rate
into the graph and then have its driver snap to something else underneath,
resampling the last mile below the clock. rawform closes that gap by watching
the output node's live Format parameter, the same fact `pw-top`'s FORMAT
column shows. While a track plays, the driver's realized rate is compared
with the graph clock: agreement is silence; a divergence is logged and the
published outcome switches to the DRIVER'S rate (the status suffix reads
"(Resampled to N Hz)" with the hardware's true rate), and a later
re-convergence heals the outcome the same way. The watch follows the stream
when the session manager migrates it to a new default device.

In practice, on PipeWire 1.6.8 with in-tree ALSA drivers, a rate the graph
confirms is a rate the driver runs: the device investigation force-verified an
interior rate all the way to the driver's configured format, and no divergence
has been observed on the tested hardware. The watch exists so that claim is
continuously verified rather than assumed, and so the exotic case (an
out-of-tree or misdeclaring driver) reports honestly instead of flattering
the graph.

## Diagnostics

- `pw-top` shows the live graph: the driver node's RATE and FORMAT columns
  are the hardware ground truth.
- `pw-metadata -n settings 0` shows the graph's clock settings. Note that a
  per-stream force does NOT appear here; the settings metadata only knows the
  graph's own rate.
- rawform's log console (the `>` prompt on the status line under the
  playlist opens it) narrates every rate decision, switch, confirmation,
  external change, and device event.
- `RAWFORM_PW_PODS=1 rawform_audio_cli rates` dumps the raw EnumFormat pod
  inventory of the resolved output device: what the driver actually
  advertises, before any interpretation.

## Crash recovery

None needed on this backend. The rate force lifts with rawform's stream, so a
killed process cannot leave the graph reclocked. (The `rate_ledger.yaml`
crash-recovery file documented for macOS never appears on Linux.)
