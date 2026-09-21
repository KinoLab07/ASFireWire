// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DICETcatProtocol.cpp - Generic DICE/TCAT protocol state and duplex control

#include "DICETcatProtocol.hpp"

#include "../../../../Common/TimingUtils.hpp"
#include "../../../../Logging/Logging.hpp"
#include "../Core/DICENotificationMailbox.hpp"

#include <memory>
#include <utility>

namespace ASFW::Audio::DICE::TCAT {

namespace {

// Avid Mbox Pro UILEDState, inside the TCAT application (vendor) section.
// Extracted from Avid's LaunchdDaemon: DiceUtils::FeatureQuadlet<Murphy::UILEDState>.
constexpr uint32_t kMboxProUILedStateOffset = 0x14;

[[nodiscard]] bool HasUsableRuntimeCaps(const AudioStreamRuntimeCaps& caps) noexcept {
    // CoreAudio visibility is not a wire-topology signal. A Weiss INT202, for
    // example, deliberately has zero HAL input channels while still reporting
    // and using a device->host DICE stream. Validate the physical DICE sections
    // through their slot counts instead.
    return caps.sampleRateHz != 0 &&
        caps.hostOutputPcmChannels != 0 &&
        caps.deviceToHostAm824Slots != 0 &&
        caps.hostToDeviceAm824Slots != 0;
}

void LogRuntimeCaps(const char* source, const AudioStreamRuntimeCaps& caps) {
    ASFW_LOG(DICE,
             "DICETcatProtocol: runtime caps source=%{public}s rate=%u in=%u out=%u d2hSlots=%u h2dSlots=%u usable=%u",
             source,
             caps.sampleRateHz,
             caps.hostInputPcmChannels,
             caps.hostOutputPcmChannels,
             caps.deviceToHostAm824Slots,
             caps.hostToDeviceAm824Slots,
             HasUsableRuntimeCaps(caps) ? 1U : 0U);
}

void LogStreamConfigSummary(const char* label, const StreamConfig& config) {
    ASFW_LOG(DICE,
             "DICETcatProtocol: %{public}s stream summary count=%u pcm=%u midi=%u am824=%u entrySize=%u parsedEntrySize=%u",
             label,
             config.numStreams,
             config.TotalPcmChannels(),
             config.TotalMidiPorts(),
             config.TotalAm824Slots(),
             config.entrySizeBytes,
             config.parsedEntrySizeBytes);
}

} // namespace

bool DICETcatProtocol::MakeDiceClockConfiguration(
    const AudioClockConfig& requested, DiceClockConfiguration& out) noexcept {
    if (!IsSupportedAudioClockConfig(requested)) {
        return false;
    }
    // The DICE adapter owns the register encoding: Linux selects the requested
    // rate by updating GLOBAL_CLOCK_SELECT while preserving the source bits
    // (dice-stream.c:60-85; dice-interface.h:80-95). Encode the requested rate
    // via the standard table; source stays Internal (bring-up policy).
    uint32_t clockSelect = 0;
    if (!DiceClockSelectForRate(requested.sampleRateHz, ClockSource::Internal,
                                clockSelect)) {
        return false;
    }
    out = DiceClockConfiguration{
        .sampleRateHz = requested.sampleRateHz,
        .clockSelect = clockSelect,
    };
    return true;
}

DICETcatProtocol::DICETcatProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                   Protocols::Ports::FireWireBusInfo& busInfo,
                                   Discovery::DeviceRegistry& routeRegistry,
                                   const Discovery::DeviceRouteToken& route,
                                   ::ASFW::IRM::IRMClient* irmClient,
                                   ::ASFW::Scheduling::ITimerScheduler* timerScheduler,
                                   DICETcatRuntimePolicy runtimePolicy)
    : busInfo_(busInfo)
    , irmClient_(irmClient)
    , io_(busOps, busInfo, routeRegistry, route)
    , diceReader_(io_)
    , timerScheduler_(timerScheduler)
    , runtimePolicy_(runtimePolicy) {
}

IOReturn DICETcatProtocol::Initialize() {
    if (!duplexCtrl_) {
        duplexCtrl_.emplace(diceReader_, io_, busInfo_, nullptr /*workQueue*/, GeneralSections{},
                            timerScheduler_,
                            DICEBringupPolicy{
                                .requireSourceLockBeforeStreamEnable =
                                    runtimePolicy_.requireSourceLockBeforeStreamEnable,
                                .requireSourceLockAtConfirm =
                                    runtimePolicy_.requireSourceLockAtConfirm,
                            });
        duplexCtrl_->SetTeardownCancelToken(teardownCancel_);
    }

    if (runtimePolicy_.routerCycleNotifyMask != 0 && runtimePolicy_.routerProgramCount >= 2) {
        NotificationMailbox::SetObserver(this, &DICETcatProtocol::NotificationThunk);
        ASFW_LOG(DICE, "DICETcatProtocol: front-panel cycles %u output modes (mask=0x%08x)",
                 runtimePolicy_.routerProgramCount, runtimePolicy_.routerCycleNotifyMask);
    }

    initialized_ = true;
    ASFW_LOG(DICE, "DICETcatProtocol::Initialize defers generic discovery until runtime");
    return kIOReturnSuccess;
}

IOReturn DICETcatProtocol::Shutdown() {
    NotificationMailbox::ClearObserver(this);
    if (duplexCtrl_) {
        if (duplexCtrl_->IsPrepared() || duplexCtrl_->IsRunning()) {
            const IOReturn stopStatus = duplexCtrl_->StopDuplex();
            if (stopStatus != kIOReturnSuccess && stopStatus != kIOReturnUnsupported) {
                ASFW_LOG(DICE, "DICETcatProtocol::Shutdown duplex stop failed: 0x%x", stopStatus);
            }
        }

        duplexCtrl_->ReleaseOwner([](IOReturn status) {
            if (status != kIOReturnSuccess) {
                ASFW_LOG(DICE, "DICETcatProtocol::Shutdown ReleaseOwner failed: 0x%x", status);
            }
        });
    }

    sections_ = {};
    sectionsLoaded_ = false;
    initialized_ = false;
    ResetRuntimeCaps();
    return kIOReturnSuccess;
}

