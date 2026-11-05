// Folia brand marks, built from explicit vertices.
//
// The icon and banner are a low-poly leaf (Folia ≈ folium, leaf/page): a
// hand-placed vertex list, triangulated, each face shaded from a fold along
// the midrib. The wordmark is made the same way (polylines through vertices),
// and the vertices are drawn as anchor dots. Every output is regenerated from
// this file:
//
//   swift packaging/brand/make_brand.swift          # writes next to this file
//   swift packaging/brand/make_brand.swift outdir   # or into outdir
//
// Writes folia-icon.svg/.png (1024), Folia.icns (the app icon, see
// src/CMakeLists.txt) and folia-banner.svg/.png (1280×640, GitHub's social
// preview size; the README uses the SVG). Needs only macOS (CoreGraphics,
// CoreText, iconutil). SVG text is emitted as outlines, so no fonts are needed
// to view it.

import AppKit
import CoreText
import ImageIO
import UniformTypeIdentifiers

// MARK: - Basics

struct P {
    var x: Double
    var y: Double
    init(_ x: Double, _ y: Double) { self.x = x; self.y = y }
}
func + (a: P, b: P) -> P { P(a.x + b.x, a.y + b.y) }
func - (a: P, b: P) -> P { P(a.x - b.x, a.y - b.y) }
func * (a: P, k: Double) -> P { P(a.x * k, a.y * k) }
func dist(_ a: P, _ b: P) -> Double { hypot(a.x - b.x, a.y - b.y) }
func clamp01(_ v: Double) -> Double { min(1, max(0, v)) }

struct RGBA {
    var r, g, b, a: Double
    init(_ hex: UInt32, _ a: Double = 1) {
        r = Double((hex >> 16) & 0xff) / 255
        g = Double((hex >> 8) & 0xff) / 255
        b = Double(hex & 0xff) / 255
        self.a = a
    }
    init(r: Double, g: Double, b: Double, a: Double) { self.r = r; self.g = g; self.b = b; self.a = a }
    func alpha(_ k: Double) -> RGBA { RGBA(r: r, g: g, b: b, a: a * k) }
    func mix(_ o: RGBA, _ t: Double) -> RGBA {
        RGBA(r: r + (o.r - r) * t, g: g + (o.g - g) * t, b: b + (o.b - b) * t, a: a + (o.a - a) * t)
    }
    var cg: CGColor { CGColor(srgbRed: r, green: g, blue: b, alpha: a) }
    var hex: String {
        String(format: "#%02x%02x%02x", Int((r * 255).rounded()), Int((g * 255).rounded()), Int((b * 255).rounded()))
    }
}

/// Deterministic PRNG so every run produces identical files.
struct RNG {
    var s: UInt64
    mutating func next() -> Double {
        s = s &* 6364136223846793005 &+ 1442695040888963407
        return Double(s >> 11) / Double(UInt64(1) << 53)
    }
}

// Palette (accent is Folia's default theme accent, presets/default/shell.lua).
let accent = RGBA(0x2f6fdf)
let deep = RGBA(0x1a43a0)
let light = RGBA(0x86baff)
let ink = RGBA(0x111c38)

// MARK: - Scene (one list of drawing ops, two backends: SVG and CoreGraphics)

enum Paint { case solid(RGBA), vertical(RGBA, RGBA) }
struct Shadow { var dy: Double; var blur: Double; var color: RGBA }
enum Op {
    case fill(CGPath, Paint, Shadow?)
    case stroke(CGPath, RGBA, Double, CGLineJoin, CGLineCap)
}

final class Scene {
    let width: Double, height: Double
    var ops: [Op] = []
    init(_ w: Double, _ h: Double) { width = w; height = h }
    func fill(_ p: CGPath, _ c: RGBA) { ops.append(.fill(p, .solid(c), nil)) }
    func stroke(_ p: CGPath, _ c: RGBA, _ w: Double, join: CGLineJoin = .miter, cap: CGLineCap = .butt) {
        ops.append(.stroke(p, c, w, join, cap))
    }
}

