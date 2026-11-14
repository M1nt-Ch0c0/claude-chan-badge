import Foundation

// 界面状态。只在主线程读写；引擎用 mutate 投递过来。
// 网页端每帧拿一次快照（有改动时），电平单独走一条更轻的通道。

struct Turn: Codable {
    var id: Int
    var role: String      // user | bot | note | task
    var text: String
    var live: Bool
    var time: String
    var taskId: Int?
}

struct TaskStep: Codable {
    var tool: String
    var text: String
}

struct AgentTask: Codable {
    var id: Int
    var title: String
    var prompt: String
    var status: String    // running | done | failed | cancelled
    var steps: [TaskStep]
    var result: String
    var started: Double   // 毫秒时间戳
    var seconds: Double
    var cost: Double?
    var model: String
}

struct LogRow: Codable {
    var time: String
    var side: String
    var text: String
}

struct Caption: Codable, Equatable {
    var role: Int         // 0 无，1 主人，2 小克，3 后台任务
    var text: String
}

struct Option: Codable {
    var value: String
    var label: String
    var note: String
}

private struct Snapshot: Codable {
    var badge: String?
    var realtime: String
    var phase: String
    var paused: Bool
    var micMuted: Bool
    var speakerMuted: Bool
    var voice: String
    var voices: [Option]
    var agentModel: String
    var agentModels: [Option]
    var agentLevel: String
    var agentLevels: [Option]
    var agentReady: Bool
    var caption: Caption
    var turns: [Turn]
    var tasks: [AgentTask]
    var logs: [LogRow]
    var inFrames: Int
    var outFrames: Int
}

final class DeskModel {
    var badge: String?
    var realtime = "connecting"   // connecting | online | offline | nokey
    var phase = "idle"            // idle | listening | thinking | speaking | paused
    var paused = false
    var micMuted = false
    var speakerMuted = false
    var voice: String
    var agentModel: String
    var agentLevel: String
    var agentReady = false
    var caption = Caption(role: 0, text: "")
    var turns: [Turn] = []
    var tasks: [AgentTask] = []
    var logs: [LogRow] = []
    var inFrames = 0
    var outFrames = 0
    var inLevel = 0.0
    var outLevel = 0.0

    private(set) var dirty = true
    private static let idLock = NSLock()
    private static var lastId = 0

    /// 任何线程都能取，回合编号由引擎先分配，再投递到主线程。
    static func newId() -> Int {
        idLock.lock()
        defer { idLock.unlock() }
        lastId += 1
        return lastId
    }

    static let voices = [
        Option(value: "marin", label: "Marin", note: "温柔清亮"),
        Option(value: "cedar", label: "Cedar", note: "沉稳"),
        Option(value: "coral", label: "Coral", note: "活泼"),
        Option(value: "sage", label: "Sage", note: "轻柔"),
    ]
    static let agentModels = [
        Option(value: "claude-sonnet-5-5", label: "Sonnet 5.5", note: "快，日常够用"),
        Option(value: "claude-opus-5-5", label: "Opus 5.5", note: "最聪明，慢一点"),
        // 部分中转下 Haiku 5.5 跑 Claude Code 时，工具结果之后返回 400（named conversation participants），暂不提供。
    ]
    static let agentLevels = [
        Option(value: "readonly", label: "只看", note: "只能读文件、搜网页，不改任何东西"),
        Option(value: "standard", label: "标准", note: "可以读写文件、跑命令；删除、sudo、推送等会被拦下"),
        Option(value: "full", label: "完全", note: "不设限制。听错一句也会照做，谨慎打开"),
    ]

    init() {
        let d = UserDefaults.standard
        voice = d.string(forKey: "voice") ?? "marin"
        let stored = d.string(forKey: "agentModel") ?? ""
        agentModel = Self.agentModels.contains { $0.value == stored } ? stored : "claude-sonnet-5-5"
        agentLevel = d.string(forKey: "agentLevel") ?? "standard"
    }

    func saveSettings() {
        let d = UserDefaults.standard
        d.set(voice, forKey: "voice")
        d.set(agentModel, forKey: "agentModel")
        d.set(agentLevel, forKey: "agentLevel")
    }

    /// 从任意线程修改界面状态。
    func mutate(_ change: @escaping (DeskModel) -> Void) {
        if Thread.isMainThread {
            change(self)
            dirty = true
        } else {
            DispatchQueue.main.async {
                change(self)
                self.dirty = true
            }
        }
    }

    func log(_ side: String, _ text: String) {
        let stamp = Self.clock.string(from: Date())
        print("\(stamp) [\(side)] \(text)")
        fflush(stdout)
        mutate { m in
            m.logs.append(LogRow(time: stamp, side: side, text: text))
            if m.logs.count > 400 { m.logs.removeFirst(m.logs.count - 400) }
        }
    }

    // 以下只在主线程调用。

    func addTurn(id: Int = DeskModel.newId(), role: String, text: String, live: Bool, taskId: Int? = nil) {
        turns.append(Turn(id: id, role: role, text: text, live: live, time: Self.clock.string(from: Date()), taskId: taskId))
        if turns.count > 160 { turns.removeFirst(turns.count - 160) }
    }

    func updateTurn(_ id: Int, _ change: (inout Turn) -> Void) {
        guard let i = turns.lastIndex(where: { $0.id == id }) else { return }
        change(&turns[i])
    }

    func removeTurn(_ id: Int) {
        turns.removeAll { $0.id == id }
    }

    func upsertTask(_ task: AgentTask) {
        if let i = tasks.firstIndex(where: { $0.id == task.id }) {
            tasks[i] = task
        } else {
            tasks.append(task)
            addTurn(role: "task", text: task.title, live: false, taskId: task.id)
        }
    }

    /// 电平每帧都变，不触发整份快照。
    func setLevels(input: Double? = nil, output: Double? = nil) {
        DispatchQueue.main.async {
            if let input { self.inLevel = input }
            if let output { self.outLevel = output }
        }
    }

    func takeSnapshot() -> String? {
        dirty = false
        let snap = Snapshot(
            badge: badge, realtime: realtime, phase: phase, paused: paused,
            micMuted: micMuted, speakerMuted: speakerMuted,
            voice: voice, voices: Self.voices,
            agentModel: agentModel, agentModels: Self.agentModels,
            agentLevel: agentLevel, agentLevels: Self.agentLevels, agentReady: agentReady,
            caption: caption, turns: turns, tasks: tasks, logs: Array(logs.suffix(250)),
            inFrames: inFrames, outFrames: outFrames)
        guard let data = try? JSONEncoder().encode(snap) else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static let clock: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f
    }()
}
