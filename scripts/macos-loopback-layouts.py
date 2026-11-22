#!/usr/bin/env python3
# This file is part of rawform.
# Copyright (C) 2026 Etienne Fleurant
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: GPL-3.0-or-later

#
# rawform - loopback check of the macOS speaker layouts.
#
# Plays one WAVEX file per layout through rawform_audio_cli into a virtual
# multichannel device (BlackHole 16ch by default), records the device's channels
# back with scripts/macos-loopback-capture.swift, and reports which tone reached
# which device channel. No speakers are involved, so it runs on a stereo Mac.
#
# Each layout runs twice:
#   - with the device's speaker configuration as found;
#   - with the device's speakers set to the layout's own speakers in reverse
#     order. A channel that follows its label and one that keeps its index then
#     land on different device channels, which tells the two behaviors apart.
#
# A source speaker the device labels must arrive on the device channel with that
# label; one the device does not label must arrive on the device channel with its
# own index; every other channel must be silent. A run also fails when the sink
# reports that declaring the layout or the channel map failed. The device's rate
# and speaker configuration are restored at exit.
#
# Usage (from the repository root, macOS):
#   python3 scripts/macos-loopback-layouts.py                  # every layout
#   python3 scripts/macos-loopback-layouts.py --only 7.1.4     # one layout
#   python3 scripts/macos-loopback-layouts.py --cli PATH       # another CLI build
#   python3 scripts/macos-loopback-layouts.py --self-test      # no device needed
#   python3 scripts/macos-loopback-layouts.py --write-fixtures DIR
#
# Needs BlackHole 16ch (brew install --cask blackhole-16ch) and, on the first
# run, microphone access for the terminal: macOS hands a process without it
# silent buffers, which the check reports as such.

import argparse
import array
import json
import math
import queue
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time
from collections import namedtuple
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_CLI = REPO / "build/core/rawform_audio_cli"
HELPER_SOURCE = REPO / "scripts/macos-loopback-capture.swift"
HELPER = REPO / "build/loopback/macos-loopback-capture"
DEFAULT_DEVICE = "BlackHole 16ch"

RATE = 48000
TONE_SECONDS = 4
AMPLITUDE = 0.2

# Peak level below which a channel counts as silent (-60 dBFS). The loopback is
# digital, so an idle channel reads exact zeros.
SILENT = 0.001

# Each speaker's WAVEFORMATEXTENSIBLE bit, the CoreAudio label CoreAudioSink.cpp
# declares for it, and its test tone. Every tone is a multiple of 10 Hz, so a
# 0.1 s analysis window holds whole periods of each and puts it on an exact
# Goertzel bin.
Speaker = namedtuple("Speaker", "bit label tone")
SPEAKERS = {
    "FL": Speaker(0x1, 1, 300),
    "FR": Speaker(0x2, 2, 500),
    "FC": Speaker(0x4, 3, 700),
    "LFE": Speaker(0x8, 4, 60),
    "BL": Speaker(0x10, 5, 900),
    "BR": Speaker(0x20, 6, 1100),
    "FLC": Speaker(0x40, 7, 1900),
    "FRC": Speaker(0x80, 8, 2100),
    "BC": Speaker(0x100, 9, 1300),
    "SL": Speaker(0x200, 10, 1500),
    "SR": Speaker(0x400, 11, 1700),
    "TC": Speaker(0x800, 12, 2300),
    "TFL": Speaker(0x1000, 13, 2500),
    "TFC": Speaker(0x2000, 14, 2700),
    "TFR": Speaker(0x4000, 15, 2900),
    "TBL": Speaker(0x8000, 16, 3100),
    "TBC": Speaker(0x10000, 17, 3300),
    "TBR": Speaker(0x20000, 18, 3500),
}
SPEAKER_BY_LABEL = {s.label: name for name, s in SPEAKERS.items()}
LABEL_UNKNOWN = 0xFFFFFFFF
LABEL_DISCRETE_BASE = 1 << 16

LAYOUTS = [
    ("4.0", 0x107),
    ("5.1", 0x3F),
    ("7.1(wide)", 0xFF),
    ("5.1.2(back)", 0x503F),
    ("7.1.4", 0x2D63F),
]

PCM_GUID = bytes.fromhex("0100000000001000800000aa00389b71")
FLOAT_GUID = bytes.fromhex("0300000000001000800000aa00389b71")

PROMPT = re.compile(r"^(> )+")
DEVICE_LINE = re.compile(r"^\s+(\d+)\. (.*?)(?:  \[system default\])?$")


class CheckError(Exception):
    pass