func poly(_ pts: [P], closed: Bool = true) -> CGPath {
    let m = CGMutablePath()
    m.move(to: CGPoint(x: pts[0].x, y: pts[0].y))
    for p in pts.dropFirst() { m.addLine(to: CGPoint(x: p.x, y: p.y)) }
    if closed { m.closeSubpath() }
    return m
}
func circle(_ c: P, _ r: Double) -> CGPath {
    CGPath(ellipseIn: CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r), transform: nil)
}
/// Superellipse |x|^n + |y|^n = 1 as a polygon (close to Apple's icon squircle at n = 5).
func squircle(center c: P, half a: Double, n: Double = 5, count: Int = 360) -> CGPath {
    var pts: [P] = []
    for i in 0..<count {
        let t = Double(i) / Double(count) * 2 * .pi
        let ct = cos(t), st = sin(t)
        let x = a * (ct < 0 ? -1 : 1) * pow(abs(ct), 2 / n)
        let y = a * (st < 0 ? -1 : 1) * pow(abs(st), 2 / n)
        pts.append(P(c.x + x, c.y + y))
    }
    return poly(pts)
}

// MARK: SVG backend

func num(_ v: Double) -> String {
    var s = String(format: "%.2f", v)
    while s.hasSuffix("0") { s.removeLast() }
    if s.hasSuffix(".") { s.removeLast() }
    return s == "-0" ? "0" : s
}

func svgD(_ path: CGPath) -> String {
    var d = ""
    path.applyWithBlock { e in
        let p = e.pointee.points
        func xy(_ i: Int) -> String { "\(num(Double(p[i].x))) \(num(Double(p[i].y)))" }
        switch e.pointee.type {
        case .moveToPoint: d += "M\(xy(0))"
        case .addLineToPoint: d += "L\(xy(0))"
        case .addQuadCurveToPoint: d += "Q\(xy(0)) \(xy(1))"
        case .addCurveToPoint: d += "C\(xy(0)) \(xy(1)) \(xy(2))"
        case .closeSubpath: d += "Z"
        @unknown default: break
        }
    }
    return d
}

func renderSVG(_ sc: Scene, title: String) -> String {
    var defs = "", body = "", id = 0
    func colorAttr(_ key: String, _ c: RGBA) -> String {
        "\(key)=\"\(c.hex)\"" + (c.a < 0.999 ? " \(key)-opacity=\"\(num(c.a))\"" : "")
    }
    for op in sc.ops {
        switch op {
        case let .fill(path, paint, shadow):
            var attrs: String
            switch paint {
            case let .solid(c): attrs = colorAttr("fill", c)
            case let .vertical(a, b):
                id += 1
                defs += "<linearGradient id=\"g\(id)\" x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\">"
                    + "<stop offset=\"0\" stop-color=\"\(a.hex)\"/><stop offset=\"1\" stop-color=\"\(b.hex)\"/></linearGradient>\n"
                attrs = "fill=\"url(#g\(id))\""
            }
            if let sh = shadow {
                id += 1
                defs += "<filter id=\"f\(id)\" x=\"-20%\" y=\"-20%\" width=\"140%\" height=\"140%\">"
                    + "<feDropShadow dx=\"0\" dy=\"\(num(sh.dy))\" stdDeviation=\"\(num(sh.blur / 2))\" "
                    + "flood-color=\"\(sh.color.hex)\" flood-opacity=\"\(num(sh.color.a))\"/></filter>\n"
                attrs += " filter=\"url(#f\(id))\""
            }
            body += "<path d=\"\(svgD(path))\" \(attrs)/>\n"
        case let .stroke(path, c, w, join, cap):
            let j = join == .miter ? "miter" : join == .round ? "round" : "bevel"
            let k = cap == .butt ? "butt" : cap == .round ? "round" : "square"
            body += "<path d=\"\(svgD(path))\" fill=\"none\" \(colorAttr("stroke", c)) stroke-width=\"\(num(w))\" "
                + "stroke-linejoin=\"\(j)\" stroke-linecap=\"\(k)\"/>\n"
        }
    }
    let w = num(sc.width), h = num(sc.height)
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"\(w)\" height=\"\(h)\" viewBox=\"0 0 \(w) \(h)\">\n"
        + "<title>\(title)</title>\n<defs>\n\(defs)</defs>\n\(body)</svg>\n"
}

