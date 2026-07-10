import { getDbPool } from "../db.js";
import { json } from "../http.js";

async function handleHealth(req, res) {
  await getDbPool().query("SELECT 1");
  return json(res, 200, { ok: true, service: "telemetry-server", db: "connected" });
}

export { handleHealth };
