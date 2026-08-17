# Runtime Kernel 架构设计

## 1. 背景与范围

AgentFramework 的 V1 最终目标是让单个 Coding Agent 能够自主完成一个完整 Issue：理解任务、检索上下文、制定计划、修改代码、构建测试、根据失败继续修复，并在完成或无法继续时给出可验证结论。V1 分为五个独立子项目：

1. Runtime Kernel；
2. Coding Tool Layer；
3. Python RAG/知识库外挂；
4. Autonomous Issue Workflow；
5. Recovery & Evaluation。

本文只设计第一个子项目 Runtime Kernel。后续工具和 RAG 在本阶段只表现为稳定 Port 与测试替身，不实现真实能力。

## 2. 已确认决策

- 主 Runtime 使用 C++17，从空仓库重新设计和实现，不复制 `D:\Users\Lenovo\AGENT` 的课程文件。
- 第一版只提供 CLI，不同时实现 HTTP 服务或 TUI。
- 第一个真实模型 Adapter 使用 Anthropic-compatible Messages 协议。
- 核心采用显式状态机、Ports/Adapters 和追加式事件日志。
- 当前状态保存在内存 `TaskState`；事实记录保存在每任务一个 JSONL 文件中。
- 第一阶段保持单进程、单任务、单线程、顺序执行，不并发执行工具。
- 完整断点恢复、自动重试和幂等执行属于后续 Recovery 子项目。

## 3. 目标

Runtime Kernel 必须提供以下能力：

- 用明确状态和合法转移表达一次 Agent 任务；
- 通过统一 `ModelClient` 调用真实或 Fake 模型；
- 通过 Port 隔离未来 Coding Tools 和 Python RAG；
- 在每次状态变化和外部调用边界写入可审计事件；
- 从完整事件序列重放出同一个最终 `TaskState`；
- 通过预算和终态阻止无限模型循环或终态后的额外调用；
- 把协议、持久化和 CLI 细节限制在 Adapter 内部；
- 在 Windows/MSVC 环境下完成 C++17 构建、测试和 CLI 运行。

## 4. 非目标

本阶段不实现：

- 真实文件读取、搜索、编辑、构建、测试或 Git 工具；
- 文档摄取、切块、Embedding、向量数据库、重排或真实 RAG；
- 自动规划完整 Issue 的工作流；
- 断点续跑、崩溃后自动恢复、自动重试或调用幂等性；
- 多任务并发、后台任务、子 Agent 或多 Agent；
- HTTP 服务、TUI、桌面界面、MCP 或插件系统；
- 多模型路由、Fallback 或多个生产 Provider Adapter。

## 5. 架构

### 5.1 依赖方向

```text
CLI / Concrete Adapters
        |
        v
Application RuntimeEngine
        |
        v
Domain Types + Port Interfaces
```

Domain 不依赖 HTTP、文件系统、CLI、具体 Provider 或外部进程。Application 只依赖 Domain 和 Port。Adapter 实现 Port，并负责把外部格式转换为内部类型。Composition Root 负责创建和连接对象。

### 5.2 Domain

Domain 保存不依赖基础设施的核心类型：

- `TaskId`；
- `TaskStatus`；
- `TaskState`；
- `Message` 与内容块；
- `ToolDefinition`、`ToolCall` 与 `ToolResult`；
- `Evidence` 与 `EvidencePack`；
- `RuntimeEvent`；
- `RuntimeError`；
- `RuntimeBudgets` 与使用量。

`StateReducer` 是纯函数：给定旧状态和一个合法事件，返回新状态。相同初始状态和相同事件序列必须得到相同最终状态。

### 5.3 Application

`RuntimeEngine` 是唯一任务编排器，职责是：

1. 接收 Issue、workspace 和预算；
2. 验证当前状态是否允许下一步；
3. 请求相关知识上下文；
4. 组装内部 `ModelRequest`；
5. 调用 `ModelClient`；
6. 处理最终文本或工具调用；
7. 按顺序调用 `ToolGateway`；
8. 生成、持久化并应用事件；
9. 在终态返回结构化 `RuntimeResult`。

`RuntimeEngine` 不解析 Provider JSON，不写 JSONL 字符串，不读取终端，也不知道未来 Python RAG 的通信方式。

### 5.4 Ports

#### ModelClient

接受统一的 `ModelRequest`，返回统一的 `ModelResponse`。内部响应必须表达最终文本、工具调用、停止原因、用量和 Provider 请求标识。Provider 的原始 JSON 不进入 Domain。

