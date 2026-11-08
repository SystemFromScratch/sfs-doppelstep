---
template: home.html
title: Doppelstep
hide:
  - navigation
  - toc
---

# Quick start { .sfs-print-only }

<span class="sfs-eyebrow">Quick start</span>

## See it move in a few minutes, without hardware

All you need is Docker and Git. The first run downloads the toolchain and Zephyr, so it
takes a while; after that, the tests take about half a minute. The same firmware that runs
on the board runs here as a Linux program, with a simulated stepper motor behind it.

```bash
git clone https://github.com/SystemFromScratch/sfs-doppelstep.git doppelstep
cd doppelstep
docker compose build                                         # toolchain image
docker compose run --rm zephyr bash scripts/west-init.sh     # Zephyr and the libraries
docker compose run --rm zephyr bash scripts/run-tests.sh     # firmware + SIL and integration tests
```

Then start the simulated servo and move it from your PC, the way a robot controller would:

```bash
docker compose run --rm zephyr west build -b native_sim/native/64 -s app -d build-native
docker compose run --rm -p 5555:5555 zephyr python3 tools/sim/run_native.py run --tcp 5555
python tools/sim/dxl_smoke_test.py --tcp localhost:5555      # second terminal: ping, torque, move
```

No Docker at hand? Open the repository in **GitHub Codespaces**: the environment image is
ready, nothing is built at the start ([Development environment](development.md#codespaces)).

## Find your way

| I want to… | Read |
|---|---|
| Understand what Doppelstep is, in plain words | [Introduction](overview.md) |
| See which technologies and libraries it is built on, and why | [Technology stack](technology.md) |
| Build it, run it on a PC, flash the board | [Getting started](getting-started.md) |
| Wire the board and the motor, bring it up safely | [Hardware and bring-up](hardware.md) |
| Connect ROS 2, DynamixelSDK or DYNAMIXEL Wizard | [DYNAMIXEL emulation](dynamixel.md) |
| Change the motor, the limits, the ID or the baud rate | [Configuration](configuration.md) |
| Read the code: layers, threads, the path of a command | [Architecture](architecture.md) |
| Port it to another microcontroller | [Porting to a new MCU family](porting.md) |
| Run and extend the tests | [Tests and CI](testing.md) |
| Get the whole manual as a PDF | [Doppelstep.pdf](https://systemfromscratch.github.io/sfs-doppelstep/Doppelstep.pdf), or build it: `python tools/docs/build_manual.py` ([how](development.md#documentation-and-pdf-manual)) |
