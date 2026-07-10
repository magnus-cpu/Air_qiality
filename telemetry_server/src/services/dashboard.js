import { getDbPool } from "../db.js";

const VALID_GASES = new Set(["nh3", "red", "ox"]);

function parseLimit(value, fallback, max) {
  const parsed = Number.parseInt(String(value ?? ""), 10);
  if (!Number.isFinite(parsed) || parsed <= 0) {
    return fallback;
  }
  return Math.min(parsed, max);
}

function parseTimestamp(value) {
  const parsed = Number.parseInt(String(value ?? ""), 10);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : null;
}

function normalizeGas(value) {
  const gas = String(value || "all").toLowerCase();
  return VALID_GASES.has(gas) ? gas : "all";
}

async function listDevices() {
  const [rows] = await getDbPool().execute(
    `SELECT
       d.id,
       d.device_id,
       d.device_name,
       d.location_name,
       d.active,
       d.registered_at,
       d.last_seen_at,
       MAX(s.timestamp_ms) AS latest_timestamp_ms,
       MAX(s.received_at) AS latest_received_at
     FROM devices d
     LEFT JOIN telemetry_samples s ON s.device_id = d.id
     GROUP BY d.id
     ORDER BY COALESCE(MAX(s.received_at), d.registered_at) DESC`
  );
  return rows;
}

async function findDevicePk(deviceId) {
  if (!deviceId) {
    const [rows] = await getDbPool().execute(
      `SELECT id
       FROM devices
       WHERE active = 1
       ORDER BY COALESCE(last_seen_at, registered_at) DESC
       LIMIT 1`
    );
    return rows[0]?.id || null;
  }

  const [rows] = await getDbPool().execute(
    `SELECT id
     FROM devices
     WHERE device_id = ?
     LIMIT 1`,
    [deviceId]
  );
  return rows[0]?.id || null;
}

async function latestTelemetry({ deviceId, limit }) {
  const devicePk = await findDevicePk(deviceId);
  if (!devicePk) {
    return [];
  }

  const rowLimit = parseLimit(limit, 30, 200);
  const [rows] = await getDbPool().execute(
    `SELECT
       s.sample_uuid,
       s.sample_id,
       s.timestamp_ms,
       s.uptime_ms,
       s.time_synced,
       s.received_at,
       s.source_ip,
       d.device_id,
       d.device_name,
       l.location_name,
       l.latitude,
       l.longitude,
       h.heater_on,
       h.warmup,
       h.since_change,
       MAX(CASE WHEN g.gas_type = 'nh3' THEN g.ppm END) AS nh3_ppm,
       MAX(CASE WHEN g.gas_type = 'red' THEN g.ppm END) AS red_ppm,
       MAX(CASE WHEN g.gas_type = 'ox' THEN g.ppm END) AS ox_ppm,
       MAX(CASE WHEN g.gas_type = 'nh3' THEN g.ppm_valid END) AS nh3_ppm_valid,
       MAX(CASE WHEN g.gas_type = 'red' THEN g.ppm_valid END) AS red_ppm_valid,
       MAX(CASE WHEN g.gas_type = 'ox' THEN g.ppm_valid END) AS ox_ppm_valid
     FROM telemetry_samples s
     JOIN devices d ON d.id = s.device_id
     LEFT JOIN telemetry_locations l ON l.sample_uuid = s.sample_uuid
     LEFT JOIN telemetry_heater_events h ON h.sample_uuid = s.sample_uuid
     LEFT JOIN telemetry_gas_readings g ON g.sample_uuid = s.sample_uuid
     WHERE s.device_id = ?
     GROUP BY s.sample_uuid
     ORDER BY s.timestamp_ms DESC, s.received_at DESC
     LIMIT ${rowLimit}`,
    [devicePk]
  );
  return rows;
}

async function telemetryTrends({ deviceId, from, to, gas, limit }) {
  const devicePk = await findDevicePk(deviceId);
  if (!devicePk) {
    return [];
  }

  const rowLimit = parseLimit(limit, 1000, 5000);
  const filters = ["s.device_id = ?"];
  const params = [devicePk];
  const fromMs = parseTimestamp(from);
  const toMs = parseTimestamp(to);
  const gasType = normalizeGas(gas);

  if (fromMs !== null) {
    filters.push("s.timestamp_ms >= ?");
    params.push(fromMs);
  }
  if (toMs !== null) {
    filters.push("s.timestamp_ms <= ?");
    params.push(toMs);
  }
  if (gasType !== "all") {
    filters.push("g.gas_type = ?");
    params.push(gasType);
  }

  const [rows] = await getDbPool().execute(
    `SELECT
       s.sample_uuid,
       s.timestamp_ms,
       s.received_at,
       s.time_synced,
       g.gas_type,
       g.ppm,
       g.ppm_valid
     FROM telemetry_samples s
     JOIN telemetry_gas_readings g ON g.sample_uuid = s.sample_uuid
     WHERE ${filters.join(" AND ")}
     ORDER BY s.timestamp_ms ASC, g.gas_type ASC
     LIMIT ${rowLimit}`,
    params
  );
  return rows;
}

export {
  latestTelemetry,
  listDevices,
  telemetryTrends,
};
