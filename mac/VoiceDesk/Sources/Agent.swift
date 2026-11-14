import Foundation

/// 把需要动手的事交给本机的 Claude Code（无头模式）去做。
/// 小克只负责听和说；真正读写文件、跑命令、查网页的是这里起的 `claude -p` 进程。
final class AgentRunner {
    var onUpdate: ((AgentTask) -> Void)?
    var onFinish: ((AgentTask) -> Void)?

    private let q = DispatchQueue(label: "voice.desk.agent")
    private var tasks: [Int: AgentTask] = [:]
    private var procs: [Int: Process] = [:]
    private var buffers: [Int: Data] = [:]
    private var stderrTail: [Int: String] = [:]
    private var gotResult: Set<Int> = []
    private var nextId = 1
    private var lastSession: String?
    private var sessions: [Int: String] = [:]
    let executable: String?

    private static let systemNote = """
    你正被桌面语音助手「小克」调用，替主人在这台 Mac 上办事。主人是用嘴说的，任务描述可能有听写错误，按最合理的意思理解。
    直接动手完成，不要反问。确实缺关键信息，或者操作有风险（删除、对外发送、付款、发布、改系统设置）时不要执行，在结果里说清楚需要主人确认什么。
    最后一条消息用中文写，两三句话说清结果，会被小克念给主人听：不要 Markdown、不要列表、不要代码块；必要时带上关键的文件名或数字。
    """

    init() {
        executable = Self.findClaude()
    }

    var ready: Bool { executable != nil }

    /// 开始一个任务，返回任务编号。resume 为 true 时接着上一个任务的会话继续。
    func start(prompt: String, title: String, resume: Bool, model: String, level: String) -> Int {
        q.sync {
            let id = nextId
            nextId += 1
            var task = AgentTask(id: id, title: title.isEmpty ? String(prompt.prefix(24)) : title,
                                 prompt: prompt, status: "running", steps: [], result: "",
                                 started: Date().timeIntervalSince1970 * 1000, seconds: 0, cost: nil,
                                 model: model)
            tasks[id] = task
            guard let exe = executable else {
                task.status = "failed"
                task.result = "这台 Mac 上找不到 claude 命令。"
                tasks[id] = task
                let snapshot = task
                DispatchQueue.global().async { self.onFinish?(snapshot) }
                return id
            }
            launch(id: id, exe: exe, prompt: prompt, resume: resume ? lastSession : nil, model: model, level: level)
            onUpdate?(task)
            return id
        }
    }

    /// 取消一个任务；id 为空时取消全部正在跑的。返回被取消的编号。
    func cancel(id: Int?) -> [Int] {
        q.sync {
            let ids = id.map { [$0] } ?? Array(procs.keys)
            var done: [Int] = []
            for i in ids {
                guard let p = procs[i], p.isRunning else { continue }
                tasks[i]?.status = "cancelled"
                p.terminate()
                done.append(i)
            }
            return done.sorted()
        }
    }

    /// 给模型看的任务概况。
    func summary() -> [[String: Any]] {
        q.sync {
            tasks.values.sorted { $0.id < $1.id }.suffix(6).map { t in
                var row: [String: Any] = ["task_id": t.id, "title": t.title, "status": t.status]
                if t.status == "running" {
                    row["elapsed_seconds"] = Int(Date().timeIntervalSince1970 - t.started / 1000)
                    if let last = t.steps.last { row["current_step"] = "\(last.tool) \(last.text)" }
                } else if !t.result.isEmpty {
                    row["result"] = String(t.result.prefix(600))
                }
                return row
            }
        }
    }

    var running: [AgentTask] {
        q.sync { tasks.values.filter { $0.status == "running" }.sorted { $0.id < $1.id } }
    }

    // MARK: - 进程

