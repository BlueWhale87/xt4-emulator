"""Recover the pinned original bootstrap without executing or patching it."""
import hashlib

ORIGINAL_SHA256 = '853d273b5603d3a2b93493f0ebd6a762b0b4b6bdc70317777775ae5f8ed93bc5'
BOOTSTRAP_OFFSET = 0x20274
BOOTSTRAP_SIZE = 0xc880
BOOTSTRAP_SHA256 = 'bf2d1d4ca9d1d60e5125a964988a3f1852e177c3176d35dc59dcc63afe41f6b1'
CALIBRATION_DAT_OFFSET = 0x2a2f4
CALIBRATION_SIZE = 128
CALIBRATION_SHA256 = '10fa7918107458a1b72e18ef91b9bfc27b6eb76e3b562aa6566c5561dcf4929a'


def extract_bootstrap(original: bytes) -> bytes:
    if len(original) != 51278280 or hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('Original DAT fingerprint mismatch')
    data = bytes(value ^ 0xff for value in
                 original[BOOTSTRAP_OFFSET:BOOTSTRAP_OFFSET + BOOTSTRAP_SIZE])
    if len(data) != BOOTSTRAP_SIZE or hashlib.sha256(data).hexdigest() != BOOTSTRAP_SHA256:
        raise ValueError('Bootstrap fingerprint mismatch')
    return data


def extract_calibration(original: bytes) -> bytes:
    if len(original) != 51278280 or hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('Original DAT fingerprint mismatch')
    data = bytes(value ^ 0xff for value in
                 original[CALIBRATION_DAT_OFFSET:CALIBRATION_DAT_OFFSET + CALIBRATION_SIZE])
    if len(data) != CALIBRATION_SIZE or hashlib.sha256(data).hexdigest() != CALIBRATION_SHA256:
        raise ValueError('Calibration extent fingerprint mismatch')
    return data
