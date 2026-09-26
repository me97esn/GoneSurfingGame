# Convert images/"surfboards outline.svg" into the point lists SBoardOutlineGlyph strokes.
#
# A sibling of svg2glyph.py rather than an extension of it, because the contract differs in the one
# way that matters: svg2glyph normalises EACH drawing to span -1..1, which is right for the tutorial
# board (one shape, filling its widget) and wrong here. The board rack's whole job is showing that a
# foamie is longer than a fish, so every board shares ONE scale factor.
#
# Three things the first version got wrong, all of them visible in the game before they were traced
# back to here:
#
#   * It only read <path>. The foamie's fin screws and leash plug are seven <ellipse> elements, so
#     they were silently dropped and the foamie came out as a bare oval.
#   * It threw the style away and drew everything at one weight. The drawing already says what it
#     wants: stroke-width 1.5 on the outlines, ~0.5 on the stringers. Those are exported as relative
#     weights now, so a stringer is thinner than the rail it sits inside, as drawn.
#   * A closed subpath left a visible notch where the polyline met itself - Slate strokes with butt
#     caps and does not mitre that seam. Closed shapes now repeat their second point so the next
#     segment paints over the join.
#
# Output convention, matching what the glyph expects:
#   x = along the board, +x is the NOSE
#   y = across the board, +/- half width
#   scale is shared: the longest board spans x -1..1, every other board proportionally shorter
#   each board centred on its own bounding box; Length/Width exported so a widget can bottom-align
#   a rack without measuring anything at runtime
#
# Usage:  python Tools/svg2boards.py > Source/GoneSurfing/BoardOutlineShapes.inc
import io, re, sys, math, xml.etree.ElementTree as ET

SVG = '{http://www.w3.org/2000/svg}'
INK = '{http://www.inkscape.org/namespaces/inkscape}'
NUM = re.compile(r'[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?')
SEG = 40                      # bezier subdivisions. 16 (svg2glyph's value) left a
                              # visible straight chord across the tight nose curve at
                              # rack size; these are drawn far larger than the tutorial
                              # glyph they were borrowed from.
ELLIPSE_SEG = 12              # a fin screw is ~2px across in the rack; 12 is already round
REFERENCE_STROKE = 1.5        # the outlines' stroke-width; everything is a ratio of this

SRC = r'E:\windowsgrejor\git\GoneSurfingUE5\GoneSurfing\images\surfboards outline.svg'
# Draw order is also rack order: difficulty ascending.
BOARDS = ['Foamie', 'Funboard', 'Hybrid', 'Fish', 'Shortboard']


def stroke_weight(el):
    """Relative stroke weight for an element, 1.0 being the board outline.

    ZERO is meaningful: it marks a shape the drawing FILLS rather than strokes - the foamie's fin
    screws and leash plug. Outlining those left a heavy ring, because a 1px stroke around a 3px dot
    is mostly stroke; the glyph fills them instead."""
    style = el.get('style') or ''
    flat = style.replace(' ', '')
    stroked = 'stroke:none' not in flat
    filled = 'fill:none' not in flat and re.search(r'fill:#', flat) is not None
    if not stroked and filled:
        return 0.0
    m = re.search(r'stroke-width\s*:\s*([\d.]+)', style)
    if m and stroked:
        return max(0.15, float(m.group(1)) / REFERENCE_STROKE)
    return 0.55