#### ToolGateway

提供当前可用工具定义，并执行结构化 `ToolCall`。本阶段生产配置使用空工具集合；单元测试使用 Fake 来验证工具循环和多工具顺序。

#### KnowledgeProvider

根据 Issue、workspace 和当前状态返回带稳定来源标识的 `EvidencePack`。本阶段生产配置返回空 Evidence；后续 Python RAG Adapter 替换它。

#### EventStore

为单个任务顺序追加事件，并读取事件用于验证重放。Port 不暴露 JSONL 细节。

#### Clock 与 IdGenerator

时间戳、Task ID 和 correlation ID 通过可替换 Port 产生，使单元测试能够使用确定值，不依赖真实时钟和随机数。

### 5.5 Adapters

- `AnthropicMessagesClient`：映射内部消息、工具定义、`tool_use`、`tool_result`、停止原因和错误；只有此模块理解 `/v1/messages` 协议。
- `JsonlEventStore`：序列化、顺序追加、刷新、读取和验证事件行。
- `EmptyToolGateway`：返回空工具定义，不执行真实工具。
- `EmptyKnowledgeProvider`：返回空 EvidencePack。
- `CliApp`：解析命令行、显示脱敏状态、处理取消信号并返回稳定退出码。
- `main.cpp`：读取配置并组合具体 Adapter，不包含任务状态逻辑。

## 6. 状态机

### 6.1 状态

非终态：

- `Created`；
- `PreparingContext`；
- `AwaitingModel`；
- `AwaitingTool`。

终态：

- `Completed`；
- `Failed`；
- `BudgetExceeded`；
- `Cancelled`。

### 6.2 规则

- `Created` 只能进入 `PreparingContext` 或直接进入失败/取消终态。
- 上下文准备完成后进入 `AwaitingModel`。
- 模型返回最终文本时进入 `Completed`。
- 模型返回工具调用时进入 `AwaitingTool`。
- 全部工具结果形成下一轮消息后重新进入 `PreparingContext`。
- 任何非终态都可以因不可恢复错误、预算或用户取消进入对应终态。
- 终态拒绝任何新的模型、知识或工具调用事件。

非法转移是 Runtime 缺陷，不自动修正；它必须产生明确错误并使测试失败。

### 6.3 事件与状态转移归属

- `TaskStarted` 创建初始 `Created` 状态。
- `ContextPreparationStarted` 使 `Created` 或完成一轮工具处理后的 `AwaitingTool` 进入 `PreparingContext`。
- `ContextPrepared` 使 `PreparingContext` 进入 `AwaitingModel`。
- `ContextPreparationFailed` 记录 KnowledgeProvider 调用失败，随后由 `TaskFailed` 进入 `Failed`。
- `ModelCallStarted` 表示模型调用在途，状态保持 `AwaitingModel`。
- `ModelCallSucceeded` 返回工具调用时进入 `AwaitingTool`；返回最终文本时保持 `AwaitingModel`，随后由 `TaskCompleted` 进入 `Completed`。
- `ModelCallFailed` 记录调用失败，随后由 `TaskFailed` 进入 `Failed`。
- `ToolCallStarted`、`ToolCallSucceeded` 和 `ToolCallFailed` 记录单个工具调用边界；只有 ToolGateway 基础设施失败才追加 `TaskFailed` 并进入 `Failed`。
- `TaskBudgetExceeded` 和 `TaskCancelled` 分别进入对应终态。

调用边界事件不代替任务终态事件。一个模型或基础设施失败必须先记录具体调用失败，再记录任务为什么结束。

## 7. 事件模型与持久化

### 7.1 事件信封

每行 JSONL 是一个完整事件，包含：

- `schema_version`；
- `sequence`；
- `task_id`；
- `timestamp`；
- `event_type`；
- `correlation_id`；
- `payload`。

`sequence` 从 1 开始，在单个任务内连续且严格递增。`schema_version` 用于后续兼容，不在第一阶段实现迁移系统。

### 7.2 主要事件

- `TaskStarted`；
- `ContextPreparationStarted`；
- `ContextPrepared`；
- `ContextPreparationFailed`；
- `ModelCallStarted`；
- `ModelCallSucceeded`；
- `ModelCallFailed`；
- `ToolCallStarted`；
- `ToolCallSucceeded`；
- `ToolCallFailed`；
- `TaskCompleted`；
- `TaskFailed`；
- `TaskBudgetExceeded`；
- `TaskCancelled`。

