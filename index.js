const PAGE_LIMIT = 500;
const HISTORY_WINDOW_DAYS = 7;
const MERGE_WINDOW_MS = 20000;
const EXPECTED_FULL_VALUE_COUNT = 4;


function jsonResponse(data, status = 200) {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      "Content-Type": "application/json",
      "Cache-Control": "no-store"
    }
  });
}

function getTrackedDeviceIds(env) {
  if (env.TTN_DEVICE_IDS) {
    return env.TTN_DEVICE_IDS.split(",").map((id) => id.trim()).filter(Boolean);
  }

  if (env.TTN_DEVICE_ID) {
    return [env.TTN_DEVICE_ID];
  }

  return [];
}

function getStoredKey(deviceId) {
  return `latest:${deviceId || "device"}`;
}

function isSensorOnline(receivedAt) {
  if (!receivedAt || receivedAt === "--") return false;

  const lastUpdate = new Date(receivedAt);
  if (Number.isNaN(lastUpdate.getTime())) return false;

  return (new Date() - lastUpdate) / 1000 < 120;
}

const PRESET_TITLES = [
  "Temperature",
  "pH",
  "Water Level",
  "Turbidity",
  "Title5",
  "Title6",
  "Title7",
  "Title8"
];
 
// Scale factor used to decode each slot's binary value back to a
// real number (e.g. slot 4/turbidity uses x10, everything else x100).
// Must match whatever scale the sending device used to encode it.
const SLOT_SCALES = [100, 100, 100, 10, 10, 100, 100, 100];

function decodeBinaryPayload(bytes) {
  // bytes[0] = 0xFF marker (already confirmed by caller)
  // bytes[1] = starting slot number
  // bytes[2..] = pairs of (high, low) bytes, one int16 per value
  const startSlot = bytes[1];
  const result = { rawText: `[binary] slot ${startSlot}` };

  let slot = startSlot;

  for (let i = 2; i + 1 < bytes.length; i += 2) {
    if (slot > 8) break;

    let raw = (bytes[i] << 8) | bytes[i + 1];
    if (raw > 32767) raw -= 65536; // convert to signed 16-bit

    const scale = SLOT_SCALES[slot - 1] || 100;
    const value = raw / scale;

    result[`Var${slot}`] = value;
    result[`Title${slot}`] = PRESET_TITLES[slot - 1] || `Title${slot}`;

    slot++;
  }

  return result;
}

function decodeTTNPayload(base64Payload) {
  if (!base64Payload) return null;

  try {
    const decoded = atob(base64Payload); // base64 -> raw bytes as a string
    const bytes = Array.from(decoded, (c) => c.charCodeAt(0));

    // 0xFF marker byte means this is a BINARY payload, not text.
    if (bytes[0] === 0xFF && bytes.length >= 2) {
      return decodeBinaryPayload(bytes);
    }

    // Otherwise, treat as plain text (existing behavior).
    // Optional "N-" prefix tells us which slot to start filling from,
    // e.g. "3-12.40,330.0" means these values go into Var3, Var4.
    // A full message with no prefix just starts at slot 1 as before.
    const prefixMatch = decoded.match(/^(\d+)-(.+)$/);

    let startSlot = 1;
    let valuesText = decoded;

    if (prefixMatch) {
      startSlot = parseInt(prefixMatch[1], 10) || 1;
      valuesText = prefixMatch[2];
    }

    const parts = valuesText.split(",").map((p) => p.trim());

    const result = { rawText: decoded };

    parts.forEach((value, index) => {
      const slot = startSlot + index;
      if (slot > 8) return;

      const varKey = `Var${slot}`;
      const titleKey = `Title${slot}`;

      const num = parseFloat(value);

      result[varKey] = Number.isNaN(num) ? null : num;
      result[titleKey] = PRESET_TITLES[slot - 1] || titleKey;
    });

    return result;
  } catch {
    return null;
  }
}

