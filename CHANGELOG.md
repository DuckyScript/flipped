# Changelog

All notable changes to Flipper Zero Flipped Firmware will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- GitHub Actions workflows for build, lint, and release
- Auto-sync workflow to merge `claude/**` branches into `dev`
- Terminal app with file commands and WiFi support
- App Store app with ESP32 WiFi bridge for downloading apps
- Multi-source package manager with `sources.list` support
- CHIP-8 emulator
- WiFi Bruteforce app
- WiFi Deauth app
- HOIC network scanning app
- Stopwatch app
- Morse Code translator
- Flashlight app
- Tally Counter app
- Dice Roller app
- Custom BLE Beacon app
- SubGHz brute force attack scene for static-code protocols

### Fixed

- Pre-existing build bugs and GCC 13.2 compatibility
- Expanded bin_raw buffers and sensitivity for better signal capture
- Expanded external CC1101 module frequency ranges to match internal radio
