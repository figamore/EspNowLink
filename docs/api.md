# EspNowLink Usage Guide

EspNowLink gives ESP32 projects a paired, encrypted ESP-NOW link that feels
like a serial port after the two devices have bonded. The same library is used
on both sides; choose `Client` for the peripheral/display/control device and
`Server` for the controller/device that accepts pairings.


## Model

- A server opens a temporary pairing window.
- A client starts pairing and discovers servers during that window.
- Pairing creates encrypted peer credentials, stores them in NVS, and records
  the peer MAC address, hostname, and WiFi channel.
- After pairing, the link reconnects automatically and exposes serial-style
  `read()` / `write()` APIs.
- A client can store up to five profiles, so one device can be paired with
  several servers and switch between them.
- A server can store and communicate with multiple paired clients at the same
  time. The application decides what each client is allowed to do.

## Basic Requirements

- ESP32 with the Arduino ESP32 core.
- `#include <EspNowLink.h>`.
- Call `begin()` once from `setup()`.
- Call `poll()` frequently from `loop()`.
- Avoid long blocking delays in `loop()`. Pairing, reconnect, keepalive, packet
  reassembly, and callbacks are all driven by `poll()`.

## Minimal Client

A client is the side that initiates discovery and usually behaves like a
peripheral, panel, sensor, display, or remote control.

```cpp
#include <Arduino.h>
#include <EspNowLink.h>

EspNowLink radio;

void setup() {
  Serial.begin(115200);

  EspNowLinkConfig cfg;
  cfg.role = EspNowLinkRole::Client;
  cfg.hostname = "panel-1";

  radio.onEvent([](const EspNowLinkEventInfo& event) {
    Serial.printf("event=%u state=%s peer=%s detail=%u\n",
                  unsigned(event.type),
                  radio.stateName(),
                  EspNowLink::formatMac(event.mac).c_str(),
                  unsigned(event.detail));
  });

  if (!radio.begin(cfg)) {
    Serial.println("EspNowLink failed to start");
    return;
  }

  if (!radio.isPaired()) {
    // Client pairing stays active until paired or cancelled.
    radio.startPairing();
  }
}

void loop() {
  radio.poll();

  while (Serial.available()) {
    radio.write((uint8_t)Serial.read());
  }

  while (radio.available()) {
    Serial.write(radio.read());
  }
}
```

## Minimal Server

A server is the side that accepts pairings. Open the pairing window only when the
user intentionally asks for it, such as from a button, command, or UI action.

```cpp
#include <Arduino.h>
#include <EspNowLink.h>

EspNowLink radio;

void setup() {
  Serial.begin(115200);

  EspNowLinkConfig cfg;
  cfg.role = EspNowLinkRole::Server;
  cfg.hostname = "controller-1";

  if (!radio.begin(cfg)) {
    Serial.println("EspNowLink failed to start");
    return;
  }
}

void loop() {
  radio.poll();

  // Example: replace this with a real button, terminal command, or web action.
  if (Serial.read() == 'p') {
    radio.openPairingWindow(60000); // 60 seconds
  }

  while (radio.available()) {
    int c = radio.read();
    Serial.write(c);
  }
}
```

## Pairing Workflow

Use this sequence for a normal first-time pairing:

1. Start the server and call `openPairingWindow(60000)`.
2. Start the client and call `startPairing()`.
3. Wait for `EspNowLinkEvent::Paired` and then `EspNowLinkEvent::Connected`.
4. Stop showing pairing UI once connected.

Client pairing can be left open until success:

```cpp
radio.startPairing();     // no timeout
radio.cancelPairing();    // user cancelled
```

Server pairing should usually have a timeout:

```cpp
radio.openPairingWindow(60000);
radio.cancelPairingWindow();
```

Pairing data is stored in the NVS namespace from `EspNowLinkConfig`, so it
survives normal reflashes that do not erase NVS.

## Sending and Receiving

Once `isConnected()` is true, you can send bytes with `write()`:

```cpp
radio.write((const uint8_t*)"hello\n", 6);
```

There are two receive styles. They are intentionally both available, but they
are not equivalent:

| API | Best for | Peer identity |
| --- | --- | --- |
| `available()` / `read()` | Simple Arduino-style serial bridges and one-peer client sketches. | Not preserved; server data from multiple clients is merged into one byte stream. |
| `onReceive()` | Real applications, binary/message-oriented protocols, and multi-client servers. | Preserved; callback includes the sender MAC address. |

Use the Stream-style API when you want something that feels like `Serial`:

```cpp
while (radio.available()) {
  int c = radio.read();
  if (c >= 0) {
    Serial.write(c);
  }
}
```

Use the callback API when the sender matters:

```cpp
radio.onReceive([](const uint8_t* data, size_t len, const uint8_t mac[6]) {
  Serial.printf("%u bytes from %s\n", unsigned(len),
                EspNowLink::formatMac(mac).c_str());
});
```

