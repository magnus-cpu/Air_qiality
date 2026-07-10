CREATE TABLE IF NOT EXISTS devices (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  device_id VARCHAR(64) NOT NULL,
  device_name VARCHAR(128) NOT NULL DEFAULT '',
  location_name VARCHAR(128) NOT NULL DEFAULT '',
  api_token CHAR(48) NOT NULL,
  active TINYINT(1) NOT NULL DEFAULT 1,
  registered_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_seen_at TIMESTAMP NULL DEFAULT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uq_devices_device_id (device_id),
  UNIQUE KEY uq_devices_api_token (api_token)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS telemetry_archive_files (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  device_id BIGINT UNSIGNED NOT NULL,
  relative_path VARCHAR(255) NOT NULL,
  archive_date DATE NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uq_archive_file_path (device_id, relative_path),
  KEY idx_archive_files_device_date (device_id, archive_date),
  CONSTRAINT fk_archive_files_device
    FOREIGN KEY (device_id) REFERENCES devices(id)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS telemetry_samples (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  device_id BIGINT UNSIGNED NOT NULL,
  sample_uuid VARCHAR(96) NOT NULL,
  sample_id BIGINT UNSIGNED NOT NULL,
  timestamp_ms BIGINT UNSIGNED NOT NULL,
  uptime_ms BIGINT UNSIGNED NOT NULL,
  time_synced TINYINT(1) NOT NULL DEFAULT 0,
  received_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  source_ip VARCHAR(64) NOT NULL DEFAULT '',
  PRIMARY KEY (id),
  UNIQUE KEY uq_sample_uuid (sample_uuid),
  KEY idx_samples_device_time (device_id, timestamp_ms),
  KEY idx_samples_device_sample (device_id, sample_id),
  CONSTRAINT fk_samples_device
    FOREIGN KEY (device_id) REFERENCES devices(id)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS telemetry_archive_entries (
  sample_uuid VARCHAR(96) NOT NULL,
  archive_file_id BIGINT UNSIGNED NOT NULL,
  line_number BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (sample_uuid),
  KEY idx_archive_entries_file_line (archive_file_id, line_number),
  CONSTRAINT fk_archive_entries_sample
    FOREIGN KEY (sample_uuid) REFERENCES telemetry_samples(sample_uuid)
    ON DELETE CASCADE,
  CONSTRAINT fk_archive_entries_file
    FOREIGN KEY (archive_file_id) REFERENCES telemetry_archive_files(id)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS telemetry_locations (
  sample_uuid VARCHAR(96) NOT NULL,
  location_name VARCHAR(128) NOT NULL DEFAULT '',
  latitude VARCHAR(32) NOT NULL DEFAULT '',
  longitude VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (sample_uuid),
  CONSTRAINT fk_locations_sample
    FOREIGN KEY (sample_uuid) REFERENCES telemetry_samples(sample_uuid)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS telemetry_gas_readings (
  sample_uuid VARCHAR(96) NOT NULL,
  gas_type ENUM('nh3', 'red', 'ox') NOT NULL,
  raw INT NOT NULL DEFAULT 0,
  mv INT NOT NULL DEFAULT 0,
  resistance_ohms DOUBLE NOT NULL DEFAULT 0,
  ppm DOUBLE NOT NULL DEFAULT 0,
  ppm_valid TINYINT(1) NOT NULL DEFAULT 0,
  PRIMARY KEY (sample_uuid, gas_type),
  KEY idx_gas_type (gas_type),
  CONSTRAINT fk_gas_sample
    FOREIGN KEY (sample_uuid) REFERENCES telemetry_samples(sample_uuid)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS telemetry_heater_events (
  sample_uuid VARCHAR(96) NOT NULL,
  heater_on INT NOT NULL DEFAULT 0,
  warmup INT NOT NULL DEFAULT 0,
  since_change INT NOT NULL DEFAULT 0,
  PRIMARY KEY (sample_uuid),
  CONSTRAINT fk_heater_sample
    FOREIGN KEY (sample_uuid) REFERENCES telemetry_samples(sample_uuid)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS telemetry_upload_receipts (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  device_id BIGINT UNSIGNED NOT NULL,
  source_file VARCHAR(64) NOT NULL,
  start_offset BIGINT UNSIGNED NOT NULL,
  end_offset BIGINT UNSIGNED NOT NULL,
  accepted_records INT UNSIGNED NOT NULL DEFAULT 0,
  archive_file_id BIGINT UNSIGNED NULL DEFAULT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uq_upload_receipt (device_id, source_file, start_offset, end_offset),
  KEY idx_upload_receipts_archive_file (archive_file_id),
  CONSTRAINT fk_upload_receipts_device
    FOREIGN KEY (device_id) REFERENCES devices(id)
    ON DELETE CASCADE,
  CONSTRAINT fk_upload_receipts_archive_file
    FOREIGN KEY (archive_file_id) REFERENCES telemetry_archive_files(id)
    ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
