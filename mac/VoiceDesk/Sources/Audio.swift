import Foundation

func pcmRMS(_ pcm: Data) -> Double {
    let n = pcm.count / 2
    if n == 0 { return 0 }
    var acc = 0.0
    pcm.withUnsafeBytes { raw in
        let p = raw.bindMemory(to: UInt8.self)
        for i in 0..<n {
            let u = UInt16(p[i * 2]) | (UInt16(p[i * 2 + 1]) << 8)
            let s = Int16(bitPattern: u)
            acc += Double(s) * Double(s)
        }
    }
    return (acc / Double(n)).squareRoot() / 32768.0
}

/// 线性重采样。16 kHz 和 24 kHz 之间够用，状态跨帧保留。
final class Resampler {
    let srcRate: Double
    let dstRate: Double
    private var frac = 0.0
    private var last: Int16 = 0
    private var primed = false

    init(src: Double, dst: Double) {
        srcRate = src
        dstRate = dst
    }

    func reset() {
        frac = 0
        last = 0
        primed = false
    }

    func push(_ pcm: Data) -> Data {
        let n = pcm.count / 2
        if n == 0 || dstRate <= 0 { return Data() }
        var samples = [Int16](repeating: 0, count: n)
        pcm.withUnsafeBytes { raw in
            let p = raw.bindMemory(to: UInt8.self)
            for i in 0..<n {
                let u = UInt16(p[i * 2]) | (UInt16(p[i * 2 + 1]) << 8)
                samples[i] = Int16(bitPattern: u)
            }
        }
        if !primed {
            last = samples[0]
            primed = true
        }
        let step = srcRate / dstRate
        var pos = frac
        var out: [Int16] = []
        out.reserveCapacity(Int(Double(n) / step) + 4)
        while Int(pos) < n {
            let idx = Int(pos)
            let a = idx == 0 ? last : samples[idx - 1]
            let b = samples[idx]
            let t = pos - Double(idx)
            let v = Double(a) + (Double(b) - Double(a)) * t
            out.append(Int16(max(-32768, min(32767, v.rounded()))))
            pos += step
        }
        frac = pos - Double(n)
        last = samples[n - 1]
        var data = Data(count: out.count * 2)
        for (i, s) in out.enumerated() {
            let u = UInt16(bitPattern: s)
            data[i * 2] = UInt8(u & 0xFF)
            data[i * 2 + 1] = UInt8(u >> 8)
        }
        return data
    }
}

final class Slicer {
    // 注意：别对 removeFirst 之后的 Data 调 removeAll(keepingCapacity:)，
    // Foundation 会在 InlineSlice 上断言崩溃（2026-11-14 实测）。一律换成新的 Data。
    private var buf = Data()

    func reset() { buf = Data() }

    /// 取出不足一帧的尾巴（调用方补零）。
    func flush() -> Data {
        let rest = Data(buf)
        buf = Data()
        return rest
    }

    func push(_ pcm: Data) -> [Data] {
        buf.append(pcm)
        var frames: [Data] = []
        var off = 0
        let bytes = Data(buf)
        while bytes.count - off >= Lan.frameBytes {
            frames.append(bytes.subdata(in: off..<(off + Lan.frameBytes)))
            off += Lan.frameBytes
        }
        buf = off > 0 ? bytes.subdata(in: off..<bytes.count) : bytes
        return frames
    }
}