function getWebhookRoot(body) {
  if (body?.data?.uplink_message) {
    return body.data;
  }

  return body;
}

function extractWebhookData(body, env) {
  const root = getWebhookRoot(body);
  const endDeviceIds = root?.end_device_ids || {};
  const uplinkMessage = root?.uplink_message || {};

  return {
    device_id: endDeviceIds.device_id || env.TTN_DEVICE_ID || "--",
    received_at:
      root?.received_at ||
      uplinkMessage?.received_at ||
      body?.time ||
      new Date().toISOString(),

    frm_payload: uplinkMessage?.frm_payload || null,
    f_port: uplinkMessage?.f_port ?? "--",
    decoded_payload: uplinkMessage?.decoded_payload || null
  };
}

async function readLatestForDevice(env, deviceId) {
  const raw = await env.DEVICE_DATA.get(getStoredKey(deviceId));

  if (!raw) {
    return {
      cloudConnected: true,
      sensorActive: false,
      error: "",
      message: "Connected to webhook backend, but no uplink has been received yet.",
      device_id: deviceId || "--",
      received_at: "--",
      frm_payload: null,
      f_port: "--",
      decoded_payload: null
    };
  }

  try {
    const parsed = JSON.parse(raw);

    return {
      cloudConnected: true,
      sensorActive: isSensorOnline(parsed.received_at),
      error: "",
      message: isSensorOnline(parsed.received_at)
        ? "Sensor data received."
        : "No recent sensor data received.",
      ...parsed
    };
  } catch {
    return {
      cloudConnected: true,
      sensorActive: false,
      error: "Stored device data is invalid.",
      message: "",
      device_id: deviceId || "--",
      received_at: "--",
      frm_payload: null,
      f_port: "--",
      decoded_payload: null
    };
  }
}

async function readLatestDeviceData(env) {
  const deviceIds = getTrackedDeviceIds(env);

  if (!deviceIds.length) {
    return {
      cloudConnected: true,
      sensorActive: false,
      error: "No TTN_DEVICE_IDS configured.",
      message: "",
      devices: []
    };
  }

  const devices = await Promise.all(
    deviceIds.map((deviceId) => readLatestForDevice(env, deviceId))
  );

  // Keep single-device shaped fields at the top level for backward compatibility
  // (first device's data), plus the full multi-device list under `devices`.
  const primary = devices[0];

  return {
    ...primary,
    devices
  };
}

function parseEventStream(text) {
  const lines = text
    .split("\n")
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line) =>
      line.startsWith("data:")
        ? line.replace(/^data:\s*/, "")
        : line
    );

  const rows = [];

  for (const line of lines) {
    try {
      const parsed = JSON.parse(line);
      rows.push(parsed?.result || parsed);
    } catch {}
  }

  return rows;
}

function normaliseHistoryRow(result) {
  const uplinkMessage = result?.uplink_message || null;
  const endDeviceIds = result?.end_device_ids || {};

  const receivedAt =
    result?.received_at ||
    uplinkMessage?.received_at ||
    null;

  const frmPayload = uplinkMessage?.frm_payload || null;

  let decoded = null;
  let rawText = "--";

  if (frmPayload) {
    decoded = decodeTTNPayload(frmPayload);

    if (decoded) {
      rawText = decoded.rawText || "--";
    }
  }

  if (!receivedAt) return null;

  return {
    timestamp: receivedAt,
    device_id: endDeviceIds.device_id || "--",
    f_port: uplinkMessage?.f_port ?? "--",

    Title1: decoded?.Title1 || "Title1",
    Var1: decoded?.Var1 ?? null,

    Title2: decoded?.Title2 || "Title2",
    Var2: decoded?.Var2 ?? null,

    Title3: decoded?.Title3 || "Title3",
    Var3: decoded?.Var3 ?? null,

    Title4: decoded?.Title4 || "Title4",
    Var4: decoded?.Var4 ?? null,

    Title5: decoded?.Title5 || "Title5",
    Var5: decoded?.Var5 ?? null,

    Title6: decoded?.Title6 || "Title6",
    Var6: decoded?.Var6 ?? null,

    Title7: decoded?.Title7 || "Title7",
    Var7: decoded?.Var7 ?? null,

    Title8: decoded?.Title8 || "Title8",
    Var8: decoded?.Var8 ?? null,

    rawText
  };
}

