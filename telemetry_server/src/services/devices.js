import { randomBytes } from "crypto";

import { getDbPool } from "../db.js";

function generateToken() {
  return randomBytes(24).toString("hex");
}

async function findDeviceByToken(token) {
  if (!token) {
    return null;
  }

  const [rows] = await getDbPool().execute(
    `SELECT id, device_id, device_name, location_name, api_token, active
     FROM devices
     WHERE api_token = ?
     LIMIT 1`,
    [token]
  );

  if (!rows.length || !rows[0].active) {
    return null;
  }
  return rows[0];
}

async function updateDeviceSeen(devicePk) {
  await getDbPool().execute(
    "UPDATE devices SET last_seen_at = CURRENT_TIMESTAMP WHERE id = ?",
    [devicePk]
  );
}

async function registerDevice({ deviceId, deviceName, locationName }) {
  const apiToken = generateToken();

  await getDbPool().execute(
    `INSERT INTO devices (device_id, device_name, location_name, api_token, active, last_seen_at)
     VALUES (?, ?, ?, ?, 1, CURRENT_TIMESTAMP)
     ON DUPLICATE KEY UPDATE
       device_name = VALUES(device_name),
       location_name = VALUES(location_name),
       api_token = VALUES(api_token),
       active = 1,
       last_seen_at = CURRENT_TIMESTAMP`,
    [deviceId, deviceName, locationName, apiToken]
  );

  return apiToken;
}

async function authenticateDevice(deviceId, apiToken) {
  const [rows] = await getDbPool().execute(
    `SELECT id, device_id, device_name, location_name, active
     FROM devices
     WHERE device_id = ? AND api_token = ?
     LIMIT 1`,
    [deviceId, apiToken]
  );

  if (!rows.length || !rows[0].active) {
    return null;
  }

  await updateDeviceSeen(rows[0].id);
  return rows[0];
}

export {
  authenticateDevice,
  findDeviceByToken,
  registerDevice,
  updateDeviceSeen,
};
