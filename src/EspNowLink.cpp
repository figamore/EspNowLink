// Copyright 2026 Figamore (https://github.com/figamore)
// SPDX-License-Identifier: Apache-2.0

#include "EspNowLink.h"

#include <new>
#include <stddef.h>
#include <string.h>

using namespace espnow_link;

namespace {

constexpr uint32_t kBeaconIntervalMs = 250;
constexpr uint32_t kPairResultTimeoutMs = 5000;
constexpr uint32_t kPairConfirmRetryMs = 300;
constexpr uint32_t kPairCompleteRetryMs = 100;
constexpr uint8_t kPairCompleteAttempts = 3;
constexpr uint32_t kConnectTimeoutMs = 5000;
constexpr uint32_t kKeepaliveIntervalMs = 2000;
constexpr uint32_t kReconnectProbeMs = 1000;
constexpr uint32_t kSynchronizeRetryMs = 300;
constexpr uint32_t kSynchronizeTimeoutMs = 3000;
constexpr uint32_t kFragmentReassemblyTimeoutMs = 3000;
constexpr uint32_t kServerPairingResultRetryMs = 300;
constexpr uint32_t kServerPairingHandshakeTimeoutMs = 10000;
constexpr uint32_t kServerPendantIdleTimeoutMs = 10000;

const uint8_t kBroadcastMac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
const uint8_t kZeroSession[kSessionIdSize] = {};

bool macEqual(const uint8_t a[6], const uint8_t b[6]) {
  return a && b && memcmp(a, b, 6) == 0;
}

bool macNonzero(const uint8_t mac[6]) {
  if (!mac) {
    return false;
  }
  for (size_t i = 0; i < 6; ++i) {
    if (mac[i] != 0) {
      return true;
    }
  }
  return false;
}

bool lmkNonzero(const uint8_t lmk[16]) {
  if (!lmk) {
    return false;
  }
  for (size_t i = 0; i < 16; ++i) {
    if (lmk[i] != 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

EspNowLink* EspNowLink::instance_ = nullptr;

EspNowLink::EspNowLink() {
  memset(&profiles_, 0, sizeof(profiles_));
  profiles_.version = kProfileStoreVersion;
}

EspNowLink::~EspNowLink() {
  end();
  delete[] serverPeers_;
  serverPeers_ = nullptr;
}

bool EspNowLink::begin(const EspNowLinkConfig& config) {
  if (started_) {
    end();
  }

  config_ = config;
  if (!config_.hostname) {
    config_.hostname = "esp32";
  }
  if (!config_.nvsNamespace) {
    config_.nvsNamespace = "espnowlink";
  }
  if (!config_.labels.pairingWindow || !config_.labels.pairingWindow[0]) {
    config_.labels.pairingWindow = kDefaultPairingWindowLabel;
  }
  if (!config_.labels.pairingSession || !config_.labels.pairingSession[0]) {
    config_.labels.pairingSession = kDefaultPairingSessionLabel;
  }
  if (!config_.labels.pmk || !config_.labels.pmk[0]) {
    config_.labels.pmk = kDefaultPmkLabel;
  }

  if (config_.setWifiMode) {
    if (config_.role == EspNowLinkRole::Client) {
      WiFi.persistent(false);
      WiFi.mode(WIFI_STA);
      if (config_.disconnectStaOnClientBegin) {
        WiFi.disconnect(false, false);
        delay(100);
      }
    } else if (config_.interface == WIFI_IF_AP) {
      WiFi.mode(WIFI_AP);
    } else {
      WiFi.mode(WIFI_STA);
    }
  }
  if (config_.disableWifiSleep) {
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
  }

  if (config_.role == EspNowLinkRole::Server && !ensureServerPeers()) {
    return false;
  }

  packetQueue_ = xQueueCreate(kPacketQueueSize, sizeof(RxPacket));
  if (!packetQueue_) {
    return false;
  }

  if (esp_now_init() != ESP_OK) {
    vQueueDelete(packetQueue_);
    packetQueue_ = nullptr;
    return false;
  }

  uint8_t pmk[kLmkSize];
  derivePmk(config_.labels.pmk, pmk);
  esp_err_t pmkResult = esp_now_set_pmk(pmk);
  secureZero(pmk, sizeof(pmk));
  if (pmkResult != ESP_OK) {
    esp_now_deinit();
    vQueueDelete(packetQueue_);
    packetQueue_ = nullptr;
    return false;
  }

  instance_ = this;
  if (esp_now_register_recv_cb(onRecvStatic) != ESP_OK ||
      esp_now_register_send_cb(onSentStatic) != ESP_OK) {
    esp_now_deinit();
    vQueueDelete(packetQueue_);
    packetQueue_ = nullptr;
    instance_ = nullptr;
    return false;
  }

  resetLinkSession();
  loadProfiles();
  loadActiveIntoRuntime();
  if (config_.role == EspNowLinkRole::Server) {
    loadServerPeersIntoRuntime();
  }
  started_ = true;

  if (config_.role == EspNowLinkRole::Server) {
    for (uint8_t i = 0; i < profiles_.count; ++i) {
      registerPeer(profiles_.profiles[i].mac, 0, true, profiles_.profiles[i].lmk);
    }
  } else if (hasActiveProfile()) {
    registerPeer(peerMac_, preferredChannel_, true, lmk_);
    beginReconnecting();
  }

  if (config_.role == EspNowLinkRole::Server && profiles_.count > 0) {
    setState(EspNowLinkState::Synchronizing);
  } else {
    if (!hasActiveProfile()) {
      setState(EspNowLinkState::Unpaired);
    }
  }

  emit(EspNowLinkEvent::Started);
  return true;
}

void EspNowLink::end() {
  if (!started_) {
    return;
  }
  esp_now_unregister_recv_cb();
  esp_now_unregister_send_cb();
  esp_now_deinit();
  if (packetQueue_) {
    vQueueDelete(packetQueue_);
    packetQueue_ = nullptr;
  }
  delete[] serverPeers_;
  serverPeers_ = nullptr;
  if (instance_ == this) {
    instance_ = nullptr;
  }
  started_ = false;
  setState(EspNowLinkState::Unpaired);
}

void EspNowLink::poll() {
  if (!started_) {
    return;
  }

  RxPacket packet;
  uint8_t processed = 0;
  while (processed < 16 && popPacket(packet)) {
    processPacket(packet);
    ++processed;
  }

  uint32_t now = millis();
  if (config_.role == EspNowLinkRole::Client) {
    pollClient(now);
  } else {
    pollServer(now);
  }
}

bool EspNowLink::startPairing(uint32_t timeoutMs) {
  if (!started_ || config_.role != EspNowLinkRole::Client) {
    return false;
  }

  if (packetQueue_) {
    xQueueReset(packetQueue_);
  }
  clearClientPairing();
  if (config_.role == EspNowLinkRole::Client && hasActiveProfile()) {
    removePeer(peerMac_);
  }
  resetLinkSession();
  removeBroadcastPeer();

  derivePairingWindowLmk(config_.labels.pairingWindow, pairingWindowLmk_);
  pairingKeypairValid_ = generateEcdhKeypair(pairingPrivateKey_, pairingPublicKey_);
  if (!pairingKeypairValid_) {
    clearClientPairing();
    emit(EspNowLinkEvent::PairingFailed);
    return false;
  }
  newPairingSessionId();
  probeIndex_ = 12;
  opChannel_ = kProbeOrder[0];
  esp_wifi_set_channel(opChannel_, WIFI_SECOND_CHAN_NONE);
  pairingStartMs_ = millis();
  pairingTimeoutMs_ = timeoutMs;
  beaconLastMs_ = 0;
  pairingComplete_ = false;
  setState(EspNowLinkState::Discovering);
  emit(EspNowLinkEvent::PairingStarted, nullptr, timeoutMs);
  return true;
}

void EspNowLink::cancelPairing() {
  if (!started_ || config_.role != EspNowLinkRole::Client) {
    return;
  }
  if (packetQueue_) {
    xQueueReset(packetQueue_);
  }
  clearClientPairing();
  removeBroadcastPeer();
  pairingComplete_ = false;
  if (hasActiveProfile()) {
    esp_wifi_set_channel(preferredChannel_, WIFI_SECOND_CHAN_NONE);
    registerPeer(peerMac_, preferredChannel_, true, lmk_);
    beginReconnecting();
  } else {
    setState(EspNowLinkState::Unpaired);
  }
  emit(EspNowLinkEvent::PairingCancelled);
}

bool EspNowLink::openPairingWindow(uint32_t timeoutMs) {
  if (!started_ || config_.role != EspNowLinkRole::Server) {
    return false;
  }
  clearServerPairing(true);
  if (packetQueue_) {
    xQueueReset(packetQueue_);
  }
  pairingWindowActive_ = true;
  pairingWindowUntilMs_ = millis() + timeoutMs;
  setState(EspNowLinkState::PairingWindowOpen);
  emit(EspNowLinkEvent::PairingWindowOpened, nullptr, timeoutMs);
  return true;
}

void EspNowLink::cancelPairingWindow() {
  pairingWindowActive_ = false;
  pairingWindowUntilMs_ = 0;
  clearServerPairing(true);
  if (profiles_.count > 0) {
    setState(EspNowLinkState::Synchronizing);
  } else {
    setState(EspNowLinkState::Unpaired);
  }
  emit(EspNowLinkEvent::PairingCancelled);
}

size_t EspNowLink::write(uint8_t c) {
  if (c == 0x11 || c == 0x13) {
    return 1;
  }
  if (c == 0x18 || (c >= 0x80 && c <= 0x9f) || (c >= 0xb0 && c <= 0xb3)) {
    sendRealtime(c);
    return 1;
  }
  if (txLen_ == 0 && (c == '?' || c == '!' || c == '~')) {
    sendRealtime(c);
    return 1;
  }

  if (txLen_ < sizeof(txBuffer_)) {
    txBuffer_[txLen_++] = c;
  }
  if (c == '\n' || txLen_ >= sizeof(txBuffer_) - 1) {
    size_t len = txLen_;
    txLen_ = 0;
    if (config_.role == EspNowLinkRole::Server) {
      broadcast(txBuffer_, len);
    } else {
      sendFragmentsTo(peerMac_, txBuffer_, len);
    }
  }
  return 1;
}

size_t EspNowLink::write(const uint8_t* data, size_t len) {
  if (!data) {
    return 0;
  }
  for (size_t i = 0; i < len; ++i) {
    write(data[i]);
  }
  return len;
}

bool EspNowLink::writeTo(const uint8_t mac[6], const uint8_t* data, size_t len) {
  if (!data || len == 0 || !mac) {
    return false;
  }
  if (config_.role == EspNowLinkRole::Server) {
    PeerRuntime* peer = findServerPeer(mac);
    return peer && peer->connected && sendFragmentsTo(*peer, data, len);
  }
  if (!hasActiveProfile() || !macEqual(mac, peerMac_)) {
    return false;
  }
  return sendFragmentsTo(peerMac_, data, len);
}

bool EspNowLink::broadcast(const uint8_t* data, size_t len) {
  if (!data || len == 0) {
    return false;
  }
  if (config_.role != EspNowLinkRole::Server) {
    return sendFragmentsTo(peerMac_, data, len);
  }
  bool sent = false;
  for (uint8_t i = 0; i < profiles_.count; ++i) {
    if (serverPeers_ && serverPeers_[i].connected) {
      sent = sendFragmentsTo(serverPeers_[i], data, len) || sent;
    }
  }
  return sent;
}

int EspNowLink::read() {
  int head = rxHead_.load(std::memory_order_acquire);
  if (head == rxTail_) {
    return -1;
  }
  uint8_t c = rxBuffer_[rxTail_];
  rxTail_ = (rxTail_ + 1) % kRxBufferSize;
  return c;
}

int EspNowLink::available() const {
  int head = rxHead_.load(std::memory_order_acquire);
  int tail = rxTail_;
  return head >= tail ? head - tail : (int)kRxBufferSize - tail + head;
}

bool EspNowLink::sendRealtime(uint8_t c) {
  if (config_.role == EspNowLinkRole::Server) {
    bool sent = false;
    for (uint8_t i = 0; i < profiles_.count; ++i) {
      if (serverPeers_) {
        sent = sendRealtimeTo(serverPeers_[i].mac, c) || sent;
      }
    }
    return sent;
  }
  if (state_ != EspNowLinkState::Connected || !hasActiveProfile()) {
    return false;
  }
  uint8_t packet[1 + kAntiReplayTagSize + 1];
  packet[0] = kPacketRealtime;
  if (!stampAntiReplayTag(txPeerKnown_, txPeerNonce_, txCounter_, packet + 1)) {
    return false;
  }
  packet[1 + kAntiReplayTagSize] = c;
  return esp_now_send(peerMac_, packet, sizeof(packet)) == ESP_OK;
}

bool EspNowLink::sendRealtimeTo(const uint8_t mac[6], uint8_t c) {
  if (!mac) {
    return false;
  }
  if (config_.role != EspNowLinkRole::Server) {
    return macEqual(mac, peerMac_) && sendRealtime(c);
  }
  PeerRuntime* peer = findServerPeer(mac);
  if (!peer || !peer->connected) {
    return false;
  }
  uint8_t packet[1 + kAntiReplayTagSize + 1];
  packet[0] = kPacketRealtime;
  if (!stampAntiReplayTag(peer->txPeerKnown, peer->txPeerNonce, peer->txCounter, packet + 1)) {
    return false;
  }
  packet[1 + kAntiReplayTagSize] = c;
  return esp_now_send(peer->mac, packet, sizeof(packet)) == ESP_OK;
}

bool EspNowLink::isPaired() const {
  if (config_.role == EspNowLinkRole::Server) {
    return profiles_.count > 0;
  }
  return hasActiveProfile();
}

bool EspNowLink::isConnected() const {
  return state_ == EspNowLinkState::Connected;
}

bool EspNowLink::isPairing() const {
  return state_ == EspNowLinkState::Discovering ||
         state_ == EspNowLinkState::Confirming ||
         state_ == EspNowLinkState::PairingWindowOpen;
}

EspNowLinkState EspNowLink::state() const {
  return state_;
}

const char* EspNowLink::stateName() const {
  switch (state_) {
    case EspNowLinkState::Unpaired: return "Unpaired";
    case EspNowLinkState::Discovering: return "Discovering";
    case EspNowLinkState::Confirming: return "Confirming";
    case EspNowLinkState::Synchronizing: return "Synchronizing";
    case EspNowLinkState::Connected: return "Connected";
    case EspNowLinkState::Reconnecting: return "Reconnecting";
    case EspNowLinkState::PairingWindowOpen: return "PairingWindowOpen";
  }
  return "Unknown";
}

int8_t EspNowLink::rssi() const {
  if (config_.role == EspNowLinkRole::Server) {
    int total = 0;
    int count = 0;
    for (uint8_t i = 0; i < profiles_.count; ++i) {
      if (serverPeers_ && serverPeers_[i].connected && serverPeers_[i].rssi != -100) {
        total += serverPeers_[i].rssi;
        ++count;
      }
    }
    return count ? (int8_t)(total / count) : -100;
  }
  return peerRssi_;
}

size_t EspNowLink::profileCount() const {
  return profiles_.count;
}

int EspNowLink::activeProfileIndex() const {
  return profiles_.count > 0 ? profiles_.active : -1;
}

bool EspNowLink::getProfile(size_t index, EspNowLinkProfile& out) const {
  memset(&out, 0, sizeof(out));
  if (index >= profiles_.count) {
    return false;
  }
  const StoredProfile& profile = profiles_.profiles[index];
  memcpy(out.mac, profile.mac, sizeof(out.mac));
  memcpy(out.lmk, profile.lmk, sizeof(out.lmk));
  out.channel = profile.channel;
  out.active = index == profiles_.active;
  strlcpy(out.hostname, profile.hostname, sizeof(out.hostname));
  return true;
}

bool EspNowLink::selectProfile(size_t index) {
  if (index >= profiles_.count) {
    return false;
  }
  if (config_.role == EspNowLinkRole::Server) {
    profiles_.active = index;
    if (!saveProfiles()) {
      return false;
    }
    loadServerPeersIntoRuntime();
    setState(EspNowLinkState::Synchronizing);
    emit(EspNowLinkEvent::ProfileChanged, profiles_.profiles[index].mac, index);
    return true;
  }
  if (config_.role == EspNowLinkRole::Client && hasActiveProfile()) {
    removePeer(peerMac_);
  }
  profiles_.active = index;
  if (!saveProfiles()) {
    return false;
  }
  loadActiveIntoRuntime();
  registerPeer(peerMac_,
               config_.role == EspNowLinkRole::Server ? 0 : preferredChannel_,
               true,
               lmk_);
  if (config_.role == EspNowLinkRole::Client) {
    beginReconnecting();
  } else {
    loadServerPeersIntoRuntime();
    setState(EspNowLinkState::Synchronizing);
  }
  emit(EspNowLinkEvent::ProfileChanged, peerMac_, index);
  return true;
}

bool EspNowLink::removeProfile(size_t index) {
  if (index >= profiles_.count) {
    return false;
  }
  uint8_t removedMac[6];
  memcpy(removedMac, profiles_.profiles[index].mac, sizeof(removedMac));
  removePeer(removedMac);
  for (size_t i = index; i + 1 < profiles_.count; ++i) {
    profiles_.profiles[i] = profiles_.profiles[i + 1];
  }
  --profiles_.count;
  memset(&profiles_.profiles[profiles_.count], 0, sizeof(profiles_.profiles[profiles_.count]));
  if (profiles_.count == 0) {
    profiles_.active = 0;
    if (config_.role == EspNowLinkRole::Client) {
      memset(peerMac_, 0, sizeof(peerMac_));
      secureZero(lmk_, sizeof(lmk_));
      memset(peerHostname_, 0, sizeof(peerHostname_));
    }
    setState(EspNowLinkState::Unpaired);
  } else if (config_.role == EspNowLinkRole::Server) {
    profiles_.active = index < profiles_.count ? index : profiles_.count - 1;
    setState(EspNowLinkState::Synchronizing);
  } else {
    profiles_.active = index < profiles_.count ? index : profiles_.count - 1;
    loadActiveIntoRuntime();
    registerPeer(peerMac_, preferredChannel_, true, lmk_);
    beginReconnecting();
  }
  saveProfiles();
  if (config_.role == EspNowLinkRole::Server) {
    loadServerPeersIntoRuntime();
  }
  emit(EspNowLinkEvent::ProfileChanged, removedMac, index);
  return true;
}

void EspNowLink::clearProfiles() {
  if (config_.role == EspNowLinkRole::Server) {
    for (uint8_t i = 0; i < profiles_.count; ++i) {
      removePeer(profiles_.profiles[i].mac);
    }
  } else if (hasActiveProfile()) {
    removePeer(peerMac_);
  }
  secureZero(&profiles_, sizeof(profiles_));
  profiles_.version = kProfileStoreVersion;
  if (serverPeers_) {
    for (uint8_t i = 0; i < kMaxProfiles; ++i) {
      PeerRuntime& peer = serverPeers_[i];
      memset(peer.mac, 0, sizeof(peer.mac));
      secureZero(peer.lmk, sizeof(peer.lmk));
      peer.channel = kPairChannel;
      memset(peer.hostname, 0, sizeof(peer.hostname));
      resetServerPeer(peer);
    }
  }
  memset(peerMac_, 0, sizeof(peerMac_));
  secureZero(lmk_, sizeof(lmk_));
  memset(peerHostname_, 0, sizeof(peerHostname_));
  Preferences prefs;
  if (prefs.begin(config_.nvsNamespace, false)) {
    prefs.remove("profiles");
    prefs.end();
  }
  resetLinkSession();
  setState(EspNowLinkState::Unpaired);
  emit(EspNowLinkEvent::ProfileChanged);
}

size_t EspNowLink::peerCount() const {
  return config_.role == EspNowLinkRole::Server ? profiles_.count : (isConnected() ? 1 : 0);
}

bool EspNowLink::getPeer(size_t index, EspNowLinkPeerInfo& out) const {
  memset(&out, 0, sizeof(out));
  out.rssi = -100;
  if (config_.role == EspNowLinkRole::Server) {
    if (index >= profiles_.count) {
      return false;
    }
    if (!serverPeers_) {
      return false;
    }
    const PeerRuntime& peer = serverPeers_[index];
    memcpy(out.mac, peer.mac, sizeof(out.mac));
    out.channel = peer.channel;
    out.connected = peer.connected;
    out.rssi = peer.rssi;
    out.lastRxMs = peer.lastRxMs;
    strlcpy(out.hostname, peer.hostname, sizeof(out.hostname));
    return macNonzero(out.mac);
  }
  if (index != 0 || !hasActiveProfile()) {
    return false;
  }
  memcpy(out.mac, peerMac_, sizeof(out.mac));
  out.channel = opChannel_;
  out.connected = isConnected();
  out.rssi = peerRssi_;
  out.lastRxMs = lastRxMs_;
  strlcpy(out.hostname, peerHostname_, sizeof(out.hostname));
  return true;
}

void EspNowLink::onEvent(EventCallback cb) {
  eventCb_ = cb;
}

void EspNowLink::onReceive(ReceiveCallback cb) {
  receiveCb_ = cb;
}

String EspNowLink::formatMac(const uint8_t mac[6]) {
  char out[18];
  snprintf(out, sizeof(out), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(out);
}

#if ESP_IDF_VERSION_MAJOR >= 5
void EspNowLink::onRecvStatic(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!instance_ || !info) {
    return;
  }
  uint8_t channel = (info->rx_ctrl) ? info->rx_ctrl->channel : 0;
  int8_t rssi = (info->rx_ctrl) ? static_cast<int8_t>(info->rx_ctrl->rssi) : -100;
  instance_->handleRecv(info->src_addr, data, len, channel, rssi);
}
#else
void EspNowLink::onRecvStatic(const uint8_t* mac, const uint8_t* data, int len) {
  if (!instance_) {
    return;
  }
  instance_->handleRecv(mac, data, len, instance_->currentWifiChannel(), -100);
}
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void EspNowLink::onSentStatic(const esp_now_send_info_t* info, esp_now_send_status_t status) {
  if (instance_ && status != ESP_NOW_SEND_SUCCESS) {
    instance_->emit(EspNowLinkEvent::SendFailed, info ? info->des_addr : nullptr, status);
  }
}
#else
void EspNowLink::onSentStatic(const uint8_t* mac, esp_now_send_status_t status) {
  if (instance_ && status != ESP_NOW_SEND_SUCCESS) {
    instance_->emit(EspNowLinkEvent::SendFailed, mac, status);
  }
}
#endif

void EspNowLink::handleRecv(const uint8_t* src, const uint8_t* data, int len, uint8_t channel, int8_t rssi) {
  if (!src || !data || len < 1 || len > (int)kMaxEspNowPayload) {
    return;
  }
  queuePacket(src, data, len, channel, rssi);
}

bool EspNowLink::queuePacket(const uint8_t* src, const uint8_t* data, int len, uint8_t channel, int8_t rssi) {
  if (!packetQueue_) {
    return false;
  }
  RxPacket packet = {};
  memcpy(packet.src, src, sizeof(packet.src));
  packet.len = len;
  packet.channel = channel;
  packet.rssi = rssi;
  memcpy(packet.data, data, len);
  return xQueueSend(packetQueue_, &packet, 0) == pdTRUE;
}

bool EspNowLink::popPacket(RxPacket& packet) {
  if (!packetQueue_) {
    return false;
  }
  return xQueueReceive(packetQueue_, &packet, 0) == pdTRUE;
}

void EspNowLink::processPacket(const RxPacket& packet) {
  if (packet.len == 0) {
    return;
  }
  uint8_t type = packet.data[0];
  if (config_.role == EspNowLinkRole::Client) {
    if (type == kPacketPairChallenge && handlePairChallenge(packet)) {
      return;
    }
    if (type == kPacketPairResult && handlePairResult(packet)) {
      return;
    }
    if (!hasActiveProfile() || !macEqual(packet.src, peerMac_)) {
      return;
    }
  } else {
    if (type == kPacketDiscovery) {
      handleServerDiscovery(packet);
      return;
    }
    if (type == kPacketPairConfirm) {
      handleServerConfirm(packet);
      return;
    }
    if (type == kPacketPairComplete) {
      handleServerComplete(packet);
      return;
    }
    PeerRuntime* peer = findServerPeer(packet.src);
    if (!peer) {
      return;
    }
    if (packet.rssi != -100) {
      peer->rssi = peer->rssi == -100 ? packet.rssi : static_cast<int8_t>(((int)peer->rssi * 4 + packet.rssi) / 5);
    }
    if (type == kPacketKeepalive) {
      handleKeepalivePacket(*peer, packet);
    } else if (type == kPacketRealtime) {
      handleRealtimePacket(*peer, packet);
    } else if (type == kPacketData) {
      handleDataPacket(*peer, packet);
    }
    return;
  }

  if (packet.rssi != -100) {
    peerRssi_ = peerRssi_ == -100 ? packet.rssi : static_cast<int8_t>(((int)peerRssi_ * 4 + packet.rssi) / 5);
  }
  if (type == kPacketKeepalive) {
    handleKeepalivePacket(packet);
  } else if (type == kPacketRealtime) {
    handleRealtimePacket(packet);
  } else if (type == kPacketData) {
    handleDataPacket(packet);
  }
}

void EspNowLink::pollClient(uint32_t now) {
  if (state_ == EspNowLinkState::Connected &&
      (uint32_t)(now - lastRxMs_) > kConnectTimeoutMs) {
    beginReconnecting();
  }

  if (state_ == EspNowLinkState::Reconnecting) {
    if ((uint32_t)(now - reconnectLastMs_) >= kReconnectProbeMs) {
      reconnectLastMs_ = now;
      uint8_t ch = reconnectProbeIndex_ < 3 && reconnectSavedChannel_ >= 1 && reconnectSavedChannel_ <= 13
                     ? reconnectSavedChannel_
                     : kProbeOrder[(reconnectProbeIndex_ - 3) % 13];
      ++reconnectProbeIndex_;
      if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) == ESP_OK &&
          registerPeer(peerMac_, ch, true, lmk_)) {
        opChannel_ = ch;
        sendKeepalive(peerMac_);
      }
    }
    return;
  }

  if (state_ == EspNowLinkState::Synchronizing) {
    if ((uint32_t)(now - stateSinceMs_) >= kSynchronizeTimeoutMs) {
      beginReconnecting();
      return;
    }
    if ((uint32_t)(now - synchronizeLastTxMs_) >= kSynchronizeRetryMs) {
      synchronizeLastTxMs_ = now;
      synchronizeAuthenticatedTx_ = sendKeepalive(peerMac_) || synchronizeAuthenticatedTx_;
    }
    return;
  }

  if (state_ == EspNowLinkState::Connected &&
      (uint32_t)(now - keepaliveLastMs_) >= kKeepaliveIntervalMs) {
    keepaliveLastMs_ = now;
    sendKeepalive(peerMac_);
    return;
  }

  if (state_ != EspNowLinkState::Discovering &&
      state_ != EspNowLinkState::Confirming) {
    return;
  }

  if (pairingTimeoutMs_ != 0 && (uint32_t)(now - pairingStartMs_) >= pairingTimeoutMs_) {
    clearClientPairing();
    derivePairingWindowLmk(config_.labels.pairingWindow, pairingWindowLmk_);
    pairingKeypairValid_ = generateEcdhKeypair(pairingPrivateKey_, pairingPublicKey_);
    if (!pairingKeypairValid_) {
      if (hasActiveProfile()) {
        beginReconnecting();
      } else {
        setState(EspNowLinkState::Unpaired);
      }
      emit(EspNowLinkEvent::PairingFailed);
      return;
    }
    newPairingSessionId();
    pairingStartMs_ = now;
    setState(EspNowLinkState::Discovering);
  }

  if (state_ == EspNowLinkState::Confirming) {
    if (pairingCompletionPending_) {
      if (pairingCompletionAttempts_ < kPairCompleteAttempts &&
          (uint32_t)(now - pairingLastCompletionMs_) >= kPairCompleteRetryMs) {
        ++pairingCompletionAttempts_;
        pairingLastCompletionMs_ = now;
        esp_now_send(pairingPeerMac_,
                     reinterpret_cast<const uint8_t*>(&pairingCompletePacket_),
                     sizeof(pairingCompletePacket_));
        return;
      }
      if (pairingCompletionAttempts_ >= kPairCompleteAttempts &&
          (uint32_t)(now - pairingLastCompletionMs_) >= kPairCompleteRetryMs) {
        PairResultV4Packet result;
        memcpy(&result, &pairingResult_, sizeof(result));
        completePairingFromResult(result);
        secureZero(&result, sizeof(result));
        return;
      }
      return;
    }

    if ((uint32_t)(now - pairingAwaitResultMs_) >= kPairResultTimeoutMs) {
      removePeer(pairingPeerMac_);
      secureZero(pairingLmk_, sizeof(pairingLmk_));
      setState(EspNowLinkState::Discovering);
      beaconLastMs_ = 0;
      return;
    }
    if ((uint32_t)(now - pairingLastConfirmMs_) >= kPairConfirmRetryMs) {
      pairingLastConfirmMs_ = now;
      esp_wifi_set_channel(pairingPeerChannel_, WIFI_SECOND_CHAN_NONE);
      esp_now_send(pairingPeerMac_,
                   reinterpret_cast<const uint8_t*>(&pairingConfirm_),
                   sizeof(pairingConfirm_));
    }
    return;
  }

  if ((uint32_t)(now - beaconLastMs_) >= kBeaconIntervalMs) {
    beaconLastMs_ = now;
    probeIndex_ = (probeIndex_ + 1) % 13;
    opChannel_ = kProbeOrder[probeIndex_];
    if (esp_wifi_set_channel(opChannel_, WIFI_SECOND_CHAN_NONE) != ESP_OK ||
        !addBroadcastPeer(opChannel_)) {
      return;
    }

    uint8_t myMac[6];
    if (!localMac(myMac)) {
      return;
    }
    if (!pairingKeypairValid_) {
      pairingKeypairValid_ = generateEcdhKeypair(pairingPrivateKey_, pairingPublicKey_);
    }
    if (pairingKeypairValid_) {
      DiscoveryV4Packet discovery;
      buildDiscovery(discovery, myMac, opChannel_);
      esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t*>(&discovery), sizeof(discovery));
      secureZero(&discovery, sizeof(discovery));
    }
  }
}

void EspNowLink::pollServer(uint32_t now) {
  if (!serverPeers_) {
    return;
  }

  if (pairingWindowActive_ && (int32_t)(pairingWindowUntilMs_ - now) <= 0) {
    pairingWindowActive_ = false;
    clearServerPairing(true);
    emit(EspNowLinkEvent::PairingWindowClosed);
    setState(hasActiveProfile() ? EspNowLinkState::Synchronizing : EspNowLinkState::Unpaired);
  }

  if (serverPairing_.state == PairingTransaction::State::AwaitConfirm) {
    if ((uint32_t)(now - serverPairing_.lastMs) > kServerPairingHandshakeTimeoutMs) {
      clearServerPairing(true);
    }
  } else if (serverPairing_.state == PairingTransaction::State::ReadyResult) {
    ++serverPairing_.attempts;
    serverPairing_.lastMs = now;
    if (esp_now_send(serverPairing_.mac, serverPairing_.result, serverPairing_.resultLen) == ESP_OK) {
      serverPairing_.state = PairingTransaction::State::AwaitComplete;
    } else if (serverPairing_.attempts >= 3) {
      uint8_t failedMac[6];
      memcpy(failedMac, serverPairing_.mac, sizeof(failedMac));
      clearServerPairing(true);
      emit(EspNowLinkEvent::PairingFailed, failedMac);
    }
  } else if (serverPairing_.state == PairingTransaction::State::AwaitComplete) {
    if ((uint32_t)(now - serverPairing_.startedMs) > kServerPairingHandshakeTimeoutMs) {
      clearServerPairing(true);
    } else if ((uint32_t)(now - serverPairing_.lastMs) >= kServerPairingResultRetryMs) {
      serverPairing_.lastMs = now;
      esp_now_send(serverPairing_.mac, serverPairing_.result, serverPairing_.resultLen);
    }
  }

  bool anyConnected = false;
  for (uint8_t i = 0; i < profiles_.count; ++i) {
    PeerRuntime& peer = serverPeers_[i];
    if (!macNonzero(peer.mac)) {
      continue;
    }
    if (peer.connected &&
        peer.lastRxMs != 0 &&
        (uint32_t)(now - peer.lastRxMs) > kServerPendantIdleTimeoutMs) {
      uint8_t mac[6];
      memcpy(mac, peer.mac, sizeof(mac));
      resetServerPeer(peer);
      emit(EspNowLinkEvent::Disconnected, mac);
      continue;
    }
    if (peer.connected) {
      anyConnected = true;
      if ((uint32_t)(now - keepaliveLastMs_) >= kKeepaliveIntervalMs) {
        sendKeepalive(peer);
      }
    }
  }
  if ((uint32_t)(now - keepaliveLastMs_) >= kKeepaliveIntervalMs) {
    keepaliveLastMs_ = now;
  }
  if (!pairingWindowActive_) {
    if (anyConnected) {
      setState(EspNowLinkState::Connected);
    } else if (profiles_.count > 0) {
      setState(EspNowLinkState::Synchronizing);
    } else {
      setState(EspNowLinkState::Unpaired);
    }
  }
}

bool EspNowLink::loadProfiles() {
  Preferences prefs;
  if (!prefs.begin(config_.nvsNamespace, false)) {
    return false;
  }
  bool ok = false;
  if (prefs.isKey("profiles") &&
      prefs.getBytesLength("profiles") == sizeof(profiles_) &&
      prefs.getBytes("profiles", &profiles_, sizeof(profiles_)) == sizeof(profiles_) &&
      profiles_.version == kProfileStoreVersion &&
      profiles_.count <= kMaxProfiles &&
      (profiles_.count == 0 || profiles_.active < profiles_.count)) {
    ok = true;
    for (uint8_t i = 0; i < profiles_.count; ++i) {
      ok = ok && validProfile(profiles_.profiles[i]);
    }
  }
  prefs.end();
  if (!ok) {
    secureZero(&profiles_, sizeof(profiles_));
    profiles_.version = kProfileStoreVersion;
  }
  return ok;
}

bool EspNowLink::saveProfiles() {
  Preferences prefs;
  if (!prefs.begin(config_.nvsNamespace, false)) {
    return false;
  }
  bool ok = prefs.putBytes("profiles", &profiles_, sizeof(profiles_)) == sizeof(profiles_);
  prefs.end();
  return ok;
}

bool EspNowLink::validProfile(const StoredProfile& profile) const {
  return profile.version == kProfileStoreVersion &&
         macNonzero(profile.mac) &&
         (profile.mac[0] & 0x01) == 0 &&
         lmkNonzero(profile.lmk) &&
         profile.channel >= 1 &&
         profile.channel <= 14 &&
         memchr(profile.hostname, '\0', sizeof(profile.hostname)) != nullptr;
}

bool EspNowLink::hasActiveProfile() const {
  return macNonzero(peerMac_) && (peerMac_[0] & 0x01) == 0 && lmkNonzero(lmk_);
}

bool EspNowLink::storeActiveProfile(const uint8_t mac[6], const uint8_t lmk[16], uint8_t channel, const char* hostname) {
  if (!mac || !lmk || channel < 1 || channel > 14) {
    return false;
  }
  int slot = -1;
  for (uint8_t i = 0; i < profiles_.count; ++i) {
    if (macEqual(profiles_.profiles[i].mac, mac)) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    if (profiles_.count < kMaxProfiles) {
      slot = profiles_.count++;
    } else {
      slot = profiles_.active < kMaxProfiles ? profiles_.active : 0;
    }
  }
  StoredProfile& profile = profiles_.profiles[slot];
  memset(&profile, 0, sizeof(profile));
  profile.version = kProfileStoreVersion;
  memcpy(profile.mac, mac, 6);
  memcpy(profile.lmk, lmk, 16);
  profile.channel = channel;
  strlcpy(profile.hostname, hostname ? hostname : "", sizeof(profile.hostname));
  profiles_.active = slot;
  return saveProfiles();
}

void EspNowLink::loadActiveIntoRuntime() {
  if (profiles_.count == 0 || profiles_.active >= profiles_.count) {
    return;
  }
  const StoredProfile& profile = profiles_.profiles[profiles_.active];
  memcpy(peerMac_, profile.mac, sizeof(peerMac_));
  memcpy(lmk_, profile.lmk, sizeof(lmk_));
  preferredChannel_ = profile.channel ? profile.channel : kPairChannel;
  opChannel_ = preferredChannel_;
  strlcpy(peerHostname_, profile.hostname, sizeof(peerHostname_));
}

bool EspNowLink::ensureServerPeers() {
  if (serverPeers_) {
    return true;
  }
  serverPeers_ = new (std::nothrow) PeerRuntime[kMaxProfiles];
  return serverPeers_ != nullptr;
}

void EspNowLink::loadServerPeersIntoRuntime() {
  if (!ensureServerPeers()) {
    return;
  }
  for (uint8_t i = 0; i < kMaxProfiles; ++i) {
    PeerRuntime& peer = serverPeers_[i];
    memset(peer.mac, 0, sizeof(peer.mac));
    secureZero(peer.lmk, sizeof(peer.lmk));
    peer.channel = kPairChannel;
    memset(peer.hostname, 0, sizeof(peer.hostname));
    resetServerPeer(peer);
  }
  for (uint8_t i = 0; i < profiles_.count; ++i) {
    const StoredProfile& profile = profiles_.profiles[i];
    PeerRuntime& peer = serverPeers_[i];
    memcpy(peer.mac, profile.mac, sizeof(peer.mac));
    memcpy(peer.lmk, profile.lmk, sizeof(peer.lmk));
    peer.channel = profile.channel ? profile.channel : kPairChannel;
    strlcpy(peer.hostname, profile.hostname, sizeof(peer.hostname));
    resetServerPeer(peer);
  }
}

EspNowLink::PeerRuntime* EspNowLink::findServerPeer(const uint8_t mac[6]) {
  if (!mac || !serverPeers_) {
    return nullptr;
  }
  for (uint8_t i = 0; i < profiles_.count; ++i) {
    if (macEqual(serverPeers_[i].mac, mac)) {
      return &serverPeers_[i];
    }
  }
  return nullptr;
}

const EspNowLink::PeerRuntime* EspNowLink::findServerPeer(const uint8_t mac[6]) const {
  if (!mac || !serverPeers_) {
    return nullptr;
  }
  for (uint8_t i = 0; i < profiles_.count; ++i) {
    if (macEqual(serverPeers_[i].mac, mac)) {
      return &serverPeers_[i];
    }
  }
  return nullptr;
}

void EspNowLink::resetServerPeer(PeerRuntime& peer) {
  peer.txPeerKnown.store(false, std::memory_order_release);
  peer.txPeerNonce.store(0, std::memory_order_release);
  peer.txCounter.store(0, std::memory_order_release);
  issueRxChallenge(peer.rxNonce);
  peer.rxReplay = {};
  peer.frag = {};
  peer.txSequence = 0;
  peer.lastRxMs = 0;
  peer.rssi = -100;
  peer.connected = false;
}

bool EspNowLink::setState(EspNowLinkState state) {
  if (state_ == state) {
    return true;
  }
  state_ = state;
  stateSinceMs_ = millis();
  emit(EspNowLinkEvent::StateChanged);
  return true;
}

bool EspNowLink::registerPeer(const uint8_t mac[6], uint8_t channel, bool encrypted, const uint8_t lmk[16]) {
  if (!mac || !macNonzero(mac) || (encrypted && !lmk)) {
    return false;
  }
  if (channel < 1 || channel > 14) {
    channel = 0;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = channel;
  peer.encrypt = encrypted;
  peer.ifidx = interface();
  if (encrypted) {
    memcpy(peer.lmk, lmk, 16);
  }
  if (esp_now_is_peer_exist(mac)) {
    if (esp_now_mod_peer(&peer) == ESP_OK) {
      return true;
    }
    esp_now_del_peer(mac);
  }
  return esp_now_add_peer(&peer) == ESP_OK;
}

void EspNowLink::removePeer(const uint8_t mac[6]) {
  if (mac && esp_now_is_peer_exist(mac)) {
    esp_now_del_peer(mac);
  }
}

void EspNowLink::removeBroadcastPeer() {
  removePeer(kBroadcastMac);
}

bool EspNowLink::addBroadcastPeer(uint8_t channel) {
  removeBroadcastPeer();
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kBroadcastMac, 6);
  peer.channel = channel;
  peer.encrypt = false;
  peer.ifidx = interface();
  return esp_now_add_peer(&peer) == ESP_OK;
}

uint8_t EspNowLink::currentWifiChannel() const {
  if (interface() == WIFI_IF_AP) {
    wifi_config_t config = {};
    if (esp_wifi_get_config(WIFI_IF_AP, &config) == ESP_OK &&
        config.ap.channel >= 1 &&
        config.ap.channel <= 14) {
      return config.ap.channel;
    }
  }

  uint8_t arduinoChannel = WiFi.channel();
  if (arduinoChannel >= 1 && arduinoChannel <= 14) {
    return arduinoChannel;
  }

  uint8_t primary = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&primary, &second) == ESP_OK &&
      primary >= 1 &&
      primary <= 14) {
    return primary;
  }
  return 0;
}

