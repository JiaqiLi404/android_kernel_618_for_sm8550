"""A/B OTA payload.bin parser and extractor.

Reads full OTA packages (`*-ota_full-*.zip`, i.e. Xiaomi "recovery ROMs"),
bare payload.bin files, or HTTP(S) URLs to either. Only the partitions that
are asked for are read, so over HTTP the rest of the multi-GB zip is never
downloaded.

    PYTHONPATH=scripts python3 -m inputlib.payload info  <zip|payload.bin|url>
    PYTHONPATH=scripts python3 -m inputlib.payload extract <zip|payload.bin|url> outdir [-p system ...]
"""

from __future__ import annotations

import argparse
import bz2
import hashlib
import lzma
import os
import struct
import sys
import threading
import zipfile
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field

from .log import PortError, log
from .remote import HttpFile, LocalFile, is_url

PAYLOAD_MAGIC = b"CrAU"

OP_REPLACE = 0
OP_REPLACE_BZ = 1
OP_ZERO = 6
OP_DISCARD = 7
OP_REPLACE_XZ = 8
OP_REPLACE_ZSTD = 14

OP_NAMES = {
    0: "REPLACE",
    1: "REPLACE_BZ",
    2: "MOVE",
    3: "BSDIFF",
    4: "SOURCE_COPY",
    5: "SOURCE_BSDIFF",
    6: "ZERO",
    7: "DISCARD",
    8: "REPLACE_XZ",
    9: "PUFFDIFF",
    10: "BROTLI_BSDIFF",
    11: "ZUCCHINI",
    12: "LZ4DIFF_BSDIFF",
    13: "LZ4DIFF_PUFFDIFF",
    14: "REPLACE_ZSTD",
}

CHUNK_BYTES = 2 << 20


# --- minimal protobuf decoding -------------------------------------------------


def _varint(buf: bytes, pos: int) -> tuple[int, int]:
    result = shift = 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not b & 0x80:
            return result, pos
        shift += 7


def decode(buf: bytes) -> dict[int, list]:
    """Decode one protobuf message into {field_number: [values]}."""
    out: dict[int, list] = {}
    pos, n = 0, len(buf)
    while pos < n:
        key, pos = _varint(buf, pos)
        num, wt = key >> 3, key & 7
        if wt == 0:
            val, pos = _varint(buf, pos)
        elif wt == 1:
            val = struct.unpack_from("<Q", buf, pos)[0]
            pos += 8
        elif wt == 2:
            ln, pos = _varint(buf, pos)
            val = buf[pos : pos + ln]
            pos += ln
        elif wt == 5:
            val = struct.unpack_from("<I", buf, pos)[0]
            pos += 4
        else:
            raise ValueError(f"unsupported protobuf wire type {wt}")
        out.setdefault(num, []).append(val)
    return out


def _one(msg: dict, num: int, default=None):
    v = msg.get(num)
    return v[-1] if v else default


# --- manifest model ------------------------------------------------------------


@dataclass
class Operation:
    type: int
    data_offset: int
    data_length: int
    dst_extents: list[tuple[int, int]]
    data_sha256: bytes | None


@dataclass
class PartitionUpdate:
    name: str
    size: int
    sha256: bytes | None
    version: str
    operations: list[Operation]


@dataclass
class DynamicGroup:
    name: str
    size: int
    partitions: list[str]


@dataclass
class DynamicMetadata:
    groups: list[DynamicGroup] = field(default_factory=list)
    snapshot_enabled: bool = False
    vabc_enabled: bool = False
    vabc_compression: str = ""
    cow_version: int = 0

    def logical_partitions(self) -> list[str]:
        return [p for g in self.groups for p in g.partitions]


def _parse_partition(raw: bytes) -> PartitionUpdate:
    m = decode(raw)
    info = decode(_one(m, 7, b""))
    ops = []
    for o in m.get(8, []):
        om = decode(o)
        exts = [(_one(e, 1, 0), _one(e, 2, 0)) for e in map(decode, om.get(6, []))]
        ops.append(Operation(_one(om, 1, 0), _one(om, 2, 0), _one(om, 3, 0), exts, _one(om, 8)))
    return PartitionUpdate(
        name=_one(m, 1, b"").decode(),
        size=_one(info, 1, 0),
        sha256=_one(info, 2),
        version=_one(m, 17, b"").decode(errors="replace"),
        operations=ops,
    )


