// Thin Swift shell: status item, arrowless panel drawn with CoreGraphics, URLSession
// transport, and a coalesced background scheduler. All logic lives in core.cpp.
import Cocoa
import Darwin
import ServiceManagement

let kOverview = -1
let kW: CGFloat = 340
let kPad: CGFloat = 14

func rgb(_ c: UInt32, _ a: CGFloat = 1) -> NSColor {
  NSColor(
    srgbRed: CGFloat((c >> 16) & 255) / 255, green: CGFloat((c >> 8) & 255) / 255,
    blue: CGFloat(c & 255) / 255, alpha: a)
}

func ownedStr(_ p: UnsafeMutablePointer<CChar>?) -> String {
  guard let p else { return "" }
  defer { free(UnsafeMutableRawPointer(p)) }
  return String(cString: p)
}
func borrowStr(_ p: UnsafePointer<CChar>?) -> String {
  guard let p else { return "" }
  return String(cString: p)
}

func provName(_ p: Int) -> String { borrowStr(ub_info_name(Int32(p))) }
func provSymbol(_ p: Int) -> String { borrowStr(ub_info_symbol(Int32(p))) }
func provRGB(_ p: Int) -> UInt32 { ub_info_rgb(Int32(p)) }
func provNeedsKey(_ p: Int) -> Bool { ub_info_needs_key(Int32(p)) != 0 }
func provKeyHelp(_ p: Int) -> String { borrowStr(ub_info_key_help(Int32(p))) }

struct Win {
  var label = ""
  var usedPct = -1.0
  var resetAt: Int64 = 0
  var durationSec: Int64 = 0
  var inBar = true
}
struct Snap {
  static let empty = Snap()
  var ok = false
  var hasData = false
  var error = ""
  var email = ""
  var plan = ""
  var windows: [Win] = []
  var notes: [String] = []
  var barText = ""
  var updatedAt: Int64 = 0
}

func readSnap(_ p: UnsafeMutablePointer<UBSnapshot>?) -> Snap {
  guard let p else { return Snap.empty }
  var s = Snap()
  s.ok = ub_snapshot_ok(p) != 0
  s.hasData = ub_snapshot_has_data(p) != 0
  s.error = borrowStr(ub_snapshot_error(p))
  s.email = borrowStr(ub_snapshot_email(p))
  s.plan = borrowStr(ub_snapshot_plan(p))
  s.barText = borrowStr(ub_snapshot_bar_text(p))
  s.updatedAt = ub_snapshot_updated(p)
  for i in 0..<Int(ub_snapshot_window_count(p)) {
    let ii = Int32(i)
    s.windows.append(
      Win(
        label: borrowStr(ub_snapshot_window_label(p, ii)), usedPct: ub_snapshot_window_used(p, ii),
        resetAt: ub_snapshot_window_reset(p, ii), durationSec: ub_snapshot_window_duration(p, ii),
        inBar: ub_snapshot_window_in_bar(p, ii) != 0))
  }
  for i in 0..<Int(ub_snapshot_note_count(p)) {
    s.notes.append(borrowStr(ub_snapshot_note(p, Int32(i))))
  }
  return s
}

struct Req {
  var method = "GET"
  var url = ""
  var body = ""
  var headers: [(String, String)] = []
}

final class PlanBox {
  let ptr: UnsafeMutablePointer<UBPlan>
  let requests: [Req]
  let error: String
  let authProblem: Bool
  init?(pid: Int, interactive: Bool) {
    guard let p = ub_plan_build(Int32(pid), interactive ? 1 : 0) else { return nil }
    ptr = p
    var rq: [Req] = []
    for i in 0..<Int(ub_plan_req_count(p)) {
      let ii = Int32(i)
      var hs: [(String, String)] = []
      for h in 0..<Int(ub_plan_req_header_count(p, ii)) {
        let hh = Int32(h)
        hs.append((borrowStr(ub_plan_req_header_key(p, ii, hh)), borrowStr(ub_plan_req_header_value(p, ii, hh))))
      }
      rq.append(
        Req(
          method: borrowStr(ub_plan_req_method(p, ii)), url: borrowStr(ub_plan_req_url(p, ii)),
          body: borrowStr(ub_plan_req_body(p, ii)), headers: hs))
    }
    requests = rq
    error = borrowStr(ub_plan_error(p))
    authProblem = ub_plan_auth_problem(p) != 0
  }
  deinit { ub_plan_free(ptr) }
}

struct UReply {
  var status = 0  // 0 = transport failure
  var body = Data()
  var err = ""
}

// One shared URLSession with a serial delegate queue; performs a plan's
// requests concurrently and reports back on a utility queue.
final class Transport: NSObject, URLSessionDataDelegate {
  struct Task {
    var data = Data()
    var done: (Data, URLResponse?, Error?) -> Void
  }
  let lock = NSLock()
  var tasks: [Int: Task] = [:]
  lazy var session: URLSession = {
    let c = URLSessionConfiguration.ephemeral
    c.urlCache = nil
    c.httpCookieStorage = nil
    c.httpShouldSetCookies = false
    c.requestCachePolicy = .reloadIgnoringLocalCacheData
    c.timeoutIntervalForRequest = 20
    c.timeoutIntervalForResource = 60
    c.waitsForConnectivity = true
    c.httpMaximumConnectionsPerHost = 2
    c.httpAdditionalHeaders = ["User-Agent": "CoffeeUsage/1.0"]
    let q = OperationQueue()
    q.maxConcurrentOperationCount = 1
    q.qualityOfService = .utility
    return URLSession(configuration: c, delegate: self, delegateQueue: q)
  }()

