"""python3 mouse.py click X Y [scale]  -- emulate absolute pointing with a relative virtio mouse via QMP"""
import socket, json, sys, time

s = socket.socket(socket.AF_UNIX); s.connect("/mnt/storage-adata/gsi16/cf/qmp.sock")
f = s.makefile("rw"); f.readline()
def c(n, a=None):
    f.write(json.dumps({"execute": n, **({"arguments": a} if a else {})}) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r: return r
c("qmp_capabilities")
def rel(dx, dy):
    ev = []
    if dx: ev.append({"type": "rel", "data": {"axis": "x", "value": dx}})
    if dy: ev.append({"type": "rel", "data": {"axis": "y", "value": dy}})
    if ev: c("input-send-event", {"events": ev})
def btn(down): c("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": down}}]})
def goto(x, y, k):
    for _ in range(70): rel(-60, -60); time.sleep(0.004)      # slam into the top-left corner
    tx, ty = int(x / k), int(y / k)
    while tx or ty:
        sx = max(-12, min(12, tx)); sy = max(-12, min(12, ty))
        rel(sx, sy); tx -= sx; ty -= sy; time.sleep(0.01)
a = sys.argv[1:]
k = float(a[3]) if len(a) > 3 else 1.0
if a[0] == "click":
    goto(int(a[1]), int(a[2]), k); time.sleep(0.3); btn(True); time.sleep(0.1); btn(False)
print("ok")
