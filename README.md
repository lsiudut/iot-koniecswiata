# ESPHome W6100 support for Koniec Swiata IoT

This repository provides an ESPHome Ethernet component for the Wiznet W6100
on ESP32, plus complete configurations for the Koniec Swiata IoT ESP32-S3
board. The Ethernet implementation uses the W6100 in MACRAW mode, following
the same overall model as ESPHome's W5500 support.

The examples cover the base board, WiFi-only operation, a Hormann gate through
HCPBridge, a Panasonic Aquarea heat pump, and a Thessla Green AirPack4 over
Modbus RTU.

## Repository layout

| Path | Purpose |
| --- | --- |
| `esphome/components/ethernet/` | Local ESPHome Ethernet component and W6100 ESP-IDF MAC/PHY driver |
| [`examples/koniec_swiata_iot.yaml`](examples/koniec_swiata_iot.yaml) | General-purpose Ethernet configuration |
| [`examples/koniec_swiata_iot_wifi.yaml`](examples/koniec_swiata_iot_wifi.yaml) | WiFi-only configuration |
| [`examples/koniec_swiata_iot_hormann_gate.yaml`](examples/koniec_swiata_iot_hormann_gate.yaml) | Hormann HCP gate controller and relay buttons |
| [`examples/koniec_swiata_iot_panasonic_heat_pump.yaml`](examples/koniec_swiata_iot_panasonic_heat_pump.yaml) | Panasonic Aquarea through an isolated dual-UART HAT |
| [`examples/koniec_swiata_iot_thessla_green.yaml`](examples/koniec_swiata_iot_thessla_green.yaml) | Thessla Green AirPack4 through onboard RS485 |

All example files load the local W6100 component with a path relative to the
example:

```yaml
external_components:
  - source:
      type: local
      path: ../esphome/components
    components: [ethernet]
```

Keep that directory relationship when copying or moving an example.

## Board interfaces and pinout

The GPIO numbers below are ESP32-S3 GPIO numbers, not connector pin numbers.
Use the board silkscreen or schematic to identify the corresponding physical
terminal. Do not infer terminal voltage or current ratings from the GPIO map.

### W6100 Ethernet

The W6100 and its SPI wiring are onboard. Normally the only user connection is
the Ethernet cable.

| Signal | ESP32-S3 pin | Direction at ESP32 | Purpose |
| --- | ---: | --- | --- |
| MOSI | GPIO11 | Output | SPI data to W6100 |
| MISO | GPIO13 | Input | SPI data from W6100 |
| CLK | GPIO12 | Output | SPI clock |
| CS | GPIO10 | Output | W6100 chip select |
| INT | GPIO9 | Input | W6100 active-low interrupt |
| nCOL | GPIO14 | Input | Active-low collision indication |
| nDPX | GPIO21 | Input | Active-low full-duplex indication |
| nSPD | GPIO47 | Input | Active-low 100 Mbit/s indication |

The local component defaults to SPI2 on ESP32-S3 and a 26.67 MHz SPI clock.
The current board examples do not configure a W6100 reset pin.

The optional `nCOL`, `nDPX`, and `nSPD` inputs are diagnostics. ESPHome prints
their decoded states during `dump_config()`: collision/idle, full/half duplex,
and 100M/10M. They do not control link negotiation.

### Sensors and status LED

| Function | Pin or address | Notes |
| --- | --- | --- |
| I2C SDA | GPIO8 | Onboard I2C bus |
| I2C SCL | GPIO18 | Onboard I2C bus |
| LM75B address | `0x48` | A0, A1, and A2 are all tied low |
| LM75B OS/alarm | GPIO38 | Active low; configured with an input pull-up |
| Status LED | GPIO48 | Driven as a GPIO output |

The fitted TI LM75B is a 9-bit sensor with 0.5 deg C measurement resolution.
`accuracy_decimals: 2` changes formatting only; it cannot create finer
measurements. The ESP32 internal temperature sensor is also exposed for chip
diagnostics, but it measures the die and should not be treated as room
temperature.

The examples blink the status LED for 1 second on and 1 second off while idle.
Pressing a relay button interrupts that pattern with three fast flashes, each
using 100 ms on and 100 ms off, then resumes the idle indication.