  func perform(_ reqs: [Req], completion: @escaping ([UReply]) -> Void) {
    if reqs.isEmpty { completion([]); return }
    var out = Array(repeating: UReply(), count: reqs.count)
    let group = DispatchGroup()
    for (i, r) in reqs.enumerated() {
      guard let url = URL(string: r.url) else { continue }  // stays a transport failure
      var urlReq = URLRequest(url: url)
      urlReq.httpMethod = r.method
      for h in r.headers { urlReq.setValue(h.1, forHTTPHeaderField: h.0) }
      if !r.body.isEmpty { urlReq.httpBody = Data(r.body.utf8) }
      group.enter()
      let task = session.dataTask(with: urlReq)
      lock.lock()
      tasks[task.taskIdentifier] = Task(done: { data, resp, err in
        out[i] = UReply(
          status: (resp as? HTTPURLResponse)?.statusCode ?? 0, body: data,
          err: (err as NSError?)?.localizedDescription ?? "")
        group.leave()
      })
      lock.unlock()
      task.resume()
    }
    group.notify(queue: DispatchQueue.global(qos: .utility)) { completion(out) }
  }

  func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
    lock.lock()
    tasks[dataTask.taskIdentifier]?.data.append(data)
    lock.unlock()
  }

  func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
    lock.lock()
    let t = tasks.removeValue(forKey: task.taskIdentifier)
    lock.unlock()
    guard let t else { return }
    t.done(t.data, task.response, error)
  }
}

// Pins every Data's bytes for the duration of f (nested scopes keep all
// outer buffers alive), then hands the buffer pointers over.
func withPinned<T>(_ datas: [Data], _ f: ([UnsafeRawBufferPointer]) -> T) -> T {
  var bufs: [UnsafeRawBufferPointer] = []
  bufs.reserveCapacity(datas.count)
  func rec(_ i: Int) -> T {
    if i == datas.count { return f(bufs) }
    return datas[i].withUnsafeBytes { b in
      bufs.append(b)
      return rec(i + 1)
    }
  }
  return rec(0)
}

func parsePlan(_ box: PlanBox, _ replies: [UReply], now: Int64) -> UnsafeMutablePointer<UBSnapshot>? {
  var datas: [Data] = []
  datas.reserveCapacity(replies.count * 2)
  for r in replies {
    datas.append(r.body)
    datas.append(Data(r.err.utf8))
  }
  return withPinned(datas) { bufs in
    var rs: [UBResponse] = []
    for i in replies.indices {
      rs.append(
        UBResponse(
          status: Int32(replies[i].status),
          body: bufs[2 * i].baseAddress.map { $0.assumingMemoryBound(to: CChar.self) },
          body_len: bufs[2 * i].count,
          err: bufs[2 * i + 1].baseAddress.map { $0.assumingMemoryBound(to: CChar.self) },
          err_len: bufs[2 * i + 1].count))
    }
    return rs.withUnsafeBufferPointer { ub_parse_plan(box.ptr, $0.baseAddress, Int32($0.count), now) }
  }
}

// ------------------------------------------------------------------ view

func attrs(_ f: NSFont, _ c: NSColor) -> [NSAttributedString.Key: Any] {
  [.font: f, .foregroundColor: c]
}
func textH(_ s: String, _ a: [NSAttributedString.Key: Any], _ w: CGFloat) -> CGFloat {
  ceil(
    (s as NSString).boundingRect(
      with: NSSize(width: w, height: 1e4), options: .usesLineFragmentOrigin, attributes: a
    ).height)
}
func drawRight(_ s: String, _ a: [NSAttributedString.Key: Any], _ rightX: CGFloat, _ y: CGFloat) {
  let sz = (s as NSString).size(withAttributes: a)
  (s as NSString).draw(at: NSPoint(x: rightX - sz.width, y: y), withAttributes: a)
}
func fillRound(_ r: NSRect, _ rad: CGFloat, _ c: NSColor) {
  c.setFill()
  NSBezierPath(roundedRect: r, xRadius: rad, yRadius: rad).fill()
}
func separator(_ y: CGFloat, _ draw: Bool) {
  guard draw else { return }
  if let c = NSColor.windowBackgroundColor.blended(withFraction: 0.12, of: .labelColor) {
    c.setFill()
    NSBezierPath(rect: NSRect(x: kPad, y: y, width: kW - 2 * kPad, height: 1)).fill()
  }
}

func symbolImage(_ name: String, _ pt: CGFloat) -> NSImage {
  let img =
    NSImage(systemSymbolName: name, accessibilityDescription: nil)
    ?? NSImage(systemSymbolName: "circle", accessibilityDescription: nil)!
  return img.withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: pt, weight: .regular)) ?? img
}
func drawSymbol(_ name: String, _ pt: CGFloat, _ color: NSColor, _ center: NSPoint) {
  var img = symbolImage(name, pt)
  let cfg = NSImage.SymbolConfiguration(hierarchicalColor: color)
  img = img.withSymbolConfiguration(cfg) ?? img
  let s = img.size
  img.draw(in: NSRect(x: center.x - s.width / 2, y: center.y - s.height / 2, width: s.width, height: s.height))
}

// Real provider marks from Assets/*.svg (see Assets/README.md). Tab buttons
// use template copies tinted via contentTintColor; CoreGraphics rows use
// color-baked copies since direct drawing takes no tint.
let brandAssetNames = ["codex", "claude", "deepseek", "grok", "muse", "openrouter"]
var brandRawCache: [Int: NSImage] = [:]
var brandTemplateCache: [Int: NSImage] = [:]
var brandRowCache: [Int: (normal: NSImage, white: NSImage)] = [:]

