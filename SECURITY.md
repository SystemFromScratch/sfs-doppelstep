# Security policy

Doppelstep is firmware for a device that switches motor power and accepts firmware updates over
its bus, so security reports are welcome and handled with priority.

## Reporting a vulnerability

Please do not open a public issue for a vulnerability. Report it privately, either:

- through GitHub: **Security → Report a vulnerability** on this repository, or
- by e-mail to [contact@systemfromscratch.com](mailto:contact@systemfromscratch.com).

Include the commit, the build (board, simulated motor or `native_sim`), the steps or the packets
that show the problem, and its effect. You will get an answer within five working days with an
assessment and the next steps. Fixes are published with a security advisory that credits the
reporter, unless you prefer to stay anonymous.

## Supported versions

| Version | Supported |
|---|---|
| `main` | yes |
| latest release (0.1.x) | yes |
| older releases | no: update to the latest release |

Security fixes go to `main` first and into the next release, which `CHANGELOG.md` lists.

## What is in scope

| In scope | Examples |
|---|---|
| The firmware of this repository | Packet handling of the bus, the Control Table mapping, the drivers, the protection paths |
| The update path | MCUboot configuration, serial recovery, image verification in the GUI |
| The build and CI configuration | The Docker image, the workflows, the published environment image |

Vulnerabilities in Zephyr RTOS, MCUboot, Stepper-C, the X430 emulator, DXL2-C or other
dependencies belong to those projects; if you are unsure, report here and the report will be
forwarded.

## Known by design

- **Development signing key.** The build signs the application with the public development key
  of MCUboot, so any image signed with that key boots. This is intended for development only. A
  product must use its own key: [Firmware update, Signature](docs/firmware-update.md#signature).
- **No authentication on the bus.** DYNAMIXEL Protocol 2.0 has no authentication, like the servo
  it emulates. Anyone with access to the bus can command the motor and start a firmware update;
  the signature check decides which images may run.
