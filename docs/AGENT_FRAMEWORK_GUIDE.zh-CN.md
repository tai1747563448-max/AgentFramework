# AgentFramework 中文说明书

> 版本范围：本说明书对应单 Agent、C++17 Runtime、Python RAG 外挂这一里程碑。它以仓库内真实的“修复计算器并在崩溃后恢复”工作流为主线，解释全部生产代码；多 Agent 不在本版本范围内。

## 1. 我们究竟做了什么

这个项目是一个本地优先的 Coding Agent 框架。用户给它一个 Issue 和一个工作区，它会：

1. 从本地知识库检索与 Issue 有关的证据；
2. 把当前任务、历史消息、工具定义和证据交给模型；
3. 让模型选择受控工具读取、搜索或修改工作区；
4. 在明确启用后，用固定的 CMake/CTest 命令配置、构建和测试；
5. 把每一步先写入 JSONL 事件日志，再更新内存状态；
6. 进程崩溃后从日志重放，并从最后一个持久状态继续。

任务结束后，用户可以再用两个独立的 credential-free 命令离线验证和评估日志；`run` 本身不会自动执行这两条命令。

它不是一个“让模型随便执行命令”的外壳。模型不能选择 Shell、任意可执行程序、Git 命令、网络命令、环境变量或工作目录。它现在也不是多 Agent 系统、MCP 服务、HTTP 服务、TUI、插件平台、向量数据库或自动重试调度器。

一句话概括边界：

```text
一个前台任务 + 一个状态机 + 一份耐久事件日志
            + 五个安全文件工具
            + 三个可选结构化构建工具
            + 一个可选 Python SQLite/BM25 知识外挂
```

## 2. 总体架构

依赖方向始终朝内：外部库和操作系统只能出现在 Adapter，核心业务规则不依赖它们。

```text
用户 / CLI
    |
    v
main.cpp 组合根
    |
    +--> RuntimeEngine ------------------------------+
    |       |                                        |
    |       +--> Ports 接口                          |
    |       |      Model / Tool / Knowledge          |
    |       |      EventStore / Clock / ID / Cancel  |
    |       |                                        |
    |       +--> StateReducer <--> Domain            |
    |                                                |
    +--> Adapters                                    |
            Anthropic + CPR HTTP                     |
            JSONL 持久化                              |
            Workspace 文件工具                       |
            CMake/CTest + DirectProcessRunner        |
            Python RAG 进程                           |
                                                     |
Python 3.10+ sidecar                                  |
    indexer -> SQLite -> BM25 retriever --------------+
```

四层职责如下：

| 层 | 目录 | 只负责什么 |
|---|---|---|
| Domain | `src/domain` | 类型、状态、事件、错误和不变量所需的数据词汇 |
| Application | `src/application` | 状态重放、Runtime 控制流、离线评估 |
| Ports | `src/ports` | 核心需要外界提供的抽象接口 |
| Adapters | `src/adapters` | HTTP、文件系统、进程、JSONL、RAG 等具体实现 |

`src/main.cpp` 只负责选择实现并把它们接起来，`src/cli` 只负责命令边界和安全输出，`src/config` 只负责配置。

## 3. 构建、测试和本地备份

### 3.1 Windows / Visual Studio 2022

在仓库根目录执行：

```powershell
cmake -S . -B build/vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build/vs2022 --config Debug --parallel
ctest --test-dir build/vs2022 -C Debug --output-on-failure
```

`CMakeLists.txt` 固定使用 C++17，构建两个主要静态库和 `agent` 可执行文件。依赖是固定版本的 `nlohmann/json`、CPR 和 `dotenv-cpp`。默认测试全是本地离线测试；只有显式配置 `AGENT_ENABLE_LIVE_TESTS=ON` 才会生成 Provider live smoke 目标。

Python 3.10+ 可用时，CTest 还会注册 Python RAG 单元测试、C++/Python RAG 集成测试和真实自主修复工作流。

### 3.2 本地 Git 备份

`backup` 不是 GitHub，而是 E 盘裸仓库：`E:\GitBackups\AgentFramework.git`。每个可解释里程碑提交后可执行：

```powershell
$branch = git branch --show-current
git push backup $branch
$local = git rev-parse HEAD
$remote = ((git ls-remote backup "refs/heads/$branch") -split "\s+")[0]
if ($local -ne $remote) { throw "backup commit mismatch" }
```

它备份 Git 已跟踪内容，不备份 `.env`、运行时日志或其他被忽略文件，也不是异地灾备。

## 4. 从一个真实任务看完整调用链

仓库中的 `tests/integration/autonomous_issue_workflow_test.cpp` 不是纸面示例。它在 Unicode 临时目录创建一个真实的小型 CMake 项目：

```cpp
int add(int left, int right) {
    return left - right; // 故意写错
}
```

对应测试要求 `add(20, 22) == 42`。测试先真实执行配置、构建和 CTest，确认基线失败；随后让生产 Runtime 完成下面的流程，只有模型是离线脚本模型。

### 4.1 第一次运行：读取并修改

Issue 是：修好 `calculator.cpp`，构建并运行精确测试 `calculator.correct`。