bool DICETcatProtocol::GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
    if (!runtimeCapsValid_.load(std::memory_order_acquire)) {
        return false;
    }

    outCaps.sampleRateHz = runtimeSampleRateHz_.load(std::memory_order_relaxed);
    outCaps.hostInputPcmChannels = hostInputPcmChannels_.load(std::memory_order_relaxed);
    outCaps.hostOutputPcmChannels = hostOutputPcmChannels_.load(std::memory_order_relaxed);
    outCaps.deviceToHostAm824Slots = deviceToHostAm824Slots_.load(std::memory_order_relaxed);
    outCaps.hostToDeviceAm824Slots = hostToDeviceAm824Slots_.load(std::memory_order_relaxed);
    outCaps.deviceToHostIsoChannel =
        static_cast<uint8_t>(deviceToHostIsoChannel_.load(std::memory_order_relaxed));
    outCaps.hostToDeviceIsoChannel =
        static_cast<uint8_t>(hostToDeviceIsoChannel_.load(std::memory_order_relaxed));

    // Per-stream geometry: the runtimeCapsValid_ acquire-load above establishes
    // happens-before with the writer's release-store, so the plain arrays are
    // safe to read here.
    outCaps.deviceToHostStreamCount = deviceToHostStreamCount_.load(std::memory_order_relaxed);
    outCaps.hostToDeviceStreamCount = hostToDeviceStreamCount_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxAudioStreamsPerDirection; ++i) {
        outCaps.deviceToHostStreams[i] = deviceToHostStreams_[i];
        outCaps.hostToDeviceStreams[i] = hostToDeviceStreams_[i];
    }
    return true;
}

void DICETcatProtocol::EnsureRuntimeStreamGeometry(VoidCallback callback) {
    EnsureRuntimeCapsLoaded(std::move(callback));
}

void DICETcatProtocol::SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept {
    teardownCancel_ = cancel;
    if (duplexCtrl_) {
        duplexCtrl_->SetTeardownCancelToken(cancel);
    }
}

void DICETcatProtocol::WriteRouterProgram(const uint16_t* entries,
                                         uint32_t count,
                                         VoidCallback callback) {
    diceReader_.ReadExtensionSections(
        [this, entries, count, callback = std::move(callback)](IOReturn status,
                                                               ExtensionSections ext) mutable {
            if (status != kIOReturnSuccess) {
                ASFW_LOG(DICE, "WriteRouterProgram: extension sections unreadable (0x%08x)", status);
                callback(status);
                return;
            }

            const uint32_t routerBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.router);
            const uint32_t commandBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.command);
            if (routerBase == kDICEExtensionOffset || commandBase == kDICEExtensionOffset) {
                ASFW_LOG(DICE, "WriteRouterProgram: device exposes no TCAT router/command section");
                callback(kIOReturnUnsupported);
                return;
            }

            // Wire image: quadlet 0 is the entry count, then one quadlet per entry.
            const size_t bytes = (static_cast<size_t>(count) + 1U) * 4U;
            auto buffer = std::make_shared<std::vector<uint8_t>>(bytes, 0U);
            FW::WriteBE32(buffer->data(), count);
            for (uint32_t i = 0; i < count; ++i) {
                FW::WriteBE32(buffer->data() + (i + 1U) * 4U, entries[i]);
            }

            (void)io_.WriteBlock(
                MakeDICEAddress(routerBase),
                std::span<const uint8_t>(buffer->data(), buffer->size()),
                [this, commandBase, count, buffer, callback = std::move(callback)](
                    Async::AsyncStatus writeStatus) mutable {
                    const IOReturn st = Protocols::Ports::MapAsyncStatusToIOReturn(writeStatus);
                    if (st != kIOReturnSuccess) {
                        ASFW_LOG(DICE, "WriteRouterProgram: router write failed (0x%08x)", st);
                        callback(st);
                        return;
                    }

                    // Commit for every rate mode, as the vendor driver does. A
                    // low-mode-only commit is accepted but does not take effect.
                    const uint32_t opcode = ExtensionCommandOpcode::kExecute |
                                            ExtensionCommandOpcode::kRateLow |
                                            ExtensionCommandOpcode::kRateMiddle |
                                            ExtensionCommandOpcode::kRateHigh |
                                            ExtensionCommandOpcode::kLoadRouter;
                    (void)io_.WriteQuadBE(
                        MakeDICEAddress(commandBase + ExtensionCommandOffset::kOpcode),
                        opcode,
                        [count, opcode, callback = std::move(callback)](
                            Async::AsyncStatus cmdStatus) mutable {
                            const IOReturn cst = Protocols::Ports::MapAsyncStatusToIOReturn(cmdStatus);
                            if (cst == kIOReturnSuccess) {
                                ASFW_LOG(DICE,
                                         "WriteRouterProgram: programmed %u router entries (opcode=0x%08x)",
                                         count, opcode);
                            } else {
                                ASFW_LOG(DICE, "WriteRouterProgram: commit failed (0x%08x)", cst);
                            }
                            callback(cst);
                        });
                });
        });
}

void DICETcatProtocol::ReadRouterEntryCount(CountCallback callback) {
    ExtensionSections ext{};
    if (CachedExtensions(ext)) {
        ReadRouterEntryCountAt(ext, std::move(callback));
        return;
    }
    diceReader_.ReadExtensionSections(
        [this, callback = std::move(callback)](IOReturn status, ExtensionSections fresh) mutable {
            if (status != kIOReturnSuccess) {
                callback(status, 0U);
                return;
            }
            CacheExtensions(fresh);
            ReadRouterEntryCountAt(fresh, std::move(callback));
        });
}

void DICETcatProtocol::ReadRouterEntryCountAt(const ExtensionSections& ext,
                                              CountCallback callback) {
    const uint32_t routerBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.router);
    if (routerBase == kDICEExtensionOffset) {
        callback(kIOReturnUnsupported, 0U);
        return;
    }
    (void)io_.ReadQuadBE(MakeDICEAddress(routerBase),
                         [callback = std::move(callback)](Async::AsyncStatus st,
                                                          uint32_t value) mutable {
                             const IOReturn r = Protocols::Ports::MapAsyncStatusToIOReturn(st);
                             callback(r, r == kIOReturnSuccess ? value : 0U);
                         });
}

void DICETcatProtocol::ApplyStartupRouter(VoidCallback callback) {
    // Devices whose outputs ship routed to MUTED need a router program before
    // anything reaches their analog stage. The TCAT router is not flash-backed,
    // so the program is only ever as durable as the device's power.
    //
    // This used to latch on the first success and never look again, and that
    // was wrong: a burst of bus resets around bring-up can leave the device
    // holding nothing while every write still reported success, and the latch
    // then guaranteed that nobody would ever put the program back. The device
    // stayed silent until someone reprogrammed it by hand. So rather than
    // trusting the latch, read back the entry count the device is really
    // holding and reprogram whenever it does not match.

    const uint16_t* entries = runtimePolicy_.startupRouterEntries;
    uint32_t count = runtimePolicy_.startupRouterEntryCount;
    uint8_t ledSelect = 0;
    if (runtimePolicy_.routerPrograms != nullptr && runtimePolicy_.routerProgramCount > 0) {
        const auto& program = runtimePolicy_.routerPrograms[0];
        entries = program.entries;
        count = program.entryCount;
        ledSelect = program.ledSelect;
    }
    if (entries == nullptr || count == 0) {
        callback(kIOReturnSuccess);
        return;
    }

    ReadRouterEntryCount([this, entries, count, ledSelect, callback = std::move(callback)](
                             IOReturn status, uint32_t live) mutable {
        if (startupRouterApplied_ && status == kIOReturnSuccess && live == count) {
            callback(kIOReturnSuccess);
            return;
        }
        if (startupRouterApplied_) {
            ASFW_LOG(DICE, "Startup router lost: device holds %u of %u entries; reprogramming",
                     live, count);
            startupRouterApplied_ = false;
        }
        ProgramStartupRouter(entries, count, ledSelect, 0U, std::move(callback));
    });
}

