import Foundation
import IOKit

extension ASFWDriverConnector {
    /// Invoke the full AudioCoordinator lifecycle for one published audio GUID.
    /// The driver owns IRM reservations, PCR connections, AM824 setup, and
    /// rollback; MCP does not assemble those wire actions itself.
    func setAudioStreaming(guid: UInt64, enabled: Bool) -> kern_return_t {
        guard isConnected, connection != 0, guid != 0 else { return kIOReturnNotReady }
        var input = guid
        return IOConnectCallScalarMethod(
            connection,
            (enabled ? Method.startAudioStreaming : Method.stopAudioStreaming).rawValue,
            &input,
            1,
            nil,
            nil
        )
    }
}

/// One mutually exclusive output configuration offered by a device that cannot
/// drive all of its outputs at once.
struct OutputMode: Identifiable, Hashable {
    let index: UInt32
    let name: String
    var id: UInt32 { index }
}

extension ASFWDriverConnector {
    /// Read the output modes a device offers and which one is active. Devices
    /// without the constraint report an empty list.
    func outputModes(guid: UInt64) -> (modes: [OutputMode], active: UInt32, status: kern_return_t) {
        guard isConnected, connection != 0, guid != 0 else { return ([], 0, kIOReturnNotReady) }

        var input = guid
        var scalars = [UInt64](repeating: 0, count: 2)
        var scalarCount = UInt32(scalars.count)
        var blob = [UInt8](repeating: 0, count: 512)
        var blobSize = blob.count

        let status = IOConnectCallMethod(
            connection,
            Method.getOutputModes.rawValue,
            &input, 1,
            nil, 0,
            &scalars, &scalarCount,
            &blob, &blobSize
        )
        guard status == kIOReturnSuccess, scalarCount >= 2 else { return ([], 0, status) }

        let count = UInt32(truncatingIfNeeded: scalars[0])
        let active = UInt32(truncatingIfNeeded: scalars[1])
        guard count > 0 else { return ([], active, kIOReturnSuccess) }

        // Names arrive as a single NUL-separated blob.
        var names: [String] = []
        var start = blob.startIndex
        while start < blob.endIndex, names.count < Int(count) {
            guard let end = blob[start...].firstIndex(of: 0) else { break }
            if end == start { break }
            names.append(String(decoding: blob[start..<end], as: UTF8.self))
            start = blob.index(after: end)
        }

        let modes = (0..<Int(count)).map { i in
            OutputMode(index: UInt32(i),
                       name: i < names.count ? names[i] : "Mode \(i + 1)")
        }
        return (modes, active, kIOReturnSuccess)
    }

    /// Switch the device to one of its output modes. This is the same switch the
    /// front-panel button performs.
    @discardableResult
    func selectOutputMode(guid: UInt64, index: UInt32) -> kern_return_t {
        guard isConnected, connection != 0, guid != 0 else { return kIOReturnNotReady }
        var input: [UInt64] = [guid, UInt64(index)]
        return IOConnectCallScalarMethod(
            connection,
            Method.selectOutputMode.rawValue,
            &input, 2,
            nil, nil
        )
    }
}

extension ASFWDriverConnector {
    /// Per-output trim as raw register bytes. On the Mbox Pro the byte is
    /// attenuation in half decibels, so 0 is unity.
    func outputTrims(guid: UInt64) -> [UInt8] {
        guard isConnected, connection != 0, guid != 0 else { return [] }

        var input = guid
        var scalars = [UInt64](repeating: 0, count: 1)
        var scalarCount = UInt32(scalars.count)
        var blob = [UInt8](repeating: 0, count: 16)
        var blobSize = blob.count

        let status = IOConnectCallMethod(
            connection,
            Method.getOutputTrims.rawValue,
            &input, 1,
            nil, 0,
            &scalars, &scalarCount,
            &blob, &blobSize
        )
        guard status == kIOReturnSuccess, scalarCount >= 1 else { return [] }
        let count = Int(scalars[0])
        guard count > 0, count <= blob.count else { return [] }
        return Array(blob[0..<count])
    }

