# Latency → 自我优化路线图（保留特色，不盲照搬）

**修订说明（v2，2026-09-18）**：v1（§3 之前已删）的思路是"凡 cc-haha 有、我没有 → 列改进项"。v2 调整三条原则：
1. **保留特色**：凡 latency 已超 cc-haha、或 cc-haha 的实现是 UI/TS 框架耦合物的，**不照搬**。v1 误把 `Tool.ts` 的 30 方法当设计模式范本，实际是 794 行 React/Ink 集成文件。
2. **真问题优先**：改进项按"用户感知强度 × 实施依赖"排序，不是按"与 cc-haha 的差距大小"。
3. **渐进抽象**：抽象服务于需求，不为"看起来像 cc-haha"而抽象。

**范围**：`AgentFramework-latency/` 检出分支 `perf/cli-latency-parity`。
**合并策略**：P0 跑通后合回 `AgentFramework/` main；合并时**不重 stage 7+ GB runtime** 到 latency（参 `feedback_branch_strategy.md`）。
**范本参考**：`E:/desktop/How_to_build_a_agent/cc-haha-main/`（仅取设计意图，**不复制其 UI 框架 / TS 范式 / 远程 feature flag**）。

---

## §0 准则

### 0.1 不照搬的清单（v1 误抄的部分）

| cc-haha 的"特性" | 为什么 latency 不做 |
|---|---|
| `Tool.ts` 的"30 方法"契约（`SetToolJSXFn` / `ToolProgressType`） | 这是 React/Ink UI 框架耦合物——`SetToolJSXFn` 用于在终端里渲染 tool UI，TS-idiomatic。latency 走 ANSI SGR 直绘，**5 个核心方法 + opt-in 派生**足够 |
| `bun:bundle` 的 `feature(...)` 编译期剥离 | C++ 无等价物；用 CMake `if(ENABLE_X)` 项目级开关即可 |
| `GrowthBook` 远程 feature flag | 没有远程配置中心需求；远程 flag 会引入额外攻击面 |
| `bridge/` / `MailboxBridge` / `inbox-poller` / `SSH session` | 多端扩展（VS Code / Web / SSH），不是 parity 必需 |
| `outputStyles/` + 自定义 presenter 主题 | latency 的 `kFrames/kVerbs` 已经做了一定的主题化；P2 期再做 plugin 化 |
| `task teams` / 多 agent 协作 | latency 当前定位是 single-agent；multi-agent 是 P3 远期 |

### 0.2 已超 cc-haha 的能力（保留不动）

下面这些是**我们做得比范本更好的**，不要因为范本里有"类似功能"就改：

- **LatencyTraceSink + 4 stage**（`process_start / submit / first_text_received / first_text_rendered`，main.cpp:140/507、runtime_engine.cpp:295-298、anthropic_messages_client.cpp:381）。cc-haha 的 telemetry 是隐式的、埋在 React render 里；latency 的 trace 是显式 `emit_latency_sample` 跨模块调用。T07 改造时要把这条**作为已有可观测资产**同步用。
- **证据 isolation + RAG sidecar**（`reproc++` 子进程 + JSONL 协议 + `native_rag_pack_verifier` + 进程自愈）。比 cc-haha 简单 RAG 更严谨——RAG 数据与 agent runtime **物理隔离**。
- **`ToolExecutionContext` 把 workspace 提到 context**（`tool_gateway.h:11-13`），不在 tool `name` 里塞语义。解耦比 cc-haha `ToolUseContext` 的 794 行 context 对象更克制。
- **`ToolResult { tool_call_id, content, is_error }`**（`model_types.h:37-41`）把错误作为正常返回路径，**不是异常类型**——retryable 标志通过 `is_error` 而不是异常传播。
- **`bounded_json_value` fail-closed**（`workspace_tool_gateway.cpp:411-415`）防止参数 vectored 攻击。
- **8 态 TaskStatus + 双 reducer**（state/session）+ JSONL 持久化 + replay——已具备 cc-haha 级别的 replay-debug 能力，且更轻量。
- **`OperationContext` 同时携带 `Cancellation + deadline`**（`operation_context.h:23-46`）——port 调用的横切关注点已经统一。

### 0.3 排序准则

| 优先级 | 判定 | 例子 |
|---|---|---|
| **P0** | 用户立即能感受到的延迟/失败，且实施依赖链短 | T01（3 工具并发） / T02（529 重试） / T03（context 爆炸） |
| **P1** | 合并后下一迭代，依赖 P0 落地 | T07（fsync 周期化） / T08（cost 实时） / T09（diff） / T10（/plan） / T25（reactive compact） / T13（hook 声明式） |
| **P2** | 远期 par，与产品决策挂钩 | T22（sandbox） / T11（permission） / T12（provider 抽象） / T14（MCP） 等 |

---

## §1 P0：用户立即能感受到的延迟/失败（5 条）

### T01 并行 tool 执行（架构设计）★ **修正 v1 错误**

- **问题陈述**：`RuntimeEngine::AwaitingTool`（`runtime_engine.cpp:562-637`）串行 N 个 tool_use，wall-clock = sum(各耗时) 而非 max。单次响应含 3 个独立 `read_file` 场景，从 ~1.5s 放大到"自然并发 ~500ms"。
- **改后方案**：
  - 保持 `ToolGateway::execute` 同步签名不变；新增 `Tool::isConcurrencySafe()` 默认 `false`（fail-closed）。
  - runtime 在 `AwaitingTool` 收到 N 个 `ToolUseBlock` 时按 `state->budgets.max_parallel_tools`（默认 4）分批 `std::async` 启动。
  - 事件顺序确定：所有 `ToolCallStartedPayload / SucceededPayload / FailedPayload` 仍按 reducer 顺序追加。
