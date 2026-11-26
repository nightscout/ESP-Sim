# ESP-Sim: an Omnipod DASH and Omnipod 5 pod, and Dexcom G6 and G7 CGM, simulator on the ESP32-S3

> **Test equipment only.** ESP-Sim pretends to be an insulin pod, and
> optionally a CGM, so that controller apps (Loop, Trio and others using
> OmnipodKit, CGMBLEKit and G7SensorKit) can be developed and tested without
> real devices. It delivers no insulin, its glucose readings are made up, and
> nothing it reports reflects a real device. Never use it — or an app paired
> to it — for anyone's therapy. **Never let a phone that controls a real pump
> or pod read the simulated CGM:** the app would dose insulin from made-up
> numbers. This is not medical advice; talk to your care team about your own
> treatment.

This is a C port of the Go DASH simulator
([jeremybarnum/pod](https://github.com/jeremybarnum/pod) at
`a491a5e`, itself from [LoopKit/pod](https://github.com/loopkit/pod) and
0pen-dash) from a Raspberry Pi to an ESP32. It speaks the same Bluetooth
protocol — pairing, EAP-AKA sessions, encrypted pod commands — and serves the
same control panel, from a board that needs no Pi, no Linux and no Go.

It can also be an Omnipod 5 pod, and at the same time a Dexcom G6 or G7:
see [Omnipod 5](#omnipod-5) and [Dexcom G6 and G7](#dexcom-g6-and-g7) below.

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

## Pages

The board serves a small set of pages. `/` and `/cgm` share one document and one
nav; `/pump` and `/settings` are their own pages.

| Page | What it is | Aliases |
|---|---|---|
| `/` | Home: what is simulated right now (the pump and the CGM, with each one's status), the firmware card and the pod's comms counters | |
| `/cgm` | The CGM page: the sensor selector, the sensor's own box (pairing code, states) and the glucose source with its settings (the same file as `/`) | `/g7` -> `/cgm` |
| `/pump` | The pump page: simulator selector and the pod, YpsoPump or Medtrum controls | `/pod`, `/medtrum` -> `/pump` |
| `/settings` | Network, clock, firmware and reboot | |
| `/setup` | The captive portal's Wi-Fi-or-skip sheet | |

The JSON and form endpoints keep the panel's names: `GET /status` and
`POST /command` for the pod, `GET /ypso` and `POST /ypso` for the YpsoPump,
`GET /patch` and `POST /patch` for the Medtrum base, and the CGM's
`/cgm/status` and `/cgm/command`. `GET /settings/status` is what the settings
page polls; `POST /settings/wifi` behaves like `/wifi`, `POST /settings/forget`
clears the saved network, `POST /settings/reboot` restarts, and
`POST /settings/pump` is the `pumpSelect` switch the pump page posts.
`POST /setup/wifi` saves the same network as `/wifi`, and
`POST /setup/dismiss` sets the dismissal cookie.

## Captive portal

With no saved network (or after the saved one cannot be reached), the board
runs its own access point and answers the OS connectivity probes so the phone
shows the setup sheet:

- **Before dismissal:** Apple's `/hotspot-detect.html` gets the sheet itself;
  Android's `/generate_204` and `/gen_204`, Windows' `/connecttest.txt` and
  `/ncsi.txt`, and `/fwlink` answer `302` to `/setup`; `/` also redirects to
  `/setup`.
- **Dismissal:** `POST /setup/dismiss` sets
  `setup=done; Path=/; Max-Age=604800`. A successful join (`POST /setup/wifi`)
  sets the same cookie. The cookie is browser state only; the board keeps no
  server-side "dismissed" flag.
- **After dismissal, or with a joined network:** Apple gets `Success`, Android
  gets `204` with no body, Windows gets `Microsoft Connect Test` or
  `Microsoft NCSI`, and `/` serves the panel. `GET /setup` still serves the
  sheet directly.
- **Unknown app paths:** they fall through to the same portal handler, which
  answers `404`. The `/*` catch-all is registered last, so it never shadows a
  page.

A DNS responder listens on UDP 53 while the AP runs. Every A query is answered
with the AP address `192.168.4.1`, so any name the phone asks for lands on the
board; other query types get a NOERROR/no-record answer. It starts and stops
with the access point.

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
| `components/cgmsim/` | The simulated Dexcom G6 transmitter and G7 sensor, and the virtual person (`person.c`), as portable C |
| `components/ypsosim/` | The simulated YpsoPump, as portable C |
| `components/portalsim/` | The captive-portal request decision, as portable C |
| `components/dashpod/` | The pod, as portable C: crypto (AES-CCM, CMAC, X25519, Milenage; AES, SHA-256 and P-256 from mbedTLS), messages, link layer, pairing (DASH and Omnipod 5), EAP-AKA, commands, responses, state machine |
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

The portal and DNS lanes are separate: they need neither mbedTLS nor ESP-IDF,
so they build with plain `cc`.

| Lane | Command | Covers |
|---|---|---|
| Core, CGM, YpsoPump (default) | `make -C test/host` | The pod core and the CGM/YpsoPump state machines |
| Medtrum | `make -C test/host medtrum` | The Medtrum patch, delivery draw and serial helpers |
| Captive portal | `make -C test/host portal` | The portal decision matrix: Apple/Android/Windows probes, `/fwlink`, root and cookie, `/setup`, methods, NULL inputs |
| DNS | `make -C test/host dns` | The DNS answer builder: compression pointer, trailing dot, non-A, truncated query, echoed id |

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

**Glucose that moves like a person's.** In the CGM card, set *Glucose comes
from* to *A virtual person*. The person runs their own day: breakfast, lunch,
dinner and sometimes snacks, at varied times, each a food from a list with its
own carbohydrate and speed (juice fast, porridge steady, pizza late and long).
They treat their own lows, sometimes exercise, rise before dawn, and drift; the
sensor adds a few mg/dL of noise and the trend arrow follows the last 15
minutes. Their insulin is either their own (basal, and meal boluses that are
sometimes off in size or timing, plus the odd correction), or **only what the
simulated pump delivers**: then the AID's decisions show in the glucose. The
YpsoPump counts everything it delivers; the pod counts its boluses and its
running basal (its temp basals are not modelled yet). ISF, carb ratio and the
basal need are set in the panel. It is a toy model for exercising software,
not a physiological simulator: its numbers say nothing about any real person.

Its JSON API: `GET /cgm`, and `POST /cgm` with
`{"command": "cgmPattern", "pattern": 0|1, "source": 0|1, "isf": 45, "icr": 10, "basalNeed": 80}`,
`{"command": "cgmEat", "grams": 20}`, `{"command": "cgmExercise", "minutes": 45}`,
`{"command": "cgmNew", "kind": 0|6|7, "id": "81AB7Q"}`,
`{"command": "cgmGlucose", "glucose": 120, "trend": 10}` (tenths of mg/dL per
minute), `{"command": "cgmState", "state": 0}` (0: automatic),
`{"command": "cgmSignalLoss", "on": true}` or `{"command": "cgmSkipWarmup"}`.

The host tests check its messages against the byte vectors in CGMBLEKit's and
G7SensorKit's own tests, and run whole sessions (G7; G6 watched and G6 logged
in) against a simulated phone.

## YpsoPump

In the control panel's *Pump* card, choose *YpsoPump* and a firmware version;
the board restarts as a YpsoPump instead of the pod (one pump at a time, plus
the CGM). It follows the rockit YpsoPump driver (`rockit-driver-ypso`,
`docs/COMMAND-SPEC.md`).

| Firmware | What the driver sees |
|---|---|
| V05.02.03 | the hardware-validated pump: Crypto service (XChaCha20-Poly1305, counters), Control 1.4, 15-byte bolus with CRC, extended bolus, 44-byte bolus status, 20-byte system status with CRC, SAFEVAR history indexes |
| V35.01.00 | study firmware: as V05, plus the `_alt` characteristics |
| V65.00.06, V03.00.00 | OPN: no Crypto service (everything raw, unframed), Control 1.0 / 1.1, 8-byte SAFEVAR bolus, 13-byte bolus status, 18-byte system status, u16 history indexes, 10 % temp-basal steps |
| V02.00.00 | the oldest baseline: no Control service |

- **Keys:** the simulated pump makes its own random keys and sign-in password
  when it becomes a new pump. No real pump's key, password or vendor key is
  used.
- **Encryption (V05, V35):** the pump runs the key exchange an encrypted
  YpsoPump runs: the phone reads `NewPublicKey` (a fresh challenge and the
  pump's X25519 public key) and writes an authorisation blob to
  `WriteChallengeUtcAppPubKey`, sealed with a server key the pump trusts; both
  sides then hold the new session key (XChaCha20-Poly1305, counters from the
  bottom). A blob that does not open is refused `0x8A`, a counter not above the
  last exchange's `0x8B`. A stock pump trusts the vendor's server key; this one
  trusts its own, made at random on the board; the panel does not hand it
  out, so a phone cannot run the key exchange with it yet. The clear-text firmware
  (V65, V03, V02) needs none of this.
- **Bluetooth:** it advertises as `YpsoPump_<serial>` from the board's own
  address and bonds before anything but its version strings can be read. Like
  the real pump it pairs by passkey: when the phone asks for a code, the
  6-digit passkey appears at the top of the panel's *YpsoPump* card (and in
  the serial log); type that on the phone. If an earlier try left a stale
  pairing, forget the pump on the phone and press *Forget Bluetooth bonds*.
  Like the real pump it keeps bonds with up to 5 devices (phones, watches),
  forgetting the oldest when a sixth pairs, and talks to one at a time: it
  stops advertising while a device is connected.
- **Sign-in password:** by default the simulated pump lets any phone in
  without its authorisation password (or with any password), so an app that
  has none for it works at once. A real pump never does this; untick *Let any
  phone in* in the *YpsoPump* card to refuse a phone without the pump's own
  password (`0x08`), like a real pump.
- **Simulated:** basal and temp basal (up to 500 % for up to 24 h, the limits it
  reports in settings 116-119; 100 % cancels; a second one while one
  runs is refused `0x86`), immediate, extended and combination boluses with
  progress, stop, the settings the driver reads (serial, basal profiles A/B,
  limits), the event history, counters, the clock, the error codes (`0x08`
  wrong password, `0x8A` wrong key, `0x8B` counter behind, `0x81`-`0x87`).
  The panel sets the reservoir (or no cartridge), battery, suspend, and
  restarts the pump (reboot counter +1).

The host tests check it against the driver spec's golden vectors (the pump
CRC and an XChaCha20 frame) and run whole sessions on encrypted and clear-text
firmware, and a key exchange made the way rockit makes it. Rockit has
connected to the clear-text firmware (V65); the key exchange has not yet run
against rockit itself.

## License

GPL-3.0-or-later, as the code it was ported from.