In server mode with multiple clients, prefer `onReceive()` and use `writeTo()`
when replying to a specific client.

`write(uint8_t)` is optimized for serial-style traffic:

- Realtime bytes such as `?`, `!`, `~` are sent as small
  realtime packets when possible.
- Normal bytes are buffered and sent as data packets.
- Long writes are fragmented and reassembled automatically.

Keep callbacks short. If your application needs heavy parsing, copy the data or
set a flag and process it from `loop()`.

## Multi-Client Servers

In server mode, `EspNowLink` keeps independent encrypted runtime state for
each paired client: reconnect/keepalive state, replay protection, fragment
reassembly, transmit counters, RSSI, and last receive time. This lets a single
controller, hub, or appliance talk to several remotes or sensors at once.

The library does not decide ownership or permissions. For example, an HVAC
controller might accept temperature updates from every room sensor, but only
accept setpoint changes from selected remotes. That policy belongs in your
application.

Use `onReceive()` to identify which client sent a message:

```cpp
radio.onReceive([](const uint8_t* data, size_t len, const uint8_t mac[6]) {
  Serial.printf("rx from %s: %u bytes\n",
                EspNowLink::formatMac(mac).c_str(),
                unsigned(len));

  // Your application decides whether this peer may control anything.
});
```

Reply to one peer:

```cpp
radio.writeTo(mac, (const uint8_t*)"ok\n", 3);
radio.sendRealtimeTo(mac, '?');
```

Send to every currently connected peer:

```cpp
radio.broadcast((const uint8_t*)"system online\n", 14);
radio.sendRealtime('?');
```

Inspect paired peers:

```cpp
for (size_t i = 0; i < radio.peerCount(); ++i) {
  EspNowLinkPeerInfo peer;
  if (radio.getPeer(i, peer)) {
    Serial.printf("%s connected=%u rssi=%d\n",
                  EspNowLink::formatMac(peer.mac).c_str(),
                  peer.connected,
                  peer.rssi);
  }
}
```

`read()` / `available()` still expose a merged byte stream for simple serial
bridge sketches. Use `onReceive()` plus `writeTo()` when peer identity matters.

## Realtime Packets

Use `sendRealtime()` when a single byte must bypass normal data buffering:

Examples:
```cpp
radio.sendRealtime('?');
radio.sendRealtime('!');
radio.sendRealtime('~');
```

`write(uint8_t)` already routes common realtime bytes this way when they are not
part of a larger buffered line, so most applications can just use `write()`.

## Profiles

Clients can store up to `EspNowLink::kMaxProfiles` paired servers. This is for
one handheld/display/control device that can connect to multiple machines,
controllers, or sensor hubs.

```cpp
Serial.printf("profiles=%u active=%d\n",
              unsigned(radio.profileCount()),
              radio.activeProfileIndex());

for (size_t i = 0; i < radio.profileCount(); ++i) {
  EspNowLinkProfile profile;
  if (radio.getProfile(i, profile)) {
    Serial.printf("%u: %s %s ch=%u%s\n",
                  unsigned(i),
                  profile.hostname,
                  EspNowLink::formatMac(profile.mac).c_str(),
                  unsigned(profile.channel),
                  profile.active ? " active" : "");
  }
}

radio.selectProfile(0);   // switch active server
radio.removeProfile(0);   // forget one server
radio.clearProfiles();    // forget all servers
```

Servers store accepted clients in the same profile list. A server application
can list peers with `peerCount()` / `getPeer()`, remove one with
`removeProfile(index)`, or clear all pairings with `clearProfiles()`.

## Events

Register `onEvent()` before `begin()` if you want startup and pairing events.
The `detail` field is event-specific and may contain a timeout, failure code, or
other numeric diagnostic.

| Event | Meaning |
| --- | --- |
| `Started` | ESP-NOW and storage initialized. |
| `StateChanged` | State machine moved to a new state. |
| `PairingStarted` | Client pairing started. |
| `PairingCancelled` | Pairing was cancelled locally. |
| `PairingWindowOpened` | Server opened its pairing window. |
| `PairingWindowClosed` | Server pairing window closed or timed out. |
| `Paired` | Pairing completed and profile/key data was saved. |
| `Connected` | Keepalive synchronization completed; serial traffic may flow. |
| `Disconnected` | Link timed out or peer stopped responding. |
| `ProfileChanged` | Active client profile changed. |
| `SendFailed` | ESP-NOW send failed. |
| `PairingFailed` | Pairing failed authentication, validation, or delivery. |

## States

| State | Meaning |
| --- | --- |
| `Unpaired` | No usable active profile is available. |
| `Discovering` | Client is broadcasting discovery packets. |
| `Confirming` | Client is processing pairing challenge/result frames. |
| `Synchronizing` | Paired peer is known; keepalive sync is in progress. |
| `Connected` | Link is authenticated, synchronized, and usable. |
| `Reconnecting` | A saved profile exists and the client is probing channels. |
| `PairingWindowOpen` | Server is accepting pairing requests. |

