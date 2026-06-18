// Copyright 2026 Figamore (https://github.com/figamore)
// SPDX-License-Identifier: Apache-2.0

#include <Arduino.h>
#include <EspNowLink.h>
#include <string.h>

EspNowLink espnow;

void setup() {
  Serial.begin(115200);
  delay(100);

  EspNowLinkConfig cfg;
  cfg.role = EspNowLinkRole::Server;
  cfg.hostname = "espnow-server";

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

  Serial.println("Pairing window open for 60 seconds");
  espnow.openPairingWindow(60000);
}

void loop() {
  espnow.poll();

  while (Serial.available()) {
    espnow.write((uint8_t)Serial.read());
  }

  while (espnow.available()) {
    int c = espnow.read();
    Serial.write(c);

    // Tiny status response for clients that poll with '?'.
    if (c == '?') {
      const char* report = "<Idle|MPos:0.000,0.000,0.000|FS:0,0>\n";
      espnow.write((const uint8_t*)report, strlen(report));
    }
  }
}