    private func launch(id: Int, exe: String, prompt: String, resume: String?, model: String, level: String) {
        var args = ["-p", prompt, "--output-format", "stream-json", "--verbose", "--model", model,
                    "--append-system-prompt", Self.systemNote]
        switch level {
        case "readonly":
            args += ["--permission-mode", "default",
                     "--allowedTools", "Read,Glob,Grep,WebSearch,WebFetch,TodoWrite"]
        case "full":
            args += ["--dangerously-skip-permissions"]
        default:
            args += ["--permission-mode", "acceptEdits",
                     "--allowedTools", "Read,Glob,Grep,WebSearch,WebFetch,TodoWrite,Write,Edit,Bash",
                     "--disallowedTools",
                     "Bash(rm *),Bash(sudo *),Bash(git push *),Bash(shutdown *),Bash(reboot *),Bash(diskutil *),Bash(dd *),Bash(mkfs *),Bash(launchctl *),Bash(defaults delete *)"]
        }
        if let resume { args += ["--resume", resume] }

        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        p.currentDirectoryURL = FileManager.default.homeDirectoryForCurrentUser
        var env = ProcessInfo.processInfo.environment
        for k in env.keys where k == "CLAUDECODE" || k.hasPrefix("CLAUDE_CODE_ENTRYPOINT") || k.hasPrefix("CLAUDE_CODE_SSE") {
            env.removeValue(forKey: k)
        }
        let path = env["PATH"] ?? ""
        env["PATH"] = (["/opt/homebrew/bin", "/usr/local/bin", "\(NSHomeDirectory())/.local/bin"] + [path])
            .filter { !$0.isEmpty }.joined(separator: ":")
        p.environment = env
        p.standardInput = FileHandle.nullDevice

        let out = Pipe()
        let err = Pipe()
        p.standardOutput = out
        p.standardError = err
        out.fileHandleForReading.readabilityHandler = { [weak self] h in
            let data = h.availableData
            guard !data.isEmpty else {
                h.readabilityHandler = nil
                return
            }
            self?.q.async { self?.consume(id: id, data: data) }
        }
        err.fileHandleForReading.readabilityHandler = { [weak self] h in
            let data = h.availableData
            guard !data.isEmpty else {
                h.readabilityHandler = nil
                return
            }
            let text = String(decoding: data, as: UTF8.self)
            self?.q.async {
                let joined = (self?.stderrTail[id] ?? "") + text
                self?.stderrTail[id] = String(joined.suffix(1200))
            }
        }
        p.terminationHandler = { [weak self] proc in
            // 留一点时间让最后几行 stdout 读完。
            self?.q.asyncAfter(deadline: .now() + 0.3) { self?.finished(id: id, code: proc.terminationStatus) }
        }
        do {
            try p.run()
            procs[id] = p
            // 十分钟还没完就停掉。
            q.asyncAfter(deadline: .now() + 600) { [weak self] in
                guard let self, let p = self.procs[id], p.isRunning else { return }
                self.tasks[id]?.status = "failed"
                self.tasks[id]?.result = "做了十分钟还没做完，先停下了。"
                p.terminate()
            }
        } catch {
            tasks[id]?.status = "failed"
            tasks[id]?.result = "启动 claude 失败：\(error.localizedDescription)"
            if let t = tasks[id] { DispatchQueue.global().async { self.onFinish?(t) } }
        }
    }

    private func consume(id: Int, data: Data) {
        var buf = (buffers[id] ?? Data()) + data
        while let nl = buf.firstIndex(of: 0x0A) {
            let line = buf[buf.startIndex..<nl]
            buf = Data(buf[buf.index(after: nl)...])
            handleLine(id: id, line: Data(line))
        }
        buffers[id] = buf
    }

