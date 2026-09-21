// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvidMboxProProfile.hpp
// Avid Mbox Pro (3rd gen, 2011) FireWire profile (DICE/TCAT).

#pragma once

#include "../../DiceDeviceProfile.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

class AvidMboxProProfile final : public IDiceDeviceProfile {
public:
    [[nodiscard]] const char* Name() const noexcept override;

    [[nodiscard]] bool Matches(const DiceDeviceIdentity& identity) const noexcept override;

    [[nodiscard]] DiceDeviceQuirks Quirks() const noexcept override;

    [[nodiscard]] bool BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;

    // One isochronous stream per direction (TX_NUMBER == RX_NUMBER == 1, read
    // from the device's TCAT STREAM FORMAT sections), so the base class default
    // of 1 is correct and is not overridden here.

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override;

    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override;
};

} // namespace ASFW::Isoch::Audio::DICE::Profiles