// MARK: CoreGraphics backend

let sRGB = CGColorSpace(name: CGColorSpace.sRGB)!

func renderPNG(_ sc: Scene, pixelWidth: Int, to url: URL) {
    let scale = Double(pixelWidth) / sc.width
    let W = pixelWidth, H = Int((sc.height * scale).rounded())
    let ctx = CGContext(data: nil, width: W, height: H, bitsPerComponent: 8, bytesPerRow: 0, space: sRGB,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.translateBy(x: 0, y: CGFloat(H))
    ctx.scaleBy(x: scale, y: -scale)  // y down, same coordinates as the SVG
    for op in sc.ops {
        switch op {
        case let .fill(path, paint, shadow):
            ctx.saveGState()
            if let sh = shadow {
                // Shadow offsets are in device space (y up), so "down" is negative.
                ctx.setShadow(offset: CGSize(width: 0, height: -sh.dy * scale), blur: sh.blur * scale, color: sh.color.cg)
                let base: RGBA
                switch paint { case let .solid(c): base = c; case let .vertical(a, _): base = a }
                ctx.addPath(path); ctx.setFillColor(base.cg); ctx.fillPath()
                ctx.setShadow(offset: .zero, blur: 0, color: nil)
            }
            switch paint {
            case let .solid(c):
                ctx.addPath(path); ctx.setFillColor(c.cg); ctx.fillPath()
            case let .vertical(a, b):
                ctx.addPath(path); ctx.clip()
                let bb = path.boundingBoxOfPath
                let g = CGGradient(colorsSpace: sRGB, colors: [a.cg, b.cg] as CFArray, locations: [0, 1])!
                ctx.drawLinearGradient(g, start: CGPoint(x: 0, y: bb.minY), end: CGPoint(x: 0, y: bb.maxY), options: [])
            }
            ctx.restoreGState()
        case let .stroke(path, c, w, join, cap):
            ctx.saveGState()
            ctx.addPath(path)
            ctx.setStrokeColor(c.cg); ctx.setLineWidth(w); ctx.setLineJoin(join); ctx.setLineCap(cap)
            ctx.strokePath()
            ctx.restoreGState()
        }
    }
    let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dest, ctx.makeImage()!, nil)
    CGImageDestinationFinalize(dest)
}

// MARK: - The leaf

struct Leaf {
    var v: [P]                 // world-space vertices
    var tris: [(Int, Int, Int)]
    var colors: [RGBA]
    var inner: [(Int, Int)]    // edges shared by two faces
    var stem: [P]
    var k: Double              // world length of the leaf axis
}

/// Joins two vertex chains that start at a shared edge and end at a shared
/// edge with a strip of triangles, always advancing the chain whose next
/// vertex is nearer the base. Both chains are ordered base → tip along x.
func zipper(_ a: [Int], _ b: [Int], x: (Int) -> Double) -> [(Int, Int, Int)] {
    var tris: [(Int, Int, Int)] = []
    var i = 0, j = 0
    while i < a.count - 1 || j < b.count - 1 {
        let stepA = j == b.count - 1 || (i < a.count - 1 && x(a[i + 1]) <= x(b[j + 1]))
        if stepA { tris.append((a[i], a[i + 1], b[j])); i += 1 } else { tris.append((a[i], b[j], b[j + 1])); j += 1 }
    }
    return tris
}

