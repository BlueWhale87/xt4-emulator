"""Offline decoder recovered from X-T4 2.12 bytes, limited to this image.

The token interpretation is analyst-derived. Exact declared sizes and complete
input consumption are checked, but do not replace a camera RAM comparison.
No firmware patch, target execution, or hardware access occurs here.
"""
from pathlib import Path
import hashlib
import json
import struct

BASE = Path(__file__).resolve().parent
EXPECTED_SHA256 = "853d273b5603d3a2b93493f0ebd6a762b0b4b6bdc70317777775ae5f8ed93bc5"
SEGMENTS = (0x260274, 0x5C0274, 0xAE0274, 0x1A00274)


def decode_tokens(packed, max_output=128 * 1024 * 1024):
    output = bytearray()
    pos = 0
    while pos < len(packed):
        token = packed[pos]
        pos += 1
        if token < 128:
            if token == 0 or pos + token > len(packed):
                raise ValueError(f"Unsupported or truncated literal at {pos - 1:#x}")
            if len(output) + token > max_output:
                raise ValueError("Expansion exceeds declared output limit")
            output.extend(packed[pos:pos + token])
            pos += token
        else:
            if pos == len(packed):
                raise ValueError("Truncated match")
            second = packed[pos]
            pos += 1
            distance = ((token & 127) << 4) | (second >> 4)
            length = second & 15
            if not length or not distance or distance > len(output):
                raise ValueError(f"Unsupported match at {pos - 2:#x}")
            if len(output) + length > max_output:
                raise ValueError("Expansion exceeds declared output limit")
            for _ in range(length):
                output.append(output[-distance])
    return bytes(output)


def unpack_all():
    original = (BASE / "FWUP0024.DAT").read_bytes()
    if hashlib.sha256(original).hexdigest() != EXPECTED_SHA256:
        raise ValueError("Input is not the inspected official X-T4 2.12 image")
    transformed = bytes(b ^ 255 for b in original)
    destination = BASE / "unpacked"
    destination.mkdir(exist_ok=True)
    manifest = []
    for offset in SEGMENTS:
        unpacked_size, packed_size, page_count, page_size, parameter = struct.unpack_from("<5I", transformed, offset)
        if page_size != 0x4000 or parameter != 4 or page_count != (packed_size + page_size - 1) // page_size:
            raise ValueError("Unexpected segment header")
        end = offset + packed_size
        if packed_size < 20 or end > len(transformed):
            raise ValueError("Invalid segment bounds")
        output = decode_tokens(transformed[offset + 20:end], max_output=unpacked_size)
        if len(output) != unpacked_size:
            raise ValueError("Decoded size differs from header")
        output_path = destination / f"segment-{offset:x}.bin"
        output_path.write_bytes(output)
        manifest.append({
            "original_dat_offset": offset,
            "original_dat_offset_hex": hex(offset),
            "original_dat_end_exclusive": end,
            "packed_bytes_including_header": packed_size,
            "declared_unpacked_bytes": unpacked_size,
            "decoded_bytes": len(output),
            "input_fully_consumed": True,
            "raw_header_words": [unpacked_size, packed_size, page_count, page_size, parameter],
            "output_path": str(output_path.relative_to(BASE)),
            "output_sha256": hashlib.sha256(output).hexdigest(),
            "runtime_address": None,
        })
    result = {
        "original_sha256": EXPECTED_SHA256,
        "transform": "bitwise inversion of original bytes at selected segment positions",
        "token_format": {"literal": "0 < token < 128: token bytes follow", "match": "two bytes; distance=((first&127)<<4)|(second>>4), length=second&15; overlapping copy"},
        "header_semantics": "first two words interpreted as unpacked and packed sizes; remaining words validated empirically, meanings not fully established",
        "verification": "four exact declared output sizes, complete compressed-input consumption, valid backward references; no independent RAM/checksum comparison yet",
        "segments": manifest,
    }
    (destination / "manifest.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    unpack_all()
