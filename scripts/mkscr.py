#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0

"""Wrap a U-Boot command file into a legacy script image, like mkimage -T script."""

import argparse
import struct
import time
import zlib

IH_MAGIC = 0x27051956
IH_OS_LINUX = 5
IH_ARCH_ARM64 = 22
IH_TYPE_SCRIPT = 6
IH_COMP_NONE = 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="U-Boot command file")
    parser.add_argument("output", help="script image to write")
    parser.add_argument("-n", "--name", default="rpi4 amp", help="image name")
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        script = f.read()

    data = struct.pack(">II", len(script), 0) + script
    name = args.name.encode()[:32].ljust(32, b"\0")

    def header(hcrc):
        return struct.pack(
            ">7I4B32s",
            IH_MAGIC,
            hcrc,
            int(time.time()),
            len(data),
            0,
            0,
            zlib.crc32(data),
            IH_OS_LINUX,
            IH_ARCH_ARM64,
            IH_TYPE_SCRIPT,
            IH_COMP_NONE,
            name,
        )

    with open(args.output, "wb") as f:
        f.write(header(zlib.crc32(header(0))) + data)


if __name__ == "__main__":
    main()
