# Security Policy

## Supported Versions

| Version | Supported |
| ------- | --------- |
| dev     | Yes       |
| master  | Yes       |

## Reporting a Vulnerability

If you discover a security vulnerability in Flipper Zero Flipped Firmware, please report it responsibly.

**Do not open a public issue for security vulnerabilities.**

Instead, please report vulnerabilities by emailing the maintainers or by using [GitHub's private vulnerability reporting](https://github.com/DuckyScript/flipped/security/advisories/new).

### What to include

- Description of the vulnerability
- Steps to reproduce
- Affected versions or branches
- Potential impact
- Suggested fix (if any)

### Response timeline

- **Acknowledgment**: Within 48 hours
- **Initial assessment**: Within 1 week
- **Fix or mitigation**: Depends on severity

### Scope

This policy covers the Flipped firmware codebase, including:

- Core firmware and Furi HAL
- User applications in `applications_user/`
- Build system and CI/CD pipelines
- Bundled third-party libraries in `lib/`

### Out of scope

- The original Flipper Zero hardware
- Official Flipper Devices firmware (report to [flipperdevices](https://github.com/flipperdevices/flipperzero-firmware))
- Third-party apps not included in this repository

## Responsible Use

This firmware includes tools that interact with wireless protocols. Users are responsible for complying with all applicable laws and regulations. Security testing should only be performed on systems you own or have explicit authorization to test.