func brandRaw(_ p: Int) -> NSImage? {
  if let c = brandRawCache[p] { return c }
  guard p >= 0, p < brandAssetNames.count,
    let url = Bundle.main.url(forResource: brandAssetNames[p], withExtension: "svg"),
    let img = NSImage(contentsOf: url)
  else { return nil }
  img.size = NSSize(width: 18, height: 18)
  img.isTemplate = false
  brandRawCache[p] = img
  return img
}

func brandTemplate(_ p: Int) -> NSImage? {
  if let c = brandTemplateCache[p] { return c }
  guard let raw = brandRaw(p), let t = raw.copy() as? NSImage else { return nil }
  t.isTemplate = true
  brandTemplateCache[p] = t
  return t
}

func tabIcon(_ p: Int) -> NSImage {
  if let b = brandTemplate(p) { return b }
  return symbolImage(provSymbol(p), 17)
}

func tintedCopy(_ src: NSImage, _ color: NSColor) -> NSImage {
  let size = NSSize(width: 18, height: 18)
  let c = NSImage(size: size)
  c.lockFocus()
  src.draw(in: NSRect(origin: .zero, size: size))
  color.set()
  NSRect(origin: .zero, size: size).fill(using: .sourceIn)
  c.unlockFocus()
  c.isTemplate = false
  return c
}

// Row icons keep today's per-provider colors (claude coral and muse blue stay
// brand-authentic); hover flips everything to white like a native menu row.
func rowBrandIcon(_ p: Int, highlighted: Bool) -> NSImage? {
  if let c = brandRowCache[p] { return highlighted ? c.white : c.normal }
  guard let raw = brandRaw(p) else { return nil }
  let normal: NSImage
  if p == 1 || p == 4 {
    normal = raw  // claude, muse: genuine brand colors
  } else {
    normal = tintedCopy(raw, rgb(provRGB(p)))
  }
  let white = tintedCopy(raw, .white)
  brandRowCache[p] = (normal, white)
  return highlighted ? white : normal
}

func drawBrandIcon(_ img: NSImage, _ box: CGFloat, _ center: NSPoint) {
  let s = img.size
  guard s.width > 0, s.height > 0 else { return }
  let k = box / max(s.width, s.height)
  let w = s.width * k, h = s.height * k
  img.draw(in: NSRect(x: center.x - w / 2, y: center.y - h / 2, width: w, height: h))
}

// Remaining-quota bar. Highlighted (menu-hover) rows draw white-on-accent,
// mirroring CodexBar's progressTrack/progressTint highlight styles.
func drawBar(_ r: NSRect, _ remaining: Double, _ color: UInt32, highlighted: Bool = false) {
  fillRound(r, r.size.height / 2, highlighted ? .white.withAlphaComponent(0.22) : .quaternaryLabelColor)
  if remaining > 0 {
    var f = r
    f.size.width = max(r.size.height, r.size.width * min(1.0, remaining / 100.0))
    fillRound(f, r.size.height / 2, highlighted ? .white : rgb(color))
  }
}

// Most constrained remaining quota (<0 when no quota windows), and the
// menu-bar text: "21%" / "$12.30" / "—" / "…". Mirrors core barText/remainingPct.
func snapRemaining(_ s: Snap) -> Double {
  var r = -1.0
  for w in s.windows where w.inBar && w.usedPct >= 0 {
    let left = max(0.0, 100.0 - w.usedPct)
    if r < 0 || left < r { r = left }
  }
  return r
}
func snapBarText(_ s: Snap) -> String {
  let r = snapRemaining(s)
  if r >= 0 { return "\(Int(floor(r + 0.0001)))%" }
  if !s.barText.isEmpty { return s.barText }
  return s.hasData ? "—" : "…"
}

enum HitKind { case row, refresh, key, login, quit }
struct Hit {
  var r: NSRect
  var k: HitKind
  var v: Int
}

class UBView: NSView {
  weak var app: AppDelegate?
  var hover = -1
  var hoverButton = -1
  var hits: [Hit] = []
  var tracking: NSTrackingArea?
  var providerButtons: [NSButton] = []

  override var isFlipped: Bool { true }
  override var acceptsFirstResponder: Bool { true }
  override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

  override func updateTrackingAreas() {
    if let t = tracking { removeTrackingArea(t) }
    let t = NSTrackingArea(
      rect: bounds,
      options: [.mouseMoved, .mouseEnteredAndExited, .activeAlways, .inVisibleRect], owner: self,
      userInfo: nil)
    addTrackingArea(t)
    tracking = t
    super.updateTrackingAreas()
  }