def parse_path(d):
    """-> list of (points, closed) subpaths, sampled along the curves."""
    toks = re.findall(r'[MmCcZzLlHhVvSsQqTtAa]|' + NUM.pattern, d)
    i = 0
    cur = (0.0, 0.0)
    start = (0.0, 0.0)
    out, sub = [], []
    cmd = None
    need = {'m': 2, 'M': 2, 'l': 2, 'L': 2, 'c': 6, 'C': 6, 'h': 1, 'H': 1,
            'v': 1, 'V': 1, 's': 4, 'S': 4, 'q': 4, 'Q': 4, 't': 2, 'T': 2, 'a': 7, 'A': 7}
    while i < len(toks):
        t = toks[i]
        if re.match(r'^[A-Za-z]$', t):
            cmd = t
            i += 1
            if cmd in 'Zz':
                if sub:
                    out.append((sub, True))
                    sub = []
                cur = start
                continue
        if cmd is None or cmd not in need:
            i += 1
            continue
        nums = []
        while len(nums) < need[cmd] and i < len(toks):
            nums.append(float(toks[i]))
            i += 1
        if len(nums) < need[cmd]:
            break
        rel = cmd.islower()
        if cmd in 'mM':
            cur = (nums[0], nums[1]) if not rel else (cur[0] + nums[0], cur[1] + nums[1])
            if sub:
                out.append((sub, False))
            sub = [cur]
            start = cur
            cmd = 'l' if rel else 'L'      # subsequent pairs are implicit linetos
        elif cmd in 'lL':
            cur = (nums[0], nums[1]) if not rel else (cur[0] + nums[0], cur[1] + nums[1])
            sub.append(cur)
        elif cmd in 'hH':
            cur = (nums[0] if not rel else cur[0] + nums[0], cur[1])
            sub.append(cur)
        elif cmd in 'vV':
            cur = (cur[0], nums[0] if not rel else cur[1] + nums[0])
            sub.append(cur)
        elif cmd in 'cC':
            if rel:
                c1 = (cur[0] + nums[0], cur[1] + nums[1])
                c2 = (cur[0] + nums[2], cur[1] + nums[3])
                p1 = (cur[0] + nums[4], cur[1] + nums[5])
            else:
                c1, c2, p1 = (nums[0], nums[1]), (nums[2], nums[3]), (nums[4], nums[5])
            for s in range(1, SEG + 1):
                tt = s / float(SEG)
                u = 1 - tt
                sub.append((u*u*u*cur[0] + 3*u*u*tt*c1[0] + 3*u*tt*tt*c2[0] + tt*tt*tt*p1[0],
                            u*u*u*cur[1] + 3*u*u*tt*c1[1] + 3*u*tt*tt*c2[1] + tt*tt*tt*p1[1]))
            cur = p1
        else:
            cur = (nums[-2], nums[-1]) if not rel else (cur[0] + nums[-2], cur[1] + nums[-1])
            sub.append(cur)
            sys.stderr.write('WARNING: unsupported command %r approximated as a line\n' % cmd)
    if sub:
        out.append((sub, False))
    return out


def parse_ellipse(el):
    """-> one closed subpath. <ellipse>/<circle> are how Inkscape stored the foamie's fin screws
    and leash plug, and reading only <path> is what lost them."""
    cx = float(el.get('cx', 0.0))
    cy = float(el.get('cy', 0.0))
    rx = float(el.get('rx', el.get('r', 0.0)))
    ry = float(el.get('ry', el.get('r', 0.0)))
    pts = []
    for k in range(ELLIPSE_SEG):
        a = 2.0 * math.pi * k / ELLIPSE_SEG
        pts.append((cx + rx * math.cos(a), cy + ry * math.sin(a)))
    return [(pts, True)]


def strokes_of(group):
    """-> [(points, closed, weight)] for every drawable in a board group, in document order."""
    found = []
    for el in group.iter():
        tag = el.tag
        if tag == SVG + 'path':
            subs = parse_path(el.get('d', ''))
        elif tag in (SVG + 'ellipse', SVG + 'circle'):
            subs = parse_ellipse(el)
        else:
            continue
        w = stroke_weight(el)
        for pts, closed in subs:
            if len(pts) >= 2:
                found.append((pts, closed, w))
    return found


