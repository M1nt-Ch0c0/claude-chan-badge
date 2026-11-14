import AppKit
import CoreText

/// 在 Mac 上把字幕排好版，渲染成徽章能直接贴的 4 位灰度位图。
///
/// 徽章就不用带字库、不用排版，也不占字形缓存（2026-11-14 字形缓存把徽章内存撑满过）。
/// 只显示最后几行：小克说得长了，旧的行从上面滚出去。
enum SubtitleRenderer {
    static let width = 200

    struct Bitmap: Equatable {
        var width: Int
        var height: Int
        var a4: [UInt8]          // 每行 width/2 字节，高 4 位是左边的像素
    }

    /// role：1 主人，2 小克，3 后台任务（小一号字、两行）。
    static func render(_ text: String, role: Int) -> Bitmap {
        let small = role == 3
        let fontSize: CGFloat = small ? 14 : 16
        let pitch = small ? 19 : 21
        let maxLines = small ? 2 : 3
        let font = CTFontCreateWithName("PingFang SC" as CFString, fontSize, nil)
        let para = NSMutableParagraphStyle()
        para.lineBreakMode = .byWordWrapping
        let attr = NSAttributedString(string: text, attributes: [
            .font: font,
            .foregroundColor: NSColor.white,
            .paragraphStyle: para,
        ])
        let setter = CTFramesetterCreateWithAttributedString(attr)
        let path = CGPath(rect: CGRect(x: 0, y: 0, width: CGFloat(width - 4), height: 10_000), transform: nil)
        let frame = CTFramesetterCreateFrame(setter, CFRange(location: 0, length: 0), path, nil)
        var lines = (CTFrameGetLines(frame) as? [CTLine]) ?? []
        if lines.count > maxLines { lines = Array(lines.suffix(maxLines)) }
        // 高度跟着行数走，徽章那边底边固定，新的一行从下面长出来，不会整段上下跳。
        let height = pitch * max(1, lines.count) + 3

        var gray = [UInt8](repeating: 0, count: width * height)
        gray.withUnsafeMutableBytes { raw in
            guard let ctx = CGContext(data: raw.baseAddress, width: width, height: height, bitsPerComponent: 8,
                                      bytesPerRow: width, space: CGColorSpaceCreateDeviceGray(),
                                      bitmapInfo: CGImageAlphaInfo.none.rawValue) else { return }
            ctx.setAllowsFontSmoothing(false)
            ctx.setShouldAntialias(true)
            ctx.setFillColor(gray: 1, alpha: 1)
            let top = 1
            let ascent = CTFontGetAscent(font)
            let descent = CTFontGetDescent(font)
            for (i, line) in lines.enumerated() {
                let w = CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil))
                // 行尾空白不算宽度，居中更准
                let trailing = CGFloat(CTLineGetTrailingWhitespaceWidth(line))
                let x = max(0, (CGFloat(width) - (w - trailing)) / 2).rounded()
                let rowTop = CGFloat(top + i * pitch)
                let baselineFromTop = rowTop + (CGFloat(pitch) - (ascent + descent)) / 2 + ascent
                ctx.textPosition = CGPoint(x: x, y: (CGFloat(height) - baselineFromTop).rounded())
                CTLineDraw(line, ctx)
            }
        }

        var a4 = [UInt8](repeating: 0, count: width / 2 * height)
        for i in 0..<a4.count {
            let l = Int(gray[i * 2]), r = Int(gray[i * 2 + 1])
            a4[i] = UInt8(((l * 15 + 127) / 255) << 4 | ((r * 15 + 127) / 255))
        }
        return Bitmap(width: width, height: height, a4: a4)
    }

    /// 和上一次发过的比，只打包变了的行。每块最多 maxRows 行，最后一块带提交标志。
    /// 载荷：角色 u8、标志 u8（bit0 提交）、宽 u8、高 u8、起始行 u8、行数 u8、A4 像素。
    static func chunks(_ bmp: Bitmap?, role: Int, previous: Bitmap?, maxRows: Int = 28) -> [Data] {
        guard let bmp, role != 0 else { return [Data([0, 1, 0, 0, 0, 0])] }
        let stride = bmp.width / 2
        var first = 0
        var last = bmp.height - 1
        if let prev = previous, prev.width == bmp.width, prev.height == bmp.height {
            func same(_ y: Int) -> Bool {
                prev.a4[(y * stride)..<((y + 1) * stride)] == bmp.a4[(y * stride)..<((y + 1) * stride)]
            }
            while first <= last && same(first) { first += 1 }
            while last >= first && same(last) { last -= 1 }
            if first > last {
                // 像素没变（只是换了角色颜色）：发一个空块提交
                return [Data([UInt8(role), 1, UInt8(bmp.width), UInt8(bmp.height), 0, 0])]
            }
        }
        var out: [Data] = []
        var y = first
        while y <= last {
            let rows = min(maxRows, last - y + 1)
            var d = Data([UInt8(role), y + rows > last ? 1 : 0, UInt8(bmp.width), UInt8(bmp.height), UInt8(y), UInt8(rows)])
            d.append(contentsOf: bmp.a4[(y * stride)..<((y + rows) * stride)])
            out.append(d)
            y += rows
        }
        return out
    }
}
