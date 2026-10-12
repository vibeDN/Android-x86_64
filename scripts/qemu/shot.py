import socket, json, sys, time
out = sys.argv[1] if len(sys.argv) > 1 else "/mnt/storage-adata/gsi16/cf/screen.ppm"
s = socket.socket(socket.AF_UNIX); s.connect("/mnt/storage-adata/gsi16/cf/qmp.sock")
f = s.makefile("rw")
f.readline(); f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()
f.write(json.dumps({"execute": "screendump", "arguments": {"filename": out, "device": "gpu0", "format": "ppm"}}) + "\n"); f.flush()
print(f.readline().strip())
