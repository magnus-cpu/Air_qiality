# Telemetry Upload Server

Node.js telemetry receiver for the ESP32 air-quality sensor with:

- device registration
- device authentication
- MySQL storage through `mysql2`
- JSON Lines archive files on disk

## Endpoints

- `GET /health`
- `POST /api/devices/register`
- `POST /api/devices/auth`
- `POST /api/telemetry`

## Install

```bash
cd telemetry_server
npm install
```

## Environment

```bash
PORT=3000
HOST=0.0.0.0
DB_HOST=127.0.0.1
DB_PORT=3306
DB_USER=root
DB_PASSWORD=your_password
DB_NAME=air_quality
DB_POOL_SIZE=10
DEVICE_REGISTRATION_SECRET=change-this-secret
DATA_DIR=./data
MAX_BODY_BYTES=8388608
```

## Start

```bash
cd telemetry_server
PORT=3000 \
DB_HOST=127.0.0.1 \
DB_USER=root \
DB_PASSWORD=your_password \
DB_NAME=air_quality \
DEVICE_REGISTRATION_SECRET=change-this-secret \
node server.js
```

## Server structure

The server is split by responsibility:

```text
server.js                 # entrypoint
src/app.js                # startup wiring
src/routes.js             # HTTP method/path dispatch
src/controllers/          # endpoint handlers
src/services/             # device, parser, and storage logic
src/db.js                 # MySQL pool, schema creation, migrations
src/config.js             # environment-backed settings
src/http.js               # request/response helpers
```

## Database

The server auto-creates its tables on startup. The schema is also provided in [schema.sql](/home/magnus/esp_projects/Air_qiality/telemetry_server/schema.sql:1).

Tables:

- `devices`
- `telemetry_archive_files`
- `telemetry_samples`
- `telemetry_archive_entries`
- `telemetry_locations`
- `telemetry_gas_readings`
- `telemetry_heater_events`
- `telemetry_upload_receipts`

The legacy wide `telemetry_records` table is no longer used. On startup, the server migrates any existing `telemetry_records` rows into the split tables and then drops the legacy table.

`telemetry_samples.sample_uuid` is the trusted sample identity. The ESP32 creates it when the sample is taken using:

```text
<device_id>-<64-bit_boot_id_hex>-<sample_sequence_hex>
```

Example:

```text
AQ-E05A1B56110C-8F21A3BC91D04E77-0000000000000123
```

The database deduplicates on `sample_uuid`. `timestamp_ms` is still stored in `telemetry_samples` for charting and filtering, but it is not used as the unique identity because ESP32 time can be unsynced, estimated, or corrected after SNTP.

The sample data is split by function:

- `telemetry_samples`: identity, device, timestamps, receive metadata
- `telemetry_archive_files`: JSONL archive files written on disk
- `telemetry_archive_entries`: links each `sample_uuid` to an archive file and line number
- `telemetry_locations`: location metadata for the sample
- `telemetry_gas_readings`: one row per gas reading (`nh3`, `red`, `ox`)
- `telemetry_heater_events`: heater state for the sample

## Device registration

Register a device once and the server returns its API token.

```bash
curl -X POST http://localhost:3000/api/devices/register \
  -H 'Content-Type: application/json' \
  -d '{
    "device_id": "AQ-001",
    "device_name": "Village Monitor 1",
    "location_name": "Kisarawe Station",
    "registration_secret": "change-this-secret"
  }'
```

Example response:

```json
{
  "ok": true,
  "device_id": "AQ-001",
  "api_token": "generated_device_token_here"
}
```

## Device auth check

```bash
curl -X POST http://localhost:3000/api/devices/auth \
  -H 'Content-Type: application/json' \
  -d '{
    "device_id": "AQ-001",
    "api_token": "generated_device_token_here"
  }'
```

## Telemetry upload

After registration, set the firmware token as:

