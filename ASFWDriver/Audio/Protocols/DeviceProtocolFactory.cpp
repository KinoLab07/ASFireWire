// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// DeviceProtocolFactory.cpp - Factory for creating device-specific protocol handlers

#include "DeviceProtocolFactory.hpp"
#include "DICE/Focusrite/SPro24DspProtocol.hpp"
#include "DICE/TCAT/DICETcatProtocol.hpp"
#include "DICE/Avid/AvidMboxProRouting.hpp"
#include "Oxford/Apogee/ApogeeDuetProtocol.hpp"
#include "BeBoB/Phase88Protocol.hpp"
#include "BeBoB/GenericBeBoBProtocol.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"

namespace ASFW::Audio {

namespace {

// The Mbox Pro drives its line outputs, S/PDIF and both headphones at the same
// time, so it needs one router program rather than a set of exclusive modes.
// The table is Avid's own, captured from a working machine.
constexpr DICE::TCAT::DICERouterProgram kMboxProOutputModes[] = {
    {
        .entries = DICE::Avid::MboxProRouting::kRouterEntries,
        .entryCount = DICE::Avid::MboxProRouting::kRouterEntryCount,
        .name = "All Outputs",
        .ledSelect = 1,
    },
};

// Flattened (index, gain) pairs of the mixer coefficients that go with that
// table. Without them the mixer-fed outputs are silent. Index is
// output * 18 + input; unity is 0x4000; mixer inputs 8-15 are host playback
// channels 1-8. Keep this in step with kStartupMixerCoefficients in
// AvidMboxProRouting.hpp, which carries the same values.
constexpr uint16_t kMboxProMixerCoefficients[] = {
    // Line outs 1-6: one playback channel each, at unity.
    8, 0x4000,  27, 0x4000,  46, 0x4000,
    65, 0x4000, 84, 0x4000,  103, 0x4000,
    // S/PDIF: playback 7-8 at unity.
    122, 0x4000, 141, 0x4000,
    // Headphones A (outs 8-9) and B (outs 10-11): every playback channel, odd
    // channels left and even right, each at -12 dB so four cannot clip.
    152, 0x1000, 154, 0x1000, 156, 0x1000, 158, 0x1000,
    171, 0x1000, 173, 0x1000, 175, 0x1000, 177, 0x1000,
    188, 0x1000, 190, 0x1000, 192, 0x1000, 194, 0x1000,
    207, 0x1000, 209, 0x1000, 211, 0x1000, 213, 0x1000,
    // Explicit zeros for the cells the previous program left summed into the
    // monitor bus. NOT zeroed: out0<-in6 and out1<-in7, which the firmware
    // uses to apply the front-panel monitor knob, and the factory analog-in
    // feeds on the S/PDIF outputs. Zeroing the knob cells silenced every
    // analog output on hardware (9 Oct) even with the router and the rest of
    // the matrix correct.
    9, 0, 10, 0, 11, 0, 12, 0, 13, 0, 14, 0, 15, 0, 16, 0,
    26, 0, 28, 0, 29, 0, 30, 0, 31, 0, 32, 0, 33, 0, 34, 0,
};

} // namespace

std::unique_ptr<IDeviceProtocol> DeviceProtocolFactory::Create(
    uint32_t vendorId,
    uint32_t modelId,
    Protocols::Ports::FireWireBusOps& busOps,
    Protocols::Ports::FireWireBusInfo& busInfo,
    Discovery::DeviceRegistry& routeRegistry,
    const Discovery::DeviceRouteToken& route,
    IRM::IRMClient* irmClient,
    CMP::CMPClient* cmpClient,
    Scheduling::ITimerScheduler* timerScheduler
) {
    if (!route) {
        return nullptr;
    }
    const uint16_t nodeId = route.nodeId;
    if (vendorId == kFocusriteVendorId) {
        if (modelId == kSPro24DspModelId) {
            ASFW_LOG(DICE, "Creating SPro24DspProtocol for vendor=0x%06x model=0x%06x node=0x%04x",
                     vendorId, modelId, nodeId);
            return std::make_unique<DICE::Focusrite::SPro24DspProtocol>(busOps, busInfo, routeRegistry,
                                                                         route, irmClient);
        }

        if (modelId == kSPro14ModelId || modelId == kSPro24ModelId ||
            modelId == kSPro40ModelId) {
            const auto known = LookupKnownIdentity(vendorId, modelId);
            ASFW_LOG(DICE,
                     "Creating generic DICETcatProtocol for %{public}s vendor=0x%06x model=0x%06x node=0x%04x",
                     (known.has_value() && known->modelName) ? known->modelName : "Focusrite DICE",
                     vendorId,
                     modelId,
                     nodeId);
            return std::make_unique<DICE::TCAT::DICETcatProtocol>(busOps, busInfo, routeRegistry,
                                                                    route, irmClient, timerScheduler);
        }
    }

    if (vendorId == kWeissVendorId &&
        (modelId == kWeissInt202ModelId || modelId == kWeissInt203ModelId)) {
        const auto known = LookupKnownIdentity(vendorId, modelId);
        ASFW_LOG(DICE,
                 "Creating Weiss DICETcatProtocol for %{public}s vendor=0x%06x model=0x%06x node=0x%04x; "
                 "DICE remains duplex while CoreAudio hides device->host channels",
                 (known.has_value() && known->modelName) ? known->modelName : "Weiss INT",
                 vendorId,
                 modelId,
                 nodeId);
        return std::make_unique<DICE::TCAT::DICETcatProtocol>(
            busOps,
            busInfo,
            routeRegistry,
            route,
            irmClient,
            timerScheduler,
            DICE::TCAT::DICETcatRuntimePolicy{
                .exposeDeviceToHostToCoreAudio = false,
                .requireSourceLockBeforeStreamEnable = false,
                .requireSourceLockAtConfirm = false,
            });
    }

    if (vendorId == kAlesisVendorId && modelId == kAlesisMultiMixModelId) {
        ASFW_LOG(DICE,
                 "Creating generic DICETcatProtocol for Alesis MultiMix vendor=0x%06x model=0x%06x node=0x%04x",
                 vendorId,
                 modelId,
                 nodeId);
        return std::make_unique<DICE::TCAT::DICETcatProtocol>(busOps, busInfo, routeRegistry,
                                                                route, irmClient, timerScheduler);
    }

    if (vendorId == kMidasVendorId && modelId == kMidasVeniceModelId) {
        ASFW_LOG(DICE,
                 "Creating generic DICETcatProtocol for Midas Venice vendor=0x%06x model=0x%06x node=0x%04x",
                 vendorId,
                 modelId,
                 nodeId);
        return std::make_unique<DICE::TCAT::DICETcatProtocol>(busOps, busInfo, routeRegistry,
                                                                route, irmClient, timerScheduler);
    }

    if (vendorId == kAvidVendorId && modelId == kMboxProModelId) {
        // The Mbox Pro streams correctly out of the box but ships with its output
        // pairs routed to MUTED, so it stays silent until the TCAT router is
        // programmed. Avid's control panel did that; carry its table instead.
        ASFW_LOG(DICE,
                 "Creating DICETcatProtocol for Avid Mbox Pro vendor=0x%06x model=0x%06x node=0x%04x "
                 "with %u startup router entries",
                 vendorId,
                 modelId,
                 nodeId,
                 DICE::Avid::MboxProRouting::kRouterEntryCount);
        return std::make_unique<DICE::TCAT::DICETcatProtocol>(
            busOps,
            busInfo,
            routeRegistry,
            route,
            irmClient,
            timerScheduler,
            DICE::TCAT::DICETcatRuntimePolicy{
                .routerPrograms = kMboxProOutputModes,
                .routerProgramCount =
                    static_cast<uint32_t>(sizeof(kMboxProOutputModes) /
                                          sizeof(kMboxProOutputModes[0])),
                // The Mbox Pro raises this bit when the front-panel monitor
                // button is pressed. Generic DICE names it word-clock slip;
                // this device uses it for the panel event.
                .routerCycleNotifyMask = DICE::ExtStatusBits::kWordClockSlip,
                .driveFrontPanelLed = true,
                .startupMixerCoefficients = kMboxProMixerCoefficients,
                .startupMixerCoefficientCount =
                    static_cast<uint32_t>(sizeof(kMboxProMixerCoefficients) /
                                          sizeof(kMboxProMixerCoefficients[0]) / 2),
                // DAC1..DAC6 in Avid's daemon: attenuation for the six analog
                // line outputs, one quadlet each from application +0x28.
                .outputTrimCount = 6,
                .outputTrimOffset = 0x28,
                // 18 inputs by 16 outputs, unity 0x4000. Matches the 289
                // quadlet mixer section the device reports (1 + 18*16).
                .mixerInputs = 18,
                .mixerOutputs = 16,
                .mixerUnityGain = 0x4000,
            });
    }

    if (vendorId == kPreSonusVendorId && modelId == kStudioLive1602ModelId) {
        ASFW_LOG(DICE,
                 "Creating generic DICETcatProtocol for PreSonus StudioLive 16.0.2 vendor=0x%06x model=0x%06x node=0x%04x",
                 vendorId,
                 modelId,
                 nodeId);
        return std::make_unique<DICE::TCAT::DICETcatProtocol>(busOps, busInfo, routeRegistry,
                                                                route, irmClient, timerScheduler);
    }

    // Check for Apogee Duet FireWire (AV/C + vendor-dependent commands).
    if (vendorId == kApogeeVendorId && modelId == kApogeeDuetModelId) {
        ASFW_LOG(Audio,
                 "Creating ApogeeDuetProtocol for vendor=0x%06x model=0x%06x node=0x%04x",
                 vendorId, modelId, nodeId);
        // Factory path intentionally does not bind FCP transport yet.
        // AVCDiscovery wires transport for live command execution.
        return std::make_unique<Oxford::Apogee::ApogeeDuetProtocol>(
            busOps, busInfo, route, &routeRegistry, nullptr, irmClient, cmpClient, 100U,
            timerScheduler);
    }

    if (vendorId == kTerraTecVendorId && modelId == kPhase88RackFwModelId) {
        ASFW_LOG(Audio,
                 "Creating Phase88Protocol BeBoB/CMP backend vendor=0x%06x model=0x%06x node=0x%04x",
                 vendorId, modelId, nodeId);
        return std::make_unique<BeBoB::Phase88Protocol>(busOps, busInfo, route, irmClient,
                                                        cmpClient, timerScheduler);
    }
    // Known BeBoB device without a verified custom protocol: generic fallback.
    // Conservative defaults — plug-0, CMP, no mixer programming. Discovery model
    // wiring (for derived geometry) lands with the per-GUID profile work.
    if (DeviceProfiles::Audio::BeBoB::IsBeBoBDevice(vendorId, modelId)) {
        ASFW_LOG(Audio,
                 "Creating GenericBeBoBProtocol for vendor=0x%06x model=0x%06x node=0x%04x",
                 vendorId, modelId, nodeId);
        return std::make_unique<BeBoB::GenericBeBoBProtocol>(
            busOps, busInfo, route, irmClient, cmpClient, timerScheduler,
            BeBoB::DeviceModel{});
    }

    // Unknown device
    return nullptr;
}

} // namespace ASFW::Audio