事件 1–12 是：

| 序号 | 事件 | 发生了什么 |
|---:|---|---|
| 1 | `TaskStarted` | 固化 Issue、工作区和预算 |
| 2 | `ContextPreparationStarted` | 开始检索知识 |
| 3 | `ContextPrepared` | Python RAG 返回计算器修复和验证建议 |
| 4 | `ModelCallStarted` | 固化模型实际将看到的完整请求 |
| 5 | `ModelCallSucceeded` | 模型请求 `read_file` |
| 6 | `ToolCallStarted` | 固化读取意图 |
| 7 | `ToolCallSucceeded` | 返回文件内容和完整文件 SHA-256 |
| 8–10 | context/model start | 新一轮检索和模型调用 |
| 11 | `ModelCallSucceeded` | 模型请求带旧 SHA 的 `replace_text` |
| 12 | `ToolCallStarted` | 修改意图已经耐久化 |

文件工具实际把减法改成了加法，但测试用的事件存储器故意让第 13 次 append 失败。因此文件系统效果已经发生，`ToolCallSucceeded` 却没有写入日志。Runtime 返回 `PersistenceFailure`，耐久状态仍停在 `AwaitingTool`。

这正是最难处理的一类崩溃：外部效果和本地提交之间出现断点。

### 4.2 新进程恢复：以至少一次语义重做

新 `RuntimeEngine` 读取前 12 条事件并严格重放。它看到 `replace_text` 已开始但没有结果，于是重执行同一个 `ToolCall`，不伪造成功，也不增加新的逻辑工具调用计数。

由于文件已经变成新版本，旧 SHA 不再匹配。`replace_text` 返回一个正常完成但 `is_error=true` 的 `conflict` 结果。这成为事件 13。注意：

- 它不是 Runtime 崩溃；
- 它不是 `ToolCallFailed`；
- 它是给模型看的、可恢复的工具业务错误。

模型读到冲突后重新读取文件，确认加法已经存在，再继续构建。这就是 Compare-And-Swap 版本前提在恢复中的价值：框架不承诺副作用 exactly-once，而是让重复执行安全地暴露差异。

### 4.3 后续 29 条事件

完整后半段是：

| 序号 | 阶段 |
|---:|---|
| 13 | 重做 `replace_text`，得到 SHA conflict |
| 14–19 | 重新检索、模型决定 `read_file`、读取已修复内容 |
| 20–25 | 重新检索、模型决定 `configure_project`、CMake 配置成功 |
| 26–31 | 重新检索、模型决定 `build_project`、目标构建成功 |
| 32–37 | 重新检索、模型决定 `run_tests`、精确 CTest 成功 |
| 38–41 | 最后一轮检索和模型调用，收到 `end_turn` 文本 |
| 42 | `TaskCompleted`，固化最终文本 |

最终证据是：

- 42 条事件从 1 连续编号，无重复、无缺口；
- 只有 1 个 `TaskStarted`；
- 7 个逻辑模型轮次，6 个逻辑工具调用；
- 7 轮 evidence，7 个模型请求都携带对应 evidence；
- 1 个工具错误结果，就是恢复时的 conflict；
- 最终重放状态和在线内存状态完全相等；
- 评估 verdict 为 `pass`；
- `.git`、Runtime 哨兵和工作区外哨兵未被修改。

后面的代码说明都可以对应回这条真实链路。

## 5. Domain：状态机使用的语言

### 5.1 `Value` 和 `Result`

`src/domain/value.h` 与 `src/domain/value.cpp` 定义与 JSON 库无关的递归值：null、bool、`int64`、double、string、array、object。数组和对象通过只读共享节点保存，所以 Domain 不需要依赖 `nlohmann::json`。

`src/domain/result.h` 定义 `Result<T>` 和 `Result<void>`。成功值和 `RuntimeError` 互斥，调用者必须先检查 `has_value()`。这使失败成为显式控制流，而不是让异常穿过应用层。

`src/domain/runtime_error.h` 定义稳定错误码、内部说明文本和 `retryable`。用户提出的简洁失败状态在这里落实：普通终端失败就是 `TaskStatus::Failed`，原因放在 `terminal_error`，没有再添加一个含义重叠的失败状态。

### 5.2 模型、工具与 Evidence

`src/domain/model_types.h` 定义：

- `Message` 与 `ContentBlock`；
- 文本、`ToolUseBlock`、`ToolResultBlock`；
- `ToolDefinition`、`ToolCall`、`ToolResult`；
- `Evidence`、`EvidencePack`；
- 完整 `ModelRequest` 和 `ModelResponse`；
- `EndTurn / ToolUse / MaxTokens / StopSequence / Unknown`。

它同时验证模型工具调用 ID 非空且唯一、名称非空、参数必须是 object；canonical stop reason 必须与原始 Provider 字符串精确匹配。响应侧的 `ToolResultBlock` 永远非法，因为工具结果只能由本地 Runtime 产生，不能让模型伪造。

`src/domain/evidence_validation.h` 与 `src/domain/evidence_validation.cpp` 是 Provider 无关的证据防线：最多 20 项、单项内容最多 8192 字节、总内容最多 32768 字节、ID 唯一、严格 UTF-8、metadata 深度/节点/字符串受限，double 必须有限。