- **★ v1 错误修正**：`WorkspaceToolGateway` 5 个工具**不全部**标 `true`。`replace_text / write_file` 改 workspace 文件系统状态，且依赖 `expected_sha256`（line 391/401）防 race——并发会破坏此约束。
  - **正确默认**：`list_files / read_file / search_text` → `true`；`replace_text / write_file` → `false`（后续可加读写锁 opt-in）。
- **风险**：`OperationContext` 引入并发后任何 mutable state port 实现都需审计；缓解：默认 `false` 强制逐个 opt-in。
- **验收**：集成测试断言 3 工具场景 wall-clock 接近 max(各耗时)；现有 8 个 integration test 仍绿；recovery 路径在并行批次中能识别"未完成的 active_tool_call_id"。
> 范本：cc-haha `src/services/tools/StreamingToolExecutor.ts`（取"并发调度"意图，不取 React 集成）。

### T02 Provider 重试 + 退避（代码细节）

- **问题陈述**：`AnthropicMessagesClient::complete`（`anthropic_messages_client.cpp:402-411`）在 status >= 500 时直接 `retryable=true` 写进 RuntimeError 返回，**无重试**。529（overloaded）/ 5xx 抖动让 turn 直接失败，session 重放成本高。
- **改后方案**：`complete`/`post_stream` 套 `retry_with_backoff`（base=500ms、cap=8s、jitter=±20%、max=3）；`status >= 500 || error.code == TransportFailure` 参与重试；**429 fail-fast**（不与 5xx 同策略）；每次重试前检查 `options.cancellation->requested()`；每次重试向 `LatencyTraceSink` 上报 `kStageRetry`。
- **验收**：单元测试 mock 500/502/503/529 序列，最终成功且总延迟落入 [base, base*7]；trace 日志可按 `kStageRetry` 过滤统计。
> 范本：cc-haha `src/services/api/withRetry.ts`（取 backoff 算法，不取其 feature flag 机制）。

### T03 context 压缩分级（架构设计）★ **修正 v1 描述不完整**

- **问题陈述**：`ModelContextCompactor::compact`（`model_context_compactor.cpp:15-45`）单条 summarize：构造 prompt 让模型"返回 ≤ max_summary_bytes 累积摘要"。单 tool result 65KB 立刻撑爆 `compaction_threshold`（默认 64KB）。
- **★ v1 描述修正**：orchestrator **不应该是无条件串联**，应**按预算余量条件性触发**（参 cc-haha `feature('HISTORY_SNIP')` 的动态加载模式）。
- **改后方案**：新增 `services/compact/` 下 6 个独立策略类，每级有自己的触发条件：
  | 策略 | 触发条件 | 动作 | 调 LLM？ |
  |---|---|---|---|
  | `ToolResultBudget` | 单 tool_result > N bytes | 截断/丢弃最旧 | 否 |
  | `SnipOldToolResults` | 累计 tool_result > 阈值 | 截断中间最老 | 否 |
  | `MicrocompactThinking` | thinking 段累计 > 阈值 | 删除 thinking | 否 |
  | `CollapseAdjacentMessages` | 同 role 连续 ≥ K 条 | 合并 | 否 |
  | `AutoCompactSummary` | 总 token > 硬上限 | 调 model 摘要 | 是 |
  | `ReactiveCompact` | 用户/hook 触发 | 调 model 摘要 | 是 |
  - 前 4 级廉价（不调 LLM），按 token 余量条件触发；只在前 5 级全跑完仍不达标时才记 metric。
  - 通过 `LatencyTraceSink` 上报每级 token 节省百分比。
- **验收**：单元测试覆盖 6 级各自的触发条件；orchestrator 串联测试覆盖"snip 即可"的快路径；单 tool_result 65KB 场景下"snip"直接生效，省 1 次 summarize。
> 范本：cc-haha `src/services/compact/*.ts`（取独立策略 + 触发条件设计）。

### T04 Tool 抽象精简（架构设计）★ **拒绝照搬 30 方法**

- **问题陈述**：`ports/tool_gateway.h:15-22` 两个虚函数太薄；`CompositeToolGateway::execute` 不知道 tool 是否只读 / 是否可并发 / 渲染什么标题——这些全靠 presenter 端硬编码 `progress_tool_name`（`runtime_engine.cpp:28-45`）。
- **★ v1 错误修正**：cc-haha 的 `Tool.ts` 是 **794 行 React/Ink 集成文件**（`SetToolJSXFn` / `ToolProgressType` / `ToolUseContext` 含 commands/hooks/theme）——这是**前端 UI 框架产物**，不是通用设计模式。直接照搬 30 方法会把 C++ 头文件撑成 794 行。
- **改后方案**：扩 `Tool` 接口为 **5 个核心方法 + opt-in 派生类**：
  - 核心（每个 tool 必实现）：`name / description / input_schema / execute`
  - 关键（默认 false，fail-closed）：`isConcurrencySafe / isReadOnly`
  - 渲染（默认返回空，presenter 走 fallback）：`renderToolUseMessage / renderToolResultMessage`
  - 元信息（默认返回 name）：`getActivityDescription`
  - 安全（默认 deny）：`checkPermissions`（参 T11）
  - `build_tool(name, …)` 提供 fail-closed 默认实现（任一 method 未覆盖即视为不安全）。
