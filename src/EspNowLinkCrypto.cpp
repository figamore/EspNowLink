// Copyright 2026 Figamore (https://github.com/figamore)
// SPDX-License-Identifier: Apache-2.0

#include "EspNowLinkCrypto.h"

#include <esp_random.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <string.h>

namespace {

constexpr uint32_t kReplayRegenMs = 3000;
constexpr uint32_t kReplayUnderflowLimit = 8;
const char* fallbackLabel(const char* label, const char* fallback) {
  return (label && label[0]) ? label : fallback;
}

int ecdhRng(void*, unsigned char* output, size_t len) {
  for (size_t i = 0; i < len; i += sizeof(uint32_t)) {
    uint32_t word = esp_random();
    size_t chunk = (len - i < sizeof(word)) ? (len - i) : sizeof(word);
    memcpy(output + i, &word, chunk);
  }
  return 0;
}

}  // namespace

namespace espnow_link {

void derivePairingWindowLmk(const char* label, uint8_t outLmk[kLmkSize]) {
  const char* effectiveLabel = fallbackLabel(label, kDefaultPairingWindowLabel);
  uint8_t hash[32];
  mbedtls_sha256(reinterpret_cast<const uint8_t*>(effectiveLabel), strlen(effectiveLabel), hash, 0);
  memcpy(outLmk, hash, kLmkSize);
  secureZero(hash, sizeof(hash));
}

void derivePmk(const char* label, uint8_t outPmk[kLmkSize]) {
  const char* effectiveLabel = fallbackLabel(label, kDefaultPmkLabel);
  uint8_t hash[32];
  mbedtls_sha256(reinterpret_cast<const uint8_t*>(effectiveLabel), strlen(effectiveLabel), hash, 0);
  memcpy(outPmk, hash, kLmkSize);
  secureZero(hash, sizeof(hash));
}

void pairingAuthTag(const uint8_t key[kLmkSize], const uint8_t* data, size_t len, uint8_t out[kPairTagSize]) {
  uint8_t full[32];
  mbedtls_md_context_t ctx;
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, key, kLmkSize);
  mbedtls_md_hmac_update(&ctx, data, len);
  mbedtls_md_hmac_finish(&ctx, full);
  mbedtls_md_free(&ctx);

  memcpy(out, full, kPairTagSize);
  secureZero(full, sizeof(full));
}

bool verifyPairingAuthTag(const uint8_t key[kLmkSize], const uint8_t* data, size_t len, const uint8_t tag[kPairTagSize]) {
  uint8_t expected[kPairTagSize];
  pairingAuthTag(key, data, len, expected);
  bool ok = constantTimeEquals(expected, tag, kPairTagSize);
  secureZero(expected, sizeof(expected));
  return ok;
}

bool generateEcdhKeypair(uint8_t privateKey[kEcdhPrivateKeySize], uint8_t publicKey[kEcdhPublicKeySize]) {
  mbedtls_ecp_group group;
  mbedtls_mpi d;
  mbedtls_ecp_point q;
  size_t olen = 0;
  bool ok = false;

  mbedtls_ecp_group_init(&group);
  mbedtls_mpi_init(&d);
  mbedtls_ecp_point_init(&q);

  if (mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
      mbedtls_ecp_gen_keypair(&group, &d, &q, ecdhRng, nullptr) == 0 &&
      mbedtls_mpi_write_binary(&d, privateKey, kEcdhPrivateKeySize) == 0 &&
      mbedtls_ecp_point_write_binary(&group,
                                     &q,
                                     MBEDTLS_ECP_PF_UNCOMPRESSED,
                                     &olen,
                                     publicKey,
                                     kEcdhPublicKeySize) == 0 &&
      olen == kEcdhPublicKeySize) {
    ok = true;
  }

  mbedtls_ecp_point_free(&q);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&group);
  if (!ok) {
    secureZero(privateKey, kEcdhPrivateKeySize);
    secureZero(publicKey, kEcdhPublicKeySize);
  }
  return ok;
}