bool EspNowLink::localMac(uint8_t mac[6]) const {
  return esp_wifi_get_mac(interface(), mac) == ESP_OK;
}

wifi_interface_t EspNowLink::interface() const {
  return config_.role == EspNowLinkRole::Server ? config_.interface : WIFI_IF_STA;
}

void EspNowLink::resetLinkSession() {
  txPeerKnown_.store(false, std::memory_order_release);
  txPeerNonce_.store(0, std::memory_order_release);
  txCounter_.store(0, std::memory_order_release);
  txSequence_ = 0;
  issueRxChallenge(rxNonce_);
  rxReplay_ = {};
  frag_ = {};
}

void EspNowLink::beginReconnecting() {
  if (!hasActiveProfile()) {
    setState(EspNowLinkState::Unpaired);
    return;
  }
  resetLinkSession();
  lastRxMs_ = 0;
  reconnectProbeIndex_ = 0;
  reconnectLastMs_ = 0;
  reconnectSavedChannel_ = preferredChannel_;
  synchronizeAuthenticatedTx_ = false;
  setState(EspNowLinkState::Reconnecting);
}

void EspNowLink::beginSynchronizing(uint8_t channel) {
  if (!hasActiveProfile() || channel < 1 || channel > 14) {
    beginReconnecting();
    return;
  }
  opChannel_ = channel;
  synchronizeLastTxMs_ = millis();
  setState(EspNowLinkState::Synchronizing);
  synchronizeAuthenticatedTx_ = sendKeepalive(peerMac_);
}