def _parse_dynamic(raw: bytes) -> DynamicMetadata:
    m = decode(raw)
    groups = []
    for g in m.get(1, []):
        gm = decode(g)
        groups.append(DynamicGroup(_one(gm, 1, b"").decode(), _one(gm, 2, 0), [p.decode() for p in gm.get(3, [])]))
    return DynamicMetadata(
        groups=groups,
        snapshot_enabled=bool(_one(m, 2, 0)),
        vabc_enabled=bool(_one(m, 3, 0)),
        vabc_compression=_one(m, 4, b"").decode(),
        cow_version=_one(m, 5, 0),
    )


# --- payload -------------------------------------------------------------------


def _zstd_decompress(data: bytes) -> bytes:
    try:
        from compression import zstd  # Python 3.14+

        return zstd.decompress(data)
    except ImportError:
        pass
    try:
        import zstandard
    except ImportError as e:
        raise PortError("payload uses zstd; install it with: pip install zstandard") from e
    return zstandard.ZstdDecompressor().decompressobj().decompress(data)


def _decompress(op: Operation, data: bytes) -> bytes:
    if op.type == OP_REPLACE:
        return data
    if op.type == OP_REPLACE_XZ:
        return lzma.decompress(data)
    if op.type == OP_REPLACE_BZ:
        return bz2.decompress(data)
    if op.type == OP_REPLACE_ZSTD:
        return _zstd_decompress(data)
    raise PortError(
        f"operation {OP_NAMES.get(op.type, op.type)} found: this is an incremental OTA. Use a full OTA / recovery ROM (file name contains 'ota_full')."
    )


class Payload:
    def __init__(self, src, offset: int = 0, name: str = "payload.bin"):
        self.src = src
        self.name = name
        head = src.pread(offset, 24)
        if head[:4] != PAYLOAD_MAGIC:
            raise PortError(f"{name}: not a payload.bin (bad magic)")
        version, manifest_size = struct.unpack(">QQ", head[4:20])
        if version == 1:
            sig_size, hdr = 0, 20
        elif version == 2:
            sig_size, hdr = struct.unpack(">I", head[20:24])[0], 24
        else:
            raise PortError(f"{name}: unsupported payload version {version}")
        manifest = decode(src.pread(offset + hdr, manifest_size))
        self.data_offset = offset + hdr + manifest_size + sig_size
        self.block_size = _one(manifest, 3, 4096)
        self.minor_version = _one(manifest, 12, 0)
        self.security_patch = _one(manifest, 18, b"").decode()
        self.partitions: dict[str, PartitionUpdate] = {}
        for raw in manifest.get(13, []):
            p = _parse_partition(raw)
            self.partitions[p.name] = p
        dyn = _one(manifest, 15)
        self.dynamic = _parse_dynamic(dyn) if dyn is not None else None

    def extract(self, name: str, out_path: str, threads: int = 8, verify: bool = True) -> str:
        part = self.partitions.get(name)
        if part is None:
            raise PortError(f"{self.name}: no partition named {name}")
        bs = self.block_size
        tmp = out_path + ".part"
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
        try:
            os.ftruncate(fd, part.size)
            chunks = self._plan(part)
            total = sum(c[1] - c[0] for c in chunks) or 1
            done = [0]
            next_report = [0.1]
            lock = threading.Lock()

            def work(chunk):
                start, end, ops = chunk
                data = self.src.pread(self.data_offset + start, end - start) if end > start else b""
                if len(data) != end - start:
                    raise PortError(f"{name}: short read from {self.name}")
                for op in ops:
                    blob = data[op.data_offset - start : op.data_offset - start + op.data_length]
                    if verify and op.data_sha256 and hashlib.sha256(blob).digest() != op.data_sha256:
                        raise PortError(f"{name}: data hash mismatch at offset {op.data_offset} (corrupt download?)")
                    out = _decompress(op, blob)
                    pos = 0
                    for blk, num in op.dst_extents:
                        n = num * bs
                        os.pwrite(fd, out[pos : pos + n], blk * bs)
                        pos += n
                with lock:
                    done[0] += end - start
                    frac = done[0] / total
                    if part.size >= (64 << 20) and frac >= next_report[0]:
                        log(f"  {name}: {int(frac * 100)}%")
                        next_report[0] = int(frac * 10) / 10 + 0.1

            with ThreadPoolExecutor(max_workers=threads) as ex:
                for _ in ex.map(work, chunks):
                    pass
        finally:
            os.close(fd)
        if verify and part.sha256:
            h = hashlib.sha256()
            with open(tmp, "rb") as f:
                for buf in iter(lambda: f.read(16 << 20), b""):
                    h.update(buf)
            if h.digest() != part.sha256:
                raise PortError(f"{name}: image hash mismatch after extraction")
        os.replace(tmp, out_path)
        return out_path

    def _plan(self, part: PartitionUpdate):
        for op in part.operations:
            if op.type in (OP_ZERO, OP_DISCARD):
                continue
            if op.type not in (OP_REPLACE, OP_REPLACE_BZ, OP_REPLACE_XZ, OP_REPLACE_ZSTD):
                _decompress(op, b"")  # raises a helpful error
        ops = sorted((o for o in part.operations if o.data_length), key=lambda o: o.data_offset)
        chunks = []
        cur: list[Operation] = []
        cstart = cend = 0
        for op in ops:
            if cur and (op.data_offset + op.data_length - cstart > CHUNK_BYTES or op.data_offset - cend > (1 << 20)):
                chunks.append((cstart, cend, cur))
                cur = []
            if not cur:
                cstart = cend = op.data_offset
            cur.append(op)
            cend = max(cend, op.data_offset + op.data_length)
        if cur:
            chunks.append((cstart, cend, cur))
        return chunks


