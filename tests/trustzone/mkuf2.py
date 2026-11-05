# Wraps a binary for SRAM at 0x20000000 as an RP2350 Arm Secure UF2: mkuf2.py IN.bin OUT.uf2
import struct
import sys

data = open(sys.argv[1], 'rb').read()
base, family = 0x20000000, 0xe48bff59
count = (len(data) + 255) // 256
out = bytearray()
for i in range(count):
    payload = data[i * 256:(i + 1) * 256].ljust(256, b'\0')
    out += struct.pack('<IIIIIIII', 0x0A324655, 0x9E5D5157, 0x2000, base + i * 256, 256, i, count, family)
    out += payload + b'\0' * (476 - 256) + struct.pack('<I', 0x0AB16F30)
open(sys.argv[2], 'wb').write(out)
