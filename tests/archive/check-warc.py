#!/usr/bin/env python3
"""Checks a gzipped WARC file record by record: that each record is a gzip
member of its own, that its block digest is right, and that a response's
payload digest is right as WARC 1.1 defines the payload -- the entity body,
with any chunked transfer coding removed, which is also how warcprox digests
it. (warcio check digests the body with its chunked coding still on, so it
reports chunked responses from vimb and warcprox alike as failures.)

Standard library only.   check-warc.py FILE.warc.gz [--quiet]
"""

import base64
import hashlib
import sys
import zlib


def label(data):
    return "sha1:" + base64.b32encode(hashlib.sha1(data).digest()).decode().rstrip("=")


def dechunk(body):
    out, i = bytearray(), 0
    while True:
        j = body.index(b"\r\n", i)
        size = int(body[i:j].split(b";")[0].strip(), 16)
        if size == 0:
            return bytes(out)
        out += body[j + 2:j + 2 + size]
        i = j + 2 + size + 2


def members(data):
    """Each gzip member's offset and content."""
    offset = 0
    while offset < len(data):
        d = zlib.decompressobj(31)
        content = d.decompress(data[offset:])
        if not d.eof:
            raise ValueError("member at %d is cut short" % offset)
        yield offset, content
        offset = len(data) - len(d.unused_data)


def main():
    path = sys.argv[1]
    quiet = "--quiet" in sys.argv
    with open(path, "rb") as f:
        data = f.read()
    counts, failures = {}, []
    for offset, record in members(data):
        head, _, rest = record.partition(b"\r\n\r\n")
        fields = {}
        lines = head.decode("utf-8", "replace").split("\r\n")
        if lines[0] != "WARC/1.1":
            failures.append("%d: not WARC/1.1" % offset)
            continue
        for line in lines[1:]:
            name, _, value = line.partition(": ")
            fields[name.lower()] = value
        length = int(fields["content-length"])
        block = rest[:length]
        if rest[length:] != b"\r\n\r\n" or len(block) != length:
            failures.append("%d: block is not Content-Length long" % offset)
            continue
        kind = fields.get("warc-type")
        counts[kind] = counts.get(kind, 0) + 1
        if fields.get("warc-block-digest") != label(block):
            failures.append("%d %s: block digest" % (offset, kind))
        if kind == "response" and "warc-payload-digest" in fields:
            http_head, _, body = block.partition(b"\r\n\r\n")
            chunked = any(l.lower().startswith(b"transfer-encoding:") and b"chunked" in l.lower()
                          for l in http_head.split(b"\r\n"))
            truncated = "warc-truncated" in fields
            if not truncated:
                payload = dechunk(body) if chunked else body
                if fields["warc-payload-digest"] != label(payload):
                    failures.append("%d %s: payload digest (%s)" % (
                        offset, fields.get("warc-target-uri"), "chunked" if chunked else "plain"))
    for failure in failures:
        print("FAIL", failure)
    if not quiet or failures:
        print("%s: %s; %d failures" % (path, ", ".join(
            "%d %s" % (n, k) for k, n in sorted(counts.items())), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