void DICETcatProtocol::ProgramStartupRouter(const uint16_t* entries, uint32_t count,
                                            uint8_t ledSelect, uint32_t attempt,
                                            VoidCallback callback) {
    WriteRouterProgram(
        entries, count,
        [this, entries, count, ledSelect, attempt, callback = std::move(callback)](
            IOReturn status) mutable {
            if (status != kIOReturnSuccess) {
                if (attempt == 0U) {
                    ASFW_LOG(DICE, "Startup router write failed (0x%08x); retrying once", status);
                    ProgramStartupRouter(entries, count, ledSelect, attempt + 1U,
                                         std::move(callback));
                    return;
                }
                callback(status);
                return;
            }

            // A commit that reports success is not proof the table is there, so
            // confirm before latching. This is the check that turns a lost
            // program into one retry instead of a silent device.
            ReadRouterEntryCount([this, entries, count, ledSelect, attempt,
                                  callback = std::move(callback)](IOReturn readStatus,
                                                                  uint32_t live) mutable {
                if (attempt == 0U && readStatus == kIOReturnSuccess && live != count) {
                    ASFW_LOG(DICE, "Startup router did not stick (%u of %u); retrying once",
                             live, count);
                    ProgramStartupRouter(entries, count, ledSelect, attempt + 1U,
                                         std::move(callback));
                    return;
                }
                startupRouterApplied_ = true;
                activeRouterProgram_.store(0, std::memory_order_release);
                WriteFrontPanelLed(ledSelect);
                if (runtimePolicy_.outputTrimCount > 0) {
                    ReadOutputTrims();
                }
                WriteStartupMixerCoefficients();
                if (runtimePolicy_.mixerInputs > 0) {
                    (void)RefreshMixer();
                }
                callback(kIOReturnSuccess);
            });
        });
}

// --- Output modes (selectable router programs) -------------------------------
//
// The Mbox Pro keeps one serial port bank clocked at a time, so its line
// outputs and its headphones need separate router programs. Avid's driver
// solves it the same way: the front-panel button rewrites the router table and
// repaints the LED, and sends nothing else to the hardware.

const char* DICETcatProtocol::RouterProgramName(uint32_t index) const noexcept {
    if (runtimePolicy_.routerPrograms == nullptr ||
        index >= runtimePolicy_.routerProgramCount) {
        return "";
    }
    return runtimePolicy_.routerPrograms[index].name;
}

void DICETcatProtocol::SelectRouterProgram(uint32_t index, VoidCallback callback) {
    if (runtimePolicy_.routerPrograms == nullptr ||
        index >= runtimePolicy_.routerProgramCount) {
        callback(kIOReturnBadArgument);
        return;
    }
    if (!initialized_) {
        callback(kIOReturnNotReady);
        return;
    }

    const auto& program = runtimePolicy_.routerPrograms[index];
    if (program.entries == nullptr || program.entryCount == 0) {
        callback(kIOReturnBadArgument);
        return;
    }

    ASFW_LOG(DICE, "Output mode: selecting '%{public}s' (program %u)", program.name, index);

    const uint8_t ledSelect = program.ledSelect;
    WriteRouterProgram(program.entries, program.entryCount,
                       [this, index, ledSelect, callback = std::move(callback)](
                           IOReturn status) mutable {
                           if (status != kIOReturnSuccess) {
                               ASFW_LOG(DICE, "Output mode: router program failed (0x%08x)", status);
                               callback(status);
                               return;
                           }
                           activeRouterProgram_.store(index, std::memory_order_release);
                           WriteFrontPanelLed(ledSelect);
                           callback(status);
                       });
}

IOReturn DICETcatProtocol::SelectOutputMode(uint32_t index) {
    // Called from the user client's thread. Hand the FireWire work to the
    // driver's queue, the same way the front-panel path does, instead of
    // transacting from whatever thread asked. Reports whether the request was
    // accepted, not whether the device finished.
    if (runtimePolicy_.routerPrograms == nullptr ||
        index >= runtimePolicy_.routerProgramCount) {
        return kIOReturnBadArgument;
    }
    if (!initialized_) {
        return kIOReturnNotReady;
    }
    if (timerScheduler_ == nullptr) {
        return kIOReturnNotReady;
    }
    (void)timerScheduler_->ScheduleAfter(
        0, [this, index]() { SelectRouterProgram(index, [](IOReturn) {}); });
    return kIOReturnSuccess;
}

void DICETcatProtocol::CycleRouterProgram() {
    const uint32_t total = runtimePolicy_.routerProgramCount;
    if (total < 2) {
        return;
    }
    const uint32_t next =
        (activeRouterProgram_.load(std::memory_order_acquire) + 1U) % total;
    SelectRouterProgram(next, [](IOReturn) {});
}

void DICETcatProtocol::NotificationThunk(void* context, uint32_t bits) noexcept {
    if (auto* self = static_cast<DICETcatProtocol*>(context)) {
        self->OnDeviceNotification(bits);
    }
}

void DICETcatProtocol::OnDeviceNotification(uint32_t bits) noexcept {
    const uint32_t mask = runtimePolicy_.routerCycleNotifyMask;
    if (mask == 0 || (bits & mask) == 0 || runtimePolicy_.routerProgramCount < 2) {
        return;
    }
    // This runs on the local-request path. Coalesce and hand the FireWire work
    // to the driver's queue instead of transacting from here.
    if (togglePending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (timerScheduler_ == nullptr) {
        togglePending_.store(false, std::memory_order_release);
        ASFW_LOG(DICE, "Front-panel button ignored: no timer scheduler");
        return;
    }
    (void)timerScheduler_->ScheduleAfter(0, [this]() { ServiceRouterCycle(); });
}

void DICETcatProtocol::ServiceRouterCycle() {
    togglePending_.store(false, std::memory_order_release);
    if (!initialized_) {
        return;
    }
    CycleRouterProgram();
}

// --- Per-output trim ---------------------------------------------------------
//
// The Mbox Pro keeps one attenuation register per analog output inside the
// vendor application section (DAC1..DAC6 in Avid's daemon). Reads are
// asynchronous, so the value the control surface sees comes from a cache the
// protocol refreshes on demand and updates on every write.

uint8_t DICETcatProtocol::GetOutputTrim(uint32_t index) const {
    if (index >= runtimePolicy_.outputTrimCount || index >= kMaxOutputTrims) {
        return 0;
    }
    return outputTrims_[index].load(std::memory_order_acquire);
}

IOReturn DICETcatProtocol::SetOutputTrim(uint32_t index, uint8_t value) {
    if (runtimePolicy_.outputTrimCount == 0 || index >= runtimePolicy_.outputTrimCount ||
        index >= kMaxOutputTrims) {
        return kIOReturnBadArgument;
    }
    if (!initialized_) {
        return kIOReturnNotReady;
    }

    // Optimistic: the cache moves now so the UI stays responsive, and a failed
    // write is corrected by the next refresh.
    outputTrims_[index].store(value, std::memory_order_release);

    const uint32_t offset = runtimePolicy_.outputTrimOffset + index * 4U;
    diceReader_.ReadExtensionSections(
        [this, offset, index, value](IOReturn status, ExtensionSections ext) {
            if (status != kIOReturnSuccess) {
                return;
            }
            CacheExtensions(ext);
            const uint32_t appBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.application);
            if (appBase == kDICEExtensionOffset) {
                return;
            }
            (void)io_.WriteQuadBE(
                MakeDICEAddress(appBase + offset), static_cast<uint32_t>(value),
                [index, value](Async::AsyncStatus st) {
                    if (Protocols::Ports::MapAsyncStatusToIOReturn(st) != kIOReturnSuccess) {
                        ASFW_LOG(DICE, "Output trim %u write failed (value=%u)", index, value);
                    }
                });
        });
    return kIOReturnSuccess;
}

