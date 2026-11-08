# Tests

ztest suites of the parts, and a pytest suite against the `native_sim` firmware with the simulated
motor. The full description is in [docs/testing.md](../docs/testing.md).

```bash
bash scripts/run-tests.sh                              # Linux / Dev Container / Docker
docker compose run --rm zephyr bash scripts/run-tests.sh   # Windows host
docker compose run --rm zephyr bash scripts/ci.sh          # like CI: layers, builds, all tests
west twister -T tests/app -T tests/drivers -p native_sim/native/64 --integration  # ztest suites
```

| Path | Content |
|---|---|
| `conftest.py`, `firmware.py`, `support.py` | fixtures, firmware process (bus PTY, model log), constants |
| `app/`, `drivers/` | ztest suites; `drivers/common/` is a fake power stage |
| `sil/` | firmware + plant model through raw Protocol 2.0 (`tools/sim/dxl2.py`) |
| `integration/` | official DynamixelSDK, the ros2_control loop |
| `requirements.txt` | pinned packages (same versions as the `Dockerfile`) |
