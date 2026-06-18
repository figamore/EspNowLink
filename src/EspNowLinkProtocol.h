// Copyright 2026 Figamore (https://github.com/figamore)
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace espnow_link {

static constexpr uint8_t kPacketDiscovery = 0x01;
static constexpr uint8_t kPacketPairChallenge = 0x02;
static constexpr uint8_t kPacketData = 0x03;
static constexpr uint8_t kPacketPairConfirm = 0x04;
static constexpr uint8_t kPacketRealtime = 0x05;
static constexpr uint8_t kPacketKeepalive = 0x06;
static constexpr uint8_t kPacketPairResult = 0x07;
static constexpr uint8_t kPacketPairComplete = 0x08;

static constexpr uint8_t kPairingProtocolV4 = 4;
static constexpr uint8_t kPairingModePair = 1;
static constexpr size_t kMacSize = 6;
static constexpr size_t kHostnameSize = 32;
static constexpr size_t kSessionIdSize = 16;
static constexpr size_t kLmkSize = 16;
static constexpr size_t kPairTagSize = 16;
static constexpr size_t kAntiReplayTagSize = 8;
static constexpr size_t kEcdhPrivateKeySize = 32;
static constexpr size_t kEcdhPublicKeySize = 65;
static constexpr size_t kEcdhSharedSecretSize = 32;
static constexpr size_t kMaxEspNowPayload = 250;
static constexpr size_t kFragmentHeaderSize = 12;
static constexpr size_t kFragmentPayloadMax = kMaxEspNowPayload - kFragmentHeaderSize;
static constexpr size_t kMaxFragments = 8;
static constexpr size_t kAuthKeepaliveSize = 1 + 4 + kAntiReplayTagSize + 1;
static constexpr uint8_t kKeepaliveSessionConfirmed = 0x01;
static constexpr uint8_t kPairChannel = 1;
static constexpr uint8_t kProbeOrder[13] = {6, 11, 1, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};

struct __attribute__((packed)) DiscoveryV4Packet {
  uint8_t type;
  uint8_t version;
  uint8_t mode;
  uint8_t mac[kMacSize];
  uint8_t channel;
  uint8_t sessionId[kSessionIdSize];
  uint8_t publicKey[kEcdhPublicKeySize];
};

struct __attribute__((packed)) PairChallengeV4Packet {
  uint8_t type;
  uint8_t version;
  uint8_t mode;
  uint8_t mac[kMacSize];
  uint8_t channel;
  uint8_t dialChannel;
  uint8_t sessionId[kSessionIdSize];
  uint8_t publicKey[kEcdhPublicKeySize];
  char hostname[kHostnameSize];
};

struct __attribute__((packed)) PairConfirmV4Packet {
  uint8_t type;
  uint8_t version;
  uint8_t mode;
  uint8_t mac[kMacSize];
  uint8_t sessionId[kSessionIdSize];
  uint8_t authTag[kPairTagSize];
};

struct __attribute__((packed)) PairResultV4Packet {
  uint8_t type;
  uint8_t version;
  uint8_t mode;
  uint8_t mac[kMacSize];
  uint8_t channel;
  uint8_t sessionId[kSessionIdSize];
  char hostname[kHostnameSize];
  uint8_t authTag[kPairTagSize];
};

struct __attribute__((packed)) PairCompleteV4Packet {
  uint8_t type;
  uint8_t version;
  uint8_t mode;
  uint8_t mac[kMacSize];
  uint8_t sessionId[kSessionIdSize];
  uint8_t authTag[kPairTagSize];
};

struct __attribute__((packed)) FragmentHeader {
  uint8_t type;
  uint8_t nonce[4];
  uint8_t counter[4];
  uint8_t sequence;
  uint8_t fragmentIndex;
  uint8_t totalFragments;
};

static_assert(sizeof(DiscoveryV4Packet) == 91, "EspNowLink v4 discovery layout changed");
static_assert(sizeof(PairChallengeV4Packet) == 124, "EspNowLink v4 challenge layout changed");
static_assert(sizeof(PairConfirmV4Packet) == 41, "EspNowLink v4 confirmation layout changed");
static_assert(sizeof(PairResultV4Packet) == 74, "EspNowLink v4 result layout changed");
static_assert(sizeof(PairCompleteV4Packet) == 41, "EspNowLink v4 completion layout changed");
static_assert(sizeof(FragmentHeader) == kFragmentHeaderSize, "EspNowLink fragment layout changed");

}  // namespace espnow_link
