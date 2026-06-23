// Copyright 2026 Figamore (https://github.com/figamore)
// SPDX-License-Identifier: Apache-2.0

#include <Arduino.h>
#include <EspNowLink.h>
#include <string.h>

EspNowLink espnow;

constexpr uint32_t kPairingWindowMs = 5UL * 60UL * 1000UL;

bool hasPeerMac(const uint8_t mac[6]) {
  for (uint8_t i = 0; i < 6; ++i) {
    if (mac[i] != 0) {
      return true;
    }
  }
  return false;
}

const char* eventName(EspNowLinkEvent type) {
  switch (type) {
    case EspNowLinkEvent::PairingWindowOpened: return "Pairing window opened";
    case EspNowLinkEvent::PairingWindowClosed: return "Pairing window closed";
    case EspNowLinkEvent::Paired: return "Paired";
    case EspNowLinkEvent::Connected: return "Connected";
    case EspNowLinkEvent::Disconnected: return "Disconnected";
    case EspNowLinkEvent::ProfileChanged: return "Stored pairings changed";
    case EspNowLinkEvent::SendFailed: return "Send failed";
    case EspNowLinkEvent::PairingFailed: return "Pairing failed";
    default: return nullptr;
  }
}

void printEvent(const EspNowLinkEventInfo& event) {
  const char* name = eventName(event.type);
  if (!name) {
    return;
  }

  Serial.print(name);
  if (hasPeerMac(event.mac)) {
    Serial.print(": ");
    Serial.print(EspNowLink::formatMac(event.mac));
  }
  if (event.type == EspNowLinkEvent::PairingWindowOpened) {
    Serial.printf(" for %u seconds", unsigned(event.detail / 1000));
  } else if (event.type == EspNowLinkEvent::SendFailed) {
    Serial.printf(" (status %u)", unsigned(event.detail));
  }
  Serial.println();
}

void printPeer(const EspNowLinkPeerInfo& peer, size_t index) {
  Serial.printf("%u: %s connected=%u rssi=%d host=%s\n",
                unsigned(index),
                EspNowLink::formatMac(peer.mac).c_str(),
                peer.connected ? 1 : 0,
                int(peer.rssi),
                peer.hostname);
}

void printPeers() {
  size_t count = espnow.peerCount();
  Serial.printf("paired peers: %u\n", unsigned(count));
  for (size_t i = 0; i < count; ++i) {
    EspNowLinkPeerInfo peer;
    if (espnow.getPeer(i, peer)) {
      printPeer(peer, i);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

  EspNowLinkConfig cfg;
  cfg.role = EspNowLinkRole::Server;
  cfg.hostname = "hvac-controller";

  espnow.onEvent(printEvent);

  espnow.onReceive([](const uint8_t* data, size_t len, const uint8_t mac[6]) {
    Serial.printf("rx %u bytes from %s: ", unsigned(len),
                  EspNowLink::formatMac(mac).c_str());
    Serial.write(data, len);
    Serial.println();

    // This is where the application decides what this remote is allowed to do.
    // For this demo, every paired remote gets a simple targeted acknowledgement.
    const char* ack = "ack\n";
    espnow.writeTo(mac, reinterpret_cast<const uint8_t*>(ack), strlen(ack));
  });

  if (!espnow.begin(cfg)) {
    Serial.println("EspNowLink begin failed");
    return;
  }

  Serial.println("Multi-client server ready.");
  Serial.println("Commands: p=open pairing for 5 minutes, c=cancel pairing, l=list peers, b=broadcast, x=clear pairings");
}

void loop() {
  espnow.poll();

  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      case 'p':
        espnow.openPairingWindow(kPairingWindowMs);
        break;
      case 'c':
        Serial.println("Pairing cancelled.");
        espnow.cancelPairingWindow();
        break;
      case 'l':
        printPeers();
        break;
      case 'b': {
        const char* msg = "server broadcast\n";
        bool sent = espnow.broadcast(reinterpret_cast<const uint8_t*>(msg), strlen(msg));
        Serial.printf("broadcast %s\n", sent ? "sent" : "had no connected peers");
        break;
      }
      case 'x':
        Serial.println("Clearing all pairings.");
        espnow.clearProfiles();
        break;
      default:
        break;
    }
  }
}