- **验收**：编译通过；`WorkspaceToolGateway` 5 工具重写为 `build_tool()` 风格；新增单测覆盖 fail-closed 默认。
> 范本：cc-haha `Tool.ts` 只取"tool 自描述 + 渲染契约"的设计意图，**不取其 React UI 集成**。

### T05 SettingSource 分层（架构设计）

- **问题陈述**：`config/runtime_config.cpp`（800+ 行）所有配置从 `Environment` 单源（`getenv`）读；`.env` 通过 `load_explicit_env_file`（line 642）一次性灌进 process env。**无链式覆盖**——同一台机器上不同项目用不同 model/RAG pack/compaction 阈值无法实现；企业"policy 强制某些 key 不可改"也无着力点。
- **改后方案**：新增 `config/setting_source.{h,cpp}`，4 个源：
  - `PolicySource (read-only)` —— 最高优先，admin 部署
  - `ProjectSource` —— `.agentrc.json` 在 cwd
  - `UserSource` —— `%APPDATA%/agent/config.json`（Windows）/ `~/.config/agent/config.json`（Linux）
  - `EnvSource` —— process env（保留 `.env` 通过 `dotenv::init` 兼容现有脚本）
  - 合并顺序：Policy > Project > User > Env > BuiltIn
  - 新增 `--show-effective-config` 打印合并视图。
- **验收**：新增 `.agentrc.json` schema；单元测试覆盖 4 层优先级；现有部署（`.env` only）兼容。
> 范本：cc-haha `src/utils/settings/`（取 user/project/policy 分层意图）。

---

## §2 P1：合并后下一迭代（6 条）

### T07 Crash-safe session WAL（代码细节）★ **同步改造 LatencyTraceSink**

- **问题陈述**：`JsonlEventStore::append`（`jsonl_event_store.cpp:524-561`）→ `secure_append_line`（line 212-281 Windows）**每条 fsync**（line 277 `FlushFileBuffers`）。p99 写延迟被 fsync 框定，吞吐 ~50-200 events/s。
- **改后方案**：
  - 改为"32 events 或 500ms 触发的周期性 fsync"；新增 `committed_through_turn` 索引（每事件标 `committed=true/false`）。
  - **`recover_pending_turn`** 在启动时扫描到 `committed=false` 即从 `committed=true` 处截断，并在 trace 里打 'N events lost at gap'。
  - **同步改 `LatencyTraceSink`**（main.cpp:74-115）：当前每条 `stream_.flush()` 是另一处热点。改为"批量写出 + 周期 flush"，否则 T07 的吞吐提升会被 trace 抵消。
- **风险**：500ms 内最多丢 32 条事件，需 reducer 容错。缓解：`recover_pending_turn` 识别 seq gap 截断；ToolCallStarted 缺失则 ToolCallSucceeded 也丢弃。
- **验收**：吞吐提升 5-10x 到 2.3k events/s；新增 `crash_recovery_test` 集成测试覆盖 kill -9 后恢复；trace 写入吞吐同步提升。
> 范本：cc-haha `src/history.ts` 的 JSONL transcript 模式（取周期性 fsync 设计意图）。

### T08 Token / Cost 实时计数（代码细节）

- **问题陈述**：`TerminalPresenter::tick`（`terminal_presenter.cpp:286-304`）只渲染 `kSpinner + frame + shimmered_verb + elapsed_seconds`，无 token/cost。`RuntimeProgress`（`runtime_engine.h:60-66`）5 字段也无 usage 维度。`AnthropicMessagesClient::decode_response`（line 328-331）解出 `input_tokens / output_tokens` 但只写 `decoded` 字段。
- **改后方案**：
  - `RuntimeProgress` 增 `usage_delta { input_tokens, output_tokens, usd }`。
  - `TerminalPresenter` 在 phase row 下插入第二行 `tok in 1.2k / out 340 / $0.012`。
  - cost 按 `config.anthropic.model` 查表（claude-3-5-sonnet 输入 $3/M、输出 $15/M 等）。
  - **★ 与 T07 耦合**：T07 提速后 token 流式更密，T08 的"实时"端到端延迟需一并测，避免"准实时"假象。
- **验收**：presenter 单测断言出现该格式；live smoke 截图确认；trace 日志按 `kStageModelUsage` 过滤。
> 范本：cc-haha `src/components/Spinner.tsx` 的 token 显示（仅取 UI 形式，不取 React）。

### T09 Unified diff 渲染（代码细节）

- **问题陈述**：`WorkspaceToolGateway::replace_text`/`write_file`（`workspace_tool_gateway.cpp:535-574/575-621`）返回 `{"path", "created", "old_sha256", "new_sha256", "replacements", "bytes_written"}`——**不含 before/after 文本**。`TerminalPresenter::progress`（line 214-236）成功行只打印 `● done <tool_name>`，用户看不到模型"改了啥"。
- **改后方案**：
  - `write_json` 增加 `diff` 字段（统一 diff 格式）。
  - `TerminalPresenter` 在 success 行后接 ANSI 着色 `+`/`-` 行；CJK 列宽按 2 计算（与 `fit_line` 第 92 行 cells=2 逻辑一致）。
- **验收**：presenter 单测覆盖加/删/改三种 diff；CJK 宽度计算正确；live smoke 截图。
> 范本：cc-haha `src/utils/diff.ts`（取 diff 算法，不取 React 渲染层）。

