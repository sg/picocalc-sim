#!/usr/bin/env python3
# A USB/IP client for testing picocalc-sim's --usbip server without a kernel to attach to: it
# imports the device, enumerates it as Linux would, then drives one of its interfaces.
#   ci/usbip-client.py HOST PORT drive [UF2]   reads the boot ROM's drive and copies UF2 onto it
#   ci/usbip-client.py HOST PORT serial        talks to tests/usbserial over its serial port
# It prints what it found, and exits non-zero if anything goes wrong.
import socket
import struct
import sys
import time

USBIP_VERSION = 0x0111
URB_SHORT_NOT_OK = 0x1
EPIPE, EREMOTEIO = -32, -121


class Client:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=30)
        self.seq = 0
        self.waiting = {}

    def recv(self, n):
        b = b""
        while len(b) < n:
            chunk = self.s.recv(n - len(b))
            if not chunk:
                raise EOFError("the server closed the connection")
            b += chunk
        return b

    def devlist(self):
        self.s.sendall(struct.pack(">HHI", USBIP_VERSION, 0x8005, 0))
        ver, code, status, n = struct.unpack(">HHII", self.recv(12))
        return n

    def import_(self, busid="1-1"):
        self.s.sendall(struct.pack(">HHI32s", USBIP_VERSION, 0x8003, 0, busid.encode()))
        ver, code, status = struct.unpack(">HHI", self.recv(8))
        if status:
            raise RuntimeError("import refused")
        d = self.recv(0x138)
        vid, pid = struct.unpack(">HH", d[0x12C:0x130])
        return vid, pid

    def submit(self, ep, direction_in, length, setup=b"\0" * 8, data=b"", flags=0, interval=0):
        self.seq += 1
        hdr = struct.pack(">IIIII", 1, self.seq, (1 << 16) | 1, 1 if direction_in else 0, ep)
        hdr += struct.pack(">IIiii", flags | (0x200 if direction_in else 0), length, 0, -1, interval)
        self.s.sendall(hdr + setup + (b"" if direction_in else data))
        return self.seq

    def unlink(self, seqnum):
        self.seq += 1
        hdr = struct.pack(">IIIIII", 2, self.seq, (1 << 16) | 1, 0, 0, seqnum) + b"\0" * 24
        self.s.sendall(hdr)
        return self.seq

    def reply(self, seqnum):
        while seqnum not in self.waiting:
            hdr = self.recv(48)
            cmd, seq = struct.unpack(">II", hdr[:8])
            status, actual = struct.unpack(">iI", hdr[20:28])
            data = b""
            if cmd == 3 and actual and self.dirs.get(seq):
                data = self.recv(actual)
            self.waiting[seq] = (cmd, status, data, actual)
        return self.waiting.pop(seqnum)

    dirs = {}

    def xfer(self, ep, direction_in, length, setup=b"\0" * 8, data=b"", flags=0):
        seq = self.submit(ep, direction_in, length, setup, data, flags)
        self.dirs[seq] = direction_in
        cmd, status, out, actual = self.reply(seq)
        return status, out, actual

    def control(self, req_type, req, value, index, length, data=b""):
        setup = struct.pack("<BBHHH", req_type, req, value, index, length)
        status, out, actual = self.xfer(0, bool(req_type & 0x80), length, setup, data)
        if status:
            raise RuntimeError("control %02x %02x failed: %d" % (req_type, req, status))
        return out


def enumerate_device(c):
    dev = c.control(0x80, 6, 0x0100, 0, 64)
    dev = c.control(0x80, 6, 0x0100, 0, 18)
    cfg = c.control(0x80, 6, 0x0200, 0, 9)
    total = struct.unpack("<H", cfg[2:4])[0]
    cfg = c.control(0x80, 6, 0x0200, 0, total)
    langs = c.control(0x80, 6, 0x0300, 0, 255)
    product = b""
    if dev[15]:
        product = c.control(0x80, 6, 0x0300 | dev[15], 0x0409, 255)[2:].decode("utf-16-le")
    c.control(0x00, 9, cfg[5], 0, 0)
    return dev, cfg, product


def endpoints(cfg, klass):
    i, cur, eps, iface = 0, None, {}, None
    while i + 2 <= len(cfg) and cfg[i] >= 2:
        d = cfg[i : i + cfg[i]]
        if d[1] == 4:
            cur = d[5] == klass and d[3] == 0
            if cur and iface is None:
                iface = d[2]
        elif d[1] == 5 and cur and (d[3] & 3) == 2:
            eps["in" if d[2] & 0x80 else "out"] = d[2] & 15
        i += cfg[i]
    return iface, eps


def clear_halt(c, ep):
    c.control(0x02, 1, 0, ep, 0)