### Relays

| Relay | ESP32-S3 pin | Default ESPHome representation |
| --- | ---: | --- |
| Relay 1 | GPIO17 | Momentary Home Assistant button |
| Relay 2 | GPIO16 | Momentary Home Assistant button |

The base, WiFi, and Hormann examples expose relays as press buttons instead of
stateful switches. A press energizes the output for 250 ms and then releases
it. This suits gate and garage-door controller inputs that expect a brief dry
contact closure.

Before wiring a relay, identify its COM, NO, and NC terminals from the board
markings. A typical button simulation uses COM and NO in parallel with the
existing push button. Disconnect power before wiring and verify the contact
voltage/current rating. Do not assume the contacts are suitable for mains or
for directly driving a motor.

### Serial and field buses

| Interface | TX | RX | Direction control | Typical use |
| --- | ---: | ---: | ---: | --- |
| Level-shifted UART | GPIO5 | GPIO4 | - | General extension UART |
| Onboard RS485/Modbus | GPIO15 | GPIO6 | GPIO7 (RTS/DE) | Modbus RTU or Hormann HCP |
| Isolated HAT UART1 | GPIO39 | GPIO40 | - | Panasonic CN-CNT heat-pump side |
| Isolated HAT UART2 | GPIO41 | GPIO42 | - | Panasonic CZ-TAW1/client passthrough |

For an ordinary UART, cross transmit and receive: board TX goes to device RX,
and board RX goes to device TX. Connect a signal reference/ground only when the
interface design requires a common reference. Verify the electrical level
before connecting any external UART.

For RS485, connect A to A and B to B. Unfortunately some manufacturers reverse
the A/B naming convention; if no valid frames are received, verify the manuals
and then test with A/B swapped. Use twisted pair, keep stubs short, and fit a
120-ohm termination only where required at an end of the bus. The board's
GPIO7 controls transmit/receive direction and should not be used by another
component while RS485 is active.

Panasonic CN-CNT uses 5 V signalling. Use the galvanically isolated,
level-translating HAT described above. Never wire CN-CNT directly to ESP32-S3
GPIOs.

## Choosing an example

| Example | Network | Main interface | Relays | ESP32 framework |
| --- | --- | --- | --- | --- |
| `koniec_swiata_iot.yaml` | W6100 Ethernet | General board functions | Enabled | ESP-IDF |
| `koniec_swiata_iot_wifi.yaml` | WiFi | General board functions | Enabled | ESP-IDF |
| `koniec_swiata_iot_hormann_gate.yaml` | W6100 Ethernet | RS485/HCPBridge | Enabled | Arduino |
| `koniec_swiata_iot_panasonic_heat_pump.yaml` | W6100 Ethernet | Isolated dual UART | Disabled | ESP-IDF |
| `koniec_swiata_iot_thessla_green.yaml` | W6100 Ethernet | RS485/Modbus RTU | Disabled | ESP-IDF |

### General-purpose Ethernet

`examples/koniec_swiata_iot.yaml` is the best starting point for a new wired
installation. It includes Ethernet, the web server, API, OTA, both temperature
sensors, the LM75B alarm input, the status LED, and both momentary relays.

### WiFi-only

`examples/koniec_swiata_iot_wifi.yaml` disables Ethernet and enables WiFi. Put
credentials in `examples/secrets.yaml`:

```yaml
wifi_ssid: "your-network-name"
wifi_password: "your-network-password"
```

The WiFi RSSI sensor updates every 5 seconds and is disabled by default in
Home Assistant. It remains visible in the ESPHome web UI and can be enabled in
Home Assistant from the entity settings.

The example includes `captive_portal:`, but it does not define a fallback AP.
Add an `ap:` section under `wifi:` if a local setup network is required:

```yaml
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:
    ssid: "Koniec Swiata Setup"
    password: !secret fallback_ap_password
```

ESPHome currently declares Ethernet and WiFi as conflicting components in this
driver. A single configuration cannot enable WiFi only after Ethernet fails.
Use either the Ethernet example or the WiFi example.

