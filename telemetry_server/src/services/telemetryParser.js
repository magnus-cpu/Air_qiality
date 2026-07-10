const TELEMETRY_CSV_HEADER = [
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

const LEGACY_TELEMETRY_CSV_HEADER = TELEMETRY_CSV_HEADER
  .split(",")
  .slice(1)
  .join(",");

function sanitizeSegment(value, fallback) {
  const normalized = String(value || "")
    .trim()
    .replace(/[^a-zA-Z0-9._-]+/g, "_")
    .replace(/^_+|_+$/g, "");
  return normalized || fallback;
}

function parseCsvInteger(value, fieldName) {
  const parsed = Number.parseInt(String(value), 10);
  if (!Number.isFinite(parsed)) {
    throw new Error(`${fieldName} must be an integer`);
  }
  return parsed;
}

function parseCsvFloat(value, fieldName) {
  const parsed = Number.parseFloat(String(value));
  if (!Number.isFinite(parsed)) {
    throw new Error(`${fieldName} must be a number`);
  }
  return parsed;
}

function parseCsvBoolInt(value, fieldName) {
  const normalized = String(value).trim();
  if (normalized !== "0" && normalized !== "1") {
    throw new Error(`${fieldName} must be 0 or 1`);
  }
  return normalized === "1";
}

function parseCsvTelemetryBody(rawBody) {
  const records = [];
  const lines = String(rawBody || "").split(/\r?\n/);

  for (let i = 0; i < lines.length; i += 1) {
    const line = lines[i].trim();
    if (!line) {
      continue;
    }
    if (line === TELEMETRY_CSV_HEADER || line === LEGACY_TELEMETRY_CSV_HEADER) {
      continue;
    }

    const fields = line.split(",");
    const hasSampleUuid = fields.length === 26;
    const offset = hasSampleUuid ? 1 : 0;
    if (fields.length !== 25 && fields.length !== 26) {
      throw new Error(`csv line ${i + 1} has ${fields.length} fields, expected 25 or 26`);
    }

    records.push({
      sample_uuid: hasSampleUuid ? fields[0] || "" : "",
      sample_id: parseCsvInteger(fields[offset + 0], "sample_id"),
      timestamp_ms: parseCsvInteger(fields[offset + 1], "timestamp_ms"),
      uptime_ms: parseCsvInteger(fields[offset + 2], "uptime_ms"),
      location_name: fields[offset + 3] || "",
      latitude: fields[offset + 4] || "",
      longitude: fields[offset + 5] || "",
      time_synced: parseCsvBoolInt(fields[offset + 6], "time_synced"),
      nh3_raw: parseCsvInteger(fields[offset + 7], "nh3_raw"),
      red_raw: parseCsvInteger(fields[offset + 8], "red_raw"),
      ox_raw: parseCsvInteger(fields[offset + 9], "ox_raw"),
      nh3_mv: parseCsvInteger(fields[offset + 10], "nh3_mv"),
      red_mv: parseCsvInteger(fields[offset + 11], "red_mv"),
      ox_mv: parseCsvInteger(fields[offset + 12], "ox_mv"),
      nh3_res_ohms: parseCsvFloat(fields[offset + 13], "nh3_res_ohms"),
      red_res_ohms: parseCsvFloat(fields[offset + 14], "red_res_ohms"),
      ox_res_ohms: parseCsvFloat(fields[offset + 15], "ox_res_ohms"),
      nh3_ppm: parseCsvFloat(fields[offset + 16], "nh3_ppm"),
      red_ppm: parseCsvFloat(fields[offset + 17], "red_ppm"),
      ox_ppm: parseCsvFloat(fields[offset + 18], "ox_ppm"),
      nh3_ppm_valid: parseCsvBoolInt(fields[offset + 19], "nh3_ppm_valid"),
      red_ppm_valid: parseCsvBoolInt(fields[offset + 20], "red_ppm_valid"),
      ox_ppm_valid: parseCsvBoolInt(fields[offset + 21], "ox_ppm_valid"),
      heater_on: parseCsvInteger(fields[offset + 22], "heater_on"),
      warmup: parseCsvInteger(fields[offset + 23], "warmup"),
      since_change: parseCsvInteger(fields[offset + 24], "since_change"),
    });
  }

  return records;
}

function legacySampleUuid(record, device) {
  return [
    "legacy",
    sanitizeSegment(device.device_id, "unknown_sensor"),
    record.sample_id,
    record.timestamp_ms,
    record.uptime_ms,
  ].join("-");
}

function normalizeRecord(record, req, device) {
  const sampleUuid = String(record.sample_uuid || "").trim() || legacySampleUuid(record, device);
  const forwarded = req.headers["x-forwarded-for"];
  const sourceIp = typeof forwarded === "string" && forwarded.length > 0
    ? forwarded.split(",")[0].trim()
    : req.socket.remoteAddress || "";

  return {
    received_at: new Date().toISOString(),
    source_ip: sourceIp,
    device_id: device.device_id,
    device_name: device.device_name || "",
    sample_uuid: sampleUuid,
    location_name: record.location_name || device.location_name || "",
    latitude: record.latitude || "",
    longitude: record.longitude || "",
    sample_id: record.sample_id,
    timestamp_ms: record.timestamp_ms,
    uptime_ms: record.uptime_ms,
    time_synced: Boolean(record.time_synced),
    nh3_raw: Number(record.nh3_raw),
    red_raw: Number(record.red_raw),
    ox_raw: Number(record.ox_raw),
    nh3_mv: Number(record.nh3_mv),
    red_mv: Number(record.red_mv),
    ox_mv: Number(record.ox_mv),
    nh3_res_ohms: Number(record.nh3_res_ohms),
    red_res_ohms: Number(record.red_res_ohms),
    ox_res_ohms: Number(record.ox_res_ohms),
    nh3_ppm: Number(record.nh3_ppm),
    red_ppm: Number(record.red_ppm),
    ox_ppm: Number(record.ox_ppm),
    nh3_ppm_valid: Boolean(record.nh3_ppm_valid),
    red_ppm_valid: Boolean(record.red_ppm_valid),
    ox_ppm_valid: Boolean(record.ox_ppm_valid),
    heater_on: Number(record.heater_on),
    warmup: Number(record.warmup),
    since_change: Number(record.since_change),
  };
}

function validateRecord(record) {
  if (!record || typeof record !== "object") {
    return "record must be an object";
  }
  if (typeof record.sample_id !== "number") {
    return "sample_id is required";
  }
  if (record.sample_uuid !== undefined &&
      (typeof record.sample_uuid !== "string" || record.sample_uuid.trim() === "")) {
    return "sample_uuid must be a non-empty string";
  }
  if (typeof record.timestamp_ms !== "number") {
    return "timestamp_ms is required";
  }
  if (typeof record.uptime_ms !== "number") {
    return "uptime_ms is required";
  }
  return null;
}

export {
  TELEMETRY_CSV_HEADER,
  normalizeRecord,
  parseCsvTelemetryBody,
  sanitizeSegment,
  validateRecord,
};
