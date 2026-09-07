# Session-first 交互式 CLI 设计

## 目标

把现有一次性 `agent run` C++17 Runtime 扩展成可直接启动的多轮交互程序。无参数启动 `AgentFramework.exe` 后，用户可以在同一工作区连续交流，退出后再恢复；原有 `run`、`resume`、`verify-log`、`evaluate-log` 命令保持兼容。

## 核心决策

Session 是交互产品的顶层聚合，Runtime Task 是 Session 中一轮用户输入的执行记录。

```text
InteractiveCli
    -> SessionEngine
        -> SessionStore (sessions/<session-id>/events.jsonl)
        -> RuntimeEngine (one task per turn)
            -> EventStore (tasks/<task-id>/events.jsonl)
```
不能只保存用户问题和最终回答。会话上下文必须使用 provider-neutral `Message`：用户文本、助手文本、助手工具调用和用户工具结果均按原顺序保存。下一轮把已提交的完整消息序列作为 Runtime 的初始消息，再追加本轮用户消息。

## 第一阶段范围

第一阶段交付以下可执行闭环：

- 无参数启动进入交互模式。
- 首次启动允许输入工作区；空输入使用当前目录。
- 新建 Session，完整记录成功 Turn 的消息增量。
- 默认恢复当前运行数据目录中最近更新的 Session。
- `/new [workspace]` 新建会话；省略路径时沿用当前工作区。
- `/resume <session-id>` 恢复指定会话。
- `/status` 显示 Session、模型、工作区、已提交轮数和消息数。
- `/clear` 等价于在当前工作区新建 Session，旧 Session 保留。
- `/exit` 和输入 EOF 安全退出。
- 普通文本提交为新 Turn，并打印最终回答。
- Turn 启动时先持久化 `task_id`；崩溃后若存在 pending Turn，则恢复同一个 Runtime Task，不重复创建新 Task。
- 现有一次性命令和事件日志保持可读取。

第一阶段不包含全屏 TUI、流式 token 渲染、Shell `!` 模式、Session fork、模型生成摘要或自动 compact。完整历史仍然落盘；在实现上下文压缩前，达到供应商上下文上限会作为真实限制报告，不能宣称具备无限上下文。

## Runtime 扩展

`RunRequest` 增加三个仅供会话编排使用的可选字段：

```cpp
std::vector<Message> initial_messages;
std::optional<std::string> requested_task_id;
std::optional<SessionTaskLink> session_link;
```

`SessionTaskLink` 包含 `session_id` 和单调递增的 `turn_index`。普通 `run` 三者均为空，行为和旧日志字节结构保持不变。

当 `requested_task_id` 存在时，Runtime 必须验证其满足现有 `task-` 加 32 个小写十六进制字符的格式，并使用该 ID；否则仍由现有 `IdGenerator` 生成。

`TaskStartedPayload` 增加可选的 `initial_messages` 和 `session_link`。序列化规则为：为空时省略新键，使旧的一次性日志保持原格式；反序列化允许旧三键 payload 或带两个新键的新 payload，其他键继续拒绝。Reducer 在首事件中先载入 `initial_messages`，再追加本轮用户文本。

初始消息必须满足：

- 不允许 `Role::System`；system prompt 仍由 RuntimeConfig 独立提供。
- 文本不可为空。
- `ToolUseBlock` 仅能出现在 Assistant 消息。
- `ToolResultBlock` 仅能出现在 User 消息。
- 工具结果必须对应此前尚未匹配的工具调用。
- 历史末尾不能留下未匹配工具调用。

这些规则防止损坏的 Session 注入不可回放的模型上下文。

## Session 事件与状态

Session ID 格式为 `session-` 加 32 个小写十六进制字符。

Session JSONL schema version 1 包含四类事件：

```text
session_started
turn_started
turn_committed
turn_failed
```

