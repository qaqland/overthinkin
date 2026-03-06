import CoreAudio
import Foundation

// MARK: - Helper: Convert 4-byte UInt32 to FourCharCode string
func fourCC(_ value: UInt32) -> String {
    let chars: [Character] = [
        Character(UnicodeScalar((value >> 24) & 0xFF)!),
        Character(UnicodeScalar((value >> 16) & 0xFF)!),
        Character(UnicodeScalar((value >> 8) & 0xFF)!),
        Character(UnicodeScalar(value & 0xFF)!),
    ]
    let s = String(chars)
    // Return hex if contains non-printable characters
    if s.allSatisfy({ $0.isASCII && !$0.isNewline && $0 != "\0" }) {
        return "'\(s)'"
    }
    return String(format: "0x%08X", value)
}

// MARK: - Helper: Get string property
func getStringProperty(_ deviceID: AudioObjectID, selector: AudioObjectPropertySelector,
                       scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
                       element: AudioObjectPropertyElement = kAudioObjectPropertyElementMain) -> String?
{
    var address = AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: element)
    guard AudioObjectHasProperty(deviceID, &address) else { return nil }

    var name: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    let status = AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &name)
    guard status == noErr, let cfStr = name?.takeUnretainedValue() else { return nil }
    return cfStr as String
}

// MARK: - Helper: Get UInt32 property
func getUInt32Property(_ deviceID: AudioObjectID, selector: AudioObjectPropertySelector,
                       scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
                       element: AudioObjectPropertyElement = kAudioObjectPropertyElementMain) -> UInt32?
{
    var address = AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: element)
    guard AudioObjectHasProperty(deviceID, &address) else { return nil }

    var value: UInt32 = 0
    var size = UInt32(MemoryLayout<UInt32>.size)
    let status = AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &value)
    return status == noErr ? value : nil
}

// MARK: - Helper: Get Float32 property
func getFloat32Property(_ deviceID: AudioObjectID, selector: AudioObjectPropertySelector,
                        scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
                        element: AudioObjectPropertyElement = kAudioObjectPropertyElementMain) -> Float32?
{
    var address = AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: element)
    guard AudioObjectHasProperty(deviceID, &address) else { return nil }

    var value: Float32 = 0
    var size = UInt32(MemoryLayout<Float32>.size)
    let status = AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &value)
    return status == noErr ? value : nil
}

// MARK: - Helper: Get Float64 property
func getFloat64Property(_ deviceID: AudioObjectID, selector: AudioObjectPropertySelector,
                        scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
                        element: AudioObjectPropertyElement = kAudioObjectPropertyElementMain) -> Float64?
{
    var address = AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: element)
    guard AudioObjectHasProperty(deviceID, &address) else { return nil }

    var value: Float64 = 0
    var size = UInt32(MemoryLayout<Float64>.size)
    let status = AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &value)
    return status == noErr ? value : nil
}

// MARK: - Helper: Get pid_t (Int32) property
func getPidProperty(_ deviceID: AudioObjectID, selector: AudioObjectPropertySelector,
                    scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
                    element: AudioObjectPropertyElement = kAudioObjectPropertyElementMain) -> pid_t?
{
    var address = AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: element)
    guard AudioObjectHasProperty(deviceID, &address) else { return nil }

    var value: pid_t = 0
    var size = UInt32(MemoryLayout<pid_t>.size)
    let status = AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &value)
    return status == noErr ? value : nil
}

// MARK: - Get channel count
func getChannelCount(_ deviceID: AudioObjectID, scope: AudioObjectPropertyScope) -> Int {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyStreamConfiguration,
        mScope: scope,
        mElement: kAudioObjectPropertyElementMain
    )
    guard AudioObjectHasProperty(deviceID, &address) else { return 0 }

    var size: UInt32 = 0
    var status = AudioObjectGetPropertyDataSize(deviceID, &address, 0, nil, &size)
    guard status == noErr, size > 0 else { return 0 }

    // AudioBufferList's mBuffers is a variable-length tail array, must allocate by returned size
    let mem = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: MemoryLayout<AudioBufferList>.alignment)
    defer { mem.deallocate() }

    status = AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, mem)
    guard status == noErr else { return 0 }

    var channels = 0

    let listPtr = mem.bindMemory(to: AudioBufferList.self, capacity: 1)
    let buffers = UnsafeMutableAudioBufferListPointer(listPtr)
    for buffer in buffers {
        channels += Int(buffer.mNumberChannels)
    }
    return channels
}

