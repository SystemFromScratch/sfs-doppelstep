# Author & Company

<div class="sfs-brand" markdown>

![SystemFromScratch® logo](assets/images/brand/systemfromscratch-logo-on-dark.png){ .sfs-brand__logo }

</div>

Doppelstep is designed, written and maintained by **Anton Vynohradov** and published by
**SystemFromScratch®**: secure embedded systems and compliance-ready engineering, from
concept to certification-ready product.

## Author

<div class="sfs-author" markdown>

**Anton Vynohradov** · [ORCID 0009-0005-6965-0911](https://orcid.org/0009-0005-6965-0911)
{ .sfs-author__name }

Embedded Systems Engineer · Secure Product Architect · Founder of SystemFromScratch®
{ .sfs-author__role }

*Embedded systems built from scratch — from bare metal to Linux, from first schematic to
certification.*

</div>

Anton founded SystemFromScratch® to help industrial IoT, automotive, robotics, medical and
consumer electronics teams turn hardware into secure, maintainable products. He designs and
writes the software: firmware, drivers, RTOS and embedded Linux, security and compliance. For
hardware design he works with experienced partner engineers, so a customer has one point of
contact from the first board on the bench to the product in the field. He is based in Poland
and works remotely worldwide.

For Doppelstep, Anton wrote the firmware, its drivers and board support, the Stepper-C,
DXL2-C and X430 emulator libraries it is built on, the simulation models, the tests, the
development environment, the bench GUI and this documentation.

## SystemFromScratch®

SystemFromScratch® builds every layer of a connected product: secure embedded systems and
compliance-ready engineering, from concept to certification-ready product. Anton leads the
engineering; specialist partner engineers join for hardware design.

| Service | What it covers |
| --- | --- |
| Hardware design with partner engineers | Schematics and boards through trusted partners, with the firmware side from the first day |
| Hardware bring-up & integration | First boot, validation and production of new hardware, including motor-control platforms |
| BSP & device drivers | Board support packages, bootloaders and low-level drivers for MCUs, SoCs, FPGAs and custom hardware, including Zephyr drivers and Linux kernel work |
| RTOS & Embedded Linux | Porting, configuration, integration and reproducible builds for Zephyr, FreeRTOS and Embedded Linux (Yocto, Buildroot) |
| Embedded software development | Bare-metal, RTOS and Linux application firmware in C/C++, including OTA, built for reliability and long-term maintainability |
| System integration | Firmware, host software, protocols and tests brought together into one working product |
| Cybersecurity, CRA & compliance | Secure boot, signed updates, threat modelling, MISRA C/C++ and static analysis, and a roadmap to the EU Cyber Resilience Act |

Doppelstep is one of the open-source projects of the SystemFromScratch® portfolio: public
engineering work that can be inspected line by line. It shows several of these services in
one real product: Zephyr drivers and an out-of-tree board, a signed bootloader with field
update, motor control in a hard real-time interrupt, an industrial protocol, tests against a
physical model, and a reproducible environment.

## Use it, build on it

Doppelstep is licensed under the Apache License 2.0 ([License](license.md)). You may use it,
change it and build your own products on it, commercially too, under the terms of that
license. It is meant to be repeated: every step from an empty PC to a moving motor is
documented, and every number in this documentation can be re-measured from the sources.

## Contact

| Channel | Address |
| --- | --- |
| Website | [systemfromscratch.com](https://systemfromscratch.com/) |
| E-mail | [contact@systemfromscratch.com](mailto:contact@systemfromscratch.com) |
| GitHub | [github.com/AntonVynohradov](https://github.com/AntonVynohradov) |
| GitHub organization | [github.com/SystemFromScratch](https://github.com/SystemFromScratch) |
| LinkedIn | [linkedin.com/in/vynohradov](https://www.linkedin.com/in/vynohradov) |
| ORCID | [0009-0005-6965-0911](https://orcid.org/0009-0005-6965-0911) |
| Location | Poland, working remotely worldwide |

Building a connected product, bringing up new hardware, or preparing for the Cyber
Resilience Act? Get in touch through
[systemfromscratch.com](https://systemfromscratch.com/) or by e-mail.

## Support and feedback

- **Bugs and feature requests:** open an issue on
  [GitHub](https://github.com/SystemFromScratch/sfs-doppelstep/issues). Please include the
  commit or version, the build command and board, the log (RTT or `native_sim` output), and
  the steps or the test that show the problem.
- **Contributions:** follow the [architecture rules](architecture.md#four-rules); the layer
  check and the tests must pass ([Tests and CI](testing.md)).
- **Commercial support, integration and custom development:** contact SystemFromScratch®
  through [systemfromscratch.com](https://systemfromscratch.com/).

## Trademarks and acknowledgements

SystemFromScratch® is a registered trademark. The SystemFromScratch® name and logo are used
in this documentation to identify the publisher of Doppelstep; the Apache License 2.0 grants
no rights to use them, and they may not be used to endorse or promote products derived from
Doppelstep without prior written permission.

DYNAMIXEL and ROBOTIS are trademarks of ROBOTIS Co., Ltd. STM32 is a registered trademark of
STMicroelectronics International N.V. Arm and Cortex are trademarks of Arm Limited. Hardware
and protocol names are used only to describe compatibility; Doppelstep is not affiliated
with or endorsed by their owners.

Doppelstep builds on the work of the Zephyr Project, MCUboot, STMicroelectronics (STM32Cube),
SEGGER (RTT) and ROBOTIS (DynamixelSDK, the published Control Table of the XM430-W350).