bool EspNowLink::sendKeepalive(const uint8_t mac[6]) {
  uint32_t n = rxNonce_.load(std::memory_order_acquire);
  if (!txPeerKnown_.load(std::memory_order_acquire)) {
    uint8_t packet[5];
    packet[0] = kPacketKeepalive;
    memcpy(packet + 1, &n, 4);
    esp_now_send(mac, packet, sizeof(packet));
    return false;
  }

  uint8_t packet[kAuthKeepaliveSize];
  packet[0] = kPacketKeepalive;
  memcpy(packet + 1, &n, 4);
  if (!stampAntiReplayTag(txPeerKnown_, txPeerNonce_, txCounter_, packet + 5)) {
    return false;
  }
  packet[1 + 4 + kAntiReplayTagSize] =
      state_ == EspNowLinkState::Connected ? kKeepaliveSessionConfirmed : 0;
  return esp_now_send(mac, packet, sizeof(packet)) == ESP_OK;
}

bool EspNowLink::sendKeepalive(PeerRuntime& peer) {
  uint32_t n = peer.rxNonce.load(std::memory_order_acquire);
  if (!peer.txPeerKnown.load(std::memory_order_acquire)) {
    uint8_t packet[5];
    packet[0] = kPacketKeepalive;
    memcpy(packet + 1, &n, 4);
    esp_now_send(peer.mac, packet, sizeof(packet));
    return false;
  }

  uint8_t packet[kAuthKeepaliveSize];
  packet[0] = kPacketKeepalive;
  memcpy(packet + 1, &n, 4);
  if (!stampAntiReplayTag(peer.txPeerKnown, peer.txPeerNonce, peer.txCounter, packet + 5)) {
    return false;
  }
  packet[1 + 4 + kAntiReplayTagSize] = peer.connected ? kKeepaliveSessionConfirmed : 0;
  return esp_now_send(peer.mac, packet, sizeof(packet)) == ESP_OK;
}

