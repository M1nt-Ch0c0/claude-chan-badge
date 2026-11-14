import AppKit
import Foundation

/// 徽章 ⇄ Mac ⇄ Realtime（OpenAI 兼容接口）的中枢。
///
/// 线程：
/// - `queue`：Realtime 事件、工具调用、字幕和阶段（每 50 ms 一次 tick）。
/// - `tcpQueue`：徽章套接字的阻塞读循环。不能和 `queue` 合并，否则读循环不返回，事件永远跑不到。
/// - `Pacer` 自己的定时器：按实时速度把声音发给徽章。
final class VoiceEngine {
    let model: DeskModel
    let agent = AgentRunner()

    private let queue = DispatchQueue(label: "voice.desk")
    private let tcpQueue = DispatchQueue(label: "voice.desk.tcp")
    private let link = BadgeLink()
    private let pacer = Pacer()
    private let parser = FrameParser()
    private let up = Resampler(src: 16000, dst: 24000)
    private let down = Resampler(src: 24000, dst: 16000)
    private let slicer = Slicer()
    private var listenFD: Int32 = -1
    private var connFD: Int32 = -1          // 只在 tcpQueue 上用
    private var running = true
    private var tickTimer: DispatchSourceTimer?

    // 以下只在 queue 上读写
    private var ws: URLSessionWebSocketTask?
    private var wsGen = 0
    private var sessionReady = false
    private var micQueue: [Data] = []
    private var micSending = false
    private var paused = false
    private var micMuted = false
    private var speakerMuted = false
    private var voice: String
    private var agentModel: String
    private var agentLevel: String
    private var badgePhase = -1
    private var lastPlaying = Date.distantPast
    private var userSpeaking = false
    private var awaitingSince: Date?
    private var responseActive = false
    private var pendingOutputs: [(callId: String, output: String)] = []
    private var quietAfterTools = false
    private var onlyAgentStarts = true
    private var responseSpoke = false
    private var announcements: [(taskId: Int, text: String)] = []
    private var reportedTasks: Set<Int> = []
    private var userTurns: [String: Int] = [:]
    private var userTexts: [String: String] = [:]
    private var speeches: [Speech] = []
    private var caption = Caption(role: 0, text: "")
    private var captionAt = Date()
    private var shownCaption: Caption?
    private var badgeCaption: Caption?
    private var badgeCaptionAt = Date.distantPast
    private var badgeBitmap = false                         // 徽章能收字幕位图
    private var sentBitmap: SubtitleRenderer.Bitmap?
    private var runningTasks: [Int: AgentTask] = [:]

    private struct Speech {
        var itemId: String
        var turnId: Int
        var text = ""
        var audioStart: Double?
        var audioEnd: Double?
        var textDone = false
        var audioDone = false
        var shown = 0
    }

    init(model: DeskModel) {
        self.model = model
        voice = model.voice
        agentModel = model.agentModel
        agentLevel = model.agentLevel
        model.agentReady = agent.ready
    }

    func start() {
        signal(SIGPIPE, SIG_IGN)
        pacer.output = { [weak self] frames in self?.sendPCM(frames) }
        pacer.start()
        agent.onUpdate = { [weak self] task in
            guard let self else { return }
            self.model.mutate { $0.upsertTask(task) }
            self.queue.async {
                if task.status == "running" { self.runningTasks[task.id] = task } else { self.runningTasks[task.id] = nil }
            }
        }
        agent.onFinish = { [weak self] task in
            self?.queue.async { self?.taskFinished(task) }
        }
        if !agent.ready { model.log("助手", "找不到 claude 命令，后台任务不可用") }
        queue.async { [weak self] in
            guard let self else { return }
            self.listen()
            self.connectRealtime()
            self.tcpQueue.async { self.loop() }
            let t = DispatchSource.makeTimerSource(queue: self.queue)
            t.schedule(deadline: .now() + 0.05, repeating: .milliseconds(50))
            t.setEventHandler { [weak self] in self?.tick() }
            t.resume()
            self.tickTimer = t
        }
    }

    // MARK: - 界面命令（主线程调用）

