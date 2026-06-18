// Copyright 2026 Figamore (https://github.com/figamore)
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <Arduino.h>
#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "EspNowLinkProtocol.h"

namespace espnow_link {

struct ReplayState {
  uint32_t windowNonce = 0;
  uint32_t windowTop = 0;
  uint64_t windowBitmap = 0;
  uint32_t underflow = 0;
  uint32_t regenMs = 0;
};

constexpr const char* kDefaultPairingWindowLabel = "espnowlink-pairing-v1";
constexpr const char* kDefaultPairingSessionLabel = "espnowlink-session-v1";
constexpr const char* kDefaultPmkLabel = "espnowlink-pmk-v1";

void derivePairingWindowLmk(const char* label, uint8_t outLmk[kLmkSize]);
void derivePmk(const char* label, uint8_t outPmk[kLmkSize]);
void pairingAuthTag(const uint8_t key[kLmkSize], const uint8_t* data, size_t len, uint8_t out[kPairTagSize]);
bool verifyPairingAuthTag(const uint8_t key[kLmkSize], const uint8_t* data, size_t len, const uint8_t tag[kPairTagSize]);
bool generateEcdhKeypair(uint8_t privateKey[kEcdhPrivateKeySize], uint8_t publicKey[kEcdhPublicKeySize]);
bool deriveEcdhSharedSecret(const uint8_t privateKey[kEcdhPrivateKeySize],
                            const uint8_t peerPublicKey[kEcdhPublicKeySize],
                            uint8_t sharedSecret[kEcdhSharedSecretSize]);
void derivePairingSessionLmk(const char* label,
                             const uint8_t pairingWindowLmk[kLmkSize],
                             const uint8_t sharedSecret[kEcdhSharedSecretSize],
                             const uint8_t* discovery,
                             size_t discoveryLen,
                             const uint8_t* challenge,
                             size_t challengeLen,
                             uint8_t outLmk[kLmkSize]);
bool constantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len);
void secureZero(void* data, size_t len);
uint32_t randomNonce();
void issueRxChallenge(std::atomic<uint32_t>& rxNonce);
bool stampAntiReplayTag(std::atomic<bool>& txPeerKnown,
                        std::atomic<uint32_t>& txPeerNonce,
                        std::atomic<uint32_t>& txCounter,
                        uint8_t tag[kAntiReplayTagSize]);
bool acceptReplay(std::atomic<uint32_t>& rxNonce,
                  ReplayState& replay,
                  uint32_t nonce,
                  uint32_t counter,
                  uint32_t nowMs);

}  // namespace espnow_link