事件保存足以重建 TaskState 的规范化数据。敏感认证头和 API Key 永不进入事件 payload。

### 7.3 写入顺序

状态变化必须遵守：

```text
验证事件合法
→ 序列化事件
→ 追加完整一行并 flush
→ 写入成功
→ StateReducer 应用事件
```

若 append 或 flush 失败，该事件不得应用到内存状态。因为 EventStore 本身不可用，Runtime 只能向 CLI 返回 `PersistenceFailure`；不能假装已经持久化一个 `TaskFailed` 事件。

第一阶段对缺行、重复 sequence、未知 schema、非法 JSON 或非法状态转移只报告验证失败，不自动修复或截断日志。

### 7.4 文件位置

```text
runtime_data/tasks/<task_id>/events.jsonl
```

`runtime_data/` 已被 `.gitignore` 排除。事件可能包含 Issue、代码上下文、工具结果和模型文本，因此不得进入 Git。

## 8. 运行数据流

1. CLI 接收 Issue、workspace 与预算。
2. Engine 持久化并应用 `TaskStarted`。
3. Engine 检查取消与预算，持久化 `ContextPreparationStarted`，再调用 KnowledgeProvider；调用失败时依次持久化 `ContextPreparationFailed` 和 `TaskFailed`。
4. Engine 持久化 `ContextPrepared`，组装 ModelRequest。
5. Engine 持久化 `ModelCallStarted`，再调用模型。
6. 成功响应映射为内部类型并持久化 `ModelCallSucceeded`；协议或调用失败依次持久化 `ModelCallFailed` 和 `TaskFailed`。
7. 最终文本产生 `TaskCompleted`。
8. 工具调用按模型响应中的原始顺序逐个执行，每个调用独立产生 started 和 succeeded/failed 事件。
9. 全部工具结果组成一个 Messages 协议用户消息，然后持久化下一轮 `ContextPreparationStarted`，进入上下文准备和下一轮模型请求。

第一阶段不并发执行多个工具。工具命令返回非零退出码、编译失败等业务结果仍通过成功返回的结构化 `ToolResult{is_error=true}` 表达，并记录 `ToolCallSucceeded`；只有 ToolGateway 未能返回任何可信结果时才记录 `ToolCallFailed` 和 `TaskFailed`。

## 9. 预算与停止

`RuntimeBudgets` 至少包含：

- 最大模型轮次；
- 最大工具调用次数；
- 最大任务运行时间；
- 单次模型请求超时。

每次知识、模型或工具外部调用前检查相应预算。达到预算后写入 `TaskBudgetExceeded`，不再发起调用。用户取消写入 `TaskCancelled`。终态写入后 Engine 必须立即停止。

第一阶段不做自动重试。请求开始但结果未知的情况保留在事件日志中，由后续 Recovery 子项目定义处理策略。

## 10. 配置与密钥

配置从环境变量或被 Git 忽略的 `.env` 读取。配置项包含 Provider Base URL、Model ID、API Key、超时和预算。API Key 不允许作为普通 CLI 参数，避免出现在进程列表和终端历史中。

日志、异常、CLI 输出和事件 payload 必须对认证头和密钥进行脱敏。发送到自定义 Base URL 时只发送为该 URL 显式配置的凭证，不继承其他服务的认证值。

## 11. 错误处理

Runtime 使用结构化错误码，不通过解析异常文本决定控制流。错误类别包括：

- `InvalidInput`；
- `InvalidConfiguration`；
- `PersistenceFailure`；
- `TransportFailure`；
- `RequestTimeout`；
- `HttpFailure`；
- `ProtocolFailure`；
- `DependencyUnavailable`；
- `InvalidTransition`；
- `BudgetExceeded`；
- `Cancelled`。

Adapter 在边界捕获库异常并映射为上述内部错误。RuntimeEngine 决定状态转移；Adapter 不直接修改 TaskState。

KnowledgeProvider 基础设施错误先记录 `ContextPreparationFailed`，再记录 `TaskFailed`。模型非 2xx、超时、无法解析响应或缺少必需字段均先记录 `ModelCallFailed`，再记录 `TaskFailed` 并进入 `Failed`。ToolGateway 基础设施错误同样先记录 `ToolCallFailed`，再记录 `TaskFailed`。EventStore 失败是特殊情况：由于无法可靠写入终态事件，Engine 立即停止并通过 CLI 非零退出码报告。