    private func handleLine(id: Int, line: Data) {
        guard var task = tasks[id],
              let obj = try? JSONSerialization.jsonObject(with: line) as? [String: Any],
              let type = obj["type"] as? String else { return }
        switch type {
        case "system":
            if let sid = obj["session_id"] as? String { sessions[id] = sid }
            return
        case "assistant":
            guard let msg = obj["message"] as? [String: Any],
                  let content = msg["content"] as? [[String: Any]] else { return }
            for part in content {
                switch part["type"] as? String {
                case "tool_use":
                    let name = part["name"] as? String ?? "工具"
                    if name == "TodoWrite" { continue }
                    let input = part["input"] as? [String: Any] ?? [:]
                    task.steps.append(TaskStep(tool: Self.toolLabel(name), text: Self.describe(name, input)))
                case "text":
                    let text = (part["text"] as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
                    if !text.isEmpty { task.steps.append(TaskStep(tool: "想", text: String(text.prefix(160)))) }
                default:
                    break
                }
            }
            if task.steps.count > 80 { task.steps.removeFirst(task.steps.count - 80) }
        case "user":
            guard let msg = obj["message"] as? [String: Any],
                  let content = msg["content"] as? [[String: Any]] else { return }
            if content.contains(where: { ($0["is_error"] as? Bool) == true }), !task.steps.isEmpty {
                let i = task.steps.count - 1
                if !task.steps[i].text.hasSuffix("（没成功）") { task.steps[i].text += "（没成功）" }
            }
        case "result":
            gotResult.insert(id)
            let isError = obj["is_error"] as? Bool ?? false
            let result = (obj["result"] as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            if task.status == "running" { task.status = isError ? "failed" : "done" }
            task.result = result.isEmpty ? (isError ? "出错了。" : "做完了。") : result
            task.cost = obj["total_cost_usd"] as? Double
            if let ms = obj["duration_ms"] as? Double { task.seconds = ms / 1000 }
            if let sid = obj["session_id"] as? String { sessions[id] = sid }
            if !isError, let sid = sessions[id] { lastSession = sid }
        default:
            return
        }
        task.seconds = max(task.seconds, Date().timeIntervalSince1970 - task.started / 1000)
        tasks[id] = task
        onUpdate?(task)
    }

    private func finished(id: Int, code: Int32) {
        procs[id] = nil
        if let rest = buffers[id], !rest.isEmpty { handleLine(id: id, line: rest) }
        buffers[id] = nil
        guard var task = tasks[id] else { return }
        if task.status == "running" {
            task.status = code == 0 ? "done" : "failed"
        }
        if task.status == "cancelled" && task.result.isEmpty {
            task.result = "已取消。"
        } else if !gotResult.contains(id) && task.result.isEmpty {
            let tail = (stderrTail[id] ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            task.result = tail.isEmpty ? "进程退出（\(code)），没有给出结果。" : String(tail.suffix(400))
        }
        task.seconds = Date().timeIntervalSince1970 - task.started / 1000
        tasks[id] = task
        stderrTail[id] = nil
        onUpdate?(task)
        onFinish?(task)
    }

    // MARK: - 显示用

    private static func toolLabel(_ name: String) -> String {
        switch name {
        case "Bash": return "终端"
        case "Read": return "读"
        case "Write": return "写"
        case "Edit", "MultiEdit": return "改"
        case "Glob": return "找"
        case "Grep": return "搜"
        case "WebSearch": return "网搜"
        case "WebFetch": return "网页"
        case "Task", "Agent": return "分派"
        default: return name.count > 6 ? String(name.prefix(6)) : name
        }
    }

    private static func describe(_ name: String, _ input: [String: Any]) -> String {
        func s(_ k: String) -> String? { (input[k] as? String).flatMap { $0.isEmpty ? nil : $0 } }
        func short(_ path: String) -> String {
            path.replacingOccurrences(of: NSHomeDirectory(), with: "~")
        }
        let text: String
        switch name {
        case "Bash": text = s("description") ?? s("command") ?? ""
        case "Read", "Write", "Edit", "MultiEdit": text = short(s("file_path") ?? "")
        case "Glob": text = s("pattern") ?? ""
        case "Grep": text = s("pattern") ?? ""
        case "WebSearch": text = s("query") ?? ""
        case "WebFetch": text = s("url") ?? ""
        case "Task", "Agent": text = s("description") ?? ""
        default: text = s("description") ?? ""
        }
        return String(text.replacingOccurrences(of: "\n", with: " ").prefix(140))
    }

    private static func findClaude() -> String? {
        let home = NSHomeDirectory()
        let fixed = ["/opt/homebrew/bin/claude", "/usr/local/bin/claude",
                     "\(home)/.local/bin/claude", "\(home)/.claude/local/claude"]
        if let hit = fixed.first(where: { FileManager.default.isExecutableFile(atPath: $0) }) { return hit }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/zsh")
        p.arguments = ["-lc", "command -v claude"]
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return nil }
        p.waitUntilExit()
        let out = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return FileManager.default.isExecutableFile(atPath: out) ? out : nil
    }
}
