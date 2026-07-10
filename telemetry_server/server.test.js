import assert from "node:assert/strict";
import test from "node:test";

import {
  normalizeRecord,
  parseCsvTelemetryBody,
  validateRecord,
} from "./src/services/telemetryParser.js";
import {
  buildArchiveEntryRows,
  buildTelemetryTableRows,
} from "./src/services/telemetryStorage.js";
import { createRequestHandler } from "./src/routes.js";

const csvHeader = [
  "sample_uuid",
  "sample_id",
  "timestamp_ms",
  "uptime_ms",
  "location_name",
  "latitude",
  "longitude",
  "time_synced",
  "nh3_raw",
  "red_raw",
  "ox_raw",
  "nh3_mv",
  "red_mv",
  "ox_mv",
  "nh3_res_ohms",
  "red_res_ohms",
  "ox_res_ohms",
  "nh3_ppm",
  "red_ppm",
  "ox_ppm",
  "nh3_ppm_valid",
  "red_ppm_valid",
  "ox_ppm_valid",
  "heater_on",
  "warmup",
  "since_change",
].join(",");

test("CSV telemetry includes trusted sample UUID as the first field", () => {
  const [record] = parseCsvTelemetryBody(`${csvHeader}
AQ-E05A1B56110C-8F21A3BC91D04E77-0000000000000123,7,1778093103795,12345678,Home Lab,-6.8154,39.2803,1,3041,3156,994,2625,2697,963,14400.001,12520.579,135900.312,15.024,12.746,1.222,1,1,1,1,0,240
`);

  assert.equal(record.sample_uuid, "AQ-E05A1B56110C-8F21A3BC91D04E77-0000000000000123");
  assert.equal(validateRecord(record), null);
});

test("normalized telemetry keeps sample UUID for archive and DB storage", () => {
  const normalized = normalizeRecord(
    {
      sample_uuid: "AQ-E05A1B56110C-8F21A3BC91D04E77-0000000000000123",
      sample_id: 7,
      timestamp_ms: 1778093103795,
      uptime_ms: 12345678,
      time_synced: true,
    },
    { headers: {}, socket: { remoteAddress: "127.0.0.1" } },
    { device_id: "AQ-E05A1B56110C", device_name: "Home Lab", location_name: "Home Lab" }
  );

  assert.equal(normalized.sample_uuid, "AQ-E05A1B56110C-8F21A3BC91D04E77-0000000000000123");
  assert.equal(validateRecord(normalized), null);
});

test("telemetry storage rows split sample metadata, location, gases, and heater state", () => {
  const rows = buildTelemetryTableRows(42, [
    {
      sample_uuid: "AQ-E05A1B56110C-8F21A3BC91D04E77-0000000000000123",
      sample_id: 7,
      timestamp_ms: 1778093103795,
      uptime_ms: 12345678,
      location_name: "Home Lab",
      latitude: "-6.8154",
      longitude: "39.2803",
      time_synced: true,
      nh3_raw: 3041,
      red_raw: 3156,
      ox_raw: 994,
      nh3_mv: 2625,
      red_mv: 2697,
      ox_mv: 963,
      nh3_res_ohms: 14400.001,
      red_res_ohms: 12520.579,
      ox_res_ohms: 135900.312,
      nh3_ppm: 15.024,
      red_ppm: 12.746,
      ox_ppm: 1.222,
      nh3_ppm_valid: true,
      red_ppm_valid: true,
      ox_ppm_valid: true,
      heater_on: 1,
      warmup: 0,
      since_change: 240,
      source_ip: "127.0.0.1",
    },
  ]);

  assert.deepEqual(rows.samples[0], [
    42,
    "AQ-E05A1B56110C-8F21A3BC91D04E77-0000000000000123",
    7,
    1778093103795,
    12345678,
    1,
    "127.0.0.1",
  ]);
  assert.deepEqual(rows.locations[0], [
    "AQ-E05A1B56110C-8F21A3BC91D04E77-0000000000000123",
    "Home Lab",
    "-6.8154",
    "39.2803",
  ]);
  assert.deepEqual(rows.heaterEvents[0], [
    "AQ-E05A1B56110C-8F21A3BC91D04E77-0000000000000123",
    1,
    0,
    240,
  ]);
  assert.deepEqual(rows.gasReadings.map((row) => row[1]), ["nh3", "red", "ox"]);
  assert.deepEqual(rows.gasReadings[0], [
    "AQ-E05A1B56110C-8F21A3BC91D04E77-0000000000000123",
    "nh3",
    3041,
    2625,
    14400.001,
    15.024,
    1,
  ]);
});

test("archive entry rows link samples to JSONL file lines", () => {
  const rows = buildArchiveEntryRows(99, [
    { sample_uuid: "AQ-E05A1B56110C-8F21A3BC91D04E77-0000000000000123" },
    { sample_uuid: "AQ-E05A1B56110C-8F21A3BC91D04E77-0000000000000124" },
  ], 12);

  assert.deepEqual(rows, [
    ["AQ-E05A1B56110C-8F21A3BC91D04E77-0000000000000123", 99, 12],
    ["AQ-E05A1B56110C-8F21A3BC91D04E77-0000000000000124", 99, 13],
  ]);
});

test("request handler can be created with dashboard routes", () => {
  assert.equal(typeof createRequestHandler(), "function");
});