### 5.3 状态和事件

`src/domain/task_state.h` 定义八个状态：

```text
Created -> PreparingContext -> AwaitingModel -> AwaitingTool
   ^                                  |             |
   +----------------------------------+-------------+

终端：Completed / Failed / BudgetExceeded / Cancelled
```

`TaskState` 不只是一个枚举。它保存 Issue、工作区、预算、逻辑计数、消息、当前 evidence、最后的完整模型请求、是否有模型调用在途、待执行工具、活跃工具 ID、工具结果、最终文本和终端错误。恢复之所以可能，是因为下一步需要的信息都能从事件重建。

`src/domain/runtime_event.h` 与 `src/domain/runtime_event.cpp` 定义 14 种强类型事件和 `EventKind` 映射。每条 `RuntimeEvent` 都有 schema version、sequence、task ID、UTC 时间、correlation ID 和一个严格类型 payload。

## 6. Reducer：日志为何不能随便伪造

`src/application/state_reducer.h` 与 `src/application/state_reducer.cpp` 提供两个入口：

- `reduce_event(current, event)`：验证并应用一条候选事件；
- `replay_events(events)`：从头严格重放完整日志。

Reducer 是唯一被允许解释事件语义的地方。它检查：

- 第一条必须是 schema 1、sequence 1 的 `TaskStarted`；
- task ID 必须是 `task-` 加 32 个小写十六进制字符；
- 后续 task ID 一致，序号严格加一；
- 终端状态之后不能再有事件；
- 每个成功或失败结果必须对应唯一的 in-flight 调用；
- 工具结果 ID 必须匹配当前活跃调用；
- `ModelCallStarted.request` 的 messages、evidence、timeout 和 system prompt 必须绑定当前耐久状态；
- `ContextPrepared` 的 evidence 必须满足 Domain 不变量；
- stop reason、raw stop reason 与内容形状必须匹配；
- `TaskCompleted.final_text` 必须等于刚接受的终端模型文本；
- `max_tokens` 预算事件必须紧跟合法 `MaxTokens` 响应；
- 模型轮数、工具次数和时间预算事件必须使用精确名称、错误码、消息、状态和耗尽条件。

JSON 解码成功不代表事件合法；只有 Reducer 接受，它才属于这个状态机。

## 7. RuntimeEngine：真正的 Agent 循环

`src/application/runtime_engine.h` 定义 `RunRequest`、`ResumeRequest`、`RuntimeResult` 和只包含四个安全字段的 `RuntimeProgress`。`src/application/runtime_engine.cpp` 实现核心循环。

### 7.1 先预演、再落盘、最后更新内存

`append_event` 的顺序固定为：

```text
构造候选事件
  -> Reducer 预演
  -> EventStore append + flush
  -> 把预演状态提交到内存
  -> 通知进度观察者
```

所以失败 append 不会污染内存状态，也不会为了描述 append 失败再写一条注定写不进去的事件。观察者只在耐久提交后收到 task ID、sequence、event kind、status；观察者抛异常会在本次运行中被禁用，不影响任务。

### 7.2 每轮控制流

`continue_task` 根据重放得到的状态行动，而不是依赖一组易丢失的局部变量：

1. `Created`：写 `ContextPreparationStarted`；
2. `PreparingContext`：检查取消和时间，再调用 `KnowledgeProvider`；
3. `AwaitingModel`：检查取消、时间和模型轮数，固化 `ModelCallStarted`，调用模型；
4. `AwaitingTool`：按 Provider 原顺序逐个固化并执行工具；
5. 工具全部处理后回到 context 阶段；
6. `end_turn/stop_sequence` 产生 `TaskCompleted`；
7. `max_tokens` 产生 `BudgetExceeded`；
8. Provider、knowledge 或 gateway 基础设施错误直接走相应的专门失败事件并进入 `Failed`。

`ToolResult{is_error=true}` 不等于 gateway 基础设施失败：前者会回到模型，让模型读错误并调整；后者才是 `ToolCallFailed -> Failed`。

### 7.3 Guard 顺序和预算

每次外部调用之前检查顺序是：取消、墙钟时间、逻辑次数。取消优先，因此用户 Ctrl+C 不会被另一个预算状态覆盖。

逻辑模型/工具计数跨 resume 累计。一次进程尝试内的墙钟使用 monotonic clock；显式 resume 会重置这段 V1 墙钟。计数衡量耐久“调用意图”，不是物理尝试次数：同一个 in-flight 调用在多次崩溃后反复 resume，不会不断增加逻辑计数。因此 V1 不宣称全局限制物理重试次数。

## 8. Ports：核心与外界之间的插座

这些头文件只定义接口，不含具体 I/O：

