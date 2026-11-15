# ESP-Sim: an Omnipod DASH and Omnipod 5 pod, and Dexcom G6 and G7 CGM, simulator on the ESP32-S3

> **Test equipment only.** ESP-Sim pretends to be an insulin pod, and
> optionally a CGM, so that controller apps (Loop, Trio and others using
> OmnipodKit, MedtrumKit, CGMBLEKit and G7SensorKit) can be developed and
> tested without real devices. It delivers no insulin, its glucose readings
> are made up, and nothing it reports reflects a real device. Never use it —
> or an app paired to it — for anyone's therapy. **Never let a phone that
> controls a real pump or pod read the simulated CGM:** the app would dose
> insulin from made-up numbers. This is not medical advice; talk to your care
> team about your own treatment.

This is a C port of the Go DASH simulator
([jeremybarnum/pod](https://github.com/jeremybarnum/pod) at
`a491a5e`, itself from [LoopKit/pod](https://github.com/loopkit/pod) and
0pen-dash) from a Raspberry Pi to an ESP32. It speaks the same Bluetooth
protocol — pairing, EAP-AKA sessions, encrypted pod commands — and serves the
same control panel, from a board that needs no Pi, no Linux and no Go.

It can also be an Omnipod 5 pod or a Medtrum patch, and at the same time a
Dexcom G6 or G7: see [Omnipod 5](#omnipod-5), [Medtrum patch](#medtrum-patch)
and [Dexcom G6 and G7](#dexcom-g6-and-g7) below.

## Hardware

Any ESP32-S3 board with 8 MB of flash (developed on an ESP32-S3 QFN56 rev
v0.2 with 8 MB flash and 8 MB PSRAM, over its built-in USB serial/JTAG port).

## Build and flash

You need [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32s3/get-started/)
(this was built with v5.5.6). On macOS, ESP-IDF's installer leaves out CMake
and Ninja: add them with `python $IDF_PATH/tools/idf_tools.py install cmake ninja`.

```bash
. ~/esp/esp-idf/export.sh
```

```bash
idf.py set-target esp32s3
```

```bash
idf.py -p /dev/cu.usbmodem1101 build flash monitor
```

(Use your board's port: `ls /dev/cu.usbmodem*`.)

## Using it

At boot the board advertises as an unpaired DASH pod named ` :: Fake POD ::`.
Pair it from the app as a new Omnipod DASH pod.

**Control panel.** The serial console prints where it is:

- With no Wi-Fi network saved, the board runs its own access point,
  `ESP-Sim-Pod-XXXX`. Its password, derived from the chip's MAC, is printed
  at boot. Join it and open http://192.168.4.1/.
- In the panel's Wi-Fi card, save your network: the board restarts, joins it,
  takes the time from NTP and prints its address.

The panel is the Go simulator's: set the reservoir, the pod's age, alerts and
faults, and make the next command go unanswered to test the app's recovery.
Its JSON API is unchanged — `GET /status`, `POST /command` with
`{"command": "changeReservoir"|"setActiveTime"|"setAlerts"|"setFault"|"crashNextCommand"|"resetPod", ...}` —
plus `POST /wifi` with `{"ssid": ..., "password": ...}` (empty to forget).

**State** is kept in flash: a reboot is the same pod, still paired.

**Time.** The pod's age and its delivery timing come from the clock. With a
Wi-Fi network the clock is set by NTP; without one it resumes from the pod's
last save, so it loses the time the board was off. The pod reports its
activation time in the zone set in `idf.py menuconfig` → *ESP-Sim DASH pod*
(UTC by default).

## Differences from the Go simulator

- **Deactivating** the pod from the app makes the board a new, unpaired pod.
  (The Go simulator exits and needs restarting with `-fresh`.) The panel's
  *New pod* button does the same.
- **"Crash" on the next command** reboots the board, which comes back as the
  same pod.
- **Malformed commands** (too short to parse) end the connection instead of
  crashing the simulator.
- Not ported: the websocket API for `ps2/pod_simulator_frontend`, and the
  ImpossiBLE transport for the iOS Simulator.
- Like the Go simulator's Bluetooth radio, the advertisement carries the pod's
  service UUIDs (including its fault and alert status) but no manufacturer data.

Everything else, including the Go code's quirks that apps already accept,
is kept byte for byte: see the tests.

## Layout

| Path | What |
|---|---|
| `components/cgmsim/` | The simulated Dexcom G6 transmitter and G7 sensor, as portable C |
| `components/dashpod/` | The pod, as portable C: crypto (AES-CCM, CMAC, X25519, Milenage; AES, SHA-256 and P-256 from mbedTLS), messages, link layer, pairing (DASH and Omnipod 5), EAP-AKA, commands, responses, state machine |
| `components/medtrumpatch/` | The simulated Medtrum patch, as portable C: CRC-8, the key derivation, packet framing and the patch state machine |
| `main/` | The ESP32 side: NimBLE GATT and advertising for both devices (`ble.c`), the CGM's clock and commands (`cgm_app.c`), NVS (`store.c`), Wi-Fi and the HTTP panel (`panel.c`, `panel.html`) |
| `test/host/` | Host tests (macOS) |
| `tools/golden/` | Generates `test/host/golden_dash.h` by running the upstream Go code |
| `tools/o5golden/` | Generates `test/host/golden_o5.h` by running OmnipodKit's Omnipod 5 code |

## Tests

```bash
make -C test/host
```

The host tests run the same C core that runs on the board, with mbedTLS built
from ESP-IDF's copy. They check:

- CRC-32, AES-CMAC (RFC 4493), AES-CCM (RFC 3610), X25519 (RFC 7748) and
  Milenage (3GPP TS 35.208) against their published vectors;
- pairing, EAP-AKA, message encryption, every response type and the link
  layer's fragmentation byte for byte against the Go simulator's own output
  (`tools/golden/gen.sh` regenerates it from the pinned upstream commit);
- a whole run against a simulated phone: pairing, a session, commands,
  control-panel faults, a reconnect, a bad message, and deactivation.

## Omnipod 5

In the control panel's *Pod type* card, switch to Omnipod 5: the board
becomes a new, unpaired Omnipod 5 pod (and back to DASH the same way).

The Omnipod 5 side follows the controller that apps use,
[OmnipodKit](https://github.com/loopandlearn/OmnipodKit) (`8df3d7b`),
rather than the Python emulator in vicktor/openpod-5, whose pairing doesn't
match it. An Omnipod 5 pod speaks DASH's protocol with these differences, all
simulated:

- **Bluetooth:** it advertises one 128-bit UUID,
  `CE1F923D-C539-48EA-7300-0A<controller id>00` (`FFFFFFFE` before pairing),
  sends data on characteristic `…2443`, uses 244-byte packets and no RTS/CTS.
- **Pairing:** `SPS0` picks P-256 with certificates; `SPS1` exchanges P-256
  keys; conf and the LTK come from SHA-256; `SPS2.1` and `SPS2` exchange
  certificates (and signatures) encrypted with AES-CCM.
- **Certificates:** the pod's is self-signed with a P-256 key it makes when it
  becomes a new pod. OmnipodKit reads the key from it and checks the pod's
  signature, but not who issued it. No Insulet key or certificate is used or
  needed, and the pod doesn't check the phone's.
- **Commands:** signed (Type 4) commands are accepted (the signature isn't
  checked); responses carry the Omnipod CRC-16, which OmnipodKit checks for
  Omnipod 5; the version and setup responses say product 5.
- **AID setup commands** (`S3.9=8,G3.9`, `SE255.2=…`, `G3.11`…) are answered
  with the prefixes OmnipodKit expects; their values are not otherwise used.

What a real Omnipod 5 pod says in its AID answers, and its certificates'
contents, are not public: the simulator gives OmnipodKit what it checks.

## Tests (Omnipod 5)

`make -C test/host` also runs the Omnipod 5 tests: P-256 and the
certificate, AID answers, 244-byte packets, and, when `test/host/golden_o5.h`
is present, a comparison with OmnipodKit's own code and a whole pairing and
session against a simulated phone using OmnipodKit's messages.

`tools/o5golden` builds OmnipodKit's Omnipod 5 files (key exchange, packet
splitting, message encoding, CRC, AID commands) with stubs, runs them on fixed
inputs and writes `golden_o5.h`:

```bash
tools/o5golden/gen.sh
```

## Medtrum patch

> **Use a test phone.** The patch reports the reservoir, battery and delivery
> state the panel says, and it delivers no insulin. A phone paired to the
> simulated patch must never control anyone's therapy.

Medtrum's pump is two parts: a reusable **pump base** — the Bluetooth device
that carries the radio, serial number, model and firmware — and a disposable
**patch**, the reservoir and cannula that clip onto it. The simulator is a
pump base that primes a patch; Medtrum's parts are a base and a patch, and
this section uses those names throughout.

In the control panel's *Medtrum patch* card, choose the model and set the base
serial number, watch or set the patch's state, ID, reservoir, prime progress
and battery, raise a fault, make the next command go unanswered to test the
app's recovery, and start a new patch. Its JSON API is `GET /patch` and
`POST /patch`.

The Medtrum side follows the controller that apps use,
[MedtrumKit](https://github.com/loopandlearn/MedtrumKit) (`ef866fa`), the pump
manager in Trio. It speaks MedtrumKit's Bluetooth protocol: it advertises as
`MT` with the base serial in its manufacturer data, exposes the service
`669A9001-…` with a status READ characteristic and a command WRITE
characteristic, frames commands with a CRC-8, and authenticates with a key
derived from the base serial number.

- **Two models.** The base serial number decides the model and its reservoir
  size: `4A12D828` is an MD0201, a 200 U base, and `52DF1614` is an MD8301, a
  300 U base. The serial is runtime state, set in the panel and kept in flash,
  so the base can be switched without rebuilding the firmware.
- **Patch states.** A patch moves through idle, filled, priming, primed and
  active, and reports suspended, fault and stopped states, as MedtrumKit
  expects.
- **Delivery.** Basal, temp basal and bolus commands drive a simulated
  delivery state — reservoir, patch age and battery — that the app sees in
  the base's status notifications.

### Tests (Medtrum patch)

```bash
make -C test/host medtrum
```

This target builds only the portable Medtrum C core, with no mbedTLS or
ESP-IDF, and checks the CRC-8, the key derivation and the packet framing
against the byte vectors in MedtrumKit's own tests, then runs a whole session
— auth, synchronize, subscribe and the delivery commands — against a simulated
phone.

## Dexcom G6 and G7

> **Use a test phone.** The CGM's readings are whatever the panel says. A
> phone whose app reads them and also controls a real pump or pod would dose
> real insulin from them.

In the control panel's *CGM* card, start a new G6 transmitter or G7 sensor
(or none, the default). The board then shows up as two Bluetooth devices: the
pod, from its own address, and the CGM, from a random address of its own,
named like a Dexcom (`DexcomXX` for a G6, `DXCMXX` for a G7). The phone can be
connected to both at once.

The CGM follows the code apps use: LoopKit's
[CGMBLEKit](https://github.com/LoopKit/CGMBLEKit) (`776543f`) for the G6 and
[G7SensorKit](https://github.com/LoopKit/G7SensorKit) (`a03a295`) for the G7.

- **Readings** every 5 minutes, at the glucose and trend set in the panel (the
  trend moves the value at each reading, between 40 and 400 mg/dL).
- **Each reading** opens a minute of advertising. The phone connects, gets the
  reading (and any it missed: backfill), and the CGM hangs up, as the real
  ones do.
- **No Dexcom app is needed.** Both apps normally watch the Dexcom app talk to
  the sensor; the simulator sends the messages the Dexcom app's session would
  produce. A G6 app can also log in itself (CGMBLEKit's active mode, with the
  key made from the transmitter ID), start, stop and calibrate the sensor.
- **No Bluetooth pairing.** The CGM reports itself bonded without asking the
  phone to pair.
- **Sensor life:** warm-up (2 hours for a G6, 27 minutes for a G7, which the
  panel can skip), then 10 days (G6) or 10 days plus the 12-hour grace period
  (G7).
- **Testing the app:** set a state (failed, temporary issue, expired…), or
  turn on signal loss to miss readings and see them backfilled after.

Its JSON API: `GET /cgm`, and `POST /cgm` with
`{"command": "cgmNew", "kind": 0|6|7, "id": "81AB7Q"}`,
`{"command": "cgmGlucose", "glucose": 120, "trend": 10}` (tenths of mg/dL per
minute), `{"command": "cgmState", "state": 0}` (0: automatic),
`{"command": "cgmSignalLoss", "on": true}` or `{"command": "cgmSkipWarmup"}`.

The host tests check its messages against the byte vectors in CGMBLEKit's and
G7SensorKit's own tests, and run whole sessions (G7; G6 watched and G6 logged
in) against a simulated phone.

## License

GPL-3.0-or-later, as the code it was ported from.