### T10 `/plan` 命令（代码细节）

- **问题陈述**：`InteractiveCli::run`（`interactive_cli.cpp:382-555`）命令分发仅 9 个（`/exit /status /memory /memories /remember /forget /new /clear /resume`，line 436-545）。**无 `/plan`**。用户必须先 commit 才能看到结果，无法"先审后跑"。
- **改后方案**：
  - 新增 `/plan` 解析分支：`presentation.dry_run = true`，模型输出文本先入 `plan_buffer`（session 临时变量），用户 `y` 才 commit；`n` 丢弃；`edit` 进入 in-REPL 编辑。
  - `SessionEngine` 接受 `dry_run` flag；runtime 走"text-only response"快路径不发 tool_use。
- **验收**：REPL 集成测试断言"发 /plan 后模型回复进入 buffer、未确认前不动 `state.messages`"；现有 9 个命令不受影响。
> 范本：cc-haha `src/commands.ts` 的 `/plan`（取 plan mode 流程，不取 Ink JSX）。

### T25 Reactive compact（代码细节）★ **v1 P2 → v2 P1**

- **问题陈述**：v1 标 P2 不合理——`ModelContextCompactor::compact`（`model_context_compactor.cpp:15-45`）**只能**被动等阈值触发；用户在 context 快爆时无法主动触发；agent 自身无法主动提议。**这是当前真实用户痛点**，不是远期 par。
- **改后方案**：
  - `domain/model_types.h` 的 `ContentBlock` 新增 `CompactRequestBlock`（参 v1 std::variant 现有 `TextBlock/ToolUseBlock/ToolResultBlock`）。
  - `AnthropicMessagesClient::decode_response`（line 225-337）识别该 block 并 push。
  - `runtime_engine.cpp` 在 `ModelCallSucceededPayload` 处理时若含 `CompactRequestBlock` 即触发 `ReactiveCompact`（参 T03）。
  - Ctrl+C 长按（>500ms）走同一入口。
- **验收**：单测可注入 200k 上下文，触发后 token < 100k；Ctrl+C 长按触发路径被单测覆盖。
> 范本：cc-haha `src/services/compact/reactiveCompact.ts`。

### T13 PreToolUse / PostToolUse Hook（架构设计）

- **问题陈述**：`application/` 没有 `HookChain`；`runtime_engine.cpp` `AwaitingTool`（line 562-637）执行顺序硬编码；hook 配置无承载之处。
- **改后方案**：
  - 新增 `application/hook_chain.{h,cpp}`，定义 `Hook::preToolUse(call) / postToolUse(call, result)`。
  - `RuntimeEngine::AwaitingTool` 改造为过 hook 链后才发 `ToolCallStartedPayload`。
  - hook 配置从 `.agentrc.json` 的 `hooks` 段读取（与 T05 协同）。
  - 提供 1 个示例 hook（`audit_logger`）走 stdout——**这是与 cc-haha 不同之处**：cc-haha hook 系统深度耦合 React UI，latency 只做声明式 JSON hook + stdout 输出。
- **验收**：1 个示例 hook（自动追加 copyright header）通过单测；hook 链对 T11 Permission 和 T21 Proactive 是承接点。
> 范本：cc-haha `src/hooks/`（取声明式配置 + pre/post 钩子设计，不取 React UI 集成）。

---

## §3 P2：远期 par（14 条）

### T22 Sandbox 隔离（架构设计）★ **v2 提到 T11 之前**

- **问题陈述**：`DirectProcessRunner`/`ReprocJsonlProcess` 直接 fork/exec；`WorkspaceToolGateway` 5 工具基于 `files_.list/read/search/write` 直接操作文件系统；`policy_.parse`（`workspace_tool_gateway.cpp:436`）仅校验"路径必须在 workspace 内"——不限制 CPU/memory/网络。**模型被引导 `curl | sh` 主机立刻失陷**。
- **改后方案**：
  - 三平台 sandbox：macOS Seatbelt（`sandbox-exec`）/ Linux bubblewrap（`bwrap`）/ Windows Job Object + 命名空间（`CreateJobObject` + restricted token）。
  - 所有 `DirectProcessRunner` 子进程与 `WorkspaceToolGateway` 写操作走 sandbox 包装。
  - policy 用 `.agentrc.json` 的 `sandbox_profile` 字段（默认 `deny-network` / `deny-writes-outside-workspace`）。
  - **Windows 风险**：沙箱需要 `SeAssignPrimaryTokenPrivilege` 等特权，沙箱内调试会很痛。缓解：先在 Linux 上验证机制，Windows 适配后置。
- **验收**：workspace tool 在 sandbox 下执行，破坏 `..` 路径无效果；trace 上报 sandbox 包装耗时；为 T11 的 `auto mode` classifier 铺路。
> 范本：cc-haha `src/utils/sandbox.ts`（取 deny-by-default + 三平台设计意图）。

### T11 Permission 系统（架构设计）

- **问题陈述**：`ports/tool_gateway.h` 无 `checkPermissions`；`WorkspaceToolGateway::execute`（line 405-629）只做参数校验 + 路径检查，无"用户是否允许"环节。`interactive_cli.cpp` 无 `/permissions` 命令。
- **改后方案**：
  - `ToolGateway` 加 `checkPermissions(call, ctx) -> PermissionDecision`（参 T04 已加此方法）。
  - `PermissionMode` 枚举（`default / acceptEdits / plan / bypassPermissions / auto`）。
  - 配置走 `.agentrc.json` 的 `permissions` 段。
  - **v1 风险缓解**：先实现 `default + bypassPermissions` 两 mode；`auto mode`（带 classifier）放最后——classifier 需要 T22 sandbox 提供的执行边界。
  - REPL 加 `/permissions` 列出当前 mode 与待决项。