| 文件 | Runtime 需要的能力 |
|---|---|
| `src/ports/model_client.h` | 完成一个 `ModelRequest` |
| `src/ports/tool_gateway.h` | 给出工具定义并执行一个 `ToolCall` |
| `src/ports/knowledge_provider.h` | 按当前任务检索 `EvidencePack` |
| `src/ports/event_store.h` | append 事件并读取事件文件 |
| `src/ports/process_runner.h` | 无 Shell 地启动固定程序、传 argv/stdin、限时限流 |
| `src/ports/clock.h` | UTC 时间和 monotonic 毫秒 |
| `src/ports/id_generator.h` | 生成 task/correlation ID |
| `src/ports/cancellation.h` | 查询是否请求取消 |

Ports 让单元测试可以注入确定性 fake，也让未来添加第二 Provider 时不必改状态机。

## 9. Anthropic Adapter 与 HTTP

`src/adapters/anthropic/http_transport.h` 定义中性的 POST 请求/响应和 `HttpTransport`。

`src/adapters/anthropic/cpr_http_transport.h` 与 `src/adapters/anthropic/cpr_http_transport.cpp` 用 CPR 实现 POST，显式禁止重定向。超时和传输错误被归一化，响应 body 不会被拼进错误说明。

`src/adapters/anthropic/anthropic_messages_client.h` 与 `src/adapters/anthropic/anthropic_messages_client.cpp` 负责：

- 校验 base URL、模型、凭据方式、API version 和 max tokens；
- 在 `/v1/messages` 编码 system、历史消息、工具 schema；
- 把 evidence 放在额外的 user 文本中，并明确标记为“不可信参考数据”；
- 在 header 中使用且只使用 API key 或 Bearer 二者之一；
- 解码文本和工具调用；
- 拒绝重复/空工具 ID、非 object 参数、模型伪造的 tool result；
- 执行 stop/content 矩阵；
- 不把凭据、Provider 错误 body 或原始 transport 文本传播给 CLI。

当前只有这一个 Provider Adapter。默认离线测试并不证明真实 Provider 现在可用；live smoke 必须显式开启并由用户提供凭据后另行运行。

## 10. JSON 与 JSONL 持久化

`src/adapters/json/value_json.h` 与 `src/adapters/json/value_json.cpp` 是 Domain `Value` 和 `nlohmann::json` 之间的显式 codec；它拒绝无法表示或不安全的值。

`src/adapters/persistence/event_json.h` 与 `src/adapters/persistence/event_json.cpp` 是事件 wire schema。Schema 自己拥有的 object 必须键集合精确；只有 Provider-neutral 的自由 `Value` map 允许开放键。解码不会替代 Reducer 验证。

`src/adapters/persistence/jsonl_event_store.h` 与 `src/adapters/persistence/jsonl_event_store.cpp` 把每条事件写到：

```text
<runtime-root>/tasks/<task-id>/events.jsonl
```

安全 append 在 Windows 使用不跟随 reparse point 的 Handle，在 POSIX 使用 `openat`/`O_NOFOLLOW`；要求目录链正常、leaf 是普通单链接文件，写后 flush/fsync。恢复专用 `read_task(task_id)` 同样逐组件拒绝链接/reparse 和多链接 leaf，并把解码后的 task ID 绑定请求 ID。普通 `read_file(path)` 只供用户显式选择的 `verify-log/evaluate-log` 使用。

读取时逐行 JSON 解码，再检查单 task、连续 sequence，最后完整 replay。日志损坏会被拒绝，不会自动修补。

## 11. 安全工作区文件工具

### 11.1 路径策略和公共数据

`src/adapters/workspace/workspace_types.h` 定义文件工具内部的 Fault、相对路径、分页、列表、搜索和写入结果。

`src/adapters/workspace/workspace_path_policy.h` 与 `src/adapters/workspace/workspace_path_policy.cpp` 解析模型提供的路径并执行边界策略：拒绝绝对路径、`..`、平台别名、链接/reparse、硬链接、错误 leaf 类型，以及 `.git`、`.worktrees`、`.agent`、`.rag`、runtime data、`.env*`、常见凭据文件和保留临时名。工作区 root 本身也不能是链接，runtime root 的物理别名也受到保护。

`src/adapters/workspace/workspace_text.h` 与 `src/adapters/workspace/workspace_text.cpp` 实现严格 UTF-8、SHA-256、按完整行分页和非重叠字面搜索；大小写不敏感模式只折叠 ASCII，保证跨平台确定性。

### 11.2 文件操作

`src/adapters/workspace/workspace_file_ops.h` 与 `src/adapters/workspace/workspace_file_ops.cpp` 实现实际 I/O：

- 文本文件最多 1 MiB，必须严格 UTF-8 且无 NUL；
- list/search 稳定排序且有结果、条目、文件和扫描字节上限；
- `read_file` 返回完整文件 SHA，但内容可以分页；
- `replace_text` 要求旧 SHA 和精确非重叠匹配次数；
- `write_file(create)` 只创建不存在文件；
- `write_file(overwrite)` 必须携带当前 SHA；
- 写入使用同目录独占临时文件、flush、原子安装并复验最终字节。

CAS 前提防止模型拿着过期内容覆盖用户或上一轮已做的修改。

### 11.3 给模型看的 Gateway

`src/adapters/workspace/workspace_tool_gateway.h` 与 `src/adapters/workspace/workspace_tool_gateway.cpp` 暴露恰好五个 closed-schema 工具：