- `session_started`：`workspace_utf8`、`model`。
- `turn_started`：`turn_index`、`task_id`、`user_text`。必须在 Runtime 外部调用前写入。
- `turn_committed`：`turn_index`、`task_id`、`messages`。消息必须等于该 Task 相对 Session 旧上下文的真实增量。
- `turn_failed`：`turn_index`、`task_id`、稳定的状态名和安全摘要。详细错误仍保存在 Task 日志，不复制敏感细节到终端。

Reducer 要求事件序号连续、Session ID 一致、每次最多一个 pending Turn、Turn index 从 1 单调递增、完成/失败必须对应当前 pending Turn。`turn_committed` 才会把消息增量加入下一轮活动上下文；失败 Turn 的用户文本仍由 `turn_started` 永久保存，完整执行轨迹由其 `task_id` 指向 Task JSONL。

## SessionStore

`JsonlSessionStore` 在以下路径持久化：

```text
<AGENT_RUNTIME_ROOT>/sessions/<session-id>/events.jsonl
```

它提供：

```cpp
append(SessionEvent)
read_session(session_id)
list_sessions()
```

路径必须先验证 ID 并确认位于 sessions 根目录内；拒绝符号链接或 reparse point 穿越。每个事件编码为单行 JSON、追加后 flush。读取采用严格 JSON 和严格 schema；损坏尾部不静默修复。

列表通过读取每个合法 Session 的日志得到，按最后事件时间降序排列。单个损坏 Session 不能被自动选中，显式 `/resume` 时返回持久化错误。

## SessionEngine 与恢复顺序

创建会话：

1. 生成 Session ID。
2. 写入 `session_started`。
3. 返回重放后的 `SessionState`。

运行 Turn：

1. 验证 Session 当前没有 pending Turn。
2. 预生成 Task ID。
3. 先写 `turn_started`。
4. 使用 Session 已提交 messages、固定 workspace、task ID 和 session link 调用 `RuntimeEngine::run`。
5. Runtime 完成后，从 `TaskState.messages` 截取消息增量。
6. 成功终态写 `turn_committed`；其他终态写 `turn_failed`。

恢复 pending Turn：

1. 用 Session 中的 task ID 读取 Task 日志。
2. 日志不存在时，以相同 task ID 启动该 Turn；日志存在时调用现有 `RuntimeEngine::resume`。
3. Runtime 到达终态后提交对应 Session 结束事件。

如果 Runtime Task 已终态但 Session 结束事件尚未写入，恢复只做本地回放并补写 Session 事件，不再次调用模型或工具。

## 交互 CLI

无参数进入 `InteractiveCli`；有参数仍进入现有 `CliApp`。

启动画面显示应用名、模型、工作区和 Session ID，不打印密钥、base URL 或完整错误详情。进度事件默认只显示简洁的阶段信息，最终文本继续使用现有 UTF-8/控制字符净化逻辑。

Windows 双击时工作目录并不可靠。因此交互启动按以下顺序寻找环境配置：

1. 显式 `--env-file`（用于命令行）。
2. `AgentFramework.exe` 同目录下的 `.env`（用于便携目录）。
3. 当前工作目录下的 `.env`（用于开发运行）。

只在无参数交互启动时执行自动发现；`verify-log` 和 `evaluate-log` 保持完全无凭据路径。密钥不得写入二进制、日志、Session 或 Task payload。

## 兼容性和验收

- 旧 schema-v1 Task fixture 必须继续通过 `verify-log`。
- 旧 `agent run/resume/verify-log/evaluate-log` CLI 测试不得改变预期输出。
- Session 第二轮的首次 ModelRequest 必须逐块等于第一轮提交 transcript 加第二轮用户消息。
- 模拟在 `turn_started` 后崩溃，重启必须继续同一 task ID。
- 模拟 Runtime 已完成但 Session 未提交，恢复不得产生新的模型/工具调用。
- Session JSONL 非法 ID、额外键、断行 JSON、序号跳跃和 task link 不一致必须 fail closed。
- Windows Debug 构建、完整离线 CTest 和一个使用脚本模型的双轮进程测试必须通过。
- 真实 MiniMax 冒烟测试只在用户配置存在且明确运行时执行；离线测试通过不能替代真实供应商验证。
