"""k618_rdread.py <rd.bin> [out.txt]: read back what a test boot wrote onto rawdump.

Two channels live there. The whole copies (K618LOG) sit in slots that alternate, so the newest can
be in either; within one run it is the one written latest after boot (highest ms). seq restarts at 1
on every boot, so a run that died before its second write leaves the previous run's copy in the
other slot -- tell runs apart by what they say (release, probe line, timing), never by the highest
seq alone. Copies of the 6.12 line carry a K612LOG header and are not counted.

The ring (K618RNG, from log_ring_* in k618_probe) is the log written forward, one 4 KB block at a
time, each block holding the next few printk records; the blocks are put back in index order here.
It reaches further than the last whole copy, because the panic and die notifiers drain it after
everything else has stopped -- that tail is what says how the boot died. With out.txt given, the
copy goes to out.txt and the ring, when it has anything, to out.txt with -ring before its suffix.
"""
import re
import sys

BLK = 4096
d = open(sys.argv[1], "rb").read()
out = sys.argv[2] if len(sys.argv) > 2 else None

hdr = re.compile(rb"K618LOG seq (\d+) (\S+) at (\d+) ms")
hits = [(m.start(), int(m.group(1)), m.group(2).decode(), int(m.group(3))) for m in hdr.finditer(d)]
print("copies:", len(hits))
for off, seq, rel, ms in hits:
    end = d.find(b"K618END", off)
    n = (end - off) if end > off else -1
    print(f"  at 0x{off:07x}  seq {seq:5d}  {rel}  {ms} ms  {n} bytes  {'closed' if end > off else 'OPEN'}")
if out and hits:
    off, seq, rel, ms = max(hits, key=lambda h: h[3])
    end = d.find(b"K618END", off)
    body = d[off:end if end > off else off + 4 * 1024 * 1024]
    open(out, "wb").write(body)
    print(f"wrote {out}: {len(body)} bytes (seq {seq}, {ms} ms, {rel})")

# the ring: one header at the start of each block, so look only at block boundaries
rng = re.compile(rb"K618RNG (\d+) at (\d+) ms, (\d+) bytes(?:, run ([0-9a-f]{8}))?")
blocks = {}
newest, stale = None, 0
for off in range(0, len(d) - BLK + 1, BLK):
    m = rng.match(d, off)
    if not m:
        continue
    idx, ms, n, run = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)
    # every run writes its index 0 at the ring's start first, so the first block names the newest
    # run; a boot that did not fill the ring leaves the previous boot's blocks after its own
    if newest is None:
        newest = run
        print(f"ring: run {run.decode() if run else '(unnamed)'}")
    if run != newest:
        stale += 1
        continue
    text = d[off + 96:off + 96 + n]
    # a wrapped ring holds an older block at the same place: the newest index wins
    if idx not in blocks or blocks[idx][0] < ms:
        blocks[idx] = (ms, text)
if blocks:
    first, last = min(blocks), max(blocks)
    missing = [i for i in range(first, last + 1) if i not in blocks]
    print(f"ring: {len(blocks)} blocks, index {first}..{last}, "
          f"{min(b[0] for b in blocks.values())}..{max(b[0] for b in blocks.values())} ms"
          + (f", {len(missing)} missing: {missing[:8]}" if missing else "")
          + (f", {stale} blocks of earlier runs left out" if stale else ""))
    if out:
        ro = re.sub(r"(\.[^.]*)?$", lambda m: "-ring" + (m.group(1) or ""), out, count=1)
        with open(ro, "wb") as f:
            prev = None
            for i in range(first, last + 1):
                if i in blocks:


                    if prev is not None and i != prev + 1:
                        f.write(b"\n[k618_rdread: blocks %d..%d were overwritten]\n\n" % (prev + 1, i - 1))
                    f.write(blocks[i][1])
                    prev = i
        print(f"wrote {ro}: {sum(len(b[1]) for b in blocks.values())} bytes of log")
else:
    print("ring: nothing")