CLI 输出错误码、task_id 和可操作摘要，不输出密钥、完整认证头或不受控的大型响应正文。

## 12. CLI

第一阶段提供两个行为：

```text
agent run --workspace <path> --issue <text>
agent verify-log --events <events.jsonl>
```

`run` 执行一个前台任务并实时显示脱敏状态事件。`verify-log` 读取事件、验证 sequence 和状态转移，并输出重放后的终态。

稳定退出码至少区分：成功、输入/配置错误、任务失败、预算结束、用户取消和持久化失败。具体数字在实施计划中固定，并由 CLI 测试锁定。

## 13. 测试设计

### 13.1 Domain 单元测试

- 每条合法状态转移；
- 每条非法状态转移；
- 终态不可继续；
- 预算边界的等于、少于和超过情况；
- 相同事件序列重放得到相同状态；
- Unicode Issue、路径和消息内容不丢失。

### 13.2 RuntimeEngine 测试

使用 Fake Clock、ID、Model、Tool、Knowledge 和 EventStore 验证：

- 单轮最终文本；
- 工具调用后进入下一轮模型请求；
- 一次响应中多个工具保持原始顺序；
- 普通工具错误作为 `ToolResult{is_error=true}` 返回模型，并仍记录 `ToolCallSucceeded`；
- ToolGateway 基础设施失败记录 `ToolCallFailed` 和 `TaskFailed`；
- Provider、KnowledgeProvider 和 ToolGateway 基础设施失败；
- KnowledgeProvider 失败按顺序记录 `ContextPreparationFailed` 和 `TaskFailed`；
- 最大模型轮次、工具次数和时间预算；
- 用户取消；
- EventStore append 失败时状态不先行改变；
- 终态后没有额外外部调用。

### 13.3 JSONL Adapter 测试

- 逐行追加与 flush；
- sequence 连续；
- 完整日志读取和重放；
- 非法 JSON、缺行、重复 sequence、未知 schema 和非法转移；
- Windows Unicode 路径；
- 日志中不存在测试密钥。

### 13.4 Anthropic Adapter 测试

通过 Fake HTTP Transport 验证：

- 内部消息和工具定义映射到 Messages 请求；
- 文本、`tool_use`、`tool_result` 和停止原因的双向映射；
- 多内容块顺序；
- 非 2xx、超时、空正文、非法 JSON 和缺少字段；
- 错误与日志不包含认证值。

普通自动测试不访问网络。配置真实凭证时，另提供显式启用的真实模型 smoke test；没有凭证时必须明确报告跳过，不能把跳过描述为通过真实调用。

### 13.5 CLI 与集成测试

- 参数解析和稳定退出码；
- Fake Model 下从 Issue 到 Completed 的完整运行；
- 产生事件文件并通过 `verify-log` 重放；
- MSVC/C++17 构建和 CTest；
- 真实控制台中的 UTF-8 输入输出。

## 14. 初步文件职责

```text
CMakeLists.txt
src/
  main.cpp
  domain/
    task_state.*
    runtime_event.*
    runtime_error.*
    model_types.*
  application/
    runtime_engine.*
    state_reducer.*
  ports/
    model_client.h
    tool_gateway.h
    knowledge_provider.h
    event_store.h
    clock.h
    id_generator.h
  adapters/
    anthropic/
    persistence/
    empty/
  cli/
    cli_app.*
tests/
  domain/
  application/
  adapters/
  cli/
```

具体文件合并或拆分由实施计划根据每个类型的实际大小确定，但依赖层级不得反转。

## 15. 验收标准

Runtime Kernel 完成必须同时满足：

1. Windows/MSVC 下 CMake 配置、构建和 CTest 全部成功；
2. CLI 能用 Fake Model 完成一个任务并生成 JSONL；
3. `verify-log` 能重放该 JSONL，并得到与运行结束时一致的 TaskState；
4. 工具循环、多工具顺序、普通工具失败、Provider 失败、预算和取消均有测试；
5. EventStore 失败时不会先改变内存状态；
6. Anthropic Adapter 的请求与响应映射由离线测试覆盖；
7. 有凭证时，显式真实模型 smoke test 成功；无凭证时准确报告未执行；
8. 密钥不出现在 Git、事件、日志、测试输出或最终回复中；
9. 工作树干净，设计、计划和实现按职责形成可解释 Commit；
10. 每个 Commit 均推送到本地 `backup`，并验证两端 Commit ID 一致。
