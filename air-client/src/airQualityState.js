const LEVEL_RANK = {
  unknown: 0,
  normal: 1,
  elevated: 2,
  high: 3,
};

const LEVEL_LABEL = {
  unknown: "Unknown",
  normal: "Normal",
  elevated: "Elevated",
  high: "High",
};

const CHANNEL_GUIDELINES = [
  {
    id: "red",
    label: "RED",
    pollutant: "CO-oriented reducing gases",
    basis: "EPA CO NAAQS",
    elevatedAt: 9,
    highAt: 35,
    unit: "ppm",
    guideline: "9 ppm 8-hour; 35 ppm 1-hour",
  },
  {
    id: "ox",
    label: "OX",
    pollutant: "NO2-oriented oxidizing gas",
    basis: "EPA NO2 NAAQS",
    elevatedAt: 0.053,
    highAt: 0.1,
    unit: "ppm",
    guideline: "53 ppb annual; 100 ppb 1-hour",
  },
  {
    id: "nh3",
    label: "NH3",
    pollutant: "Ammonia channel",
    basis: "NIOSH/OSHA occupational exposure limits",
    elevatedAt: 25,
    highAt: 35,
    unit: "ppm",
    guideline: "NIOSH REL 25 ppm TWA; STEL 35 ppm; IDLH 300 ppm",
  },
];

const AIR_QUALITY_SOURCES = [
  {
    label: "EPA NAAQS table",
    url: "https://www.epa.gov/criteria-air-pollutants/naaqs-table",
  },
  {
    label: "WHO exposure brief",
    url: "https://www.who.int/publications/i/item/B09461",
  },
  {
    label: "NIOSH ammonia guide",
    url: "https://www.cdc.gov/niosh/npg/npgd0028.html",
  },
  {
    label: "EPA ammonia AEGL",
    url: "https://www.epa.gov/aegl/ammonia-results-aegl-program",
  },
];

function numericPpm(record, id) {
  const value = Number(record?.[`${id}_ppm`]);
  return Number.isFinite(value) ? value : null;
}

function isValid(record, id) {
  return Number(record?.[`${id}_ppm_valid`]) === 1;
}

function assessChannel(record, guideline) {
  const value = numericPpm(record, guideline.id);
  const valid = isValid(record, guideline.id);

  if (!valid || value === null) {
    return {
      ...guideline,
      value,
      level: "unknown",
      state: LEVEL_LABEL.unknown,
      detail: "No validated sample",
    };
  }

  if (!Number.isFinite(guideline.elevatedAt)) {
    return {
      ...guideline,
      value,
      level: "unknown",
      state: LEVEL_LABEL.unknown,
      detail: guideline.guideline,
    };
  }

  const level = value >= guideline.highAt ? "high" : value >= guideline.elevatedAt ? "elevated" : "normal";
  return {
    ...guideline,
    value,
    level,
    state: LEVEL_LABEL[level],
    detail: guideline.guideline,
  };
}

function assessAirQualityState(record) {
  if (!record) {
    return {
      level: "unknown",
      label: LEVEL_LABEL.unknown,
      summary: "No current sample",
      channels: CHANNEL_GUIDELINES.map((guideline) => assessChannel(null, guideline)),
    };
  }

  const channels = CHANNEL_GUIDELINES.map((guideline) => assessChannel(record, guideline));
  const knownChannels = channels.filter((channel) => channel.level !== "unknown");
  const strongest = knownChannels.reduce(
    (current, channel) => (LEVEL_RANK[channel.level] > LEVEL_RANK[current.level] ? channel : current),
    { level: "normal" }
  );
  const level = knownChannels.length === 0 ? "unknown" : strongest.level;

  return {
    level,
    label: LEVEL_LABEL[level],
    summary:
      level === "unknown"
        ? "No validated guideline comparison"
        : `${knownChannels.length} channel${knownChannels.length === 1 ? "" : "s"} compared with guideline thresholds`,
    channels,
  };
}

export { AIR_QUALITY_SOURCES, CHANNEL_GUIDELINES, assessAirQualityState };
