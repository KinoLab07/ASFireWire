// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvidMboxProProfile.cpp
// Avid Mbox Pro (3rd gen, 2011) FireWire profile (DICE/TCAT).
//
// Stream geometry READ FROM THE DEVICE, not inferred from marketing material.
// TCAT STREAM FORMAT sections at DICE base 0xFFFFE0000000 (2026-09-16, gen 1):
//
//   section table : global @40/360  tx @400/568  rx @968/1128  extSync @2096/16
//   TX (device->host) : nb_tx_streams = 1, number_audio = 8, number_midi = 2, speed = 2 (S400)
//   RX (host->device) : nb_rx_streams = 1, number_audio = 8, number_midi = 2
//   GLOBAL nickname   : "Mbox Pro"
//   GLOBAL CLOCK_CAPS : 0x1381007E -> 44.1/48/88.2/96/176.4/192 kHz
//   GLOBAL STATUS     : 0x00000101 (clock locked)
//
// Identity cross-checked with linux sound/firewire/dice/dice.c:263,
// DICE_DEV_ENTRY_TYPICAL(OUI_AVID, 0x000004, snd_dice_detect_extension_formats).

#include "AvidMboxProProfile.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

namespace {

constexpr uint32_t kAvidVendorId   = 0x00a07e;
constexpr uint32_t kMboxProModelId = 0x000004;

// Single stream per direction. The device reports pcm=8, midi=2 BUT
// am824Slots=9, i.e. its two MIDI ports are multiplexed into ONE AM824 slot.
// Read live from the device (2026-09-16):
//   [DICE] TX Streams: count=1 entrySize=280B pcm=8 midi=2 am824Slots=9
//   [DICE] RX Streams: count=1 entrySize=280B pcm=8 midi=2 am824Slots=9
// so DBS is 9, not 8+2. Same convention as the Focusrite Saffire (8 PCM + 1
// MIDI slot => DBS 9). An initial 10 here was wrong and is what the device-
// reported am824Slots corrected.
constexpr uint32_t kPcmChannels = 8;
constexpr uint32_t kMidiSlots   = 1;  // one AM824 slot carrying both MIDI ports
constexpr uint32_t kDbs         = kPcmChannels + kMidiSlots; // 9

void FillStreamConfig(DiceStreamConfig& out, DiceStreamDirection direction) noexcept {
    out = DiceStreamConfig{};
    out.direction  = direction;
    out.sampleRate = 48000;
    out.streamMode = Encoding::StreamMode::kBlocking;
    out.sid        = 0;
    out.framesPerDataPacket = 8;
    out.fdf        = 0x02;
    out.fmt        = 0x10;
    out.pcmChannels = kPcmChannels;
    out.midiSlots   = kMidiSlots;
    out.dbs         = kDbs;
}

} // namespace

const char* AvidMboxProProfile::Name() const noexcept {
    return "Avid Mbox Pro (DICE)";
}

bool AvidMboxProProfile::Matches(const DiceDeviceIdentity& identity) const noexcept {
    return identity.vendorId == kAvidVendorId && identity.modelId == kMboxProModelId;
}

DiceDeviceQuirks AvidMboxProProfile::Quirks() const noexcept {
    DiceDeviceQuirks quirks{};
    // Same TCAT DICE chip family as the Focusrite Saffire and Midas Venice, so
    // the same wire encoding is the starting point. Unverified on this device:
    // if playback is silent or distorted, the host->device encoding is the first
    // thing to revisit.
    quirks.tx.hostToDevicePcmEncoding    = Encoding::AudioWireFormat::kRawPcm24In32;
    quirks.tx.dbsPolicy                  = DbsPolicy::Constant;
    quirks.tx.defaultNonAudioSlotWord    = 0x80000000;
    quirks.tx.initializeNonAudioSlots    = true;
    quirks.tx.preserveFdfInNoDataPackets = true;
    quirks.rx.deviceToHostPcmEncoding    = Encoding::AudioWireFormat::kAM824;
    quirks.rx.dbsPolicy                  = DbsPolicy::Constant;
    return quirks;
}

bool AvidMboxProProfile::BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DiceStreamDirection::HostToDevice);
    return true;
}

bool AvidMboxProProfile::BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DiceStreamDirection::DeviceToHost);
    return true;
}

uint32_t AvidMboxProProfile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    uint32_t framesPerPacket = 8;
    uint32_t rateAddend = 0;
    if (sampleRate > 96000.0) {
        framesPerPacket = 32;
        rateAddend = 4;
    } else if (sampleRate > 48000.0) {
        framesPerPacket = 16;
        rateAddend = 2;
    }
    return (6 + rateAddend) * framesPerPacket;
}

uint32_t AvidMboxProProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    uint32_t framesPerPacket = 8;
    uint32_t rateAddend = 0;
    if (sampleRate > 96000.0) {
        framesPerPacket = 32;
        rateAddend = 4;
    } else if (sampleRate > 48000.0) {
        framesPerPacket = 16;
        rateAddend = 2;
    }
    return (16 + rateAddend) * framesPerPacket;
}

uint32_t AvidMboxProProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    if (sampleRate > 96000.0) return 119;
    if (sampleRate > 48000.0) return 59;
    return 29;
}

uint32_t AvidMboxProProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    if (sampleRate > 96000.0) return 119;
    if (sampleRate > 48000.0) return 59;
    return 29;
}

} // namespace ASFW::Isoch::Audio::DICE::Profiles
