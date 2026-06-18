// Copyright 2026 Figamore (https://github.com/figamore)
// SPDX-License-Identifier: Apache-2.0

#include <Arduino.h>
#include <EspNowLink.h>
#include <string.h>

EspNowLink espnow;

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

  espnow.onEvent([](const EspNowLinkEventInfo& event) {
    Serial.printf("event=%u state=%u peer=%s detail=%u\n",
                  unsigned(event.type),
                  unsigned(event.state),
                  EspNowLink::formatMac(event.mac).c_str(),
                  unsigned(event.detail));
  });

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
  Serial.println("Commands: p=open pairing, c=cancel pairing, l=list peers, b=broadcast, x=clear pairings");
}

void loop() {
  espnow.poll();

  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      case 'p':
        Serial.println("Pairing window open for 60 seconds.");
        espnow.openPairingWindow(60000);
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