function countNonNullVars(row) {
  return [1, 2, 3, 4, 5, 6, 7, 8]
    .filter((n) => row[`Var${n}`] != null)
    .length;
}

 
function mergePartialHistoryRows(history) {
  // Group rows by device_id first, since merging only makes sense
  // within the same device's own timeline.
  const byDevice = {};
 
  for (const row of history) {
    const key = row.device_id || "unknown";
    if (!byDevice[key]) byDevice[key] = [];
    byDevice[key].push(row);
  }
 
  const mergedHistory = [];
 
  for (const deviceId of Object.keys(byDevice)) {
    const rows = byDevice[deviceId]; // already sorted by timestamp overall,
                                      // still sorted within this filtered subset
 
    let pendingPartial = null;
 
    for (const row of rows) {
      const valueCount = countNonNullVars(row);
      const isPartial = valueCount > 0 && valueCount < EXPECTED_FULL_VALUE_COUNT;
 
      if (!isPartial) {
        // Complete row (or empty) - flush any pending partial as-is first
        // (better to show incomplete data than silently drop it),
        // then push this row normally.
        if (pendingPartial) {
          mergedHistory.push(pendingPartial);
          pendingPartial = null;
        }
        mergedHistory.push(row);
        continue;
      }
 
      // This row is partial
      if (!pendingPartial) {
        pendingPartial = row;
        continue;
      }
 
      const gapMs =
        new Date(row.timestamp).getTime() -
        new Date(pendingPartial.timestamp).getTime();
 
      if (gapMs >= 0 && gapMs < MERGE_WINDOW_MS) {
        // Each row's values already carry their correct global slot
        // (thanks to the "N-" prefix decoded earlier), so merging is
        // now a simple field-by-field combine - no guessing needed.
        const combined = { ...pendingPartial };

        for (let n = 1; n <= 8; n++) {
          if (row[`Var${n}`] != null) {
            combined[`Var${n}`] = row[`Var${n}`];
            combined[`Title${n}`] = row[`Title${n}`];
          }
        }

        combined.timestamp = row.timestamp; // use the newer timestamp
        combined.rawText = `${pendingPartial.rawText} + ${row.rawText}`;
        combined.merged = true;

        mergedHistory.push(combined);
        pendingPartial = null;
      } else {
        // Too far apart to merge - flush the old pending partial as-is,
        // and this row becomes the new pending partial
        mergedHistory.push(pendingPartial);
        pendingPartial = row;
      }
    }
 
    // Flush any leftover pending partial at the end
    if (pendingPartial) {
      mergedHistory.push(pendingPartial);
    }
  }
 
  // Re-sort since we processed device-by-device, not strictly chronologically
  mergedHistory.sort((a, b) => new Date(a.timestamp) - new Date(b.timestamp));
 
  return mergedHistory;
}

async function fetchHistoryPage(env, afterIso, beforeIso) {
  const {
    TTN_REGION: region,
    TTN_APP_ID: appId,
    TTN_API_KEY: apiKey
  } = env;

  const url =
    `https://${region}.cloud.thethings.network/api/v3/as/applications/` +
    `${encodeURIComponent(appId)}/packages/storage/uplink_message` +
    `?after=${encodeURIComponent(afterIso)}` +
    `&before=${encodeURIComponent(beforeIso)}` +
    `&order=-received_at` +
    `&limit=${PAGE_LIMIT}`;

  const response = await fetch(url, {
    method: "GET",
    headers: {
      Authorization: `Bearer ${apiKey}`,
      Accept: "text/event-stream"
    }
  });

  if (!response.ok) {
    const text = await response.text();

    throw new Error(
      `TTN history request failed: ${response.status} ${response.statusText}${text ? ` - ${text}` : ""}`
    );
  }

  const body = await response.text();

  return parseEventStream(body);
}

