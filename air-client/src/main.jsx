import React, { useEffect, useMemo, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import {
  Activity,
  AlertCircle,
  AlertTriangle,
  CheckCircle2,
  Flame,
  Gauge,
  HelpCircle,
  MapPin,
  Navigation,
  RefreshCw,
  Server,
  Timer,
} from "lucide-react";

import {
  getDevices,
  getHealth,
  getLatestTelemetry,
  getTelemetryTrends,
} from "./api.js";
import { AIR_QUALITY_SOURCES, assessAirQualityState } from "./airQualityState.js";
import "./styles.css";

const GASES = [
  { id: "nh3", label: "NH3", color: "#0f8b8d" },
  { id: "red", label: "RED", color: "#d95d39" },
  { id: "ox", label: "OX", color: "#476c9b" },
];
const STATE_ICONS = {
  normal: CheckCircle2,
  elevated: AlertCircle,
  high: AlertTriangle,
  unknown: HelpCircle,
};
const COUNTRY_MAP_ZOOM = 7;
const SENSOR_MAP_ZOOM = 16;
const TILE_SIZE = 256;
// Adjust displayed times by this many hours (use positive to add hours).
const TIME_ADJUST_HOURS = 3;

function formatTime(value) {
  if (!value) {
    return "-";
  }
  // adjust timestamp by configured hours before formatting
  const adjusted = new Date(Number(value) + TIME_ADJUST_HOURS * 60 * 60 * 1000);
  return adjusted.toLocaleString("en-US", {
    year: "2-digit",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    timeZone: "UTC",
  });
}

function ppm(value) {
  const numeric = Number(value);
  return Number.isFinite(numeric) ? numeric.toFixed(3) : "-";
}

function latestPpm(record, gas) {
  return record ? ppm(record[`${gas}_ppm`]) : "-";
}

function latestValid(record, gas) {
  return record ? Number(record[`${gas}_ppm_valid`]) === 1 : false;
}

function signedPpmDelta(current, previous, gas) {
  if (!current || !previous) {
    return null;
  }
  const currentValue = Number(current[`${gas}_ppm`]);
  const previousValue = Number(previous[`${gas}_ppm`]);
  if (!Number.isFinite(currentValue) || !Number.isFinite(previousValue)) {
    return null;
  }
  return currentValue - previousValue;
}

function formatDelta(value) {
  if (!Number.isFinite(value)) {
    return "-";
  }
  const sign = value > 0 ? "+" : "";
  return `${sign}${value.toFixed(3)}`;
}

function formatDeltaDetail(value) {
  return Number.isFinite(value) ? `${formatDelta(value)} from previous` : "no previous sample";
}

function deltaClass(value) {
  if (!Number.isFinite(value) || value === 0) {
    return "delta neutral";
  }
  return value > 0 ? "delta positive" : "delta negative";
}

function MetricCard({ icon: Icon, label, value, detail, color }) {
  return (
    <section className="metric-card">
      <div className="metric-icon" style={{ color }}>
        <Icon size={18} />
      </div>
      <div>
        <p>{label}</p>
        <strong>{value}</strong>
        <span>{detail}</span>
      </div>
    </section>
  );
}

function StateBadge({ level, children }) {
  return <span className={`state-badge ${level}`}>{children}</span>;
}

function CurrentStatePanel({ sample }) {
  const state = useMemo(() => assessAirQualityState(sample), [sample]);
  const StateIcon = STATE_ICONS[state.level] || HelpCircle;

  return (
    <section className={`panel current-state-panel ${state.level}`}>
      <div className="current-state-summary">
        <div className="state-icon">
          <StateIcon size={20} />
        </div>
        <div>
          <p>Current State</p>
          <h2>{state.label}</h2>
          <span>{state.summary}</span>
        </div>
      </div>
      <div className="state-channel-grid">
        {state.channels.map((channel) => (
          <article key={channel.id} className={`state-channel ${channel.level}`}>
            <div>
              <p>{channel.label}</p>
              <strong>{Number.isFinite(channel.value) ? `${channel.value.toFixed(3)} ${channel.unit}` : "-"}</strong>
            </div>
            <StateBadge level={channel.level}>{channel.state}</StateBadge>
            <span>{channel.pollutant}</span>
            <small>{channel.detail}</small>
          </article>
        ))}
      </div>
      <div className="state-sources">
        {AIR_QUALITY_SOURCES.map((source) => (
          <a key={source.url} href={source.url} target="_blank" rel="noreferrer">
            {source.label}
          </a>
        ))}
      </div>
    </section>
  );
}

function DeltaCell({ current, previous, gas }) {
  const delta = signedPpmDelta(current, previous, gas);
  return <td className={deltaClass(delta)}>{formatDelta(delta)}</td>;
}

function projectLatLon(lat, lon, zoom) {
  const scale = TILE_SIZE * 2 ** zoom;
  const sinLat = Math.sin((lat * Math.PI) / 180);
  return {
    x: ((lon + 180) / 360) * scale,
    y: (0.5 - Math.log((1 + sinLat) / (1 - sinLat)) / (4 * Math.PI)) * scale,
  };
}

function unprojectLatLon(x, y, zoom) {
  const scale = TILE_SIZE * 2 ** zoom;
  const lon = (x / scale) * 360 - 180;
  const n = Math.PI - (2 * Math.PI * y) / scale;
  const lat = (180 / Math.PI) * Math.atan(0.5 * (Math.exp(n) - Math.exp(-n)));
  return { lat, lon };
}

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max);
}