IOReturn DICETcatProtocol::RefreshOutputTrims() {
    if (runtimePolicy_.outputTrimCount == 0) {
        return kIOReturnUnsupported;
    }
    if (!initialized_) {
        return kIOReturnNotReady;
    }
    ReadOutputTrims();
    return kIOReturnSuccess;
}

void DICETcatProtocol::ReadOutputTrims() {
    const uint32_t count = runtimePolicy_.outputTrimCount;
    const uint32_t offset = runtimePolicy_.outputTrimOffset;
    diceReader_.ReadExtensionSections(
        [this, count, offset](IOReturn status, ExtensionSections ext) {
            if (status != kIOReturnSuccess) {
                return;
            }
            CacheExtensions(ext);
            const uint32_t appBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.application);
            if (appBase == kDICEExtensionOffset) {
                return;
            }
            (void)io_.ReadBlock(
                MakeDICEAddress(appBase + offset), count * 4U,
                [this, count](Async::AsyncStatus st, std::span<const uint8_t> payload) {
                    if (Protocols::Ports::MapAsyncStatusToIOReturn(st) != kIOReturnSuccess ||
                        payload.size() < count * 4U) {
                        ASFW_LOG(DICE, "Output trim read failed");
                        return;
                    }
                    for (uint32_t i = 0; i < count && i < kMaxOutputTrims; ++i) {
                        // Big-endian quadlet; the trim lives in the low byte.
                        outputTrims_[i].store(payload[i * 4U + 3U], std::memory_order_release);
                    }
                });
        });
}

// --- Mixer matrix ------------------------------------------------------------
//
// The TCAT mixer section is a one-quadlet header followed by one quadlet per
// coefficient, indexed output-major: index = out * inputs + in. Unity is
// device specific (0x4000 on the Mbox Pro). Reads are chunked because the
// whole matrix is larger than a comfortable block transfer.

namespace {
constexpr uint32_t kMixerChunkQuadlets = 64;
}

uint16_t DICETcatProtocol::GetMixerCoefficient(uint32_t out, uint32_t in) const {
    if (out >= runtimePolicy_.mixerOutputs || in >= runtimePolicy_.mixerInputs) {
        return 0;
    }
    const uint32_t index = out * runtimePolicy_.mixerInputs + in;
    if (index >= kMaxMixerCells) {
        return 0;
    }
    return mixerCells_[index].load(std::memory_order_acquire);
}

IOReturn DICETcatProtocol::SetMixerCoefficient(uint32_t out, uint32_t in, uint16_t gain) {
    if (runtimePolicy_.mixerInputs == 0 || out >= runtimePolicy_.mixerOutputs ||
        in >= runtimePolicy_.mixerInputs) {
        return kIOReturnBadArgument;
    }
    if (!initialized_) {
        return kIOReturnNotReady;
    }
    const uint32_t index = out * runtimePolicy_.mixerInputs + in;
    if (index >= kMaxMixerCells) {
        return kIOReturnBadArgument;
    }

    // Optimistic, like the trims: the cache moves now so faders stay smooth.
    // The cache is also what the write reads from, so a move that arrives while
    // an earlier write is still in flight is not lost, it supersedes it.
    mixerCells_[index].store(gain, std::memory_order_release);
    IssueMixerWrite(index);
    return kIOReturnSuccess;
}

namespace {
// A coefficient write in flight for longer than this has lost its completion:
// a dropped transaction or a bus reset can swallow one. Without a way to take
// the cell back, a single lost completion strands it forever -- it stays marked
// busy, every later move is coalesced behind a write that will never finish,
// and that fader silently stops reaching the device for the rest of the
// session, which looks exactly like "the sound went and never came back".
constexpr uint64_t kMixerWriteStallNs = 500ULL * 1000ULL * 1000ULL;
}  // namespace

bool DICETcatProtocol::ClaimMixerCell(uint32_t index) {
    bool expected = false;
    if (mixerCellBusy_[index].compare_exchange_strong(expected, true,
                                                      std::memory_order_acq_rel)) {
        mixerCellIssuedAt_[index].store(mach_absolute_time(), std::memory_order_release);
        return true;
    }

    const uint64_t issued = mixerCellIssuedAt_[index].load(std::memory_order_acquire);
    const uint64_t now = mach_absolute_time();
    if (issued == 0 || now <= issued ||
        (now - issued) < ASFW::Timing::nanosToHostTicks(kMixerWriteStallNs)) {
        return false;
    }

    // Take the cell over. If the stale completion does arrive later it only
    // clears the flag, and the value written is always the newest one from the
    // cache, so a redundant write is harmless.
    ASFW_LOG(DICE, "Mixer coefficient %u write stalled; taking the cell over", index);
    mixerCellIssuedAt_[index].store(now, std::memory_order_release);
    return true;
}

void DICETcatProtocol::IssueMixerWrite(uint32_t index) {
    if (index >= kMaxMixerCells) {
        return;
    }

    // Claim the cell. If a write is already in flight, leave the value behind
    // as dirty and let that write's completion pick it up.
    if (!ClaimMixerCell(index)) {
        mixerCellDirty_[index].store(true, std::memory_order_release);
        // The in-flight write may have completed between the failed claim and
        // that store, in which case nobody would come back for the value, so
        // try once more to claim the cell ourselves.
        if (!ClaimMixerCell(index)) {
            return;
        }
        mixerCellDirty_[index].store(false, std::memory_order_release);
    }

    ExtensionSections ext{};
    if (!CachedExtensions(ext)) {
        // First write of the session: pay for the section read once, and the
        // callback caches it so no later write has to.
        diceReader_.ReadExtensionSections(
            [this, index](IOReturn status, ExtensionSections fresh) {
                if (status != kIOReturnSuccess) {
                    mixerCellBusy_[index].store(false, std::memory_order_release);
                    return;
                }
                CacheExtensions(fresh);
                WriteMixerCell(fresh, index);
            });
        return;
    }
    WriteMixerCell(ext, index);
}

