import { appendFileSync, existsSync, readFileSync } from "fs";
import { join, relative, sep } from "path";

import { getSettings } from "../config.js";
import { getDbPool } from "../db.js";
import { ensureDir } from "../http.js";
import { normalizeRecord, sanitizeSegment } from "./telemetryParser.js";

function storagePathFor(device) {
  const { dataDir } = getSettings();
  const receivedAt = new Date();
  const year = String(receivedAt.getUTCFullYear());
  const month = String(receivedAt.getUTCMonth() + 1).padStart(2, "0");
  const day = String(receivedAt.getUTCDate()).padStart(2, "0");
  const datePart = `${year}-${month}-${day}`;
  const sensorKey = sanitizeSegment(device.device_id, "unknown_sensor");
  const dir = join(dataDir, sensorKey);
  ensureDir(dir);
  return join(dir, `${datePart}.jsonl`);
}

function archiveDateForPath(filePath) {
  const match = filePath.match(/(\d{4}-\d{2}-\d{2})\.jsonl$/);
  return match ? match[1] : new Date().toISOString().slice(0, 10);
}

function archiveRelativePath(filePath) {
  const { serverRoot } = getSettings();
  return relative(serverRoot, filePath).split(sep).join("/");
}

function countLines(filePath) {
  if (!existsSync(filePath)) {
    return 0;
  }
  const content = readFileSync(filePath, "utf8");
  if (!content) {
    return 0;
  }
  return content.endsWith("\n")
    ? content.split("\n").length - 1
    : content.split("\n").length;
}

function buildTelemetryTableRows(devicePk, normalizedRecords) {
  const samples = [];
  const locations = [];
  const gasReadings = [];
  const heaterEvents = [];

  for (const record of normalizedRecords) {
    samples.push([
      devicePk,
      record.sample_uuid,
      record.sample_id,
      record.timestamp_ms,
      record.uptime_ms,
      record.time_synced ? 1 : 0,
      record.source_ip,
    ]);

    locations.push([
      record.sample_uuid,
      record.location_name,
      record.latitude,
      record.longitude,
    ]);

    gasReadings.push(
      [
        record.sample_uuid,
        "nh3",
        record.nh3_raw,
        record.nh3_mv,
        record.nh3_res_ohms,
        record.nh3_ppm,
        record.nh3_ppm_valid ? 1 : 0,
      ],
      [
        record.sample_uuid,
        "red",
        record.red_raw,
        record.red_mv,
        record.red_res_ohms,
        record.red_ppm,
        record.red_ppm_valid ? 1 : 0,
      ],
      [
        record.sample_uuid,
        "ox",
        record.ox_raw,
        record.ox_mv,
        record.ox_res_ohms,
        record.ox_ppm,
        record.ox_ppm_valid ? 1 : 0,
      ]
    );

    heaterEvents.push([
      record.sample_uuid,
      record.heater_on,
      record.warmup,
      record.since_change,
    ]);
  }

  return {
    gasReadings,
    heaterEvents,
    locations,
    samples,
  };
}

async function upsertArchiveFile(devicePk, filePath) {
  const relativePath = archiveRelativePath(filePath);
  const archiveDate = archiveDateForPath(filePath);
  const [result] = await getDbPool().execute(
    `INSERT INTO telemetry_archive_files (
      device_id, relative_path, archive_date
    ) VALUES (?, ?, ?)
    ON DUPLICATE KEY UPDATE
      id = LAST_INSERT_ID(id),
      archive_date = VALUES(archive_date),
      updated_at = CURRENT_TIMESTAMP`,
    [devicePk, relativePath, archiveDate]
  );

  return {
    archiveFileId: result.insertId,
    relativePath,
  };
}

function buildArchiveEntryRows(archiveFileId, normalizedRecords, firstLineNumber) {
  return normalizedRecords.map((record, index) => [
    record.sample_uuid,
    archiveFileId,
    firstLineNumber + index,
  ]);
}