The helper `stateName()` returns a printable string for the current state.

## Configuration Reference

```cpp
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
```

| Field | Use |
| --- | --- |
| `role` | Selects client or server behavior. |
| `hostname` | Human-readable device name sent during pairing and stored in profiles. |
| `nvsNamespace` | NVS namespace used for saved profiles and keys. Change this if your app needs isolated storage. |
| `interface` | WiFi interface used for ESP-NOW, usually `WIFI_IF_STA`. |
| `setWifiMode` | When true, the library configures WiFi mode during `begin()`. Set false only if your app owns WiFi setup. |
| `disconnectStaOnClientBegin` | Client-only helper that disconnects STA WiFi so ESP-NOW can control channel behavior. |
| `disableWifiSleep` | Disables WiFi sleep for lower latency and more reliable keepalives. |
| `labels` | Protocol domain labels used during pairing and ESP-NOW key setup. Leave defaults for new EspNowLink projects; override only when both sides intentionally share another compatible protocol. |

## Protocol Labels

EspNowLink uses protocol labels to separate one wire protocol from another. They
are not user passwords, but both devices must use the same labels or pairing and
saved-peer communication will fail authentication. The default labels are for new
EspNowLink projects:

```cpp
EspNowLinkConfig cfg;
cfg.labels.pairingWindow = "espnowlink-pairing-v1";
cfg.labels.pairingSession = "espnowlink-session-v1";
cfg.labels.pmk = "espnowlink-pmk-v1";
```

Only override these for compatibility with an existing product or fork, and do it
before calling `begin()`.

## WiFi Mode and Channels

ESP-NOW uses the current WiFi radio channel. If your application also connects to
an access point, the AP controls the channel. If there is no router, the library
can use its own channel probing and saved profile channel.

Important rules:

- Keep `poll()` running while reconnecting; reconnect probing is not automatic
  without it.
- Pairings are tied to the WiFi interface MAC address. ESP32 STA and AP
  interfaces have different MAC addresses, so pairings made on one interface do
  not automatically apply to the other.
- If your application manages WiFi itself, set `setWifiMode = false` and make
  sure the selected `interface` and channel are stable before calling `begin()`.
- For lowest latency, leave `disableWifiSleep = true` unless you have a strong
  power-saving reason.

## Security Overview

Pairing is temporary, user-initiated, and creates encrypted per-peer ESP-NOW
credentials. Connected traffic uses encrypted ESP-NOW peers and replay protection
for realtime, keepalive, and data packets. The public API intentionally documents
how to use the transport without exposing implementation internals.

## Common Patterns

### Periodic status polling

```cpp
static uint32_t lastPoll = 0;

void loop() {
  radio.poll();

  if (radio.isConnected() && millis() - lastPoll >= 250) {
    lastPoll = millis();
    radio.sendRealtime('?');
  }
}
```

### Pairing button on a server

```cpp
if (pairButtonPressed()) {
  radio.openPairingWindow(60000);
}

if (cancelButtonPressed()) {
  radio.cancelPairingWindow();
}
```

### Server with several remotes

```cpp
radio.onReceive([](const uint8_t* data, size_t len, const uint8_t mac[6]) {
  if (remoteMayControl(mac, data, len)) {
    applyRemoteCommand(mac, data, len);
    radio.writeTo(mac, (const uint8_t*)"ok\n", 3);
  } else {
    radio.writeTo(mac, (const uint8_t*)"denied\n", 7);
  }
});

radio.broadcast((const uint8_t*)"state changed\n", 14);
```

### Pairing screen on a client

```cpp
if (!radio.isPaired() && userTappedPair()) {
  radio.startPairing();
}

if (userTappedCancel()) {
  radio.cancelPairing();
}

if (radio.isConnected()) {
  showMainScreen();
}
```

## Troubleshooting

| Symptom | Things to check |
| --- | --- |
| Client never pairs | Server pairing window is open, both devices are powered, `poll()` is running, and the client has not been left on an old incompatible profile. |
| Pairing works but reconnect does not | The server is on the same WiFi mode/interface as when paired, NVS was not erased, and the client profile still exists. |
| Connected but no data | Confirm `isConnected()`, call `poll()` often, and make sure the sender includes line endings if the receiver expects line-oriented data. |
| Choppy or delayed commands | Avoid long blocking work in `loop()`, keep WiFi sleep disabled, and use `sendRealtime()` for urgent single-byte controls. |
| Multiple clients conflict | The library delivers peer identity through `onReceive()`; implement ownership or permissions in your application. |
| Multiple servers conflict | Give each server a unique hostname and use client profiles to select the intended peer. |
| After reflashing, pairings vanished | The upload erased NVS or the `nvsNamespace` changed. Normal firmware upload should not erase the NVS partition. |
