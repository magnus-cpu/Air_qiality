"use strict";

import { startServer } from "./src/app.js";

startServer().catch((error) => {
  console.error("Failed to start telemetry server:", error);
  process.exit(1);
});
