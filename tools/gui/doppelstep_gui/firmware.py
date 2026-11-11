# SPDX-License-Identifier: Apache-2.0
"""Firmware update over USART2 with MCUboot serial recovery (docs/firmware-update.md).

Sequence:
  1. DYNAMIXEL Reboot (resets the MCU on the board), then the DYNAMIXEL port is closed
  2. SMP on the same port at 1 Mbps: echo repeated until MCUboot answers in its window
  3. image list before the upload (what slot 0 held)
  4. image upload into slot 0
  5. image list: slot 0 must hold an image whose hash is the SHA-256 TLV of the file
  6. os reset
  7. the DYNAMIXEL port is opened again: the application must answer the ping as XM430-W350

The SMP client is smpclient (dependency of smpmgr, requirements.txt), used in this
process: a separate smpmgr process can start too late for the 500 ms window.
"""

import asyncio
import time
from dataclasses import dataclass, field

from .device import is_doppelstep
from .registers import XM430_W350_MODEL

# MCUboot of the board: fixed 1 Mbps, SMP line buffers 128 bytes x 8
# (app/sysbuild/mcuboot.conf, docs/firmware-update.md)
SMP_BAUD = 1000000
SMP_LINE_LENGTH = 128
SMP_LINE_BUFFERS = 8

# Tool timeouts (seconds): how long this program waits, not properties of the firmware
ECHO_INTERVAL = 0.05          # one SMP echo attempt
ATTACH_TIMEOUT = 3.0          # echoes after the Reboot until MCUboot answers
UPLOAD_FIRST_TIMEOUT = 40.0   # first chunk: MCUboot erases slot 0 first
UPLOAD_CHUNK_TIMEOUT = 2.5
LIST_TIMEOUT = 2.0
APP_START_TIMEOUT = 5.0       # application answers DYNAMIXEL after os reset


@dataclass
class Step:
    name: str
    ok: bool
    detail: str
    seconds: float


@dataclass
class UpdateResult:
    file: str
    file_version: str
    file_sha256: str
    size: int
    steps: list = field(default_factory=list)
    started: float = field(default_factory=time.time)

    @property
    def ok(self):
        return bool(self.steps) and all(s.ok for s in self.steps)


class UpdateFailed(Exception):
    pass


class SmpSession:
    """smpclient on one serial port (the real implementation of the SMP session)."""

    def __init__(self, port):
        self.port = port
        self.client = None

    def prepare(self):
        """Import smpclient now, before the Reboot: the import takes about 0.8 s on
        Windows, longer than the 500 ms in which MCUboot waits for SMP."""
        import smpclient  # noqa: F401
        import smpclient.generics  # noqa: F401
        import smpclient.requests.image_management  # noqa: F401
        import smpclient.requests.os_management  # noqa: F401
        import smpclient.transport.serial  # noqa: F401

    async def open(self):
        from smpclient import SMPClient
        from smpclient.transport.serial import BufferParams, SMPSerialTransport

        transport = SMPSerialTransport(BufferParams(line_length=SMP_LINE_LENGTH,
                                                    line_buffers=SMP_LINE_BUFFERS),
                                       baudrate=SMP_BAUD)
        self.client = SMPClient(transport, self.port, timeout_s=ECHO_INTERVAL)
        # connect() also sends an MCUmgr parameters request; MCUboot answers it
        # with "not supported", and any SMP request inside the window keeps it
        # in serial recovery.
        await self.client.connect(connect_timeout_s=1.0)

    async def close(self):
        if self.client is not None:
            await self.client.disconnect()
            self.client = None

    async def echo(self, timeout):
        """True when MCUboot answers at all. Without CONFIG_BOOT_MGMT_ECHO it answers
        rc 8 (not supported), and it is in serial recovery all the same."""
        from smpclient.requests.os_management import EchoWrite

        try:
            await self.client.request(EchoWrite(d="doppelstep"), timeout_s=timeout)
        except TimeoutError:
            return False
        return True

    async def images(self):
        """[(slot, version, hash bytes or None)] listed by MCUboot."""
        from smpclient.generics import success
        from smpclient.requests.image_management import ImageStatesRead

        response = await self.client.request(ImageStatesRead(), timeout_s=LIST_TIMEOUT)
        if not success(response):
            raise UpdateFailed(f"image list refused: {response}")
        return [(img.slot, img.version, bytes(img.hash) if img.hash is not None else None)
                for img in response.images]

    async def upload(self, data, progress):
        async for offset in self.client.upload(data, slot=0,
                                               first_timeout_s=UPLOAD_FIRST_TIMEOUT,
                                               subsequent_timeout_s=UPLOAD_CHUNK_TIMEOUT):
            progress(offset, len(data))

    async def reset(self):
        from smpclient.requests.os_management import ResetWrite

        await self.client.request(ResetWrite(), timeout_s=LIST_TIMEOUT)


