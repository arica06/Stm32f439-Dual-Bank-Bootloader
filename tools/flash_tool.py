#!/usr/bin/env python3
"""Host tool for the STM32F439 dual-bank (A/B) UART bootloader.

    python flash_tool.py COM5 info
    python flash_tool.py COM5 flash app.bin --version 3 --boot
    python flash_tool.py COM5 confirm      # accept the image that is on trial
    python flash_tool.py COM5 rollback     # go back to the previous image
    python flash_tool.py COM5 boot

The image is always written to the INACTIVE slot. Start the tool, then press
RESET (or hold B1 + RESET) on the board: the tool keeps sending INFO requests
until the bootloader answers.
"""
import argparse
import struct
import sys
import time
import zlib

import serial  # pip install pyserial

SOF = 0xA5
CMD_INFO, CMD_BEGIN, CMD_DATA, CMD_END, CMD_BOOT = 1, 2, 3, 4, 5
CMD_CONFIRM, CMD_ROLLBACK = 7, 8
ACK, NACK = 0x06, 0x15
CHUNK = 256
FLAG_NO_TRIAL = 1
ERR_FRAME = 1
ERRORS = {
    1: "frame", 2: "state", 3: "param", 4: "flash", 5: "crc", 6: "no bootable image",
    7: "bad image", 8: "new image still on trial: run 'confirm' or 'rollback' first",
}
STATES = {0: "confirmed", 1: "trial"}
INFO_FORMAT = "<IIBBBBII"          # 20 bytes
INFO_SIZE = struct.calcsize(INFO_FORMAT)


def make_frame(cmd, payload=b""):
    body = bytes([cmd]) + struct.pack("<H", len(payload)) + payload
    return bytes([SOF]) + body + struct.pack("<I", zlib.crc32(body))


def read_frame(ser, timeout):
    ser.timeout = timeout
    while True:
        b = ser.read(1)
        if not b:
            raise TimeoutError("no response")
        if b[0] == SOF:
            break
    hdr = ser.read(3)
    if len(hdr) < 3:
        raise TimeoutError("short header")
    cmd, ln = hdr[0], struct.unpack("<H", hdr[1:3])[0]
    rest = ser.read(ln + 4)
    if len(rest) < ln + 4:
        raise TimeoutError("short frame")
    payload = rest[:ln]
    (crc,) = struct.unpack("<I", rest[ln:])
    if zlib.crc32(hdr + payload) != crc:
        raise IOError("bad CRC from target")
    return cmd, payload


def transact(ser, cmd, payload=b"", timeout=2.0, retries=3):
    last = None
    for _ in range(retries):
        ser.write(make_frame(cmd, payload))
        try:
            rcmd, rpayload = read_frame(ser, timeout)
        except (TimeoutError, IOError) as e:
            last = e
            continue
        if rcmd == ACK:
            return rpayload
        code = rpayload[0] if rpayload else 0
        if code == ERR_FRAME:          # corrupted on the wire, resend
            last = IOError("target saw a corrupted frame")
            continue
        raise RuntimeError(f"target NACK: {ERRORS.get(code, code)}")
    raise RuntimeError(f"no valid answer after {retries} tries: {last}")


def parse_info(payload):
    if len(payload) != INFO_SIZE:
        raise ValueError(f"INFO reply must be {INFO_SIZE} bytes, got {len(payload)}")
    bl_ver, slot, active, state, tries, mask, v0, v1 = struct.unpack(INFO_FORMAT, payload)
    return {
        "bootloader": f"{bl_ver >> 16}.{(bl_ver >> 8) & 0xFF}.{bl_ver & 0xFF}",
        "slot_size": slot,
        "active": active,
        "target": 1 - active,                       # where the next image will be written
        "state": STATES.get(state, str(state)),
        "tries": tries,
        "present": [bool(mask & 1), bool(mask & 2)],
        "metadata_valid": bool(mask & 0x80),
        "version": [v0, v1],
    }


