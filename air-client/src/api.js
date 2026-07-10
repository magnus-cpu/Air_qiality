const API_BASE = import.meta.env.VITE_API_BASE_URL || "";

async function fetchJson(path) {
  const response = await fetch(`${API_BASE}${path}`);
  if (!response.ok) {
    throw new Error(`Request failed: ${response.status}`);
  }
  return response.json();
}

function query(params) {
  const search = new URLSearchParams();
  for (const [key, value] of Object.entries(params)) {
    if (value !== undefined && value !== null && value !== "") {
      search.set(key, value);
    }
  }
  const text = search.toString();
  return text ? `?${text}` : "";
}

async function getHealth() {
  return fetchJson("/health");
}

async function getDevices() {
  const data = await fetchJson("/api/devices");
  return data.devices || [];
}

async function getLatestTelemetry({ deviceId, limit = 40 }) {
  const data = await fetchJson(`/api/telemetry/latest${query({ device_id: deviceId, limit })}`);
  return data.records || [];
}

async function getTelemetryTrends({ deviceId, gas = "all", from, to, limit = 1200 }) {
  const data = await fetchJson(`/api/telemetry/trends${query({
    device_id: deviceId,
    gas,
    from,
    to,
    limit,
  })}`);
  return data.points || [];
}

export {
  getDevices,
  getHealth,
  getLatestTelemetry,
  getTelemetryTrends,
};