// MARK: - Get Data Source name
func getDataSourceName(_ deviceID: AudioObjectID, scope: AudioObjectPropertyScope, sourceID: UInt32) -> String? {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyDataSourceNameForIDCFString,
        mScope: scope,
        mElement: kAudioObjectPropertyElementMain
    )
    guard AudioObjectHasProperty(deviceID, &address) else { return nil }

    var source = sourceID
    var name: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<AudioValueTranslation>.size)

    let status = withUnsafeMutablePointer(to: &source) { sourcePtr in
        withUnsafeMutablePointer(to: &name) { namePtr in
            var translation = AudioValueTranslation(
                mInputData: sourcePtr,
                mInputDataSize: UInt32(MemoryLayout<UInt32>.size),
                mOutputData: namePtr,
                mOutputDataSize: UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
            )
            return AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &translation)
        }
    }

    guard status == noErr, let cf = name?.takeUnretainedValue() else { return nil }
    return cf as String
}

// MARK: - Get available sample rate ranges
func getAvailableSampleRates(_ deviceID: AudioObjectID) -> [AudioValueRange]? {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyAvailableNominalSampleRates,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    guard AudioObjectHasProperty(deviceID, &address) else { return nil }

    var size: UInt32 = 0
    var status = AudioObjectGetPropertyDataSize(deviceID, &address, 0, nil, &size)
    guard status == noErr, size > 0 else { return nil }

    let count = Int(size) / MemoryLayout<AudioValueRange>.size
    var ranges = [AudioValueRange](repeating: AudioValueRange(), count: count)
    status = AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &ranges)
    return status == noErr ? ranges : nil
}

// MARK: - Get stream list
func getStreams(_ deviceID: AudioObjectID, scope: AudioObjectPropertyScope) -> [AudioStreamID]? {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyStreams,
        mScope: scope,
        mElement: kAudioObjectPropertyElementMain
    )
    guard AudioObjectHasProperty(deviceID, &address) else { return nil }

    var size: UInt32 = 0
    var status = AudioObjectGetPropertyDataSize(deviceID, &address, 0, nil, &size)
    guard status == noErr, size > 0 else { return nil }

    let count = Int(size) / MemoryLayout<AudioStreamID>.size
    var streamIDs = [AudioStreamID](repeating: 0, count: count)
    status = AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &streamIDs)
    return status == noErr ? streamIDs : nil
}

// MARK: - Get stream physical format
func getStreamPhysicalFormat(_ streamID: AudioStreamID) -> AudioStreamBasicDescription? {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioStreamPropertyPhysicalFormat,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    guard AudioObjectHasProperty(streamID, &address) else { return nil }

    var asbd = AudioStreamBasicDescription()
    var size = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
    let status = AudioObjectGetPropertyData(streamID, &address, 0, nil, &size, &asbd)
    return status == noErr ? asbd : nil
}

// MARK: - Get stream virtual format
func getStreamVirtualFormat(_ streamID: AudioStreamID) -> AudioStreamBasicDescription? {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioStreamPropertyVirtualFormat,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    guard AudioObjectHasProperty(streamID, &address) else { return nil }

    var asbd = AudioStreamBasicDescription()
    var size = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
    let status = AudioObjectGetPropertyData(streamID, &address, 0, nil, &size, &asbd)
    return status == noErr ? asbd : nil
}

