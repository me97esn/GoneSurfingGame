# Generate the foamie's deck mask: the fin screws and leash plug, as a texture the board
# material lerps black through.
#
#   python Tools/make_foamie_deck.py                  # default screw size
#   python Tools/make_foamie_deck.py --radius 0.02    # bigger, as a fraction of board WIDTH
#
# WHY A TEXTURE
#
# The foamie is the one board with no stringer. Every other board draws its stringer in the
# material (M_Surfboard: LocalPosition -> Abs -> SmoothStep -> Lerp), and the foamie needs the
# opposite: no stripe, but black circles where a real soft-top has its fin screws and leash plug.
# Procedurally that is ~5 material nodes per circle; one texture sample is six nodes however many
# circles it holds, with room for a deck pad or a logo later without touching the graph.
#
# WHERE THE NUMBERS COME FROM, AND THE TRAP IN THEM
#
# Positions come from the drawing the board rack is built from, images/"surfboards outline.svg",
# where the foamie is drawn with seven FILLED dots and no stringer. They are stored here as
# FRACTIONS of the board's bounding box, which is the one form that survives everything: any
# per-axis scale or translate cancels out of a bbox fraction.
#
# The board's PROPORTIONS do not survive, and that was an expensive lesson. Reading the FBX's raw
# vertex arrays gives 21.7 x 227.8 x 1.2 for this mesh; the engine reports 197 x 760 x 52. The
# difference is the node transform, which the raw vertex data does not include - per-axis factors
# of 9.1, 3.3 and 43.6, so not even close to uniform. Authoring circles against the FBX numbers
# produced ellipses stretched 2.7x across the board, because 227.8/21.7 = 10.5 where the real
# board is 760/197 = 3.9.
#
# So MESH_LENGTH/MESH_WIDTH below are read from the STATIC MESH EDITOR ("Approx Size" at the
# bottom of the asset window), not from the FBX. If the foamie is ever re-exported with a
# different transform, re-read them there.
#
# HOW THE MATERIAL SAMPLES IT
#
# M_SurfboardFoamie planar-projects from the mesh's own bounds, so it needs no constants at all:
#
#     uvw = (LocalPosition - ObjectLocalBounds.Min) / ObjectLocalBounds.Extents
#     u = uvw.G   (along the board)      v = uvw.R   (across it)
#
# That is why the positions came out right even while the shape was wrong - fractions are exactly
# what Object Local Bounds produces. Import with sRGB OFF, Compression = Masks, Address = Clamp.
import argparse, os, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
OUT = os.path.join(PROJECT, "Content", "Materials", "T_FoamieScrews.png")

# From the Static Mesh editor's "Approx Size" for surfboard_foamie: 197 x 760 x 52.
# Only the ratio matters here, and only for keeping circles circular.
MESH_LENGTH = 760.0
MESH_WIDTH = 197.0

# Screws as (along, across) fractions of the board's bounding box: along 0 = tail, 1 = nose;
# across 0.5 = centreline.
#
# Snapped to true symmetry. The drawing has the three centreline screws about 1% left of centre
# and the two pairs about 0.6% right - hand-drawn wobble, invisible at icon size and plainly wrong
# on a board, where fin screws are symmetric by construction. The along positions are the
# artwork's own.
PAIR_OFFSET = 0.2465          # side screws, either side of the centreline
SCREWS = [
    (0.02083, 0.5,                "leash plug"),
    (0.04528, 0.5,                "centre fin screw"),
    (0.08645, 0.5,                "centre fin screw"),
    (0.14438, 0.5 - PAIR_OFFSET,  "side fin screw"),
    (0.14438, 0.5 + PAIR_OFFSET,  "side fin screw"),
    (0.17043, 0.5 - PAIR_OFFSET,  "side fin screw"),
    (0.17043, 0.5 + PAIR_OFFSET,  "side fin screw"),
]

# Radius as a fraction of the board's WIDTH, so it means the same thing whatever units the mesh
# turns out to be in - which, per the note above, is not a given.
#
# The artwork's own dots are 0.0085 of board LENGTH, which on this mesh is 3.3% of the width per
# side: sized to read at icon scale, a porthole in 3D (confirmed on the wave).
DEFAULT_RADIUS = 0.015

# Aspect matched to the mesh, height a multiple of 4 so block compression still works. A circle
# drawn here is then a circle on the board, with no compensation to get wrong.
WIDTH = 2048
HEIGHT = int(round(WIDTH * MESH_WIDTH / MESH_LENGTH / 4)) * 4

SUPERSAMPLE = 3      # 9 samples a pixel; enough to keep small circles smooth


def write_grey_png(path, width, height, rows):
    """8-bit greyscale PNG. Hand-rolled so the tool needs no image library."""
    raw = b"".join(b"\x00" + bytes(row) for row in rows)     # filter byte 0 per scanline

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--radius", type=float, default=DEFAULT_RADIUS,
                    help="screw radius as a fraction of board width (default %.3f)"
                         % DEFAULT_RADIUS)
    ap.add_argument("--out", default=OUT)
    args = ap.parse_args()

    # A circle of radius R (in units of board width) spans R * WIDTH/LENGTH of the board's length.
    # With the texture's aspect matched to the board's, both come out the same pixel count.
    rx = args.radius * (MESH_WIDTH / MESH_LENGTH) * WIDTH
    ry = args.radius * HEIGHT

    rows = [bytearray(WIDTH) for _ in range(HEIGHT)]   # 0 = no screw

    for along, across, _ in SCREWS:
        cu = along * WIDTH
        cv = across * HEIGHT
        for py in range(max(0, int(cv - ry) - 1), min(HEIGHT, int(cv + ry) + 2)):
            for px in range(max(0, int(cu - rx) - 1), min(WIDTH, int(cu + rx) + 2)):
                hits = 0
                for sy in range(SUPERSAMPLE):
                    for sx in range(SUPERSAMPLE):
                        u = px + (sx + 0.5) / SUPERSAMPLE
                        v = py + (sy + 0.5) / SUPERSAMPLE
                        if ((u - cu) / rx) ** 2 + ((v - cv) / ry) ** 2 <= 1.0:
                            hits += 1
                if hits:
                    rows[py][px] = max(rows[py][px], int(255 * hits / (SUPERSAMPLE ** 2)))

    write_grey_png(args.out, WIDTH, HEIGHT, rows)

    print("wrote %s  (%dx%d, greyscale)" % (args.out, WIDTH, HEIGHT))
    print("  board %.0f x %.0f  (aspect %.2f:1, from the Static Mesh editor)"
          % (MESH_LENGTH, MESH_WIDTH, MESH_LENGTH / MESH_WIDTH))
    print("  screw radius %.3f of width -> %.1f x %.1f px  (equal = round on the board)"
          % (args.radius, rx, ry))
    print("  diameter on the board: %.1f units across a %.0f-unit width"
          % (2 * args.radius * MESH_WIDTH, MESH_WIDTH))
    print("  %d screws, symmetric about the centreline" % len(SCREWS))


if __name__ == "__main__":
    main()