| 工具 | 用途 |
|---|---|
| `list_files` | 列出安全相对路径 |
| `read_file` | 分页读取并取得完整 SHA |
| `search_text` | 确定性字面搜索 |
| `replace_text` | 基于 SHA 和匹配数的替换 |
| `write_file` | 有版本前提地创建或覆盖 |

Gateway 对参数深度、节点数、编码后字节和结果 JSON 做二次限界。模型输入错误、访问拒绝、冲突和限额都成为 `is_error=true` 的结构化 `ToolResult`，模型可以阅读；只有无法产生可信结果的内部异常才作为 gateway failure 结束任务。

## 12. 受控进程与结构化构建工具

`src/adapters/process/direct_process_runner.h` 与 `src/adapters/process/direct_process_runner.cpp` 实现 Windows/POSIX 双平台直接子进程：

- 不经过 Shell；
- 程序和 argv 分离；
- 支持 Unicode argv、cwd 和 stdin；
- 同时排空 stdout/stderr，避免管道死锁；
- 对两路输出分别保存有界前缀/尾部并修复无效 UTF-8；
- 超时后终止 Windows Job 或 POSIX 进程组；
- 构造缩减后的子进程环境，过滤 Provider 凭据；
- 返回退出码、超时、耗时和截断标记。

`src/adapters/build/cmake_tool_gateway.h` 与 `src/adapters/build/cmake_tool_gateway.cpp` 只允许三条结构化路径：

- `configure_project(configuration)`；
- `build_project(configuration, optional target)`；
- `run_tests(configuration, optional exact test_name)`。

程序固定为 `cmake` 或 `ctest`，构建目录固定为 `<workspace>/.agent/cmake-build`，配置只允许 Debug/Release，target 严格校验，测试名被转成精确且转义过的正则。非零退出和 timeout 作为模型可见的工具错误，而不是 Runtime 基础设施失败。

启用构建工具仍等于授权工作区中的 CMake、编译器和测试以当前用户权限运行代码；它不是 OS 沙箱，只应对可信本地仓库开启。

`src/adapters/tools/composite_tool_gateway.h` 与 `src/adapters/tools/composite_tool_gateway.cpp` 合并多个 Gateway，保证工具名非空且不重复，并按名称路由。默认只有五个文件工具；启用构建后是五加三。

`src/adapters/empty/empty_tool_gateway.h`、`src/adapters/empty/empty_tool_gateway.cpp`、`src/adapters/empty/empty_knowledge_provider.h`、`src/adapters/empty/empty_knowledge_provider.cpp` 是禁用可选能力时的空实现，也是最小测试/嵌入入口。

## 13. Python RAG / 知识库外挂

Python 只承担本地知识索引和查询，不控制 Runtime 状态机。

### 13.1 建索引

```powershell
$python = (Get-Command python).Source
$script = (Resolve-Path .\rag\agent_rag_cli.py).Path
& $python -E -s -X utf8 $script build `
  --source .\docs `
  --index .\.rag\knowledge.sqlite3
```

`rag/agent_rag/indexer.py`：

- 只接受有界的文本和代码扩展名；
- 跳过 build、`.git`、`.agent`、`.rag` 等目录；
- 跳过链接、reparse、硬链接、常见凭据名和分词后含 secret/password/token 等名字；
- 限制条目、文件、单文件和总语料字节；
- 严格 UTF-8，按稳定路径/行号切成有重叠的块；
- 保存 content SHA、token count 和 posting；
- 在临时 SQLite 中构建和 quick_check，再原子替换正式索引。

文件名过滤不是内容级秘密扫描，所以 source 必须是可信、经过选择的语料目录；示例刻意使用 `docs` 而不是整个私人磁盘。

### 13.2 查询

`rag/agent_rag/retriever.py` 实现确定性 tokenizer 和 BM25：支持 ASCII identifier、snake_case、camelCase、中文单字/双字；SQLite 用只读 immutable 模式打开；评分相同再按 source ID 排序。输出受 top-k 和总内容字节限制。

`rag/agent_rag/protocol.py` 定义精确 schema 1 stdin 请求和 stdout 响应，拒绝多余/缺失键、错误类型、空 query、NUL 和越界值。

`rag/agent_rag/cli.py` 只支持 `build` 与 `query` 两个命令，错误只输出固定摘要；`rag/agent_rag_cli.py` 是可直接执行的入口；`rag/agent_rag/__init__.py` 声明包和 schema version。

### 13.3 C++ Adapter

`src/adapters/rag/python_rag_knowledge_provider.h` 与 `src/adapters/rag/python_rag_knowledge_provider.cpp` 每个 context round 启动一个固定进程：

```text
<python> -E -s -X utf8 <script> query --index <index>
```

只把耐久 Issue 以 JSON 写入 stdin，不把工作区文件或凭据交给 Python。script/index 必须是绝对 canonical 普通单链接文件且路径无链接组件。Adapter 对响应执行严格键集、重复键、UTF-8、大小、top-k、相对 citation path、正行号、非负有限 score、64 位小写 SHA、content 重算 SHA 和 `source_id = path#Lx-Ly[-Pn]` 绑定检查，然后再经过 Domain `EvidencePack` 验证。