    func handle(_ cmd: [String: Any]) {
        let value = cmd["value"]
        switch cmd["cmd"] as? String {
        case "pause": queue.async { self.setPaused(!self.paused, from: "本机") }
        case "mic":
            queue.async {
                self.micMuted.toggle()
                let v = self.micMuted
                self.micQueue.removeAll()
                self.model.mutate { $0.micMuted = v }
            }
        case "speaker":
            queue.async {
                self.speakerMuted.toggle()
                let v = self.speakerMuted
                if v { self.pacer.cancel() }
                self.model.mutate { $0.speakerMuted = v }
            }
        case "voice":
            guard let v = value as? String, v != model.voice else { return }
            model.voice = v
            model.saveSettings()
            model.mutate { _ in }
            queue.async {
                self.voice = v
                self.model.log("本机", "音色换成 \(v)，重新连接")
                self.connectRealtime()
            }
        case "agentModel":
            guard let v = value as? String else { return }
            model.agentModel = v
            model.saveSettings()
            model.mutate { _ in }
            queue.async { self.agentModel = v }
        case "agentLevel":
            guard let v = value as? String else { return }
            model.agentLevel = v
            model.saveSettings()
            model.mutate { _ in }
            queue.async { self.agentLevel = v }
        case "reconnect":
            queue.async { self.connectRealtime() }
        case "send":
            guard let text = (cmd["text"] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines), !text.isEmpty else { return }
            let direct = cmd["agent"] as? Bool ?? false
            queue.async { direct ? self.delegateDirect(text) : self.sendText(text) }
        case "cancelTask":
            let id = (value as? Int) ?? (value as? Double).map { Int($0) }
            queue.async {
                let ids = self.agent.cancel(id: id)
                if !ids.isEmpty { self.model.log("助手", "取消任务 \(ids.map(String.init).joined(separator: ","))") }
            }
        case "clear":
            model.mutate { m in
                m.turns.removeAll()
                m.tasks.removeAll { $0.status != "running" }
                for t in m.tasks { m.addTurn(role: "task", text: t.title, live: false, taskId: t.id) }
            }
        case "clearLogs":
            model.mutate { $0.logs.removeAll() }
        default:
            break
        }
    }

    // MARK: - 暂停、文字输入、直接派任务

    private func setPaused(_ on: Bool, from who: String) {
        guard on != paused else { return }
        paused = on
        model.mutate { $0.paused = on }
        // 告诉徽章（新固件会跟着切换暂停；旧固件忽略这个字节）。
        if who != "徽章" { link.send(kind: Lan.ctrl, payload: Data([0xA1, on ? 1 : 0])) }
        if on {
            pacer.cancel()
            micQueue.removeAll()
            if responseActive { sendJSON(["type": "response.cancel"]) }
            finishAllSpeeches()
            userSpeaking = false
            awaitingSince = nil
            setCaption(0, "")
        }
        model.log(who, on ? "暂停" : "继续")
    }

    private func sendText(_ text: String) {
        model.mutate { $0.addTurn(role: "user", text: text, live: false) }
        guard sessionReady else {
            model.mutate { $0.addTurn(role: "note", text: "还没连上模型，这句没送出去", live: false) }
            return
        }
        if paused { setPaused(false, from: "本机") }
        sendJSON(["type": "conversation.item.create",
                  "item": ["type": "message", "role": "user",
                           "content": [["type": "input_text", "text": text]]]])
        sendJSON(["type": "response.create"])
        awaitingSince = Date()
        setCaption(1, text)
    }

    private func delegateDirect(_ text: String) {
        model.mutate { $0.addTurn(role: "user", text: text, live: false) }
        let title = text.count > 20 ? String(text.prefix(20)) + "…" : text
        _ = agent.start(prompt: text, title: title, resume: false, model: agentModel, level: agentLevel)
    }

    private func taskFinished(_ task: AgentTask) {
        runningTasks[task.id] = nil
        let word: String
        switch task.status {
        case "done": word = "做完了"
        case "cancelled": word = "被取消了"
        default: word = "没做成"
        }
        model.log("助手", "#\(task.id) \(word)（\(Int(task.seconds)) 秒）")
        if task.status == "cancelled" || reportedTasks.contains(task.id) { return }
        announcements.append((task.id, """
        （系统通知，不是主人说的话）后台任务 #\(task.id)「\(task.title)」\(word)。助手的结果：\(task.result.prefix(1500))
        请用一两句自然的口语把要点告诉主人。不要念出长路径或网址，不要说「系统通知」。
        """))
    }

    // MARK: - 每 50 ms：字幕进度、阶段、通知

