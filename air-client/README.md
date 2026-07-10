# Air Client

React + Vite dashboard for the telemetry server.

## Run

Install dependencies, then start the dev server:

```bash
npm install
npm run dev
```

By default, Vite proxies `/api` and `/health` to:

```text
http://localhost:3000
```

To point at another telemetry server:

```bash
VITE_API_PROXY_TARGET=http://your-server:3000 npm run dev
```

For a deployed build, set:

```text
VITE_API_BASE_URL=http://your-server:3000
```

## Dashboard Data

The dashboard reads:

- `GET /health`
- `GET /api/devices`
- `GET /api/telemetry/latest?device_id=<id>`
- `GET /api/telemetry/trends?device_id=<id>&gas=all&from=<timestamp_ms>`