void DICETcatProtocol::WriteMixerCell(const ExtensionSections& ext, uint32_t index) {
    const uint32_t mixerBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.mixer);
    if (mixerBase == kDICEExtensionOffset) {
        mixerCellBusy_[index].store(false, std::memory_order_release);
        return;
    }
    const uint16_t value = mixerCells_[index].load(std::memory_order_acquire);
    const Async::AsyncHandle queued = io_.WriteQuadBE(
        MakeDICEAddress(mixerBase + 4U + index * 4U), static_cast<uint32_t>(value),
        [this, index](Async::AsyncStatus st) {
            if (Protocols::Ports::MapAsyncStatusToIOReturn(st) != kIOReturnSuccess) {
                ASFW_LOG(DICE, "Mixer coefficient %u write failed", index);
            }
            mixerCellBusy_[index].store(false, std::memory_order_release);
            if (mixerCellDirty_[index].exchange(false, std::memory_order_acq_rel)) {
                IssueMixerWrite(index);
            }
        });
    if (!queued.IsValid()) {
        // Nothing was enqueued, so no completion will arrive to free the cell.
        mixerCellBusy_[index].store(false, std::memory_order_release);
    }
}

void DICETcatProtocol::CacheExtensions(const ExtensionSections& ext) {
    extSections_ = ext;
    extValid_.store(true, std::memory_order_release);
}

bool DICETcatProtocol::CachedExtensions(ExtensionSections& out) const {
    if (!extValid_.load(std::memory_order_acquire)) {
        return false;
    }
    out = extSections_;
    return true;
}

IOReturn DICETcatProtocol::RefreshMixer() {
    if (runtimePolicy_.mixerInputs == 0 || runtimePolicy_.mixerOutputs == 0) {
        return kIOReturnUnsupported;
    }
    if (!initialized_) {
        return kIOReturnNotReady;
    }
    diceReader_.ReadExtensionSections([this](IOReturn status, ExtensionSections ext) {
        if (status != kIOReturnSuccess) {
            return;
        }
        CacheExtensions(ext);
        const uint32_t mixerBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.mixer);
        if (mixerBase == kDICEExtensionOffset) {
            return;
        }
        ReadMixerChunk(mixerBase, 0);
    });
    return kIOReturnSuccess;
}

void DICETcatProtocol::ReadMixerChunk(uint32_t mixerBase, uint32_t firstIndex) {
    const uint32_t total = runtimePolicy_.mixerInputs * runtimePolicy_.mixerOutputs;
    if (firstIndex >= total || firstIndex >= kMaxMixerCells) {
        return;
    }
    const uint32_t remaining = total - firstIndex;
    const uint32_t chunk = remaining < kMixerChunkQuadlets ? remaining : kMixerChunkQuadlets;

    (void)io_.ReadBlock(
        MakeDICEAddress(mixerBase + 4U + firstIndex * 4U), chunk * 4U,
        [this, mixerBase, firstIndex, chunk](Async::AsyncStatus st,
                                             std::span<const uint8_t> payload) {
            if (Protocols::Ports::MapAsyncStatusToIOReturn(st) != kIOReturnSuccess ||
                payload.size() < chunk * 4U) {
                ASFW_LOG(DICE, "Mixer read failed at index %u", firstIndex);
                return;
            }
            for (uint32_t i = 0; i < chunk; ++i) {
                const uint32_t index = firstIndex + i;
                if (index >= kMaxMixerCells) {
                    break;
                }
                // Big-endian quadlet; the gain is the low 16 bits.
                const uint16_t gain =
                    static_cast<uint16_t>((payload[i * 4U + 2U] << 8) | payload[i * 4U + 3U]);
                mixerCells_[index].store(gain, std::memory_order_release);
            }
            // Walk the rest of the matrix.
            ReadMixerChunk(mixerBase, firstIndex + chunk);
        });
}

// Outputs fed from the mixer carry silence until the matrix has coefficients,
// so a router program is only half the configuration. Written once at bring-up.
//
// The writes are chained rather than issued in a burst: firing all of them at
// once overruns the async queue and only the first couple land.
void DICETcatProtocol::WriteStartupMixerCoefficients() {
    if (runtimePolicy_.startupMixerCoefficients == nullptr ||
        runtimePolicy_.startupMixerCoefficientCount == 0) {
        return;
    }
    diceReader_.ReadExtensionSections([this](IOReturn status, ExtensionSections ext) {
        if (status != kIOReturnSuccess) {
            return;
        }
        CacheExtensions(ext);
        const uint32_t mixerBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.mixer);
        if (mixerBase == kDICEExtensionOffset) {
            return;
        }
        WriteStartupMixerCoefficient(mixerBase, 0);
    });
}

void DICETcatProtocol::WriteStartupMixerCoefficient(uint32_t mixerBase, uint32_t pair) {
    const uint32_t count = runtimePolicy_.startupMixerCoefficientCount;
    if (pair >= count) {
        ASFW_LOG(DICE, "Startup mixer: wrote %u coefficients", count);
        return;
    }
    const uint16_t index = runtimePolicy_.startupMixerCoefficients[pair * 2];
    const uint16_t gain = runtimePolicy_.startupMixerCoefficients[pair * 2 + 1];
    if (index < kMaxMixerCells) {
        mixerCells_[index].store(gain, std::memory_order_release);
    }

    (void)io_.WriteQuadBE(
        MakeDICEAddress(mixerBase + 4U + index * 4U), static_cast<uint32_t>(gain),
        [this, mixerBase, pair, index](Async::AsyncStatus st) {
            if (Protocols::Ports::MapAsyncStatusToIOReturn(st) != kIOReturnSuccess) {
                ASFW_LOG(DICE, "Startup mixer coefficient %u failed", index);
            }
            // Continue regardless: one bad cell should not abort the rest.
            WriteStartupMixerCoefficient(mixerBase, pair + 1);
        });
}

// --- Peak meters --------------------------------------------------------------
//
// The TCAT peak section mirrors the router: one quadlet per route, with the
// route in the low 16 bits and its peak level in the high 16. Reading it is
// how the original panel drove its meters.

uint32_t DICETcatProtocol::GetPeakCount() const {
    return peakCount_.load(std::memory_order_acquire);
}

uint32_t DICETcatProtocol::GetPeakEntry(uint32_t index) const {
    if (index >= kMaxPeakEntries) {
        return 0;
    }
    return peakEntries_[index].load(std::memory_order_acquire);
}