def speakers_of(mask):
    ordered = sorted(SPEAKERS.items(), key=lambda kv: kv[1].bit)
    return [name for name, s in ordered if mask & s.bit]


def slug(layout):
    return re.sub(r"[^0-9A-Za-z.]+", "-", layout).strip("-")


# ---------------------------------------------------------------------------
# WAV files
# ---------------------------------------------------------------------------

def wavex_bytes(channels, rate, mask, payload, bits, guid):
    block = channels * bits // 8
    fmt = struct.pack("<HHIIHHHHI", 0xFFFE, channels, rate, rate * block, block,
                      bits, 22, bits, mask) + guid
    return (b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(payload))
            + b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt
            + b"data" + struct.pack("<I", len(payload)) + payload)


def tone_file(mask):
    """A 16-bit WAVEX carrying one tone per speaker of the mask, in mask order."""
    names = speakers_of(mask)
    n = len(names)
    second = array.array("h", bytes(2 * n * RATE))
    for c, name in enumerate(names):
        step = 2.0 * math.pi * SPEAKERS[name].tone / RATE
        for i in range(RATE):
            second[i * n + c] = round(AMPLITUDE * 32767 * math.sin(step * i))
    if sys.byteorder == "big":
        second.byteswap()
    return wavex_bytes(n, RATE, mask, second.tobytes() * TONE_SECONDS, 16, PCM_GUID)


def read_wav(path):
    """Channels, rate, interleaved float samples and speaker mask of a WAV file."""
    data = Path(path).read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise CheckError(f"{path} is not a WAV file")
    fmt = payload = None
    pos = 12
    while pos + 8 <= len(data):
        chunk = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + size]
        if chunk == b"fmt ":
            fmt = body
        elif chunk == b"data":
            payload = body
        pos += 8 + size + (size & 1)
    if fmt is None or payload is None:
        raise CheckError(f"{path} has no fmt or data chunk")
    tag, channels, rate = struct.unpack_from("<HHI", fmt)
    bits = struct.unpack_from("<H", fmt, 14)[0]
    mask = 0
    if tag == 0xFFFE:
        mask = struct.unpack_from("<I", fmt, 20)[0]
        tag = struct.unpack_from("<H", fmt, 24)[0]
    if tag == 3 and bits == 32:
        samples = array.array("f")
        samples.frombytes(payload[:len(payload) - len(payload) % 4])
    elif tag == 1 and bits == 16:
        ints = array.array("h")
        ints.frombytes(payload[:len(payload) - len(payload) % 2])
        samples = array.array("f", (v / 32768.0 for v in ints))
    else:
        raise CheckError(f"{path}: format {tag} at {bits} bits is not handled")
    if sys.byteorder == "big":
        samples.byteswap()
    return channels, rate, samples, mask


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------

def goertzel(x, freq, rate):
    """Amplitude of one frequency in x, exact when x holds whole periods of it."""
    w = 2.0 * math.pi * freq / rate
    c = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s1, s2 = v + c * s1 - s2, s1
    real = s1 - s2 * math.cos(w)
    imag = s2 * math.sin(w)
    return 2.0 * math.hypot(real, imag) / len(x)


def steady_window(samples, channels, rate):
    """Start and length of a window inside the captured playback, or a reason."""
    block = rate // 100
    blocks = len(samples) // channels // block
    loud = []
    for b in range(blocks):
        seg = samples[b * block * channels:(b + 1) * block * channels]
        if max(seg) >= SILENT or -min(seg) >= SILENT:
            loud.append(b)
    if not loud:
        return None, ("every channel is silent: allow microphone access for the "
                      "terminal (System Settings > Privacy & Security > Microphone) "
                      "and run again")
    # Half a second in skips the device open; 0.2 s before the end skips the stop.
    start = (loud[0] + 50) * block
    length = rate // 10
    if start + length > (loud[-1] - 20) * block:
        return None, "the captured playback is too short to analyze"
    return (start, length), None


def classify(x, rate):
    """The speaker whose tone fills x, None for silence, or a description."""
    peak = max(max(x), -min(x))
    if peak < SILENT:
        return None
    ranked = sorted(((goertzel(x, s.tone, rate), name) for name, s in SPEAKERS.items()),
                    reverse=True)
    (a1, n1), (a2, n2) = ranked[0], ranked[1]
    if a2 > 0.1 * a1:
        return f"mix {n1}+{n2}"
    if a1 < 0.5 * peak:
        return "no test tone"
    return n1