检索失败走 `ContextPreparationFailed -> Failed`；原始 Python stderr 不持久化、不发给模型。

## 14. 恢复语义：能保证什么，不能保证什么

`run` 从空状态开始；`resume` 先完整 replay，再依据状态进入同一个 `continue_task`。

可以保证：

- 无合法完整日志就不会调用 Provider、RAG 或工具；
- terminal resume 幂等，不追加事件、不调用外部系统；
- in-flight 模型请求从耐久 `last_model_request` 精确重发；
- active tool 从耐久 `ToolCall` 精确重做；
- 已接受的终端模型响应无需再调用模型即可补写终端事件；
- 逻辑模型/工具计数跨进程保留；
- resume 前再次检查取消和本次尝试的墙钟。

不能保证：

- 外部副作用 exactly-once；
- 多次人工 resume 的物理尝试次数有全局上限；
- 墙钟预算跨进程累计；
- 对抗另一个恶意进程同时竞态修改文件的完整 OS 沙箱安全。

文件写工具的 SHA/CAS 是处理“效果已发生但结果未提交”的具体机制，不是普遍事务系统。

## 15. 离线验证与评估

`src/application/task_evaluator.h` 与 `src/application/task_evaluator.cpp` 先调用同一个严格 replay，再计算：

- terminal status；
- model rounds / tool calls；
- evidence rounds / items；
- 携带 evidence 的模型请求数；
- `is_error=true` 的工具结果数；
- last sequence。

只有同时满足下面条件才是 `pass`：状态为 `Completed`、最终文本非空、没有 terminal error、没有模型/工具在途、没有未处理工具和结果。指标加法做溢出检查。

评估说明“这份耐久轨迹完成且结构自洽”，不等于对代码质量、需求正确性或真实 Provider 可用性的万能判断。真实工作流另外用 CTest 结果和哨兵文件约束补足这些证据。

## 16. CLI、配置和输出安全

### 16.1 命令

`src/cli/cli_app.h` 与 `src/cli/cli_app.cpp` 提供：

```powershell
agent --env-file .env run --workspace . --issue "Fix the failing test"
agent --env-file .env resume --task-id task-0123456789abcdef0123456789abcdef
agent verify-log --events .\runtime_data\tasks\<task-id>\events.jsonl
agent evaluate-log --events .\runtime_data\tasks\<task-id>\events.jsonl
```

`verify-log` 和 `evaluate-log` 是 credential-free 早期路径，禁止与 `--env-file` 组合，不创建 Provider 或 RAG。退出码固定：

| 码 | 含义 |
|---:|---|
| 0 | success |
| 2 | 输入或配置错误 |
| 3 | 任务失败/评估未通过 |
| 4 | 预算耗尽 |
| 5 | 已取消 |
| 6 | 持久化失败 |
| 7 | 事件日志非法 |

进度只打印 task ID、sequence、固定事件名和固定状态名。致命/终端错误只打印验证过的 ID、固定 status/error code 和固定摘要，不反射内部 message。最终文本最多渲染 8192 字节，无效 UTF-8 和终端控制字符可见转义，截断不切断 code point。

### 16.2 配置

`src/config/runtime_config.h` 与 `src/config/runtime_config.cpp`：

- 要求 `AGENT_BASE_URL`、`AGENT_MODEL`；
- 要求 `AGENT_API_KEY` 与 `AGENT_AUTH_TOKEN` 恰好一个；
- 正整数解析无尾随垃圾且检查范围；
- opt-in flag 必须精确为 `0` 或 `1`；
- build timeout 限制 1–600 秒；
- RAG top-k 限制 1–20，timeout 1–60 秒；
- `.env` 只有显式 `--env-file` 才加载，不向上搜索；
- dotenv 库诊断被静默，避免把畸形秘密行打印出来。

`.env.example` 只包含空凭据字段和安全默认值。真实 `.env`、runtime data 和 RAG 索引不应提交 Git。

## 17. 组合根和系统适配器

`src/main.cpp` 是唯一 composition root：

1. 先解析启动参数；
2. 对 verify/evaluate 走完全离线路径；
3. 需要运行任务时才加载 env/config；
4. 创建 CPR transport 和 Anthropic client；
5. 创建 DirectProcessRunner；
6. 创建 workspace gateway，可选加入 CMake gateway；
7. 根据开关选择 Python RAG 或 empty knowledge；
8. 创建 JSONL、clock、ID、cancellation 和 RuntimeEngine；
9. 用窄 callback 把 `run/resume/verify` 交给 CLI。

`src/adapters/system/system_clock.h` 与 `src/adapters/system/system_clock.cpp` 提供 UTC 时间戳和 monotonic 毫秒。

`src/adapters/system/random_id_generator.h` 与 `src/adapters/system/random_id_generator.cpp` 用系统随机源生成合法 task/correlation ID。

`src/adapters/system/signal_cancellation.h` 与 `src/adapters/system/signal_cancellation.cpp` 把 SIGINT 变成进程级原子取消标记，由 Runtime 在下一次外部调用前观察。