IOReturn DICETcatProtocol::RefreshPeaks() {
    if (!initialized_) {
        return kIOReturnNotReady;
    }
    diceReader_.ReadExtensionSections([this](IOReturn status, ExtensionSections ext) {
        if (status != kIOReturnSuccess) {
            return;
        }
        CacheExtensions(ext);
        const uint32_t peakBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.peak);
        if (peakBase == kDICEExtensionOffset) {
            return;
        }
        const uint32_t want = ext.peak.size < kMaxPeakEntries ? ext.peak.size : kMaxPeakEntries;
        (void)io_.ReadBlock(
            MakeDICEAddress(peakBase), want * 4U,
            [this, want](Async::AsyncStatus st, std::span<const uint8_t> payload) {
                if (Protocols::Ports::MapAsyncStatusToIOReturn(st) != kIOReturnSuccess ||
                    payload.size() < want * 4U) {
                    return;
                }
                uint32_t live = 0;
                for (uint32_t i = 0; i < want; ++i) {
                    const uint32_t v = (static_cast<uint32_t>(payload[i * 4U]) << 24) |
                                       (static_cast<uint32_t>(payload[i * 4U + 1]) << 16) |
                                       (static_cast<uint32_t>(payload[i * 4U + 2]) << 8) |
                                       static_cast<uint32_t>(payload[i * 4U + 3]);
                    peakEntries_[i].store(v, std::memory_order_release);
                    if ((v & 0xFFFFU) != 0) {
                        live = i + 1;
                    }
                }
                peakCount_.store(live, std::memory_order_release);
            });
    });
    return kIOReturnSuccess;
}

void DICETcatProtocol::WriteFrontPanelLed(uint8_t select) {
    if (!runtimePolicy_.driveFrontPanelLed) {
        return;
    }
    diceReader_.ReadExtensionSections(
        [this, select](IOReturn status, ExtensionSections ext) {
            if (status != kIOReturnSuccess) {
                return;
            }
            CacheExtensions(ext);
            const uint32_t appBase = ASFW::Audio::DICE::ExtensionAbsoluteOffset(ext.application);
            if (appBase == kDICEExtensionOffset) {
                return;
            }
            // Application section +0x14 is the Mbox Pro's UILEDState. Its
            // control byte is the low byte of the quadlet; bits 6-5 carry the
            // monitor selection. Bit 0 is part of the value the device ships
            // with and is preserved. Verified on hardware: 0 leaves the light
            // off, 1 lights it green.
            const uint32_t value = 0x00000100U | 0x01U |
                                   (static_cast<uint32_t>(select & 0x3U) << 5);
            (void)io_.WriteQuadBE(
                MakeDICEAddress(appBase + kMboxProUILedStateOffset), value,
                [value](Async::AsyncStatus st) {
                    if (Protocols::Ports::MapAsyncStatusToIOReturn(st) != kIOReturnSuccess) {
                        ASFW_LOG(DICE, "Front-panel LED write failed (value=0x%08x)", value);
                    }
                });
        });
}

void DICETcatProtocol::PrepareDuplex(const AudioDuplexChannels& channels,
                                     const AudioClockConfig& desiredClock,
                                     PrepareCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    DiceClockConfiguration diceClock{};
    if (!MakeDiceClockConfiguration(desiredClock, diceClock)) {
        callback(kIOReturnUnsupported, {});
        return;
    }

    // Remember the live clock so a later per-StartIO PrepareDuplex48k targets it
    // rather than reverting the device to 48 kHz (see selectedClock_).
    if (desiredClock.sampleRateHz != 0) {
        selectedClock_ = desiredClock;
    }

    ApplyStartupRouter([this, channels, diceClock, callback = std::move(callback)](
                           IOReturn routerStatus) mutable {
        // A router program is device configuration, not a bring-up gate: if it
        // fails the streams can still come up (silently, on devices that need
        // it), so log and continue rather than failing the whole start.
        if (routerStatus != kIOReturnSuccess) {
            ASFW_LOG(DICE, "PrepareDuplex: startup router not applied (0x%08x); continuing",
                     routerStatus);
        }
        duplexCtrl_->PrepareDuplex(
            channels,
            diceClock,
            [this, callback = std::move(callback)](IOReturn status, DiceDuplexPrepareResult result) mutable {
                if (status == kIOReturnSuccess) {
                    CacheRuntimeCaps(result.runtimeCaps);
                }
                callback(status, result);
            });
    });
}

void DICETcatProtocol::ProgramRx(StageCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    duplexCtrl_->ProgramRx(std::move(callback));
}

void DICETcatProtocol::ProgramTxAndEnableDuplex(StageCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    duplexCtrl_->ProgramTxAndEnableDuplex(std::move(callback));
}

void DICETcatProtocol::ConfirmDuplexStart(ConfirmCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    duplexCtrl_->ConfirmDuplexStart(
        [this, callback = std::move(callback)](IOReturn status, DiceDuplexConfirmResult result) mutable {
            if (status == kIOReturnSuccess) {
                CacheRuntimeCaps(result.runtimeCaps);
            }
            callback(status, result);
        });
}

void DICETcatProtocol::ApplyClockConfig(const AudioClockConfig& desiredClock,
                                        ClockApplyCallback callback) {
    if (!initialized_ || !duplexCtrl_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    DiceClockConfiguration diceClock{};
    if (!MakeDiceClockConfiguration(desiredClock, diceClock)) {
        callback(kIOReturnUnsupported, {});
        return;
    }

    // An idle sample-rate change lands here (RunIdleClockApply). Remember it so
    // the next StartIO's PrepareDuplex48k keeps the device at this rate instead
    // of rewriting CLOCK_SELECT back to 48 kHz (see selectedClock_).
    if (desiredClock.sampleRateHz != 0) {
        selectedClock_ = desiredClock;
    }

    duplexCtrl_->ApplyClockConfig(
        diceClock,
        [this, callback = std::move(callback)](IOReturn status, DiceClockApplyResult result) mutable {
            if (status == kIOReturnSuccess) {
                CacheRuntimeCaps(result.runtimeCaps);
            }
            callback(status, result);
        });
}

void DICETcatProtocol::ReadDuplexHealth(HealthCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    EnsureSectionsLoaded([this, callback = std::move(callback)](IOReturn sectionStatus) mutable {
        if (sectionStatus != kIOReturnSuccess) {
            callback(sectionStatus, {});
            return;
        }

        diceReader_.ReadGlobalState(
            sections_,
            [this, callback = std::move(callback)](IOReturn status, GlobalState global) mutable {
                if (status != kIOReturnSuccess) {
                    callback(status, {});
                    return;
                }

                AudioStreamRuntimeCaps caps{};
                (void)GetRuntimeAudioStreamCaps(caps);
                const uint32_t clockSource =
                    global.clockSelect & ClockSelect::kSourceMask;
                const bool clockReferenceHealthy =
                    clockSource != static_cast<uint32_t>(ClockSource::ARX1) ||
                    (IsArx1Locked(global.extStatus) && !HasArx1Slip(global.extStatus));

                callback(status,
                         DiceDuplexHealthResult{
                             .generation = busInfo_.GetGeneration(),
                             .appliedClock =
                                 AudioClockConfig{
                                     .sampleRateHz = global.sampleRate,
                                 },
                             .runtimeCaps = caps,
                             .sourceLocked = IsSourceLocked(global.status),
                             .clockReferenceHealthy = clockReferenceHealthy,
                             .nominalRateHz = NominalRateHz(global.status),
                             .notification = global.notification,
                             .status = global.status,
                             .extStatus = global.extStatus,
                         });
            });
    });
}

void DICETcatProtocol::PrepareDuplex48k(const AudioDuplexChannels& channels, VoidCallback callback) {
    // This per-StartIO bring-up must honor the user's selected clock. Using a
    // hardcoded 48 kHz here rewrites CLOCK_SELECT on every StartIO and fights an
    // idle 44.1 kHz change (the device PLL flaps 44.1k<->48k and audio starves).
    // Fall back to 48 kHz only before any rate has been selected.
    AudioClockConfig clock = selectedClock_;
    if (clock.sampleRateHz == 0) {
        clock = AudioClockConfig{
            .sampleRateHz = 48000U,
        };
    }
    PrepareDuplex(channels,
                  clock,
                  [callback = std::move(callback)](IOReturn status, DiceDuplexPrepareResult) mutable {
                      callback(status);
                  });
}

void DICETcatProtocol::ProgramRxForDuplex48k(VoidCallback callback) {
    ProgramRx([callback = std::move(callback)](IOReturn status, DiceDuplexStageResult) mutable {
        callback(status);
    });
}

void DICETcatProtocol::ProgramTxAndEnableDuplex48k(VoidCallback callback) {
    ProgramTxAndEnableDuplex([callback = std::move(callback)](IOReturn status, DiceDuplexStageResult) mutable {
        callback(status);
    });
}

void DICETcatProtocol::ConfirmDuplex48kStart(VoidCallback callback) {
    ConfirmDuplexStart([callback = std::move(callback)](IOReturn status, DiceDuplexConfirmResult) mutable {
        callback(status);
    });
}

IOReturn DICETcatProtocol::StopDuplex() {
    if (!duplexCtrl_) {
        return kIOReturnSuccess;
    }
    return duplexCtrl_->StopDuplex();
}

void DICETcatProtocol::UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                                            Protocols::AVC::FCPTransport* transport) {
    (void)transport;
    io_.UpdateRoute(route);
}