func makeLeaf(center: P, size: Double, angle: Double = -45) -> Leaf {
    // Leaf-local frame: x runs base (0) → tip (1) along the midrib, y is the
    // half-width (+ upper, − lower), z lifts the midrib into a gentle fold.
    let local: [(x: Double, y: Double, z: Double)] = [
        (0.00, 0.000, 0.00), (0.24, 0.000, 0.13), (0.50, 0.000, 0.15), (0.76, 0.000, 0.11), // 0-3 base + midrib
        (1.00, 0.000, 0.00),                                                                   // 4 tip
        (0.10, 0.130, 0.00), (0.30, 0.260, 0.05), (0.52, 0.270, 0.00), (0.72, 0.175, 0.04), (0.87, 0.085, 0.00), // 5-9 upper edge
        (0.18, -0.190, 0.04), (0.40, -0.270, 0.00), (0.62, -0.240, 0.05), (0.84, -0.110, 0.00), // 10-13 lower edge
    ]
    let stemLocal = [P(0, 0), P(-0.16, -0.03)]
    let th = angle * .pi / 180
    let u = P(cos(th), sin(th))
    let n = P(u.y, -u.x)  // perpendicular, pointing to the leaf's upper side
    func rot(_ x: Double, _ y: Double) -> P { u * x + n * y }

    var all = local.map { rot($0.x, $0.y) }
    all += stemLocal.map { rot($0.x, $0.y) }
    let minX = all.map(\.x).min()!, maxX = all.map(\.x).max()!
    let minY = all.map(\.y).min()!, maxY = all.map(\.y).max()!
    let k = size / max(maxX - minX, maxY - minY)
    let off = center - P((minX + maxX) / 2, (minY + maxY) / 2) * k
    let v = local.map { off + rot($0.x, $0.y) * k }
    let stem = stemLocal.map { off + rot($0.x, $0.y) * k }

    let midrib = [0, 1, 2, 3]
    let tris = zipper(midrib, [5, 6, 7, 8, 9, 4], x: { local[$0].x })
             + zipper(midrib, [10, 11, 12, 13, 4], x: { local[$0].x })

    // Flat shading: light from the upper left, toward the viewer.
    let L = { () -> (Double, Double, Double) in
        let l = (-0.5, -0.8, 0.85); let m = sqrt(l.0 * l.0 + l.1 * l.1 + l.2 * l.2)
        return (l.0 / m, l.1 / m, l.2 / m)
    }()
    var shade: [Double] = []
    for (a, b, c) in tris {
        let pa = (v[a].x, v[a].y, local[a].z * k), pb = (v[b].x, v[b].y, local[b].z * k), pc = (v[c].x, v[c].y, local[c].z * k)
        let e1 = (pb.0 - pa.0, pb.1 - pa.1, pb.2 - pa.2), e2 = (pc.0 - pa.0, pc.1 - pa.1, pc.2 - pa.2)
        var nx = e1.1 * e2.2 - e1.2 * e2.1, ny = e1.2 * e2.0 - e1.0 * e2.2, nz = e1.0 * e2.1 - e1.1 * e2.0
        if nz < 0 { nx = -nx; ny = -ny; nz = -nz }
        let m = sqrt(nx * nx + ny * ny + nz * nz)
        shade.append((nx * L.0 + ny * L.1 + nz * L.2) / m)
    }
    let lo = shade.min()!, hi = shade.max()!
    let colors = shade.map { s -> RGBA in
        let t = 0.04 + 0.92 * (s - lo) / (hi - lo)
        return t < 0.5 ? deep.mix(accent, t * 2) : accent.mix(light, (t - 0.5) * 2)
    }

    var count: [String: (Int, Int, Int)] = [:]
    for (a, b, c) in tris {
        for (p, q) in [(a, b), (b, c), (c, a)] {
            let key = "\(min(p, q))-\(max(p, q))"
            let e = count[key] ?? (min(p, q), max(p, q), 0)
            count[key] = (e.0, e.1, e.2 + 1)
        }
    }
    var inner: [(Int, Int)] = []
    for key in count.keys.sorted() where count[key]!.2 == 2 {
        inner.append((count[key]!.0, count[key]!.1))
    }
    return Leaf(v: v, tris: tris, colors: colors, inner: inner, stem: stem, k: k)
}

