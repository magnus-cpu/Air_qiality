# Storage Notes

This project stores telemetry on the SD card using FATFS through ESP-IDF.

## FAT filename limitation

The current build uses:

- `CONFIG_FATFS_LFN_NONE=y`

That means long filenames are disabled in FATFS for this firmware build.

Because of that, files created on the SD card by the firmware must use FAT 8.3-safe names.

Examples used by the telemetry pipeline:

- `telact.csv`
- `telupl.csv`
- `telupl.sta`

Names like `telemetry_active.csv` are too long for the current FATFS configuration and can fail with:

- `errno=22 (Invalid argument)`

## Why this matters

If a new SD-backed feature is added later, keep filenames short unless FAT long filename support is enabled in `sdkconfig`.

Relevant config:

- [sdkconfig](/home/magnus/esp_projects/Air_qiality/sdkconfig:1246)

Relevant code:

- [main/telemetry_pipeline.c](/home/magnus/esp_projects/Air_qiality/main/telemetry_pipeline.c:23)
- [main/sd_card.c](/home/magnus/esp_projects/Air_qiality/main/sd_card.c:1)
