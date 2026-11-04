"""Flattened device tree (DTB) reader/writer: enough to edit nodes and properties.

The strings block is kept as read and new property names are appended, so a
tree written back without edits is byte-identical to the input (for blobs laid
out as dtc writes them), and an edit changes only what it touches.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

MAGIC = 0xD00DFEED
BEGIN_NODE, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9


@dataclass
class Node:
    name: str
    props: dict[str, bytes] = field(default_factory=dict)  # insertion order = blob order
    children: list[Node] = field(default_factory=list)

    def child(self, name: str) -> Node | None:
        return next((c for c in self.children if c.name == name), None)

    def walk(self, path: str = ""):
        """Yield (path, node) depth-first, this node included."""
        here = path + "/" + self.name if path or self.name else "/"
        yield here.replace("//", "/"), self
        for c in self.children:
            yield from c.walk("" if here == "/" else here)

    def u32s(self, prop: str) -> list[int] | None:
        v = self.props.get(prop)
        if v is None:
            return None
        return list(struct.unpack(f">{len(v) // 4}I", v[: len(v) // 4 * 4]))

    def set_u32s(self, prop: str, values: list[int]) -> None:
        self.props[prop] = struct.pack(f">{len(values)}I", *values)

    def string(self, prop: str) -> str | None:
        v = self.props.get(prop)
        return None if v is None else v.split(b"\0")[0].decode("utf-8", "replace")


class Fdt:
    def __init__(self, root: Node, rsvmap: list[tuple[int, int]], boot_cpuid: int, strings: bytes):
        self.root = root
        self.rsvmap = rsvmap
        self.boot_cpuid = boot_cpuid
        self._strings = bytearray(strings)
        self._offsets: dict[str, int] = {}

    @classmethod
    def parse(cls, data: bytes) -> Fdt:
        if len(data) < 40:
            raise ValueError("not a device tree blob (too short)")
        magic, total, off_struct, off_strings, off_rsv, version, _last, cpuid, size_strings, size_struct = struct.unpack_from(">10I", data, 0)
        if magic != MAGIC:
            raise ValueError("not a device tree blob (bad magic)")
        if version < 17:
            raise ValueError(f"device tree version {version} is not supported")
        if total > len(data):
            raise ValueError("truncated device tree blob")
        rsv, pos = [], off_rsv
        while True:
            addr, size = struct.unpack_from(">QQ", data, pos)
            pos += 16
            if not addr and not size:
                break
            rsv.append((addr, size))
        strings = data[off_strings : off_strings + size_strings]
        fdt = cls(Node(""), rsv, cpuid, strings)
        stack: list[Node] = []
        pos, end = off_struct, off_struct + size_struct
        while pos < end:
            (tok,) = struct.unpack_from(">I", data, pos)
            pos += 4
            if tok == BEGIN_NODE:
                nul = data.index(b"\0", pos)
                name = data[pos:nul].decode("utf-8", "replace")
                pos = (nul + 4) & ~3
                if stack:
                    node = Node(name)
                    stack[-1].children.append(node)
                else:
                    node = fdt.root
                    node.name = name
                stack.append(node)
            elif tok == END_NODE:
                stack.pop()
            elif tok == PROP:
                length, nameoff = struct.unpack_from(">II", data, pos)
                pos += 8
                nul = strings.index(b"\0", nameoff)
                pname = strings[nameoff:nul].decode("utf-8", "replace")
                fdt._offsets.setdefault(pname, nameoff)
                stack[-1].props[pname] = bytes(data[pos : pos + length])
                pos = (pos + length + 3) & ~3
            elif tok == NOP:
                continue
            elif tok == END:
                break
            else:
                raise ValueError(f"bad device tree token {tok:#x} at {pos - 4}")
        if stack:
            raise ValueError("unterminated node in device tree blob")
        return fdt

    def _nameoff(self, name: str) -> int:
        off = self._offsets.get(name)
        if off is None:
            off = len(self._strings)
            self._strings += name.encode() + b"\0"
            self._offsets[name] = off
        return off

    def to_bytes(self) -> bytes:
        out = bytearray()

        def emit(node: Node) -> None:
            name = node.name.encode() + b"\0"
            out.extend(struct.pack(">I", BEGIN_NODE) + name + b"\0" * (-len(name) % 4))
            for pname, value in node.props.items():
                out.extend(struct.pack(">III", PROP, len(value), self._nameoff(pname)) + value + b"\0" * (-len(value) % 4))
            for c in node.children:
                emit(c)
            out.extend(struct.pack(">I", END_NODE))

        emit(self.root)
        out.extend(struct.pack(">I", END))
        rsv = b"".join(struct.pack(">QQ", a, s) for a, s in self.rsvmap) + b"\0" * 16
        off_rsv = 40
        off_struct = off_rsv + len(rsv)
        off_strings = off_struct + len(out)
        total = off_strings + len(self._strings)
        header = struct.pack(">10I", MAGIC, total, off_struct, off_strings, off_rsv, 17, 16, self.boot_cpuid, len(self._strings), len(out))
        return header + rsv + bytes(out) + bytes(self._strings)


def split(blob: bytes) -> list[bytes]:
    """The device trees concatenated in a DTB section (vendor_boot, dtb.img)."""
    out, pos = [], 0
    while pos + 8 <= len(blob):
        magic, size = struct.unpack_from(">II", blob, pos)
        if magic != MAGIC or size < 40:
            break
        out.append(blob[pos : pos + size])
        pos += size
    return out
