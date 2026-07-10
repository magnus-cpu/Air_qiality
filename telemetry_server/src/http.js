import { mkdirSync } from "fs";

import { getSettings } from "./config.js";

function ensureDir(dirPath) {
  mkdirSync(dirPath, { recursive: true });
}

function json(res, statusCode, payload) {
  const body = JSON.stringify(payload);
  res.writeHead(statusCode, {
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": Buffer.byteLength(body),
  });
  res.end(body);
}

function contentType(req) {
  return String(req.headers["content-type"] || "")
    .split(";")[0]
    .trim()
    .toLowerCase();
}

function requestBody(req) {
  const { maxBodyBytes } = getSettings();

  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks = [];

    req.on("data", (chunk) => {
      size += chunk.length;
      if (size > maxBodyBytes) {
        reject(new Error("payload too large"));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });

    req.on("end", () => resolve(Buffer.concat(chunks).toString("utf8")));
    req.on("error", reject);
  });
}

function clientIp(req) {
  const forwarded = req.headers["x-forwarded-for"];
  if (typeof forwarded === "string" && forwarded.length > 0) {
    return forwarded.split(",")[0].trim();
  }
  return req.socket.remoteAddress || "";
}

function parseBearerToken(req) {
  const auth = req.headers.authorization || "";
  const match = /^Bearer\s+(.+)$/i.exec(auth);
  return match ? match[1].trim() : "";
}

async function parseJsonRequest(req, res) {
  try {
    const rawBody = await requestBody(req);
    return JSON.parse(rawBody);
  } catch (error) {
    const status = error && error.message === "payload too large" ? 413 : 400;
    json(res, status, { ok: false, error: "invalid json body" });
    return null;
  }
}

function parseUnsignedHeader(value, fallback = null) {
  const parsed = Number.parseInt(String(value ?? ""), 10);
  if (!Number.isFinite(parsed) || parsed < 0) {
    return fallback;
  }
  return parsed;
}

export {
  clientIp,
  contentType,
  ensureDir,
  json,
  parseBearerToken,
  parseJsonRequest,
  parseUnsignedHeader,
  requestBody,
};