```c
#define TELEMETRY_UPLOAD_URL "http://YOUR_SERVER_IP:3000/api/telemetry"
#define TELEMETRY_UPLOAD_API_KEY "Bearer generated_device_token_here"
```

The firmware then uploads with the `Authorization` header automatically.

Example upload:

```bash
curl -X POST http://localhost:3000/api/telemetry \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer generated_device_token_here' \
  -d '{
    "records": [
      {
        "sample_uuid": "AQ-001-8F21A3BC91D04E77-0000000000000001",
        "sample_id": 1,
        "timestamp_ms": 1746451200000,
        "uptime_ms": 10000,
        "location_name": "Kisarawe Station",
        "latitude": "-6.9000",
        "longitude": "39.0667",
        "time_synced": true,
        "nh3_raw": 120,
        "red_raw": 115,
        "ox_raw": 121,
        "nh3_mv": 430,
        "red_mv": 420,
        "ox_mv": 440,
        "nh3_res_ohms": 12.3,
        "red_res_ohms": 13.4,
        "ox_res_ohms": 14.5,
        "nh3_ppm": 3.2,
        "red_ppm": 4.1,
        "ox_ppm": 5.4,
        "nh3_ppm_valid": true,
        "red_ppm_valid": true,
        "ox_ppm_valid": true,
        "heater_on": 1,
        "warmup": 0,
        "since_change": 240
      }
    ]
  }'
```

Backlog CSV files can also be posted directly now:

```bash
curl -X POST http://localhost:3000/api/telemetry \
  -H 'Content-Type: text/csv' \
  -H 'Authorization: Bearer generated_device_token_here' \
  -H 'X-Telemetry-File-Name: tm260513.csv' \
  -H 'X-Telemetry-Start-Offset: 0' \
  --data-binary $'sample_uuid,sample_id,timestamp_ms,uptime_ms,location_name,latitude,longitude,time_synced,nh3_raw,red_raw,ox_raw,nh3_mv,red_mv,ox_mv,nh3_res_ohms,red_res_ohms,ox_res_ohms,nh3_ppm,red_ppm,ox_ppm,nh3_ppm_valid,red_ppm_valid,ox_ppm_valid,heater_on,warmup,since_change\nAQ-001-8F21A3BC91D04E77-0000000000000001,1,1746451200000,10000,Kisarawe Station,-6.9000,39.0667,1,120,115,121,430,420,440,12.300,13.400,14.500,3.200,4.100,5.400,1,1,1,1,0,240\n'
```

The firmware backlog uploader now streams the remaining file to the server in one request from the saved offset instead of rebuilding many small JSON batches. The server also records completed CSV uploads by `device + file name + start/end offset`, so if a request succeeds but the device misses the response and retries, the server can acknowledge it without storing the same file segment twice. CSV receipts also keep the archive file ID for the JSONL file that received the accepted records.

## Storage layout

Uploads are stored in two places:

1. MySQL in the split telemetry tables
2. Disk archive in `telemetry_server/data/<device_id>/YYYY-MM-DD.jsonl`

Example:

```text
telemetry_server/data/AQ-001/2026-05-05.jsonl
```

Each line is one normalized JSON record with:

- `received_at`
- `source_ip`
- `device_id`
- `device_name`
- `sample_uuid`

The database keeps the archive link in `telemetry_archive_files` and `telemetry_archive_entries`. To find the archived JSONL line for a sample, join `telemetry_samples.sample_uuid` to `telemetry_archive_entries.sample_uuid`, then join `telemetry_archive_entries.archive_file_id` to `telemetry_archive_files.id`.

## Health check

```bash
curl http://localhost:3000/health
```

## Dashboard read API

The React dashboard uses these read endpoints:

```bash
curl http://localhost:3000/api/devices
curl 'http://localhost:3000/api/telemetry/latest?device_id=AQ-001&limit=30'
curl 'http://localhost:3000/api/telemetry/trends?device_id=AQ-001&gas=all&from=1746451200000'
```

Trend rows come from `telemetry_samples` joined to `telemetry_gas_readings`.
