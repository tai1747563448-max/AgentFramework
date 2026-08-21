# 安全工作区文件工具 MVP 设计

## 1. 目的

本子项目只让现有单 Coding Agent 获得完成编码任务所需的第一组文件能力：

```text
查看目录 → 读取代码 → 搜索文本 → 精确修改 → 再次读取验证
```

它不是通用文件系统、安全沙箱或 IDE。完成本子项目后应立即进入“结构化构建与测试工具”，不继续扩张文件层。

## 2. 固定范围

生产 `ToolGateway` 提供五个工具：

1. `list_files`
2. `read_file`
3. `search_text`
4. `replace_text`
5. `write_file`

本阶段明确不提供：

- Shell、PowerShell、`cmd.exe` 或任意进程执行；
- Git 操作；
- 删除、移动、重命名、复制、chmod 或目录创建；
- glob、regex、模糊搜索和语义搜索；
- 二进制文件或编码自动转换；
- Python RAG、多 Agent、MCP、插件和远程服务；
- 面向恶意并发进程的完美 TOCTOU 防御。

## 3. 架构

```text
RuntimeEngine
  └─ ToolGateway port
       └─ WorkspaceToolGateway
            ├─ WorkspacePathPolicy
            ├─ UTF-8 / SHA-256 helpers
            └─ std::filesystem + small platform replace helper
```

Domain 保持 provider-neutral。Application 只把已持久化的 `TaskState.workspace_utf8` 传给工具，不解析路径和参数。文件系统、JSON 和平台差异全部留在 Adapter。

Port 只增加执行上下文：

```cpp
struct ToolExecutionContext {
    std::string workspace_utf8;
};

virtual Result<ToolResult> execute(
    const ToolCall& call,
    const ToolExecutionContext& context) = 0;
```

现有状态、事件种类、Reducer 和失败终态不增加新概念。

## 4. 安全边界

### 4.1 路径

工具只接受 `/` 分隔的 workspace 相对路径：

- `.` 只表示 workspace 根；
- 拒绝空路径、绝对路径、盘符、UNC、反斜杠、NUL、空分量、`.`/`..` 分量；
- 单个路径最多 4096 UTF-8 bytes；
- Windows 拒绝 `:`、尾随点/空格和设备名；
- 返回结果只显示相对路径，不显示宿主绝对路径。

每次操作执行：

1. workspace 必须存在且是目录；
2. 用绝对、规范化路径验证候选仍位于 workspace 下；
3. 检查 workspace 到目标的每个现有分量；任何 symlink、junction 或 reparse point 都拒绝；
4. 可读写 leaf 必须是普通文件，`hard_link_count == 1`；
5. 写入前再次执行相同检查。

Windows 使用 `GetFileAttributesW` 补充 `std::filesystem::symlink_status`，拒绝 `FILE_ATTRIBUTE_REPARSE_POINT`。POSIX 使用 `symlink_status` 和 `hard_link_count`。

这套 MVP 防止正常目录结构中的逃逸和链接别名，但不宣称抵御一个恶意本地进程在检查与打开之间持续抢占目录项。该威胁在 Recovery & Evaluation 阶段根据真实使用风险重新评估；当前不引入 `NtCreateFile`、`openat` 文件系统框架或自研跨平台 handle 层。

### 4.2 保护路径

直接访问下列项返回 `access_denied`，目录遍历和搜索则忽略：

- 任意 `.git`、`.worktrees` 分量；
- `.env` 和 `.env.*`，但允许 `.env.example`；
- `.git-credentials`、`.netrc`、`_netrc`、`.npmrc`、`.pypirc`；
- 默认 `runtime_data`；
- 配置的 `runtime_root` 位于 workspace 内时的对应子树。

首版不新增“任意保护目录”配置项，避免扩大配置面。

### 4.3 资源限制

- 单个文本文件最大 1 MiB；
- 单个工具结果 JSON 最大 64 KiB；
- list/search 每次最多返回 200 项；
- 递归 list/search 最多检查 2000 个目录项和 500 个文件；
- search 最多读取 16 MiB；
- 所有文本必须是严格 UTF-8 且不含 NUL；
- arguments object 最多 64 层、10000 节点、确定性 JSON 编码最大 2 MiB。

达到结果或扫描限制时返回 `truncated=true`，不能把部分扫描伪装成完整结果。

## 5. 工具契约

每个工具 arguments 必须是 object，拒绝未知 key。所有结果都是紧凑 UTF-8 JSON。

### 5.1 `list_files`

```json
{"path":".","recursive":false,"max_results":100}
```

- `path` required；
- `recursive` optional，默认 false；
- `max_results` optional，1..200，默认 100；
- 返回稳定排序的 `{path,type,size_bytes?}` entries、`truncated` 和 `omitted_entries`。

### 5.2 `read_file`

```json
{"path":"src/main.cpp","start_line":1,"max_lines":200}
```

