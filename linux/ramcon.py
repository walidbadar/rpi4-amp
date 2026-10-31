#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0

"""Print Zephyr's RAM console from Linux. Run as root."""

import mmap
import os
import sys

RAM_CONSOLE_BASE = 0x30800000
RAM_CONSOLE_SIZE = 0x10000


def main():
    fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
    try:
        mem = mmap.mmap(fd, RAM_CONSOLE_SIZE, mmap.MAP_SHARED, mmap.PROT_READ,
                        offset=RAM_CONSOLE_BASE)
    finally:
        os.close(fd)

    # /dev/mem maps memory hidden from Linux as Device memory: aligned 32-bit reads only
    words = memoryview(mem).cast("I")
    buf = b"".join(w.to_bytes(4, "little") for w in words)
    mem.close()

    sys.stdout.write(buf.replace(b"\0", b"").decode(errors="replace"))


if __name__ == "__main__":
    main()
