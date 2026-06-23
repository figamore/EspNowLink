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

void setup() {
  Serial.begin(115200);
  delay(100);

  EspNowLinkConfig cfg;
  cfg.role = EspNowLinkRole::Server;
  cfg.hostname = "espnow-server";

  espnow.onEvent(printEvent);

  if (!espnow.begin(cfg)) {
    Serial.println("EspNowLink begin failed");
    return;
  }

  Serial.println();
  Serial.println("EspNowLink basic server");
  Serial.println("1. Open both serial monitors at 115200 baud.");
  Serial.println("2. After pairing, type in either serial monitor; the text appears on the other.");
  Serial.println("3. Send '?' from the client to receive a tiny pong reply from this server.");
  Serial.println("Reset this board to reopen the automatic pairing window.");
  Serial.println();

  espnow.openPairingWindow(kPairingWindowMs);
}

void loop() {
  espnow.poll();

  while (Serial.available()) {
    espnow.write((uint8_t)Serial.read());
  }

  while (espnow.available()) {
    int c = espnow.read();
    Serial.write(c);

    // Tiny generic response so the client can prove round-trip communication.
    if (c == '?') {
      const char* reply = "pong from BasicServer\n";
      espnow.write((const uint8_t*)reply, strlen(reply));
    }
  }
}
