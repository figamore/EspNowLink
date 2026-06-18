// Copyright 2026 Figamore (https://github.com/figamore)
// SPDX-License-Identifier: Apache-2.0

#include <Arduino.h>
#include <EspNowLink.h>

EspNowLink espnow;

void setup() {
  Serial.begin(115200);
  delay(100);

  EspNowLinkConfig cfg;
  cfg.role = EspNowLinkRole::Client;
  cfg.hostname = "espnow-client";

  espnow.onEvent([](const EspNowLinkEventInfo& event) {
    Serial.printf("event=%u state=%u peer=%s detail=%u\n",
                  unsigned(event.type),
                  unsigned(event.state),
                  EspNowLink::formatMac(event.mac).c_str(),
                  unsigned(event.detail));
  });

  if (!espnow.begin(cfg)) {
    Serial.println("EspNowLink begin failed");
    return;
  }

  if (!espnow.isPaired()) {
    Serial.println("Open a pairing window on the server, then pairing starts here.");
    espnow.startPairing();
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
