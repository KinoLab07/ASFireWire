// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DICETcatProtocol.hpp - Generic DICE/TCAT protocol state and duplex control

#pragma once

#include "../../Duplex/IDuplexDeviceControl.hpp"
#include "../Core/DICEDuplexBringupController.hpp"
#include "../Core/DICETransaction.hpp"
#include "../Core/DICETypes.hpp"
#include "../../IDeviceProtocol.hpp"
#include "../../../../Protocols/Ports/ProtocolRegisterIO.hpp"

#include <atomic>
#include <functional>
#include <optional>

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::Scheduling {
class ITimerScheduler;
}

namespace ASFW::Audio::DICE::TCAT {

// Separates DICE's operational stream topology from the channels the HAL
// publishes. Some hardware needs both DICE directions active for its clock or
// firmware protocol while exposing only one analog/audio direction to users.
// One selectable router table: the wire entries plus how it presents itself.
struct DICERouterProgram final {
    const uint16_t* entries{nullptr};
    uint32_t entryCount{0};
    // Shown in logs and offered to control surfaces. Stable, not localised.
    const char* name{""};
    // Value written to the front-panel LED's monitor-select field when this
    // program is active. On the Mbox Pro 0 leaves the light off, so programs
    // start at 1.
    uint8_t ledSelect{0};
};

struct DICETcatRuntimePolicy final {
    bool exposeDeviceToHostToCoreAudio{true};
    // Keep the target-rate transition strict, but do not require a GLOBAL
    // source-lock indication before/just after host IT starts. This is for
    // devices whose selected receive-clock path only locks once host packets
    // are flowing; it does not select ARX1 as a clock source.
    bool requireSourceLockBeforeStreamEnable{true};
    bool requireSourceLockAtConfirm{true};

    // Device-specific TCAT EAP router program, written once before the first
    // duplex bring-up. Some devices (Avid Mbox Pro) power up with their output
    // pairs routed to MUTED and stay silent until a host programs the router.
    // Entries are in TCAT wire order: (dst << 8) | src.
    const uint16_t* startupRouterEntries{nullptr};
    uint32_t startupRouterEntryCount{0};

    // Selectable router programs. Some hardware cannot drive all of its
    // outputs at once and needs a different router table per output mode: the
    // Avid Mbox Pro keeps only one serial port bank clocked, so its line
    // outputs and its headphones are separate programs. Avid's own driver
    // works the same way - its front-panel handler (LDevice::SetSpeakerSelect)
    // rewrites the router table and repaints the LED, and sends nothing else
    // to the device.
    //
    // When present, program 0 replaces startupRouterEntries at bring-up. The
    // list is ordered; the front-panel button cycles through it, and a control
    // surface can select any entry by index.
    const DICERouterProgram* routerPrograms{nullptr};
    uint32_t routerProgramCount{0};
    // Notification bits that advance to the next program. Zero disables the
    // front-panel trigger without disabling programmatic selection.
    uint32_t routerCycleNotifyMask{0};
    // Track the active program in the front-panel LED's monitor-select field
    // (application section +0x14, low byte bits 6-5).
    bool driveFrontPanelLed{false};

    // Per-output trim registers inside the TCAT application (vendor) section.
    // One quadlet each, starting at this byte offset. Zero count disables.
    uint32_t outputTrimCount{0};
    uint32_t outputTrimOffset{0};

    // Mixer matrix geometry. The TCAT mixer section holds one quadlet per
    // coefficient after a one-quadlet header, indexed output-major.
    uint32_t mixerInputs{0};
    uint32_t mixerOutputs{0};
    uint16_t mixerUnityGain{0x4000};

