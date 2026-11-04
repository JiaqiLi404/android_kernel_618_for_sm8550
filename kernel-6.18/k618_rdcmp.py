"""k618_rdcmp.py <boot A> <boot B>: compare two test boot images' ramdisks file by file, reading
every concatenated newc archive like the kernel does (a later file of the same name wins)."""
import hashlib, os, subprocess, sys, tempfile

def files(img):
    d = tempfile.mkdtemp()
    subprocess.run([sys.executable, "-I", os.path.join(os.environ.get("MKB", os.path.expanduser("~/xiaomi13-k618-build/home/mkbootimg")), "unpack_bootimg.py"),
                    "--boot_img", img, "--out", d], check=True, capture_output=True)
    data = subprocess.run(["lz4", "-dcq", os.path.join(d, "ramdisk")], check=True, capture_output=True).stdout
    out, i = {}, 0
    while i + 110 <= len(data):
        if data[i:i + 6] not in (b"070701", b"070702"):
            i += 1          # padding between archives
            continue
        h = data[i:i + 110]
        size, nlen = int(h[54:62], 16), int(h[94:102], 16)
        name = data[i + 110:i + 110 + nlen - 1].decode()
        j = (i + 110 + nlen + 3) & ~3
        body = data[j:j + size]
        i = (j + size + 3) & ~3
        if name != "TRAILER!!!":
            out[name] = body
    return out

a, b = files(sys.argv[1]), files(sys.argv[2])
diff = sorted(n for n in set(a) | set(b) if a.get(n) != b.get(n))
print(f"{len(a)} / {len(b)} files; differ: {len(diff)}")
for n in diff:
    print("  ", n, len(a.get(n, b"")), len(b.get(n, b"")))
for n in ("lib/modules/modules.load", "lib/modules/modules.options"):
    print(n, "same" if a.get(n) == b.get(n) else "DIFFERENT", hashlib.sha256(b.get(n, b"")).hexdigest()[:12])
print("first lines of B's modules.load:", b"/".join(b["lib/modules/modules.load"].split(b"\n")[:3]).decode())