int EspNowLink::acceptKeepalive(const uint8_t* data, int len) {
  if (len == (int)kAuthKeepaliveSize) {
    uint32_t advertised, nonce, counter;
    memcpy(&advertised, data + 1, 4);
    memcpy(&nonce, data + 5, 4);
    memcpy(&counter, data + 9, 4);
    if (!acceptReplay(rxNonce_, rxReplay_, nonce, counter, millis()) || advertised == 0) {
      return 0;
    }
    txPeerNonce_.store(advertised, std::memory_order_release);
    txPeerKnown_.store(true, std::memory_order_release);
    return (data[1 + 4 + kAntiReplayTagSize] & kKeepaliveSessionConfirmed) ? 3 : 2;
  }
  if (len == 5) {
    uint32_t advertised;
    memcpy(&advertised, data + 1, 4);
    if (advertised == 0) {
      return 0;
    }
    bool newSession =
        !txPeerKnown_.load(std::memory_order_acquire) ||
        txPeerNonce_.load(std::memory_order_acquire) != advertised;
    txPeerNonce_.store(advertised, std::memory_order_release);
    txPeerKnown_.store(true, std::memory_order_release);
    if (newSession) {
      txCounter_.store(0, std::memory_order_release);
      issueRxChallenge(rxNonce_);
      rxReplay_ = {};
    }
    return 1;
  }
  return 0;
}

