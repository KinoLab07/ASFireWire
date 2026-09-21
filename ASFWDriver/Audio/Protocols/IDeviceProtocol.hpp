// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// IDeviceProtocol.hpp - Interface for device-specific protocol handlers

#pragma once

#include "AudioTypes.hpp"
#include "../../Discovery/DeviceRouteToken.hpp"

#include <DriverKit/IOReturn.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ASFW::Protocols::AVC {
    class FCPTransport;
}

namespace ASFW::IRM {
    class IRMClient;
}

namespace ASFW::Audio {
class IDuplexDeviceControl;
}

namespace ASFW::Audio {

/// Interface for device-specific protocol handlers
///
/// Device protocols are instantiated by DeviceProtocolFactory when a
/// known device is detected during discovery. Each protocol handler
/// encapsulates vendor-specific control logic (DSP, routing, etc.).
class IDeviceProtocol {
public:
    virtual ~IDeviceProtocol() = default;
    
    /// Initialize the protocol (read device state, cache parameters)
    /// @return kIOReturnSuccess on success
    virtual IOReturn Initialize() = 0;
    
    /// Shutdown the protocol (release resources)
    /// @return kIOReturnSuccess on success
    virtual IOReturn Shutdown() = 0;
    
    /// Get human-readable device name
    virtual const char* GetName() const = 0;
    
    /// Check if device supports DSP effects
    virtual bool HasDsp() const { return false; }
    
    /// Check if device supports hardware mixer
    virtual bool HasMixer() const { return false; }

    // Output modes. Some hardware cannot drive all of its outputs at once and
    // instead offers a set of mutually exclusive configurations - the Avid
    // Mbox Pro can clock either its line outputs or its headphones, never
    // both. Devices without that constraint report zero modes.
    virtual uint32_t GetOutputModeCount() const { return 0; }
    virtual uint32_t GetActiveOutputMode() const { return 0; }
    virtual const char* GetOutputModeName(uint32_t /*index*/) const { return ""; }
    virtual IOReturn SelectOutputMode(uint32_t /*index*/) { return kIOReturnUnsupported; }

    // Per-output trim, as a raw device register value. The meaning of the byte
    // is device specific; on the Avid Mbox Pro it is attenuation in half
    // decibels (register = -2 x dB), so 0 is unity and 255 is the quietest.
    // Values are served from a cache the protocol keeps in step with the
    // device, because reads are asynchronous.
    virtual uint32_t GetOutputTrimCount() const { return 0; }
    virtual uint8_t GetOutputTrim(uint32_t /*index*/) const { return 0; }
    virtual IOReturn SetOutputTrim(uint32_t /*index*/, uint8_t /*value*/) {
        return kIOReturnUnsupported;
    }
    virtual IOReturn RefreshOutputTrims() { return kIOReturnUnsupported; }

    // Mixer matrix. Coefficients are linear gains as the device stores them,
    // with a device-specific unity value; the caller converts to decibels.
    // Like the trims, values are served from a cache kept in step with the
    // device because reads are asynchronous.
    virtual uint32_t GetMixerInputCount() const { return 0; }
    virtual uint32_t GetMixerOutputCount() const { return 0; }
    virtual uint16_t GetMixerUnityGain() const { return 0; }
    virtual uint16_t GetMixerCoefficient(uint32_t /*out*/, uint32_t /*in*/) const { return 0; }
    virtual IOReturn SetMixerCoefficient(uint32_t /*out*/, uint32_t /*in*/, uint16_t /*gain*/) {
        return kIOReturnUnsupported;
    }
    virtual IOReturn RefreshMixer() { return kIOReturnUnsupported; }

    // Peak meters. The device keeps one entry per router route, each holding
    // the route itself in the low half and its current peak in the high half,
    // so a caller finds a meter by looking up the route it cares about.
    virtual uint32_t GetPeakCount() const { return 0; }
    virtual uint32_t GetPeakEntry(uint32_t /*index*/) const { return 0; }
    virtual IOReturn RefreshPeaks() { return kIOReturnUnsupported; }

    /// Query runtime-discovered audio stream capabilities.
    /// Returns true when the protocol has authoritative stream caps (e.g. DICE TX/RX stream formats).
    virtual bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
        (void)outCaps;
        return false;
    }

    /// Query per-channel device labels discovered from the protocol's stream
    /// format (e.g. DICE TX/RX name sections). `inNames` is host input/capture,
    /// `outNames` is host output/playback, both in channel order; an empty entry
    /// means "no label for that channel". Returns true only when authoritative
    /// labels are available (caps loaded); callers fall back to synthesized
    /// names otherwise.
    virtual bool GetChannelLabels(std::vector<std::string>& inNames,
                                  std::vector<std::string>& outNames) const {
        (void)inNames;
        (void)outNames;
        return false;
    }

    using VoidCallback = std::function<void(IOReturn)>;

    /// Optional bring-up hook to prepare device-side duplex state at 48kHz.
    /// Drivers can call this before any IRM reservation or host IR/IT startup.
    /// Implementations should be idempotent.
    virtual void PrepareDuplex48k(const AudioDuplexChannels& channels, VoidCallback callback) {
        (void)channels;
        callback(kIOReturnUnsupported);
    }

    /// Optional hook to program the device-side RX leg after playback IRM allocation.
    virtual void ProgramRxForDuplex48k(VoidCallback callback) {
        callback(kIOReturnUnsupported);
    }

    /// Optional hook to program the device-side TX leg and enable duplex streaming.
    virtual void ProgramTxAndEnableDuplex48k(VoidCallback callback) {
        callback(kIOReturnUnsupported);
    }

    /// Optional completion hook after host IR/IT contexts are running.
    /// DICE devices can use this to verify stream lock/state.
    virtual void ConfirmDuplex48kStart(VoidCallback callback) {
        callback(kIOReturnUnsupported);
    }

    /// Optional teardown hook to stop device-side duplex state.
    virtual IOReturn StopDuplex() {
        return kIOReturnUnsupported;
    }

    /// Optional internal hook for backends that need the protocol's IRM client.
    virtual ::ASFW::IRM::IRMClient* GetIRMClient() const {
        return nullptr;
    }

    /// Optional protocol-neutral duplex control interface used by the audio lifecycle.
    virtual IDuplexDeviceControl* AsDuplexDeviceControl() noexcept {
        return nullptr;
    }

    virtual const IDuplexDeviceControl* AsDuplexDeviceControl() const noexcept {
        return nullptr;
    }

    /// Update volatile runtime context that can change across bus resets.
    virtual void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                                      Protocols::AVC::FCPTransport* transport) {
        (void)route;
        (void)transport;
    }

    /// Check if protocol can expose/control a boolean control.
    // These virtuals intentionally match the host-facing `(class, element[, value])` contract.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    virtual bool SupportsBooleanControl(uint32_t classIdFourCC,
                                        uint32_t element) const {
        (void)classIdFourCC;
        (void)element;
        return false;
    }

    /// Read protocol-backed boolean control value.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    virtual IOReturn GetBooleanControlValue(uint32_t classIdFourCC,
                                            uint32_t element,
                                            bool& outValue) {
        (void)classIdFourCC;
        (void)element;
        (void)outValue;
        return kIOReturnUnsupported;
    }

    /// Write protocol-backed boolean control value.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    virtual IOReturn SetBooleanControlValue(uint32_t classIdFourCC,
                                            uint32_t element,
                                            bool value) {
        (void)classIdFourCC;
        (void)element;
        (void)value;
        return kIOReturnUnsupported;
    }
};

} // namespace ASFW::Audio
