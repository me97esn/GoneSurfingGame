# Measure the surfboard FBX models and derive the two numbers a board profile cannot guess:
# visualScale and surferDeckOffsetZ.
#
#   python Tools/measure_boards.py            # report
#   python Tools/measure_boards.py --write     # report AND update Content/Boards/*.json
#
# WHY THIS EXISTS
#
# The physics body is identical for every board - that is what keeps them fair against each other -
# so a board's size and its rider's foot height are pure presentation, carried by two profile
# fields. Both are properties of the MESH, so both go stale the moment a model is re-exported, and
# both fail silently: a wrong visualScale just makes a board the wrong size, and a wrong
# surferDeckOffsetZ puts the surfer's feet through the deck. Neither throws, neither logs.
#
#   visualScale       = target length / mesh length
#   surferDeckOffsetZ = this board's deck height - the reference board's deck height
#
# TARGET LENGTHS are not measured, they are the design: the rack draws five outlines at fixed
# relative sizes (a foamie is 1.56x a shortboard) and the boards on the wave must match the rack the
# player just read. They are held here as the on-screen lengths that ratio produces.
#
# THE DECK TRIM is needed because the models do not share a vertical frame. Their deck surfaces sit
# at different heights in their own files - the fish's is at +0.170 where the foamie, funboard and
# hybrid all have theirs at +0.509 - and the rider is authored at one height for all five.
import argparse, io, os, re, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
MODELS = os.path.join(PROJECT, "Content", "3dModels")
BOARDS = os.path.join(PROJECT, "Content", "Boards")

# id -> (profile filename, on-screen length in unreal units)
#
# The lengths are the rack's proportions made concrete, anchored on the shortboard at 171.5. Change
# them only if the outlines in images/"surfboards outline.svg" change, and keep the two in step -
# they are the same design expressed twice.
TARGETS = {
    "foamie":     ("1-foamie.json",     267.8),
    "funboard":   ("2-funboard.json",   231.6),
    "fish":       ("3-fish.json",       157.2),
    "hybrid":     ("4-hybrid.json",     183.4),
    "shortboard": ("5-shortboard.json", 171.5),
}

# The board the surfer's authored height belongs to. Its trim is 0 by definition and every other
# board is measured against it, so switch this only if the rider is re-authored on another board.
REFERENCE = "foamie"


def read_vertices(path):
    """Every Vertices array in a binary FBX, flattened. Enough to measure a model without an
    FBX SDK - the file is a tree of length-delimited nodes and the vertex arrays are plain
    (optionally deflated) doubles."""
    d = open(path, "rb").read()
    version = struct.unpack_from("<I", d, 23)[0]
    found = []

    def array(buf, off, fmt, elem_size):
        count, encoding, compressed_len = struct.unpack_from("<III", buf, off)
        off += 12
        raw = buf[off:off + compressed_len]
        off += compressed_len
        if encoding == 1:
            raw = zlib.decompress(raw)
        return list(struct.unpack("<%d%s" % (count, fmt), raw[:count * elem_size])), off

    def node(buf, off):
        if version >= 7500:
            end, num_props, _ = struct.unpack_from("<QQQ", buf, off)
            off += 24
        else:
            end, num_props, _ = struct.unpack_from("<III", buf, off)
            off += 12
        name_len = buf[off]
        off += 1
        name = buf[off:off + name_len].decode("utf8", "replace")
        off += name_len
        if end == 0:          # null terminator record: end of this sibling list
            return None, off
        values = None
        for _ in range(num_props):
            t = chr(buf[off]); off += 1
            if t == "d":   values, off = array(buf, off, "d", 8)
            elif t == "f": values, off = array(buf, off, "f", 4)
            elif t in "ilb":
                _, off = array(buf, off, {"i": "i", "l": "q", "b": "b"}[t],
                               {"i": 4, "l": 8, "b": 1}[t])
            elif t == "Y": off += 2
            elif t == "C": off += 1
            elif t in "IF": off += 4
            elif t in "DL": off += 8
            elif t in "SR": off += 4 + struct.unpack_from("<I", buf, off)[0]
            else:
                return None, end          # unknown property type - skip the subtree, not the file
        if name == "Vertices" and values:
            found.append(values)
        while off < end:
            ok, off = node(buf, off)
            if ok is None and off >= end:
                break
        return True, end

    pos = 27                              # past "Kaydara FBX Binary" + version
    while pos < len(d) - 13:
        ok, pos = node(d, pos)
        if ok is None:
            break
    return [v for arr in found for v in arr]


def measure(path):
    v = read_vertices(path)
    if not v:
        raise RuntimeError("no vertex data found in %s" % path)
    axes = [v[0::3], v[1::3], v[2::3]]
    extents = [max(a) - min(a) for a in axes]
    thin = extents.index(min(extents))    # thickness axis; its max is the deck surface
    return {
        "length": max(extents),
        "width": sorted(extents, reverse=True)[1],
        "deck": max(axes[thin]),
        "bottom": min(axes[thin]),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--write", action="store_true",
                    help="update visualScale and surferDeckOffsetZ in Content/Boards/*.json")
    args = ap.parse_args()

    rows = {}
    for board, (_, target) in TARGETS.items():
        fbx = os.path.join(MODELS, "surfboard_%s.fbx" % board)
        if not os.path.exists(fbx):
            print("MISSING %s" % fbx, file=sys.stderr)
            return 1
        m = measure(fbx)
        m["scale"] = target / m["length"]
        m["deck_world"] = m["deck"] * m["scale"]
        rows[board] = m

    ref_deck = rows[REFERENCE]["deck_world"]
    print("%-11s %8s %8s %9s | %7s %10s %9s" %
          ("board", "length", "width", "deck", "scale", "deck(world)", "trim"))
    for board, m in rows.items():
        m["trim"] = m["deck_world"] - ref_deck
        print("%-11s %8.2f %8.2f %9.3f | %7.3f %10.3f %9.3f" %
              (board, m["length"], m["width"], m["deck"],
               m["scale"], m["deck_world"], m["trim"]))

    if not args.write:
        print("\n(run with --write to apply these to Content/Boards/*.json)")
        return 0

    for board, (filename, _) in TARGETS.items():
        p = os.path.join(BOARDS, filename)
        c = io.open(p, encoding="utf-8").read()
        c = re.sub(r'"visualScale":\s*-?[\d.]+',
                   '"visualScale": %.3f' % rows[board]["scale"], c, count=1)
        c = re.sub(r'"surferDeckOffsetZ":\s*-?[\d.]+',
                   '"surferDeckOffsetZ": %.3f' % rows[board]["trim"], c, count=1)
        io.open(p, "w", encoding="utf-8", newline="").write(c)
        print("wrote %s" % filename)
    return 0


if __name__ == "__main__":
    sys.exit(main())