### Hormann gate

`examples/koniec_swiata_iot_hormann_gate.yaml` uses
[`mapero/esphome-hcpbridge`](https://github.com/mapero/esphome-hcpbridge) over
the onboard RS485 interface. It exposes the Hormann door as a garage cover,
the garage light as a switch, and HCP connectivity as a diagnostic entity. It
also keeps both onboard relay pulse buttons for an external or property gate.

This upstream component requires the Arduino framework and the
`emelianov/modbus-esp8266` library. Do not casually convert this example to
ESP-IDF. The component also registers Home Assistant API services for close,
half, vent, and open positions; their exact service names are listed in the
YAML comments.

Wire the HCP bus through the board RS485 terminals, not directly to the ESP32.
If `HCPBridge Connected` remains false, first verify motor power and bus mode,
then GPIO15/GPIO6/GPIO7 ownership, and finally RS485 A/B polarity.

### Panasonic Aquarea heat pump

`examples/koniec_swiata_iot_panasonic_heat_pump.yaml` uses the
[`ElVit/esphome_components` Panasonic component](https://github.com/ElVit/esphome_components/tree/main/components/panasonic_heatpump).
It exposes the component's TOP/SET measurements, binary and text states,
setpoint numbers, switches, selects, two climate zones, and domestic hot water
control.

UART1 connects to the heat pump CN-CNT port at 9600 baud, 8 data bits, even
parity, and 1 stop bit. UART2 is the optional passthrough for CZ-TAW1 or another
CN-CNT client, using the same format. Both sides require the isolated UART HAT.
Set `log_uart_msg: true` temporarily to inspect 203-byte Panasonic frames;
return it to `false` after testing because it is deliberately verbose.

### Thessla Green AirPack4

`examples/koniec_swiata_iot_thessla_green.yaml` uses ESPHome's standard
`modbus` and `modbus_controller` components through the onboard RS485 adapter.
It is configured as a Modbus RTU client at 9600 baud, 8E1, slave address 1,
with GPIO7 as flow control and a 10-second controller update interval.

The file implements the useful values from the official AirPack4 register
table as read-only sensors, status/alarm binary sensors, selects, numbers, and
safe user-facing switches. Potentially destructive or awkward registers are
documented but left inactive: communication settings, product/access locks,
service calibration, reset, packed schedules, dates, and multi-register
values. Read the comments near the bottom of the YAML before adding them.

Confirm the ventilation unit's slave address, serial format, and firmware
register map before enabling control. Begin with read-only entities and watch
the logs for valid responses before changing holding registers.

## Build, flash, and logs

ESPHome 2026.5.3 was used during board testing. Pinning the container version
makes later rebuilds reproducible.

### Native ESPHome

Validate and compile without flashing:

```sh
esphome config examples/koniec_swiata_iot.yaml
esphome compile examples/koniec_swiata_iot.yaml
```

Compile, flash over USB, and attach logs:

```sh
esphome run examples/koniec_swiata_iot.yaml --device /dev/ttyACM0
```

Use `/dev/ttyUSB0` instead when that is how the USB serial interface appears.
For an OTA upload, replace the device with its IP address or `.local` name.

### Podman or Docker

Podman example for USB flashing:

```sh
podman run --rm -it --privileged --network=host \
  -v "$PWD":/config -v /run/udev:/run/udev:ro \
  ghcr.io/esphome/esphome:2026.5.3 \
  run examples/koniec_swiata_iot.yaml --device /dev/ttyACM0
```

Use `docker` in place of `podman` when appropriate. On an SELinux host, add
`:Z` to the `/config` mount if the container cannot read the repository. For a
less permissive setup, replace `--privileged` with an explicit
`--device=/dev/ttyACM0`; USB discovery and reset behavior vary by host.

Compile-only validation does not require USB access:

```sh
podman run --rm -it -v "$PWD":/config \
  ghcr.io/esphome/esphome:2026.5.3 \
  compile examples/koniec_swiata_iot.yaml
```

### Production credentials

The examples favor first-boot simplicity: API encryption, OTA authentication,
and web-server authentication are not configured. Treat them as trusted-LAN
examples. Before exposing a controller beyond an isolated home network, add
ESPHome API encryption, an OTA password, and web-server authentication using
values from `examples/secrets.yaml`. Never expose the web server or native API
directly to the public Internet.

## Common modifications

### Use Ethernet polling instead of the interrupt

GPIO9 interrupt operation is retained in the examples because it is part of
the board interface. On the tested board, however, interrupt mode has sometimes
obtained a DHCP address while failing to answer ARP or ping. Ten-millisecond
polling restored reliable traffic.

Use exactly one of `interrupt_pin` and `polling_interval`:

```yaml
ethernet:
  type: W6100
  # ...SPI and diagnostic pins...
  # interrupt_pin: GPIO9
  polling_interval: 10ms
```

Do not set both; configuration validation rejects that combination. A 100 ms
interval adds significant packet latency and was not the known-good setting,
so 10 ms is the recommended troubleshooting value.

### Change relay pulse duration

Change the `delay` between `output.turn_on` and `output.turn_off` under the
corresponding template button. For a 500 ms press:

```yaml
- output.turn_on: relay_1_output
- delay: 500ms
- output.turn_off: relay_1_output
```

To disable relays, remove both relay GPIO outputs, the relay feedback script,
and the template buttons. Keep `status_led_idle` and `status_led_output` if the
idle LED is still wanted.

### Configure a static Ethernet address

Add this beneath `ethernet:` and choose values valid for the local network:

```yaml
manual_ip:
  static_ip: 172.16.0.181
  gateway: 172.16.0.1
  subnet: 255.255.255.0
  dns1: 172.16.0.1
```

### Stop rebooting when Home Assistant is unavailable

All current examples already use:

```yaml
api:
  reboot_timeout: 0s
```

This disables the API connection watchdog. It does not disable hardware
watchdogs or reboot recovery for genuine firmware faults.

## Troubleshooting

### Ethernet gets an address but does not answer ping or ARP

Switch from `interrupt_pin: GPIO9` to `polling_interval: 10ms`, rebuild, and
test again. This exact failure has occurred on the board. Also confirm that no
other component owns GPIO9 or the SPI pins.

### Ethernet never gets an address

Check the RJ45 link LEDs, cable, switch port, power, and the nSPD/nDPX states in
the ESPHome startup log. Confirm all SPI pins and CS against the table above.
Then try 10 ms polling. If a static address is configured, verify its subnet,
gateway, and that no other host uses it.

### ESP-IDF reports multicast-filter errors

Messages such as these are expected with the current MACRAW driver:

```text
add mac address to filter not supported
failed to add mac filter
Failed to add multicast filter for IPv4
```

The W6100 driver cannot implement ESP-IDF's requested Ethernet MAC multicast
filter exactly. This is not serious for unicast traffic, DHCP, ping, ESPHome
API, OTA, or the web server. It can affect multicast discovery such as mDNS;
use the device IP address if `<name>.local` discovery is unreliable.

### Safe mode or a boot loop appears

Capture the complete serial log from reset onward. Temporarily remove optional
application integrations while retaining logger, API, and the network block.
If Ethernet setup is implicated, use the base example and 10 ms polling. A
safe-mode warning can remain after earlier crashes until a full successful
boot interval has elapsed; the first goal is a stable serial log, not repeated
blind reflashing.

### LM75B changes only in 0.5 deg C steps

That is correct for the fitted 9-bit LM75B. The displayed two decimal places
are formatting. I2C scan should show address `0x48`; if it does not, verify
GPIO8/GPIO18 and board power.

### Modbus devices do not respond

Verify baud rate, parity, stop bits, slave address, and A/B polarity. Confirm
GPIO7 is used as the RS485 flow-control pin. Increase logger verbosity only
while diagnosing; excessive UART or protocol logging can obscure timing
problems. Start with one or two read-only registers before enabling a large
configuration or any writes.

### Home Assistant is unavailable and the board stays running

This is intentional. `api.reboot_timeout: 0s` prevents a disconnected Home
Assistant instance from rebooting the controller. Use the serial log, web UI,
device IP, or OTA endpoint to inspect it independently.