  func hitAt(_ p: NSPoint) -> Int {
    for (i, h) in hits.enumerated() { if h.r.contains(p) { return i } }
    return -1
  }
  override func mouseMoved(with event: NSEvent) {
    let p = convert(event.locationInWindow, from: nil)
    let h = hitAt(p)
    var hb = -1
    for (i, b) in providerButtons.enumerated() where b.frame.contains(p) { hb = i; break }
    if h != hover || hb != hoverButton {
      hover = h
      hoverButton = hb
      needsDisplay = true
    }
  }
  override func mouseExited(with event: NSEvent) {
    if hover != -1 || hoverButton != -1 {
      hover = -1
      hoverButton = -1
      needsDisplay = true
    }
  }
  override func mouseUp(with event: NSEvent) {
    let h = hitAt(convert(event.locationInWindow, from: nil))
    guard h >= 0, let a = app else { return }
    let hit = hits[h]
    switch hit.k {
    case .row: a.select(hit.v)
    case .refresh: a.refreshAll(interactive: true, force: true)
    case .key: a.setApiKeyFor(hit.v)
    case .login:
      a.toggleLogin()
      needsDisplay = true
    case .quit: NSApp.terminate(nil)
    }
  }
  override func keyDown(with event: NSEvent) {
    let c = event.charactersIgnoringModifiers?.lowercased() ?? ""
    if event.modifierFlags.contains(.command) && c == "r" {
      app?.refreshAll(interactive: true, force: true)
    } else if event.modifierFlags.contains(.command) && c == "q" {
      NSApp.terminate(nil)
    } else if event.keyCode == 53 {
      app?.hidePanel()  // escape closes the panel
    } else if event.keyCode == 123 || event.keyCode == 124 {  // left/right arrows cycle tabs
      guard let a = app else {
        super.keyDown(with: event)
        return
      }
      var t = a.selected + (event.keyCode == 124 ? 1 : -1)
      let n = Int(ub_provider_count())
      if t < kOverview { t = n - 1 }
      if t >= n { t = kOverview }
      a.select(t)
    } else {
      super.keyDown(with: event)
    }
  }

  override func draw(_ dirtyRect: NSRect) { _ = layoutDrawing(draw: true) }

  @objc func selectProvider(_ sender: NSButton) { app?.select(sender.tag) }