int EspNowLink::acceptKeepalive(PeerRuntime& peer, const uint8_t* data, int len) {
  if (len == (int)kAuthKeepaliveSize) {
    uint32_t advertised, nonce, counter;
    memcpy(&advertised, data + 1, 4);
    memcpy(&nonce, data + 5, 4);
    memcpy(&counter, data + 9, 4);
    if (!acceptReplay(peer.rxNonce, peer.rxReplay, nonce, counter, millis()) || advertised == 0) {
      return 0;
    }
    peer.txPeerNonce.store(advertised, std::memory_order_release);
    peer.txPeerKnown.store(true, std::memory_order_release);
    return (data[1 + 4 + kAntiReplayTagSize] & kKeepaliveSessionConfirmed) ? 3 : 2;
  }
  if (len == 5) {
    uint32_t advertised;
    memcpy(&advertised, data + 1, 4);
    if (advertised == 0) {
      return 0;
    }
    bool newSession =
        !peer.txPeerKnown.load(std::memory_order_acquire) ||
        peer.txPeerNonce.load(std::memory_order_acquire) != advertised;
    peer.txPeerNonce.store(advertised, std::memory_order_release);
    peer.txPeerKnown.store(true, std::memory_order_release);
    if (newSession) {
      peer.txCounter.store(0, std::memory_order_release);
      issueRxChallenge(peer.rxNonce);
      peer.rxReplay = {};
      peer.frag = {};
      peer.txSequence = 0;
      peer.connected = false;
      peer.lastRxMs = 0;
    }
    return 1;
  }
  return 0;
}

void EspNowLink::setConnectedNow() {
  bool wasConnected = state_ == EspNowLinkState::Connected;
  lastRxMs_ = millis();
  setState(EspNowLinkState::Connected);
  if (!wasConnected) {
    emit(EspNowLinkEvent::Connected, peerMac_);
  }
}

void EspNowLink::setServerPeerConnected(PeerRuntime& peer) {
  bool wasConnected = peer.connected;
  peer.connected = true;
  peer.lastRxMs = millis();
  setState(EspNowLinkState::Connected);
  if (!wasConnected) {
    emit(EspNowLinkEvent::Connected, peer.mac);
  }
}

void EspNowLink::newPairingSessionId() {
  do {
    for (size_t i = 0; i < sizeof(pairingSessionId_); i += sizeof(uint32_t)) {
      uint32_t value = randomNonce();
      memcpy(pairingSessionId_ + i, &value, sizeof(value));
    }
  } while (constantTimeEquals(pairingSessionId_, kZeroSession, sizeof(pairingSessionId_)));
}