    @discardableResult
    func setOutputTrim(guid: UInt64, index: UInt32, value: UInt8) -> kern_return_t {
        guard isConnected, connection != 0, guid != 0 else { return kIOReturnNotReady }
        var input: [UInt64] = [guid, UInt64(index), UInt64(value)]
        return IOConnectCallScalarMethod(
            connection,
            Method.setOutputTrim.rawValue,
            &input, 3,
            nil, nil
        )
    }
}

/// Geometry plus one output row of the mixer matrix.
struct MixerRow {
    let inputs: Int
    let outputs: Int
    let unityGain: UInt16
    let gains: [UInt16]
}

extension ASFWDriverConnector {
    func mixerRow(guid: UInt64, output: UInt32) -> MixerRow? {
        guard isConnected, connection != 0, guid != 0 else { return nil }

        var input: [UInt64] = [guid, UInt64(output)]
        var scalars = [UInt64](repeating: 0, count: 3)
        var scalarCount = UInt32(scalars.count)
        var blob = [UInt8](repeating: 0, count: 64)
        var blobSize = blob.count

        let status = IOConnectCallMethod(
            connection,
            Method.getMixerRow.rawValue,
            &input, 2,
            nil, 0,
            &scalars, &scalarCount,
            &blob, &blobSize
        )
        guard status == kIOReturnSuccess, scalarCount >= 3 else { return nil }

        let inputs = Int(scalars[0])
        guard inputs > 0, inputs * 2 <= blob.count else { return nil }

        var gains: [UInt16] = []
        gains.reserveCapacity(inputs)
        for i in 0..<inputs {
            // Native order: the driver copied UInt16s straight out.
            gains.append(UInt16(blob[i * 2]) | (UInt16(blob[i * 2 + 1]) << 8))
        }
        return MixerRow(inputs: inputs,
                        outputs: Int(scalars[1]),
                        unityGain: UInt16(truncatingIfNeeded: scalars[2]),
                        gains: gains)
    }

    @discardableResult
    func setMixerCoefficient(guid: UInt64, output: UInt32, input: UInt32, gain: UInt16) -> kern_return_t {
        guard isConnected, connection != 0, guid != 0 else { return kIOReturnNotReady }
        var args: [UInt64] = [guid, UInt64(output), UInt64(input), UInt64(gain)]
        return IOConnectCallScalarMethod(
            connection,
            Method.setMixerCoefficient.rawValue,
            &args, 4,
            nil, nil
        )
    }
}

extension ASFWDriverConnector {
    /// Peak meters, keyed by router route. Each word packs the level in the
    /// high 16 bits and the route (destination << 8 | source) in the low 16.
    func peaks(guid: UInt64) -> [UInt16: UInt16] {
        guard isConnected, connection != 0, guid != 0 else { return [:] }

        var input = guid
        var scalars = [UInt64](repeating: 0, count: 1)
        var scalarCount = UInt32(scalars.count)
        var blob = [UInt8](repeating: 0, count: 512)
        var blobSize = blob.count

        let status = IOConnectCallMethod(
            connection,
            Method.getPeaks.rawValue,
            &input, 1,
            nil, 0,
            &scalars, &scalarCount,
            &blob, &blobSize
        )
        guard status == kIOReturnSuccess, scalarCount >= 1 else { return [:] }

        let count = min(Int(scalars[0]), blob.count / 4)
        var out: [UInt16: UInt16] = [:]
        out.reserveCapacity(count)
        for i in 0..<count {
            // Native order: the driver copied UInt32s straight out.
            let word = UInt32(blob[i*4]) | (UInt32(blob[i*4+1]) << 8)
                     | (UInt32(blob[i*4+2]) << 16) | (UInt32(blob[i*4+3]) << 24)
            let route = UInt16(word & 0xFFFF)
            let level = UInt16((word >> 16) & 0xFFFF)
            if route != 0 { out[route] = max(out[route] ?? 0, level) }
        }
        return out
    }
}
