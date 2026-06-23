// Copyright 2026 Figamore (https://github.com/figamore)
// SPDX-License-Identifier: Apache-2.0

#include <Arduino.h>
#include <EspNowLink.h>

EspNowLink espnow;

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
    case EspNowLinkEvent::PairingStarted: return "Looking for server pairing window";
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
  if (event.type == EspNowLinkEvent::SendFailed) {
    Serial.printf(" (status %u)", unsigned(event.detail));
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(100);

  EspNowLinkConfig cfg;
  cfg.role = EspNowLinkRole::Client;
  cfg.hostname = "espnow-client";

  espnow.onEvent(printEvent);

  if (!espnow.begin(cfg)) {
    Serial.println("EspNowLink begin failed");
    return;
  }

  Serial.println();
  Serial.println("EspNowLink basic client");
  Serial.println("1. Open both serial monitors at 115200 baud.");
  Serial.println("2. After pairing, type in either serial monitor; the text appears on the other.");
  Serial.println("3. Send '?' here to ask the server for a tiny pong reply.");
  Serial.println();

  if (!espnow.isPaired()) {
    Serial.println("Waiting for the server pairing window. Reset the server if its window expired.");
    espnow.startPairing();
  } else {
    Serial.println("Already paired. Waiting for the server to connect.");
  }
}

void loop() {
  espnow.poll();

  while (Serial.available()) {
    espnow.write((uint8_t)Serial.read());
  }

  while (espnow.available()) {
    Serial.write(espnow.read());
  }
}