void DICETcatProtocol::EnsureSectionsLoaded(VoidCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady);
        return;
    }

    if (sectionsLoaded_) {
        callback(kIOReturnSuccess);
        return;
    }

    diceReader_.ReadGeneralSections([this, callback = std::move(callback)](IOReturn status, GeneralSections sections) mutable {
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "DICETcatProtocol: failed to read general sections: 0x%x", status);
            callback(status);
            return;
        }

        sections_ = sections;
        sectionsLoaded_ = true;
        ASFW_LOG(DICE,
                 "DICETcatProtocol: loaded sections global=%u/%u tx=%u/%u rx=%u/%u ext=%u/%u",
                 sections_.global.offset,
                 sections_.global.size,
                 sections_.txStreamFormat.offset,
                 sections_.txStreamFormat.size,
                 sections_.rxStreamFormat.offset,
                 sections_.rxStreamFormat.size,
                 sections_.extSync.offset,
                 sections_.extSync.size);
        callback(kIOReturnSuccess);
    });
}

void DICETcatProtocol::EnsureRuntimeCapsLoaded(VoidCallback callback) {
    if (!initialized_) {
        callback(kIOReturnNotReady);
        return;
    }

    if (runtimeCapsValid_.load(std::memory_order_acquire)) {
        callback(kIOReturnSuccess);
        return;
    }

    EnsureSectionsLoaded([this, callback = std::move(callback)](IOReturn sectionStatus) mutable {
        if (sectionStatus != kIOReturnSuccess) {
            callback(sectionStatus);
            return;
        }

        struct RuntimeCapsState {
            GlobalState global;
            StreamConfig tx;
            StreamConfig rx;
        };

        auto state = std::make_shared<RuntimeCapsState>();
        diceReader_.ReadGlobalState(
            sections_,
            [this, state, callback = std::move(callback)](IOReturn globalStatus, GlobalState global) mutable {
                if (globalStatus != kIOReturnSuccess) {
                    ASFW_LOG(DICE, "DICETcatProtocol: failed to read global state: 0x%x", globalStatus);
                    callback(globalStatus);
                    return;
                }

                state->global = global;
                ASFW_LOG(DICE,
                         "DICETcatProtocol: global state rate=%u clockSelect=0x%08x status=0x%08x extStatus=0x%08x notification=0x%08x",
                         global.sampleRate,
                         global.clockSelect,
                         global.status,
                         global.extStatus,
                         global.notification);
                diceReader_.ReadTxStreamConfig(
                    sections_,
                    [this, state, callback = std::move(callback)](IOReturn txStatus, StreamConfig tx) mutable {
                        if (txStatus != kIOReturnSuccess) {
                            ASFW_LOG(DICE, "DICETcatProtocol: failed to read TX stream config: 0x%x", txStatus);
                            callback(txStatus);
                            return;
                        }

                        state->tx = tx;
                        LogStreamConfigSummary("TX", state->tx);
                        diceReader_.ReadRxStreamConfig(
                            sections_,
                            [this, state, callback = std::move(callback)](IOReturn rxStatus, StreamConfig rx) mutable {
                                if (rxStatus != kIOReturnSuccess) {
                                    ASFW_LOG(DICE, "DICETcatProtocol: failed to read RX stream config: 0x%x", rxStatus);
                                    callback(rxStatus);
                                    return;
                                }

                                state->rx = rx;
                                LogStreamConfigSummary("RX", state->rx);
                                CacheRuntimeCaps(state->global, state->tx, state->rx);
                                AudioStreamRuntimeCaps caps{};
                                (void)GetRuntimeAudioStreamCaps(caps);
                                LogRuntimeCaps("standard-dice", caps);
                                if (!HasUsableRuntimeCaps(caps)) {
                                    ASFW_LOG(DICE,
                                             "DICETcatProtocol: standard DICE discovery produced zero or partial caps; audio publication should fail closed");
                                }
                                callback(kIOReturnSuccess);
                            });
                    });
            });
    });
}