function OpenStreetMapPanel({ sample, selected }) {
  const mapRef = useRef(null);
  const dragRef = useRef(null);
  const [popupOpen, setPopupOpen] = useState(false);
  const [zoom, setZoom] = useState(COUNTRY_MAP_ZOOM);
  const [center, setCenter] = useState({ lat: -6.369, lon: 34.8888 });
  const [mapSize, setMapSize] = useState({ width: 900, height: 520 });
  const lat = Number(sample?.latitude);
  const lon = Number(sample?.longitude);
  const hasLocation = Number.isFinite(lat) && Number.isFinite(lon);
  const clampedZoom = clamp(zoom, 3, 18);
  const centerWorld = projectLatLon(center.lat, center.lon, clampedZoom);
  const topLeft = {
    x: centerWorld.x - mapSize.width / 2,
    y: centerWorld.y - mapSize.height / 2,
  };
  const tileLimit = 2 ** clampedZoom;
  const tiles = [];
  const minTileX = Math.floor(topLeft.x / TILE_SIZE) - 1;
  const maxTileX = Math.floor((topLeft.x + mapSize.width) / TILE_SIZE) + 1;
  const minTileY = Math.max(0, Math.floor(topLeft.y / TILE_SIZE) - 1);
  const maxTileY = Math.min(tileLimit - 1, Math.floor((topLeft.y + mapSize.height) / TILE_SIZE) + 1);
  const markerWorld = hasLocation ? projectLatLon(lat, lon, clampedZoom) : null;
  const markerPosition = markerWorld
    ? { left: markerWorld.x - topLeft.x, top: markerWorld.y - topLeft.y }
    : null;

  for (let tileY = minTileY; tileY <= maxTileY; tileY += 1) {
    for (let tileX = minTileX; tileX <= maxTileX; tileX += 1) {
      const wrappedX = ((tileX % tileLimit) + tileLimit) % tileLimit;
      tiles.push({
        key: `${tileX}:${tileY}:${clampedZoom}`,
        left: tileX * TILE_SIZE - topLeft.x,
        top: tileY * TILE_SIZE - topLeft.y,
        url: `https://tile.openstreetmap.org/${clampedZoom}/${wrappedX}/${tileY}.png`,
      });
    }
  }

  useEffect(() => {
    if (hasLocation) {
      setCenter({ lat, lon });
      setZoom(COUNTRY_MAP_ZOOM);
      setPopupOpen(false);
    }
  }, [hasLocation, lat, lon]);

  useEffect(() => {
    if (!mapRef.current) {
      return undefined;
    }
    const updateSize = () => {
      const rect = mapRef.current.getBoundingClientRect();
      setMapSize({
        width: Math.max(rect.width, 320),
        height: Math.max(rect.height, 320),
      });
    };
    updateSize();
    const observer = new ResizeObserver(updateSize);
    observer.observe(mapRef.current);
    return () => observer.disconnect();
  }, []);

  function zoomMap(nextZoom) {
    setZoom(clamp(nextZoom, 3, 18));
  }

  function handleWheel(event) {
    event.preventDefault();
    zoomMap(clampedZoom + (event.deltaY < 0 ? 1 : -1));
  }

  function handlePointerDown(event) {
    dragRef.current = {
      pointerId: event.pointerId,
      startX: event.clientX,
      startY: event.clientY,
      center,
    };
    event.currentTarget.setPointerCapture(event.pointerId);
  }

  function handlePointerMove(event) {
    const drag = dragRef.current;
    if (!drag || drag.pointerId !== event.pointerId) {
      return;
    }
    const startWorld = projectLatLon(drag.center.lat, drag.center.lon, clampedZoom);
    const nextCenter = unprojectLatLon(
      startWorld.x - (event.clientX - drag.startX),
      startWorld.y - (event.clientY - drag.startY),
      clampedZoom
    );
    setCenter({
      lat: clamp(nextCenter.lat, -85, 85),
      lon: nextCenter.lon,
    });
  }

  function handlePointerUp(event) {
    if (dragRef.current?.pointerId === event.pointerId) {
      dragRef.current = null;
    }
  }

  return (
    <section className="panel map-panel">
      <div className="panel-header">
        <div>
          <h2>Sensor Location</h2>
          <p>{sample?.location_name || selected?.location_name || "No location recorded"}</p>
        </div>
        <Navigation size={20} />
      </div>
      {hasLocation ? (
        <>
          <div
            ref={mapRef}
            className="osm-map"
            role="application"
            aria-label="OpenStreetMap sensor location"
            onWheel={handleWheel}
            onPointerDown={handlePointerDown}
            onPointerMove={handlePointerMove}
            onPointerUp={handlePointerUp}
            onPointerCancel={handlePointerUp}
          >
            {tiles.map((tile) => (
              <img
                key={tile.key}
                className="osm-tile"
                src={tile.url}
                alt=""
                draggable="false"
                style={{ left: tile.left, top: tile.top }}
              />
            ))}
            <button
              className={`map-marker ${popupOpen ? "focused" : ""}`}
              style={{ left: markerPosition?.left, top: markerPosition?.top }}
              type="button"
              onPointerDown={(event) => event.stopPropagation()}
              onClick={() => {
                setCenter({ lat, lon });
                setZoom(SENSOR_MAP_ZOOM);
                setPopupOpen(true);
              }}
              aria-label="Zoom to sensor and show latest values"
            >
              <MapPin size={26} />
            </button>
            {popupOpen ? (
              <div className="map-popup" onPointerDown={(event) => event.stopPropagation()}>
                <div className="popup-title">{sample?.location_name || selected?.location_name || "Sensor"}</div>
                <div className="popup-time">{formatTime(sample?.timestamp_ms)}</div>
                <div className="popup-values">
                  {GASES.map((item) => (
                    <span key={item.id}>
                      {item.label} <strong>{ppm(sample?.[`${item.id}_ppm`])}</strong>
                    </span>
                  ))}
                </div>
                <button type="button" onClick={() => setPopupOpen(false)}>Close</button>
              </div>
            ) : null}
            <div className="zoom-controls">
              <button type="button" onPointerDown={(event) => event.stopPropagation()} onClick={() => zoomMap(clampedZoom + 1)}>+</button>
              <button type="button" onPointerDown={(event) => event.stopPropagation()} onClick={() => zoomMap(clampedZoom - 1)}>-</button>
            </div>
          </div>
          <div className="map-meta">
            <span>Zoom {clampedZoom}</span>
            <span>Latitude {sample.latitude}</span>
            <span>Longitude {sample.longitude}</span>
            <a href={`https://www.openstreetmap.org/?mlat=${lat}&mlon=${lon}#map=${clampedZoom}/${lat}/${lon}`} target="_blank" rel="noreferrer">
              OpenStreetMap
            </a>
          </div>
        </>
      ) : (
        <div className="chart-empty">No latitude and longitude for this device yet</div>
      )}
    </section>
  );
}

