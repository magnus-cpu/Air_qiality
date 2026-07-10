import assert from "node:assert/strict";

import { assessAirQualityState } from "./airQualityState.js";

const normal = assessAirQualityState({
  red_ppm: 1,
  red_ppm_valid: 1,
  ox_ppm: 0.02,
  ox_ppm_valid: 1,
  nh3_ppm: 0.2,
  nh3_ppm_valid: 1,
});

assert.equal(normal.level, "normal");
assert.equal(normal.label, "Normal");

const elevated = assessAirQualityState({
  red_ppm: 10,
  red_ppm_valid: 1,
  ox_ppm: 0.02,
  ox_ppm_valid: 1,
  nh3_ppm: 0.2,
  nh3_ppm_valid: 1,
});

assert.equal(elevated.level, "elevated");
assert.equal(elevated.channels.find((item) => item.id === "red").state, "Elevated");

const high = assessAirQualityState({
  red_ppm: 1,
  red_ppm_valid: 1,
  ox_ppm: 0.101,
  ox_ppm_valid: 1,
  nh3_ppm: 0.2,
  nh3_ppm_valid: 1,
});

assert.equal(high.level, "high");
assert.equal(high.channels.find((item) => item.id === "ox").state, "High");

const no2Elevated = assessAirQualityState({
  red_ppm: 1,
  red_ppm_valid: 1,
  ox_ppm: 0.081,
  ox_ppm_valid: 1,
  nh3_ppm: 0.2,
  nh3_ppm_valid: 1,
});

const oxChannel = no2Elevated.channels.find((item) => item.id === "ox");
assert.equal(no2Elevated.level, "elevated");
assert.equal(oxChannel.pollutant, "NO2-oriented oxidizing gas");
assert.equal(oxChannel.detail, "53 ppb annual; 100 ppb 1-hour");

const nh3Elevated = assessAirQualityState({
  red_ppm: 1,
  red_ppm_valid: 1,
  ox_ppm: 0.02,
  ox_ppm_valid: 1,
  nh3_ppm: 25,
  nh3_ppm_valid: 1,
});

const nh3ElevatedChannel = nh3Elevated.channels.find((item) => item.id === "nh3");
assert.equal(nh3Elevated.level, "elevated");
assert.equal(nh3ElevatedChannel.state, "Elevated");
assert.equal(nh3ElevatedChannel.detail, "NIOSH REL 25 ppm TWA; STEL 35 ppm; IDLH 300 ppm");

const nh3High = assessAirQualityState({
  red_ppm: 1,
  red_ppm_valid: 1,
  ox_ppm: 0.02,
  ox_ppm_valid: 1,
  nh3_ppm: 35,
  nh3_ppm_valid: 1,
});

assert.equal(nh3High.level, "high");
assert.equal(nh3High.channels.find((item) => item.id === "nh3").state, "High");

const unknown = assessAirQualityState(null);

assert.equal(unknown.level, "unknown");
assert.equal(unknown.label, "Unknown");