func anchor(_ sc: Scene, _ p: P, r: Double, ring: Double, ringColor: RGBA, fill: RGBA = RGBA(0xffffff)) {
    sc.fill(circle(p, r), ringColor)
    sc.fill(circle(p, r - ring), fill)
}

func draw(_ leaf: Leaf, into sc: Scene, anchors: Bool = true) {
    let k = leaf.k
    sc.stroke(poly(leaf.stem, closed: false), deep, 0.032 * k, join: .round, cap: .round)
    for (i, t) in leaf.tris.enumerated() {
        sc.fill(poly([leaf.v[t.0], leaf.v[t.1], leaf.v[t.2]]), leaf.colors[i])
    }
    for (a, b) in leaf.inner {
        sc.stroke(poly([leaf.v[a], leaf.v[b]], closed: false), RGBA(0xffffff, 0.55), 0.0036 * k, cap: .round)
    }
    guard anchors else { return }
    for p in leaf.v + [leaf.stem.last!] {
        anchor(sc, p, r: 0.0145 * k, ring: 0.0052 * k, ringColor: deep)
    }
}

// MARK: - Wordmark: "Folia" as polylines through vertices

struct Wordmark {
    var lines: [(pts: [P], closed: Bool)] = []
    var tittle = P(0, 0), tittleR = 0.0
    var width = 0.0, stroke = 0.0
}

/// Letters in cap-height units (y up), placed with left edge `x` and baseline `y` at cap height `C`.
func makeWordmark(x ox: Double, baseline oy: Double, cap C: Double) -> Wordmark {
    let s = 0.12, X = 0.72  // stroke, x-height
    var w = Wordmark()
    func T(_ x: Double, _ y: Double) -> P { P(ox + x * C, oy - y * C) }
    func octagon(_ cx: Double) -> [P] {
        let a = X / 2 - s / 2, R = a / cos(.pi / 8)
        return (0..<8).map { i in
            let t = .pi / 8 + Double(i) * .pi / 4
            return T(cx + R * cos(t), X / 2 + R * sin(t))
        }
    }
    // F
    w.lines.append(([T(0.56, 1 - s / 2), T(s / 2, 1 - s / 2), T(s / 2, 0)], false))
    w.lines.append(([T(s / 2, 0.48), T(0.47, 0.48)], false))
    // o
    let xo = 0.60
    w.lines.append((octagon(xo + X / 2), true))
    // l
    let xl = xo + X + 0.14
    w.lines.append(([T(xl + s / 2, 1), T(xl + s / 2, 0)], false))
    // i (its tittle is a single vertex, in the accent)
    let xi = xl + s + 0.17
    w.lines.append(([T(xi + s / 2, X), T(xi + s / 2, 0)], false))
    w.tittleR = s * 0.62 * C
    w.tittle = T(xi + s / 2, X + 0.10 + s * 0.62)
    // a (single-storey: octagon bowl + stem on its right flat)
    let xa = xi + s + 0.14
    w.lines.append((octagon(xa + X / 2), true))
    w.lines.append(([T(xa + X - s / 2, X), T(xa + X - s / 2, 0)], false))
    w.width = (xa + X) * C
    w.stroke = s * C
    return w
}

func draw(_ w: Wordmark, into sc: Scene) {
    for l in w.lines { sc.stroke(poly(l.pts, closed: l.closed), ink, w.stroke) }
    // The one visible vertex in the wordmark: the i's tittle, drawn like the leaf's anchors.
    anchor(sc, w.tittle, r: w.tittleR, ring: w.tittleR * 0.42, ringColor: accent)
}

// MARK: - Text as outlines

