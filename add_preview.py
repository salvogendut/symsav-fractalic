#!/usr/bin/env python3
"""Insert screensaver preview image into fractalic.sav at code offset 256."""

import os
import re
import struct

STARFIELD_ASM = os.path.join(os.path.dirname(__file__), '../symsav-starfield/Scr-Starfield.asm')
SAV_FILE = os.path.join(os.path.dirname(__file__), 'fractali.sav')

INSERT_AT = 256
PREVIEW_PIXELS = 640
PREVIEW_TOTAL = 3 + PREVIEW_PIXELS  # 643: 3-byte header + 640 pixel bytes


def parse_preview(asm_path):
    with open(asm_path, 'r', encoding='iso-8859-1') as f:
        lines = f.readlines()

    start = None
    for i, line in enumerate(lines):
        if re.search(r'\bdb\s+16\s*,\s*64\s*,\s*40\b', line, re.IGNORECASE):
            start = i
            break
    if start is None:
        raise ValueError("db 16,64,40 not found in starfield ASM")

    result = bytearray([16, 64, 40])
    for line in lines[start + 1:]:
        line = line.strip()
        if not line or line.startswith(';'):
            continue
        m = re.match(r'db\s+(.*?)(?:\s*;.*)?$', line, re.IGNORECASE)
        if not m:
            break
        for v in re.findall(r'#([0-9A-Fa-f]{1,2})', m.group(1)):
            result.append(int(v, 16))
        if len(result) >= PREVIEW_TOTAL:
            break

    if len(result) < PREVIEW_TOTAL:
        raise ValueError(f"Only got {len(result)} preview bytes, need {PREVIEW_TOTAL}")
    return bytes(result[:PREVIEW_TOTAL])


def patch(sav_path, preview):
    ins = len(preview)

    with open(sav_path, 'rb') as f:
        data = bytearray(f.read())

    code_len    = struct.unpack_from('<H', data, 0)[0]
    data_len    = struct.unpack_from('<H', data, 2)[0]
    trans_len   = struct.unpack_from('<H', data, 4)[0]
    reloc_count = struct.unpack_from('<H', data, 8)[0]

    trans_start = code_len + data_len
    reloc_start = trans_start + trans_len
    assert reloc_start + reloc_count * 2 == len(data), "File size mismatch"

    relocs = list(struct.unpack_from('<' + 'H' * reloc_count, data, reloc_start))

    new_data = data[:INSERT_AT] + bytearray(preview) + data[INSERT_AT:reloc_start]

    new_relocs = [r + ins if r >= INSERT_AT else r for r in relocs]

    for new_r in new_relocs:
        v = struct.unpack_from('<H', new_data, new_r)[0]
        if v >= INSERT_AT:
            struct.pack_into('<H', new_data, new_r, v + ins)

    struct.pack_into('<H', new_data, 0, code_len + ins)

    new_reloc_raw = struct.pack('<' + 'H' * reloc_count, *new_relocs)
    result = bytes(new_data) + new_reloc_raw

    with open(sav_path, 'wb') as f:
        f.write(result)

    print(f"Inserted {ins} bytes at offset {INSERT_AT}")
    print(f"code_len: {code_len} -> {code_len + ins}")
    print(f"File size: {len(data)} -> {len(result)}")


def main():
    preview = parse_preview(STARFIELD_ASM)
    print(f"Parsed {len(preview)} preview bytes from {os.path.basename(STARFIELD_ASM)}")
    patch(SAV_FILE, preview)


if __name__ == '__main__':
    main()