    // Coefficients written once at bring-up. A device whose outputs are fed
    // from mixer outputs carries silence until these exist, so the router
    // program alone is not enough. Pairs of (index, gain), index being
    // output * mixerInputs + input.
    const uint16_t* startupMixerCoefficients{nullptr};   // flattened pairs
    uint32_t startupMixerCoefficientCount{0};
};

class DICETcatProtocol final : public Audio::IDeviceProtocol,
                               public Audio::IDuplexDeviceControl {
public:
    using VoidCallback = std::function<void(IOReturn)>;
    using PrepareCallback = IDuplexDeviceControl::PrepareCallback;
    using StageCallback = IDuplexDeviceControl::StageCallback;
    using ConfirmCallback = IDuplexDeviceControl::ConfirmCallback;
    using ClockApplyCallback = IDuplexDeviceControl::ClockApplyCallback;
    using HealthCallback = IDuplexDeviceControl::HealthCallback;

    DICETcatProtocol(Protocols::Ports::FireWireBusOps& busOps,
                     Protocols::Ports::FireWireBusInfo& busInfo,
                     Discovery::DeviceRegistry& routeRegistry,
                     const Discovery::DeviceRouteToken& route,
                     ::ASFW::IRM::IRMClient* irmClient = nullptr,
                     ::ASFW::Scheduling::ITimerScheduler* timerScheduler = nullptr,
                     DICETcatRuntimePolicy runtimePolicy = {});

    IOReturn Initialize() override;
    IOReturn Shutdown() override;
    const char* GetName() const override { return "TCAT DICE"; }
    Audio::IDuplexDeviceControl* AsDuplexDeviceControl() noexcept override { return this; }
    const Audio::IDuplexDeviceControl* AsDuplexDeviceControl() const noexcept override { return this; }

    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;
    bool GetChannelLabels(std::vector<std::string>& inNames,
                          std::vector<std::string>& outNames) const override;

    void PrepareDuplex(const AudioDuplexChannels& channels,
                       const AudioClockConfig& desiredClock,
                       PrepareCallback callback) override;
    void ProgramRx(StageCallback callback) override;
    void ProgramTxAndEnableDuplex(StageCallback callback) override;
    void ConfirmDuplexStart(ConfirmCallback callback) override;
    void ApplyClockConfig(const AudioClockConfig& desiredClock,
                          ClockApplyCallback callback) override;
    void ReadDuplexHealth(HealthCallback callback) override;
    void EnsureRuntimeStreamGeometry(VoidCallback callback) override;
    void SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept override;

    // --- Output modes (selectable router programs) ---------------------------
    // Exposed so a control surface can drive the same switch the front-panel
    // button does.
    [[nodiscard]] uint32_t RouterProgramCount() const noexcept {
        return runtimePolicy_.routerProgramCount;
    }
    [[nodiscard]] uint32_t ActiveRouterProgram() const noexcept {
        return activeRouterProgram_.load(std::memory_order_acquire);
    }
    [[nodiscard]] const char* RouterProgramName(uint32_t index) const noexcept;
    void SelectRouterProgram(uint32_t index, VoidCallback callback);
    void CycleRouterProgram();

    // IDeviceProtocol output-mode surface, so a control surface can drive the
    // same switch without knowing this is a DICE device.
    uint32_t GetOutputModeCount() const override { return RouterProgramCount(); }
    uint32_t GetActiveOutputMode() const override { return ActiveRouterProgram(); }
    const char* GetOutputModeName(uint32_t index) const override {
        return RouterProgramName(index);
    }
    IOReturn SelectOutputMode(uint32_t index) override;

    uint32_t GetOutputTrimCount() const override { return runtimePolicy_.outputTrimCount; }
    uint8_t GetOutputTrim(uint32_t index) const override;
    IOReturn SetOutputTrim(uint32_t index, uint8_t value) override;
    IOReturn RefreshOutputTrims() override;

    uint32_t GetMixerInputCount() const override { return runtimePolicy_.mixerInputs; }
    uint32_t GetMixerOutputCount() const override { return runtimePolicy_.mixerOutputs; }
    uint16_t GetMixerUnityGain() const override { return runtimePolicy_.mixerUnityGain; }
    uint16_t GetMixerCoefficient(uint32_t out, uint32_t in) const override;
    IOReturn SetMixerCoefficient(uint32_t out, uint32_t in, uint16_t gain) override;
    IOReturn RefreshMixer() override;

    uint32_t GetPeakCount() const override;
    uint32_t GetPeakEntry(uint32_t index) const override;
    IOReturn RefreshPeaks() override;
    ::ASFW::IRM::IRMClient* GetIRMClient() const override { return irmClient_; }

    void PrepareDuplex48k(const AudioDuplexChannels& channels, VoidCallback callback) override;
    void ProgramRxForDuplex48k(VoidCallback callback) override;
    void ProgramTxAndEnableDuplex48k(VoidCallback callback) override;
    void ConfirmDuplex48kStart(VoidCallback callback) override;
    IOReturn StopDuplex() override;
    void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                              Protocols::AVC::FCPTransport* transport) override;

