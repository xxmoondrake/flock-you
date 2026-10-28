# Changelog

Versions follow [Semantic Versioning](https://semver.org/). Each release is a `vX.Y.Z` tag, which the firmware reports as its version.

## v0.1.0

The first release under the Limier name.

- **Boards:** the Seeed XIAO ESP32S3 and the Heltec WiFi LoRa 32 V4.3 and V4 R8.
- **Detection:** passive Wi-Fi and Bluetooth LE detection of Flock cameras and a watchlist of surveillance and law-enforcement vendors.
- **Recording:** a 4 MB log in flash that survives power loss and firmware updates.
- **Heltec:** screens and button menus, GNSS, and battery protection.
- **Dashboard:** a browser dashboard over USB, and over Wi-Fi on the Heltec, behind a login. It has:
  - detections, a map, notes, labels, and an ignore list;
  - live updates;
  - CSV, JSON, and GPX export;
  - device settings.
- **Alerts:**
  - upstream's buzzer sounds on a passive piezo on GPIO3, with each tier mutable;
  - a screen flash for specific matches on the Heltec.