def expected_channels(names, labels, channels):
    """The speaker expected on each device channel (None: silence), and conflicts."""
    by_label = {}
    for ch, label in enumerate(labels[:channels]):
        by_label.setdefault(label, []).append(ch)
    expected = [None] * channels
    conflicts = []
    for index, name in enumerate(names):
        hits = by_label.get(SPEAKERS[name].label, [])
        target = hits[0] if len(hits) == 1 else index
        if target >= channels:
            continue
        if expected[target] is not None:
            conflicts.append(f"the device configuration sends {expected[target]} and "
                             f"{name} to channel {target + 1}")
        expected[target] = name
    return expected, conflicts


def analyze(capture, names, labels):
    """Verdict, per-channel rows and notes for one captured run."""
    channels, rate, samples, _ = read_wav(capture)
    window, reason = steady_window(samples, channels, rate)
    if window is None:
        return False, [], [reason]
    start, length = window
    found = []
    for ch in range(channels):
        x = samples[start * channels + ch:(start + length) * channels:channels]
        found.append(classify(x, rate))
    labels = list(labels) + [LABEL_UNKNOWN] * max(0, channels - len(labels))
    expected, conflicts = expected_channels(names, labels, channels)
    rows = [(ch, labels[ch], expected[ch], found[ch]) for ch in range(channels)]
    ok = not conflicts and expected == found
    notes = list(conflicts)
    n = len(names)
    if not ok and found[:n] == list(names) and expected[:n] != list(names):
        notes.append("each channel kept its source index: the AUHAL did not follow the "
                     "device's speaker labels")
    return ok, rows, notes


def label_name(label):
    if label in SPEAKER_BY_LABEL:
        return SPEAKER_BY_LABEL[label]
    if label == LABEL_UNKNOWN:
        return "unknown"
    if label == 0:
        return "unused"
    if label >> 16 == 1:
        return f"discrete {label - LABEL_DISCRETE_BASE}"
    return f"label {label}"


def report(title, ok, rows, notes, width):
    print(f"{title}: {'PASS' if ok else 'FAIL'}")
    if rows:
        print("    ch  device label   expected  found")
    for ch, label, expected, found in rows:
        if ch >= width and expected is None and found is None:
            continue
        mark = "" if expected == found else "   <- differs"
        print(f"    {ch + 1:>2}  {label_name(label):<13}  {expected or 'silent':<8}  "
              f"{found or 'silent'}{mark}")
    for note in notes:
        print(f"    note: {note}")
    sys.stdout.flush()


# ---------------------------------------------------------------------------
# Self-test: the file writer, the reader and the analyzer, with no device
# ---------------------------------------------------------------------------

def simulated_capture(src, src_channels, rate, routing, channels=16):
    """A capture with routing[i] the device channel that received source channel i."""
    lead, body, tail = int(0.3 * rate), int(1.5 * rate), int(0.3 * rate)
    capture = array.array("f", bytes(4 * (lead + body + tail) * channels))
    for i, dst in enumerate(routing):
        start = lead * channels + dst
        capture[start:start + body * channels:channels] = src[i::src_channels][:body]
    return capture


def self_test():
    wrong = 0
    with tempfile.TemporaryDirectory(prefix="rawform-loopback-") as tmp:
        for layout, mask in LAYOUTS:
            names = speakers_of(mask)
            n = len(names)
            source = Path(tmp) / f"{slug(layout)}.wav"
            source.write_bytes(tone_file(mask))
            src_channels, rate, src, file_mask = read_wav(source)
            header_ok = (src_channels, rate, file_mask) == (n, RATE, mask)
            print(f"{layout} (mask {mask:#x}): file header "
                  f"{'as written' if header_ok else 'WRONG'}")
            wrong += 0 if header_ok else 1

            identity = list(range(n))
            swapped = identity[:]
            swapped[0], swapped[-1] = swapped[-1], swapped[0]
            reversed_labels = [SPEAKERS[name].label for name in reversed(names)]
            cases = [
                ("identity capture, device unlabeled", identity, [], True, None),
                ("first and last channels swapped", swapped, [], False, None),
                ("capture follows reversed labels", identity[::-1], reversed_labels,
                 True, None),
                ("capture keeps index under reversed labels", identity,
                 reversed_labels, False, "kept its source index"),
                ("silent capture", [], [], False, "microphone access"),
            ]
            for description, routing, labels, should_pass, note in cases:
                capture = Path(tmp) / "capture.wav"
                capture.write_bytes(wavex_bytes(
                    16, rate, 0, simulated_capture(src, n, rate, routing).tobytes(),
                    32, FLOAT_GUID))
                ok, _, notes = analyze(capture, names, labels)
                as_expected = ok == should_pass and (
                    note is None or any(note in text for text in notes))
                wrong += 0 if as_expected else 1
                verdict = "PASS" if ok else "FAIL"
                print(f"    {description}: {verdict}, "
                      f"{'as expected' if as_expected else 'WRONG'}")
    print("self-test:", "all as expected" if wrong == 0 else f"{wrong} WRONG")
    return 0 if wrong == 0 else 1


