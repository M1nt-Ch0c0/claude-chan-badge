import AppKit
import WebKit

/// 网页画界面，Swift 只管状态和命令。
/// 开发时设 VOICEDESK_UI_DIR=Sources/Resources/ui 可以直接改 HTML、刷新窗口（⌘R）。
final class WebHost: NSObject, WKScriptMessageHandler, WKNavigationDelegate {
    let webView: DeskWebView
    let model: DeskModel
    var onCommand: (([String: Any]) -> Void)?
    private var timer: Timer?
    private var ready = false
    private var lastIn = -1.0
    private var lastOut = -1.0

    init(model: DeskModel) {
        self.model = model
        let cfg = WKWebViewConfiguration()
        let ucc = WKUserContentController()
        cfg.userContentController = ucc
        webView = DeskWebView(frame: NSRect(x: 0, y: 0, width: 1180, height: 760), configuration: cfg)
        super.init()
        ucc.add(WeakHandler(self), name: "desk")
        webView.navigationDelegate = self
        webView.setValue(false, forKey: "drawsBackground")
        webView.isInspectable = true
        load()
        let t = Timer(timeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in self?.pump() }
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    func load() {
        ready = false
        let dir: URL
        if let dev = ProcessInfo.processInfo.environment["VOICEDESK_UI_DIR"], !dev.isEmpty {
            dir = URL(fileURLWithPath: dev, isDirectory: true)
        } else if let res = Bundle.module.url(forResource: "ui", withExtension: nil) {
            dir = res
        } else {
            return
        }
        webView.loadFileURL(dir.appendingPathComponent("index.html"), allowingReadAccessTo: dir)
    }

    private func pump() {
        guard ready else { return }
        if model.dirty, let json = model.takeSnapshot() {
            webView.evaluateJavaScript("window.desk&&desk.state(\(json))", completionHandler: nil)
        }
        let i = model.inLevel
        let o = model.outLevel
        if abs(i - lastIn) > 0.001 || abs(o - lastOut) > 0.001 {
            lastIn = i
            lastOut = o
            webView.evaluateJavaScript(String(format: "window.desk&&desk.levels(%.4f,%.4f)", i, o), completionHandler: nil)
        }
    }

    func userContentController(_ ucc: WKUserContentController, didReceive message: WKScriptMessage) {
        guard let body = message.body as? [String: Any], let cmd = body["cmd"] as? String else { return }
        switch cmd {
        case "ready":
            ready = true
            model.mutate { _ in }
        case "dragZone":
            if let right = body["right"] as? Double { webView.noDragRight = CGFloat(right) }
            if let height = body["height"] as? Double { webView.dragHeight = CGFloat(height) }
        case "reload":
            load()
        default:
            onCommand?(body)
        }
    }

    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        // 页面里的外链交给浏览器，不在窗口里跳转。
        if let url = action.request.url, ["http", "https"].contains(url.scheme ?? "") {
            NSWorkspace.shared.open(url)
            decisionHandler(.cancel)
            return
        }
        decisionHandler(.allow)
    }
}

/// 隐藏标题栏后，顶部这条用来拖窗口；右侧 noDragRight 宽度内留给页面里的按钮。
final class DeskWebView: WKWebView {
    var dragHeight: CGFloat = 56
    var noDragRight: CGFloat = 260

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        let fromTop = isFlipped ? p.y : bounds.height - p.y
        if fromTop < dragHeight && p.x > 80 && p.x < bounds.width - noDragRight {
            if event.clickCount == 2 {
                window?.performZoom(nil)
            } else {
                window?.performDrag(with: event)
            }
            return
        }
        super.mouseDown(with: event)
    }

    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        if event.modifierFlags.contains(.command), event.charactersIgnoringModifiers == "r",
           ProcessInfo.processInfo.environment["VOICEDESK_UI_DIR"] != nil {
            reload()
            return true
        }
        return super.performKeyEquivalent(with: event)
    }
}

private final class WeakHandler: NSObject, WKScriptMessageHandler {
    weak var target: WKScriptMessageHandler?
    init(_ target: WKScriptMessageHandler) { self.target = target }
    func userContentController(_ ucc: WKUserContentController, didReceive message: WKScriptMessage) {
        target?.userContentController(ucc, didReceive: message)
    }
}