  func layoutDrawing(draw: Bool) -> CGFloat {
    guard let a = app else { return 0 }
    hits.removeAll()
    let now = ub_now_sec()
    let f11 = NSFont.systemFont(ofSize: 11)
    let f12 = NSFont.systemFont(ofSize: 12)
    let f13 = NSFont.systemFont(ofSize: 13)
    let fb13 = NSFont.systemFont(ofSize: 13, weight: .semibold)
    let numbers = NSFont.monospacedDigitSystemFont(ofSize: 13, weight: .semibold)
    let titleFont = NSFont.systemFont(ofSize: 15, weight: .semibold)
    let lab = NSColor.labelColor
    let sec = NSColor.secondaryLabelColor
    let n = Int(ub_provider_count())
    let cw = kW - 2 * kPad
    var y: CGFloat = 8

    // Native icon buttons keep every provider one click away in a single row.
    let count = n + 1
    let tw = cw / CGFloat(count)
    if providerButtons.isEmpty {
      for i in 0..<count {
        let tab = i - 1
        let name = tab == kOverview ? "Overview" : provName(tab)
        let img = tab == kOverview ? symbolImage("square.grid.2x2", 17) : tabIcon(tab)
        let b = NSButton(image: img, target: self, action: #selector(selectProvider(_:)))
        b.isBordered = false
        b.tag = tab
        b.toolTip = name
        b.setAccessibilityLabel(name)
        addSubview(b)
        providerButtons.append(b)
      }
    }
    for i in 0..<count {
      let b = providerButtons[i]
      b.frame = NSRect(x: kPad + CGFloat(i) * tw, y: y, width: tw, height: 40)
      let selected = b.tag == a.selected
      let hov = i == hoverButton
      // Hovered tab: solid accent pill + white glyph, like a native menu row.
      // Selected (not hovered): translucent tint + accent glyph.
      b.contentTintColor = hov ? .white : (selected ? .controlAccentColor : sec)
      b.setAccessibilityValue(selected ? "Selected" : "")
      if draw {
        if hov {
          fillRound(b.frame.insetBy(dx: 2, dy: 3), 6, .controlAccentColor)
        } else if selected {
          fillRound(b.frame.insetBy(dx: 2, dy: 3), 6, NSColor.controlAccentColor.withAlphaComponent(0.12))
        }
      }
    }
    y += 48
    separator(y, draw)
    y += 8

    if a.selected == kOverview {
      for p in 0..<n {
        let s = a.snaps[p]
        let row = NSRect(x: kPad - 6, y: y - 5, width: cw + 12, height: 40)
        hits.append(Hit(r: row, k: .row, v: p))
        if draw {
          // Native-menu-style hover: accent pill, everything flips to white,
          // progress bar included (CodexBar's selectionBackground/primary styles).
          let hi = hover == hits.count - 1
          if hi { fillRound(row, 6, .selectedContentBackgroundColor) }
          let fg = hi ? NSColor.selectedMenuItemTextColor : lab
          let fg2 = hi ? NSColor.selectedMenuItemTextColor : sec
          if let bi = rowBrandIcon(p, highlighted: hi) {
            drawBrandIcon(bi, 15, NSPoint(x: kPad + 8, y: y + 9))
          } else {
            drawSymbol(
              provSymbol(p), 13, hi ? .selectedMenuItemTextColor : rgb(provRGB(p)),
              NSPoint(x: kPad + 8, y: y + 9))
          }
          (provName(p) as NSString).draw(
            at: NSPoint(x: kPad + 22, y: y), withAttributes: attrs(fb13, fg))
          let bt = snapBarText(s)
          let rem = snapRemaining(s)
          var right = rem >= 0 ? "\(bt) left" : bt
          if !s.ok && !s.hasData { right = a.inflight[p] ? "Updating…" : "Unavailable" }
          drawRight(right, attrs(numbers, fg), kPad + cw - 16, y)
          drawSymbol("chevron.right", 9, fg2, NSPoint(x: kPad + cw - 3, y: y + 10))
          if rem >= 0 {
            drawBar(NSRect(x: kPad + 22, y: y + 22, width: cw - 22, height: 6), rem, provRGB(p),
              highlighted: hi)
          } else {
            let sub = s.hasData ? (s.notes.first ?? "") : s.error
            var subA = attrs(f11, fg2)
            let style = NSMutableParagraphStyle()
            style.lineBreakMode = .byTruncatingTail
            subA[.paragraphStyle] = style
            (sub as NSString).draw(
              with: NSRect(x: kPad + 22, y: y + 18, width: cw - 22, height: 16),
              options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine], attributes: subA)
          }
        }
        y += 40
      }
    } else {
      let p = a.selected
      let s = a.snaps[p]
      if draw {
        (provName(p) as NSString).draw(at: NSPoint(x: kPad, y: y), withAttributes: attrs(titleFont, lab))
        if a.inflight[p] { drawRight("Updating…", attrs(f12, sec), kPad + cw, y + 2) }
      }
      y += 26

      if !s.error.isEmpty {
        let ea = attrs(f12, .systemOrange)
        let h = textH(s.error, ea, cw)
        if draw {
          (s.error as NSString).draw(
            with: NSRect(x: kPad, y: y, width: cw, height: h), options: .usesLineFragmentOrigin,
            attributes: ea)
        }
        y += h + 8
      }
      if s.windows.isEmpty && !s.barText.isEmpty {
        if draw {
          ("Balance" as NSString).draw(at: NSPoint(x: kPad, y: y), withAttributes: attrs(fb13, lab))
          drawRight(s.barText, attrs(numbers, lab), kPad + cw, y - 1)
        }
        y += 24
      }
      for w in s.windows {
        let left = w.usedPct >= 0 ? max(0.0, 100.0 - w.usedPct) : -1
        if draw {
          (w.label as NSString).draw(at: NSPoint(x: kPad, y: y), withAttributes: attrs(fb13, lab))
          let value = left >= 0 ? "\(Int(floor(left + 1e-4)))% left" : "Unavailable"
          drawRight(value, attrs(numbers, lab), kPad + cw, y - 1)
        }
        y += 21
        if draw { drawBar(NSRect(x: kPad, y: y, width: cw, height: 6), left, provRGB(p)) }
        y += 10
        if w.resetAt > now {
          if draw {
            ("Resets in \(ownedStr(ub_fmt_duration_copy(w.resetAt - now)))" as NSString).draw(
              at: NSPoint(x: kPad, y: y), withAttributes: attrs(f12, sec))
          }
          y += 17
        }
        y += 8
      }
      if s.windows.isEmpty && s.barText.isEmpty && s.error.isEmpty {
        if draw {
          ("Loading…" as NSString).draw(at: NSPoint(x: kPad, y: y), withAttributes: attrs(f12, sec))
        }
        y += 24
      }
    }

    // ---- footer menu rows
    y += 8
    separator(y, draw)
    y += 8
    func row(_ title: String, _ shortcut: String?, _ k: HitKind, _ v: Int, _ check: Bool) {
      let r = NSRect(x: kPad - 6, y: y, width: cw + 12, height: 24)
      hits.append(Hit(r: r, k: k, v: v))
      if draw {
        let hi = hover == hits.count - 1
        if hi { fillRound(r, 6, .selectedContentBackgroundColor) }
        let fg = hi ? NSColor.selectedMenuItemTextColor : lab
        let fg2 = hi ? NSColor.selectedMenuItemTextColor : sec
        (title as NSString).draw(at: NSPoint(x: kPad, y: y + 4), withAttributes: attrs(f13, fg))
        if let shortcut { drawRight(shortcut, attrs(f13, fg2), kPad + cw, y + 4) }
        if check { drawRight("✓", attrs(f13, fg), kPad + cw, y + 4) }
      }
      y += 24
    }
    row("Refresh", "⌘R", .refresh, 0, false)
    if a.selected != kOverview && provNeedsKey(a.selected) {
      row("Set API Key…", nil, .key, a.selected, false)
    }
    row("Launch at Login", nil, .login, 0, a.loginEnabled())
    row("Quit CoffeeUsage", "⌘Q", .quit, 0, false)
    return y + 8
  }
}

// ------------------------------------------------------------------ app

class AppDelegate: NSObject, NSApplicationDelegate {
  var snaps: [Snap] = []
  var inflight: [Bool] = []
  var backoffUntil: [Int64] = []
  var backoffStep: [Int] = []
  var selected = kOverview
  var barPid = 1  // Claude

  var item: NSStatusItem!
  var panel: NSPanel!
  var view: UBView!
  var transport = Transport()
  var scheduler: NSBackgroundActivityScheduler?
  var outsideMonitor: Any?

