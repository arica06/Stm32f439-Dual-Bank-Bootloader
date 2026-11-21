import struct
import sys
import zlib
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import flash_tool as ft  # noqa: E402


class FakeSerial:
    """Minimal stand-in for serial.Serial: serves bytes from a buffer."""

    def __init__(self, data: bytes):
        self.buf = bytearray(data)
        self.timeout = None

    def read(self, n: int) -> bytes:
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out


def test_frame_layout():
    f = ft.make_frame(ft.CMD_DATA, b"\x01\x02")
    assert f[0] == 0xA5
    assert f[1] == ft.CMD_DATA
    assert f[2:4] == b"\x02\x00"                       # little-endian length
    assert struct.unpack("<I", f[-4:])[0] == zlib.crc32(f[1:-4])


def test_roundtrip():
    cmd, payload = ft.read_frame(FakeSerial(ft.make_frame(ft.ACK, b"abc")), 0.1)
    assert (cmd, payload) == (ft.ACK, b"abc")


def test_garbage_before_sof_is_skipped():
    frame = b"\x00\x11\x22" + ft.make_frame(ft.NACK, b"\x05")
    cmd, payload = ft.read_frame(FakeSerial(frame), 0.1)
    assert (cmd, payload) == (ft.NACK, b"\x05")


def test_bad_crc_is_detected():
    frame = bytearray(ft.make_frame(ft.ACK, b"abc"))
    frame[-1] ^= 0xFF
    with pytest.raises(IOError):
        ft.read_frame(FakeSerial(bytes(frame)), 0.1)


def test_timeout_on_empty_line():
    with pytest.raises(TimeoutError):
        ft.read_frame(FakeSerial(b""), 0.1)


def test_new_commands_are_distinct():
    ids = [ft.CMD_INFO, ft.CMD_BEGIN, ft.CMD_DATA, ft.CMD_END, ft.CMD_BOOT,
           ft.CMD_CONFIRM, ft.CMD_ROLLBACK, ft.ACK, ft.NACK]
    assert len(set(ids)) == len(ids)


def test_begin_payload_has_four_words():
    payload = struct.pack("<IIII", 1024, 0xDEADBEEF, 3, ft.FLAG_NO_TRIAL)
    assert len(payload) == 16


def test_parse_info():
    raw = struct.pack(ft.INFO_FORMAT, 0x00020000, 0xF0000, 1, 1, 2, 0x83, 5, 6)
    info = ft.parse_info(raw)
    assert info["bootloader"] == "2.0.0"
    assert info["active"] == 1 and info["target"] == 0
    assert info["state"] == "trial" and info["tries"] == 2
    assert info["present"] == [True, True] and info["metadata_valid"]
    assert info["version"] == [5, 6]


def test_parse_info_blank_device():
    raw = struct.pack(ft.INFO_FORMAT, 0x00020000, 0xF0000, 0, 0, 0, 0x00, 0, 0)
    info = ft.parse_info(raw)
    assert info["target"] == 1 and info["state"] == "confirmed"
    assert info["present"] == [False, False] and not info["metadata_valid"]


def test_parse_info_rejects_wrong_length():
    with pytest.raises(ValueError):
        ft.parse_info(b"\x00" * 19)