def write_fixtures(directory):
    directory.mkdir(parents=True, exist_ok=True)
    for layout, mask in LAYOUTS:
        path = directory / f"{slug(layout)}.wav"
        path.write_bytes(tone_file(mask))
        print(f"{path}  {layout}  mask {mask:#x}  {' '.join(speakers_of(mask))}")
    return 0


# ---------------------------------------------------------------------------
# The device run
# ---------------------------------------------------------------------------

def build_helper():
    if HELPER.exists() and HELPER.stat().st_mtime >= HELPER_SOURCE.stat().st_mtime:
        return
    HELPER.parent.mkdir(parents=True, exist_ok=True)
    print(f"compiling {HELPER_SOURCE.relative_to(REPO)}")
    result = subprocess.run(
        ["swiftc", "-O", "-module-cache-path", str(HELPER.parent / "module-cache"),
         "-o", str(HELPER), str(HELPER_SOURCE)],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise CheckError("swiftc failed:\n" + result.stderr)


def helper(*args):
    result = subprocess.run([str(HELPER), *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise CheckError(result.stderr.strip() or f"the capture helper failed: {args}")
    return result.stdout


class Cli:
    """rawform_audio_cli's interactive mode, driven over stdin."""

    def __init__(self, path):
        self.log = []
        self._lines = queue.Queue()
        self._proc = subprocess.Popen(
            [str(path), "interactive"], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self._proc.stdout:
            # The prompt ends without a newline, so it prefixes the next output line.
            line = PROMPT.sub("", line.rstrip("\n"))
            self.log.append(line)
            self._lines.put(line)
        self._lines.put(None)

    def send(self, command):
        self._proc.stdin.write(command + "\n")
        self._proc.stdin.flush()

    def _next_line(self, deadline):
        timeout = deadline - time.monotonic()
        if timeout <= 0:
            return None
        try:
            return self._lines.get(timeout=timeout)
        except queue.Empty:
            return None

    def select_device(self, name):
        self.send("devices")
        deadline = time.monotonic() + 5
        index = None
        while (line := self._next_line(deadline)) is not None:
            match = DEVICE_LINE.match(line)
            if match and match.group(2) == name:
                index = int(match.group(1))
            if line.strip().startswith("(select with"):
                break
        if index is None:
            raise CheckError(f"rawform_audio_cli does not list a device named \"{name}\"")
        self.send(f"device {index}")

    def close(self):
        try:
            self.send("stop")
            self.send("quit")
            self._proc.stdin.close()
        except OSError:
            pass
        try:
            self._proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self._proc.kill()
            self._proc.wait()


def play_and_capture(cli_path, device, source, capture, cli_log):
    record = subprocess.Popen(
        [str(HELPER), "record", device, str(TONE_SECONDS + 5), str(capture)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if not record.stdout.readline().startswith("recording"):
        record.wait()
        raise CheckError(record.stderr.read().strip() or "the capture did not start")
    cli = Cli(cli_path)
    try:
        cli.select_device(device)
        time.sleep(0.5)
        cli.send(f"play {source}")
        time.sleep(TONE_SECONDS + 1.5)
    finally:
        cli.close()
        cli_log.write_text("\n".join(cli.log) + "\n")
    record.wait(timeout=TONE_SECONDS + 20)
    if record.returncode != 0:
        raise CheckError(record.stderr.read().strip() or "the capture failed")
    return [line for line in cli.log
            if ("CoreAudioSink:" in line and "failed" in line)
            or line.startswith("[error]")]


def run_layout(args, work, layout, mask, labels, tag):
    names = speakers_of(mask)
    stem = f"{slug(layout)}-{tag}"
    source = work / f"{slug(layout)}.wav"
    if not source.exists():
        source.write_bytes(tone_file(mask))
    capture = work / f"{stem}-capture.wav"
    cli_log = work / f"{stem}-cli.log"
    breadcrumbs = play_and_capture(args.cli, args.device, source, capture, cli_log)
    ok, rows, notes = analyze(capture, names, labels)
    if breadcrumbs:
        ok = False
        notes += [f"rawform reported: {line}" for line in breadcrumbs]
    if not ok:
        notes.append(f"CLI output in {cli_log}")
    return ok, rows, notes


def run_device(args):
    if not Path(args.cli).exists():
        raise CheckError(f"{args.cli} not found: build the engine "
                         "(cmake -S audio -B build/core) or pass --cli")
    build_helper()
    info = json.loads(helper("layout", args.device))
    layouts = [(name, mask) for name, mask in LAYOUTS
               if not args.only or name in args.only]
    widest = max(len(speakers_of(mask)) for _, mask in layouts)
    if info["output_channels"] < widest or info["input_channels"] < widest:
        raise CheckError(f"\"{args.device}\" has {info['output_channels']} outputs and "
                         f"{info['input_channels']} inputs; {widest} are needed")
    work = Path(args.work) if args.work else Path(
        tempfile.mkdtemp(prefix="rawform-loopback-"))
    work.mkdir(parents=True, exist_ok=True)
    original_rate = int(info["rate"])
    original_layout = info["raw"]
    found_labels = info["labels"]
    print(f"device \"{args.device}\": {info['output_channels']} channels, "
          f"{original_rate} Hz, speakers as found: "
          f"{' '.join(label_name(x) for x in found_labels) or 'none'}")
    print(f"files in {work}")

    results = []
    try:
        if original_rate != RATE:
            helper("set-rate", args.device, str(RATE))
        for layout, mask in layouts:
            names = speakers_of(mask)
            title = f"{layout} (mask {mask:#x})"

            ok, rows, notes = run_layout(args, work, layout, mask, found_labels, "found")
            report(f"{title}, speakers as found", ok, rows, notes, len(names))
            results.append((layout, "as found", ok))

            if not info["settable"] or not original_layout:
                print(f"{title}, reversed speakers: skipped, the device's speaker "
                      "configuration cannot be set and restored")
                results.append((layout, "reversed", None))
                continue
            wanted = [SPEAKERS[name].label for name in reversed(names)]
            device_labels = wanted + [LABEL_UNKNOWN] * (info["output_channels"] - len(names))
            helper("set-layout", args.device, ",".join(str(x) for x in device_labels))
            held = json.loads(helper("layout", args.device))["labels"]
            if held[:len(names)] != wanted:
                print(f"{title}, reversed speakers: skipped, the device did not keep "
                      "the speaker configuration")
                results.append((layout, "reversed", None))
            else:
                ok, rows, notes = run_layout(args, work, layout, mask, held, "reversed")
                report(f"{title}, reversed speakers", ok, rows, notes, len(names))
                results.append((layout, "reversed", ok))
            helper("restore-layout", args.device, original_layout)
    finally:
        if info["settable"] and original_layout:
            try:
                helper("restore-layout", args.device, original_layout)
            except CheckError as error:
                print(f"warning: the speaker configuration was not restored: {error}")
        if original_rate != RATE:
            try:
                helper("set-rate", args.device, str(original_rate))
            except CheckError as error:
                print(f"warning: the rate was not restored to {original_rate} Hz: {error}")

    print("summary")
    by_layout = {}
    for layout, config, ok in results:
        by_layout.setdefault(layout, []).append(
            f"{config}: {'skipped' if ok is None else 'PASS' if ok else 'FAIL'}")
    for layout, cells in by_layout.items():
        print(f"    {layout:<12} " + "   ".join(cells))
    return 0 if all(ok is not False for _, _, ok in results) else 1


def main():
    parser = argparse.ArgumentParser(
        description="Loopback check of rawform's macOS speaker layouts.")
    parser.add_argument("--cli", default=str(DEFAULT_CLI),
                        help="the rawform_audio_cli to drive (default: %(default)s)")
    parser.add_argument("--device", default=DEFAULT_DEVICE,
                        help="the loopback device (default: %(default)s)")
    parser.add_argument("--only", action="append", choices=[name for name, _ in LAYOUTS],
                        help="run this layout only; repeatable")
    parser.add_argument("--work", help="keep the files in this directory")
    parser.add_argument("--self-test", action="store_true",
                        help="check the file writer and the analyzer, with no device")
    parser.add_argument("--write-fixtures", metavar="DIR",
                        help="write each layout's WAVEX file into DIR and stop")
    args = parser.parse_args()
    try:
        if args.self_test:
            return self_test()
        if args.write_fixtures:
            return write_fixtures(Path(args.write_fixtures))
        if sys.platform != "darwin":
            raise CheckError("the loopback check runs on macOS only")
        return run_device(args)
    except CheckError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