    private func tick() {
        let st = pacer.status()
        let now = Date()
        if st.playing { lastPlaying = now }

        reveal(st)

        if let since = awaitingSince, now.timeIntervalSince(since) > 10 { awaitingSince = nil }

        let phase: Int
        if paused {
            phase = 0
        } else if st.playing || (badgePhase == 3 && now.timeIntervalSince(lastPlaying) < 0.35) {
            phase = 3
        } else if userSpeaking {
            phase = 1
        } else if awaitingSince != nil || responseActive {
            phase = 2
        } else {
            phase = 0
        }
        sendPhase(phase)

        announcements.removeAll { reportedTasks.contains($0.taskId) }
        if !announcements.isEmpty, sessionReady, !paused, phase == 0, !responseActive,
           now.timeIntervalSince(lastPlaying) > 0.8 {
            let (taskId, text) = announcements.removeFirst()
            reportedTasks.insert(taskId)
            sendJSON(["type": "conversation.item.create",
                      "item": ["type": "message", "role": "system",
                               "content": [["type": "input_text", "text": text]]]])
            sendJSON(["type": "response.create", "response": ["tool_choice": "none"]])
            awaitingSince = now
        }

        // 说完一阵子以后，字幕换成后台任务进度，或者清掉。
        if phase == 0 && !st.playing {
            let idle = now.timeIntervalSince(captionAt)
            if let task = runningTasks.values.min(by: { $0.id < $1.id }),
               caption.role == 0 || caption.role == 3 || idle > 8 {
                var line = "在办：\(task.title)"
                if let step = task.steps.last { line += "\n\(step.tool) · \(step.text.prefix(28))" }
                if caption != Caption(role: 3, text: line) { setCaption(3, line, touch: caption.role != 3) }
            } else if runningTasks.isEmpty && caption.role == 3 {
                setCaption(0, "")
            } else if caption.role != 0 && idle > 14 {
                setCaption(0, "")
            }
        }
        flushCaption()
        model.setLevels(output: st.level)
    }

    /// 按徽章实际念到的位置，把小克的话一个字一个字放出来。
    private func reveal(_ st: Pacer.Status) {
        guard !speeches.isEmpty else { return }
        var keep: [Speech] = []
        for var s in speeches {
            let chars = Array(s.text)
            var target = s.shown
            if let a = s.audioStart {
                let end = s.audioEnd ?? st.received
                let span = end - a
                let ratio = span > 0 ? min(1, max(0, (st.played - a) / span)) : 0
                target = Int((Double(chars.count) * ratio).rounded(.up))
                if s.audioDone && st.played >= end - 0.02 { target = chars.count }
            } else if s.audioDone || speakerMuted || paused {
                target = chars.count
            }
            target = min(chars.count, max(target, s.shown))
            let finished = s.textDone && s.audioDone && target >= chars.count
            if target != s.shown || finished {
                s.shown = target
                let shown = String(chars.prefix(target))
                let turn = s.turnId
                model.mutate { $0.updateTurn(turn) { t in t.text = shown; t.live = !finished } }
                if target > 0 { setCaption(2, shown) }
            }
            if !finished { keep.append(s) }
        }
        speeches = keep
    }

    private func finishAllSpeeches() {
        for s in speeches {
            let shown = String(Array(s.text).prefix(max(s.shown, 0)))
            let turn = s.turnId
            model.mutate { m in
                if shown.isEmpty { m.removeTurn(turn) } else { m.updateTurn(turn) { t in t.text = shown + "……"; t.live = false } }
            }
        }
        speeches.removeAll()
    }

    private func speechIndex(_ itemId: String) -> Int {
        if let i = speeches.firstIndex(where: { $0.itemId == itemId }) { return i }
        let turn = DeskModel.newId()
        model.mutate { $0.addTurn(id: turn, role: "bot", text: "", live: true) }
        speeches.append(Speech(itemId: itemId, turnId: turn))
        return speeches.count - 1
    }

    // MARK: - 字幕

    private func setCaption(_ role: Int, _ text: String, touch: Bool = true) {
        caption = Caption(role: role, text: text)
        if touch { captionAt = Date() }
    }

    private func flushCaption() {
        if shownCaption != caption {
            shownCaption = caption
            let c = caption
            model.mutate { $0.caption = c }
        }
        let now = Date()
        // 徽章字幕每秒最多 4 次：每次都是一个小包、一次整段重排。
        guard badgeCaption != caption, now.timeIntervalSince(badgeCaptionAt) > 0.25 || caption.role == 0 else { return }
        badgeCaption = caption
        badgeCaptionAt = now
        if badgeBitmap {
            let role = caption.text.isEmpty ? 0 : caption.role
            let bmp = role == 0 ? nil : SubtitleRenderer.render(caption.text, role: role)
            for chunk in SubtitleRenderer.chunks(bmp, role: role, previous: sentBitmap) {
                link.send(kind: Lan.subimg, payload: chunk)
            }
            sentBitmap = bmp
            return
        }
        var payload = Data([UInt8(caption.role)])
        // 徽章只显示 3 行（约 40 字），多发只会让它多排版、多占字形缓存。
        payload.append(Self.utf8Tail(String(caption.text.suffix(42)), maxBytes: 200))
        link.send(kind: Lan.text, payload: payload)
    }