void EspNowLink::buildDiscovery(DiscoveryV4Packet& packet, const uint8_t mac[6], uint8_t channel) const {
  memset(&packet, 0, sizeof(packet));
  packet.type = kPacketDiscovery;
  packet.version = kPairingProtocolV4;
  packet.mode = kPairingModePair;
  memcpy(packet.mac, mac, 6);
  packet.channel = channel;
  memcpy(packet.sessionId, pairingSessionId_, sizeof(packet.sessionId));
  memcpy(packet.publicKey, pairingPublicKey_, sizeof(packet.publicKey));
}

void EspNowLink::clearClientPairing() {
  if (state_ == EspNowLinkState::Confirming) {
    removePeer(pairingPeerMac_);
  }
  pairingKeypairValid_ = false;
  secureZero(pairingPrivateKey_, sizeof(pairingPrivateKey_));
  secureZero(pairingPublicKey_, sizeof(pairingPublicKey_));
  secureZero(pairingWindowLmk_, sizeof(pairingWindowLmk_));
  secureZero(pairingLmk_, sizeof(pairingLmk_));
  secureZero(&pairingConfirm_, sizeof(pairingConfirm_));
  secureZero(&pairingCompletePacket_, sizeof(pairingCompletePacket_));
  secureZero(&pairingResult_, sizeof(pairingResult_));
  memset(pairingPeerMac_, 0, sizeof(pairingPeerMac_));
  pairingPeerChannel_ = 0;
  pairingCompletionPending_ = false;
  pairingCompletionAttempts_ = 0;
}

bool EspNowLink::handlePairChallenge(const RxPacket& packet) {
  if (state_ != EspNowLinkState::Discovering ||
      !pairingKeypairValid_ ||
      packet.len != sizeof(PairChallengeV4Packet)) {
    return false;
  }

  PairChallengeV4Packet challenge;
  memcpy(&challenge, packet.data, sizeof(challenge));
  if (challenge.type != kPacketPairChallenge ||
      challenge.version != kPairingProtocolV4 ||
      challenge.mode != kPairingModePair ||
      !macEqual(packet.src, challenge.mac) ||
      !constantTimeEquals(challenge.sessionId, pairingSessionId_, sizeof(challenge.sessionId)) ||
      challenge.channel < 1 ||
      challenge.channel > 14 ||
      challenge.dialChannel < 1 ||
      challenge.dialChannel > 14 ||
      challenge.publicKey[0] != 0x04) {
    return false;
  }

  uint8_t sharedSecret[kEcdhSharedSecretSize] = {};
  if (!deriveEcdhSharedSecret(pairingPrivateKey_, challenge.publicKey, sharedSecret)) {
    return false;
  }

  uint8_t myMac[6];
  if (!localMac(myMac)) {
    secureZero(sharedSecret, sizeof(sharedSecret));
    return false;
  }
  DiscoveryV4Packet discovery;
  buildDiscovery(discovery, myMac, challenge.dialChannel);
  derivePairingSessionLmk(config_.labels.pairingSession,
                          pairingWindowLmk_,
                          sharedSecret,
                          reinterpret_cast<const uint8_t*>(&discovery),
                          sizeof(discovery),
                          reinterpret_cast<const uint8_t*>(&challenge),
                          sizeof(challenge),
                          pairingLmk_);

  memset(&pairingConfirm_, 0, sizeof(pairingConfirm_));
  pairingConfirm_.type = kPacketPairConfirm;
  pairingConfirm_.version = kPairingProtocolV4;
  pairingConfirm_.mode = kPairingModePair;
  memcpy(pairingConfirm_.mac, myMac, sizeof(pairingConfirm_.mac));
  memcpy(pairingConfirm_.sessionId, challenge.sessionId, sizeof(pairingConfirm_.sessionId));
  pairingAuthTag(pairingLmk_,
                 reinterpret_cast<const uint8_t*>(&pairingConfirm_),
                 offsetof(PairConfirmV4Packet, authTag),
                 pairingConfirm_.authTag);

  uint8_t channel = packet.channel >= 1 && packet.channel <= 14 ? packet.channel : challenge.channel;
  bool registered =
      esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK &&
      registerPeer(challenge.mac, channel, false, nullptr);
  bool sent = registered &&
              esp_now_send(challenge.mac,
                           reinterpret_cast<const uint8_t*>(&pairingConfirm_),
                           sizeof(pairingConfirm_)) == ESP_OK;
  if (sent) {
    memcpy(pairingPeerMac_, challenge.mac, sizeof(pairingPeerMac_));
    pairingPeerChannel_ = channel;
    pairingLastConfirmMs_ = millis();
    pairingAwaitResultMs_ = pairingLastConfirmMs_;
    setState(EspNowLinkState::Confirming);
  } else if (registered) {
    removePeer(challenge.mac);
  }

  secureZero(sharedSecret, sizeof(sharedSecret));
  secureZero(&discovery, sizeof(discovery));
  secureZero(&challenge, sizeof(challenge));
  return sent;
}

bool EspNowLink::handlePairResult(const RxPacket& packet) {
  if (state_ != EspNowLinkState::Confirming ||
      !macEqual(packet.src, pairingPeerMac_) ||
      packet.len != sizeof(PairResultV4Packet)) {
    return false;
  }

  PairResultV4Packet result;
  memcpy(&result, packet.data, sizeof(result));
  bool valid =
      result.type == kPacketPairResult &&
      result.version == kPairingProtocolV4 &&
      result.mode == kPairingModePair &&
      macEqual(packet.src, result.mac) &&
      constantTimeEquals(result.sessionId, pairingSessionId_, sizeof(result.sessionId)) &&
      verifyPairingAuthTag(pairingLmk_,
                           reinterpret_cast<const uint8_t*>(&result),
                           offsetof(PairResultV4Packet, authTag),
                           result.authTag);
  if (!valid) {
    secureZero(&result, sizeof(result));
    return false;
  }

  uint8_t myMac[6];
  if (!localMac(myMac)) {
    secureZero(&result, sizeof(result));
    return false;
  }
  memset(&pairingCompletePacket_, 0, sizeof(pairingCompletePacket_));
  pairingCompletePacket_.type = kPacketPairComplete;
  pairingCompletePacket_.version = kPairingProtocolV4;
  pairingCompletePacket_.mode = kPairingModePair;
  memcpy(pairingCompletePacket_.mac, myMac, sizeof(pairingCompletePacket_.mac));
  memcpy(pairingCompletePacket_.sessionId, result.sessionId, sizeof(pairingCompletePacket_.sessionId));
  pairingAuthTag(pairingLmk_,
                 reinterpret_cast<const uint8_t*>(&pairingCompletePacket_),
                 offsetof(PairCompleteV4Packet, authTag),
                 pairingCompletePacket_.authTag);
  memcpy(&pairingResult_, &result, sizeof(pairingResult_));
  pairingCompletionPending_ = true;
  pairingCompletionAttempts_ = 1;
  pairingLastCompletionMs_ = millis();
  esp_now_send(pairingPeerMac_,
               reinterpret_cast<const uint8_t*>(&pairingCompletePacket_),
               sizeof(pairingCompletePacket_));
  secureZero(&result, sizeof(result));
  return true;
}

bool EspNowLink::completePairingFromResult(const PairResultV4Packet& result) {
  uint8_t channel = result.channel >= 1 && result.channel <= 14 ? result.channel : kPairChannel;
  if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK ||
      !registerPeer(result.mac, channel, true, pairingLmk_)) {
    emit(EspNowLinkEvent::PairingFailed, result.mac);
    return false;
  }
  memcpy(peerMac_, result.mac, sizeof(peerMac_));
  memcpy(lmk_, pairingLmk_, sizeof(lmk_));
  strlcpy(peerHostname_, result.hostname, sizeof(peerHostname_));
  preferredChannel_ = channel;
  opChannel_ = channel;
  storeActiveProfile(peerMac_, lmk_, channel, peerHostname_);
  pairingComplete_ = true;
  memset(pairingPeerMac_, 0, sizeof(pairingPeerMac_));
  pairingPeerChannel_ = 0;
  clearClientPairing();
  removeBroadcastPeer();
  resetLinkSession();
  beginSynchronizing(channel);
  emit(EspNowLinkEvent::Paired, peerMac_);
  return true;
}