@dataclass
class OtaPackage:
    payload: Payload
    metadata: dict[str, str]
    source: str


def _parse_kv(text: str) -> dict[str, str]:
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def open_ota(path: str) -> OtaPackage:
    """Open an OTA zip / payload.bin, local or remote."""
    try:
        src = HttpFile(path) if is_url(path) else LocalFile(path)
    except OSError as e:
        raise PortError(f"cannot open {path}: {e}") from e
    magic = src.pread(0, 4)
    label = path.rsplit("/", 1)[-1].split("?", 1)[0]
    if magic == PAYLOAD_MAGIC:
        return OtaPackage(Payload(src, 0, label), {}, path)
    if magic[:2] != b"PK":
        raise PortError(f"{label}: neither an OTA zip nor payload.bin")
    fobj = src if isinstance(src, HttpFile) else open(path, "rb")
    try:
        zf = zipfile.ZipFile(fobj)
        try:
            info = zf.getinfo("payload.bin")
        except KeyError as e:
            raise PortError(
                f"{label}: no payload.bin inside. Use a full OTA (recovery ROM) zip; for fastboot ROMs (.tgz) extract them and pass the images/ directory."
            ) from e
        if info.compress_type != zipfile.ZIP_STORED:
            raise PortError(f"{label}: payload.bin is compressed inside the zip; unzip it and pass payload.bin")
        meta: dict[str, str] = {}
        for mname in ("META-INF/com/android/metadata", "payload_properties.txt"):
            try:
                meta.update(_parse_kv(zf.read(mname).decode(errors="replace")))
            except KeyError:
                pass
    finally:
        if fobj is not src:
            fobj.close()
    lh = src.pread(info.header_offset, 30)
    if lh[:4] != b"PK\x03\x04":
        raise PortError(f"{label}: corrupt zip local header")
    nlen, elen = struct.unpack("<HH", lh[26:30])
    offset = info.header_offset + 30 + nlen + elen
    return OtaPackage(Payload(src, offset, label), meta, path)


def _cmd_info(args) -> int:
    ota = open_ota(args.source)
    p = ota.payload
    for k in ("pre-device", "post-build", "post-build-incremental", "post-sdk-level", "post-security-patch-level", "ota-type"):
        if k in ota.metadata:
            print(f"{k}: {ota.metadata[k]}")
    print(f"block size: {p.block_size}, minor version: {p.minor_version}, security patch: {p.security_patch}")
    if p.dynamic:
        d = p.dynamic
        print(f"dynamic partitions: snapshot={d.snapshot_enabled} vabc={d.vabc_enabled} compression={d.vabc_compression} cow={d.cow_version}")
        for g in d.groups:
            print(f"  group {g.name} size={g.size}: {' '.join(g.partitions)}")
    for name, part in p.partitions.items():
        print(f"  {name:<20} {part.size:>12}  ops={len(part.operations)}")
    return 0


def _cmd_extract(args) -> int:
    ota = open_ota(args.source)
    os.makedirs(args.outdir, exist_ok=True)
    names = args.partition or list(ota.payload.partitions)
    for n in names:
        log(f"extracting {n}")
        ota.payload.extract(n, os.path.join(args.outdir, n + ".img"), threads=args.threads, verify=not args.no_verify)
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="PYTHONPATH=scripts python3 -m inputlib.payload")
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("info")
    i.add_argument("source")
    i.set_defaults(func=_cmd_info)
    e = sub.add_parser("extract")
    e.add_argument("source")
    e.add_argument("outdir")
    e.add_argument("-p", "--partition", action="append")
    e.add_argument("-j", "--threads", type=int, default=8)
    e.add_argument("--no-verify", action="store_true")
    e.set_defaults(func=_cmd_extract)
    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except PortError as err:
        print(f"error: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