function TrendChart({ points, gas }) {
  const [tooltip, setTooltip] = useState(null);
  const chartRef = useRef(null);
  const series = useMemo(() => {
    const allowed = gas === "all" ? GASES.map((item) => item.id) : [gas];
    const grouped = new Map(allowed.map((item) => [item, []]));
    for (const point of points) {
      if (!grouped.has(point.gas_type) || Number(point.ppm_valid) !== 1) {
        continue;
      }
      grouped.get(point.gas_type).push({
        x: Number(point.timestamp_ms),
        y: Number(point.ppm),
      });
    }
    return grouped;
  }, [points, gas]);

  const allPoints = [...series.values()].flat();
  if (allPoints.length === 0) {
    return <div className="chart-empty">No valid ppm points for this selection</div>;
  }
  const timestamps = [...new Set(allPoints.map((point) => point.x))].sort((a, b) => a - b);

  const minX = Math.min(...allPoints.map((point) => point.x));
  const maxX = Math.max(...allPoints.map((point) => point.x));
  const minY = Math.min(...allPoints.map((point) => point.y));
  const maxY = Math.max(...allPoints.map((point) => point.y));
  const yPadding = Math.max((maxY - minY) * 0.12, 0.5);
  const xSpan = Math.max(maxX - minX, 1);
  const yMin = minY - yPadding;
  const ySpan = Math.max(maxY + yPadding - yMin, 1);
  const width = 900;
  const height = 330;
  const pad = { top: 24, right: 28, bottom: 42, left: 58 };
  const plotW = width - pad.left - pad.right;
  const plotH = height - pad.top - pad.bottom;

  const toX = (value) => pad.left + ((value - minX) / xSpan) * plotW;
  const toY = (value) => pad.top + plotH - ((value - yMin) / ySpan) * plotH;
  const showTooltip = (time) => {
    const rows = GASES.map((item) => {
      const point = (series.get(item.id) || []).find((value) => value.x === time);
      if (!point) {
        return null;
      }
      return {
        gas: item.label,
        color: item.color,
        value: point.y,
        y: toY(point.y),
      };
    }).filter(Boolean);

    if (rows.length === 0) {
      setTooltip(null);
      return;
    }

    setTooltip({
      time,
      x: toX(time),
      rows,
      y: Math.min(...rows.map((row) => row.y)),
    });
  };

  const showNearestTooltip = (clientX) => {
    const bounds = chartRef.current?.getBoundingClientRect();
    if (!bounds) {
      return;
    }

    const viewX = ((clientX - bounds.left) / bounds.width) * width;
    const clampedX = clamp(viewX, pad.left, width - pad.right);
    const time = minX + ((clampedX - pad.left) / plotW) * xSpan;
    const nearest = timestamps.reduce((closest, value) =>
      Math.abs(value - time) < Math.abs(closest - time) ? value : closest
    );
    showTooltip(nearest);
  };

  return (
    <svg
      ref={chartRef}
      className="trend-chart"
      viewBox={`0 0 ${width} ${height}`}
      role="img"
      onMouseMove={(event) => showNearestTooltip(event.clientX)}
      onMouseLeave={() => setTooltip(null)}
      onFocus={() => showTooltip(timestamps[0])}
      onBlur={() => setTooltip(null)}
      tabIndex="0"
    >
      {[0, 1, 2, 3].map((step) => {
        const y = pad.top + (plotH / 3) * step;
        const labelValue = yMin + ySpan * ((3 - step) / 3);
        return (
          <g key={step}>
            <line x1={pad.left} x2={width - pad.right} y1={y} y2={y} className="grid-line" />
            <text x={18} y={y + 4} className="axis-label">{labelValue.toFixed(1)}</text>
          </g>
        );
      })}
      {GASES.map((item) => {
        const values = series.get(item.id) || [];
        if (values.length === 0) {
          return null;
        }
        const d = values.map((point, index) => `${index === 0 ? "M" : "L"} ${toX(point.x)} ${toY(point.y)}`).join(" ");
        return <path key={item.id} d={d} fill="none" stroke={item.color} strokeWidth="3" strokeLinecap="round" />;
      })}
      {GASES.map((item) => {
        const values = series.get(item.id) || [];
        return values.map((point) => {
          const cx = toX(point.x);
          const cy = toY(point.y);
          return (
            <circle
              key={`${item.id}:${point.x}:${point.y}`}
              className="chart-point"
              cx={cx}
              cy={cy}
              r="8"
              fill={item.color}
              onMouseEnter={() => showTooltip(point.x)}
              onFocus={() => showTooltip(point.x)}
              tabIndex="0"
              aria-label={`${item.label} ${point.y.toFixed(3)} ppm at ${formatTime(point.x)}`}
            />
          );
        });
      })}
      {tooltip ? (
        <>
          <line className="tooltip-guide" x1={tooltip.x} x2={tooltip.x} y1={pad.top} y2={height - pad.bottom} />
          {tooltip.rows.map((row) => (
            <g key={row.gas}>
              <circle className="tooltip-active-ring" cx={tooltip.x} cy={row.y} r="8" stroke={row.color} />
              <circle cx={tooltip.x} cy={row.y} r="3.5" fill={row.color} />
            </g>
          ))}
          <g className="chart-tooltip" transform={`translate(${clamp(tooltip.x + 14, 8, width - 208)} ${clamp(tooltip.y - 58, 8, height - 138)})`}>
            <rect width="200" height={62 + tooltip.rows.length * 22} rx="8" />
            <text x="14" y="22" className="tooltip-title">{formatTime(tooltip.time)}</text>
            <line x1="14" x2="186" y1="36" y2="36" className="tooltip-divider" />
            {tooltip.rows.map((row, index) => {
              const rowY = 58 + index * 22;
              return (
                <g key={row.gas}>
                  <circle cx="18" cy={rowY - 4} r="4" fill={row.color} />
                  <text x="30" y={rowY} className="tooltip-label">{row.gas}</text>
                  <text x="186" y={rowY} className="tooltip-value" textAnchor="end">{row.value.toFixed(3)} ppm</text>
                </g>
              );
            })}
          </g>
        </>
      ) : null}
      <text x={pad.left} y={height - 10} className="axis-label">{formatTime(minX)}</text>
      <text x={width - pad.right - 145} y={height - 10} className="axis-label">{formatTime(maxX)}</text>
    </svg>
  );
}