// MARK: - TransportType to readable string
func transportTypeName(_ type: UInt32) -> String {
    switch type {
    case kAudioDeviceTransportTypeBuiltIn:      return "Built-In"
    case kAudioDeviceTransportTypeAggregate:     return "Aggregate"
    case kAudioDeviceTransportTypeVirtual:       return "Virtual"
    case kAudioDeviceTransportTypePCI:           return "PCI"
    case kAudioDeviceTransportTypeUSB:           return "USB"
    case kAudioDeviceTransportTypeFireWire:      return "FireWire"
    case kAudioDeviceTransportTypeBluetooth:     return "Bluetooth"
    case kAudioDeviceTransportTypeBluetoothLE:   return "Bluetooth LE"
    case kAudioDeviceTransportTypeHDMI:          return "HDMI"
    case kAudioDeviceTransportTypeDisplayPort:   return "DisplayPort"
    case kAudioDeviceTransportTypeAirPlay:       return "AirPlay"
    case kAudioDeviceTransportTypeAVB:           return "AVB"
    case kAudioDeviceTransportTypeThunderbolt:   return "Thunderbolt"
    default:                                     return fourCC(type)
    }
}

// MARK: - Format AudioStreamBasicDescription
func formatASBD(_ asbd: AudioStreamBasicDescription) -> String {
    var parts: [String] = []
    parts.append("SampleRate: \(asbd.mSampleRate) Hz")
    parts.append("FormatID: \(fourCC(asbd.mFormatID))")
    parts.append("FormatFlags: \(String(format: "0x%X", asbd.mFormatFlags))")
    parts.append("BitsPerChannel: \(asbd.mBitsPerChannel)")
    parts.append("ChannelsPerFrame: \(asbd.mChannelsPerFrame)")
    parts.append("FramesPerPacket: \(asbd.mFramesPerPacket)")
    parts.append("BytesPerFrame: \(asbd.mBytesPerFrame)")
    parts.append("BytesPerPacket: \(asbd.mBytesPerPacket)")
    return parts.joined(separator: ", ")
}

// MARK: - Print per-channel volume properties for a scope
func printPerChannelVolumes(_ deviceID: AudioObjectID, scope: AudioObjectPropertyScope, scopeName: String) {
    let channelCount = getChannelCount(deviceID, scope: scope)
    guard channelCount > 0 else { return }

    for ch in 1...UInt32(channelCount) {
        let scalar = getFloat32Property(deviceID, selector: kAudioDevicePropertyVolumeScalar, scope: scope, element: ch)
        let dB = getFloat32Property(deviceID, selector: kAudioDevicePropertyVolumeDecibels, scope: scope, element: ch)
        let mute = getUInt32Property(deviceID, selector: kAudioDevicePropertyMute, scope: scope, element: ch)

        if scalar != nil || dB != nil || mute != nil {
            var info = "    [\(scopeName)] Channel \(ch):"
            if let s = scalar { info += "  Volume(scalar)=\(String(format: "%.4f", s))" }
            if let d = dB { info += "  Volume(dB)=\(String(format: "%.2f", d)) dB" }
            if let m = mute { info += "  Mute=\(m != 0 ? "YES" : "NO")" }
            print(info)
        }
    }

    // Master channel (element 0)
    let masterScalar = getFloat32Property(deviceID, selector: kAudioDevicePropertyVolumeScalar, scope: scope, element: kAudioObjectPropertyElementMain)
    let masterDB = getFloat32Property(deviceID, selector: kAudioDevicePropertyVolumeDecibels, scope: scope, element: kAudioObjectPropertyElementMain)
    let masterMute = getUInt32Property(deviceID, selector: kAudioDevicePropertyMute, scope: scope, element: kAudioObjectPropertyElementMain)

    if masterScalar != nil || masterDB != nil || masterMute != nil {
        var info = "    [\(scopeName)] Master:"
        if let s = masterScalar { info += "  Volume(scalar)=\(String(format: "%.4f", s))" }
        if let d = masterDB { info += "  Volume(dB)=\(String(format: "%.2f", d)) dB" }
        if let m = masterMute { info += "  Mute=\(m != 0 ? "YES" : "NO")" }
        print(info)
    }
}

// MARK: - Main function: Get all devices and print properties