    [[nodiscard]] Protocols::Ports::ProtocolRegisterIO& IO() noexcept { return io_; }
    [[nodiscard]] DICETransaction& Transaction() noexcept { return diceReader_; }

private:
    friend class DICETcatProtocolTestPeer;

    [[nodiscard]] static bool MakeDiceClockConfiguration(
        const AudioClockConfig& requested,
        DiceClockConfiguration& out) noexcept;
    void EnsureSectionsLoaded(VoidCallback callback);
    void ApplyStartupRouter(VoidCallback callback);

    // The router program lives in volatile registers, so what the device is
    // actually holding is read back rather than assumed; see ApplyStartupRouter.
    using CountCallback = std::function<void(IOReturn, uint32_t)>;
    void ReadRouterEntryCount(CountCallback callback);
    void ReadRouterEntryCountAt(const ExtensionSections& ext, CountCallback callback);
    void ProgramStartupRouter(const uint16_t* entries, uint32_t count, uint8_t ledSelect,
                              uint32_t attempt, VoidCallback callback);
    void EnsureRuntimeCapsLoaded(VoidCallback callback);
    void CacheRuntimeCaps(const GlobalState& global,
                          const StreamConfig& tx,
                          const StreamConfig& rx) noexcept;
    void CacheRuntimeCaps(const AudioStreamRuntimeCaps& caps) noexcept;
    void ResetRuntimeCaps() noexcept;

    Protocols::Ports::FireWireBusInfo& busInfo_;
    ::ASFW::IRM::IRMClient* irmClient_{nullptr};
    Protocols::Ports::ProtocolRegisterIO io_;
    DICETransaction diceReader_;
    std::optional<ASFW::Audio::DICE::DICEDuplexBringupController> duplexCtrl_;
    const std::atomic<bool>* teardownCancel_{nullptr};
    ::ASFW::Scheduling::ITimerScheduler* timerScheduler_{nullptr};  // driver-owned
    DICETcatRuntimePolicy runtimePolicy_{};
    bool startupRouterApplied_{false};

    // --- Front-panel router toggle -------------------------------------------
    void WriteRouterProgram(const uint16_t* entries, uint32_t count, VoidCallback callback);
    void OnDeviceNotification(uint32_t bits) noexcept;
    static void NotificationThunk(void* context, uint32_t bits) noexcept;
    void ServiceRouterCycle();
    void WriteFrontPanelLed(uint8_t select);

    void ReadOutputTrims();
    void ReadMixerChunk(uint32_t mixerBase, uint32_t firstIndex);

    // The EAP section table is fixed once the device has enumerated, so it is
    // cached on the first successful read. Re-reading it before every write
    // doubled the traffic of a fader drag and the extra reads crowded the
    // async queue until most coefficient writes were dropped.
    void CacheExtensions(const ExtensionSections& ext);
    bool CachedExtensions(ExtensionSections& out) const;

    // One write in flight per cell, with the newest value coalesced behind it.
    // A drag emits far more moves than the bus can carry, and without this the
    // queue fills with stale intermediate values and the final one is lost.
    void IssueMixerWrite(uint32_t index);
    bool ClaimMixerCell(uint32_t index);
    void WriteMixerCell(const ExtensionSections& ext, uint32_t index);
    void WriteStartupMixerCoefficients();
    void WriteStartupMixerCoefficient(uint32_t mixerBase, uint32_t pair);

