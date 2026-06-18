# EspNowLink

EspNowLink is an Arduino and PlatformIO library for encrypted, paired
ESP-NOW serial-style links on ESP32.

The library currently exposes one role-based API, `EspNowLink`, that can be
initialized as a client/peripheral or server/controller. It is intended for
general ESP32 projects that need a small paired wireless transport without
requiring a router.

**Start here:** [`docs/api.md`](docs/api.md) covers setup, pairing, profiles,
events, serial transport, WiFi/channel notes, and troubleshooting.

## Features

- Temporary pairing-window workflow.
- Unified client/server initialization.
- Client profiles stored in ESP32 NVS.
- Encrypted pairing with per-peer ESP-NOW keys.
- Hardware ESP-NOW peer encryption.
- Keepalive synchronization and reconnect channel probing.
- Replay protection for realtime, keepalive, and data packets.
- Serial-like `read()` / `write()` after connection.
- Multi-profile client support.
- Multi-client server support with application-owned control policy.
- Arduino Library Manager and PlatformIO compatible layout.

## Protocol Labels

New projects use the default protocol labels built into EspNowLink. If you need
wire compatibility with an existing product or fork, both sides must use the same
labels before pairing. Set them in `EspNowLinkConfig::labels` before `begin()`.

## Security Model

Pairing is intentionally temporary and user-initiated. After a device is paired,
traffic is sent through encrypted ESP-NOW peers with per-peer keys, and session
traffic includes replay protection for realtime, keepalive, and data packets.

## Client

```cpp
#include <EspNowLink.h>

EspNowLink espnow;

void setup() {
  Serial.begin(115200);

  EspNowLinkConfig cfg;
  cfg.role = EspNowLinkRole::Client;
  cfg.hostname = "sensor-panel";

  espnow.onEvent([](const EspNowLinkEventInfo& event) {
    Serial.printf("event=%u peer=%s\n",
                  unsigned(event.type),
                  EspNowLink::formatMac(event.mac).c_str());
  });

  espnow.begin(cfg);
  if (!espnow.isPaired()) {
    espnow.startPairing();
  }
}

void loop() {
  espnow.poll();

  static uint32_t last = 0;
  if (espnow.isConnected() && millis() - last > 250) {
    last = millis();
    espnow.write('?');
  }

  while (espnow.available()) {
    Serial.write(espnow.read());
  }
}
```

## Server

```cpp
#include <EspNowLink.h>

EspNowLink espnow;

void setup() {
  Serial.begin(115200);

  EspNowLinkConfig cfg;
  cfg.role = EspNowLinkRole::Server;
  cfg.hostname = "espnow-server";

  espnow.begin(cfg);
  espnow.openPairingWindow(60000);
}

void loop() {
  espnow.poll();
}
```

Server applications should usually receive with `onReceive()` so they know which
paired client sent the message:

```cpp
espnow.onReceive([](const uint8_t* data, size_t len, const uint8_t mac[6]) {
  // The application decides what this peer is allowed to do.
  espnow.writeTo(mac, data, len);
});
```

The `available()` / `read()` Stream-style API is still useful for simple serial
bridges, but on a multi-client server it exposes a merged byte stream without
peer identity.

## Examples

- `examples/BasicClient` - Serial bridge client that starts pairing and exposes
  the link through the USB serial monitor.
- `examples/BasicServer` - Serial bridge server with a temporary pairing window.
- `examples/MultiClientServer` - Server that pairs multiple clients, lists
  peers, sends targeted replies, and broadcasts to connected clients.

## Documentation

- [`docs/api.md`](docs/api.md) - Full usage guide and API reference.

## License

EspNowLink is licensed under the Apache License 2.0. See [`LICENSE`](LICENSE)
and [`NOTICE`](NOTICE).
