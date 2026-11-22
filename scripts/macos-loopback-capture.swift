/*
 * This file is part of rawform.
 * Copyright (C) 2026 Etienne Fleurant
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// macos-loopback-capture.swift
//
// The CoreAudio half of scripts/macos-loopback-layouts.py: it records every input
// channel of a loopback device, and reads or sets the sample rate and the speaker
// configuration of the device's output side. Python's standard library has no
// CoreAudio access, so this small tool carries it; the script compiles it with
// swiftc into build/loopback/.
//
// Usage:
//   macos-loopback-capture list
//   macos-loopback-capture layout <device>
//   macos-loopback-capture set-layout <device> <label,label,...>
//   macos-loopback-capture restore-layout <device> <hex>
//   macos-loopback-capture set-rate <device> <hz>
//   macos-loopback-capture record <device> <seconds> <out.wav>
//
// `layout` prints one line of JSON: the device's channel counts and nominal rate,
// whether its speaker configuration is settable, that configuration expanded to
// one CoreAudio label per channel, and the raw property bytes in hex, which
// `restore-layout` writes back unchanged. `record` prints a line once the capture
// runs, so the caller knows when to start playback, and writes a float32 WAV.

import AudioToolbox
import CoreAudio
import Foundation

let usage = """
    usage: macos-loopback-capture list
           macos-loopback-capture layout <device>
           macos-loopback-capture set-layout <device> <label,label,...>
           macos-loopback-capture restore-layout <device> <hex>
           macos-loopback-capture set-rate <device> <hz>
           macos-loopback-capture record <device> <seconds> <out.wav>
    """

// Every failure ends the run with one line on stderr; the calling script
// reports it.
func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data("macos-loopback-capture: \(message)\n".utf8))
    exit(1)
}

func check(_ status: OSStatus, _ what: String) {
    if status != noErr {
        fail("\(what) failed (OSStatus \(status))")
    }
}

func address(_ selector: AudioObjectPropertySelector,
             _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal)
    -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(mSelector: selector, mScope: scope,
                               mElement: kAudioObjectPropertyElementMain)
}

func propertyBytes(_ object: AudioObjectID,
                   _ addr: AudioObjectPropertyAddress) -> [UInt8]? {
    var addr = addr
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(object, &addr, 0, nil, &size) == noErr else {
        return nil
    }
    if size == 0 {
        return []
    }
    var bytes = [UInt8](repeating: 0, count: Int(size))
    let status = bytes.withUnsafeMutableBytes {
        AudioObjectGetPropertyData(object, &addr, 0, nil, &size, $0.baseAddress!)
    }
    guard status == noErr else {
        return nil
    }
    return Array(bytes.prefix(Int(size)))
}

func stringProperty(_ object: AudioObjectID,
                    _ selector: AudioObjectPropertySelector) -> String? {
    var addr = address(selector)
    var value: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    let status = withUnsafeMutablePointer(to: &value) {
        AudioObjectGetPropertyData(object, &addr, 0, nil, &size, $0)
    }
    guard status == noErr, let value else {
        return nil
    }
    return value.takeRetainedValue() as String
}

func channelCount(_ device: AudioObjectID, _ scope: AudioObjectPropertyScope) -> Int {
    var addr = address(kAudioDevicePropertyStreamConfiguration, scope)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(device, &addr, 0, nil, &size) == noErr,
          size > 0 else {
        return 0
    }
    // An AudioBufferList holds pointers, so it needs pointer alignment, which a
    // byte array does not promise.
    let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: 16)
    defer { raw.deallocate() }
    guard AudioObjectGetPropertyData(device, &addr, 0, nil, &size, raw) == noErr else {
        return 0
    }
    let list = UnsafeMutableAudioBufferListPointer(
        raw.assumingMemoryBound(to: AudioBufferList.self))
    return list.reduce(0) { $0 + Int($1.mNumberChannels) }
}

func nominalRate(_ device: AudioObjectID) -> Double {
    var addr = address(kAudioDevicePropertyNominalSampleRate)
    var rate: Float64 = 0
    var size = UInt32(MemoryLayout<Float64>.size)
    check(AudioObjectGetPropertyData(device, &addr, 0, nil, &size, &rate),
          "reading the sample rate")
    return rate
}

struct Device {
    let id: AudioObjectID
    let name: String
    let uid: String
}

func devices() -> [Device] {
    guard let bytes = propertyBytes(AudioObjectID(kAudioObjectSystemObject),
                                    address(kAudioHardwarePropertyDevices)) else {
        fail("cannot list the audio devices")
    }
    var ids = [AudioObjectID](repeating: 0,
                              count: bytes.count / MemoryLayout<AudioObjectID>.size)
    ids.withUnsafeMutableBytes { $0.copyBytes(from: bytes.prefix($0.count)) }
    return ids.compactMap { id in
        guard let name = stringProperty(id, kAudioObjectPropertyName),
              let uid = stringProperty(id, kAudioDevicePropertyDeviceUID) else {
            return nil
        }
        return Device(id: id, name: name, uid: uid)
    }
}

func device(named name: String) -> Device {
    guard let found = devices().first(where: { $0.name == name }) else {
        fail("no audio device named \"\(name)\"; `list` prints the names")
    }
    return found
}

func listDevices() {
    for d in devices() {
        let inputs = channelCount(d.id, kAudioObjectPropertyScopeInput)
        let outputs = channelCount(d.id, kAudioObjectPropertyScopeOutput)
        print("\(d.name)  (in \(inputs), out \(outputs), \(Int(nominalRate(d.id))) Hz)")
    }
}

func readUInt32(_ bytes: [UInt8], _ offset: Int) -> UInt32 {
    (0..<4).reduce(UInt32(0)) { $0 | UInt32(bytes[offset + $1]) << (8 * $1) }
}

func appendUInt32(_ value: UInt32, to bytes: inout [UInt8]) {
    withUnsafeBytes(of: value.littleEndian) { bytes.append(contentsOf: $0) }
}

// An AudioChannelLayout is a 12-byte header (tag, bitmap, description count)
// followed by 20-byte descriptions whose first field is the label.
func descriptionLabels(_ layout: [UInt8]) -> [UInt32] {
    guard layout.count >= 12 else {
        return []
    }
    let count = Int(readUInt32(layout, 8))
    return (0..<count).compactMap { i in
        let at = 12 + 20 * i
        return at + 4 <= layout.count ? readUInt32(layout, at) : nil
    }
}

func formatLayout(_ property: AudioFormatPropertyID, _ specifier: UInt32) -> [UInt8] {
    var specifier = specifier
    var size: UInt32 = 0
    guard AudioFormatGetPropertyInfo(property, 4, &specifier, &size) == noErr,
          size > 0 else {
        return []
    }
    var out = [UInt8](repeating: 0, count: Int(size))
    let status = out.withUnsafeMutableBytes {
        AudioFormatGetProperty(property, 4, &specifier, &size, $0.baseAddress!)
    }
    return status == noErr ? out : []
}

// A layout given as a tag or a bitmap names its speakers only implicitly;
// AudioToolbox expands both into descriptions.
func labels(ofLayout layout: [UInt8]) -> [UInt32] {
    guard layout.count >= 12 else {
        return []
    }
    let tag = readUInt32(layout, 0)
    if tag == kAudioChannelLayoutTag_UseChannelDescriptions {
        return descriptionLabels(layout)
    }
    if tag == kAudioChannelLayoutTag_UseChannelBitmap {
        return descriptionLabels(
            formatLayout(kAudioFormatProperty_ChannelLayoutForBitmap, readUInt32(layout, 4)))
    }
    return descriptionLabels(formatLayout(kAudioFormatProperty_ChannelLayoutForTag, tag))
}

func layoutBytes(labels: [UInt32]) -> [UInt8] {
    var out = [UInt8]()
    appendUInt32(kAudioChannelLayoutTag_UseChannelDescriptions, to: &out)
    appendUInt32(0, to: &out)
    appendUInt32(UInt32(labels.count), to: &out)
    for label in labels {
        appendUInt32(label, to: &out)
        // Flags, then three coordinates; zero bits are 0.0 in a Float32.
        for _ in 0..<4 {
            appendUInt32(0, to: &out)
        }
    }
    return out
}

func layoutAddress() -> AudioObjectPropertyAddress {
    address(kAudioDevicePropertyPreferredChannelLayout, kAudioObjectPropertyScopeOutput)
}

func printLayout(_ dev: Device) {
    var addr = layoutAddress()
    let raw = propertyBytes(dev.id, addr) ?? []
    var settable: DarwinBoolean = false
    let canSet = AudioObjectIsPropertySettable(dev.id, &addr, &settable) == noErr
        && settable.boolValue
    let object: [String: Any] = [
        "name": dev.name,
        "input_channels": channelCount(dev.id, kAudioObjectPropertyScopeInput),
        "output_channels": channelCount(dev.id, kAudioObjectPropertyScopeOutput),
        "rate": nominalRate(dev.id),
        "settable": canSet,
        "labels": labels(ofLayout: raw).map { Int($0) },
        "raw": raw.map { String(format: "%02x", $0) }.joined(),
    ]
    guard let json = try? JSONSerialization.data(withJSONObject: object,
                                                 options: [.sortedKeys]) else {
        fail("cannot encode the layout of \"\(dev.name)\"")
    }
    print(String(decoding: json, as: UTF8.self))
}

func setLayout(_ dev: Device, _ layout: [UInt8]) {
    var addr = layoutAddress()
    var copy = layout
    let status = copy.withUnsafeMutableBytes {
        AudioObjectSetPropertyData(dev.id, &addr, 0, nil, UInt32($0.count), $0.baseAddress!)
    }
    check(status, "setting the speaker configuration of \"\(dev.name)\"")
}

func parseLabels(_ text: String) -> [UInt32] {
    text.split(separator: ",").map { field in
        guard let label = UInt32(field) else {
            fail("\"\(field)\" is not a channel label")
        }
        return label
    }
}

func bytes(fromHex hex: String) -> [UInt8] {
    let digits = Array(hex)
    guard digits.count % 2 == 0 else {
        fail("the saved layout has an odd number of hex digits")
    }
    return stride(from: 0, to: digits.count, by: 2).map { i in
        guard let byte = UInt8(String(digits[i...i + 1]), radix: 16) else {
            fail("the saved layout is not hex")
        }
        return byte
    }
}

func setRate(_ dev: Device, _ rate: Double) {
    var addr = address(kAudioDevicePropertyNominalSampleRate)
    var value = rate
    check(AudioObjectSetPropertyData(dev.id, &addr, 0, nil,
                                     UInt32(MemoryLayout<Float64>.size), &value),
          "setting the sample rate of \"\(dev.name)\"")
    // The HAL applies a rate change asynchronously.
    var tries = 0
    while nominalRate(dev.id) != rate && tries < 50 {
        Thread.sleep(forTimeInterval: 0.05)
        tries += 1
    }
    if nominalRate(dev.id) != rate {
        fail("\"\(dev.name)\" did not switch to \(Int(rate)) Hz")
    }
}

func writeWav(_ path: String, _ samples: Data, channels: Int, rate: Int) {
    var out = [UInt8]()
    func tag(_ text: String) { out.append(contentsOf: Array(text.utf8)) }
    func u16(_ value: Int) {
        withUnsafeBytes(of: UInt16(value).littleEndian) { out.append(contentsOf: $0) }
    }
    tag("RIFF")
    appendUInt32(UInt32(4 + 8 + 40 + 8 + samples.count), to: &out)
    tag("WAVE")
    tag("fmt ")
    appendUInt32(40, to: &out)
    u16(0xFFFE)
    u16(channels)
    appendUInt32(UInt32(rate), to: &out)
    appendUInt32(UInt32(rate * channels * 4), to: &out)
    u16(channels * 4)
    u16(32)
    u16(22)
    u16(32)
    appendUInt32(0, to: &out)
    // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    out.append(contentsOf: [0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                            0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71])
    tag("data")
    appendUInt32(UInt32(samples.count), to: &out)
    var file = Data(out)
    file.append(samples)
    do {
        try file.write(to: URL(fileURLWithPath: path))
    } catch {
        fail("cannot write \(path): \(error.localizedDescription)")
    }
}

// Records at the device's own rate, so the queue converts nothing and each
// captured channel is the device channel as rawform wrote it.
func record(_ dev: Device, seconds: Double, to path: String) {
    let channels = channelCount(dev.id, kAudioObjectPropertyScopeInput)
    guard channels > 0 else {
        fail("\"\(dev.name)\" has no input channels")
    }
    let rate = nominalRate(dev.id)
    var format = AudioStreamBasicDescription(
        mSampleRate: rate, mFormatID: kAudioFormatLinearPCM,
        mFormatFlags: kLinearPCMFormatFlagIsFloat | kLinearPCMFormatFlagIsPacked,
        mBytesPerPacket: UInt32(4 * channels), mFramesPerPacket: 1,
        mBytesPerFrame: UInt32(4 * channels), mChannelsPerFrame: UInt32(channels),
        mBitsPerChannel: 32, mReserved: 0)

    let callbacks = DispatchQueue(label: "macos-loopback-capture")
    var captured = Data()
    var queue: AudioQueueRef?
    let created = AudioQueueNewInputWithDispatchQueue(&queue, &format, 0, callbacks) {
        q, buffer, _, _, _ in
        captured.append(buffer.pointee.mAudioData.assumingMemoryBound(to: UInt8.self),
                        count: Int(buffer.pointee.mAudioDataByteSize))
        AudioQueueEnqueueBuffer(q, buffer, 0, nil)
    }
    check(created, "creating the capture queue")
    guard let q = queue else {
        fail("creating the capture queue returned no queue")
    }
    // The property's value is a CFStringRef, so the queue gets the reference's
    // address, not the string's bytes.
    var uid = dev.uid as CFString
    let selected = withUnsafePointer(to: &uid) {
        AudioQueueSetProperty(q, kAudioQueueProperty_CurrentDevice, $0,
                              UInt32(MemoryLayout<CFString>.size))
    }
    check(selected, "selecting \"\(dev.name)\" for capture")

    let bufferBytes = UInt32(Int(rate / 20) * 4 * channels)
    for _ in 0..<4 {
        var buffer: AudioQueueBufferRef?
        check(AudioQueueAllocateBuffer(q, bufferBytes, &buffer), "allocating a capture buffer")
        guard let buffer else {
            fail("allocating a capture buffer returned no buffer")
        }
        check(AudioQueueEnqueueBuffer(q, buffer, 0, nil), "queueing a capture buffer")
    }
    check(AudioQueueStart(q, nil), "starting the capture")
    print("recording \(channels) channels at \(Int(rate)) Hz")
    fflush(stdout)

    Thread.sleep(forTimeInterval: seconds)
    check(AudioQueueStop(q, true), "stopping the capture")
    let samples = callbacks.sync { captured }
    AudioQueueDispose(q, true)
    writeWav(path, samples, channels: channels, rate: Int(rate))
}

let arguments = Array(CommandLine.arguments.dropFirst())
switch (arguments.first ?? "", arguments.count) {
case ("list", 1):
    listDevices()
case ("layout", 2):
    printLayout(device(named: arguments[1]))
case ("set-layout", 3):
    setLayout(device(named: arguments[1]), layoutBytes(labels: parseLabels(arguments[2])))
case ("restore-layout", 3):
    setLayout(device(named: arguments[1]), bytes(fromHex: arguments[2]))
case ("set-rate", 3):
    guard let rate = Double(arguments[2]), rate > 0 else {
        fail("\"\(arguments[2])\" is not a sample rate")
    }
    setRate(device(named: arguments[1]), rate)
case ("record", 4):
    guard let seconds = Double(arguments[2]), seconds > 0 else {
        fail("\"\(arguments[2])\" is not a duration in seconds")
    }
    record(device(named: arguments[1]), seconds: seconds, to: arguments[3])
default:
    fail(usage)
}