  func applicationDidFinishLaunching(_ notification: Notification) {
    let n = Int(ub_provider_count())
    snaps = Array(repeating: .empty, count: n)
    inflight = Array(repeating: false, count: n)
    backoffUntil = Array(repeating: 0, count: n)
    backoffStep = Array(repeating: 0, count: n)
    let d = UserDefaults.standard
    d.register(defaults: ["tab": 1, "barProvider": 1])
    selected = min(max(d.integer(forKey: "tab"), kOverview), n - 1)
    barPid = min(max(d.integer(forKey: "barProvider"), 0), n - 1)

    item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    item.button?.imagePosition = .imageLeading
    item.button?.target = self
    item.button?.action = #selector(toggle(_:))

    view = UBView(frame: NSRect(x: 0, y: 0, width: kW, height: 400))
    view.app = self
    // Arrowless floating panel (CodexBar/Tailscale style): frosted material,
    // rounded corners, positioned under the status item. No NSPopover tail.
    let fx = NSVisualEffectView(frame: NSRect(x: 0, y: 0, width: kW, height: 400))
    fx.material = .popover
    fx.state = .active
    fx.wantsLayer = true
    fx.layer?.cornerRadius = 12
    // Square top corners: the panel docks flush under the menu bar like a
    // native menu; only the bottom corners stay round.
    fx.layer?.maskedCorners = [.layerMinXMinYCorner, .layerMaxXMinYCorner]
    fx.layer?.masksToBounds = true
    // Layer clipping alone doesn't shape AppKit's behind-window material.
    // Mask the material too, so its backing and window shadow match the panel.
    let mask = NSImage(size: NSSize(width: 26, height: 26), flipped: false) { rect in
      NSColor.black.setFill()
      NSBezierPath(roundedRect: rect, xRadius: 12, yRadius: 12).fill()
      NSBezierPath(rect: NSRect(x: 0, y: 12, width: 26, height: 14)).fill()
      return true
    }
    mask.capInsets = NSEdgeInsets(top: 12, left: 12, bottom: 12, right: 12)
    mask.resizingMode = .stretch
    fx.maskImage = mask
    fx.addSubview(view)
    let panel = NSPanel(
      contentRect: NSRect(x: 0, y: 0, width: kW, height: 400),
      styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
    panel.isOpaque = false
    panel.backgroundColor = .clear
    panel.hasShadow = true
    panel.level = .popUpMenu
    panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
    panel.title = "CoffeeUsage"
    panel.contentView = fx
    self.panel = panel

    updateBar()
    if getenv("COFFEEUSAGE_SNAPSHOT") != nil { return }
    scheduleBackground()
    fetch(barPid, interactive: false, force: true)

    let wc = NSWorkspace.shared.notificationCenter
    wc.addObserver(self, selector: #selector(didWake(_:)), name: NSWorkspace.didWakeNotification, object: nil)
    NotificationCenter.default.addObserver(
      self, selector: #selector(powerChanged(_:)),
      name: Notification.Name.NSProcessInfoPowerStateDidChange, object: nil)
  }

  // Coalesced, OS-managed background refresh of the menu bar provider only.
  func scheduleBackground() {
    scheduler?.invalidate()
    let low = ProcessInfo.processInfo.isLowPowerModeEnabled
    let s = NSBackgroundActivityScheduler(identifier: "local.usagebar.refresh")
    s.repeats = true
    s.interval = low ? 15 * 60 : 5 * 60
    s.tolerance = low ? 5 * 60 : 90
    s.qualityOfService = .utility
    s.schedule { [weak self] done in
      DispatchQueue.main.async { [weak self] in
        guard let self else { return }
        self.fetch(self.barPid, interactive: false, force: false)
      }
      done(.finished)
    }
    scheduler = s
  }
  @objc func powerChanged(_ n: Notification) {
    DispatchQueue.main.async { [weak self] in self?.scheduleBackground() }
  }
  @objc func didWake(_ n: Notification) {
    DispatchQueue.main.asyncAfter(deadline: .now() + 8) { [weak self] in
      guard let self else { return }
      if ub_now_sec() - self.snaps[self.barPid].updatedAt > 120 {
        self.fetch(self.barPid, interactive: false, force: false)
      }
    }
  }

  func updateBar() {
    guard let b = item.button else { return }
    // Same real brand mark as the tabs, as a monochrome template glyph.
    let img = brandTemplate(barPid) ?? symbolImage(provSymbol(barPid), 13)
    img.isTemplate = true
    if b.image !== img { b.image = img }
    let t = " " + snapBarText(snaps[barPid])
    if b.title != t {
      let f = NSFont.monospacedDigitSystemFont(ofSize: 13, weight: .regular)
      b.attributedTitle = NSAttributedString(string: t, attributes: [.font: f])
      // The item's width changes with the text; the menu bar lays that out on
      // the next pass (animated), so re-anchor after it lands and once more
      // after the animation settles. Right edge is stable across our own
      // width changes (verified with a status-item probe); this covers
      // neighbor movement and layout transients.
      if panel.isVisible {
        DispatchQueue.main.async { [weak self] in self?.placePanel() }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { [weak self] in
          if self?.panel.isVisible == true { self?.placePanel() }
        }
      }
    }
    b.toolTip = "\(provName(barPid)) usage"
  }

  func fetch(_ p: Int, interactive: Bool, force: Bool) {
    if inflight[p] { return }
    let now = ub_now_sec()
    if !force && now < backoffUntil[p] { return }
    inflight[p] = true
    if panel.isVisible { view.needsDisplay = true }
    guard let box = PlanBox(pid: p, interactive: interactive) else {
      inflight[p] = false
      return
    }
    transport.perform(box.requests) { [weak self] replies in
      // parse() folds plan.error into the snapshot, so the plan seed/error
      // travels with the result; no separate fallback needed here.
      let now2 = ub_now_sec()
      guard let snapPtr = parsePlan(box, replies, now: now2) else { return }
      defer { ub_snapshot_free(snapPtr) }
      var snap = readSnap(snapPtr)
      snap.updatedAt = now2
      if snap.ok { ub_learn_sample(Int32(p), snapPtr) }
      let status = replies.first?.status ?? 200
      DispatchQueue.main.async { [weak self] in
        self?.apply(snap, pid: p, status: status, authProblem: box.authProblem)
      }
    }
  }

  func apply(_ s: Snap, pid p: Int, status: Int, authProblem: Bool) {
    inflight[p] = false
    if s.ok || !snaps[p].hasData {
      snaps[p] = s
    } else {
      // Keep last good data, surface the error.
      snaps[p].ok = false
      snaps[p].error = s.error
      if !s.email.isEmpty { snaps[p].email = s.email }
    }
    // Back off on rate limits and rejected credentials to avoid pointless radio wakeups.
    let now = ub_now_sec()
    if status == 429 || status == 401 || status == 403 || authProblem {
      backoffStep[p] = min(backoffStep[p] + 1, 4)
      backoffUntil[p] = now + (Int64(15 * 60) << (backoffStep[p] - 1))
      if authProblem && status == 200 { backoffUntil[p] = now + 10 * 60 }  // local-only check, cheap
    } else if s.ok {
      backoffStep[p] = 0
      backoffUntil[p] = 0
    }
    if p == barPid { updateBar() }
    if panel.isVisible { relayout() }
  }

  func refreshAll(interactive: Bool, force: Bool) {
    let now = ub_now_sec()
    for i in 0..<snaps.count {
      if !force && now - snaps[i].updatedAt < 60 { continue }
      fetch(i, interactive: interactive, force: force)
    }
  }

  func relayout() {
    let h = view.layoutDrawing(draw: false)
    let sz = NSSize(width: kW, height: h)
    if view.frame.size != sz {
      view.setFrameSize(sz)
      panel.contentView?.setFrameSize(sz)
      panel.setContentSize(sz)
    }
    if panel.isVisible { placePanel() }  // keep the top edge anchored under the item
    view.needsDisplay = true
  }

  // Right-align the panel to the status item, clamped to the visible screen.
  // The bar text (and item width) animates through transients on every switch
  // and each re-sample of it can land on a different x, so horizontal placement
  // is computed once per opening and then frozen: while the panel is visible
  // its own frame is the anchor. (Height still re-anchors the top edge below.)
  func placePanel() {
    let panel = panel!
    let screen = item.button?.window?.screen ?? NSScreen.main
    let visible = screen?.visibleFrame ?? NSScreen.main!.visibleFrame
    let btn = item.button?.window?.frame ?? .zero  // screen coordinates
    var x = panel.frame.minX
    if !panel.isVisible {
      x = round(btn.maxX - kW)
      x = max(visible.minX + 4, min(x, visible.maxX - kW - 4))
    }
    // Flush with the menu bar (no gap), like a native menu.
    var y = btn.minY - panel.frame.size.height
    y = max(y, visible.minY)
    panel.setFrame(NSRect(x: x, y: y, width: kW, height: panel.frame.size.height), display: false)
  }

  func hidePanel() {
    panel.orderOut(nil)
    if let m = outsideMonitor {
      NSEvent.removeMonitor(m)
      outsideMonitor = nil
    }
    view.hover = -1
  }

  @objc func toggle(_ sender: Any?) {
    if panel.isVisible {
      hidePanel()
      return
    }
    relayout()
    placePanel()
    panel.orderFront(nil)
    panel.makeKey()
    view.window?.makeFirstResponder(view)
    // Transient behavior (what NSPopoverBehaviorTransient did): any click
    // outside the panel and off the status item dismisses it.
    outsideMonitor = NSEvent.addGlobalMonitorForEvents(
      matching: [.leftMouseDown, .rightMouseDown]
    ) { [weak self] _ in
      guard let self, self.panel.isVisible else { return }
      let m = NSEvent.mouseLocation
      if self.panel.frame.contains(m) { return }  // inside: the panel handles it
      if let f = self.item.button?.window?.frame, f.contains(m) { return }  // status item: toggle handles it
      self.hidePanel()
    }
    refreshAll(interactive: false, force: false)  // stale (>60s) providers only
  }

  func select(_ tab: Int) {
    selected = tab
    view.hover = -1
    view.hoverButton = -1
    UserDefaults.standard.set(tab, forKey: "tab")
    if tab != kOverview && tab != barPid {
      barPid = tab
      UserDefaults.standard.set(tab, forKey: "barProvider")
      updateBar()
    }
    relayout()
  }

  func setApiKeyFor(_ p: Int) {
    hidePanel()
    NSApp.activate(ignoringOtherApps: true)
    let al = NSAlert()
    al.messageText = "\(provName(p)) API Key"
    al.informativeText =
      "Stored in your login Keychain. Get one at \(provKeyHelp(p)). Leave empty to remove."
    let f = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 280, height: 24))
    al.accessoryView = f
    al.addButton(withTitle: "Save")
    al.addButton(withTitle: "Cancel")
    al.window.initialFirstResponder = f
    guard al.runModal() == .alertFirstButtonReturn else { return }
    let key = f.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
    _ = ub_set_api_key(Int32(p), key)
    snaps[p] = .empty
    backoffUntil[p] = 0
    fetch(p, interactive: false, force: true)
  }

