# SPDX-License-Identifier: Apache-2.0
"""Signed MCUboot image (zephyr.signed.bin): header, version and SHA-256 TLV.

MCUboot serial recovery lists only images that pass its check, and the "hash"
it reports is the SHA-256 TLV of the image (boot_serial.c, boot_serial_get_hash).
Comparing that hash with the TLV of the file proves which image slot 0 holds.

Format: MCUboot docs/design.md (struct image_header, image_tlv_info, image_tlv).
"""

import struct
from dataclasses import dataclass

IMAGE_MAGIC = 0x96F3B83D
TLV_INFO_MAGIC = 0x6907
TLV_PROT_INFO_MAGIC = 0x6908
TLV_SHA256 = 0x10

# slot0_partition of boards/st/b_g431b_esc1/b_g431b_esc1.dts (docs/firmware-update.md)
SLOT0_SIZE = 86 * 1024


class ImageError(ValueError):
    pass


@dataclass(frozen=True)
class Image:
    data: bytes
    version: str
    header_size: int
    body_size: int
    sha256: bytes

    @property
    def size(self):
        return len(self.data)

    @property
    def fits_slot0(self):
        return self.size <= SLOT0_SIZE


def _tlvs(data, offset, magic):
    """{type: value} of one TLV area at `offset`; returns (tlvs, end)."""
    if len(data) < offset + 4:
        raise ImageError("TLV area missing")
    info_magic, total = struct.unpack_from("<HH", data, offset)
    if info_magic != magic:
        raise ImageError(f"bad TLV info magic 0x{info_magic:04x} at {offset}")
    end = offset + total
    if end > len(data):
        raise ImageError("TLV area runs past the end of the file")
    tlvs = {}
    pos = offset + 4
    while pos < end:
        if pos + 4 > end:
            raise ImageError("truncated TLV entry")
        kind, length = struct.unpack_from("<HH", data, pos)
        tlvs.setdefault(kind, data[pos + 4:pos + 4 + length])
        pos += 4 + length
    return tlvs, end


def parse(data):
    """Image of the bytes of a signed MCUboot image."""
    data = bytes(data)
    if len(data) < 32:
        raise ImageError("file too short for an MCUboot header")
    magic, _load, hdr_size, prot_size, img_size, _flags = struct.unpack_from("<IIHHII", data, 0)
    if magic != IMAGE_MAGIC:
        raise ImageError("not a signed MCUboot image (bad magic): use zephyr.signed.bin")
    major, minor, revision, build = struct.unpack_from("<BBHI", data, 20)
    offset = hdr_size + img_size
    tlvs = {}
    if prot_size:
        prot, offset = _tlvs(data, offset, TLV_PROT_INFO_MAGIC)
        tlvs.update(prot)
    unprot, _end = _tlvs(data, offset, TLV_INFO_MAGIC)
    tlvs.update(unprot)
    sha = tlvs.get(TLV_SHA256)
    if sha is None or len(sha) != 32:
        raise ImageError("no SHA-256 TLV in the image")
    # same text as bs_list_img_ver() of MCUboot serial recovery
    version = f"{major}.{minor}.{revision}" + (f".{build}" if build else "")
    return Image(data, version, hdr_size, img_size, bytes(sha))


def load(path):
    with open(path, "rb") as f:
        return parse(f.read())
