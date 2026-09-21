// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvidAudioProfiles.hpp - Avid/Digidesign FireWire audio device knowledge (DICE/TCAT).
// Knows ONLY Avid devices; performs no runtime protocol construction.

#pragma once

#include "../../Common/DeviceProfileTypes.hpp"
#include "../AudioDeviceIds.hpp"
#include "../AudioProfileTypes.hpp"

#include <optional>

namespace ASFW::DeviceProfiles::Audio::Avid {

[[nodiscard]] constexpr std::optional<DeviceIdentityHint>
LookupIdentity(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId == kAvidVendorId && query.modelId == kMboxProModelId) {
        return DeviceIdentityHint{.vendorId = query.vendorId,
                                  .modelId = query.modelId,
                                  .vendorName = kAvidVendorName,
                                  .modelName = kMboxProModelName,
                                  .source = MatchSource::VendorModel};
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<AudioProfileHint>
LookupAudioProfile(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId == kAvidVendorId && query.modelId == kMboxProModelId) {
        // Config ROM read from the device (2026-09-16):
        //   modalias ieee1394:ven0000A07Emo00000004sp0000A07Ever00000001
        // The single unit directory carries specifier_id == the vendor OUI (0x00A07E)
        // and version 0x000001 — the canonical DICE signature, which is why the
        // generic TA 61883 classifier leaves this device as Unknown.
        // Cross-checked: linux sound/firewire/dice/dice.c:246 (DICE_INTERFACE 0x000001)
        // and dice.c:263 DICE_DEV_ENTRY_TYPICAL(OUI_AVID, 0x000004,
        // snd_dice_detect_extension_formats) — the layout is advertised by the device
        // through EAP, so no hardcoded stream geometry belongs here.
        return AudioProfileHint{.family = AudioProtocolFamily::DICE,
                                .mode = AudioIntegrationMode::kHardcodedNub,
                                .source = MatchSource::VendorModel};
    }
    return std::nullopt;
}

} // namespace ASFW::DeviceProfiles::Audio::Avid
