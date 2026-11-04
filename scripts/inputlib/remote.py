"""Seekable read-only file objects for local paths and HTTP(S) URLs.

Remote files are read with HTTP Range requests, so only the bytes that are
actually needed get downloaded (OTA zips store payload.bin uncompressed).
"""

from __future__ import annotations

import io
import os
import re
import time
import urllib.error
import urllib.request

from .log import log

USER_AGENT = "Mozilla/5.0 (X11; Linux x86_64) xiaomi13-k618-inputs/1.0"
MAX_RANGE_BYTES = 256 << 10


class RangeUnsupported(Exception):
    pass


# Xiaomi serves the same OTA paths from several hosts.
XIAOMI_MIRRORS = [
    "bigota.d.miui.com",
    "cdnorg.d.miui.com",
    "bkt-sgp-miui-ota-update-alisgp.oss-ap-southeast-1.aliyuncs.com",
]


def mirrors(url: str) -> list[str]:
    m = re.match(r"(https?://)([^/]+)(/.*)", url)
    if not m or m.group(2) not in XIAOMI_MIRRORS:
        return [url]
    return [url] + [f"https://{h}{m.group(3)}" for h in XIAOMI_MIRRORS if h != m.group(2)]


def is_url(path: str) -> bool:
    return path.startswith(("http://", "https://"))


class HttpFile(io.RawIOBase):
    def __init__(self, url: str, retries: int = 3, timeout: int = 20):
        super().__init__()
        self.url = url
        self.retries = retries
        self.timeout = timeout
        self._pos = 0
        self._cache_off = -1
        self._cache = b""
        errors = []
        for candidate in mirrors(url):
            self.url = candidate
            try:
                self.size = self._probe_size()
                break
            except (OSError, RangeUnsupported) as e:
                errors.append(f"{candidate}: {e}")
                log(f"  cannot use {candidate}: {e}")
        else:
            raise IOError("; ".join(errors))
        if self.url != url:
            log(f"  using mirror {self.url}")

    def _request(self, start: int, end: int) -> bytes:
        """Fetch bytes [start, end] (inclusive) with retries."""
        want = end - start + 1
        if want > MAX_RANGE_BYTES:
            return b"".join(self._request(pos, min(end, pos + MAX_RANGE_BYTES - 1))
                            for pos in range(start, end + 1, MAX_RANGE_BYTES))
        delay = 2
        last: Exception | None = None
        for attempt in range(self.retries):
            req = urllib.request.Request(self.url, headers={"Range": f"bytes={start}-{end}", "User-Agent": USER_AGENT})
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                    if resp.status != 206:
                        raise RangeUnsupported(f"server ignored Range request (HTTP {resp.status}); download the file and pass a local path")
                    match = re.fullmatch(r"bytes (\d+)-(\d+)/(\d+)", resp.headers.get("Content-Range", ""))
                    if not match or (int(match[1]), int(match[2])) != (start, end):
                        raise RangeUnsupported("server returned a different byte range")
                    if resp.headers.get("Content-Length") not in (None, str(want)):
                        raise RangeUnsupported("server returned a different response length")
                    data = resp.read(want + 1)
                if len(data) != want:
                    raise IOError(f"short read: got {len(data)} of {want} bytes")
                return data
            except (urllib.error.URLError, IOError, TimeoutError, ConnectionError) as e:
                if isinstance(e, urllib.error.HTTPError) and e.code in (403, 404, 410):
                    raise IOError(f"HTTP {e.code} for {self.url}") from e
                last = e
                if attempt + 1 < self.retries:
                    log(f"  range {start}-{end} failed ({e}); retry in {delay}s")
                    time.sleep(delay)
                    delay = min(delay * 2, 60)
        raise IOError(f"giving up on {self.url}: {last}")

    def _probe_size(self) -> int:
        req = urllib.request.Request(self.url, headers={"Range": "bytes=0-0", "User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                cr = resp.headers.get("Content-Range", "")
                m = re.fullmatch(r"bytes 0-0/(\d+)", cr)
                if resp.status == 206 and m and resp.headers.get("Content-Length") in (None, "1"):
                    return int(m.group(1))
                raise RangeUnsupported(f"{self.url}: server does not support Range requests (HTTP {resp.status})")
        except urllib.error.HTTPError as e:
            raise IOError(f"HTTP {e.code} for {self.url}") from e

    def pread(self, offset: int, size: int) -> bytes:
        if size <= 0 or offset >= self.size:
            return b""
        end = min(offset + size, self.size) - 1
        return self._request(offset, end)

    # io.RawIOBase interface (used by zipfile for the central directory).
    def readable(self) -> bool:
        return True

    def seekable(self) -> bool:
        return True

    def tell(self) -> int:
        return self._pos

    def seek(self, offset: int, whence: int = 0) -> int:
        if whence == 0:
            self._pos = offset
        elif whence == 1:
            self._pos += offset
        else:
            self._pos = self.size + offset
        return self._pos

    def read(self, size: int = -1) -> bytes:
        if size is None or size < 0:
            size = self.size - self._pos
        # zipfile does many small reads near the end of the archive; serve
        # them from a 1 MiB window to avoid a request per read.
        if 0 <= self._cache_off <= self._pos and self._pos + size <= self._cache_off + len(self._cache):
            rel = self._pos - self._cache_off
            data = self._cache[rel : rel + size]
        elif size <= 64 << 10:
            win = 1 << 20
            start = max(0, min(self._pos, self.size - win))
            self._cache = self.pread(start, win)
            self._cache_off = start
            rel = self._pos - start
            data = self._cache[rel : rel + size]
        else:
            data = self.pread(self._pos, size)
        self._pos += len(data)
        return data

    def readinto(self, b) -> int:
        data = self.read(len(b))
        b[: len(data)] = data
        return len(data)


class LocalFile:
    def __init__(self, path: str):
        self.path = path
        self._fd = os.open(path, os.O_RDONLY)
        self.size = os.fstat(self._fd).st_size

    def pread(self, offset: int, size: int) -> bytes:
        return os.pread(self._fd, size, offset)

    def close(self) -> None:
        os.close(self._fd)


def open_source(path: str):
    return HttpFile(path) if is_url(path) else LocalFile(path)


def download(url: str, dst: str, retries: int = 3, timeout: int = 20) -> str:
    """Plain GET of a whole file (redirects followed), retried; returns dst."""
    last = None
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "curl/8.5.0", "Accept": "*/*"})
            with urllib.request.urlopen(req, timeout=timeout) as resp, open(dst + ".part", "wb") as f:
                while True:
                    buf = resp.read(1 << 20)
                    if not buf:
                        break
                    f.write(buf)
            os.replace(dst + ".part", dst)
            return dst
        except (OSError, urllib.error.URLError) as e:
            last = e
            log(f"  download failed ({e}); retry {attempt + 1}/{retries}")
            time.sleep(min(30, 2**attempt))
    raise IOError(f"cannot download {url}: {last}")
