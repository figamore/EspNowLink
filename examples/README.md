# Examples

## BasicClient

Small serial bridge client. Use this on a peripheral/removable device when you
want to pair with one server and pass bytes between USB serial and ESP-NOW.

## BasicServer

Small serial bridge server. It opens a pairing window at boot and echoes received
bytes to USB serial. This is the simplest server example, but it treats incoming
client data as one merged stream.

## MultiClientServer

Server example for several remotes, sensors, or panels. It shows how to:

- open and cancel a pairing window from USB serial
- list paired peers
- receive messages with the sender MAC address
- send a targeted reply with `writeTo()`
- broadcast to all connected clients with `broadcast()`

Use this example when peer identity, permissions, or application-level ownership
matter.
