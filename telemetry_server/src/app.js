import { createServer } from "http";

import { getSettings, loadEnv } from "./config.js";
import { initDatabase } from "./db.js";
import { ensureDir } from "./http.js";
import { createRequestHandler } from "./routes.js";

async function startServer() {
  await loadEnv();
  const settings = getSettings();

  ensureDir(settings.dataDir);
  await initDatabase();

  const server = createServer(createRequestHandler());
  server.listen(settings.port, settings.host, () => {
    console.log(`Telemetry server listening on http://${settings.host}:${settings.port}`);
    console.log(`MySQL database: ${settings.db.host}:${settings.db.port}/${settings.db.database}`);
    console.log(`Storing JSONL uploads in ${settings.dataDir}`);
    console.log(settings.registrationSecret ? "Device registration secret enabled" : "Warning: DEVICE_REGISTRATION_SECRET is empty");
  });

  return server;
}

export { startServer };
