import { getSettings } from "../config.js";
import { clientIp, json, parseJsonRequest } from "../http.js";
import { authenticateDevice, registerDevice } from "../services/devices.js";

async function handleDeviceRegistration(req, res) {
  const body = await parseJsonRequest(req, res);
  if (!body) {
    return;
  }

  const deviceId = String(body.device_id || "").trim();
  const deviceName = String(body.device_name || "").trim();
  const locationName = String(body.location_name || "").trim();
  const registrationSecret = String(body.registration_secret || "").trim();
  const { registrationSecret: configuredSecret } = getSettings();

  if (!deviceId) {
    return json(res, 400, { ok: false, error: "device_id is required" });
  }
  if (!configuredSecret) {
    return json(res, 500, { ok: false, error: "registration secret is not configured on server" });
  }
  if (registrationSecret !== configuredSecret) {
    return json(res, 401, { ok: false, error: "invalid registration secret" });
  }

  const apiToken = await registerDevice({ deviceId, deviceName, locationName });
  console.log(`[register] device_id=${deviceId} ip=${clientIp(req)}`);

  return json(res, 200, {
    ok: true,
    device_id: deviceId,
    api_token: apiToken,
  });
}

async function handleDeviceAuth(req, res) {
  const body = await parseJsonRequest(req, res);
  if (!body) {
    return;
  }

  const deviceId = String(body.device_id || "").trim();
  const apiToken = String(body.api_token || "").trim();

  if (!deviceId || !apiToken) {
    return json(res, 400, { ok: false, error: "device_id and api_token are required" });
  }

  const device = await authenticateDevice(deviceId, apiToken);
  if (!device) {
    console.warn(`[auth] rejected device_id=${deviceId} ip=${clientIp(req)}`);
    return json(res, 401, { ok: false, error: "invalid device credentials" });
  }

  console.log(`[auth] ok device_id=${device.device_id} ip=${clientIp(req)}`);
  return json(res, 200, {
    ok: true,
    device_id: device.device_id,
    device_name: device.device_name,
    location_name: device.location_name,
  });
}

export { handleDeviceAuth, handleDeviceRegistration };
