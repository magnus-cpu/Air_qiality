import { resolve, join, dirname } from "path";
import { fileURLToPath } from "url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);
const serverRoot = resolve(__dirname, "..");

let settings = buildSettings();

function buildSettings() {
  const dataDir = resolve(process.env.DATA_DIR || join(serverRoot, "data"));

  return {
    serverRoot,
    port: Number(process.env.PORT || 3000),
    host: process.env.HOST || "0.0.0.0",
    dataDir,
    maxBodyBytes: Number(process.env.MAX_BODY_BYTES || 8 * 1024 * 1024),
    registrationSecret: process.env.DEVICE_REGISTRATION_SECRET || "",
    db: {
      host: process.env.DB_HOST || "127.0.0.1",
      port: Number(process.env.DB_PORT || 3306),
      user: process.env.DB_USER || "root",
      password: process.env.DB_PASSWORD || "",
      database: process.env.DB_NAME || "air_quality",
      waitForConnections: true,
      connectionLimit: Number(process.env.DB_POOL_SIZE || 10),
      queueLimit: 0,
    },
  };
}

async function loadEnv() {
  try {
    const { config } = await import("dotenv");
    config();
  } catch (error) {
    if (error && error.code !== "ERR_MODULE_NOT_FOUND") {
      throw error;
    }
  }
  settings = buildSettings();
  return settings;
}

function getSettings() {
  return settings;
}

export { getSettings, loadEnv };