    static constexpr uint32_t kMaxPeakEntries = 128;
    std::atomic<uint32_t> peakEntries_[kMaxPeakEntries]{};
    std::atomic<uint32_t> peakCount_{0};

    static constexpr uint32_t kMaxMixerCells = 512;
    std::atomic<uint16_t> mixerCells_[kMaxMixerCells]{};

    static constexpr uint32_t kMaxOutputTrims = 8;
    std::atomic<uint8_t> outputTrims_[kMaxOutputTrims]{};

    std::atomic<bool> togglePending_{false};
    std::atomic<uint32_t> activeRouterProgram_{0};  // 0 = startup, 1 = alt
    GeneralSections sections_{};
    bool initialized_{false};
    bool sectionsLoaded_{false};

    // Published through extValid_ (release on store, acquire on load), the
    // same fence idiom the runtime caps arrays use below.
    ExtensionSections extSections_{};
    std::atomic<bool> extValid_{false};

    std::atomic<bool> mixerCellBusy_[kMaxMixerCells]{};
    std::atomic<bool> mixerCellDirty_[kMaxMixerCells]{};
    std::atomic<uint64_t> mixerCellIssuedAt_[kMaxMixerCells]{};

    // The user-selected device clock, remembered across StartIO cycles so the
    // per-StartIO bring-up (PrepareDuplex48k) targets the live rate instead of a
    // hardcoded 48 kHz. Updated whenever a real clock is applied (ApplyClockConfig
    // for idle rate changes, PrepareDuplex for restarts). Default {0} means
    // "nothing selected yet" → PrepareDuplex48k falls back to 48 kHz. Without this
    // every StartIO rewrites CLOCK_SELECT back to 48 kHz and fights a 44.1 kHz
    // selection, flapping the device PLL and starving audio.
    AudioClockConfig selectedClock_{};

    std::atomic<uint32_t> runtimeSampleRateHz_{0};
    std::atomic<uint32_t> hostInputPcmChannels_{0};
    std::atomic<uint32_t> hostOutputPcmChannels_{0};
    std::atomic<uint32_t> deviceToHostAm824Slots_{0};
    std::atomic<uint32_t> hostToDeviceAm824Slots_{0};
    std::atomic<uint32_t> deviceToHostIsoChannel_{AudioStreamRuntimeCaps::kInvalidIsoChannel};
    std::atomic<uint32_t> hostToDeviceIsoChannel_{AudioStreamRuntimeCaps::kInvalidIsoChannel};

    // Per-stream wire geometry (DICE TX_NUMBER/RX_NUMBER + per-stream channels).
    // Counts are atomic; the arrays are plain and published through the
    // runtimeCapsValid_ release/acquire fence (written before the release-store,
    // read after the acquire-load), mirroring the scalar fields above.
    std::atomic<uint32_t> deviceToHostStreamCount_{0};
    std::atomic<uint32_t> hostToDeviceStreamCount_{0};
    AudioStreamWireInfo deviceToHostStreams_[kMaxAudioStreamsPerDirection]{};
    AudioStreamWireInfo hostToDeviceStreams_[kMaxAudioStreamsPerDirection]{};

    // Per-channel device labels, flattened across this direction's streams in
    // channel order (input == device TX, output == device RX). Published
    // through the runtimeCapsValid_ release/acquire fence like the arrays above;
    // only the (global, tx, rx) cache path fills them (the caps-only overload
    // leaves them intact). Covers the widest supported interface (32x32).
    static constexpr uint32_t kMaxChannelLabels = 32;
    std::atomic<uint32_t> inputChannelLabelCount_{0};
    std::atomic<uint32_t> outputChannelLabelCount_{0};
    char inputChannelLabels_[kMaxChannelLabels][64]{};
    char outputChannelLabels_[kMaxChannelLabels][64]{};

    std::atomic<bool> runtimeCapsValid_{false};
};

} // namespace ASFW::Audio::DICE::TCAT