async function fetchHistoryFromTTN(env) {
  const {
    TTN_REGION: region,
    TTN_APP_ID: appId,
    TTN_API_KEY: apiKey
  } = env;

  const deviceIds = getTrackedDeviceIds(env);

  if (!region || !appId || !deviceIds.length || !apiKey) {
    return {
      ok: false,
      status: 500,
      error: "Missing TTN environment variables for history (region/appId/deviceIds/apiKey)."
    };
  }

  const trackedSet = new Set(deviceIds);

  const now = new Date();

  const afterDate = new Date(
    now.getTime() - HISTORY_WINDOW_DAYS * 24 * 60 * 60 * 1000
  );

  let beforeCursor = now.toISOString();

  const history = [];
  const seenKeys = new Set();

  try {
    while (true) {
      const rows = await fetchHistoryPage(
        env,
        afterDate.toISOString(),
        beforeCursor
      );

      if (!rows.length) break;

      let oldestTimestampThisPage = null;
      let deviceRowsThisPage = 0;

      for (const result of rows) {
        const endDeviceIds = result?.end_device_ids || {};

        if (!trackedSet.has(endDeviceIds.device_id)) continue;

        const row = normaliseHistoryRow(result);

        if (!row) continue;

        const rowTime = new Date(row.timestamp).getTime();

        if (Number.isNaN(rowTime)) continue;

        deviceRowsThisPage += 1;

        const dedupeKey =
          `${row.device_id}|${row.timestamp}|${row.rawText}|` +
          `${row.Title1}|${row.Var1}|` +
          `${row.Title2}|${row.Var2}|` +
          `${row.Title3}|${row.Var3}|` +
          `${row.Title4}|${row.Var4}|` +
          `${row.Title5}|${row.Var5}|` +
          `${row.Title6}|${row.Var6}|` +
          `${row.Title7}|${row.Var7}|` +
          `${row.Title8}|${row.Var8}`;

        if (!seenKeys.has(dedupeKey)) {
          seenKeys.add(dedupeKey);
          history.push(row);
        }

        if (
          oldestTimestampThisPage == null ||
          rowTime < new Date(oldestTimestampThisPage).getTime()
        ) {
          oldestTimestampThisPage = row.timestamp;
        }
      }

      if (!oldestTimestampThisPage) {
        break;
      }

      const oldestMs = new Date(oldestTimestampThisPage).getTime();

      if (Number.isNaN(oldestMs)) {
        break;
      }

      if (oldestMs <= afterDate.getTime()) {
        break;
      }

      beforeCursor = new Date(oldestMs - 1).toISOString();

      if (rows.length < PAGE_LIMIT) {
        break;
      }

      if (deviceRowsThisPage === 0) {
        break;
      }
    }

    history.sort(
      (a, b) => new Date(a.timestamp) - new Date(b.timestamp)
    );
    
    const mergedHistory = mergePartialHistoryRows(history);
    
    return {
      ok: true,
      status: 200,
      history: mergedHistory
    };

  } catch (error) {
    return {
      ok: false,
      status: 500,
      error: `Failed to fetch TTN history: ${error.message}`
    };
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (url.pathname === "/api/health") {
      return jsonResponse({ status: "ok" });
    }

    if (url.pathname === "/api/test-connection") {
      const latest = await readLatestDeviceData(env);

      return jsonResponse({
        status: "ok",
        cloudConnected: true,
        sensorActive: latest.sensorActive,
        message: latest.message
      });
    }

    if (url.pathname === "/api/device-data") {
      return jsonResponse(await readLatestDeviceData(env));
    }

    if (url.pathname === "/api/history") {
      const result = await fetchHistoryFromTTN(env);

      if (!result.ok) {
        return jsonResponse(
          {
            error: result.error,
            history: []
          },
          result.status
        );
      }

      return jsonResponse({
        error: "",
        history: result.history
      });
    }

    if (
      url.pathname === "/api/ttn-webhook" &&
      request.method === "POST"
    ) {
      if (env.WEBHOOK_SECRET) {
        const suppliedSecret =
          request.headers.get("x-webhook-secret") ||
          url.searchParams.get("secret");

        if (suppliedSecret !== env.WEBHOOK_SECRET) {
          return jsonResponse(
            { error: "Unauthorized webhook request." },
            401
          );
        }
      }

      let body;

      try {
        body = await request.json();
      } catch {
        return jsonResponse({ error: "Invalid JSON body." }, 400);
      }

      if (
        !body?.uplink_message &&
        !body?.data?.uplink_message
      ) {
        return jsonResponse({
          status: "ignored",
          message:
            "Request received, but no uplink_message was present."
        });
      }

      const extracted = extractWebhookData(body, env);
      const latestKey = getStoredKey(extracted.device_id);
      const pendingKey = `pending:${extracted.device_id}`;

      const currentDecoded = decodeTTNPayload(extracted.frm_payload) || {};
      const currentValueCount = Object.keys(currentDecoded)
        .filter((k) => k.startsWith("Var") && currentDecoded[k] != null)
        .length;

      if (currentValueCount >= EXPECTED_FULL_VALUE_COUNT) {
        // Already a complete reading on its own - store directly as latest,
        // and clear any stale pending partial for this device.
        await env.DEVICE_DATA.put(latestKey, JSON.stringify(extracted));
        try { await env.DEVICE_DATA.delete(pendingKey); } catch {}

      } else if (currentValueCount > 0) {
        // Partial message - check if there is a recent pending partial to merge with
        const pendingRaw = await env.DEVICE_DATA.get(pendingKey);
        let merged = false;

        if (pendingRaw) {
          try {
            const pending = JSON.parse(pendingRaw);

            const pendingTime = new Date(pending.received_at).getTime();
            const currentTime = new Date(extracted.received_at).getTime();
            const gapMs = currentTime - pendingTime;

            if (gapMs >= 0 && gapMs < MERGE_WINDOW_MS) {
              const pendingDecoded = decodeTTNPayload(pending.frm_payload) || {};
              const mergedDecoded = { ...pendingDecoded };

              for (let n = 1; n <= 8; n++) {
                if (currentDecoded[`Var${n}`] != null) {
                  mergedDecoded[`Var${n}`] = currentDecoded[`Var${n}`];
                  mergedDecoded[`Title${n}`] = currentDecoded[`Title${n}`];
                }
              }

              const mergedText = [1, 2, 3, 4, 5, 6, 7, 8]
                .map((n) => mergedDecoded[`Var${n}`])
                .filter((v) => v != null)
                .join(",");

              const combinedReading = {
                ...extracted,
                frm_payload: btoa(mergedText),
                merged: true,
                mergedFrom: [pending.received_at, extracted.received_at]
              };

              // NOW it's complete - write to the public "latest" key,
              // and clear the pending slot.
              await env.DEVICE_DATA.put(latestKey, JSON.stringify(combinedReading));
              try { await env.DEVICE_DATA.delete(pendingKey); } catch {}
              merged = true;
            }
          } catch {}
        }

        if (!merged) {
          // No recent pending partial to merge with - this becomes the new
          // pending partial. The public "latest" value is NOT touched, so
          // the live display keeps showing the last complete reading until
          // this one's other half arrives.
          await env.DEVICE_DATA.put(pendingKey, JSON.stringify(extracted));
        }

      } else {
        // No usable values at all - store as-is, nothing to merge
        await env.DEVICE_DATA.put(latestKey, JSON.stringify(extracted));
      }

      return jsonResponse({
        status: "stored",
        message: "Uplink stored successfully."
      });
    }

    return env.ASSETS.fetch(request);
  }
};