func textPath(_ str: String, font: NSFont, kern: Double = 0, x: Double, baseline: Double) -> (path: CGPath, width: Double) {
    var attrs: [NSAttributedString.Key: Any] = [.font: font]
    if kern != 0 { attrs[.kern] = kern }
    let line = CTLineCreateWithAttributedString(NSAttributedString(string: str, attributes: attrs))
    var width = CTLineGetTypographicBounds(line, nil, nil, nil)
    if kern != 0 { width -= kern }  // no tracking after the last letter
    let out = CGMutablePath()
    for run in CTLineGetGlyphRuns(line) as! [CTRun] {
        let n = CTRunGetGlyphCount(run)
        var glyphs = [CGGlyph](repeating: 0, count: n)
        var pos = [CGPoint](repeating: .zero, count: n)
        CTRunGetGlyphs(run, CFRange(location: 0, length: n), &glyphs)
        CTRunGetPositions(run, CFRange(location: 0, length: n), &pos)
        let runFont = (CTRunGetAttributes(run) as! [NSAttributedString.Key: Any])[.font] as! NSFont
        for i in 0..<n {
            var t = CGAffineTransform(a: 1, b: 0, c: 0, d: -1, tx: x + pos[i].x, ty: baseline - pos[i].y)
            if let g = CTFontCreatePathForGlyph(runFont as CTFont, glyphs[i], &t) { out.addPath(g) }
        }
    }
    return (out, width)
}

// MARK: - Icon

func iconScene(detail: Bool) -> Scene {
    let sc = Scene(1024, 1024)
    // macOS icon grid: 824 pt body centred in 1024, soft drop shadow.
    sc.ops.append(.fill(squircle(center: P(512, 512), half: 412),
                        .vertical(RGBA(0xffffff), RGBA(0xe6ecf7)),
                        Shadow(dy: 10, blur: 28, color: RGBA(0x0b1530, 0.30))))
    draw(makeLeaf(center: P(512, 512), size: 560), into: sc, anchors: detail)
    return sc
}

// MARK: - Banner

func bannerScene() -> Scene {
    let W = 1280.0, H = 640.0
    let sc = Scene(W, H)
    sc.ops.append(.fill(poly([P(0, 0), P(W, 0), P(W, H), P(0, H)]), .vertical(RGBA(0xfbfcff), RGBA(0xedf1f9)), nil))

    // Measure the text block, then centre leaf + text as one group.
    let leafSize = 420.0, gap = 88.0, cap = 150.0
    let tagFont = NSFont.systemFont(ofSize: 29, weight: .medium)
    let tagline = "Programmable, lightweight local notes."
    let featFont = NSFont.systemFont(ofSize: 14, weight: .semibold)
    let features = ["NESTED PAGES", "LATEX MATH", "CODE BLOCKS", "LUA LAYOUTS"]
    let featKern = 1.4, sepGap = 15.0
    let tagW = textPath(tagline, font: tagFont, x: 0, baseline: 0).width
    let featWs = features.map { textPath($0, font: featFont, kern: featKern, x: 0, baseline: 0).width }
    let featW = featWs.reduce(0, +) + Double(features.count - 1) * 2 * sepGap
    let wordW = makeWordmark(x: 0, baseline: 0, cap: cap).width
    let textW = max(wordW, tagW, featW)
    let left = (W - (leafSize + gap + textW)) / 2
    let leafC = P(left + leafSize / 2, H / 2)
    let tx = left + leafSize + gap
    let blockH = cap + 62 + 42
    let wordBase = H / 2 - blockH / 2 + cap
    let tagBase = wordBase + 62, featBase = tagBase + 42

    // Background mesh: a jittered vertex grid, triangulated, fading out from
    // the leaf (and from the top-right corner) so the text stays clean.
    var rng = RNG(s: 0xF011A)
    let cw = 92.0, ch = 92.0
    let cols = Int(W / cw) + 2, rows = Int(H / ch) + 2
    var grid: [[P]] = []
    for j in -1...rows {
        var row: [P] = []
        for i in -1...cols {
            let jx = (rng.next() - 0.5) * 0.64 * cw, jy = (rng.next() - 0.5) * 0.64 * ch
            row.append(P(Double(i) * cw + jx, Double(j) * ch + jy))
        }
        grid.append(row)
    }
    func mask(_ p: P) -> Double {
        let a = pow(clamp01(1 - dist(p, leafC) / 470), 1.4)
        let b = 0.75 * pow(clamp01(1 - dist(p, P(W + 30, -30)) / 430), 1.4)
        return max(a, b)
    }
    var edges: [(P, P)] = []
    for j in 0..<(grid.count - 1) {
        for i in 0..<(grid[j].count - 1) {
            let a = grid[j][i], b = grid[j][i + 1], c = grid[j + 1][i + 1], d = grid[j + 1][i]
            let faces = rng.next() < 0.5 ? [[a, b, c], [a, c, d]] : [[a, b, d], [b, c, d]]
            for f in faces {
                let m = mask(P((f[0].x + f[1].x + f[2].x) / 3, (f[0].y + f[1].y + f[2].y) / 3))
                if m > 0.01 { sc.fill(poly(f), accent.alpha(0.11 * m * (0.3 + 0.7 * rng.next()))) }
            }
            edges.append((a, b)); edges.append((a, d))
            edges.append(rng.next() < 0.5 ? (a, c) : (b, d))
        }
    }
    for (p, q) in edges {
        let m = mask(P((p.x + q.x) / 2, (p.y + q.y) / 2))
        if m > 0.01 { sc.stroke(poly([p, q], closed: false), accent.alpha(0.30 * m), 1) }
    }
    for row in grid { for p in row {
        let m = mask(p)
        if m > 0.01 { sc.fill(circle(p, 2.6), accent.alpha(0.55 * m)) }
    } }

    draw(makeLeaf(center: leafC, size: leafSize), into: sc)
    draw(makeWordmark(x: tx, baseline: wordBase, cap: cap), into: sc)

    sc.fill(textPath(tagline, font: tagFont, x: tx, baseline: tagBase).path, RGBA(0x37435f))
    var fx = tx
    for (i, f) in features.enumerated() {
        if i > 0 {
            sc.fill(circle(P(fx + sepGap, featBase - 5), 2.6), accent)
            fx += 2 * sepGap
        }
        sc.fill(textPath(f, font: featFont, kern: featKern, x: fx, baseline: featBase).path, RGBA(0x76819a))
        fx += featWs[i]
    }
    return sc
}