# One command as Linux's usb-storage runs it: a reply shorter than asked for ends the data stage
# with -EREMOTEIO, and a halted pipe is cleared before the CSW is read.
def scsi(c, eps, cb, length, data_in=True, data=b""):
    c.tag = getattr(c, "tag", 0) + 1
    cbw = struct.pack("<IIIBBB", 0x43425355, c.tag, length, 0x80 if data_in else 0, 0, len(cb)) + cb.ljust(16, b"\0")
    status, _, _ = c.xfer(eps["out"], False, 31, data=cbw)
    if status:
        raise RuntimeError("CBW failed: %d" % status)
    got = b""
    if length:
        if data_in:
            status, got, _ = c.xfer(eps["in"], True, length, flags=URB_SHORT_NOT_OK)
            if status == EREMOTEIO:
                status = 0
        else:
            status, _, _ = c.xfer(eps["out"], False, length, data=data)
        if status == EPIPE:
            clear_halt(c, (0x80 | eps["in"]) if data_in else eps["out"])
        elif status:
            raise RuntimeError("SCSI data stage failed: %d" % status)
    status, csw, _ = c.xfer(eps["in"], True, 13)
    if status == EPIPE:
        clear_halt(c, 0x80 | eps["in"])
        status, csw, _ = c.xfer(eps["in"], True, 13)
    if status or len(csw) != 13 or struct.unpack("<I", csw[:4])[0] != 0x53425355:
        raise RuntimeError("bad CSW")
    return csw[12], got


def drive(c, cfg, uf2):
    iface, eps = endpoints(cfg, 8)
    st, inq = scsi(c, eps, bytes([0x12, 0, 0, 0, 36, 0]), 36)
    st, _ = scsi(c, eps, bytes(6), 0)
    st, cap = scsi(c, eps, bytes([0x25] + [0] * 9), 8)
    blocks = struct.unpack(">I", cap[:4])[0] + 1
    st, mbr = scsi(c, eps, bytes([0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0]), 512)
    # Linux asks for 192 bytes of every mode page to find write protection; the boot ROMs send
    # less, then halt the pipe before the CSW.
    st, modes = scsi(c, eps, bytes([0x1A, 0, 0x3F, 0, 192, 0]), 192)
    if st or len(modes) < 4:
        raise RuntimeError("MODE SENSE failed")
    print("drive: %s %s, %u MB, sector 0 ends %02x%02x, %s" % (inq[8:16].decode().strip(), inq[16:32].decode().strip(),
                                                               blocks >> 11, mbr[510], mbr[511],
                                                               "read only" if modes[2] & 0x80 else "writable"))
    if mbr[510:512] != b"\x55\xaa":
        raise RuntimeError("sector 0 has no signature")
    if not uf2:
        return
    data = open(uf2, "rb").read()
    lba = 0x1000
    for off in range(0, len(data), 65536):
        chunk = data[off : off + 65536]
        n = len(chunk) // 512
        cb = bytes([0x2A, 0]) + struct.pack(">I", lba + off // 512) + bytes([0]) + struct.pack(">H", n) + b"\0"
        st, _ = scsi(c, eps, cb, len(chunk), False, chunk)
        if st:
            raise RuntimeError("WRITE(10) failed")
    print("copied %s: %u KB" % (uf2, len(data) // 1024))


def serial(c, cfg):
    iface, eps = endpoints(cfg, 0x0A)
    comm = 0
    c.control(0x21, 0x20, 0, comm, 7, struct.pack("<IBBB", 115200, 0, 0, 8))
    c.control(0x21, 0x22, 3, comm, 0)
    # A read that nothing answers, taken back as Linux does when a program closes the port.
    seq = c.submit(eps["in"], True, 64)
    c.dirs[seq] = True
    text = b""
    deadline = time.time() + 20
    while b"USBSER READY" not in text and time.time() < deadline:
        cmd, status, data, _ = c.reply(seq)
        text += data
        seq = c.submit(eps["in"], True, 64)
        c.dirs[seq] = True
    line = b"hello over usbip"
    c.xfer(eps["out"], False, len(line) + 1, data=line + b"\r")
    while b"USBSER DONE" not in text and time.time() < deadline:
        cmd, status, data, _ = c.reply(seq)
        text += data
        seq = c.submit(eps["in"], True, 64)
        c.dirs[seq] = True
    u = c.unlink(seq)
    cmd, status, _, _ = c.reply(u)
    got = [l for l in text.decode(errors="replace").splitlines() if l.startswith("USBSER GOT")]
    lines = text.count(b"USBSER LINE")
    print("serial: %s, %d lines, unlink %d" % (got[0] if got else "nothing", lines, status))
    want = "USBSER GOT %d %08x" % (len(line), sum(b * 31 ** (len(line) - 1 - i) for i, b in enumerate(line)) % 2**32)
    if not got or got[0].strip() != want or lines != 100 or status not in (0, -104):
        raise RuntimeError("serial check failed: want %s" % want)


def main():
    host, port, what = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    for _ in range(60):
        try:
            if Client(host, port).devlist():
                break
        except (OSError, EOFError):
            pass
        time.sleep(0.5)
    c = Client(host, port)
    vid, pid = c.import_()
    dev, cfg, product = enumerate_device(c)
    print("imported %04x:%04x %s" % (vid, pid, product))
    if what == "drive":
        drive(c, cfg, sys.argv[4] if len(sys.argv) > 4 else None)
    else:
        serial(c, cfg)


if __name__ == "__main__":
    main()