- **验收**：单元测试覆盖 5 个 mode 各自的 allow/deny/ask 路径；audit log 走 T13 hook。
> 范本：cc-haha `src/utils/permissions/`（取 5 个 mode 设计，不取其 bashClassifier）。

### T12 LLM Provider 抽象（架构设计）★ **修正 v1 改后方案**

- **问题陈述**：`ports/model_client.h:10-29` `complete(req)`/`complete(req, options)`；`AnthropicMessagesClient::complete`（`anthropic_messages_client.cpp:345-427`）硬编码 Anthropic 字段（`x-api-key`/`authorization`、`anthropic-version`、`stop_reason` 字面量映射、`cache_control`）。
- **★ v1 改后方案不彻底**：v1 说 "`runtime_engine.cpp` 的 `is_known_stop_reason_pair` 调用保持"——但**该函数实现（`model_types.h:80-95`）写死了 Anthropic 协议字符串字面量**：
  ```cpp
  case StopReason::EndTurn:    return raw_stop_reason == "end_turn";
  case StopReason::ToolUse:    return raw_stop_reason == "tool_use";
  ```
  这就是 application 层与 provider 字段耦合的真实凭据。
- **改后方案**：
  - `raw_stop_reason` 改为**中性 StopReason 枚举**（`AnthropicMessagesClient::map_stop_reason` 内部仍保留 raw 字符串做 decode，但不再外泄）。
  - `is_known_stop_reason_pair` 改为**单参数 StopReason**（去掉 raw 字符串参数）。
  - 新增 `ports/model_client_factory.h`，按 `AGENT_PROVIDER` 环境变量选 anthropic/openai/local。
  - `AnthropicMessagesClient` 内部不外泄 protocol-specific 字段。
- **验收**：1 个非 Anthropic provider（OpenAI 为例）端到端跑通现有 integration test；`raw_stop_reason` 字符串在 `runtime_engine.cpp` 中不再出现。
> 范本：cc-haha `src/services/api/` 多 provider 抽象（取 adapter 切换意图）。

### T14 MCP 协议支持（架构设计）★ **降低承诺**

- **问题陈述**：`ports/tool_gateway.h` 不接受外部注册；`CompositeToolGateway`（`composite_tool_gateway.cpp`）构造时 `emplace(name, &gateway)`，无运行时注册。
- **改后方案**：
  - 新增 `adapters/mcp/{stdio_client,json_rpc,server_registry}.{h,cpp}`，实现 MCP 的 `initialize / tools/list / tools/call`。
  - `CompositeToolGateway` 增加 `register_runtime(name, gateway)`。
  - **★ v2 降低承诺**：先 stdio JSON-RPC 接入 1 个公开 server（filesystem）——stdio 在 Windows 上要做适配（路径转换、信号处理）。SSE/HTTP transport 后置。
  - permission 与 hook 链对 MCP tool 一视同仁（依赖 T11/T13）。
- **验收**：1 个公开 MCP server（filesystem）端到端跑通；stdio Windows 适配有专门单测。
> 范本：cc-haha `src/services/mcp/`（取 stdio JSON-RPC + server registry 意图）。

### T17 Background task 系统（架构设计）

- **问题陈述**：状态机只有 `Created / PreparingContext / AwaitingModel / AwaitingTool / Cancelled / Completed` 8 态，无 `Background` 子状态；`/tasks` 命令不存在；`TaskOutputTool` 未注册。
- **改后方案**：
  - `TaskType` 枚举（`local_bash / local_agent / in_process_teammate`）。
  - `TaskState` 增加 `background` 字段。
  - `runtime_engine.cpp` 拆出 `TaskSpawner` 与 `TaskOutputTool`。
  - `interactive_cli.cpp` 加 `/tasks` 列出活跃任务。
  - **与 T01 协同**：background task 启动后立即进入并行调度。
- **验收**：1 个示例：并行启动 `cmake build` + `pytest`，模型通过 `TaskOutput` 拉取结果。
> 范本：cc-haha `src/Task.ts`、`src/tasks/`。

### T18 Plugin / outputStyle 系统（架构设计）★ **v2 收敛范围**

- **问题陈述**：`main.cpp`（544 行）手工装配所有依赖；presenter 主题（`kFrames/kVerbs`，`terminal_presenter.cpp:31-54`）编译期硬编码。
- **★ v2 收敛范围**：v1 说"plugin 加载 .so/.dll"在 Windows 上需要 dll search path / manifest / delay-load 等坑，工作量极大。**先做 outputStyle 主题化**（`presentation.theme_id` 字段），就能解 80% "第三方 fork 改场景"诉求。
- **改后方案**：
  - 短期：`presentation.theme_id` 字段，编译期切换（`kFramesThemeClaude / kFramesThemeMinimal / kFramesThemeAscii`）。
  - 长期：`adapters/plugin/` 提供 `PluginHost`：`dlopen` 一个 `.so`/`.dll`，从约定符号 `agent_plugin_init(registry*)` 拉注册回调——**工作量"极大"，建议 P3 远期**。
- **验收**：短期：3 个主题切换 live smoke；长期：1 个示例 plugin 实现 1 tool + 1 command。
> 范本：cc-haha `src/plugins/`（仅取 plugin 注册接口形态，不取其 React 集成）。