    /// 取字符串末尾、不超过 maxBytes 的完整字符。徽章只显示最后几行。
    static func utf8Tail(_ text: String, maxBytes: Int) -> Data {
        let all = Data(text.utf8)
        if all.count <= maxBytes { return all }
        var out = Data()
        for ch in text.reversed() {
            let b = Data(String(ch).utf8)
            if out.count + b.count > maxBytes { break }
            out.insert(contentsOf: b, at: 0)
        }
        return out
    }

    // MARK: - 徽章套接字

    private func listen() {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { return }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = Self.port.bigEndian
        addr.sin_addr.s_addr = INADDR_ANY
        let ok = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        if ok != 0 || Darwin.listen(fd, 2) != 0 {
            model.log("本机", "\(Self.port) 端口被占用，可能还有旧的 VoiceDesk 或 realtime_bridge.py 在跑")
            close(fd)
            return
        }
        listenFD = fd
        model.log("本机", "监听 0.0.0.0:\(Self.port)")
    }

    private func loop() {
        var buf = [UInt8](repeating: 0, count: 4096)
        while running {
            guard listenFD >= 0 else {
                Thread.sleep(forTimeInterval: 0.5)
                continue
            }
            if connFD < 0 {
                var addr = sockaddr_in()
                var len = socklen_t(MemoryLayout<sockaddr_in>.size)
                let fd = withUnsafeMutablePointer(to: &addr) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { accept(listenFD, $0, &len) }
                }
                guard fd >= 0 else { continue }
                var one: Int32 = 1
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))
                setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
                var tv = timeval(tv_sec: 2, tv_usec: 0)
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
                connFD = fd
                link.attach(fd)
                parser.reset()
                let ip = String(cString: inet_ntoa(addr.sin_addr))
                model.mutate { $0.badge = ip }
                model.log("徽章", "连上 \(ip)")
                queue.async {
                    self.badgePhase = -1
                    self.badgeCaption = nil
                }
                continue
            }
            var rfds = fd_set()
            fdZero(&rfds)
            fdSet(listenFD, &rfds)
            fdSet(connFD, &rfds)
            var tv = timeval(tv_sec: 0, tv_usec: 200_000)
            let ready = select(max(listenFD, connFD) + 1, &rfds, nil, nil, &tv)
            if ready > 0 && fdIsSet(listenFD, &rfds) {
                model.log("徽章", "新连接替换旧的")
                dropConn()
                continue
            }
            if ready > 0 && fdIsSet(connFD, &rfds) {
                let n = recv(connFD, &buf, buf.count, 0)
                if n <= 0 {
                    dropConn()
                    model.log("徽章", "断开")
                    continue
                }
                for frame in parser.feed(Data(buf.prefix(n))) { onFrame(frame) }
            }
        }
    }

    private func dropConn() {
        let fd = link.detach()
        if fd >= 0 { close(fd) }
        connFD = -1
        model.mutate { $0.badge = nil }
    }

    private var inCount = 0

    private func onFrame(_ frame: LanFrame) {
        switch frame.kind {
        case Lan.hello:
            link.send(kind: Lan.helloAck, payload: Data())
            var rate: UInt32 = 0
            if frame.payload.count >= 4 {
                for i in 0..<4 { rate |= UInt32(frame.payload[frame.payload.startIndex + i]) << (8 * i) }
            }
            let caps = frame.payload.count >= 6 ? frame.payload[frame.payload.startIndex + 5] : 0
            let bitmap = caps & 1 != 0
            queue.async {
                self.badgeBitmap = bitmap
                self.sentBitmap = nil
                self.badgeCaption = nil
            }
            model.log("徽章", "HELLO \(rate) Hz\(bitmap ? "，字幕走位图" : "")")
            // 刚连上时以徽章为准：先当作没暂停，徽章若在暂停会紧接着发 CTRL 1。
            queue.async { self.setPaused(false, from: "徽章") }
        case Lan.ctrl:
            if frame.payload.count == 1 {
                let hold = frame.payload[frame.payload.startIndex] == 1
                queue.async { self.setPaused(hold, from: "徽章") }
            }
        case Lan.pcm:
            inCount += 1
            if inCount % 2 == 0 {
                let level = pcmRMS(frame.payload)
                let count = inCount
                model.setLevels(input: level)
                if count % 25 == 0 { model.mutate { $0.inFrames = count } }
            }
            let payload = frame.payload
            queue.async { self.forwardMic(payload) }
        default:
            break
        }
    }

    private var outCount = 0

    private func sendPCM(_ frames: [Data]) {
        link.sendBatch(kind: Lan.pcm, payloads: frames)
        let before = outCount
        outCount += frames.count
        if outCount / 25 != before / 25 {
            let c = outCount
            model.mutate { $0.outFrames = c }
        }
    }

    private func sendPhase(_ value: Int) {
        guard value != badgePhase else { return }
        badgePhase = value
        let names = ["idle", "listening", "thinking", "speaking"]
        let name = paused ? "paused" : names[max(0, min(3, value))]
        model.mutate { $0.phase = name }
        link.send(kind: Lan.ctrl, payload: Data([0xA0, UInt8(value)]))
    }

    // MARK: - 麦克风 → Realtime

    private func forwardMic(_ pcm: Data) {
        if paused || micMuted || !sessionReady { return }
        // 喇叭贴着麦克风：她说话时、以及说完后 0.4 秒内不收音。
        if pacer.status().playing || Date().timeIntervalSince(lastPlaying) < 0.4 { return }
        let upPCM = up.push(pcm)
        guard !upPCM.isEmpty, micQueue.count < 50 else { return }
        micQueue.append(upPCM)
        pumpMic()
    }

    private func pumpMic() {
        guard !micSending, let ws, !micQueue.isEmpty else { return }
        micSending = true
        let chunk = micQueue.removeFirst()
        let msg = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"\(chunk.base64EncodedString())\"}"
        ws.send(.string(msg)) { [weak self] _ in
            self?.queue.async {
                self?.micSending = false
                self?.pumpMic()
            }
        }
    }

    // MARK: - Realtime 会话

    private func connectRealtime() {
        wsGen += 1
        ws?.cancel(with: .goingAway, reason: nil)
        ws = nil
        sessionReady = false
        micQueue.removeAll()
        micSending = false
        responseActive = false
        awaitingSince = nil
        userSpeaking = false
        up.reset()
        down.reset()
        slicer.reset()
        guard let auth = Self.loadAuth(), let host = URL(string: auth.base)?.host else {
            model.mutate { $0.realtime = "nokey" }
            model.log("模型", "没有配置地址和密钥：设 VOICEDESK_API_KEY（可选 VOICEDESK_BASE_URL），或写 ~/.config/voicedesk/config.json")
            return
        }
        var comp = URLComponents()
        comp.scheme = "wss"
        comp.host = host
        comp.port = URL(string: auth.base)?.port
        comp.path = "/v1/realtime"
        comp.queryItems = [URLQueryItem(name: "model", value: auth.realtimeModel)]
        guard let url = comp.url else { return }
        var req = URLRequest(url: url)
        req.setValue("Bearer \(auth.key)", forHTTPHeaderField: "Authorization")
        let task = URLSession.shared.webSocketTask(with: req)
        task.maximumMessageSize = 8 * 1024 * 1024
        ws = task
        model.mutate { $0.realtime = "connecting" }
        task.resume()
        receive(gen: wsGen)
    }

    private func receive(gen: Int) {
        ws?.receive { [weak self] result in
            self?.queue.async {
                guard let self, gen == self.wsGen else { return }
                switch result {
                case .success(let message):
                    if case .string(let text) = message { self.onEvent(text) }
                    self.receive(gen: gen)
                case .failure(let error):
                    self.sessionReady = false
                    self.model.mutate { $0.realtime = "offline" }
                    self.model.log("模型", "断开：\(error.localizedDescription)")
                    self.queue.asyncAfter(deadline: .now() + 2) { [weak self] in
                        guard let self, self.running, gen == self.wsGen else { return }
                        self.connectRealtime()
                    }
                }
            }
        }
    }

    private func sessionConfig() -> [String: Any] {
        let now = DateFormatter.localizedString(from: Date(), dateStyle: .full, timeStyle: .short)
        let instructions = """
        你是小克，住在主人桌上的一块小徽章里，也能替主人操作这台 Mac。
        说话：简短自然的口语普通话，一般一两句，适合直接念出来。吐字清楚，不夹英文，不用表情符号、Markdown 或列表，不说自己是程序。
        做事：凡是需要动手的——找文件、整理文件、看网页查资料、跑命令、写东西、改设置、操作应用——就调用 run_agent_task，把主人的要求整理成一段完整清晰的中文任务交给助手，然后只用一句很短的话告诉主人你去办了。任务在后台进行，做完会有系统通知，你再用一两句话把结果讲给主人。只是打开某个应用或网页时，直接用 open_app 或 open_url。交出去之后不要自己去查进度；只有主人问进度时才调用 check_agent_tasks，让你停就调用 cancel_agent_task。如果主人说「接着刚才的」「再改一下」，run_agent_task 的 continue_previous 设为 true。
        现在是 \(now)。
        """
        let tools: [[String: Any]] = [
            [
                "type": "function", "name": "run_agent_task",
                "description": "把需要在这台 Mac 上动手的事交给后台助手（Claude Code）完成：查找/整理/读写文件、搜索网页查资料、运行命令、操作应用、写文档或代码等。立即返回，任务在后台跑，完成后会收到系统通知。",
                "parameters": [
                    "type": "object",
                    "properties": [
                        "task": ["type": "string", "description": "完整清晰的中文任务描述，包含主人的原意、目标和任何限制"],
                        "title": ["type": "string", "description": "十个字以内的任务标题，显示在屏幕上"],
                        "continue_previous": ["type": "boolean", "description": "是否接着上一个任务的上下文继续"],
                    ],
                    "required": ["task", "title"],
                ],
            ],
            [
                "type": "function", "name": "check_agent_tasks",
                "description": "查看后台任务的进度和结果。",
                "parameters": ["type": "object", "properties": [String: Any]()],
            ],
            [
                "type": "function", "name": "cancel_agent_task",
                "description": "取消后台任务。不给 task_id 就取消所有正在进行的任务。",
                "parameters": ["type": "object", "properties": ["task_id": ["type": "integer"]]],
            ],
            [
                "type": "function", "name": "open_app",
                "description": "在 Mac 上打开一个应用，例如 Safari、Music、Calculator、Notes。",
                "parameters": ["type": "object",
                               "properties": ["name": ["type": "string", "description": "应用的英文名"]],
                               "required": ["name"]],
            ],
            [
                "type": "function", "name": "open_url",
                "description": "用默认浏览器打开一个网址。",
                "parameters": ["type": "object",
                               "properties": ["url": ["type": "string"]],
                               "required": ["url"]],
            ],
        ]
        return [
            "type": "realtime",
            "instructions": instructions,
            "tools": tools,
            "tool_choice": "auto",
            "audio": [
                "input": [
                    "format": ["type": "audio/pcm", "rate": 24000],
                    "transcription": ["model": Self.loadAuth()?.transcribeModel ?? "gpt-4o-mini-transcribe", "language": "zh"],
                    "turn_detection": ["type": "server_vad", "silence_duration_ms": 500],
                ],
                "output": [
                    "format": ["type": "audio/pcm", "rate": 24000],
                    "voice": voice,
                ],
            ],
        ]
    }

    private func onEvent(_ text: String) {
        guard let data = text.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let kind = obj["type"] as? String else { return }
        let itemId = obj["item_id"] as? String ?? ""
        switch kind {
        case "session.created":
            sendJSON(["type": "session.update", "session": sessionConfig()])
        case "session.updated":
            if !sessionReady { model.log("模型", "已连上 · \(voice)") }
            sessionReady = true
            up.reset()
            down.reset()
            let v = voice
            model.mutate { $0.realtime = "online"; $0.voice = v }

        case "input_audio_buffer.speech_started":
            userSpeaking = true
            if userTurns[itemId] == nil {
                let turn = DeskModel.newId()
                userTurns[itemId] = turn
                model.mutate { $0.addTurn(id: turn, role: "user", text: "", live: true) }
            }
            setCaption(1, "")
        case "input_audio_buffer.speech_stopped":
            userSpeaking = false
            awaitingSince = Date()
        case "conversation.item.input_audio_transcription.delta":
            let delta = obj["delta"] as? String ?? ""
            let text = (userTexts[itemId] ?? "") + delta
            userTexts[itemId] = text
            if let turn = userTurns[itemId] { model.mutate { $0.updateTurn(turn) { $0.text = text } } }
            if caption.role == 1 { setCaption(1, text) }
        case "conversation.item.input_audio_transcription.completed",
             "conversation.item.input_audio_transcription.failed":
            let text = ((obj["transcript"] as? String) ?? userTexts[itemId] ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            userTexts[itemId] = nil
            if let turn = userTurns.removeValue(forKey: itemId) {
                model.mutate { m in
                    if text.isEmpty { m.removeTurn(turn) } else { m.updateTurn(turn) { $0.text = text; $0.live = false } }
                }
            }
            if caption.role == 1 && !text.isEmpty { setCaption(1, text) }
            if !text.isEmpty { model.log("主人", text) }

        case "response.created":
            responseActive = true
            responseSpoke = false
            awaitingSince = nil
        case "response.output_audio_transcript.delta":
            let i = speechIndex(itemId)
            speeches[i].text += obj["delta"] as? String ?? ""
        case "response.output_audio_transcript.done":
            let i = speechIndex(itemId)
            if let t = obj["transcript"] as? String { speeches[i].text = t }
            speeches[i].textDone = true
            model.log("小克", speeches[i].text)
        case "response.output_audio.delta":
            guard let delta = obj["delta"] as? String, let pcm = Data(base64Encoded: delta) else { return }
            if paused || speakerMuted { return }
            responseSpoke = true
            let i = speechIndex(itemId)
            if speeches[i].audioStart == nil { speeches[i].audioStart = pacer.status().received }
            pacer.push(slicer.push(down.push(pcm)))
        case "response.output_audio.done":
            var tail = slicer.flush()
            if !tail.isEmpty {
                tail.append(Data(count: Lan.frameBytes - tail.count))
                pacer.push([tail])
            }
            let i = speechIndex(itemId)
            speeches[i].audioDone = true
            if speeches[i].audioStart != nil { speeches[i].audioEnd = pacer.status().received }
        case "response.output_item.done":
            guard let item = obj["item"] as? [String: Any], item["type"] as? String == "function_call" else { return }
            let name = item["name"] as? String ?? ""
            let callId = item["call_id"] as? String ?? ""
            let argsText = item["arguments"] as? String ?? "{}"
            let args = (try? JSONSerialization.jsonObject(with: Data(argsText.utf8))) as? [String: Any] ?? [:]
            let output = runTool(name: name, args: args)
            if name != "run_agent_task" { onlyAgentStarts = false }
            pendingOutputs.append((callId, output))
        case "response.done":
            responseActive = false
            // 没出声就结束的回答（只调了工具，或被取消），把对应的回合标成已完成。
            for i in speeches.indices where !speeches[i].audioDone {
                speeches[i].audioDone = true
                speeches[i].textDone = true
            }
            if !pendingOutputs.isEmpty {
                for (callId, output) in pendingOutputs {
                    sendJSON(["type": "conversation.item.create",
                              "item": ["type": "function_call_output", "call_id": callId, "output": output]])
                }
                pendingOutputs.removeAll()
                // 派完任务、查完进度后只说话，不再调工具，免得她一遍遍自己查进度。
                // 调工具前已经说过「我去办」的，就不用再补一句。
                if onlyAgentStarts && responseSpoke {
                    awaitingSince = nil
                } else if quietAfterTools {
                    sendJSON(["type": "response.create", "response": ["tool_choice": "none"]])
                } else {
                    sendJSON(["type": "response.create"])
                }
                if !(onlyAgentStarts && responseSpoke) { awaitingSince = Date() }
                quietAfterTools = false
                onlyAgentStarts = true
            }
        case "error":
            let err = obj["error"] as? [String: Any]
            let msg = (err?["message"] as? String) ?? kind
            if msg.contains("no active response") { return }
            model.log("模型", "错误：\(msg)")
        default:
            break
        }
    }

    private func sendJSON(_ obj: [String: Any]) {
        guard let ws, let data = try? JSONSerialization.data(withJSONObject: obj),
              let text = String(data: data, encoding: .utf8) else { return }
        ws.send(.string(text)) { _ in }
    }

    // MARK: - 工具

    private func runTool(name: String, args: [String: Any]) -> String {
        func json(_ o: [String: Any]) -> String {
            (try? JSONSerialization.data(withJSONObject: o)).flatMap { String(data: $0, encoding: .utf8) } ?? "{}"
        }
        model.log("工具", "\(name) \(args.isEmpty ? "" : String(describing: args))")
        switch name {
        case "run_agent_task":
            let task = (args["task"] as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            guard !task.isEmpty else { return json(["ok": false, "error": "任务是空的"]) }
            guard agent.ready else { return json(["ok": false, "error": "这台 Mac 上找不到 claude 命令"]) }
            let title = args["title"] as? String ?? ""
            let resume = args["continue_previous"] as? Bool ?? false
            let id = agent.start(prompt: task, title: title, resume: resume, model: agentModel, level: agentLevel)
            quietAfterTools = true
            return json(["ok": true, "task_id": id,
                         "note": "已在后台开始。只用很短一句话告诉主人你去办了（刚才已经说过就不用再说），不要复述细节。不要查询进度，做完会自动通知你。"])
        case "check_agent_tasks":
            let rows = agent.summary()
            quietAfterTools = true
            // 这里已经把结果交给她了，完成通知就不再重复。
            for row in rows where (row["status"] as? String) != "running" {
                if let id = row["task_id"] as? Int { reportedTasks.insert(id) }
            }
            let running = rows.contains { ($0["status"] as? String) == "running" }
            return json(["tasks": rows,
                         "note": rows.isEmpty ? "目前没有后台任务"
                             : running ? "还在进行的任务做完会自动通知你，现在告诉主人还在办就好。" : ""])
        case "cancel_agent_task":
            let id = (args["task_id"] as? Int) ?? (args["task_id"] as? Double).map { Int($0) }
            let ids = agent.cancel(id: id)
            return json(["cancelled": ids])
        case "open_app":
            let app = (args["name"] as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            guard !app.isEmpty else { return json(["ok": false, "error": "没有应用名"]) }
            let ok = Self.run("/usr/bin/open", ["-a", app]) == 0
            model.mutate { $0.addTurn(role: "note", text: ok ? "打开了 \(app)" : "没找到应用 \(app)", live: false) }
            return json(ok ? ["ok": true] : ["ok": false, "error": "没找到这个应用，可以换个英文名试试，或者交给 run_agent_task"])
        case "open_url":
            let raw = (args["url"] as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            let fixed = raw.contains("://") ? raw : "https://\(raw)"
            guard let url = URL(string: fixed), ["http", "https"].contains(url.scheme ?? "") else {
                return json(["ok": false, "error": "网址不对"])
            }
            let ok = Self.run("/usr/bin/open", [url.absoluteString]) == 0
            model.mutate { $0.addTurn(role: "note", text: "打开了 \(url.host ?? fixed)", live: false) }
            return json(["ok": ok])
        default:
            return json(["ok": false, "error": "没有这个工具"])
        }
    }

    private static func run(_ exe: String, _ args: [String]) -> Int32 {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return -1 }
        p.waitUntilExit()
        return p.terminationStatus
    }

    static let port: UInt16 = UInt16(ProcessInfo.processInfo.environment["VOICEDESK_PORT"] ?? "") ?? 19731

    private struct Auth {
        let base: String
        let key: String
        let realtimeModel: String
        let transcribeModel: String
    }

    /// 地址和密钥按顺序找：环境变量 → ~/.config/voicedesk/config.json →
    /// ~/.claude/settings.json 里的 env（Claude Code 和 Realtime 共用同一个中转时可以直接复用）。
    /// 密钥只放进请求头，不写日志。
    private static func loadAuth() -> Auth? {
        let env = ProcessInfo.processInfo.environment
        let home = FileManager.default.homeDirectoryForCurrentUser
        func json(_ path: String) -> [String: Any]? {
            guard let data = try? Data(contentsOf: home.appendingPathComponent(path)) else { return nil }
            return (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
        }
        let file = json(".config/voicedesk/config.json") ?? [:]
        let claude = (json(".claude/settings.json")?["env"] as? [String: Any]) ?? [:]

        var base = env["VOICEDESK_BASE_URL"] ?? file["base_url"] as? String
        var key = env["VOICEDESK_API_KEY"] ?? file["api_key"] as? String
        if key == nil, let k = claude["ANTHROPIC_API_KEY"] as? String {
            key = k
            base = base ?? claude["ANTHROPIC_BASE_URL"] as? String
        }
        guard let key, !key.isEmpty else { return nil }
        var b = base ?? "https://api.openai.com"
        while b.hasSuffix("/") { b.removeLast() }
        return Auth(base: b, key: key,
                    realtimeModel: env["VOICEDESK_REALTIME_MODEL"] ?? file["realtime_model"] as? String ?? "gpt-realtime-2.1",
                    transcribeModel: env["VOICEDESK_TRANSCRIBE_MODEL"] ?? file["transcribe_model"] as? String ?? "gpt-4o-mini-transcribe")
    }
}

private func fdZero(_ set: inout fd_set) {
    withUnsafeMutableBytes(of: &set.fds_bits) { $0.initializeMemory(as: UInt8.self, repeating: 0) }
}

private func fdSet(_ fd: Int32, _ set: inout fd_set) {
    withUnsafeMutableBytes(of: &set.fds_bits) { raw in
        raw.bindMemory(to: Int32.self)[Int(fd) / 32] |= Int32(1) << (Int(fd) % 32)
    }
}

private func fdIsSet(_ fd: Int32, _ set: inout fd_set) -> Bool {
    withUnsafeBytes(of: &set.fds_bits) { raw in
        (raw.bindMemory(to: Int32.self)[Int(fd) / 32] & (Int32(1) << (Int(fd) % 32))) != 0
    }
}
