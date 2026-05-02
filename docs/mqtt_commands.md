# MQTT Command Reference

This firmware can publish realtime air-quality telemetry over MQTT and receive simple commands.

## Configuration

Configure MQTT from the device setup web page.

- Broker URI example: `mqtt://192.168.1.10:1883`
- Default base topic: `air_quality`
- Device ID: generated from the ESP32 Wi-Fi STA MAC address as 12 lowercase hex characters.

The topic structure is:

```text
<base_topic>/<device_id>/<channel>
```

Example for device `aabbccddeeff`:

```text
air_quality/aabbccddeeff/telemetry
air_quality/aabbccddeeff/status
air_quality/aabbccddeeff/cmd
```

## Published Topics

### Telemetry

Topic:

```text
<base_topic>/<device_id>/telemetry
```

Publish rate: every 2 seconds while MQTT is connected.

Example payload:

```json
{
  "device_id": "aabbccddeeff",
  "nh3_raw": 1234,
  "red_raw": 1200,
  "ox_raw": 1180,
  "nh3_mv": 850,
  "red_mv": 820,
  "ox_mv": 810,
  "heater_on": 1,
  "warmup": 0,
  "since_change": 95,
  "wifi_connected": true,
  "ip": "192.168.1.44",
  "sd_card": "mounted: 15272MB"
}
```

Field notes:

- `*_raw`: ADC raw reading.
- `*_mv`: calibrated millivolts, or `-1` if calibration/read failed.
- `heater_on`: `1` means on, `0` means off.
- `warmup`: `1` means heater is still inside the warm-up window.
- `since_change`: seconds since heater state last changed.
- `sd_card`: current SD-card mount/status string.

### Status

Topic:

```text
<base_topic>/<device_id>/status
```

Payload:

```text
online
```

This is published with retain enabled when the MQTT client connects.

## Command Topic

The firmware subscribes to:

```text
<base_topic>/<device_id>/cmd
```

QoS: `1`

After a heater command is accepted, the firmware publishes telemetry immediately.

## Supported Commands

### Turn Heater On

Any of these payloads are accepted:

```text
heater:on
heater=1
1
```

JSON-style payload:

```json
{"heater_on":true}
```

### Turn Heater Off

Any of these payloads are accepted:

```text
heater:off
heater=0
0
```

JSON-style payload:

```json
{"heater_on":false}
```

### Force Telemetry Publish

Any of these payloads are accepted:

```text
publish
status
```

## Recommended Command Structure

For new commands, prefer JSON payloads because they are easier to extend without breaking old clients.

Recommended shape:

```json
{
  "cmd": "heater",
  "value": true,
  "request_id": "optional-client-id-001"
}
```

Current firmware does not require `cmd` or `request_id`; it only checks for the supported payloads above. Keep future commands small and explicit:

```json
{"cmd":"publish"}
{"cmd":"heater","value":false}
```

Recommended rules for adding commands:

- Use the same command topic: `<base_topic>/<device_id>/cmd`.
- Keep command names lowercase.
- Use booleans for binary settings, not strings.
- Publish telemetry after any command that changes device state.
- Add an acknowledgement topic later if clients need command confirmation beyond the telemetry update.

## Mosquitto Examples

Subscribe to telemetry:

```sh
mosquitto_sub -h 192.168.1.10 -t 'air_quality/+/telemetry' -v
```

Turn heater on:

```sh
mosquitto_pub -h 192.168.1.10 -t 'air_quality/aabbccddeeff/cmd' -m 'heater:on'
```

Turn heater off with JSON:

```sh
mosquitto_pub -h 192.168.1.10 -t 'air_quality/aabbccddeeff/cmd' -m '{"heater_on":false}'
```

Force telemetry publish:

```sh
mosquitto_pub -h 192.168.1.10 -t 'air_quality/aabbccddeeff/cmd' -m 'publish'
```