bool deriveEcdhSharedSecret(const uint8_t privateKey[kEcdhPrivateKeySize],
                            const uint8_t peerPublicKey[kEcdhPublicKeySize],
                            uint8_t sharedSecret[kEcdhSharedSecretSize]) {
  mbedtls_ecp_group group;
  mbedtls_mpi d;
  mbedtls_mpi z;
  mbedtls_ecp_point q;
  bool ok = false;

  mbedtls_ecp_group_init(&group);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&z);
  mbedtls_ecp_point_init(&q);

  if (mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
      mbedtls_mpi_read_binary(&d, privateKey, kEcdhPrivateKeySize) == 0 &&
      mbedtls_ecp_point_read_binary(&group, &q, peerPublicKey, kEcdhPublicKeySize) == 0 &&
      mbedtls_ecp_check_pubkey(&group, &q) == 0 &&
      mbedtls_ecdh_compute_shared(&group, &z, &q, &d, ecdhRng, nullptr) == 0 &&
      mbedtls_mpi_write_binary(&z, sharedSecret, kEcdhSharedSecretSize) == 0) {
    ok = true;
  }

  mbedtls_ecp_point_free(&q);
  mbedtls_mpi_free(&z);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&group);
  if (!ok) {
    secureZero(sharedSecret, kEcdhSharedSecretSize);
  }
  return ok;
}

void derivePairingSessionLmk(const char* label,
                             const uint8_t pairingWindowLmk[kLmkSize],
                             const uint8_t sharedSecret[kEcdhSharedSecretSize],
                             const uint8_t* discovery,
                             size_t discoveryLen,
                             const uint8_t* challenge,
                             size_t challengeLen,
                             uint8_t outLmk[kLmkSize]) {
  uint8_t full[32];
  mbedtls_md_context_t ctx;
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1);
  const char* effectiveLabel = fallbackLabel(label, kDefaultPairingSessionLabel);
  mbedtls_md_hmac_starts(&ctx, pairingWindowLmk, kLmkSize);
  mbedtls_md_hmac_update(&ctx, reinterpret_cast<const uint8_t*>(effectiveLabel), strlen(effectiveLabel));
  mbedtls_md_hmac_update(&ctx, sharedSecret, kEcdhSharedSecretSize);
  mbedtls_md_hmac_update(&ctx, discovery, discoveryLen);
  mbedtls_md_hmac_update(&ctx, challenge, challengeLen);
  mbedtls_md_hmac_finish(&ctx, full);
  mbedtls_md_free(&ctx);

  memcpy(outLmk, full, kLmkSize);
  secureZero(full, sizeof(full));
}

bool constantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len) {
  if (!a || !b) {
    return false;
  }
  uint8_t diff = 0;
  for (size_t i = 0; i < len; ++i) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0;
}

void secureZero(void* data, size_t len) {
  if (!data) {
    return;
  }
  volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(data);
  while (len--) {
    *p++ = 0;
  }
}

uint32_t randomNonce() {
  uint32_t nonce;
  do {
    nonce = esp_random();
  } while (nonce == 0);
  return nonce;
}

void issueRxChallenge(std::atomic<uint32_t>& rxNonce) {
  rxNonce.store(randomNonce(), std::memory_order_release);
}

bool stampAntiReplayTag(std::atomic<bool>& txPeerKnown,
                        std::atomic<uint32_t>& txPeerNonce,
                        std::atomic<uint32_t>& txCounter,
                        uint8_t tag[kAntiReplayTagSize]) {
  if (!txPeerKnown.load(std::memory_order_acquire)) {
    return false;
  }
  uint32_t nonce = txPeerNonce.load(std::memory_order_acquire);
  uint32_t counter = txCounter.fetch_add(1, std::memory_order_relaxed) + 1;
  memcpy(tag, &nonce, 4);
  memcpy(tag + 4, &counter, 4);
  return true;
}

bool acceptReplay(std::atomic<uint32_t>& rxNonce,
                  ReplayState& replay,
                  uint32_t nonce,
                  uint32_t counter,
                  uint32_t nowMs) {
  uint32_t current = rxNonce.load(std::memory_order_acquire);
  if (current != replay.windowNonce) {
    replay.windowNonce = current;
    replay.windowTop = 0;
    replay.windowBitmap = 0;
    replay.underflow = 0;
  }
  if (nonce != current || counter == 0) {
    return false;
  }

  if (counter > replay.windowTop) {
    uint32_t shift = counter - replay.windowTop;
    replay.windowBitmap = (shift >= 64) ? 0ULL : (replay.windowBitmap << shift);
    replay.windowBitmap |= 1ULL;
    replay.windowTop = counter;
    replay.underflow = 0;
    return true;
  }

  uint32_t diff = replay.windowTop - counter;
  if (diff < 64) {
    uint64_t mask = 1ULL << diff;
    if (replay.windowBitmap & mask) {
      return false;
    }
    replay.windowBitmap |= mask;
    return true;
  }

  if (++replay.underflow >= kReplayUnderflowLimit) {
    if ((uint32_t)(nowMs - replay.regenMs) > kReplayRegenMs) {
      replay.regenMs = nowMs;
      issueRxChallenge(rxNonce);
    }
    replay.underflow = 0;
  }
  return false;
}

}  // namespace espnow_link
