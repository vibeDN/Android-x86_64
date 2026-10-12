"""QMP input helper: python3 inject.py swipe x1 y1 x2 y2 | tap x y   (coords in 0..1279 / 0..799 screen px)"""
import socket, json, sys, time

W, H = 1280, 800


class Q:
    def __init__(self):
        s = socket.socket(socket.AF_UNIX)
        s.connect("/mnt/storage-adata/gsi16/cf/qmp.sock")
        self.f = s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, args=None):
        self.f.write(json.dumps({"execute": name, **({"arguments": args} if args else {})}) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                return r

    def ev(self, events):
        return self.cmd("input-send-event", {"events": events})

    def move(self, x, y):
        ax = int(x * 32767 / (W - 1))
        ay = int(y * 32767 / (H - 1))
        self.ev([{"type": "abs", "data": {"axis": "x", "value": ax}}, {"type": "abs", "data": {"axis": "y", "value": ay}}])

    def btn(self, down):
        self.ev([{"type": "btn", "data": {"button": "left", "down": down}}])


q = Q()
a = sys.argv[1:]
if a[0] == "tap":
    x, y = int(a[1]), int(a[2])
    q.move(x, y); time.sleep(0.1); q.btn(True); time.sleep(0.1); q.btn(False)
elif a[0] == "swipe":
    x1, y1, x2, y2 = map(int, a[1:5])
    q.move(x1, y1); time.sleep(0.1); q.btn(True)
    n = 40
    for i in range(1, n + 1):
        q.move(x1 + (x2 - x1) * i // n, y1 + (y2 - y1) * i // n); time.sleep(0.05)
    q.btn(False)
print("ok")