async function recordArchiveEntries(archiveFileId, normalizedRecords, firstLineNumber) {
  const rows = buildArchiveEntryRows(archiveFileId, normalizedRecords, firstLineNumber);
  await getDbPool().query(
    `INSERT INTO telemetry_archive_entries (
      sample_uuid, archive_file_id, line_number
    ) VALUES ?
    ON DUPLICATE KEY UPDATE
      archive_file_id = VALUES(archive_file_id),
      line_number = VALUES(line_number)`,
    [rows]
  );
}

async function persistTelemetryRecords(device, records, req) {
  const normalizedRecords = records.map((record) => normalizeRecord(record, req, device));
  const rows = buildTelemetryTableRows(device.id, normalizedRecords);
  const pool = getDbPool();

  await pool.query(
    `INSERT INTO telemetry_samples (
      device_id, sample_uuid, sample_id, timestamp_ms, uptime_ms, time_synced, source_ip
    ) VALUES ?
    ON DUPLICATE KEY UPDATE
      sample_id = VALUES(sample_id),
      timestamp_ms = VALUES(timestamp_ms),
      uptime_ms = VALUES(uptime_ms),
      time_synced = VALUES(time_synced),
      source_ip = VALUES(source_ip),
      received_at = CURRENT_TIMESTAMP`,
    [rows.samples]
  );

  await pool.query(
    `INSERT INTO telemetry_locations (
      sample_uuid, location_name, latitude, longitude
    ) VALUES ?
    ON DUPLICATE KEY UPDATE
      location_name = VALUES(location_name),
      latitude = VALUES(latitude),
      longitude = VALUES(longitude)`,
    [rows.locations]
  );

  await pool.query(
    `INSERT INTO telemetry_gas_readings (
      sample_uuid, gas_type, raw, mv, resistance_ohms, ppm, ppm_valid
    ) VALUES ?
    ON DUPLICATE KEY UPDATE
      raw = VALUES(raw),
      mv = VALUES(mv),
      resistance_ohms = VALUES(resistance_ohms),
      ppm = VALUES(ppm),
      ppm_valid = VALUES(ppm_valid)`,
    [rows.gasReadings]
  );

  await pool.query(
    `INSERT INTO telemetry_heater_events (
      sample_uuid, heater_on, warmup, since_change
    ) VALUES ?
    ON DUPLICATE KEY UPDATE
      heater_on = VALUES(heater_on),
      warmup = VALUES(warmup),
      since_change = VALUES(since_change)`,
    [rows.heaterEvents]
  );

  const filePath = storagePathFor(device);
  const firstLineNumber = countLines(filePath) + 1;
  const lines = normalizedRecords.map((record) => JSON.stringify(record)).join("\n") + "\n";
  appendFileSync(filePath, lines, "utf8");
  const archive = await upsertArchiveFile(device.id, filePath);
  await recordArchiveEntries(archive.archiveFileId, normalizedRecords, firstLineNumber);

  return {
    archiveFileId: archive.archiveFileId,
    filePath,
    relativePath: archive.relativePath,
  };
}

async function findUploadReceipt(devicePk, sourceFile, startOffset, endOffset) {
  const [rows] = await getDbPool().execute(
    `SELECT r.accepted_records, r.archive_file_id, f.relative_path
     FROM telemetry_upload_receipts r
     LEFT JOIN telemetry_archive_files f ON f.id = r.archive_file_id
     WHERE r.device_id = ? AND r.source_file = ? AND r.start_offset = ? AND r.end_offset = ?
     LIMIT 1`,
    [devicePk, sourceFile, startOffset, endOffset]
  );
  return rows[0] || null;
}

async function recordUploadReceipt(devicePk, sourceFile, startOffset, endOffset, acceptedRecords, archiveFileId = null) {
  await getDbPool().execute(
    `INSERT INTO telemetry_upload_receipts (
      device_id, source_file, start_offset, end_offset, accepted_records, archive_file_id
    ) VALUES (?, ?, ?, ?, ?, ?)
    ON DUPLICATE KEY UPDATE
      accepted_records = VALUES(accepted_records),
      archive_file_id = VALUES(archive_file_id)`,
    [devicePk, sourceFile, startOffset, endOffset, acceptedRecords, archiveFileId]
  );
}

export {
  buildArchiveEntryRows,
  buildTelemetryTableRows,
  findUploadReceipt,
  persistTelemetryRecords,
  recordUploadReceipt,
};
