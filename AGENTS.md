# Maintainer and LLM instructions

This file is the operating guide for humans and automated coding agents
changing this repository. Read `README.md`, the target YAML, and the relevant
component source before editing. Do not infer hardware behavior from generic
ESP32 or W5500 examples when this repository records board-specific behavior.

## Project scope

This repository has two closely related responsibilities:

1. Provide an ESPHome `ethernet` external component for the Wiznet W6100 on
   ESP32. It includes a MACRAW-mode ESP-IDF MAC/PHY implementation.
2. Provide usable Koniec Swiata IoT ESP32-S3 configurations for the board and
   its real interfaces.

Keep changes focused on the component and examples. Do not vendor all of
ESPHome, generate a standalone ESP-IDF application, or replace a proven
third-party integration with placeholder registers.

## Source map

| Path | Responsibility |
| --- | --- |
| `esphome/components/ethernet/__init__.py` | ESPHome schema, validation, code generation, and source filtering |
| `esphome/components/ethernet/ethernet_component.h` | Component state and public configuration setters |
| `esphome/components/ethernet/ethernet_component.cpp` | Framework-independent network component behavior |
| `esphome/components/ethernet/ethernet_component_esp32.cpp` | SPI bus, ESP-IDF Ethernet/netif setup, GPIO diagnostics |
| `esphome/components/ethernet/esp_eth_mac_w6100.c` | W6100 MACRAW transport and ESP-IDF MAC interface |
| `esphome/components/ethernet/esp_eth_phy_w6100.c` | W6100 PHY state, link, speed, and duplex |
| `esphome/components/ethernet/esp_eth_w6100.h` | Driver construction API |
| `esphome/components/ethernet/w6100.h` | W6100 registers, commands, and bit definitions |
| `examples/*.yaml` | Board-level configurations and integration documentation |

The component intentionally uses the name `ethernet`, replacing ESPHome's
stock component only for configurations that load this local component. At
present its schema supports `type: W6100` on ESP32 only.

## Hardware contract

Treat this map as a compatibility contract. If a change reassigns a pin, update
every affected tracked example and `README.md` in the same commit.

| Function | GPIO/settings |
| --- | --- |
| W6100 MOSI/MISO/CLK/CS | 11 / 13 / 12 / 10 |
| W6100 interrupt | GPIO9, active low |
| W6100 nCOL/nDPX/nSPD | GPIO14 / GPIO21 / GPIO47, active low |
| I2C SDA/SCL | GPIO8 / GPIO18 |
| LM75B | I2C `0x48`, A0-A2 low, 9-bit/0.5 deg C |
| LM75B alarm | GPIO38, active low, pull-up input |
| Relay 1/2 | GPIO17 / GPIO16 |
| Status LED | GPIO48 |
| Level-shifted UART TX/RX | GPIO5 / GPIO4 |
| RS485 TX/RX/RTS-DE | GPIO15 / GPIO6 / GPIO7 |
| Isolated HAT UART1 TX/RX | GPIO39 / GPIO40 |
| Isolated HAT UART2 TX/RX | GPIO41 / GPIO42 |

Do not invent connector numbers, supply voltages, relay ratings, isolation
ratings, termination layout, or polarity that is not present in tracked source
or a supplied schematic. Firmware GPIO names are not sufficient evidence for
those properties.

## W6100 design constraints

- Socket 0 runs in W6100 MACRAW mode.
- ESP32-S3 defaults to SPI2 and 26.67 MHz.
- `interrupt_pin` and `polling_interval` are mutually exclusive.
- When neither is supplied, supported ESP-IDF versions receive the 10 ms
  polling default. Older unsupported IDF versions require an interrupt pin.
- The board interrupt is GPIO9, but field testing found a case where interrupt
  mode obtained DHCP while ARP/ping stopped. Ten-millisecond polling restored
  traffic. Preserve polling as the documented first recovery step.
- Do not raise the recommended polling interval to 100 ms without packet-loss,
  latency, API, web-server, and OTA testing. Ten milliseconds is the known-good
  value.
- `nCOL`, `nSPD`, and `nDPX` are floating diagnostic inputs. Their decoded
  states are logged; they do not configure the PHY.
- ESP-IDF may log that adding an IPv4 multicast MAC filter is unsupported. The
  current W6100 MAC can filter IP multicast but cannot satisfy the exact MAC
  filter ioctl. Treat this as a discovery/multicast limitation, not a failed
  unicast connection.
- Ethernet declares a configuration conflict with WiFi. Do not claim automatic
  WiFi fallback works until runtime interface support is implemented and tested.
- Preserve compatibility with both ESP-IDF and Arduino source filtering. The
  Hormann example depends on Arduino; the other tracked examples use ESP-IDF.

Changes to register addressing, interrupt clearing, receive-buffer accounting,
SPI transaction format, or ESP-IDF ownership have a large blast radius. For
such changes, inspect the W6100 datasheet and the matching ESP-IDF `esp_eth`
interfaces rather than relying on memory.

## Example-specific rules

### `examples/koniec_swiata_iot.yaml`

This is the canonical general-purpose Ethernet example. It owns the shared
board behavior: web server, API with no disconnect reboot, OTA, I2C sensors,
alarm input, two momentary relay buttons, and status LED scripts. Apply broadly
useful board changes here first, then decide which specialized examples need
the same update.

Relay buttons currently pulse for 250 ms. They must remain momentary `button`
entities unless a use case explicitly needs a stateful output. The idle LED is
1 second on plus 1 second off; relay feedback is three 100 ms on/off flashes.

