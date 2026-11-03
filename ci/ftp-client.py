"""Logs in to the emulated ftpserver through a port forward and checks listing, download and upload
in both passive modes.

    python3 ftp-client.py HOST PORT REFERENCE_FILE
"""
import ftplib
import hashlib
import io
import os
import socket
import sys
import time

host, port, ref = sys.argv[1], int(sys.argv[2]), sys.argv[3]
expected = open(ref, "rb").read()
name = os.path.basename(ref)

# The forward refuses connections until the firmware has joined the network and started serving.
deadline = time.time() + 120
while True:
    try:
        ftp = ftplib.FTP()
        ftp.connect(host, port, timeout=30)
        break
    except (OSError, EOFError):
        if time.time() > deadline:
            sys.exit("no FTP server answered on port %d" % port)
        time.sleep(1)
print("banner:", ftp.getwelcome())
ftp.login("picocalc", "sim")

listing = ftp.nlst()
if name not in listing:
    sys.exit("PASV listing lacks %s: %s" % (name, listing))

got = io.BytesIO()
start = time.time()
ftp.retrbinary("RETR " + name, got.write, blocksize=65536)
took = time.time() - start
if got.getvalue() != expected:
    sys.exit("RETR %s: %d bytes that differ from the original" % (name, len(got.getvalue())))
print("RETR %s: %d bytes intact, %.0f KB/s" % (name, len(expected), len(expected) / 1024 / max(took, 0.001)))

up = hashlib.sha256(expected).digest() * 1000
ftp.storbinary("STOR up.bin", io.BytesIO(up))
back = io.BytesIO()
ftp.retrbinary("RETR up.bin", back.write)
if back.getvalue() != up:
    sys.exit("STOR then RETR of up.bin did not round-trip")
print("STOR+RETR up.bin: %d bytes intact" % len(up))

# Clients that speak IPv6 use extended passive mode, whose reply carries only a port.
ftp.af = socket.AF_INET6
if sorted(ftp.nlst()) != sorted(listing + ["up.bin"]):
    sys.exit("EPSV listing differs")
print("EPSV listing: ok")
ftp.quit()