- `path` required；
- `start_line` optional，默认 1；
- `max_lines` optional，1..1000，默认 200；
- 返回完整文件 `sha256`、行范围、`total_lines`、`content`、`truncated` 和 `next_start_line`；
- 空文件为 0 行；非空文件以 `\n` 计行，尾随 `\n` 不额外产生空行；
- 单行无法装入 64 KiB 结果时返回 `limit_exceeded`。

### 5.3 `search_text`

```json
{"path":".","query":"ToolGateway","case_sensitive":true,"max_results":100}
```

- `path` 和非空 `query` required；
- `case_sensitive` optional，默认 true；false 只折叠 ASCII 大小写；
- `max_results` optional，1..200，默认 100；
- 只做 literal、non-overlapping 搜索；
- 返回相对 path、1-based line/column、匹配行片段、扫描数量和截断标记；
- 非 UTF-8、超限、链接和特殊文件在目录搜索中忽略并计数；直接搜索该 leaf 时返回错误。

### 5.4 `replace_text`

```json
{"path":"src/main.cpp","old_text":"old","new_text":"new","expected_occurrences":1,"expected_sha256":"<64 lowercase hex>"}
```

- 所有字段 required；`old_text` 非空；
- 区分大小写、byte-exact、从左到右、non-overlapping；
- 当前 SHA-256 或匹配次数不符返回 `conflict`，文件不变；
- 成功返回 old/new SHA-256、替换次数和写入 bytes。

### 5.5 `write_file`

创建：

```json
{"path":"docs/note.md","content":"text\n","mode":"create"}
```

覆盖：

```json
{"path":"docs/note.md","content":"new\n","mode":"overwrite","expected_sha256":"<64 lowercase hex>"}
```

- parent 必须已存在；不创建目录；
- create 要求 leaf 不存在且禁止 `expected_sha256`；
- overwrite 要求 leaf 已存在、普通、单链接且 SHA-256 匹配；
- content 可为空，但必须是严格 UTF-8、无 NUL、最多 1 MiB。

## 6. 写入策略

修改不直接截断目标文件：

1. 读取并验证现有版本；
2. 在同一 parent 创建唯一临时文件；
3. 写入、flush、关闭并验证临时内容 SHA-256；
4. Windows 用 `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`，POSIX 用 `rename` 安装；
5. 重新读取目标并验证最终 SHA-256；
6. 失败时 best-effort 删除本次创建的临时文件。

只有无法判断最终安装状态、内部序列化失败或未知异常才是 Gateway 基础设施失败。普通路径、冲突和 I/O 问题是模型可见工具错误。

## 7. 错误语义

预期错误仍返回有值的 `ToolResult`，`is_error=true`：

```json
{"error":{"code":"conflict","message":"file content changed; read the file again","retryable":true}}
```

固定 code：

- `invalid_arguments`
- `access_denied`
- `not_found`
- `conflict`
- `unsupported_file`
- `limit_exceeded`
- `io_error`

message 使用固定、脱敏、可操作文本，不包含绝对路径、系统原始错误或文件内容。模型可修正的 `conflict` 为 retryable。

Gateway 无法返回可信结果时才使用 `Result<ToolResult>::failure`。Runtime 沿用现有语义持久化 `ToolCallFailed` 并直接进入 `Failed`；不新增 `TaskFailed`。

## 8. 数据流

```text
ModelCallSucceeded(tool_use)
→ ToolCallStarted durable
→ execute(call, {TaskState.workspace_utf8})
→ ToolResult
→ ToolCallSucceeded durable
→ 下一轮模型读取 tool_result
```

工具级错误 `is_error=true` 不终止任务。`verify-log` 只重放事件，不重新执行文件操作。

## 9. 测试与验收

必须覆盖：

- 五个 ToolDefinition 的精确 schema；
- unknown/missing/extra/wrong-type 参数；
- 相对路径、Unicode、绝对路径、`..`、Windows 危险名；
- `.git`、`.env`、runtime root 保护；
- symlink/reparse 与 hard link 不被读取或修改；无法创建 symlink 的 Windows 环境明确 SKIP，不冒充通过；
- UTF-8、NUL、1 MiB 文件和 64 KiB 结果边界；
- list/search 稳定顺序和截断；
- read 分页和 SHA-256；
- create/overwrite/replace 成功、陈旧 hash、错误次数和零修改；
- 普通工具错误继续下一模型轮，基础设施失败进入 `Failed`；
- Fake Model + 真实 WorkspaceToolGateway 完成“list → read → replace → read → Completed”；
- JSONL 可重放且 workspace 外 sentinel、`.git`、runtime_data 不变；
- fresh MSVC Debug build、默认 offline CTest、credential-free run/verify-log 均保持通过。

## 10. 完成边界

本里程碑完成即停止扩张文件层。以下事项自动进入后续子项目：

- 构建、测试和受控命令执行；
- Git diff/commit；
- Python RAG；
- 崩溃恢复、幂等调用和恶意并发硬化；
- 完整自主 Issue 工作流与最终说明书。

每个实现 commit 推送到本地 `backup/feat/safe-workspace-tools` 并核验 SHA。不连接 GitHub，不运行真实 Provider，除非届时存在新的明确必要性和安全边界。