class FirmwareUpdater:
    """Runs the sequence; the callers give the link factories so tests can fake them.

    open_bus(): device.Bus on the DYNAMIXEL port at its baud rate (opened again at the end)
    smp_session(): object with the coroutines of SmpSession
    """

    def __init__(self, image, path, dxl_id, open_bus, smp_session, log=print,
                 progress=lambda done, total: None):
        self.image = image
        self.dxl_id = dxl_id
        self.open_bus = open_bus
        self.smp_session = smp_session
        self.log = log
        self.progress = progress
        self.bus = None                  # DYNAMIXEL bus opened again after the update
        self.identity = None             # (model, firmware version) of the application
        self.result = UpdateResult(path, image.version, image.sha256.hex(), image.size)

    def _step(self, name, ok, detail, t0):
        step = Step(name, ok, detail, time.monotonic() - t0)
        self.result.steps.append(step)
        self.log(f"{'PASS' if ok else 'FAIL'}  {name}: {detail}")
        if not ok:
            raise UpdateFailed(f"{name}: {detail}")

    def run(self, bus=None):
        """Run the update. `bus`: the open DYNAMIXEL bus, closed here; None if not connected."""
        try:
            t0 = time.monotonic()
            self._step("Image file", self.image.fits_slot0,
                       f"{self.image.size} bytes, version {self.image.version}, "
                       f"SHA-256 {self.image.sha256.hex()[:16]}…"
                       + ("" if self.image.fits_slot0 else " — larger than slot 0 (86 KiB)"), t0)
            # Ready before the Reboot: MCUboot listens for 500 ms only
            session = self.smp_session()
            prepare = getattr(session, "prepare", None)
            if prepare is not None:
                prepare()
            self._reboot(bus)
            asyncio.run(self._smp(session))
            self._application()
        except UpdateFailed:
            pass
        except Exception as exc:  # noqa: BLE001 - reported to the operator, not hidden
            self.result.steps.append(Step("Unexpected error", False, repr(exc), 0.0))
            self.log(f"FAIL  unexpected error: {exc!r}")
        return self.result

    def _reboot(self, bus):
        t0 = time.monotonic()
        if bus is None:
            self._step("DYNAMIXEL Reboot", True,
                       "not connected: MCUboot must already be in serial recovery "
                       "(no valid image) or be reset by hand", t0)
            return
        try:
            bus.reboot(self.dxl_id)
            detail = f"ID {self.dxl_id} answered the Reboot; the MCU resets"
        except TimeoutError:
            detail = "no answer to the Reboot: continuing (MCUboot may already be in recovery)"
        finally:
            bus.close()
        self._step("DYNAMIXEL Reboot", True, detail, t0)

    async def _smp(self, session):
        t0 = time.monotonic()
        try:
            await session.open()
            attempts = 0
            while True:
                attempts += 1
                if await session.echo(ECHO_INTERVAL):
                    break
                if time.monotonic() - t0 > ATTACH_TIMEOUT:
                    self._step("MCUboot serial recovery", False,
                               f"no SMP answer after {attempts} echoes in {ATTACH_TIMEOUT} s. "
                               "Reconnect at the right baud rate and retry, or reset the "
                               "board by hand right before pressing Update", t0)
            self._step("MCUboot serial recovery", True,
                       f"answered echo {attempts} after {time.monotonic() - t0:.2f} s", t0)

            t0 = time.monotonic()
            before = await session.images()
            self._step("Image list before", True, _images_text(before), t0)

            t0 = time.monotonic()
            await session.upload(self.image.data, self.progress)
            self._step("Upload", True, f"{self.image.size} bytes into slot 0", t0)

            t0 = time.monotonic()
            after = await session.images()
            slot0 = [img for img in after if img[0] == 0]
            if not slot0:
                self._step("Image check", False,
                           "MCUboot lists no valid image in slot 0: the signature or hash "
                           "check failed", t0)
            _slot, version, digest = slot0[0]
            if digest is None:
                self._step("Image check", False,
                           "MCUboot reports no hash (CONFIG_BOOT_SERIAL_IMG_GRP_HASH off?)", t0)
            same = digest == self.image.sha256 and version == self.image.version
            self._step("Image check", same,
                       f"slot 0: version {version}, hash {digest.hex()[:16]}…"
                       + (" = file" if same else
                          f" ≠ file (version {self.image.version}, "
                          f"hash {self.image.sha256.hex()[:16]}…)"), t0)

            t0 = time.monotonic()
            await session.reset()
            self._step("Reset", True, "os reset sent", t0)
        finally:
            await session.close()

    def _application(self):
        t0 = time.monotonic()
        try:
            bus = self.open_bus()
        except Exception as exc:  # noqa: BLE001
            self._step("Application", False, f"cannot open the port again: {exc}", t0)
        try:
            model, firmware, waited = bus.wait_for_ping(self.dxl_id, APP_START_TIMEOUT)
        except TimeoutError:
            bus.close()
            self._step("Application", False,
                       f"ID {self.dxl_id} did not answer within {APP_START_TIMEOUT} s after "
                       "the reset", t0)
        self.bus = bus
        self.identity = (model, firmware)
        self._step("Application", is_doppelstep(model),
                   f"answered after {waited:.2f} s: model {model}"
                   + ("" if is_doppelstep(model) else f", expected {XM430_W350_MODEL}")
                   + f", firmware {firmware}", t0)


def _images_text(images):
    if not images:
        return "no valid image"
    return "; ".join(f"slot {slot}: {version}, hash "
                     f"{digest.hex()[:16] + '…' if digest else 'n/a'}"
                     for slot, version, digest in images)
