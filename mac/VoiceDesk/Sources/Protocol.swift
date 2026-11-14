import Foundation

enum Lan {
    static let magic: UInt16 = 0xC3A1
    static let hello: UInt8 = 1
    static let helloAck: UInt8 = 2
    static let pcm: UInt8 = 0x10
    static let ctrl: UInt8 = 0x11
    /// Mac → 徽章的字幕。载荷：角色 u8（0 清空，1 主人，2 小克，3 后台任务）+ UTF-8 全文（整段替换）。
    static let text: UInt8 = 0x12
    /// Mac → 徽章的字幕位图（A4，只发变了的行）。徽章在 HELLO 第 6 字节 bit0 声明支持。
    static let subimg: UInt8 = 0x13
    static let frameBytes = 640
    static let header = 10
}

struct LanFrame {
    var kind: UInt8
    var flags: UInt8
    var seq: UInt32
    var payload: Data

    func pack() -> Data {
        var out = Data(count: Lan.header + payload.count)
        out[0] = UInt8(Lan.magic & 0xFF)
        out[1] = UInt8(Lan.magic >> 8)
        out[2] = kind
        out[3] = flags
        let s = seq.littleEndian
        withUnsafeBytes(of: s) { out.replaceSubrange(4..<8, with: $0) }
        let n = UInt16(payload.count).littleEndian
        withUnsafeBytes(of: n) { out.replaceSubrange(8..<10, with: $0) }
        out.replaceSubrange(10..., with: payload)
        return out
    }

    var name: String {
        switch kind {
        case Lan.hello: return "HELLO"
        case Lan.helloAck: return "HELLO_ACK"
        case Lan.pcm: return "PCM"
        case Lan.ctrl: return "CTRL"
        case Lan.text: return "TEXT"
        case Lan.subimg: return "SUBIMG"
        default: return String(format: "0x%02X", kind)
        }
    }
}

final class FrameParser {
    private var buf = [UInt8]()

    func feed(_ data: Data) -> [LanFrame] {
        buf.append(contentsOf: data)
        var out: [LanFrame] = []
        while buf.count >= Lan.header {
            let magic = UInt16(buf[0]) | (UInt16(buf[1]) << 8)
            if magic != Lan.magic {
                buf.removeFirst()
                continue
            }
            let nbytes = Int(UInt16(buf[8]) | (UInt16(buf[9]) << 8))
            let total = Lan.header + nbytes
            if nbytes > 8192 {
                buf.removeFirst()
                continue
            }
            if buf.count < total { break }
            let payload = Data(buf[Lan.header..<total])
            let seq = UInt32(buf[4]) | (UInt32(buf[5]) << 8) | (UInt32(buf[6]) << 16) | (UInt32(buf[7]) << 24)
            out.append(LanFrame(kind: buf[2], flags: buf[3], seq: seq, payload: payload))
            buf.removeFirst(total)
        }
        return out
    }

    func reset() { buf.removeAll(keepingCapacity: true) }
}