function App() {
  const [devices, setDevices] = useState([]);
  const [selectedDevice, setSelectedDevice] = useState("");
  const [latest, setLatest] = useState([]);
  const [trends, setTrends] = useState([]);
  const [health, setHealth] = useState(null);
  const [gas, setGas] = useState("all");
  const [range, setRange] = useState("24h");
  const [activePage, setActivePage] = useState("dashboard");
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");

  const selected = devices.find((device) => device.device_id === selectedDevice) || devices[0];
  const selectedId = selected?.device_id || "";
  const newest = latest[0];
  const previous = latest[1];
  const recentRows = latest.slice(0, 5);

  async function load() {
    setLoading(true);
    setError("");
    try {
      const [healthData, deviceRows] = await Promise.all([getHealth(), getDevices()]);
      const deviceId = selectedDevice || deviceRows[0]?.device_id || "";
      const hours = range === "1h" ? 1 : range === "7d" ? 24 * 7 : 24;
      const from = Date.now() - hours * 60 * 60 * 1000;
      const [latestRows, trendRows] = await Promise.all([
        getLatestTelemetry({ deviceId }),
        getTelemetryTrends({ deviceId, gas, from }),
      ]);
      setHealth(healthData);
      setDevices(deviceRows);
      setSelectedDevice(deviceId);
      setLatest(latestRows);
      setTrends(trendRows);
    } catch (loadError) {
      setError(loadError.message || "Failed to load dashboard data");
    } finally {
      setLoading(false);
    }
  }

  useEffect(() => {
    load();
    const timer = setInterval(load, 30000);
    return () => clearInterval(timer);
  }, [selectedDevice, gas, range]);

  return (
    <main className="shell">
      <header className="app-navbar">
        <div className="brand-block">
          <Gauge size={22} />
          <span>Air Quality Dashboard</span>
        </div>
        <nav className="nav-tabs" aria-label="Dashboard pages">
          <button className={activePage === "dashboard" ? "active" : ""} type="button" onClick={() => setActivePage("dashboard")}>
            Dashboard
          </button>
          <button className={activePage === "map" ? "active" : ""} type="button" onClick={() => setActivePage("map")}>
            Map
          </button>
        </nav>
        <div className="toolbar">
          <select value={selectedId} onChange={(event) => setSelectedDevice(event.target.value)}>
            {devices.length === 0 ? <option>No devices</option> : null}
            {devices.map((device) => (
              <option key={device.device_id} value={device.device_id}>
                {device.device_name || device.device_id}
              </option>
            ))}
          </select>
          <button type="button" onClick={load}>
            <RefreshCw size={16} />
            Refresh
          </button>
        </div>
      </header>

      {error ? <div className="error">{error}</div> : null}

      <section className="status-row">
        <div className="status-item">
          <Server size={16} />
          <span>{health?.ok ? "Server online" : "Server unknown"}</span>
        </div>
        <div className="status-item">
          <Timer size={16} />
          <span>Last sample {newest ? formatTime(newest.timestamp_ms) : "-"}</span>
        </div>
        <div className="status-item">
          <MapPin size={16} />
          <span>{newest?.location_name || selected?.location_name || "No location"}</span>
        </div>
      </section>

      {activePage === "dashboard" ? (
        <>
          <section className="metrics-grid">
            {GASES.map((item) => {
              const delta = signedPpmDelta(newest, previous, item.id);
              return (
                <MetricCard
                  key={item.id}
                  icon={Gauge}
                  label={`${item.label} ppm`}
                  value={latestPpm(newest, item.id)}
                  detail={latestValid(newest, item.id) ? formatDeltaDetail(delta) : "not validated"}
                  color={item.color}
                />
              );
            })}
            <MetricCard
              icon={Flame}
              label="Heater"
              value={newest?.heater_on ? "On" : "Off"}
              detail={newest?.warmup ? "warming up" : "stable"}
              color="#8a5a44"
            />
          </section>

          <CurrentStatePanel sample={newest} />

          <section className="panel chart-panel">
            <div className="panel-header">
              <div>
                <h2>PPM Trend</h2>
                <p>{selectedId || "No device selected"}</p>
              </div>
              <div className="segmented">
                <button className={gas === "all" ? "active" : ""} onClick={() => setGas("all")}>All</button>
                {GASES.map((item) => (
                  <button key={item.id} className={gas === item.id ? "active" : ""} onClick={() => setGas(item.id)}>
                    {item.label}
                  </button>
                ))}
              </div>
              <select value={range} onChange={(event) => setRange(event.target.value)}>
                <option value="1h">1 hour</option>
                <option value="24h">24 hours</option>
                <option value="7d">7 days</option>
              </select>
            </div>
            {loading ? <div className="chart-empty">Loading telemetry...</div> : <TrendChart points={trends} gas={gas} />}
          </section>

          <section className="panel">
            <div className="panel-header">
              <div>
                <h2>Recent Readings</h2>
                <p>Latest five accepted samples from the selected device</p>
              </div>
              <Activity size={20} />
            </div>
            <div className="table-wrap">
              <table>
                <thead>
                  <tr>
                    <th>Timestamp</th>
                    <th>NH3</th>
                    <th>Delta NH3</th>
                    <th>RED</th>
                    <th>Delta RED</th>
                    <th>OX</th>
                    <th>Delta OX</th>
                  </tr>
                </thead>
                <tbody>
                  {recentRows.map((record, index) => {
                    const older = latest[index + 1];
                    return (
                      <tr key={record.sample_uuid}>
                        <td>{formatTime(record.timestamp_ms)}</td>
                        <td>{ppm(record.nh3_ppm)}</td>
                        <DeltaCell current={record} previous={older} gas="nh3" />
                        <td>{ppm(record.red_ppm)}</td>
                        <DeltaCell current={record} previous={older} gas="red" />
                        <td>{ppm(record.ox_ppm)}</td>
                        <DeltaCell current={record} previous={older} gas="ox" />
                      </tr>
                    );
                  })}
                  {recentRows.length === 0 ? (
                    <tr>
                      <td colSpan="7" className="empty-row">No readings yet</td>
                    </tr>
                  ) : null}
                </tbody>
              </table>
            </div>
          </section>
        </>
      ) : (
        <OpenStreetMapPanel sample={newest} selected={selected} />
      )}
    </main>
  );
}

createRoot(document.getElementById("root")).render(<App />);
