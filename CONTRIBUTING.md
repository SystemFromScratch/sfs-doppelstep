# Contributing to Doppelstep

Thank you for your interest. Bug reports, measurements from real hardware, ports to other boards
and fixes are all welcome.

## Before you start

- Read the [Architecture](docs/architecture.md): its four rules are checked in CI by
  `scripts/check-layers.sh`.
- For a larger change (a new board, a new MCU family, a new Operating Mode), open an issue first
  so that the design can be agreed before the work.
- Measurements on a B-G431B-ESC1 are especially valuable: the
  [hardware checklist](docs/hardware-checklist.md) lists what the PC tests cannot see.

## Workflow

1. Fork the repository and create a branch from `main`.
2. Make the change. The quickest environment is GitHub Codespaces or the Docker image
   ([Getting started](docs/getting-started.md)).
3. Run everything CI runs:

   ```bash
   docker compose run --rm zephyr bash scripts/ci.sh
   ```

   It checks the layers, builds the board images and the `native_sim` firmware without
   warnings, and runs the ztest suites, the SIL and integration tests and the fixed-point
   motion tests.
4. Add a test for new behaviour: a ztest case for driver behaviour, a test in `tests/sil` for
   the bus, motion or protection path ([Tests and CI](docs/testing.md)).
5. Update the documentation in `docs/` when the behaviour, the configuration or a measured
   figure changes; `mkdocs build` is strict and must pass.
6. Open a pull request and describe what changed, why, and how it was tested.

## Style

- C: the Zephyr coding style (`zephyr/.clang-format` in the west workspace), no floating point above the
  `power_stage` backends, no register access outside them.
- Commit messages: imperative mood, a short first line, the reason in the body.
- English in code, comments and documentation.

## License

By contributing you agree that your contribution is licensed under the
[Apache License 2.0](LICENSE), the license of this project.
