import { json } from "./http.js";
import {
  handleDashboardDevices,
  handleDashboardLatest,
  handleDashboardTrends,
} from "./controllers/dashboard.js";
import { handleDeviceAuth, handleDeviceRegistration } from "./controllers/devices.js";
import { handleHealth } from "./controllers/health.js";
import { handleTelemetryUpload } from "./controllers/telemetry.js";

function createRequestHandler() {
  return async (req, res) => {
    try {
      const url = new URL(req.url || "/", "http://localhost");

      if (req.method === "GET" && url.pathname === "/health") {
        return handleHealth(req, res);
      }

      if (req.method === "GET" && url.pathname === "/api/devices") {
        return handleDashboardDevices(req, res);
      }

      if (req.method === "GET" && url.pathname === "/api/telemetry/latest") {
        return handleDashboardLatest(req, res, url);
      }

      if (req.method === "GET" && url.pathname === "/api/telemetry/trends") {
        return handleDashboardTrends(req, res, url);
      }

      if (req.method === "POST" && url.pathname === "/api/devices/register") {
        return handleDeviceRegistration(req, res);
      }

      if (req.method === "POST" && url.pathname === "/api/devices/auth") {
        return handleDeviceAuth(req, res);
      }

      if (req.method === "POST" && url.pathname === "/api/telemetry") {
        return handleTelemetryUpload(req, res);
      }

      return json(res, 404, { ok: false, error: "not found" });
    } catch (error) {
      console.error("Unhandled request error:", error);
      return json(res, 500, { ok: false, error: "internal server error" });
    }
  };
}

export { createRequestHandler };
