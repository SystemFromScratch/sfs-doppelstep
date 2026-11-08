## What and why

<!-- What changes, and the reason. Link the issue, if there is one. -->

## How it was tested

<!-- ci.sh, new tests, and for hardware-dependent changes the bring-up step or the check of
docs/hardware-checklist.md that was run on a board, with its result. -->

## Checklist

- [ ] `docker compose run --rm zephyr bash scripts/ci.sh` passes
- [ ] New behaviour has a test (ztest case or `tests/sil`)
- [ ] `docs/` updated, `mkdocs build` passes; measured figures re-measured if they changed
