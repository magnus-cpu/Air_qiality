# Sensor Calculation Notes

This project currently uses a simple one-point calibration model for each gas channel.

## 1. ADC raw to millivolts

Each sensor output is first read with the ESP-IDF ADC one-shot driver:

- `NH3` on `ADC_CHANNEL_6`
- `RED` on `ADC_CHANNEL_0`
- `OX` on `ADC_CHANNEL_7`

The raw ADC code is converted to millivolts with the ESP-IDF ADC calibration layer:

`mv = adc_cali_raw_to_voltage(raw)`

If ADC calibration is unavailable or the read fails, the millivolt value is reported as `-1`.

## 2. Millivolts to sensor resistance

The code models each analog output as a voltage divider between the gas sensor resistance `Rs`
and a fixed load resistor `RL`.

Current constants in [sensor.c](/home/magnus/esp_projects/Air_qiality/main/sensor.c:18):

- `SENSOR_SUPPLY_MV = 3300`
- `SENSOR_LOAD_RESISTOR_OHMS = 56000`

Using the divider equation:

`Vout = Vs * RL / (Rs + RL)`

Rearranged for sensor resistance:

`Rs = RL * (Vs - Vout) / Vout`

That is exactly what the firmware computes:

`resistance_ohms = SENSOR_LOAD_RESISTOR_OHMS * ((SENSOR_SUPPLY_MV - mv) / mv)`

The resistance result is treated as invalid when:

- `mv <= 0`
- `mv >= SENSOR_SUPPLY_MV`

## 3. One-point calibration

When calibration is performed, the firmware captures for each gas:

- the current measured resistance `Rref`
- the user-entered known concentration `Cref` in ppm

Those values are stored in NVS in the `sensor` namespace under key `cal`.

Stored per-gas calibration data:

- `valid`
- `reference_resistance_ohms`
- `reference_ppm`

## 4. Current ppm estimate

The current implementation uses a first-pass inverse-resistance model:

`ppm = Cref * (Rref / Rs)`

Where:

- `Cref` is the calibration concentration entered by the user
- `Rref` is the resistance measured during calibration
- `Rs` is the current live resistance

This assumes concentration is inversely proportional to sensor resistance around the
calibration point.

## 5. Important limitation

This is a practical placeholder model, not a full sensor-characterization curve.

Many gas sensors are better modeled with a log-log curve such as:

`ppm = A * (Rs / R0)^B`

where `A` and `B` come from datasheet fitting or multi-point calibration.

So the current approach is useful for:

- getting a stable stored baseline
- showing trend-aware ppm-like numbers
- keeping telemetry format ready for future improvement

But it should not yet be treated as a lab-grade concentration model.

## 6. Files involved

- [main/sensor.c](/home/magnus/esp_projects/Air_qiality/main/sensor.c:1)
- [main/sensor.h](/home/magnus/esp_projects/Air_qiality/main/sensor.h:1)
- [main/web_server.c](/home/magnus/esp_projects/Air_qiality/main/web_server.c:545)
- [main/telemetry_pipeline.c](/home/magnus/esp_projects/Air_qiality/main/telemetry_pipeline.c:1)