def main():
    root = ET.parse(SRC).getroot()
    boards = {}
    for g in root.iter(SVG + 'g'):
        label = g.get(INK + 'label')
        if label in BOARDS:
            boards[label] = strokes_of(g)

    missing = [b for b in BOARDS if b not in boards]
    if missing:
        sys.stderr.write('ERROR: groups not found in the SVG: %s\n' % ', '.join(missing))
        sys.exit(1)

    box = {}
    for name, strokes in boards.items():
        xs = [p[0] for pts, _, _ in strokes for p in pts]
        ys = [p[1] for pts, _, _ in strokes for p in pts]
        box[name] = (min(xs), max(xs), min(ys), max(ys))

    longest = max(box[n][3] - box[n][2] for n in BOARDS)
    scale = 2.0 / longest

    print('// GENERATED by Tools/svg2boards.py from images/"surfboards outline.svg".')
    print('// Do not hand-edit: redraw the SVG and re-run the script.')
    print('//')
    print('// x = along the board, +x is the NOSE. y = across it. All boards share ONE scale, so the')
    print('// longest spans x -1..1 and the rest are proportionally shorter - that relative size IS the')
    print('// point of the rack, and it is baked in here rather than reconstructed at runtime.')
    print('//')
    print('// Each stroke carries a WEIGHT, relative to the board outline at 1.0. It comes from the')
    print('// drawing\'s own stroke-width, so a stringer stays thinner than the rail it sits inside.')
    print('// A weight of 0 means the drawing FILLED that shape rather than stroking it (the foamie fin')
    print('// screws); the glyph fills those instead of ringing them.')
    print('// Closed shapes repeat their second point, because Slate strokes with butt caps and leaves')
    print('// a notch where a closed polyline meets itself.')
    print('')

    for name in BOARDS:
        strokes = boards[name]
        minx, maxx, miny, maxy = box[name]
        cx = (minx + maxx) / 2.0
        cy = (miny + maxy) / 2.0

        def xf(p):
            # SVG y grows downward and the nose is at the TOP of the drawing (verified by
            # rasterising the file), so negating y puts the nose at +x.
            return ((cy - p[1]) * scale, (p[0] - cx) * scale)

        print('static void BuildBoard_%s(TArray<TArray<FVector2f>>& OutStrokes, TArray<float>& OutWeights)' % name)
        print('{')
        for si, (pts, closed, weight) in enumerate(strokes):
            xs = [xf(p) for p in pts]
            if closed:
                # Where a closed polyline meets itself, Slate's butt caps leave a wedge on the
                # outside of the angle - big enough to read as a GAP when the seam lands on a tight
                # curve. That is why the shortboard, hybrid and funboard broke at the nose while the
                # rounder boards did not.
                #
                # The first fix overdrew the seam with an extra segment. It closed the gap but
                # painted that segment twice, leaving a visibly brighter mark at the nose.
                #
                # Better: MOVE the seam. Rotate the loop to start at its straightest vertex, where
                # two nearly-collinear segments leave a wedge of essentially nothing, and close it
                # once. No overdraw, no doubled alpha, no gap.
                if len(xs) > 3:
                    if math.dist(xs[0], xs[-1]) < 1e-6:
                        xs = xs[:-1]                      # drop the duplicated endpoint
                    n = len(xs)

                    def turn(i):
                        a, b, c2 = xs[(i - 1) % n], xs[i], xs[(i + 1) % n]
                        v1 = (b[0] - a[0], b[1] - a[1])
                        v2 = (c2[0] - b[0], c2[1] - b[1])
                        l1 = math.hypot(v1[0], v1[1]) or 1e-9
                        l2 = math.hypot(v2[0], v2[1]) or 1e-9
                        cosang = (v1[0] * v2[0] + v1[1] * v2[1]) / (l1 * l2)
                        return math.acos(max(-1.0, min(1.0, cosang)))   # 0 = perfectly straight

                    seam = min(range(n), key=turn)
                    xs = xs[seam:] + xs[:seam]
                    xs = xs + [xs[0]]                     # close, once
            print('\t{ // stroke %d  (%d pts, weight %.2f%s)' % (si, len(xs), weight, ', closed' if closed else ''))
            print('\t\tstatic const FVector2f P[] = {')
            for k in range(0, len(xs), 4):
                print('\t\t\t' + ', '.join('{%.4ff,%.4ff}' % q for q in xs[k:k+4]) + ',')
            print('\t\t};')
            print('\t\tOutStrokes.Add(TArray<FVector2f>(P, UE_ARRAY_COUNT(P)));')
            print('\t\tOutWeights.Add(%.3ff);' % weight)
            print('\t}')
        print('}')
        print('')

    print('struct FBoardShapeDef')
    print('{')
    print('\tconst TCHAR* Id;')
    print('\tvoid (*Build)(TArray<TArray<FVector2f>>&, TArray<float>&);')
    print('\tfloat Length;   // normalised; the longest board is 2.0')
    print('\tfloat Width;')
    print('};')
    print('')
    print('static const FBoardShapeDef GBoardShapes[] = {')
    for name in BOARDS:
        minx, maxx, miny, maxy = box[name]
        print('\t{ TEXT("%s"), &BuildBoard_%s, %.4ff, %.4ff },'
              % (name.lower(), name, (maxy - miny) * scale, (maxx - minx) * scale))
    print('};')

    sys.stderr.write('shared scale %.5f (longest = %s)\n'
                     % (scale, max(BOARDS, key=lambda n: box[n][3] - box[n][2])))
    for name in BOARDS:
        minx, maxx, miny, maxy = box[name]
        ws = sorted({round(w, 2) for _, _, w in boards[name]})
        sys.stderr.write('  %-11s length %.3f  width %.3f  %d stroke(s), weights %s\n'
                         % (name, (maxy - miny) * scale, (maxx - minx) * scale,
                            len(boards[name]), ws))


main()
