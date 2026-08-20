"""Minimal PNG writing and PAX pixel-format decoding, stdlib only.

The references and diffs are PNGs so they can be looked at in any image viewer
and reviewed in a browser on a pull request. Pulling in Pillow for that would put
a compiled dependency between a fresh checkout and being able to verify
correctness, so the ~40 lines of PNG writing live here instead.
"""

import struct
import zlib


def write_png(path, width, height, rgb):
    """Write an 8-bit RGB PNG. `rgb` is width*height*3 bytes, row-major."""
    expect = width * height * 3
    if len(rgb) != expect:
        raise ValueError(f"expected {expect} bytes, got {len(rgb)}")

    # Filter type 0 (None) in front of every scanline. The images are text on a
    # flat background, so zlib does the compressing that matters and a smarter
    # filter would buy little for the complexity.
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        handle.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        handle.write(chunk(b"IEND", b""))


def read_png(path):
    """Read back a PNG this module wrote. Returns (width, height, rgb).

    Only 8-bit RGB with filter type 0 is handled, which is exactly what
    write_png produces. A reference re-saved by an image editor will land here
    with some other filter and gets a clear error rather than silent nonsense.
    """
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")

    pos = 8
    width = height = None
    idat = bytearray()
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            if depth != 8 or colour != 2:
                raise ValueError(f"{path}: expected 8-bit RGB, got depth {depth} colour {colour}")
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break

    if width is None:
        raise ValueError(f"{path}: no IHDR")

    raw = zlib.decompress(bytes(idat))
    stride = width * 3
    out = bytearray(stride * height)
    for y in range(height):
        start = y * (stride + 1)
        if raw[start] != 0:
            raise ValueError(f"{path}: scanline {y} uses filter {raw[start]}, only 0 is supported")
        out[y * stride:(y + 1) * stride] = raw[start + 1:start + 1 + stride]
    return width, height, bytes(out)


def to_rgb(pixels, fmt, width, height):
    """Expand a PAX framebuffer into 8-bit RGB.

    Channel order follows PAX's own little-endian packing. The benchmark corpus
    is white text on black, so a wrong order would not be visible by eye -- but
    it is applied identically to a reference and to the run being diffed against
    it, so a diff stays correct either way.
    """
    count = width * height
    out = bytearray(count * 3)

    if fmt == "888":
        if len(pixels) < count * 3:
            raise ValueError("short 888 buffer")
        for i in range(count):
            b, g, r = pixels[i * 3], pixels[i * 3 + 1], pixels[i * 3 + 2]
            out[i * 3], out[i * 3 + 1], out[i * 3 + 2] = r, g, b
        return bytes(out)

    if fmt == "565":
        if len(pixels) < count * 2:
            raise ValueError("short 565 buffer")
        for i in range(count):
            v = pixels[i * 2] | (pixels[i * 2 + 1] << 8)
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            # Replicate the high bits into the low ones so full-scale stays full
            # scale: 0x1F must become 0xFF, not 0xF8.
            out[i * 3] = (r << 3) | (r >> 2)
            out[i * 3 + 1] = (g << 2) | (g >> 4)
            out[i * 3 + 2] = (b << 3) | (b >> 2)
        return bytes(out)

    raise ValueError(f"unsupported pixel format {fmt!r}")


def diff_panels(reference, actual, width, height):
    """Three panels side by side: reference, actual, amplified difference.

    Returns (panel_width, panel_height, rgb) plus the numeric verdict. The
    numbers are the point -- they separate a one-LSB rounding change from a
    missing glyph, which is the difference between blessing a change and
    reverting it.
    """
    differing = 0
    max_delta = 0
    total_delta = 0
    box = [width, height, -1, -1]  # x0, y0, x1, y1

    out_w = width * 3
    out = bytearray(out_w * height * 3)

    for y in range(height):
        row = y * width * 3
        for x in range(width):
            i = row + x * 3
            dr = abs(reference[i] - actual[i])
            dg = abs(reference[i + 1] - actual[i + 1])
            db = abs(reference[i + 2] - actual[i + 2])
            delta = max(dr, dg, db)
            if delta:
                differing += 1
                total_delta += dr + dg + db
                max_delta = max(max_delta, delta)
                box[0], box[1] = min(box[0], x), min(box[1], y)
                box[2], box[3] = max(box[2], x), max(box[3], y)

            o = y * out_w * 3 + x * 3
            out[o:o + 3] = reference[i:i + 3]
            o2 = o + width * 3
            out[o2:o2 + 3] = actual[i:i + 3]
            # Amplified so a single-LSB difference is actually visible.
            o3 = o + width * 6
            out[o3] = min(255, dr * 32)
            out[o3 + 1] = min(255, dg * 32)
            out[o3 + 2] = min(255, db * 32)

    verdict = {
        "pixels_differing": differing,
        "pct_differing": differing / (width * height) * 100.0,
        "max_channel_delta": max_delta,
        "mean_channel_delta": (total_delta / (differing * 3)) if differing else 0.0,
        "bbox": box if box[2] >= 0 else None,
    }
    return out_w, height, bytes(out), verdict
