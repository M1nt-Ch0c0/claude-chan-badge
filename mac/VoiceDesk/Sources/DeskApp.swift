import AppKit
import SwiftUI

@main
struct VoiceDeskApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var app

    var body: some Scene {
        Window("小克", id: "main") {
            WebContainer(host: app.host)
                .ignoresSafeArea()
                .frame(minWidth: 920, minHeight: 620)
        }
        .windowStyle(.hiddenTitleBar)
        .defaultSize(width: 1200, height: 780)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    let model = DeskModel()
    lazy var engine = VoiceEngine(model: model)
    lazy var host: WebHost = {
        let h = WebHost(model: model)
        h.onCommand = { [weak self] cmd in self?.engine.handle(cmd) }
        return h
    }()

    func applicationDidFinishLaunching(_ note: Notification) {
        NSApp.setActivationPolicy(.regular)
        if let url = Bundle.module.url(forResource: "ui/img/cheer", withExtension: "png"),
           let img = NSImage(contentsOf: url) {
            NSApp.applicationIconImage = Self.roundIcon(img)
        }
        engine.start()
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    /// 程序坞图标：暖色圆角底 + 立绘头部。
    private static func roundIcon(_ art: NSImage) -> NSImage {
        let size = NSSize(width: 512, height: 512)
        return NSImage(size: size, flipped: false) { rect in
            let inset = rect.insetBy(dx: 40, dy: 40)
            let path = NSBezierPath(roundedRect: inset, xRadius: 96, yRadius: 96)
            NSGradient(colors: [NSColor(red: 1, green: 0.86, blue: 0.72, alpha: 1),
                                NSColor(red: 1, green: 0.62, blue: 0.40, alpha: 1)])?.draw(in: path, angle: -90)
            path.addClip()
            let w = inset.width * 1.25
            let h = w * art.size.height / art.size.width
            art.draw(in: NSRect(x: inset.midX - w / 2, y: inset.minY - h * 0.28, width: w, height: h))
            return true
        }
    }
}

struct WebContainer: NSViewRepresentable {
    let host: WebHost

    func makeNSView(context: Context) -> NSView {
        let v = WindowHook()
        host.webView.frame = v.bounds
        host.webView.autoresizingMask = [.width, .height]
        v.addSubview(host.webView)
        return v
    }

    func updateNSView(_ nsView: NSView, context: Context) {}
}

/// 拿到窗口后把标题栏做成透明、背景色和页面一致，避免启动时闪白。
final class WindowHook: NSView {
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        guard let w = window else { return }
        w.titlebarAppearsTransparent = true
        w.titleVisibility = .hidden
        w.styleMask.insert(.fullSizeContentView)
        w.isMovableByWindowBackground = false
        w.backgroundColor = NSColor(name: nil) { appearance in
            appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
                ? NSColor(red: 0.075, green: 0.059, blue: 0.051, alpha: 1)
                : NSColor(red: 0.984, green: 0.961, blue: 0.933, alpha: 1)
        }
    }
}