func getAllDevices() -> [AudioObjectID] {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyDevices,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var size: UInt32 = 0
    var status = AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size)
    guard status == noErr, size > 0 else { return [] }

    let count = Int(size) / MemoryLayout<AudioObjectID>.size
    var devices = [AudioObjectID](repeating: 0, count: count)
    status = AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &devices)
    return status == noErr ? devices : []
}

func getDefaultDevice(selector: AudioObjectPropertySelector) -> AudioObjectID? {
    var address = AudioObjectPropertyAddress(
        mSelector: selector,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var deviceID: AudioObjectID = kAudioObjectUnknown
    var size = UInt32(MemoryLayout<AudioObjectID>.size)
    let status = AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &deviceID)
    return status == noErr ? deviceID : nil
}

func main() {
    let defaultInput = getDefaultDevice(selector: kAudioHardwarePropertyDefaultInputDevice)
    let defaultOutput = getDefaultDevice(selector: kAudioHardwarePropertyDefaultOutputDevice)
    let defaultSystem = getDefaultDevice(selector: kAudioHardwarePropertyDefaultSystemOutputDevice)

    print("========================================")
    print("  macOS Audio Device Inspector")
    print("========================================")
    print()
    if let d = defaultInput  { print("Default Input Device ID:        \(d)") }
    if let d = defaultOutput { print("Default Output Device ID:       \(d)") }
    if let d = defaultSystem { print("Default System Output Device ID: \(d)") }
    print()

    let devices = getAllDevices()
    print("Total devices found: \(devices.count)")
    print()

    for (index, deviceID) in devices.enumerated() {
        let separator = "────────────────────────────────────────"
        print(separator)
        print("Device #\(index + 1)  (AudioObjectID: \(deviceID))")
        print(separator)

        // -- Basic identification info --
        if let name = getStringProperty(deviceID, selector: kAudioObjectPropertyName) {
            var marker = ""
            if deviceID == defaultInput  { marker += " ◀ Default Input" }
            if deviceID == defaultOutput { marker += " ◀ Default Output" }
            if deviceID == defaultSystem { marker += " ◀ Default System" }
            print("  Name:             \(name)\(marker)")
        }

        if let manufacturer = getStringProperty(deviceID, selector: kAudioObjectPropertyManufacturer) {
            print("  Manufacturer:     \(manufacturer)")
        }

        if let uid = getStringProperty(deviceID, selector: kAudioDevicePropertyDeviceUID) {
            print("  Device UID:       \(uid)")
        }

        if let modelUID = getStringProperty(deviceID, selector: kAudioDevicePropertyModelUID) {
            print("  Model UID:        \(modelUID)")
        }

        // -- Transport Type --
        if let transport = getUInt32Property(deviceID, selector: kAudioDevicePropertyTransportType) {
            print("  Transport Type:   \(transportTypeName(transport))")
        }

        // -- Channel count --
        let inputChannels = getChannelCount(deviceID, scope: kAudioDevicePropertyScopeInput)
        let outputChannels = getChannelCount(deviceID, scope: kAudioDevicePropertyScopeOutput)
        print("  Input Channels:   \(inputChannels)")
        print("  Output Channels:  \(outputChannels)")

        // -- Sample rate --
        if let sr = getFloat64Property(deviceID, selector: kAudioDevicePropertyNominalSampleRate) {
            print("  Nominal Sample Rate: \(sr) Hz")
        }

        // -- Available sample rates --
        if let ranges = getAvailableSampleRates(deviceID) {
            let rateStrings = ranges.map { range -> String in
                if range.mMinimum == range.mMaximum {
                    return "\(range.mMinimum)"
                } else {
                    return "\(range.mMinimum)–\(range.mMaximum)"
                }
            }
            print("  Available Sample Rates: \(rateStrings.joined(separator: ", ")) Hz")
        }

        // -- Latency & safety offset --
        for (scope, scopeName) in [(kAudioDevicePropertyScopeInput, "Input"),
                                    (kAudioDevicePropertyScopeOutput, "Output")] as [(AudioObjectPropertyScope, String)]
        {
            if let latency = getUInt32Property(deviceID, selector: kAudioDevicePropertyLatency, scope: scope) {
                print("  [\(scopeName)] Latency:        \(latency) frames")
            }
            if let safety = getUInt32Property(deviceID, selector: kAudioDevicePropertySafetyOffset, scope: scope) {
                print("  [\(scopeName)] Safety Offset:  \(safety) frames")
            }
        }

        // -- Buffer Frame Size --
        if let bufSize = getUInt32Property(deviceID, selector: kAudioDevicePropertyBufferFrameSize) {
            print("  Buffer Frame Size:     \(bufSize) frames")
        }

        // -- Buffer Frame Size Range --
        do {
            var address = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyBufferFrameSizeRange,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )
            if AudioObjectHasProperty(deviceID, &address) {
                var range = AudioValueRange()
                var size = UInt32(MemoryLayout<AudioValueRange>.size)
                if AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &range) == noErr {
                    print("  Buffer Frame Size Range: \(Int(range.mMinimum))–\(Int(range.mMaximum)) frames")
                }
            }
        }

        // -- Clock Domain --
        if let clock = getUInt32Property(deviceID, selector: kAudioDevicePropertyClockDomain) {
            print("  Clock Domain:          \(clock)")
        }

        // -- Is Alive --
        if let alive = getUInt32Property(deviceID, selector: kAudioDevicePropertyDeviceIsAlive) {
            print("  Is Alive:          \(alive != 0 ? "YES" : "NO")")
        }

        // -- Is Running --
        if let running = getUInt32Property(deviceID, selector: kAudioDevicePropertyDeviceIsRunning) {
            print("  Is Running:        \(running != 0 ? "YES" : "NO")")
        }

        // -- Hog Mode --
        if let hogPid = getPidProperty(deviceID, selector: kAudioDevicePropertyHogMode) {
            if hogPid == -1 {
                print("  Hog Mode:          None (free)")
            } else {
                print("  Hog Mode:          Owned by PID \(hogPid)")
            }
        }

        // -- Supports Mixing --
        if let mix = getUInt32Property(deviceID, selector: kAudioDevicePropertySupportsMixing) {
            print("  Supports Mixing:   \(mix != 0 ? "YES" : "NO")")
        }

        // -- Jack Connected --
        for (scope, scopeName) in [(kAudioDevicePropertyScopeInput, "Input"),
                                    (kAudioDevicePropertyScopeOutput, "Output")] as [(AudioObjectPropertyScope, String)]
        {
            if let jack = getUInt32Property(deviceID, selector: kAudioDevicePropertyJackIsConnected, scope: scope) {
                print("  [\(scopeName)] Jack Connected: \(jack != 0 ? "YES" : "NO")")
            }
        }

        // -- Per-channel volume (scalar + dB) / Mute --
        print("  --- Volume / Mute ---")
        printPerChannelVolumes(deviceID, scope: kAudioDevicePropertyScopeOutput, scopeName: "Output")
        printPerChannelVolumes(deviceID, scope: kAudioDevicePropertyScopeInput, scopeName: "Input")

        // -- Data Source --
        for (scope, scopeName) in [(kAudioDevicePropertyScopeInput, "Input"),
                                    (kAudioDevicePropertyScopeOutput, "Output")] as [(AudioObjectPropertyScope, String)]
        {
            if let src = getUInt32Property(deviceID, selector: kAudioDevicePropertyDataSource, scope: scope) {
                let srcName = getDataSourceName(deviceID, scope: scope, sourceID: src)
                print("  [\(scopeName)] Data Source:    \(fourCC(src)) \(srcName.map { "(\($0))" } ?? "")")
            }
        }

        // -- Preferred Channels For Stereo --
        for (scope, scopeName) in [(kAudioDevicePropertyScopeInput, "Input"),
                                    (kAudioDevicePropertyScopeOutput, "Output")] as [(AudioObjectPropertyScope, String)]
        {
            var address = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyPreferredChannelsForStereo,
                mScope: scope,
                mElement: kAudioObjectPropertyElementMain
            )
            if AudioObjectHasProperty(deviceID, &address) {
                var channels: [UInt32] = [0, 0]
                var size = UInt32(MemoryLayout<UInt32>.size * 2)
                if AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &channels) == noErr {
                    print("  [\(scopeName)] Preferred Stereo Channels: \(channels[0]), \(channels[1])")
                }
            }
        }

        // -- Streams Info --
        for (scope, scopeName) in [(kAudioDevicePropertyScopeInput, "Input"),
                                    (kAudioDevicePropertyScopeOutput, "Output")] as [(AudioObjectPropertyScope, String)]
        {
            if let streamIDs = getStreams(deviceID, scope: scope), !streamIDs.isEmpty {
                print("  --- [\(scopeName)] Streams (\(streamIDs.count)) ---")
                for (si, streamID) in streamIDs.enumerated() {
                    print("    Stream #\(si + 1) (ID: \(streamID))")

                    // Direction
                    if let dir = getUInt32Property(streamID, selector: kAudioStreamPropertyDirection) {
                        print("      Direction:   \(dir == 0 ? "Output" : "Input")")
                    }

                    // Terminal Type
                    if let tt = getUInt32Property(streamID, selector: kAudioStreamPropertyTerminalType) {
                        print("      Terminal Type: \(fourCC(tt))")
                    }

                    // Starting Channel
                    if let sc = getUInt32Property(streamID, selector: kAudioStreamPropertyStartingChannel) {
                        print("      Starting Channel: \(sc)")
                    }

                    // Latency
                    if let lat = getUInt32Property(streamID, selector: kAudioStreamPropertyLatency) {
                        print("      Latency:     \(lat) frames")
                    }

                    // Is Active
                    if let active = getUInt32Property(streamID, selector: kAudioStreamPropertyIsActive) {
                        print("      Active:      \(active != 0 ? "YES" : "NO")")
                    }

                    // Physical Format
                    if let pf = getStreamPhysicalFormat(streamID) {
                        print("      Physical Format: \(formatASBD(pf))")
                    }

                    // Virtual Format
                    if let vf = getStreamVirtualFormat(streamID) {
                        print("      Virtual Format:  \(formatASBD(vf))")
                    }

                    // Available Physical Formats
                    var apfAddr = AudioObjectPropertyAddress(
                        mSelector: kAudioStreamPropertyAvailablePhysicalFormats,
                        mScope: kAudioObjectPropertyScopeGlobal,
                        mElement: kAudioObjectPropertyElementMain
                    )
                    if AudioObjectHasProperty(streamID, &apfAddr) {
                        var apfSize: UInt32 = 0
                        if AudioObjectGetPropertyDataSize(streamID, &apfAddr, 0, nil, &apfSize) == noErr, apfSize > 0 {
                            let apfCount = Int(apfSize) / MemoryLayout<AudioStreamRangedDescription>.size
                            var formats = [AudioStreamRangedDescription](
                                repeating: AudioStreamRangedDescription(),
                                count: apfCount
                            )
                            if AudioObjectGetPropertyData(streamID, &apfAddr, 0, nil, &apfSize, &formats) == noErr {
                                print("      Available Physical Formats (\(apfCount)):")
                                for (fi, f) in formats.enumerated() {
                                    let rateRange: String
                                    if f.mSampleRateRange.mMinimum == f.mSampleRateRange.mMaximum {
                                        rateRange = "\(f.mSampleRateRange.mMinimum) Hz"
                                    } else {
                                        rateRange = "\(f.mSampleRateRange.mMinimum)–\(f.mSampleRateRange.mMaximum) Hz"
                                    }
                                    print("        [\(fi + 1)] \(formatASBD(f.mFormat)) | Rate Range: \(rateRange)")
                                }
                            }
                        }
                    }
                }
            }
        }

        print()
    }

    print("========================================")
    print("  Done. \(devices.count) device(s) reported.")
    print("========================================")
}

// Run
main()