### T20 SDK 编程式入口（架构设计）

- **问题陈述**：`main.cpp` 的 `run_agent`（line 117-521）把所有依赖装配、CLI 解析、REPL 启动耦合在一起；无 `agent::Engine::ask(prompt, callbacks)` API；无 pybind11。
- **改后方案**：
  - 从 `main.cpp` 抽出 `composition/` 目录，提供 `build_engine(config) -> std::unique_ptr<agent::Engine>`。
  - `Engine` 类公开 `ask(prompt, callbacks) / resume(task_id) / cancel()`。
  - 用 pybind11 导出同名 Python 类（`agent.Engine`）。
  - **与 §0.1.2 的"composition root 拆分"耦合**：v1 提到的 4 个子命令懒加载（verify-log 不需要 LLM client，节省 30s 启动）应在 T20 时一并实现。
- **验收**：1 个 C++ + 1 个 Python consumer 端到端跑通；verify-log 启动 < 1s。
> 范本：cc-haha `src/QueryEngine.ts#ask`。

### T24 Streaming tool executor（架构设计）★ **基础设施已部分存在**

- **问题陈述**：`runtime_engine.cpp:453` `model_.complete(model_request, options)` 阻塞等整个 response 完成才开始处理 `ToolUseBlock`。流式 observer（line 444-451）只把 TextDelta 渲染到 terminal，不启动任何 tool。
- **★ v2 发现**：基础设施已部分存在——`RuntimePresentationOptions::text_observer` 已经在 streaming 路径上。T24 只差一个 hook：text_observer 在收到 `ToolUseBlock` 时立刻启动并行调度。
- **改后方案**：
  - `StreamingToolExecutor` 订阅 `text_observer` 同时维护"已到齐的 `ToolUseBlock` 列表"。
  - list 满 1 个且 `isConcurrencySafe=true`（参 T04）时立刻 `std::async` 启动。
  - 执行结果缓存到 `state->pending_tool_calls`，等 model response 完整结束一并 append 事件。
  - **与 T01 协同**：T24 实际是 T01 的"流式版本"——两者共同达成"流式 + 并发"双重加速。
- **验收**：集成测试响应 TTFT 不被 tool 执行阻塞；text 仍在 streaming 时 tool 已开始跑。
> 范本：cc-haha `src/services/tools/StreamingToolExecutor.ts`。

### T06 Continue.reason 枚举（代码细节）

- **问题陈述**：`RuntimeEngine::continue_task`（`runtime_engine.cpp:263-643`）的每个 `continue;` 点是无名裸跳转；trace 只能从 `state->status` 与 `event_kind` 反推为何跳。
- **改后方案**：
  - 新增 `enum class ContinueReason { InitialCreate, ContextPrepared, AwaitingModelNextRound, AwaitingToolNext, ToolsCompletedRound, CompactionSucceeded, CancelledByUser, BudgetExceeded, … }` 共 12 项。
  - 每处 `continue` 改为 `push_reason(reason); continue;`。
  - trace sink 接收 reason 字符串。
  - **★ 顺带改造**：`continue_task` 当前是 ~370 行单 while + 9 个 if-status 分支（line 275-643）。T06 改造时顺手**按状态分发**（拆成 8 个 handler），T06 不增加工作量。
- **验收**：单元测试覆盖所有 12 reason；trace 日志可按 reason 过滤统计；"按状态分发"重构后 `continue_task` 主函数 < 100 行。
> 范本：cc-haha `src/query.ts` 12 种 reason。

### T15 Slash command 完整集（代码细节）

- **问题陈述**：`InteractiveCli::run`（`interactive_cli.cpp:382-555`）命令解析是硬编码 if-else 链（line 436-545），共 9 个 case。无 `/help /compact /init /review /rewind /fork /agents /tasks`。
- **改后方案**：
  - 抽出 `commands/registry.{h,cpp}`，定义 `Command { name, description, arg_spec, handler }`；每个命令注册到 `commands::Registry`。
  - `InteractiveCli::run` 改为按名字查 registry。
  - 新增 `/help /compact /init /review /rewind /fork /agents /tasks` 共 8 个。
- **验收**：单元测试覆盖全部命令解析（无 CLI 启动成本）；新增命令从"改 cli 主文件"降到"加一行 register"。
> 范本：cc-haha `src/commands.ts`（取数据驱动 + registry 设计，不取 Ink JSX）。

### T16 Memdir 文件系统化记忆（代码细节）

- **问题陈述**：`JsonlMemoryStore`（`jsonl_memory_store.cpp` 423 行）所有 memory event 写入 `runtime_root/memories/events.jsonl` 单文件（`event_path()` line 380-387）。**无 per-memory-id 文件**。
- **改后方案**：
  - 目录改为 `~/.agent/memories/<memory_id>.md`（每个记忆 1 个 markdown 文件）；metadata 走 frontmatter。
  - `MemoryStore::load(memory_id)` 按需读单文件；grep 走 filesystem `grep -r`。
  - 保留 event log 作为审计副本（不删除原 jsonl，新增 per-md 文件）。
- **验收**：现有 memory 单测迁移到新存储（语义不变）；`@mem:abc123` 解析单文件 O(1)；用户 `cat ~/.agent/memories/*.md` 直接读全文。
> 范本：cc-haha `src/utils/memory/memdir.ts`。

### T19 Cost-tracker Hook（代码细节）

