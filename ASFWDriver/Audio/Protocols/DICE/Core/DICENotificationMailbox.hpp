// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DICENotificationMailbox.hpp - Shared mailbox for DICE async notifications

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ASFW::Audio::DICE::NotificationMailbox {

/// Reference saffire.kext uses the local FW notification address for DICE notify quadlets.
constexpr uint64_t kHandlerOffset = 0x000100000000ULL;
/// Legacy ASFW software-latch address kept for diagnostics/backward compatibility.
constexpr uint64_t kLegacyHandlerOffset = 0x00FF0000D1CCULL;

inline std::atomic<uint32_t> gLatchedBits{0};
using ObserverFn = void(*)(void* context, uint32_t bits);

// Several subsystems care about the same notification quadlet: the audio
// backend watches clock events while a device protocol may also need them (the
// Avid Mbox Pro reports its front-panel button here). A single slot let
// whichever registered last silently displace the other, so keep a small fixed
// set instead.
inline constexpr size_t kMaxObservers = 4;
struct ObserverSlot {
    std::atomic<void*> context{nullptr};
    std::atomic<ObserverFn> fn{nullptr};
};
inline ObserverSlot gObservers[kMaxObservers];

/// Reset any latched notification bits.
inline void Reset() noexcept {
    gLatchedBits.store(0, std::memory_order_release);
}

/// Latch notification bits observed from device writes.
inline void Publish(uint32_t bits) noexcept {
    gLatchedBits.fetch_or(bits, std::memory_order_acq_rel);
    for (auto& slot : gObservers) {
        void* ctx = slot.context.load(std::memory_order_acquire);
        ObserverFn fn = slot.fn.load(std::memory_order_acquire);
        if (ctx != nullptr && fn != nullptr) {
            fn(ctx, bits);
        }
    }
}

inline void SetObserver(void* context, ObserverFn observer) noexcept {
    if (context == nullptr || observer == nullptr) {
        return;
    }
    // Re-registering the same context updates it in place rather than
    // consuming a second slot.
    for (auto& slot : gObservers) {
        if (slot.context.load(std::memory_order_acquire) == context) {
            slot.fn.store(observer, std::memory_order_release);
            return;
        }
    }
    for (auto& slot : gObservers) {
        void* expected = nullptr;
        if (slot.context.compare_exchange_strong(expected, context,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            slot.fn.store(observer, std::memory_order_release);
            return;
        }
    }
}

inline void ClearObserver(void* context) noexcept {
    for (auto& slot : gObservers) {
        if (slot.context.load(std::memory_order_acquire) == context) {
            slot.fn.store(nullptr, std::memory_order_release);
            slot.context.store(nullptr, std::memory_order_release);
            return;
        }
    }
}

[[nodiscard]] inline bool MatchesDestOffset(uint64_t destOffset) noexcept {
    return destOffset == kHandlerOffset || destOffset == kLegacyHandlerOffset;
}

[[nodiscard]] inline uint32_t DecodeWireQuadlet(const uint8_t* data) noexcept {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

[[nodiscard]] inline uint32_t PublishWireQuadlet(const uint8_t* data) noexcept {
    const uint32_t bits = DecodeWireQuadlet(data);
    Publish(bits);
    return bits;
}

/// Snapshot currently latched bits without clearing them.
[[nodiscard]] inline uint32_t Snapshot() noexcept {
    return gLatchedBits.load(std::memory_order_acquire);
}

/// Consume and clear all currently latched bits.
[[nodiscard]] inline uint32_t Consume() noexcept {
    return gLatchedBits.exchange(0, std::memory_order_acq_rel);
}

} // namespace ASFW::Audio::DICE::NotificationMailbox