## 18. 全部生产文件地图

下面逐个列出 `src` 与 `rag` 下的生产文件，便于从真实工作流反向查代码。

### 18.1 Domain

| 文件 | 职责 |
|---|---|
| `src/domain/value.h` | JSON-neutral 递归值声明 |
| `src/domain/value.cpp` | 递归值构造、访问与比较 |
| `src/domain/result.h` | 显式成功/失败容器 |
| `src/domain/runtime_error.h` | 稳定错误码和内部错误数据 |
| `src/domain/model_types.h` | 消息、工具、evidence、请求/响应、stop 规则 |
| `src/domain/evidence_validation.h` | EvidencePack 校验接口 |
| `src/domain/evidence_validation.cpp` | UTF-8、大小、唯一性和 metadata 限界 |
| `src/domain/task_state.h` | 状态、预算、用量和完整可恢复状态 |
| `src/domain/runtime_event.h` | 14 种事件 payload 与 envelope |
| `src/domain/runtime_event.cpp` | payload 到 EventKind 的总映射 |

### 18.2 Application

| 文件 | 职责 |
|---|---|
| `src/application/state_reducer.h` | 单事件 reduce 和全日志 replay API |
| `src/application/state_reducer.cpp` | 全部状态转换和防伪不变量 |
| `src/application/runtime_engine.h` | run/resume、结果和安全进度类型 |
| `src/application/runtime_engine.cpp` | append-before-commit 的单 Agent 循环 |
| `src/application/task_evaluator.h` | 固定评估结果结构 |
| `src/application/task_evaluator.cpp` | replay 后的离线指标和 pass 判定 |

### 18.3 Ports

| 文件 | 职责 |
|---|---|
| `src/ports/model_client.h` | 模型端口 |
| `src/ports/tool_gateway.h` | 工具端口及工作区上下文 |
| `src/ports/knowledge_provider.h` | 知识检索端口 |
| `src/ports/event_store.h` | 事件追加/读取端口 |
| `src/ports/process_runner.h` | 直接进程请求/输出/端口 |
| `src/ports/clock.h` | UTC/monotonic 时钟端口 |
| `src/ports/id_generator.h` | ID 端口 |
| `src/ports/cancellation.h` | 取消端口 |

### 18.4 Provider、JSON 与持久化 Adapters

| 文件 | 职责 |
|---|---|
| `src/adapters/anthropic/http_transport.h` | 中性 HTTP POST seam |
| `src/adapters/anthropic/cpr_http_transport.h` | CPR transport 声明 |
| `src/adapters/anthropic/cpr_http_transport.cpp` | 无重定向 POST 与错误归一化 |
| `src/adapters/anthropic/anthropic_messages_client.h` | Anthropic 配置和 ModelClient 声明 |
| `src/adapters/anthropic/anthropic_messages_client.cpp` | 请求编码、evidence 标记、响应/stop 校验 |
| `src/adapters/json/value_json.h` | Value codec 声明 |
| `src/adapters/json/value_json.cpp` | Value 与 nlohmann JSON 显式转换 |
| `src/adapters/persistence/event_json.h` | RuntimeEvent codec 声明 |
| `src/adapters/persistence/event_json.cpp` | 精确 versioned event schema |
| `src/adapters/persistence/jsonl_event_store.h` | JSONL store 和安全 task read API |
| `src/adapters/persistence/jsonl_event_store.cpp` | 安全 append/read、逐行 decode、完整 replay |

### 18.5 Workspace、构建和进程 Adapters

| 文件 | 职责 |
|---|---|
| `src/adapters/workspace/workspace_types.h` | 文件工具内部结果/Fault 数据 |
| `src/adapters/workspace/workspace_text.h` | UTF-8/SHA/分页/搜索原语声明 |
| `src/adapters/workspace/workspace_text.cpp` | 上述确定性纯算法实现 |
| `src/adapters/workspace/workspace_path_policy.h` | 相对路径策略声明 |
| `src/adapters/workspace/workspace_path_policy.cpp` | containment、链接和保护区策略 |
| `src/adapters/workspace/workspace_file_ops.h` | 文件 I/O 操作声明 |
| `src/adapters/workspace/workspace_file_ops.cpp` | 有界遍历、读取、CAS 原子写 |
| `src/adapters/workspace/workspace_tool_gateway.h` | 五工具 Gateway 声明 |
| `src/adapters/workspace/workspace_tool_gateway.cpp` | schema、参数/结果限界与 Fault 映射 |
| `src/adapters/process/direct_process_runner.h` | 直接进程实现声明 |
| `src/adapters/process/direct_process_runner.cpp` | Windows/POSIX 进程、管道、超时、输出限界 |
| `src/adapters/build/cmake_tool_gateway.h` | 三个构建工具声明 |
| `src/adapters/build/cmake_tool_gateway.cpp` | 固定 CMake/CTest argv 与结果映射 |
| `src/adapters/tools/composite_tool_gateway.h` | 多 Gateway 聚合声明 |
| `src/adapters/tools/composite_tool_gateway.cpp` | 唯一名校验和工具路由 |

### 18.6 RAG、空实现和系统 Adapters

