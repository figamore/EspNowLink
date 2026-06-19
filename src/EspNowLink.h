// Copyright 2026 Figamore (https://github.com/figamore)
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_idf_version.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <functional>

#include "EspNowLinkCrypto.h"
#include "EspNowLinkProtocol.h"

enum class EspNowLinkRole : uint8_t {
  Client,
  Server,
};

enum class EspNowLinkState : uint8_t {
  Unpaired,
  Discovering,
  Confirming,
  Synchronizing,
  Connected,
  Reconnecting,
  PairingWindowOpen,
};

enum class EspNowLinkEvent : uint8_t {
  Started,
  StateChanged,
  PairingStarted,
  PairingCancelled,
  PairingWindowOpened,
  PairingWindowClosed,
  Paired,
  Connected,
  Disconnected,
  ProfileChanged,
  SendFailed,
  PairingFailed,
};

struct EspNowLinkProtocolLabels {
  const char* pairingWindow = espnow_link::kDefaultPairingWindowLabel;
  const char* pairingSession = espnow_link::kDefaultPairingSessionLabel;
  const char* pmk = espnow_link::kDefaultPmkLabel;
};

struct EspNowLinkConfig {
  EspNowLinkRole role = EspNowLinkRole::Client;
  const char* hostname = "esp32";
  const char* nvsNamespace = "espnowlink";
  wifi_interface_t interface = WIFI_IF_STA;
  bool setWifiMode = true;
  bool disconnectStaOnClientBegin = true;
  bool disableWifiSleep = true;
  EspNowLinkProtocolLabels labels;
};

struct EspNowLinkProfile {
  uint8_t mac[espnow_link::kMacSize] = {};
  uint8_t lmk[espnow_link::kLmkSize] = {};
  uint8_t channel = espnow_link::kPairChannel;
  bool active = false;
  char hostname[espnow_link::kHostnameSize] = {};
};

struct EspNowLinkEventInfo {
  EspNowLinkEvent type = EspNowLinkEvent::Started;
  EspNowLinkState state = EspNowLinkState::Unpaired;
  uint8_t mac[espnow_link::kMacSize] = {};
  uint32_t detail = 0;
};

struct EspNowLinkPeerInfo {
  uint8_t mac[espnow_link::kMacSize] = {};
  uint8_t channel = espnow_link::kPairChannel;
  bool connected = false;
  int8_t rssi = -100;
  uint32_t lastRxMs = 0;
  char hostname[espnow_link::kHostnameSize] = {};
};

class EspNowLink {
 public:
  using EventCallback = std::function<void(const EspNowLinkEventInfo&)>;
  using ReceiveCallback = std::function<void(const uint8_t* data, size_t len, const uint8_t mac[6])>;

  static constexpr size_t kMaxProfiles = 5;

  EspNowLink();
  ~EspNowLink();
  EspNowLink(const EspNowLink&) = delete;
  EspNowLink& operator=(const EspNowLink&) = delete;

  bool begin(const EspNowLinkConfig& config = {});
  void end();
  void poll();

  bool startPairing(uint32_t timeoutMs = 0);
  void cancelPairing();
  bool openPairingWindow(uint32_t timeoutMs = 60000);
  void cancelPairingWindow();

  size_t write(uint8_t c);
  size_t write(const uint8_t* data, size_t len);
  bool writeTo(const uint8_t mac[6], const uint8_t* data, size_t len);
  bool broadcast(const uint8_t* data, size_t len);
  int read();
  int available() const;
  bool sendRealtime(uint8_t c);
  bool sendRealtimeTo(const uint8_t mac[6], uint8_t c);

  bool isPaired() const;
  bool isConnected() const;
  bool isPairing() const;
  EspNowLinkState state() const;
  const char* stateName() const;
  int8_t rssi() const;

  size_t profileCount() const;
  int activeProfileIndex() const;
  bool getProfile(size_t index, EspNowLinkProfile& out) const;
  bool selectProfile(size_t index);
  bool removeProfile(size_t index);
  void clearProfiles();
  size_t peerCount() const;
  bool getPeer(size_t index, EspNowLinkPeerInfo& out) const;

  void onEvent(EventCallback cb);
  void onReceive(ReceiveCallback cb);

  static String formatMac(const uint8_t mac[6]);

 private:
  struct StoredProfile {
    uint8_t version;
    uint8_t mac[6];
    uint8_t lmk[16];
    uint8_t channel;
    char hostname[espnow_link::kHostnameSize];
  } __attribute__((packed));

  struct StoredProfileList {
    uint8_t version;
    uint8_t active;
    uint8_t count;
    uint8_t reserved;
    StoredProfile profiles[kMaxProfiles];
  } __attribute__((packed));