  func loginEnabled() -> Bool { SMAppService.mainApp.status == .enabled }
  func toggleLogin() {
    do {
      if loginEnabled() {
        try SMAppService.mainApp.unregister()
      } else {
        try SMAppService.mainApp.register()
      }
    } catch {
      NSLog("Launch at login: %@", String(describing: error) as NSString)
    }
  }

}

// ------------------------------------------------------------------ CLI dump (for testing)

func dumpAll(interactive: Bool) -> Int32 {
  let transport = Transport()
  let n = Int(ub_provider_count())
  for i in 0..<n {
    guard let box = PlanBox(pid: i, interactive: interactive) else { continue }
    let sem = DispatchSemaphore(value: 0)
    var replies: [UReply] = []
    transport.perform(box.requests) { r in
      replies = r
      sem.signal()
    }
    sem.wait()
    let now = ub_now_sec()
    guard let snapPtr = parsePlan(box, replies, now: now) else { continue }
    defer { ub_snapshot_free(snapPtr) }
    let s = readSnap(snapPtr)
    if s.ok { ub_learn_sample(Int32(i), snapPtr) }
    print("== \(provName(i))  [bar: \(snapBarText(s))]")
    if !s.email.isEmpty || !s.plan.isEmpty { print("   account: \(s.email)  plan: \(s.plan)") }
    if !s.error.isEmpty { print("   error: \(s.error)") }
    for (wi, w) in s.windows.enumerated() {
      let resets =
        w.resetAt > 0 ? ownedStr(ub_fmt_duration_copy(w.resetAt - now)) as NSString : "?" as NSString
      print(
        String(
          format: "   %-14@ used %5.1f%%  resets in %@", w.label as NSString, w.usedPct, resets))
      let pl = ownedStr(ub_pace_line_copy(Int32(i), snapPtr, Int32(wi), now))
      if !pl.isEmpty { print("      \(pl)") }
    }
    for note in s.notes { print("   \(note)") }
  }
  return 0
}