def sync(ser, wait_s):
    deadline = time.time() + wait_s
    frame = make_frame(CMD_INFO)
    while time.time() < deadline:
        ser.write(frame)
        try:
            cmd, payload = read_frame(ser, 0.05)
            if cmd == ACK and len(payload) == INFO_SIZE:
                return parse_info(payload)
        except (TimeoutError, IOError):
            pass
    raise SystemExit("bootloader did not answer; press RESET while the tool is running")


def show_info(info):
    print(f"bootloader {info['bootloader']}, slot size {info['slot_size'] // 1024} KB, "
          f"metadata {'valid' if info['metadata_valid'] else 'blank'}")
    print(f"active slot {info['active']} ({info['state']}"
          + (f", {info['tries']} boot attempt(s) left" if info['state'] == 'trial' else "") + ")")
    for b in (0, 1):
        print(f"  slot {b}: {'image present' if info['present'][b] else 'empty'}, "
              f"version {info['version'][b]}" + ("  <- next update goes here" if b == info['target'] else ""))


def do_flash(ser, args, info):
    data = open(args.image, "rb").read()
    data += b"\xff" * (-len(data) % 4)            # word-align for flash programming
    crc = zlib.crc32(data)
    if len(data) > info["slot_size"]:
        raise SystemExit(f"image too large: {len(data)} > {info['slot_size']}")
    if info["state"] == "trial":
        raise SystemExit("the active image is still on trial: run 'confirm' or 'rollback' first")

    print(f"image: {len(data)} bytes, crc32 0x{crc:08X} -> slot {info['target']}")
    print("erasing...")
    flags = FLAG_NO_TRIAL if args.no_trial else 0
    transact(ser, CMD_BEGIN, struct.pack("<IIII", len(data), crc, args.version, flags), timeout=60.0)

    t0 = time.time()
    for off in range(0, len(data), CHUNK):
        chunk = data[off:off + CHUNK]
        transact(ser, CMD_DATA, struct.pack("<I", off) + chunk, timeout=2.0, retries=5)
        print(f"\rwriting {(off + len(chunk)) * 100 // len(data):3d}%", end="", flush=True)
    print(f"\nwritten in {time.time() - t0:.1f} s")

    transact(ser, CMD_END, timeout=10.0)
    print("verified, metadata updated" + (" (installed as confirmed)" if args.no_trial else " (on trial)"))
    if args.boot:
        transact(ser, CMD_BOOT)
        print("booting the new image")


def main():
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--baud", type=int, default=115200)
    common.add_argument("--wait", type=float, default=20.0, help="seconds to wait for the bootloader")

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    sub = ap.add_subparsers(dest="command", required=True)

    sub.add_parser("info", parents=[common], help="show slot and trial state")
    fl = sub.add_parser("flash", parents=[common], help="upload an image to the inactive slot")
    fl.add_argument("image", help="application .bin linked at 0x08010000")
    fl.add_argument("--version", type=int, default=1)
    fl.add_argument("--no-trial", action="store_true",
                    help="install as confirmed (no automatic rollback; for apps that cannot confirm yet)")
    fl.add_argument("--boot", action="store_true", help="start the new image afterwards")
    sub.add_parser("confirm", parents=[common], help="accept the image that is on trial")
    sub.add_parser("rollback", parents=[common], help="return to the previous image")
    sub.add_parser("boot", parents=[common], help="start the active image")

    args = ap.parse_args()
    ser = serial.Serial(args.port, args.baud, timeout=1)
    ser.reset_input_buffer()
    info = sync(ser, args.wait)
    show_info(info)

    if args.command == "flash":
        do_flash(ser, args, info)
    elif args.command == "confirm":
        transact(ser, CMD_CONFIRM)
        print("image confirmed")
    elif args.command == "rollback":
        transact(ser, CMD_ROLLBACK)
        print("rolled back to the previous image")
    elif args.command == "boot":
        transact(ser, CMD_BOOT)
        print("booting")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, serial.SerialException) as e:
        sys.exit(f"error: {e}")