  struct RxPacket {
    uint8_t src[6] = {};
    uint16_t len = 0;
    uint8_t channel = 0;
    int8_t rssi = -100;
    uint8_t data[espnow_link::kMaxEspNowPayload] = {};
  };

  struct FragState {
    uint8_t data[espnow_link::kMaxFragments][espnow_link::kFragmentPayloadMax] = {};
    uint8_t len[espnow_link::kMaxFragments] = {};
    uint8_t got = 0;
    uint8_t total = 0;
    uint8_t sequence = 0;
    bool pending = false;
    uint32_t startMs = 0;
  };

  struct PairingTransaction {
    enum class State : uint8_t {
      Idle,
      AwaitConfirm,
      ReadyResult,
      AwaitComplete,
    };

    State state = State::Idle;
    uint8_t mac[6] = {};
    uint8_t sessionId[espnow_link::kSessionIdSize] = {};
    uint8_t lmk[espnow_link::kLmkSize] = {};
    uint8_t challenge[espnow_link::kMaxEspNowPayload] = {};
    uint16_t challengeLen = 0;
    uint8_t result[espnow_link::kMaxEspNowPayload] = {};
    uint16_t resultLen = 0;
    uint8_t attempts = 0;
    uint32_t startedMs = 0;
    uint32_t lastMs = 0;
  };

  struct PeerRuntime {
    uint8_t mac[6] = {};
    uint8_t lmk[16] = {};
    uint8_t channel = espnow_link::kPairChannel;
    char hostname[espnow_link::kHostnameSize] = {};
    std::atomic<uint32_t> rxNonce{0};
    std::atomic<uint32_t> txPeerNonce{0};
    std::atomic<bool> txPeerKnown{false};
    std::atomic<uint32_t> txCounter{0};
    espnow_link::ReplayState rxReplay;
    FragState frag;
    uint8_t txSequence = 0;
    uint32_t lastRxMs = 0;
    int8_t rssi = -100;
    bool connected = false;
  };

  static constexpr uint8_t kProfileStoreVersion = 2;
  static constexpr size_t kRxBufferSize = 2048;
  static constexpr size_t kTxBufferSize = 512;
  static constexpr size_t kPacketQueueSize = 24;

  static EspNowLink* instance_;

#if ESP_IDF_VERSION_MAJOR >= 5
  static void onRecvStatic(const esp_now_recv_info_t* info, const uint8_t* data, int len);
#else
  static void onRecvStatic(const uint8_t* mac, const uint8_t* data, int len);
#endif
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
  static void onSentStatic(const esp_now_send_info_t* info, esp_now_send_status_t status);
#else
  static void onSentStatic(const uint8_t* mac, esp_now_send_status_t status);
#endif

  void handleRecv(const uint8_t* src, const uint8_t* data, int len, uint8_t channel, int8_t rssi);
  bool queuePacket(const uint8_t* src, const uint8_t* data, int len, uint8_t channel, int8_t rssi);
  bool popPacket(RxPacket& packet);
  void processPacket(const RxPacket& packet);
  void pollClient(uint32_t now);
  void pollServer(uint32_t now);

  bool loadProfiles();
  bool saveProfiles();
  bool validProfile(const StoredProfile& profile) const;
  bool hasActiveProfile() const;
  bool storeActiveProfile(const uint8_t mac[6], const uint8_t lmk[16], uint8_t channel, const char* hostname);
  void loadActiveIntoRuntime();
  bool ensureServerPeers();
  void loadServerPeersIntoRuntime();
  PeerRuntime* findServerPeer(const uint8_t mac[6]);
  const PeerRuntime* findServerPeer(const uint8_t mac[6]) const;
  void resetServerPeer(PeerRuntime& peer);

  bool setState(EspNowLinkState state);
  bool registerPeer(const uint8_t mac[6], uint8_t channel, bool encrypted, const uint8_t lmk[16]);
  void removePeer(const uint8_t mac[6]);
  void removeBroadcastPeer();
  bool addBroadcastPeer(uint8_t channel);
  uint8_t currentWifiChannel() const;
  bool localMac(uint8_t mac[6]) const;
  wifi_interface_t interface() const;

  void resetLinkSession();
  void beginReconnecting();
  void beginSynchronizing(uint8_t channel);
  bool sendKeepalive(const uint8_t mac[6]);
  bool sendKeepalive(PeerRuntime& peer);
  int acceptKeepalive(const uint8_t* data, int len);
  int acceptKeepalive(PeerRuntime& peer, const uint8_t* data, int len);
  void setConnectedNow();
  void setServerPeerConnected(PeerRuntime& peer);