// MARK: - Main

let outDir = CommandLine.arguments.count > 1
    ? URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
    : URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let fm = FileManager.default
try fm.createDirectory(at: outDir, withIntermediateDirectories: true)
func out(_ name: String) -> URL { outDir.appendingPathComponent(name) }

let icon = iconScene(detail: true)
try renderSVG(icon, title: "Folia").write(to: out("folia-icon.svg"), atomically: true, encoding: .utf8)
renderPNG(icon, pixelWidth: 1024, to: out("folia-icon.png"))

// .icns: anchors are dropped below 128 px, where they would only be specks.
let iconset = fm.temporaryDirectory.appendingPathComponent("Folia-\(getpid()).iconset", isDirectory: true)
try? fm.removeItem(at: iconset)
try fm.createDirectory(at: iconset, withIntermediateDirectories: true)
for size in [16, 32, 128, 256, 512] {
    for mult in [1, 2] {
        let px = size * mult
        let name = "icon_\(size)x\(size)\(mult == 2 ? "@2x" : "").png"
        renderPNG(iconScene(detail: px >= 128), pixelWidth: px, to: iconset.appendingPathComponent(name))
    }
}
let iconutil = Process()
iconutil.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
iconutil.arguments = ["-c", "icns", iconset.path, "-o", out("Folia.icns").path]
try iconutil.run(); iconutil.waitUntilExit()
try? fm.removeItem(at: iconset)
guard iconutil.terminationStatus == 0 else { fatalError("iconutil failed") }

let banner = bannerScene()
try renderSVG(banner, title: "Folia").write(to: out("folia-banner.svg"), atomically: true, encoding: .utf8)
renderPNG(banner, pixelWidth: 1280, to: out("folia-banner.png"))

for f in ["folia-icon.svg", "folia-icon.png", "Folia.icns", "folia-banner.svg", "folia-banner.png"] {
    print(out(f).path)
}