void EspNowLink::handleServerDiscovery(const RxPacket& packet) {
  if (!pairingWindowActive_ ||
      packet.len != sizeof(DiscoveryV4Packet)) {
    return;
  }
  DiscoveryV4Packet discovery;
  memcpy(&discovery, packet.data, sizeof(discovery));
  if (discovery.type != kPacketDiscovery ||
      discovery.version != kPairingProtocolV4 ||
      discovery.mode != kPairingModePair ||
      !macEqual(packet.src, discovery.mac) ||
      constantTimeEquals(discovery.sessionId, kZeroSession, sizeof(discovery.sessionId)) ||
      discovery.channel < 1 ||
      discovery.channel > 14 ||
      discovery.publicKey[0] != 0x04) {
    return;
  }

  if (serverPairing_.state != PairingTransaction::State::Idle) {
    bool same =
        macEqual(serverPairing_.mac, discovery.mac) &&
        constantTimeEquals(serverPairing_.sessionId, discovery.sessionId, sizeof(discovery.sessionId));
    if (same && serverPairing_.state == PairingTransaction::State::AwaitConfirm) {
      serverPairing_.lastMs = millis();
      esp_now_send(serverPairing_.mac, serverPairing_.challenge, serverPairing_.challengeLen);
    }
    return;
  }

  bool alreadyPaired = findServerPeer(discovery.mac) != nullptr;
  if (!alreadyPaired && profiles_.count >= kMaxProfiles) {
    return;
  }

  uint8_t local[6];
  if (!localMac(local)) {
    return;
  }
  PairChallengeV4Packet challenge = {};
  challenge.type = kPacketPairChallenge;
  challenge.version = kPairingProtocolV4;
  challenge.mode = kPairingModePair;
  memcpy(challenge.mac, local, sizeof(challenge.mac));
  challenge.channel = currentWifiChannel();
  if (challenge.channel < 1 || challenge.channel > 14) {
    challenge.channel = packet.channel >= 1 && packet.channel <= 14 ? packet.channel : kPairChannel;
  }
  challenge.dialChannel = discovery.channel;
  memcpy(challenge.sessionId, discovery.sessionId, sizeof(challenge.sessionId));
  strlcpy(challenge.hostname, config_.hostname, sizeof(challenge.hostname));

  uint8_t privateKey[kEcdhPrivateKeySize] = {};
  if (!generateEcdhKeypair(privateKey, challenge.publicKey)) {
    return;
  }

  uint8_t sharedSecret[kEcdhSharedSecretSize] = {};
  uint8_t windowLmk[kLmkSize] = {};
  bool derived = deriveEcdhSharedSecret(privateKey, discovery.publicKey, sharedSecret);
  if (derived) {
    derivePairingWindowLmk(config_.labels.pairingWindow, windowLmk);
    derivePairingSessionLmk(config_.labels.pairingSession,
                            windowLmk,
                            sharedSecret,
                            reinterpret_cast<const uint8_t*>(&discovery),
                            sizeof(discovery),
                            reinterpret_cast<const uint8_t*>(&challenge),
                            sizeof(challenge),
                            serverPairing_.lmk);
  }
  secureZero(privateKey, sizeof(privateKey));
  secureZero(sharedSecret, sizeof(sharedSecret));
  secureZero(windowLmk, sizeof(windowLmk));
  if (!derived) {
    return;
  }

  memcpy(serverPairing_.mac, discovery.mac, sizeof(serverPairing_.mac));
  memcpy(serverPairing_.sessionId, discovery.sessionId, sizeof(serverPairing_.sessionId));
  memcpy(serverPairing_.challenge, &challenge, sizeof(challenge));
  serverPairing_.challengeLen = sizeof(challenge);
  serverPairing_.startedMs = millis();
  serverPairing_.lastMs = serverPairing_.startedMs;
  serverPairing_.state = PairingTransaction::State::AwaitConfirm;

  registerPeer(discovery.mac, 0, false, nullptr);
  esp_now_send(discovery.mac, reinterpret_cast<const uint8_t*>(&challenge), sizeof(challenge));
  secureZero(&challenge, sizeof(challenge));
}

void EspNowLink::handleServerConfirm(const RxPacket& packet) {
  if (!pairingWindowActive_ ||
      serverPairing_.state != PairingTransaction::State::AwaitConfirm ||
      packet.len != sizeof(PairConfirmV4Packet)) {
    return;
  }
  PairConfirmV4Packet confirm;
  memcpy(&confirm, packet.data, sizeof(confirm));
  bool valid =
      confirm.type == kPacketPairConfirm &&
      confirm.version == kPairingProtocolV4 &&
      confirm.mode == kPairingModePair &&
      macEqual(packet.src, confirm.mac) &&
      macEqual(serverPairing_.mac, confirm.mac) &&
      constantTimeEquals(serverPairing_.sessionId, confirm.sessionId, sizeof(confirm.sessionId)) &&
      verifyPairingAuthTag(serverPairing_.lmk,
                           reinterpret_cast<const uint8_t*>(&confirm),
                           offsetof(PairConfirmV4Packet, authTag),
                           confirm.authTag);
  secureZero(&confirm, sizeof(confirm));
  if (!valid) {
    return;
  }

  uint8_t local[6];
  if (!localMac(local)) {
    clearServerPairing(true);
    return;
  }
  PairResultV4Packet result = {};
  result.type = kPacketPairResult;
  result.version = kPairingProtocolV4;
  result.mode = kPairingModePair;
  memcpy(result.mac, local, sizeof(result.mac));
  result.channel = currentWifiChannel();
  if (result.channel < 1 || result.channel > 14) {
    result.channel = kPairChannel;
  }
  memcpy(result.sessionId, serverPairing_.sessionId, sizeof(result.sessionId));
  strlcpy(result.hostname, config_.hostname, sizeof(result.hostname));
  pairingAuthTag(serverPairing_.lmk,
                 reinterpret_cast<const uint8_t*>(&result),
                 offsetof(PairResultV4Packet, authTag),
                 result.authTag);
  memcpy(serverPairing_.result, &result, sizeof(result));
  serverPairing_.resultLen = sizeof(result);
  serverPairing_.lastMs = millis();
  serverPairing_.attempts = 0;
  serverPairing_.state = PairingTransaction::State::ReadyResult;
  secureZero(&result, sizeof(result));
}

void EspNowLink::handleServerComplete(const RxPacket& packet) {
  if (!pairingWindowActive_ ||
      serverPairing_.state != PairingTransaction::State::AwaitComplete ||
      packet.len != sizeof(PairCompleteV4Packet)) {
    return;
  }
  PairCompleteV4Packet complete;
  memcpy(&complete, packet.data, sizeof(complete));
  bool valid =
      complete.type == kPacketPairComplete &&
      complete.version == kPairingProtocolV4 &&
      complete.mode == kPairingModePair &&
      macEqual(packet.src, complete.mac) &&
      macEqual(serverPairing_.mac, complete.mac) &&
      constantTimeEquals(serverPairing_.sessionId, complete.sessionId, sizeof(complete.sessionId)) &&
      verifyPairingAuthTag(serverPairing_.lmk,
                           reinterpret_cast<const uint8_t*>(&complete),
                           offsetof(PairCompleteV4Packet, authTag),
                           complete.authTag);
  secureZero(&complete, sizeof(complete));
  if (!valid) {
    return;
  }
  uint8_t pairedMac[6];
  memcpy(pairedMac, serverPairing_.mac, sizeof(pairedMac));
  if (activateServerPairing()) {
    pairingWindowActive_ = false;
    emit(EspNowLinkEvent::Paired, pairedMac);
  }
  clearServerPairing(false);
}

void EspNowLink::clearServerPairing(bool restorePeer) {
  uint8_t mac[6];
  memcpy(mac, serverPairing_.mac, sizeof(mac));
  secureZero(&serverPairing_, sizeof(serverPairing_));
  serverPairing_.state = PairingTransaction::State::Idle;
  if (restorePeer && macNonzero(mac)) {
    int index = -1;
    for (uint8_t i = 0; i < profiles_.count; ++i) {
      if (macEqual(mac, profiles_.profiles[i].mac)) {
        index = i;
        break;
      }
    }
    if (index >= 0) {
      registerPeer(profiles_.profiles[index].mac, profiles_.profiles[index].channel, true, profiles_.profiles[index].lmk);
    } else {
      removePeer(mac);
    }
  }
}

bool EspNowLink::activateServerPairing() {
  bool existing = findServerPeer(serverPairing_.mac) != nullptr;
  if (!existing && profiles_.count >= kMaxProfiles) {
    return false;
  }
  if (!registerPeer(serverPairing_.mac, 0, true, serverPairing_.lmk)) {
    return false;
  }
  uint8_t channel = currentWifiChannel();
  if (channel < 1 || channel > 14) {
    channel = kPairChannel;
  }
  if (!storeActiveProfile(serverPairing_.mac, serverPairing_.lmk, channel, "peripheral")) {
    if (!existing) {
      removePeer(serverPairing_.mac);
    }
    return false;
  }
  int slot = -1;
  for (uint8_t i = 0; i < profiles_.count; ++i) {
    if (macEqual(profiles_.profiles[i].mac, serverPairing_.mac)) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    if (!existing) {
      removePeer(serverPairing_.mac);
    }
    return false;
  }
  if (!ensureServerPeers()) {
    if (!existing) {
      removePeer(serverPairing_.mac);
    }
    return false;
  }
  PeerRuntime& peer = serverPeers_[slot];
  memcpy(peer.mac, serverPairing_.mac, sizeof(peer.mac));
  memcpy(peer.lmk, serverPairing_.lmk, sizeof(peer.lmk));
  peer.channel = channel;
  strlcpy(peer.hostname, "peripheral", sizeof(peer.hostname));
  resetServerPeer(peer);
  setState(EspNowLinkState::Synchronizing);
  return true;
}

void EspNowLink::handleKeepalivePacket(const RxPacket& packet) {
  int keepalive = acceptKeepalive(packet.data, packet.len);
  if (keepalive == 0) {
    return;
  }
  if (config_.role == EspNowLinkRole::Client &&
      packet.channel >= 1 &&
      packet.channel <= 14 &&
      packet.channel != opChannel_) {
    opChannel_ = packet.channel;
    esp_wifi_set_channel(opChannel_, WIFI_SECOND_CHAN_NONE);
    registerPeer(peerMac_, opChannel_, true, lmk_);
  }
  if (keepalive == 1) {
    beginSynchronizing(opChannel_);
    return;
  }
  if (state_ == EspNowLinkState::Reconnecting) {
    beginSynchronizing(opChannel_);
  } else if (state_ == EspNowLinkState::Synchronizing) {
    if (keepalive == 3 && synchronizeAuthenticatedTx_) {
      setConnectedNow();
    } else {
      synchronizeLastTxMs_ = millis();
      synchronizeAuthenticatedTx_ = sendKeepalive(peerMac_);
    }
  } else if (state_ == EspNowLinkState::Connected) {
    setConnectedNow();
  }
  if (config_.role == EspNowLinkRole::Server) {
    sendKeepalive(peerMac_);
  }
}

void EspNowLink::handleKeepalivePacket(PeerRuntime& peer, const RxPacket& packet) {
  int keepalive = acceptKeepalive(peer, packet.data, packet.len);
  if (keepalive == 0) {
    return;
  }
  if (keepalive == 1) {
    sendKeepalive(peer);
    return;
  }
  setServerPeerConnected(peer);
  sendKeepalive(peer);
}

void EspNowLink::handleRealtimePacket(const RxPacket& packet) {
  if (packet.len != 1 + kAntiReplayTagSize + 1) {
    return;
  }
  uint32_t nonce, counter;
  memcpy(&nonce, packet.data + 1, 4);
  memcpy(&counter, packet.data + 5, 4);
  if (!acceptReplay(rxNonce_, rxReplay_, nonce, counter, millis())) {
    return;
  }
  setConnectedNow();
  rxPush(packet.data[1 + kAntiReplayTagSize]);
  if (receiveCb_) {
    receiveCb_(packet.data + 1 + kAntiReplayTagSize, 1, packet.src);
  }
}