  void newPairingSessionId();
  void buildDiscovery(espnow_link::DiscoveryV4Packet& packet, const uint8_t mac[6], uint8_t channel) const;
  void clearClientPairing();
  bool handlePairChallenge(const RxPacket& packet);
  bool handlePairResult(const RxPacket& packet);
  bool completePairingFromResult(const espnow_link::PairResultV4Packet& result);

  void handleServerDiscovery(const RxPacket& packet);
  void handleServerConfirm(const RxPacket& packet);
  void handleServerComplete(const RxPacket& packet);
  void clearServerPairing(bool restorePeer);
  bool activateServerPairing();

  void handleKeepalivePacket(const RxPacket& packet);
  void handleKeepalivePacket(PeerRuntime& peer, const RxPacket& packet);
  void handleRealtimePacket(const RxPacket& packet);
  void handleRealtimePacket(PeerRuntime& peer, const RxPacket& packet);
  void handleDataPacket(const RxPacket& packet);
  void handleDataPacket(PeerRuntime& peer, const RxPacket& packet);
  bool sendFragmentsTo(const uint8_t mac[6], const uint8_t* data, size_t len);
  bool sendFragmentsTo(PeerRuntime& peer, const uint8_t* data, size_t len);
  void rxPush(uint8_t c);
  void emit(EspNowLinkEvent type, const uint8_t mac[6] = nullptr, uint32_t detail = 0);

  EspNowLinkConfig config_;
  EventCallback eventCb_;
  ReceiveCallback receiveCb_;
  EspNowLinkState state_ = EspNowLinkState::Unpaired;
  uint32_t stateSinceMs_ = 0;
  bool started_ = false;
  bool pairingComplete_ = false;

  StoredProfileList profiles_ = {};
  PeerRuntime* serverPeers_ = nullptr;
  uint8_t peerMac_[6] = {};
  uint8_t lmk_[16] = {};
  uint8_t opChannel_ = espnow_link::kPairChannel;
  uint8_t preferredChannel_ = espnow_link::kPairChannel;
  char peerHostname_[espnow_link::kHostnameSize] = {};
  int8_t peerRssi_ = -100;
  uint32_t lastRxMs_ = 0;
  uint32_t keepaliveLastMs_ = 0;
  uint32_t reconnectLastMs_ = 0;
  uint8_t reconnectProbeIndex_ = 0;
  uint8_t reconnectSavedChannel_ = espnow_link::kPairChannel;
  uint32_t synchronizeLastTxMs_ = 0;
  bool synchronizeAuthenticatedTx_ = false;

  std::atomic<uint32_t> rxNonce_{0};
  std::atomic<uint32_t> txPeerNonce_{0};
  std::atomic<bool> txPeerKnown_{false};
  std::atomic<uint32_t> txCounter_{0};
  espnow_link::ReplayState rxReplay_;
  FragState frag_;

  uint8_t rxBuffer_[kRxBufferSize] = {};
  std::atomic<int> rxHead_{0};
  int rxTail_ = 0;
  uint8_t txBuffer_[kTxBufferSize] = {};
  size_t txLen_ = 0;
  uint8_t txSequence_ = 0;

  QueueHandle_t packetQueue_ = nullptr;

  uint8_t pairingWindowLmk_[espnow_link::kLmkSize] = {};
  uint8_t pairingLmk_[espnow_link::kLmkSize] = {};
  uint8_t pairingPrivateKey_[espnow_link::kEcdhPrivateKeySize] = {};
  uint8_t pairingPublicKey_[espnow_link::kEcdhPublicKeySize] = {};
  uint8_t pairingSessionId_[espnow_link::kSessionIdSize] = {};
  uint8_t pairingPeerMac_[6] = {};
  uint8_t pairingPeerChannel_ = 0;
  bool pairingKeypairValid_ = false;
  uint32_t pairingStartMs_ = 0;
  uint32_t pairingTimeoutMs_ = 0;
  uint32_t beaconLastMs_ = 0;
  uint8_t probeIndex_ = 0;
  uint32_t pairingAwaitResultMs_ = 0;
  uint32_t pairingLastConfirmMs_ = 0;
  bool pairingCompletionPending_ = false;
  uint8_t pairingCompletionAttempts_ = 0;
  uint32_t pairingLastCompletionMs_ = 0;
  espnow_link::PairConfirmV4Packet pairingConfirm_ = {};
  espnow_link::PairCompleteV4Packet pairingCompletePacket_ = {};
  espnow_link::PairResultV4Packet pairingResult_ = {};

  bool pairingWindowActive_ = false;
  uint32_t pairingWindowUntilMs_ = 0;
  PairingTransaction serverPairing_;
};