| 文件 | 职责 |
|---|---|
| `src/adapters/rag/python_rag_knowledge_provider.h` | Python RAG 配置和端口实现声明 |
| `src/adapters/rag/python_rag_knowledge_provider.cpp` | 固定子进程协议、citation 和 evidence 校验 |
| `src/adapters/empty/empty_knowledge_provider.h` | 空 knowledge 声明 |
| `src/adapters/empty/empty_knowledge_provider.cpp` | 返回空 EvidencePack |
| `src/adapters/empty/empty_tool_gateway.h` | 空 tool gateway 声明 |
| `src/adapters/empty/empty_tool_gateway.cpp` | 无工具定义的实现 |
| `src/adapters/system/system_clock.h` | 系统时钟声明 |
| `src/adapters/system/system_clock.cpp` | UTC 和 monotonic 实现 |
| `src/adapters/system/random_id_generator.h` | 随机 ID 生成器声明 |
| `src/adapters/system/random_id_generator.cpp` | 合法 task/correlation ID 实现 |
| `src/adapters/system/signal_cancellation.h` | SIGINT 取消声明 |
| `src/adapters/system/signal_cancellation.cpp` | 原子取消标记实现 |

### 18.7 CLI、配置、入口和 Python

| 文件 | 职责 |
|---|---|
| `src/cli/cli_app.h` | 命令 callback、退出码和 CLI 声明 |
| `src/cli/cli_app.cpp` | 参数校验、安全渲染和命令分派 |
| `src/config/runtime_config.h` | Environment 抽象和 RuntimeConfig |
| `src/config/runtime_config.cpp` | env/dotenv 严格解析和默认值 |
| `src/main.cpp` | composition root 和跨平台入口 |
| `rag/agent_rag/__init__.py` | Python 包/schema 标识 |
| `rag/agent_rag/protocol.py` | 精确 stdin/stdout JSON 协议 |
| `rag/agent_rag/indexer.py` | 受限遍历、分块、SQLite 原子建库 |
| `rag/agent_rag/retriever.py` | tokenizer、只读 SQLite、BM25 排序 |
| `rag/agent_rag/cli.py` | build/query 命令和固定错误输出 |
| `rag/agent_rag_cli.py` | Python sidecar 可执行入口 |

## 19. 测试结构与证据边界

测试目录按生产层次组织：

- `tests/domain`：值、Evidence、事件和状态基础规则；
- `tests/application`：Reducer、Runtime、恢复、预算、评估；
- `tests/adapters`：Provider、JSONL、文件、进程、构建、RAG；
- `tests/cli`：命令形状、退出码、隐私、Unicode；
- `tests/integration`：真实本地文件、CMake、CTest、Python RAG 和崩溃恢复；
- `rag/tests`：Python index/query/protocol 单元测试；
- `tests/live`：只有 opt-in 才构建的真实 Provider smoke。

当前完成证据包括 Windows 主构建的 22/22 离线 CTest、关键 C++ 路径的 WSL/g++ C++17 警告即错误构建、Python RAG 测试，以及本文第 4 节的真实 42 事件工作流。没有运行需要真实凭据的 Provider 网络测试，所以不能声称 live Provider 已验证。

## 20. 怎样安全扩展

添加第二 Provider：实现 `ModelClient` 和必要的 `HttpTransport`，保持 Domain stop/content 契约不变，先补 Adapter 负向矩阵测试。

添加新工具：实现独立 `ToolGateway`，使用 closed schema、固定程序或具体 API，返回有界结构化结果，再交给 `CompositeToolGateway`。不要给模型一个通用 Shell 逃生口。

添加新知识后端：实现 `KnowledgeProvider`，输出仍必须通过同一个 `EvidencePack` 验证，并让 Reducer 绑定真正发送给模型的 evidence。

添加更强恢复：可以在事件中持久化跨尝试累计耗时、物理 attempt ID 或幂等键，但必须先写清新事件语义并扩展 Reducer；不能只在 Runtime 里加一个临时计数。

多 Agent 是未来阶段。它需要单独设计任务所有权、消息协议、冲突解决、调度、公平性、总预算和跨 Agent 可审计性。当前框架先把一个 Agent 的状态、工具、安全、恢复、RAG 和评估做成可靠基座，不用尚未设计的协同概念污染这个状态机。

## 21. 最短上手路线

1. 先跑离线 22 项 CTest，确认本机工具链；
2. 复制 `.env.example` 为本地 `.env`，不要提交；
3. 只对可信工作区开启 build tools；
4. 从经过选择的 `docs` 或源码目录手工构建 RAG 索引；
5. 配置绝对 canonical 的 RAG script/index 路径；
6. 执行 `run`；
7. 若进程在非终端状态中断，用 task ID 执行 `resume`；
8. 用 `verify-log` 检查日志，用 `evaluate-log` 查看固定指标；
9. 需要调查时直接阅读 JSONL，但把它当作包含用户/模型内容的私有审计记录。

如果只记住一条工程原则，请记住：**模型可以提出行动，但只有受控 Adapter 能产生外部效果；每个状态变化必须先通过 Reducer、再耐久落盘，最后才算真的发生。**