- **问题陈述**：`AnthropicMessagesClient::decode_response`（line 328-331）解析 `usage` 但只写 `decoded` 字段；`ModelContextCompactor::compact`（`model_context_compactor.cpp:35`）调 `model_.complete(request)` 也不上报 cost。**全工程无 `.agent/usage.jsonl` 写入点**；无 `/cost` 命令。
- **改后方案**：
  - 新增 `services/cost_tracker/{sink,hook}.{h,cpp}`：每次 model 响应（success/fail 都记录）追加 `{"ts","task_id","input","output","usd","model"}` 到 `runtime_root/usage.jsonl`。
  - 通过 T13 hook 挂在 `postModelCall` 上。
  - `/cost` 命令读 jsonl 聚合展示。
  - **与 T08 实时显示叠加**：实时 + 总计两视角；预算软上限（与 `max_task_time_ms` 同位置）有数据来源。
- **验收**：live smoke 后 `/cost` 给出正确累计；与 T08 实时数据一致。
> 范本：cc-haha `src/services/costTracker/`。

### T21 Proactive 主动打断（代码细节）

- **问题陈述**：`RuntimeEngine::AwaitingModel`/`AwaitingTool` 全程等待；**无"模型说了一段，检查是否命中危险模式，停止并询问"** 的逻辑。`policy_.parse`（`workspace_tool_gateway.cpp:436`）只校验路径在工作区——不阻止 `~/.ssh/ ~/.aws/ ~/.gitconfig` 目标路径。
- **改后方案**：
  - 新增 `services/dangerous_patterns.{h,cpp}`，定义 5 类正则（`~/.ssh / ~/.aws / rm -rf / curl | sh / 写 /etc/`）。
  - `AwaitingTool` 在 `tools_.execute` 前过 dangerous_patterns；命中即推 `TaskCancelledPayload{reason="dangerous_pattern", needs_user_confirmation=true}`。
  - presenter 显示 `危险模式命中：[pattern]；继续吗？[y/n]`。
  - **与 T11 配合**——`auto mode` classifier 复用 dangerous_patterns。
- **验收**：单测覆盖 5 类危险模式；用户对"agent 拒绝危险动作"有显式 prompt 而非 silent failure。
> 范本：cc-haha `src/utils/dangerousPatterns.ts`。

### T23 In-REPL list picker（代码细节）

- **问题陈述**：`InteractiveCli::run`（line 392-411）session 选择走 `std::getline` 直读一行；`/resume <id>`（line 527-545）也是 `std::getline` 后调 `commands_.load(argument)`。**无键盘上下键选择 UI**。
- **改后方案**：
  - 引入 `cli/picker.{h,cpp}`，定义 `ListPicker { items, on_select, columns }`；用 ANSI escape 监听 ↑/↓/Enter/Esc。
  - session 选择器、permission 确认（T11 配套）、模型选择器（T12 配套）共用同一组件。
  - presenter 已经用 ANSI SGR（`terminal_presenter.cpp:20-24` 的 kReset/kDim），picker 直接复用——**不引入 React/Ink**。
- **验收**：REPL 单测覆盖三种 picker 的键盘事件；用户体验从"打字机"升级到"菜单式"。
> 范本：cc-haha `src/components/Select.tsx`（仅取键盘事件逻辑，不取 JSX）。

---

## §4 类别×优先级速查

|        | P0 | P1 | P2 |
|--------|----|----|----|
| **架构设计** | T01 T04 T03 T05 | T13 | T22 T11 T12 T14 T17 T18 T20 T24 |
| **代码细节** | T02 | T07 T08 T09 T10 T25 | T06 T15 T16 T19 T21 T23 |

**v2 调整**：
- T13 从 P1 架构 → **P1**（v1 即是 P1；v2 强调它是 T11/T21 的承接点）
- T22 **从 P2 架构 → P2 但执行顺序在 T11 之前**（v1 误排 T11 先）
- T25 **从 P2 代码 → P1 代码**（v1 误判为远期）
- 删除 v1 的"T18 Plugin 全量实现" → 拆为短期主题切换 + 长期 dlopen

**总计**：架构 12 条 + 代码 13 条 = 25 条（v1 架构 13 + 代码 12 = 25，数量持平）。

---

## §5 实施顺序建议

```
P0 阶段（合并前）
  ┌── T01 并行 tool（带 5 工具正确默认）───┐
  ├── T02 退避重试 + trace 上报 ─────────────┤
  ├── T04 Tool 5 方法契约 ────────────────┤
  └── T03 6 级压缩（条件性触发） ─────────────┤
       └── T05 SettingSource（与 T04 并行）──┘
                       ↓
                 合并回 main
                       ↓
P1 阶段（合并后首迭代）
  T07 fsync 周期化（同步改 LatencyTraceSink）→ T08 cost 实时 → T09 diff → T10 `/plan` → T25 reactive compact → T13 hook 链
                       ↓
P2 阶段（按产品决策）
  T22 sandbox（执行机制）→ T11 permission（policy 表达）→ T12 provider 抽象 → T14 MCP → T17 background → T18 主题切换 → T20 SDK → T24 streaming tool → T06 continue.reason + 按状态分发重构 → T15 slash 集 → T16 memdir → T19 cost tracker → T21 dangerous patterns → T23 list picker
```

---

## §6 风险与权衡