@main
struct CoffeeUsageMain {
  static func main() {
    let args = CommandLine.arguments
    if args.count > 1 {
      if args[1] == "--dump" {
        let interactive = args.count > 2 && args[2] == "--interactive"
        exit(dumpAll(interactive: interactive))
      }
      if args[1] == "--import-env-keys" {  // copy DEEPSEEK_API_KEY / OPENROUTER_API_KEY into Keychain
        if let ds = getenv("DEEPSEEK_API_KEY"), String(cString: ds).isEmpty == false {
          print("DeepSeek: \(ub_set_api_key(2, ds) != 0 ? "saved" : "failed")")
        }
        if let orr = getenv("OPENROUTER_API_KEY"), String(cString: orr).isEmpty == false {
          print("OpenRouter: \(ub_set_api_key(5, orr) != 0 ? "saved" : "failed")")
        }
        exit(0)
      }
    }
    if getenv("COFFEEUSAGE_SNAPSHOT") != nil {  // testing aid: render panel tabs to PNGs
      let base = String(cString: getenv("COFFEEUSAGE_SNAPSHOT")!)
      DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
        let a = NSApp.delegate as! AppDelegate
        let now = ub_now_sec()
        for p in 0..<a.snaps.count {
          a.snaps[p] = Snap(
            ok: true, hasData: true, email: "alex.with.a.very.long.account.name.for.preview@example.com",
            plan: "Pro",
            windows: [
              Win(label: "Session", usedPct: 24, resetAt: now + 7200, durationSec: 18000),
              Win(label: "Weekly", usedPct: 61, resetAt: now + 259200, durationSec: 604800),
            ], updatedAt: now - 120)
        }
        a.snaps[1].windows = [
          Win(label: "Session", usedPct: 100, resetAt: now + 7200, durationSec: 18000),
          Win(label: "Weekly", usedPct: 0, resetAt: 0, durationSec: 604800),
        ]
        a.snaps[4] = .empty
        a.inflight[4] = true
        a.snaps[3].ok = false
        a.snaps[2] = Snap(error: "Add an API key to see your available balance.", updatedAt: now - 120)
        a.snaps[3].error = "Could not refresh usage. Showing the last available data."
        a.snaps[5].windows = []
        a.snaps[5].barText = "$12.30"
        a.snaps[5].notes = ["Credit balance available for API requests."]
        for tab in [kOverview, 0, 1, 2, 3, 4, 5] {
          for appName: NSAppearance.Name in [.aqua, .darkAqua] {
            a.selected = tab
            let v = a.view!
            // Fresh buttons per capture: reused NSButtonCells can keep a cached
            // tint from the previous appearance in this offscreen pass.
            for b in v.providerButtons { b.removeFromSuperview() }
            v.providerButtons = []
            v.appearance = NSAppearance(named: appName)
            let h = v.layoutDrawing(draw: false)
            v.setFrameSize(NSSize(width: kW, height: h))
            guard let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds) else { continue }
            v.appearance?.performAsCurrentDrawingAppearance {
              NSGraphicsContext.saveGraphicsState()
              NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
              NSColor.windowBackgroundColor.setFill()
              NSBezierPath(
                rect: NSRect(x: 0, y: 0, width: rep.pixelsWide, height: rep.pixelsHigh)
              ).fill()
              NSGraphicsContext.restoreGraphicsState()
              v.cacheDisplay(in: v.bounds, to: rep)
            }
            let path = "\(base)-\(tab + 1)-\(appName == .aqua ? "light" : "dark").png"
            if let png = rep.representation(using: .png, properties: [:]) {
              try? png.write(to: URL(fileURLWithPath: path))
            }
          }
        }
        NSApp.terminate(nil)
      }
    }
    if getenv("COFFEEUSAGE_SHOW") != nil {  // testing aid: open the panel on launch
      DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
        (NSApp.delegate as! AppDelegate).toggle(nil)
      }
    }
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    let d = AppDelegate()
    app.delegate = d
    app.run()
  }
}
