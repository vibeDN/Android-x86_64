"""python3 con.py '<shell command>' [timeout_s]  -- run a command in the guest console (ttyS0), print only its output"""
import socket, sys, time, re

cmd = sys.argv[1]
to = float(sys.argv[2]) if len(sys.argv) > 2 else 10
s = socket.socket(socket.AF_UNIX)
s.connect("/mnt/storage-adata/gsi16/cf/console.sock")
s.settimeout(0.5)
s.sendall(b"\n")
time.sleep(0.3)
s.sendall(("echo __B__; %s\necho __E__\n" % cmd).encode())
buf = b""
t0 = time.time()
while time.time() - t0 < to:
    try:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    except socket.timeout:
        pass
    t = buf.decode(errors="replace").replace("\r", "")
    if re.search(r"^__E__$", t, re.M):
        break
t = buf.decode(errors="replace").replace("\r", "")
lines = t.split("\n")
try:
    i = max(j for j, l in enumerate(lines) if l.strip() == "__B__")
    k = next(j for j in range(i + 1, len(lines)) if lines[j].strip() == "__E__")
    lines = lines[i + 1:k]
except (ValueError, StopIteration):
    pass
print("\n".join(l for l in lines if not re.match(r"^\[ *\d+\.\d+\]", l)))