### `examples/koniec_swiata_iot_wifi.yaml`

This is a WiFi-only alternative, not Ethernet fallback. Its Ethernet block is
commented. The WiFi RSSI entity uses a 5-second interval and
`disabled_by_default: true`; preserve that Home Assistant behavior unless the
request says otherwise. `captive_portal:` is present, but a fallback AP needs
an explicit `wifi.ap` configuration.

### `examples/koniec_swiata_iot_hormann_gate.yaml`

Use `github://mapero/esphome-hcpbridge` and its real `hcpbridge` platform. The
example must remain on Arduino unless upstream adds and verifies ESP-IDF
support. Preserve the external library and custom API services. RS485/HCP uses
GPIO15 TX, GPIO6 RX, and GPIO7 RTS. This is the specialized example that keeps
the relay press buttons enabled for a second gate.

Do not replace HCPBridge with generic Modbus placeholders. When changing its
schema, inspect the current upstream component and compile against the selected
revision.

### `examples/koniec_swiata_iot_panasonic_heat_pump.yaml`

Use the actual `panasonic_heatpump` component from
`https://github.com/ElVit/esphome_components/`. UART1 is the heat-pump side and
UART2 is optional client/CZ-TAW1 passthrough. Both are 9600 8E1 with 512-byte
receive buffers. Keep `log_uart_msg: false` by default. The CN-CNT side needs
5 V level handling and galvanic isolation; never document direct ESP wiring.

Relays are intentionally absent. Preserve the upstream TOP/SET naming closely
enough that users can compare entities with HeishaMon documentation.

### `examples/koniec_swiata_iot_thessla_green.yaml`

This is native ESPHome Modbus RTU on GPIO15/GPIO6/GPIO7, configured for 9600
8E1 and slave `0x01`. It contains broad AirPack4 protocol coverage. Keep the
distinction between safe active entities and intentionally inactive registers.

Do not expose communication parameters, product/access locks, hard reset,
service calibration, or packed/multi-register schedule and date fields as
casual Home Assistant controls. New write entities require checking register
type, signedness, scaling, allowed range, firmware applicability, and whether
the write must be atomic with adjacent registers. Add read-only visibility
first when uncertainty remains.

Relays are intentionally absent from this example.

## Editing workflow

1. Inspect `git status --short` before editing. Uncommitted and untracked files
   belong to the user; ignore and preserve them unless explicitly placed in
   scope.
2. Read the full target YAML and relevant component functions. Search with
   `rg` before introducing new names or duplicate logic.
3. Follow existing ESPHome patterns and upstream component schemas. For a
   documented third-party device, use its maintained component when feasible.
4. Keep edits scoped. Hardware-wide changes may need all examples; use-case
   changes generally belong in only one example.
5. Validate the exact changed examples. A local component path is relative to
   the YAML, so run commands from the repository root as shown below.
6. Review the generated diff and status. Do not add build directories,
   `examples/secrets.yaml`, personal configurations, or unrelated files.
7. Commit coherent progress with a specific message when requested by the
   project owner. Never rewrite or discard user commits or working-tree files.

Do not remove driver sources merely because a generated build copied or
compiled them. Do not delete caches or user files unless explicitly requested.

## Validation

For a YAML-only change, run at least `config`; run `compile` when dependencies
and time permit. Driver changes require compilation of an ESP-IDF example and
the Arduino Hormann example because source filtering supports both frameworks.

Native commands:

```sh
esphome config examples/koniec_swiata_iot.yaml
esphome compile examples/koniec_swiata_iot.yaml
esphome compile examples/koniec_swiata_iot_hormann_gate.yaml
```

Container command:

```sh
podman run --rm -it -v "$PWD":/config \
  ghcr.io/esphome/esphome:2026.5.3 \
  compile examples/koniec_swiata_iot.yaml
```

External components make validation network-dependent on a clean cache. If a
download fails, distinguish network/cache failure from YAML or compiler
failure in the final report. Do not silently substitute a placeholder.

Hardware validation for W6100 changes should include:

- cold boot and repeated reboot without a panic;
- DHCP or static address acquisition;
- ARP and sustained ping in both directions;
- ESPHome API connection, web UI, and OTA;
- unplug/replug link recovery;
- packet traffic under interrupt mode and 10 ms polling where relevant;
- status-pin logging at negotiated speed/duplex;
- no regressions in relay, I2C, and the active serial interface.

A successful compile does not prove Ethernet interrupt handling works. Ask for
on-device confirmation before declaring hardware-sensitive behavior complete.

## Debugging order

For an unreachable board, collect the serial boot log first. Then reduce the
problem in this order:

1. Confirm stable boot and ESPHome setup completion.
2. Confirm SPI pinout, W6100 link, and DHCP/static address.
3. If an address is present but ARP/ping fails, replace GPIO9 interrupt mode
   with 10 ms polling.
4. Test by IP before diagnosing mDNS. Multicast-filter warnings can affect
   discovery while unicast remains healthy.
5. Disable only the specialized application integration, leaving the base
   network and diagnostics intact.
6. For UART/RS485, verify electrical levels, isolation, A/B or TX/RX polarity,
   serial format, direction control, and slave/device address.
7. Add protocol logging temporarily and remove or disable noisy logging after
   the fault is understood.

When reporting work, state which examples were validated, the ESPHome version,
whether validation was compile-only or performed on hardware, and any remaining
warnings or untested interfaces.