void DICETcatProtocol::CacheRuntimeCaps(const GlobalState& global,
                                        const StreamConfig& tx,
                                        const StreamConfig& rx) noexcept {
    AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = tx.TotalPcmChannels(),
        .hostOutputPcmChannels = rx.TotalPcmChannels(),
        .deviceToHostAm824Slots = tx.TotalAm824Slots(),
        .hostToDeviceAm824Slots = rx.TotalAm824Slots(),
        .sampleRateHz = global.sampleRate,
        .deviceToHostIsoChannel = tx.FirstActiveIsoChannel(AudioStreamRuntimeCaps::kInvalidIsoChannel),
        .hostToDeviceIsoChannel = rx.FirstActiveIsoChannel(AudioStreamRuntimeCaps::kInvalidIsoChannel),
    };

    // Per-stream wire geometry from the DICE TX_NUMBER/RX_NUMBER headers. Stream
    // count includes streams the device reports with iso=-1 (disabled) that the
    // host must still arm for a multi-stream device such as the Venice F32
    // (2×16). Mirrors DICEDuplexBringupController's per-stream fill.
    auto fillPerStream = [](const StreamConfig& sc,
                            uint32_t& outCount,
                            AudioStreamWireInfo* outStreams) noexcept {
        const uint32_t count = (sc.numStreams < kMaxAudioStreamsPerDirection)
                                   ? sc.numStreams
                                   : kMaxAudioStreamsPerDirection;
        outCount = count;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& entry = sc.streams[i];
            outStreams[i].isoChannel =
                (entry.isoChannel >= 0 && entry.isoChannel <= 0x3F)
                    ? static_cast<uint8_t>(entry.isoChannel)
                    : AudioStreamWireInfo::kInvalidIsoChannel;
            outStreams[i].pcmChannels = static_cast<uint16_t>(entry.pcmChannels);
            outStreams[i].am824Slots = static_cast<uint16_t>(entry.Am824Slots());
            outStreams[i].midiPorts = static_cast<uint16_t>(entry.midiPorts);
        }
    };
    fillPerStream(tx, caps.deviceToHostStreamCount, caps.deviceToHostStreams);
    fillPerStream(rx, caps.hostToDeviceStreamCount, caps.hostToDeviceStreams);

    // Per-channel device labels from the DICE TX/RX name sections, flattened
    // across streams in channel order. Written BEFORE CacheRuntimeCaps(caps)'s
    // release-store so GetChannelLabels readers see a consistent snapshot.
    // Host input == device TX, host output == device RX (AudioTypes.hpp).
    auto fillLabels = [](const StreamConfig& sc,
                         std::atomic<uint32_t>& outCount,
                         char (&outLabels)[kMaxChannelLabels][64]) noexcept {
        uint32_t idx = 0;
        const uint32_t streams = (sc.numStreams < kMaxAudioStreamsPerDirection)
                                     ? sc.numStreams
                                     : kMaxAudioStreamsPerDirection;
        for (uint32_t s = 0; s < streams && idx < kMaxChannelLabels; ++s) {
            for (const auto& name : SplitDiceLabels(sc.streams[s].labels)) {
                if (idx >= kMaxChannelLabels) {
                    break;
                }
                strlcpy(outLabels[idx], name.c_str(), sizeof(outLabels[idx]));
                ++idx;
            }
        }
        for (uint32_t z = idx; z < kMaxChannelLabels; ++z) {
            outLabels[z][0] = '\0';
        }
        outCount.store(idx, std::memory_order_relaxed);
    };
    fillLabels(tx, inputChannelLabelCount_, inputChannelLabels_);
    fillLabels(rx, outputChannelLabelCount_, outputChannelLabels_);

    CacheRuntimeCaps(caps);
}

bool DICETcatProtocol::GetChannelLabels(std::vector<std::string>& inNames,
                                        std::vector<std::string>& outNames) const {
    if (!runtimeCapsValid_.load(std::memory_order_acquire)) {
        return false;
    }
    const uint32_t inCount = runtimePolicy_.exposeDeviceToHostToCoreAudio
                                 ? inputChannelLabelCount_.load(std::memory_order_relaxed)
                                 : 0;
    const uint32_t outCount = outputChannelLabelCount_.load(std::memory_order_relaxed);
    inNames.clear();
    outNames.clear();
    for (uint32_t i = 0; i < inCount && i < kMaxChannelLabels; ++i) {
        inNames.emplace_back(inputChannelLabels_[i]);
    }
    for (uint32_t i = 0; i < outCount && i < kMaxChannelLabels; ++i) {
        outNames.emplace_back(outputChannelLabels_[i]);
    }
    return inCount > 0 || outCount > 0;
}

void DICETcatProtocol::CacheRuntimeCaps(const AudioStreamRuntimeCaps& caps) noexcept {
    const uint32_t exposedInputChannels = runtimePolicy_.exposeDeviceToHostToCoreAudio
                                              ? caps.hostInputPcmChannels
                                              : 0;
    hostInputPcmChannels_.store(exposedInputChannels, std::memory_order_relaxed);
    deviceToHostAm824Slots_.store(caps.deviceToHostAm824Slots, std::memory_order_relaxed);
    hostOutputPcmChannels_.store(caps.hostOutputPcmChannels, std::memory_order_relaxed);
    hostToDeviceAm824Slots_.store(caps.hostToDeviceAm824Slots, std::memory_order_relaxed);
    runtimeSampleRateHz_.store(caps.sampleRateHz, std::memory_order_relaxed);
    deviceToHostIsoChannel_.store(caps.deviceToHostIsoChannel, std::memory_order_relaxed);
    hostToDeviceIsoChannel_.store(caps.hostToDeviceIsoChannel, std::memory_order_relaxed);

    // Per-stream geometry: write the plain arrays + counts BEFORE the
    // release-store of runtimeCapsValid_ so readers that pass the acquire-load
    // observe a consistent snapshot.
    deviceToHostStreamCount_.store(caps.deviceToHostStreamCount, std::memory_order_relaxed);
    hostToDeviceStreamCount_.store(caps.hostToDeviceStreamCount, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxAudioStreamsPerDirection; ++i) {
        deviceToHostStreams_[i] = caps.deviceToHostStreams[i];
        hostToDeviceStreams_[i] = caps.hostToDeviceStreams[i];
    }

    runtimeCapsValid_.store(true, std::memory_order_release);
    LogRuntimeCaps("cache", caps);
}

void DICETcatProtocol::ResetRuntimeCaps() noexcept {
    runtimeCapsValid_.store(false, std::memory_order_release);
    runtimeSampleRateHz_.store(0, std::memory_order_relaxed);
    hostInputPcmChannels_.store(0, std::memory_order_relaxed);
    hostOutputPcmChannels_.store(0, std::memory_order_relaxed);
    deviceToHostAm824Slots_.store(0, std::memory_order_relaxed);
    hostToDeviceAm824Slots_.store(0, std::memory_order_relaxed);
    deviceToHostIsoChannel_.store(AudioStreamRuntimeCaps::kInvalidIsoChannel, std::memory_order_relaxed);
    hostToDeviceIsoChannel_.store(AudioStreamRuntimeCaps::kInvalidIsoChannel, std::memory_order_relaxed);
    deviceToHostStreamCount_.store(0, std::memory_order_relaxed);
    hostToDeviceStreamCount_.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxAudioStreamsPerDirection; ++i) {
        deviceToHostStreams_[i] = AudioStreamWireInfo{};
        hostToDeviceStreams_[i] = AudioStreamWireInfo{};
    }
    inputChannelLabelCount_.store(0, std::memory_order_relaxed);
    outputChannelLabelCount_.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxChannelLabels; ++i) {
        inputChannelLabels_[i][0] = '\0';
        outputChannelLabels_[i][0] = '\0';
    }
}

} // namespace ASFW::Audio::DICE::TCAT
