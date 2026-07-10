import { json } from "../http.js";
import {
  latestTelemetry,
  listDevices,
  telemetryTrends,
} from "../services/dashboard.js";

async function handleDashboardDevices(req, res) {
  const devices = await listDevices();
  return json(res, 200, { ok: true, devices });
}

async function handleDashboardLatest(req, res, url) {
  const records = await latestTelemetry({
    deviceId: url.searchParams.get("device_id"),
    limit: url.searchParams.get("limit"),
  });
  return json(res, 200, { ok: true, records });
}

async function handleDashboardTrends(req, res, url) {
  const points = await telemetryTrends({
    deviceId: url.searchParams.get("device_id"),
    from: url.searchParams.get("from"),
    to: url.searchParams.get("to"),
    gas: url.searchParams.get("gas"),
    limit: url.searchParams.get("limit"),
  });
  return json(res, 200, { ok: true, points });
}

export {
  handleDashboardDevices,
  handleDashboardLatest,
  handleDashboardTrends,
};
