# SPDX-License-Identifier: Apache-2.0
import hashlib
import struct

import pytest

from doppelstep_gui import image


def make_image(body=b"\x00" * 100, version=(1, 2, 3, 0), protected=True, sha=True):
    hdr_size = 32
    prot = b""
    if protected:
        entry = struct.pack("<HH", 0x50, 4) + b"\x00" * 4
        prot = struct.pack("<HH", image.TLV_PROT_INFO_MAGIC, 4 + len(entry)) + entry
    header = struct.pack("<IIHHII", image.IMAGE_MAGIC, 0, hdr_size, len(prot), len(body), 0)
    header += struct.pack("<BBHI", *version) + b"\x00" * 4
    digest = hashlib.sha256(header + body + prot).digest()
    entries = b""
    if sha:
        entries += struct.pack("<HH", image.TLV_SHA256, 32) + digest
    entries += struct.pack("<HH", 0x01, 4) + b"\x11" * 4     # some other TLV
    unprot = struct.pack("<HH", image.TLV_INFO_MAGIC, 4 + len(entries)) + entries
    return header + body + prot + unprot, digest


def test_parse_reads_version_and_sha():
    data, digest = make_image()
    img = image.parse(data)
    assert img.version == "1.2.3"
    assert img.sha256 == digest
    assert img.fits_slot0


def test_version_text_matches_mcuboot():
    data, _ = make_image(version=(0, 0, 0, 7), protected=False)
    assert image.parse(data).version == "0.0.0.7"


def test_unsigned_file_is_rejected():
    with pytest.raises(image.ImageError, match="zephyr.signed.bin"):
        image.parse(b"\x7fELF" + b"\x00" * 60)


def test_missing_sha_is_rejected():
    data, _ = make_image(sha=False)
    with pytest.raises(image.ImageError, match="SHA-256"):
        image.parse(data)


def test_too_large_for_slot0():
    data, _ = make_image(body=b"\x00" * (image.SLOT0_SIZE + 1))
    assert not image.parse(data).fits_slot0