- **T01（并行 tool）**：引入后会暴露 `OperationContext` 的线程安全 bug——任何 port 实现若内部有 mutable state 都需审计。缓解：默认 `false`，逐个 opt-in。
- **T03（6 级压缩）**：工期最长；前 4 级廉价、后 2 级昂贵，**orchestrator 必须按预算余量条件触发**（v1 没强调这点，会导致无效调用）。建议先做 T03.a（orchestrator 框架）+ T03.b（ToolResultBudget）两步走。
- **T07（fsync 周期化）**：500ms 内丢 32 条事件需 reducer 容错。缓解：`recover_pending_turn` 识别 seq gap truncate；ToolCallStarted 缺失则 ToolCallSucceeded 也丢弃。
- **T11（Permission）**：用户感知最强的 UX 变化，但也是最容易引入安全 bug 的入口。**执行顺序**：先 T22 sandbox 边界 → 再 T11 policy 表达；先 default + bypassPermissions → 最后 auto mode classifier。
- **T12（多 provider）**：与 `is_known_stop_reason_pair` 的 Anthropic 字符串字面量深度耦合（v1 改后方案没覆盖到这一层）。**必须**改写为 provider-neutral。

---

## §7 v1 → v2 改动纪要（保留可追溯）

| v1 | v2 | 变更类型 |
|---|---|---|
| §0 准则（无） | §0 准则 | **新增**：明确不照搬清单 + 已超清单 + 排序准则 |
| T01 "5 工具全标 isConcurrencySafe=true" | "3 工具 true / 2 工具 false" | **错误修正**（v1 描述与代码 `expected_sha256` 防 race 矛盾） |
| T03 "orchestrator 按顺序串联" | "按预算余量条件触发" | **错误修正**（v1 误读 cc-haha 串联模型） |
| T04 "照搬 cc-haha 30 方法契约" | "5 核心方法 + opt-in 派生" | **拒绝照搬**（cc-haha 的 Tool.ts 是 794 行 React/Ink 集成，不是产品模式） |
| T11 P1 / T22 P2 | T22 在执行顺序上 T11 之前 | **优先级调整**（policy 表达依赖执行机制） |
| T25 P2 | T25 P1 | **优先级提升**（当前用户痛点） |
| T12 改后方案 | 改后方案彻底化（`is_known_stop_reason_pair` 改为单参数） | **错误修正**（v1 改后方案不彻底） |
| T07 | 同步改 LatencyTraceSink | **新增发现**（v1 没意识到 trace 写出是另一处 fsync 热点） |
| T18 "Plugin 全量实现" | 拆为短期主题切换 + 长期 dlopen | **收敛范围**（v1 误估 Windows dlopen 成本） |
| T24 | 加"基础设施已部分存在" | **新增发现**（text_observer 已在 streaming 路径上） |
| T06 | 顺手做"按状态分发重构" | **新增发现**（continue_task 是 370 行单 while，重构顺手做） |
| T07 + T08 耦合 | 强调"端到端延迟一并测" | **耦合提醒**（v1 没意识到） |
| T14 "stdio JSON-RPC" | 加"Windows 适配成本"提示 | **风险补充**（v1 没提平台适配） |
| T22 | 加"Windows 特权依赖"风险 | **风险补充**（v1 没提沙箱特权） |

**总数保持**：架构 12 条（原 13，减 1：T18 拆为两期但本期 P2 只计 1 条） + 代码 13 条（原 12，加 1：T25 升 P1 仍算 1 条但加 v2 描述）= 25 条。

---

## §8 不在 roadmap 的项（明确不做）

继承 v1：
- React/Ink UI 框架移植——C++ 应直接走 ANSI SGR，不模仿 virtual DOM
- `bun:bundle` 函数级 dead-code elimination——C++ 用 CMake `if(ENABLE_X)` 即可
- 远程 feature flag（GrowthBook）——无远程配置中心需求
- 多聊天平台 adapters（Slack/Telegram/微信/飞书）——cc-haha 的特殊扩展，不属于 parity 必需

**v2 新增明确不做**：
- cc-haha 的 `bridge/` / `MailboxBridge` / `inbox-poller` / `SSH session`（多端扩展，非 parity 必需）
- cc-haha 的 `outputStyles/` 完整 plugin 体系（短期只做编译期主题切换）
- cc-haha 的 `task teams` / 多 agent 协作（single-agent 定位，P3 远期）
- cc-haha 的 `SetToolJSXFn`（React UI 集成）——这是 v1 误判为通用契约的根源
- cc-haha 的 bashClassifier / yoloClassifier（依赖沙箱边界 + 多层 permission，T22/T11 都未做之前不要做）

---

## §9 总结

- **保留特色**：LatencyTraceSink、证据 isolation、ToolExecutionContext、bounded_json_value、OperationContext——这些都比 cc-haha 更好或更克制，**不动**。
- **真问题优先**：P0 五条全是用户立即能感受到的（tool 串行、5xx 不重试、context 爆炸、tool 抽象太薄、配置无法分层）；P1 六条是合并后第一迭代的 UX/可观测/可玩性改进。
- **拒绝照搬**：cc-haha 的 UI 框架集成、TS 范式、远程 feature flag、multi-agent 协作——这些都不是 latency 的目标场景。
- **渐进抽象**：T04 5 方法 + opt-in 派生（而非 30 方法）；T03 条件性触发（而非无条件串接）；T18 短期主题切换（而非 plugin 全量）。

**路线图核心改进**：从"列差距"变成"列真问题"——v1 的 5 处描述错误（已修正）、2 处优先级误判（已调整）、3 处范围过大（已收敛）、4 处拒绝照搬（已明确）——总条目保持 25 条不变。