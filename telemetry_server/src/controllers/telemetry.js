import { relative } from "path";

import { getSettings } from "../config.js";
import {
  clientIp,
  contentType,
  json,
  parseBearerToken,
  parseJsonRequest,
  parseUnsignedHeader,
  requestBody,
} from "../http.js";
import { findDeviceByToken, updateDeviceSeen } from "../services/devices.js";
import { parseCsvTelemetryBody, sanitizeSegment, validateRecord } from "../services/telemetryParser.js";
import {
  findUploadReceipt,
  persistTelemetryRecords,
  recordUploadReceipt,
} from "../services/telemetryStorage.js";

async function handleTelemetryUpload(req, res) {
  const token = parseBearerToken(req);
  if (!token) {
    console.warn(`[telemetry] missing bearer token ip=${clientIp(req)}`);
    return json(res, 401, { ok: false, error: "missing bearer token" });
  }

  const device = await findDeviceByToken(token);
  if (!device) {
    console.warn(`[telemetry] unknown token ip=${clientIp(req)}`);
    return json(res, 401, { ok: false, error: "unknown or inactive device" });
  }

  let records = [];
  let uploadMode = "json";
  let sourceName = "";
  let startOffset = 0;
  let endOffset = 0;

  if (contentType(req) === "text/csv") {
    uploadMode = "csv";
    sourceName = sanitizeSegment(req.headers["x-telemetry-file-name"], "");
    startOffset = parseUnsignedHeader(req.headers["x-telemetry-start-offset"], -1);
    const contentLength = parseUnsignedHeader(req.headers["content-length"], -1);
    if (!sourceName || startOffset < 0 || contentLength <= 0) {
      console.warn(`[telemetry] csv metadata missing device_id=${device.device_id}`);
      return json(res, 400, { ok: false, error: "csv uploads require file name, start offset, and content length" });
    }
    endOffset = startOffset + contentLength;

    const receipt = await findUploadReceipt(device.id, sourceName, startOffset, endOffset);
    if (receipt) {
      await updateDeviceSeen(device.id);
      console.log(`[telemetry] duplicate csv upload acknowledged device_id=${device.device_id} source_file=${sourceName} start_offset=${startOffset} end_offset=${endOffset}`);
      return json(res, 200, {
        ok: true,
        duplicate: true,
        accepted: receipt.accepted_records,
        device_id: device.device_id,
        upload_mode: uploadMode,
        stored_in: receipt.relative_path || null,
      });
    }

    try {
      records = parseCsvTelemetryBody(await requestBody(req));
    } catch (error) {
      const message = error && error.message === "payload too large"
        ? "payload too large"
        : (error && error.message) || "invalid csv body";
      console.warn(`[telemetry] csv parse failed device_id=${device.device_id} problem=${message}`);
      return json(res, message === "payload too large" ? 413 : 400, { ok: false, error: message });
    }
  } else {
    const body = await parseJsonRequest(req, res);
    if (!body) {
      return;
    }
    if (!Array.isArray(body.records)) {
      console.warn(`[telemetry] invalid payload device_id=${device.device_id}`);
      return json(res, 400, { ok: false, error: "body must contain records[]" });
    }
    records = body.records;
  }

  if (records.length === 0) {
    console.warn(`[telemetry] empty batch device_id=${device.device_id}`);
    return json(res, 400, { ok: false, error: "telemetry batch cannot be empty" });
  }

  for (let i = 0; i < records.length; i += 1) {
    const problem = validateRecord(records[i]);
    if (problem) {
      console.warn(`[telemetry] record validation failed device_id=${device.device_id} index=${i} problem=${problem}`);
      return json(res, 400, { ok: false, error: `record ${i}: ${problem}` });
    }
  }

  try {
    const archive = await persistTelemetryRecords(device, records, req);
    if (uploadMode === "csv" && sourceName) {
      await recordUploadReceipt(device.id, sourceName, startOffset, endOffset, records.length, archive.archiveFileId);
    }
    await updateDeviceSeen(device.id);

    const { serverRoot } = getSettings();
    const storedIn = relative(serverRoot, archive.filePath);
    const fileMeta = uploadMode === "csv" && sourceName
      ? ` source_file=${sourceName} start_offset=${startOffset} end_offset=${endOffset}`
      : "";
    console.log(`[telemetry] accepted device_id=${device.device_id} mode=${uploadMode} records=${records.length}${fileMeta} stored_in=${storedIn}`);
    return json(res, 200, {
      ok: true,
      accepted: records.length,
      device_id: device.device_id,
      upload_mode: uploadMode,
      stored_in: storedIn,
    });
  } catch (error) {
    console.error("Failed to persist telemetry batch:", error);
    return json(res, 500, { ok: false, error: "failed to persist batch" });
  }
}

export { handleTelemetryUpload };
