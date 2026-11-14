import Foundation

/// 和徽章的那条 TCP。整帧在锁里写完，两个线程同时发也不会把字节交错在一起。
final class BadgeLink {
    private let lock = NSLock()
    private var fd: Int32 = -1
    private var seq: UInt32 = 1

    func attach(_ newFD: Int32) {
        lock.lock()
        fd = newFD
        seq = 1
        lock.unlock()
    }

    /// 摘下当前连接，返回旧的文件描述符（由调用方关闭）。
    func detach() -> Int32 {
        lock.lock()
        let old = fd
        fd = -1
        lock.unlock()
        return old
    }

    var connected: Bool {
        lock.lock()
        defer { lock.unlock() }
        return fd >= 0
    }

    @discardableResult
    func send(kind: UInt8, payload: Data) -> Bool {
        sendBatch(kind: kind, payloads: [payload])
    }

    /// 几帧拼成一次写入：Wi-Fi 上少几个小包，徽章那边少占几块接收缓冲。
    @discardableResult
    func sendBatch(kind: UInt8, payloads: [Data]) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        guard fd >= 0, !payloads.isEmpty else { return false }
        var bytes = Data()
        for p in payloads {
            bytes.append(LanFrame(kind: kind, flags: 0, seq: seq, payload: p).pack())
            seq &+= 1
        }
        var ok = true
        bytes.withUnsafeBytes { raw in
            guard let base = raw.baseAddress else { return }
            var off = 0
            while off < raw.count {
                let n = Darwin.send(fd, base.advanced(by: off), raw.count - off, 0)
                if n > 0 {
                    off += n
                } else if n < 0 && errno == EINTR {
                    continue
                } else {
                    ok = false
                    break
                }
            }
            // 写了半帧就超时：字节流已经错位，直接断开让徽章重连，比让它从乱流里找帧头可靠。
            if !ok && off > 0 { shutdown(fd, SHUT_RDWR) }
        }
        return ok
    }
}

/// 按实时速度把模型的声音送给徽章，只提前 lead 秒。
///
/// 以前是一收到就整段塞进套接字：徽章几秒后才念完，Mac 这边却早就以为说完了，
/// 阶段提前回到空闲、暂停也停不住。现在 Mac 知道徽章大约念到哪里（played），
/// 字幕按这个进度逐字显示，暂停时只需丢掉还没发的帧。
final class Pacer {
    struct Status {
        var played: Double
        var received: Double
        var playing: Bool
        var level: Double
    }

    static let frameSeconds = 0.02
    let lead = 0.16
    var output: (([Data]) -> Void)?

    private let lock = NSLock()
    private let q = DispatchQueue(label: "voice.desk.pacer", qos: .userInteractive)
    private var timer: DispatchSourceTimer?
    private var frames: [Data] = []
    private var head = 0
    private var sent = 0.0
    private var played = 0.0
    private var received = 0.0
    private var level = 0.0
    private var lastTick = Date()

    func start() {
        let t = DispatchSource.makeTimerSource(queue: q)
        // 40 ms 一拍：通常一次发两帧（拼成一个 TCP 包）。
        t.schedule(deadline: .now(), repeating: .milliseconds(40), leeway: .milliseconds(3))
        t.setEventHandler { [weak self] in self?.tick() }
        t.resume()
        timer = t
    }

    func push(_ newFrames: [Data]) {
        guard !newFrames.isEmpty else { return }
        lock.lock()
        frames.append(contentsOf: newFrames)
        received += Double(newFrames.count) * Self.frameSeconds
        lock.unlock()
    }

    /// 丢掉还没发出去的声音。已经在路上的那 lead 秒会照常念完。
    func cancel() {
        lock.lock()
        frames.removeAll()
        head = 0
        received = sent
        lock.unlock()
    }

    func status() -> Status {
        lock.lock()
        defer { lock.unlock() }
        return Status(played: played, received: received,
                      playing: head < frames.count || played < sent - 0.001, level: level)
    }

    private func tick() {
        let now = Date()
        var out: [Data] = []
        lock.lock()
        let dt = now.timeIntervalSince(lastTick)
        lastTick = now
        if played < sent { played = min(sent, played + dt) }
        while head < frames.count && sent < played + lead {
            out.append(frames[head])
            head += 1
            sent += Self.frameSeconds
        }
        if head > 256 && head * 2 > frames.count {
            frames.removeFirst(head)
            head = 0
        }
        if head >= frames.count && head > 0 {
            frames.removeAll(keepingCapacity: true)
            head = 0
        }
        if out.isEmpty && played >= sent { level *= 0.75 }
        lock.unlock()

        if !out.isEmpty { output?(out) }
        if let last = out.last {
            let rms = pcmRMS(last)
            lock.lock()
            level = rms
            lock.unlock()
        }
    }
}