void EspNowLink::handleRealtimePacket(PeerRuntime& peer, const RxPacket& packet) {
  if (packet.len != 1 + kAntiReplayTagSize + 1) {
    return;
  }
  uint32_t nonce, counter;
  memcpy(&nonce, packet.data + 1, 4);
  memcpy(&counter, packet.data + 5, 4);
  if (!acceptReplay(peer.rxNonce, peer.rxReplay, nonce, counter, millis())) {
    return;
  }
  setServerPeerConnected(peer);
  rxPush(packet.data[1 + kAntiReplayTagSize]);
  if (receiveCb_) {
    receiveCb_(packet.data + 1 + kAntiReplayTagSize, 1, packet.src);
  }
}

void EspNowLink::handleDataPacket(const RxPacket& packet) {
  if (packet.len < (int)kFragmentHeaderSize) {
    return;
  }
  const FragmentHeader* header = reinterpret_cast<const FragmentHeader*>(packet.data);
  uint32_t nonce, counter;
  memcpy(&nonce, header->nonce, 4);
  memcpy(&counter, header->counter, 4);
  if (!acceptReplay(rxNonce_, rxReplay_, nonce, counter, millis())) {
    return;
  }

  uint8_t index = header->fragmentIndex;
  uint8_t total = header->totalFragments;
  uint8_t sequence = header->sequence;
  int payloadLen = packet.len - kFragmentHeaderSize;
  if (total == 0 || total > kMaxFragments || index >= total ||
      payloadLen < 0 || payloadLen > (int)kFragmentPayloadMax) {
    return;
  }

  setConnectedNow();
  if (frag_.pending) {
    bool staleSeq = frag_.sequence != sequence;
    bool staleTimeout = (uint32_t)(millis() - frag_.startMs) > kFragmentReassemblyTimeoutMs;
    if (staleSeq || staleTimeout) {
      frag_.pending = false;
    }
  }
  if (!frag_.pending) {
    frag_.got = 0;
    frag_.total = total;
    frag_.sequence = sequence;
    frag_.pending = true;
    frag_.startMs = millis();
    memset(frag_.len, 0, sizeof(frag_.len));
  }
  if (frag_.total != total) {
    return;
  }
  memcpy(frag_.data[index], packet.data + kFragmentHeaderSize, payloadLen);
  frag_.len[index] = payloadLen;
  frag_.got |= (uint8_t)(1u << index);
  uint8_t fullMask = total >= 8 ? 0xff : (uint8_t)((1u << total) - 1u);
  if ((frag_.got & fullMask) != fullMask) {
    return;
  }

  uint8_t assembled[kMaxFragments * kFragmentPayloadMax];
  size_t assembledLen = 0;
  for (uint8_t i = 0; i < total; ++i) {
    for (uint8_t j = 0; j < frag_.len[i]; ++j) {
      uint8_t c = frag_.data[i][j];
      if (c != '\r') {
        rxPush(c);
        if (assembledLen < sizeof(assembled)) {
          assembled[assembledLen++] = c;
        }
      }
    }
  }
  if (frag_.len[total - 1] == 0 ||
      frag_.data[total - 1][frag_.len[total - 1] - 1] != '\n') {
    rxPush('\n');
    if (assembledLen < sizeof(assembled)) {
      assembled[assembledLen++] = '\n';
    }
  }
  frag_.pending = false;
  if (receiveCb_ && assembledLen > 0) {
    receiveCb_(assembled, assembledLen, packet.src);
  }
}

void EspNowLink::handleDataPacket(PeerRuntime& peer, const RxPacket& packet) {
  if (packet.len < (int)kFragmentHeaderSize) {
    return;
  }
  const FragmentHeader* header = reinterpret_cast<const FragmentHeader*>(packet.data);
  uint32_t nonce, counter;
  memcpy(&nonce, header->nonce, 4);
  memcpy(&counter, header->counter, 4);
  if (!acceptReplay(peer.rxNonce, peer.rxReplay, nonce, counter, millis())) {
    return;
  }

  uint8_t index = header->fragmentIndex;
  uint8_t total = header->totalFragments;
  uint8_t sequence = header->sequence;
  int payloadLen = packet.len - kFragmentHeaderSize;
  if (total == 0 || total > kMaxFragments || index >= total ||
      payloadLen < 0 || payloadLen > (int)kFragmentPayloadMax) {
    return;
  }

  setServerPeerConnected(peer);
  if (peer.frag.pending) {
    bool staleSeq = peer.frag.sequence != sequence;
    bool staleTimeout = (uint32_t)(millis() - peer.frag.startMs) > kFragmentReassemblyTimeoutMs;
    if (staleSeq || staleTimeout) {
      peer.frag.pending = false;
    }
  }
  if (!peer.frag.pending) {
    peer.frag.got = 0;
    peer.frag.total = total;
    peer.frag.sequence = sequence;
    peer.frag.pending = true;
    peer.frag.startMs = millis();
    memset(peer.frag.len, 0, sizeof(peer.frag.len));
  }
  if (peer.frag.total != total) {
    return;
  }
  memcpy(peer.frag.data[index], packet.data + kFragmentHeaderSize, payloadLen);
  peer.frag.len[index] = payloadLen;
  peer.frag.got |= (uint8_t)(1u << index);
  uint8_t fullMask = total >= 8 ? 0xff : (uint8_t)((1u << total) - 1u);
  if ((peer.frag.got & fullMask) != fullMask) {
    return;
  }

  uint8_t assembled[kMaxFragments * kFragmentPayloadMax];
  size_t assembledLen = 0;
  for (uint8_t i = 0; i < total; ++i) {
    for (uint8_t j = 0; j < peer.frag.len[i]; ++j) {
      uint8_t c = peer.frag.data[i][j];
      if (c != '\r') {
        rxPush(c);
        if (assembledLen < sizeof(assembled)) {
          assembled[assembledLen++] = c;
        }
      }
    }
  }
  if (peer.frag.len[total - 1] == 0 ||
      peer.frag.data[total - 1][peer.frag.len[total - 1] - 1] != '\n') {
    rxPush('\n');
    if (assembledLen < sizeof(assembled)) {
      assembled[assembledLen++] = '\n';
    }
  }
  peer.frag.pending = false;
  if (receiveCb_ && assembledLen > 0) {
    receiveCb_(assembled, assembledLen, packet.src);
  }
}

bool EspNowLink::sendFragmentsTo(const uint8_t mac[6], const uint8_t* data, size_t len) {
  if (state_ != EspNowLinkState::Connected || !data || len == 0) {
    return false;
  }
  uint8_t total = (len + kFragmentPayloadMax - 1) / kFragmentPayloadMax;
  if (total == 0) {
    total = 1;
  }
  if (total > kMaxFragments) {
    total = kMaxFragments;
    len = kMaxFragments * kFragmentPayloadMax;
  }

  uint8_t sequence = txSequence_++;
  uint8_t packet[kMaxEspNowPayload];
  size_t offset = 0;
  bool ok = true;
  for (uint8_t i = 0; i < total; ++i) {
    size_t chunk = len - offset;
    if (chunk > kFragmentPayloadMax) {
      chunk = kFragmentPayloadMax;
    }
    packet[0] = kPacketData;
    if (!stampAntiReplayTag(txPeerKnown_, txPeerNonce_, txCounter_, packet + 1)) {
      return false;
    }
    packet[9] = sequence;
    packet[10] = i;
    packet[11] = total;
    memcpy(packet + kFragmentHeaderSize, data + offset, chunk);
    ok = esp_now_send(mac, packet, kFragmentHeaderSize + chunk) == ESP_OK && ok;
    offset += chunk;
  }
  return ok;
}

bool EspNowLink::sendFragmentsTo(PeerRuntime& peer, const uint8_t* data, size_t len) {
  if (!peer.connected || !data || len == 0) {
    return false;
  }
  uint8_t total = (len + kFragmentPayloadMax - 1) / kFragmentPayloadMax;
  if (total == 0) {
    total = 1;
  }
  if (total > kMaxFragments) {
    total = kMaxFragments;
    len = kMaxFragments * kFragmentPayloadMax;
  }

  uint8_t sequence = peer.txSequence++;
  uint8_t packet[kMaxEspNowPayload];
  size_t offset = 0;
  bool ok = true;
  for (uint8_t i = 0; i < total; ++i) {
    size_t chunk = len - offset;
    if (chunk > kFragmentPayloadMax) {
      chunk = kFragmentPayloadMax;
    }
    packet[0] = kPacketData;
    if (!stampAntiReplayTag(peer.txPeerKnown, peer.txPeerNonce, peer.txCounter, packet + 1)) {
      return false;
    }
    packet[9] = sequence;
    packet[10] = i;
    packet[11] = total;
    memcpy(packet + kFragmentHeaderSize, data + offset, chunk);
    ok = esp_now_send(peer.mac, packet, kFragmentHeaderSize + chunk) == ESP_OK && ok;
    offset += chunk;
  }
  return ok;
}

void EspNowLink::rxPush(uint8_t c) {
  int cur = rxHead_.load(std::memory_order_relaxed);
  int next = (cur + 1) % kRxBufferSize;
  if (next != rxTail_) {
    rxBuffer_[cur] = c;
    rxHead_.store(next, std::memory_order_release);
  }
}

void EspNowLink::emit(EspNowLinkEvent type, const uint8_t mac[6], uint32_t detail) {
  if (!eventCb_) {
    return;
  }
  EspNowLinkEventInfo info;
  info.type = type;
  info.state = state_;
  info.detail = detail;
  if (mac) {
    memcpy(info.mac, mac, sizeof(info.mac));
  }
  eventCb_(info);
}
