# hcpbridge_debug

Passive diagnostics for the Hörmann HCP bus when it is driven by the upstream
[`mapero/esphome-hcpbridge`](https://github.com/mapero/esphome-hcpbridge)
component (`hcpbridge:`). It does **not** replace HCPBridge and does not change
its protocol behaviour. It watches the RS485 traffic going through HCPBridge
and logs it, so you can see why the motor does not detect the bridge.

Status: compile-tested only (ESPHome 2026.9.1, Arduino framework, ESP32-S3).
It has not yet been run against a real motor. Treat its own output critically
the first time.

## Why it exists

Upstream HCPBridge logs only after it receives a complete, CRC-valid Modbus
request addressed to slave 2. Every failure below that layer produces exactly
the same result: no logs and `HCPBridge Connected` stays false. That includes
no bus power, broken wiring, swapped A/B, wrong baud rate, noise, a transceiver
that never receives, and a motor that never polls slave 2. This component makes
each of those layers visible.

## How it works

```
 Hörmann motor ──RS485── transceiver ──GPIO6 RX / GPIO15 TX / GPIO7 DE── ESP32-S3
                                            │
                                         Serial2 (57600 8E1)
                                            │
                                   ┌────────┴─────────┐
                                   │ TapStream (this) │  copies every byte into a queue
                                   └────────┬─────────┘
                                            │ unchanged reads/writes
                                   ModbusRTU (emelianov/modbus-esp8266)
                                            │
                                   HoermannGarageEngine (upstream HCPBridge)
```

1. **Stream tap.** Upstream calls `mb.begin(&Serial2, rts, true)` and stores the
   stream in `ModbusRTUTemplate::_port`. In `setup()` (priority LATE, after
   HCPBridge has started) this component replaces that pointer with
   `TapStream`, a wrapper around the same `Serial2`. Every `read()` and
   `write()` passes straight through, and the wrapper records the byte.
   - `mb` (private in `HoermannGarageEngine`) and `_port` (protected in
     `ModbusRTUTemplate`) are reached with the standard explicit template
     instantiation trick (`PrivateAccess` in `hcpbridge_debug.cpp`). Upstream
     is not patched or vendored. If upstream renames those members, this
     component stops compiling. It cannot silently misbehave.
   - The swap races harmlessly with the Modbus task: old and new pointers both
     lead to the same UART.
2. **Framing.** Bytes are grouped into frames by direction change or a gap of
   more than 1 ms. The ModbusRTU slave reads each frame in one burst after 1.75
   ms of bus silence, so this matches Modbus frames. Bytes are recorded under a
   spinlock into a 32-frame queue (64 bytes stored per frame) from the
   high-priority Modbus task. **All logging happens in the ESPHome main loop**,
   so the bridge's reply timing on the bus is not affected.
3. **Raw RX edge counter.** A PCNT (hardware pulse counter) unit counts falling
   edges on the HCPBridge RX pin. The pin is read from the `hcpbridge: rx_pin`
   config automatically. PCNT reads the pad through the GPIO matrix and does
   not take the pin away from the UART. This separates "no electrical signal"
   from "signal present but not decodable".
4. **UART errors.** `Serial2.onReceiveError()` counts break, frame, parity and
   FIFO/buffer overflow errors.
5. **Engine state.** The summary includes upstream `state->valid` and
   `state->responseAge()`. Note that upstream never resets `valid` to false, so
   `last request age` is the better liveness signal.
6. **Decoding.** Each RX frame gets a CRC check, slave address and function
   code. HCP requests are labelled using the same patterns upstream matches in
   `HoermannGarageEngine::onRequest()`. RX frames identical to the bridge's last
   reply within 50 ms are flagged as **echo**.

## Configuration

```yaml
hcpbridge_debug:
  log_frames: true      # per-frame DEBUG lines (default true)
  report_interval: 10s  # summary period (default 10s)
```

Requires `hcpbridge:` in the same config and the Arduino framework. The
component is loaded from this repository:

```yaml
external_components:
  - source: github://lsiudut/iot-koniecswiata
    components: [ethernet, hcpbridge_debug]
```

For local, unpublished changes use
`source: {type: local, path: ../esphome/components}` instead (path relative to
the YAML file). Remove the block, or set `log_frames: false`, once the bus
works. At full HCP traffic, per-frame logging is noisy.

## Protocol facts it relies on

From upstream `hoermann.h` / `hoermann.cpp`:

| Item | Value |
| --- | --- |
| Serial | `Serial2`, 57600 baud, 8E1 |
| Bridge role | Modbus RTU **slave**, address `0x02` |
| Board pins (this repo) | TX GPIO15, RX GPIO6, DE/RTS GPIO7 (high = transmit) |
| Bus scan request | FC `0x17`, write `0x9C41` ×3, read `0x9CB9` ×5 |
| Command poll | FC `0x17`, write `0x9C41` ×2, read `0x9CB9` ×8 |
| Empty command | FC `0x17`, write `0x9C41` ×2, read `0x9CB9` ×2 |
| Status broadcast | FC `0x10`, write `0x9D31` (state, position, light) |

The motor is the Modbus master. The bridge only ever transmits replies. If the
motor never sends a request addressed to `0x02`, the bridge stays silent by
design.

## Reading the logs

Log tag: `hcpbridge_debug`. Upstream engine tag: `HCI-BUS`.

Per-frame lines (DEBUG, when `log_frames: true`). The examples below are
illustrative: format is exact, byte values and timings are not captured from a
real motor.

```
RX  19 B addr=02(us) fc=17 crc=ok (+412 ms) rw read 9CB9 x5 write 9C41 x3 BUS SCAN : 02 17 9C B9 00 05 9C 41 00 03 06 ...
TX  15 B reply      : 02 17 0A 00 00 00 05 04 30 10 FF A8 45 xx xx
RX  27 B addr=00(bcast) fc=10 crc=ok (+20 ms) write 9D31 x9 status broadcast : ...
RX  13 B crc=BAD (+3 ms): ...
RX   2 B short/noise (+0 ms): FF 00
RX  15 B ECHO of own reply: ...
```

- `(us)` = addressed to slave 2, `(bcast)` = address 0, `(other)` = another
  device on the bus.
- `+N ms` = time since the previous RX frame, which shows the motor's polling
  cadence.
- `crc=n/a` = frame longer than 64 bytes. Only the first 64 bytes are stored,
  so CRC cannot be checked.

Summary every `report_interval` (INFO):

```
Last 10 s: RX edges=… bytes=… frames=… (crc ok=… bad=… short=… echo=…) | TX bytes=… replies=…
  to slave 2=… broadcast=… other slaves=… bus scans=… | UART err break=… frame=… parity=… overflow=… | dropped=…
  totals: RX edges=… bytes=… crc ok=… bad=… to us=… replies=… | RX idle level=… | engine valid=… last request age=… s
  slave addresses seen on bus: 02 …
  hint: …
```

`dropped` > 0 means the 32-frame queue overflowed between loop iterations. The
counters are still right, but some frame lines are missing.

### Hints, lowest layer first

Only the first matching hint is printed per report.

| Hint | Meaning | Next step |
| --- | --- | --- |
| Stream tap not installed | HCPBridge engine did not start | Check boot log for HCPBridge/`Serial2` errors |
| No signal on RX GPIO since boot | Zero edges and zero bytes | Bus power and wiring to the board RS485 terminals, transceiver receive enable, DE (GPIO7) idle low |
| RX line held low (break) | Idle line reads 0 or UART break errors | A/B swapped, missing bias, transceiver fault |
| Framing/parity errors without valid frames | Bytes arrive but are malformed | Baud/format (57600 8E1), A/B polarity, noise |
| RX edges but no UART bytes | Signal present, UART decodes nothing | Baud mismatch, UART not on that pin |
| Mostly CRC failures | Frames garbled | Termination, ground reference, cable, polarity |
| Own replies are read back | Receiver active while transmitting | Transceiver RE/DE wiring. Echoes may be parsed as requests |
| Motor talks but never to slave 2 | Bus works, bridge never addressed | Trigger a bus scan on the motor (method is model-specific, see motor manual/HCPBridge docs) |
| Requests for slave 2 not answered | Engine received but did not reply | Software problem in the Modbus/engine path |
| Replies are sent | Request → reply path works on the ESP side | Check DE actually switches the driver and replies reach the bus (scope or a USB-RS485 sniffer) |

If `bus scans` > 0 and `replies` > 0 but the motor still reports no device, the
ESP side is answering. Focus on the TX path (DE timing, driver, polarity) or on
reply content compared with known-working HCPBridge captures.

## Guidance for an LLM assisting with debugging

If you are an AI assistant helping someone use this component:

- **Ask for logs first, don't guess.** Request the serial or `esphome logs`
  output covering boot (including the `HCPBridge debug:` config dump) and at
  least 3 summary periods, ideally while a bus scan is triggered on the motor.
  Make sure the logger level is DEBUG or more verbose so frame lines appear.
- **Work bottom-up** through the hint table. Do not suggest protocol changes
  while edges are 0 or CRC failures dominate.
- **Board facts are fixed:** RS485 TX GPIO15, RX GPIO6, DE/RTS GPIO7. The
  repository root has `AGENTS.md` (maintainer rules), `HARDWARE.md` (electrical
  and connector reference) and `README.md`. Read `HARDWARE.md` before giving
  wiring advice. Do not invent connector pinouts, voltages or ratings that are
  not documented there.
- **Do not replace HCPBridge** with a generic `modbus_controller`/placeholder
  setup. The project deliberately uses the upstream component on Arduino.
- **Keep this component passive.** If you change it:
  - Never log, allocate or block inside `record_byte()` or `TapStream`. Those
    run in the Modbus task, and the motor expects replies within milliseconds.
  - Never consume bytes from `Serial2` outside the ModbusRTU path.
  - Validate with `esphome config` and `esphome compile` on the Hormann example
    (`examples/koniec_swiata_iot_hormann_gate.yaml`).
- **Distinguish failure types** when reporting: network or cache download
  failure vs. YAML error vs. compiler error vs. on-device behaviour.
- **Known build issue (independent of this component):** with ESPHome 2026.9.x,
  the repository's `ethernet` component fails to compile because
  `network/util.cpp` calls `EthernetComponent::is_disabled()`, which it does
  not implement. The project's documented container version is 2026.5.3. Do
  not attribute that error to `hcpbridge_debug`.
- **Useful upstream source to read:** `components/hcpbridge/hoermann.cpp`
  (`setup()`, `onRequest()`) in `mapero/esphome-hcpbridge`, and `ModbusRTU.cpp`
  `task()` / `rawSend()` in `emelianov/modbus-esp8266` 4.1.0.

## Files

| File | Purpose |
| --- | --- |
| `__init__.py` | Schema, Arduino-only check, reads RX pin from `hcpbridge:` config |
| `hcpbridge_debug.h` | Component class, frame queue, counters |
| `hcpbridge_debug.cpp` | Stream tap, private-member access, PCNT, decoding, summary/hints |
