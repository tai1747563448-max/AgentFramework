# 安全工作区文件工具架构设计

## 1. 背景与路线位置

AgentFramework 的目标是先完成一个可靠的单 Coding Agent，再进入多 Agent 协同。当前 Runtime Kernel 已提供显式状态机、模型调用、顺序工具循环、追加式事件日志、预算、取消、离线验证和 Anthropic-compatible Adapter，但生产配置仍使用 `EmptyToolGateway`，因此 Agent 还不能查看或修改工作区。

单 Agent 后续路线固定为：

1. 安全工作区文件工具；
2. 结构化构建与测试进程工具；
3. Python RAG/知识库外挂；
4. Autonomous Issue Workflow；
5. Recovery & Evaluation；
6. 使用真实工作流编写完整代码说明书；
7. 单 Agent 成熟后再设计多 Agent。

本文只设计第一项“安全工作区文件工具”。它使 Agent 能在一个明确工作区内列目录、读文本、搜索文本、精确替换和写文本，但不能执行进程、调用 Shell、操作 Git、访问网络或删除文件。

## 2. 已确认决策

- 主 Runtime 保持 C++17；本子项目不引入 Python。Python 只在后续 RAG/知识库外挂中作为独立进程出现。
- 第一批生产工具固定为 `list_files`、`read_file`、`search_text`、`replace_text`、`write_file`。
- 工具只接受相对于本次任务 workspace 的规范化路径，不接受任意绝对路径。
- 所有路径访问都以“不跟随链接、基于已打开 handle 验证”为安全边界；单纯的字符串 `canonical()` 检查不构成充分保护。
- 文本读写只支持严格 UTF-8；二进制、非法 UTF-8、NUL 和超限文件明确拒绝。
- 修改操作使用调用者提供的 SHA-256 进行乐观并发控制；`replace_text` 还必须提供预期匹配次数。
- 可预期的参数、权限、路径、冲突和文件系统错误返回 `ToolResult{is_error=true}`，供模型修正后继续任务。
- 只有 ToolGateway 自身无法形成可信结果的内部故障才返回 `Result<ToolResult>::failure(...)`，由既有 Runtime 写入 `ToolCallFailed` 并使任务进入 `Failed`。
- 本子项目不改变 Runtime 的任务状态集合，不新增通用 `TaskFailed` 路径，也不改变既有事件持久化顺序。

## 3. 目标

本子项目必须做到：

- 模型能从静态工具定义中准确知道五个工具的输入契约；
- 每个工具调用只能观察或修改当前任务 workspace 内允许访问的普通文件；
- workspace、路径分量和目标 leaf 中的 symlink、junction、mount-point reparse 或其他重解析点都不被跟随；
- 多链接普通文件不会被读取或修改，防止 hard-link alias 把工作区外内容暴露给 Agent；
- 保护 `.git`、凭据文件、Runtime 事件目录和显式配置的保护目录；
- 文件枚举、搜索结果和 JSON 字段顺序稳定，使相同输入得到可比较结果；
- 所有输入、扫描量、单文件大小、结果数量和返回正文都有固定上限；
- 修改前检测陈旧内容，修改后持久化并返回新 SHA-256；
- RuntimeEngine 继续按 durable workspace 调用工具，并将结构化工具结果通过现有事件和下一轮模型消息保存；
- Windows/MSVC 是当前必须通过的平台，同时保留明确的 POSIX 实现契约；
- 用离线 Fake Model 完成一次“查看文件 → 精确修改 → 查看结果 → 最终回答”的真实工具循环测试。

## 4. 非目标

本阶段不实现：

- 任意 Shell、PowerShell、`cmd.exe`、终端或子进程执行；
- CMake、编译器、CTest 等结构化进程工具；
- Git status、diff、commit、branch 或远端操作；
- 文件删除、移动、重命名、复制、chmod、ACL 修改或目录创建；
- 二进制文件、编码自动探测、UTF-16 或系统代码页转换；
- glob、正则表达式、模糊搜索或语义搜索；
- 自动读取 `.gitignore` 并改变工具可见性；
- Python RAG、Embedding、向量数据库或知识库摄取；
- 崩溃后的工具幂等恢复或跨进程并发写协调；
- MCP、插件系统、子 Agent 或多 Agent。

## 5. 安全模型

### 5.1 被保护资产

- workspace 外的任意文件和目录；
- workspace 内的 Git 元数据、凭据文件和 Runtime 事件；
- 其他任务或其他 worktree 的内容；
- 文件在模型读取后由用户或其他程序写入的新版本；
- Runtime 的内存状态、事件顺序和错误边界。

### 5.2 可信与不可信输入

以下输入全部视为不可信：

- 模型生成的工具名、arguments、路径、搜索词和写入内容；
- workspace 中已有的目录项、链接、文件类型和文件内容；
- 操作系统返回的可变路径字符串；
- 外部程序在一次工具调用前后对目录项所做的并发修改。

以下配置由本地 composition root 提供并视为可信：

- 本次 `RunRequest.workspace_utf8`；
- Runtime 数据根目录；
- 额外保护的相对路径；
- 本文固定的资源上限。

### 5.3 威胁边界

V1 必须防止路径穿越、绝对路径逃逸、链接跟随、hard-link alias、Windows alternate data stream、设备名和受保护路径访问。所有安全判断必须在实际 I/O 使用的 handle 上完成。

V1 的 SHA-256 是面向正常并发写入的乐观冲突检测，不声称在恶意并发进程不断替换同名目录项时提供数据库级串行化。即使发生这种竞争，handle/目录锚定操作仍不得跟随链接或越出 workspace；不能证明目标版本仍匹配时必须失败，不能静默覆盖。

## 6. 架构与依赖方向

```text
main.cpp / RuntimeConfig
        |
        v
WorkspaceToolGateway adapter
        |
        v
ToolGateway port <--- RuntimeEngine ---> durable TaskState.workspace_utf8
        |
        v
Domain ToolDefinition / ToolCall / ToolResult / Value
```

- Domain 继续只保存 provider-neutral 的工具类型，不依赖 JSON 库或文件系统。
- Application 只把 durable `TaskState.workspace_utf8` 放入工具执行上下文，不解析工具参数，不拼接路径。
- `WorkspaceToolGateway` 负责工具路由、精确 schema 校验、路径策略、安全文件 I/O、UTF-8 校验、SHA-256 和结果序列化。
- Composition root 用 Runtime 配置创建 `WorkspaceToolPolicy`，并以 `WorkspaceToolGateway` 替换生产 `EmptyToolGateway`。
- `EmptyToolGateway` 保留给最小配置和测试，但同步新 Port 签名。

### 6.1 Port 变化

`ToolGateway` 增加一个只包含 workspace 的 provider-neutral 执行上下文：

```cpp
struct ToolExecutionContext {
    std::string workspace_utf8;
};

class ToolGateway {
public:
    virtual ~ToolGateway() = default;
    virtual std::vector<ToolDefinition> definitions() const = 0;
    virtual Result<ToolResult> execute(
        const ToolCall& call,
        const ToolExecutionContext& context) = 0;
};
```

`RuntimeEngine` 在调用 ToolGateway 时从已经持久化并由 Reducer 验证过的 `TaskState.workspace_utf8` 构造 context，而不是再次使用调用方传入的临时 `RunRequest`。工具执行不允许改变 context。

### 6.2 Policy

生产 Adapter 接受不可变 `WorkspaceToolPolicy`：

```cpp
struct WorkspaceToolPolicy {
    std::filesystem::path runtime_root;
    std::vector<std::string> protected_relative_paths;
};
```

资源上限是版本化代码常量，不从模型、环境变量或 Issue 文本覆盖。后续若需要用户配置，必须先增加边界校验和事件可审计设计。

## 7. 通用输入规则

### 7.1 工具调用信封

- 工具名必须与五个固定名称之一完全相等，区分大小写。
- `ToolCall.id` 必须满足既有 Runtime 对响应 tool call 的约束；Gateway 原样返回同一个 ID。
- `arguments` 必须是 object。
- 每个工具只接受本文列出的 key；缺少 required key、额外 key、类型错误和范围错误都返回 `invalid_arguments`。
- arguments 中单个字符串最大 1 MiB，整个 arguments 的确定性 JSON 编码最大 2 MiB；整个 `Value` 树最多 64 层、10000 个节点。超过限制在做任何文件系统访问前拒绝。

### 7.2 相对路径格式

工具路径使用 UTF-8 和 `/` 分隔符，并满足：

- `.` 只允许单独表示 workspace 根目录；
- 其他路径必须非空，不能以 `/` 开头或结尾；
- 禁止 `\`、NUL、空分量、`.` 分量、`..` 分量、盘符、UNC 前缀和 URI；
- UTF-8 编码长度不超过 4096 字节，分量不超过 255 字节；
- Windows 还拒绝 `:`、末尾空格/点，以及 `CON`、`NUL`、`COM1`、`LPT1` 等保留设备名；带扩展名的设备名也拒绝；
- 结果中的路径统一使用 `/`，相对于 workspace，不回显绝对宿主路径。

大小写比较遵循实际平台文件系统语义：当前 Windows 构建使用 ordinal case-insensitive 比较；POSIX 使用 byte-wise case-sensitive 比较。保护规则也使用相同语义，避免大小写变体绕过。

### 7.3 固定保护规则

下列路径不会出现在枚举或搜索结果中，直接访问则返回 `access_denied`：

- 任意名为 `.git` 或 `.worktrees` 的路径分量；
- 任意名为 `.ssh`、`.aws`、`.azure` 或 `.kube` 的路径分量；
- leaf `.env` 和 `.env.*`，唯一例外是 `.env.example`；
- leaf `.git-credentials`、`.netrc`、`_netrc`、`.npmrc` 或 `.pypirc`；
- leaf 名称以 Gateway 专用 `.agent-tmp-` 前缀开头的内部临时文件；
- 默认 `runtime_data` 目录；
- 若 `runtime_root` 位于 workspace 内，`runtime_root` 及其全部后代；
- `protected_relative_paths` 中每个规范化路径及其全部后代。

额外保护路径必须在 Gateway 构造时通过同一相对路径语法校验；非法 policy 是初始化错误，不能降级成空保护表。

## 8. 通用文件与遍历规则

- workspace 根必须存在、是目录、不是 reparse/symlink，且通过最终 handle 路径验证。
- 从根到 leaf 的每个分量都相对于已经打开的父目录 handle 打开；不跟随 symlink/reparse。
- 可读写 leaf 必须是普通文件，并且链接计数精确为 1；目录枚举只进入真实目录。
- 稀疏文件、设备、pipe、socket、directory junction、mount point 和其他特殊对象不作为文本文件处理。
- 单个可读取或可修改文件最大 1 MiB；判断基于打开 handle 的文件大小，而不是目录项缓存。
- 文本必须是严格 UTF-8，拒绝 overlong、surrogate、超出 U+10FFFF 的序列和 NUL；不自动移除 BOM，UTF-8 BOM 作为正文的三个字节保留。
- 遍历顺序按规范化相对路径的 UTF-8 byte order 排序；Windows 先以 case-fold key 排序，再以原始 UTF-8 作为 tie-breaker。
- 递归遍历最多检查 2000 个目录项、500 个普通候选文件和 16 MiB 文件内容。达到扫描预算时返回已确定结果并明确 `truncated=true` 和固定 `truncation_reason`；不能把部分扫描伪装成完整无匹配。
- 受保护项、链接和特殊文件不会被跟随；聚合结果只返回计数 `omitted_entries`，不泄露受保护 leaf 名称。

## 9. 工具契约

所有成功和预期错误的 `ToolResult.content` 都是单行、紧凑、确定性 UTF-8 JSON；object key 采用本文展示顺序。Adapter 自己序列化 JSON，不让模型提供原始结果 JSON。完整序列化结果最大 65536 bytes；list、read、search 在加入下一条 entry/完整行/match 前预估最终 JSON 大小并安全截断，replace/write 的固定小结果不得截断。错误 JSON 使用固定短消息，永不截断。

### 9.1 `list_files`

用途：查看一个目录的普通文件和真实子目录。

输入：

```json
{
  "path": ".",
  "recursive": false,
  "max_results": 100
}
```

- `path` required，必须指向真实目录；
- `recursive` optional，默认 `false`；
- `max_results` optional，默认 100，范围 1..200。

成功内容：

```json
{"path":".","entries":[{"path":"src","type":"directory"},{"path":"README.md","type":"file","size_bytes":1200}],"truncated":false,"truncation_reason":"","omitted_entries":0}
```

- 非递归时只返回直接子项；递归时返回所有允许的相对后代；
- file 带 `size_bytes`，directory 不带；
- 达到 `max_results` 或通用遍历预算时 `truncated=true`；
- 不读取文件正文。

### 9.2 `read_file`

用途：按行读取一个 UTF-8 文本文件，并取得修改所需版本哈希。

输入：

```json
{
  "path": "src/main.cpp",
  "start_line": 1,
  "max_lines": 200
}
```

- `path` required，必须指向允许的普通文件；
- `start_line` optional，默认 1，范围 1..10000000；
- `max_lines` optional，默认 200，范围 1..1000。

成功内容：

```json
{"path":"src/main.cpp","sha256":"<64 lowercase hex>","start_line":1,"end_line":42,"total_lines":42,"content":"...","truncated":false,"next_start_line":null}
```

- 行号从 1 开始；空文件有 0 行，非空文件的 `total_lines` 等于 `\n` 数量，并在文件不以 `\n` 结束时再加 1；CRLF 的 `\r\n` 原样保留在 `content`；
- 空文件 `total_lines=0`，`start_line=1`，`end_line=0`，`content=""`；
- 非空文件的 `start_line` 大于 `total_lines` 时返回 `invalid_arguments`；
- 返回结果连同 JSON envelope 上限 65536 bytes，且不能截断 UTF-8 code point；
- 若请求行范围使序列化结果超过 65536 bytes，则只返回能完整容纳的前缀行、设置 `truncated=true` 和 `next_start_line`；
- 单行正文超过 64 KiB 返回 `limit_exceeded`，避免产生无法继续分页的伪完整行；
- `sha256` 始终覆盖完整文件原始字节，不只覆盖所返回的行。

### 9.3 `search_text`

用途：在一个文件或目录中做确定性的 literal UTF-8 文本搜索。

输入：

```json
{
  "path": ".",
  "query": "ToolGateway",
  "case_sensitive": true,
  "max_results": 100
}
```

- `path` required，可指向允许的普通文件或真实目录；
- `query` required，非空，最多 4096 字节；
- `case_sensitive` optional，默认 `true`；
- `max_results` optional，默认 100，范围 1..200；
- V1 只做 literal 搜索，不解释 regex、glob 或转义序列。

成功内容：

```json
{"path":".","query":"ToolGateway","matches":[{"path":"src/ports/tool_gateway.h","line":10,"column":7,"text":"class ToolGateway {"}],"truncated":false,"truncation_reason":"","scanned_files":20,"omitted_entries":0}
```

- `line` 和 `column` 都从 1 开始，column 以 Unicode code point 计数；
- `text` 是匹配行移除末尾 CR/LF 后的内容，单条最大 4096 字节；更长行返回一个包含本次 match 的 UTF-8 完整 code-point window，并增加布尔字段 `line_truncated=true`；
- 同一行的多个非重叠 literal 匹配分别返回；
- `case_sensitive=false` 只对 ASCII `A-Z`/`a-z` 做 locale-independent folding；非 ASCII code point 仍精确比较，避免引入平台 locale 或大型 Unicode 依赖；
- 非 UTF-8、超限、链接和特殊文件被计入 `omitted_entries`，不会导致整次目录搜索失败；直接对这类 leaf 搜索则返回对应错误；
- 达到结果或扫描预算时明确标记 truncated。

### 9.4 `replace_text`

用途：在一个现有 UTF-8 文件中做字节精确、次数精确、版本受控的替换。

输入：

```json
{
  "path": "src/main.cpp",
  "old_text": "old value",
  "new_text": "new value",
  "expected_occurrences": 1,
  "expected_sha256": "<64 lowercase hex>"
}
```

全部 key required：

- `old_text` 非空；`old_text` 与 `new_text` 均为严格 UTF-8 且各不超过 1 MiB；
- `expected_occurrences` 范围 1..1000；
- `expected_sha256` 必须是 64 个小写十六进制字符；
- 匹配按原始 UTF-8 bytes、区分大小写、从左到右、non-overlapping 计数；
- 当前完整文件 SHA-256 或匹配次数任一不符都返回 `conflict`，且不写任何字节；
- 生成结果仍必须不超过 1 MiB、严格 UTF-8 且不含 NUL。

成功内容：

```json
{"path":"src/main.cpp","old_sha256":"...","new_sha256":"...","replacements":1,"bytes_written":1234}
```

### 9.5 `write_file`

用途：创建新 UTF-8 文件，或以版本控制方式整体覆盖现有 UTF-8 文件。

创建输入：

```json
{
  "path": "docs/note.md",
  "content": "text\n",
  "mode": "create"
}
```

覆盖输入：

```json
{
  "path": "docs/note.md",
  "content": "new text\n",
  "mode": "overwrite",
  "expected_sha256": "<64 lowercase hex>"
}
```

- `path`、`content`、`mode` required；
- `mode` 只接受 `create` 或 `overwrite`；
- `content` 必须是严格 UTF-8、不含 NUL、最多 1 MiB；
- `create` 禁止 `expected_sha256`，并要求 leaf 不存在；leaf 已存在返回 `conflict`；
- `overwrite` 必须提供 `expected_sha256`，要求 leaf 是允许的普通单链接文件且当前哈希相同；
- parent 必须已经存在且允许访问；V1 不隐式创建目录；
- 空文件是合法 content。

成功内容：

```json
{"path":"docs/note.md","created":true,"sha256":"...","bytes_written":5}
```

## 10. 写入算法与持久性

`replace_text` 和 `write_file` 共用安全写入组件：

1. 通过已打开的 workspace/parent handles 解析并验证 parent；
2. 对 overwrite/replace 以 no-follow 方式打开 leaf，验证普通文件、单链接、最终 containment，读取完整 bytes 并校验 SHA-256；
3. 在同一已验证 parent 下以不可预测名称和 exclusive-create 创建临时普通文件；
4. 写入完整结果，flush 文件数据，再从临时 handle 验证大小和 SHA-256；
5. 通过 parent-anchored rename/replace 原语安装临时文件，不跟随同名 symlink；
6. 重新打开结果并验证普通文件、单链接、containment、大小和 SHA-256；
7. 在平台支持时 flush parent directory；
8. 只有全部完成后返回成功。

POSIX 使用 directory handle、`openat(..., O_NOFOLLOW)`、`fstat`、exclusive temp、`fsync` 和 `renameat`/等价受锚定操作。Windows 使用 `CreateFileW` 的 `FILE_FLAG_OPEN_REPARSE_POINT`/`FILE_FLAG_BACKUP_SEMANTICS`、`GetFileInformationByHandleEx`、`GetFinalPathNameByHandleW`、exclusive temp、`FlushFileBuffers` 和带 parent directory handle 的 `SetFileInformationByHandle(FileRenameInfoEx)`/等价受锚定替换。

若平台不能提供满足上述 containment 的替换原语，生产构建必须明确禁用修改工具并返回 `DependencyUnavailable`，不能退化为“检查路径后用普通 stream 再打开”。创建/写入中断产生的临时文件使用固定 `.agent-tmp-` 前缀；Gateway 只对本次调用自己创建且仍持有 handle 的临时文件做 best-effort 清理。V1 不扫描或删除历史临时文件；残留项受保护规则约束，不会出现在 list/search 中，也不能被模型直接访问。

SHA-256 使用项目内小型、可单元测试的 portable implementation，不依赖 Provider HTTP 栈，也不调用外部进程。摘要只用于版本标识和完整性，不保存秘密。

## 11. 结果与错误语义

### 11.1 成功

成功时：

- `Result<ToolResult>` 有值；
- `ToolResult.tool_call_id` 精确等于输入 call ID；
- `ToolResult.is_error=false`；
- `content` 是相应工具的成功 JSON。

Runtime 按既有逻辑持久化 `ToolCallSucceeded`，即使工具结果正文很大也必须先满足本设计 64 KiB 返回上限。

### 11.2 可预期工具错误

以下错误仍是有值的 `ToolResult`，`is_error=true`：

```json
{"error":{"code":"conflict","message":"file content changed; read the file again","retryable":true}}
```

固定工具错误码：

- `invalid_arguments`：工具名、key、类型、范围、UTF-8 或路径语法错误；
- `access_denied`：保护路径、链接/reparse、hard link、工作区逃逸或平台危险名称；
- `not_found`：workspace、parent 或目标不存在；
- `conflict`：create 目标已存在、hash 不符或替换次数不符；
- `unsupported_file`：非普通文件、二进制/非法 UTF-8 或不支持的对象；
- `limit_exceeded`：文件、行、参数或生成结果超过固定上限；
- `io_error`：对已验证目标发生可归因的 OS read/write/flush 错误。

固定 message 按以下优先级选择；同一次调用只返回第一个错误：

| 条件 | code | message | retryable |
|---|---|---|---|
| 未知工具名 | `invalid_arguments` | `unknown workspace tool` | false |
| schema、类型或范围非法 | `invalid_arguments` | `invalid tool arguments` | false |
| 路径语法非法 | `invalid_arguments` | `invalid workspace-relative path` | false |
| 非空文件的 start_line 越界 | `invalid_arguments` | `start_line exceeds file line count` | false |
| 保护路径或危险平台名称 | `access_denied` | `workspace path is protected` | false |
| link、reparse、hard link 或 containment 不成立 | `access_denied` | `workspace path is not permitted` | false |
| workspace、parent 或 leaf 不存在 | `not_found` | `workspace path was not found` | false |
| create 的 leaf 已存在 | `conflict` | `target already exists; read it before overwriting` | true |
| expected SHA-256 不匹配 | `conflict` | `file content changed; read the file again` | true |
| expected occurrence 不匹配 | `conflict` | `replacement occurrence count did not match` | true |
| 非普通文件、非法 UTF-8、NUL 或不支持对象 | `unsupported_file` | `file is not supported UTF-8 text` | false |
| 任一固定资源上限超出 | `limit_exceeded` | `workspace tool limit exceeded` | false |
| 可归因的 OS I/O/flush 失败 | `io_error` | `workspace file operation failed` | false |

message 不包含绝对路径、原始系统错误文本、文件内容、用户目录或凭据。短暂 OS 错误不在 V1 猜测重试性。

### 11.3 Gateway 基础设施失败

只有下列情况返回 `Result<ToolResult>::failure(RuntimeError)`：

- 工具路由或结果序列化违反内部不变量；
- SHA-256 或安全文件后端初始化失败，无法判断调用是否产生可信效果；
- 已开始写入后无法确定最终安装状态；
- 捕获到未知异常且不能形成准确工具级结果。

错误使用既有 provider-neutral `RuntimeError`，通常映射到 `DependencyUnavailable` 或 `PersistenceFailure`，message 固定且脱敏。Runtime 随后只写 `ToolCallFailed` 并直接进入 `Failed`，不追加 `TaskFailed`。

## 12. Runtime 数据流

```text
ModelResponse(ToolUse)
  -> ModelCallSucceeded durable
  -> ToolCallStarted durable
  -> RuntimeEngine builds ToolExecutionContext from TaskState.workspace_utf8
  -> WorkspaceToolGateway validates call + opens workspace handles
  -> ToolResult
  -> ToolCallSucceeded durable
  -> next ContextPreparationStarted
  -> tool_result message sent to the next model round
```

- Gateway 不直接写 RuntimeEvent 或修改 TaskState。
- 工具调用仍严格按模型响应原始顺序执行，不并发。
- 工具级错误 `is_error=true` 不终止 Runtime；模型可以在下一轮读取错误并修正路径、重新读取 hash 或缩小请求。
- Gateway 基础设施失败沿用 direct `Failed` 语义。
- 工具 arguments 和结果通过现有 ToolCall events 持久化；本子项目不引入新的 event kind 或 schema version。
- `verify-log` 仍只验证历史事件和状态，不重新执行文件工具。

## 13. Composition 与配置

生产 `run` 路径创建：

```text
WorkspaceToolPolicy(runtime_root, fixed + configured protected paths)
  -> WorkspaceToolGateway
  -> RuntimeEngine
```

`verify-log` 路径不创建 WorkspaceToolGateway，不验证 workspace，不接触文件工具，也不加载 Provider 配置以外的新依赖。

本里程碑不新增凭据环境变量。若增加额外保护路径配置，使用单一可选变量 `AGENT_PROTECTED_WORKSPACE_PATHS`，值为平台无关 `/` 相对路径的 JSON array；解析失败使 `run` 初始化以固定 `InvalidConfiguration` 失败。该可选配置不是首轮实现的必要条件；固定保护规则和 runtime root 保护必须实现。

## 14. 测试设计

### 14.1 Tool definition 与参数测试

- 五个 definition 的名称、描述、required、additionalProperties=false、类型和范围精确；
- 未知工具、非 object arguments、缺 key、多 key、错误类型和整数边界；
- Value 深度、节点数和字符串大小边界；
- 每个错误都保持输入 tool call ID，并返回固定 JSON shape。

### 14.2 路径与保护测试

- `.`、普通嵌套路径和 Unicode 路径成功；
- absolute、drive、UNC、`..`、反斜杠、空分量、NUL、超长分量拒绝；
- Windows ADS、trailing dot/space 和 device name 拒绝；
- workspace 根、父目录和 leaf symlink/reparse 分别拒绝；
- directory junction/mount point 不被递归进入；
- 普通文件 hard link 的两个名称都拒绝读取和修改；
- `.git`、`.worktrees`、`.env`、凭据 leaf、runtime root 和 configured protected subtree 直接拒绝且不出现在遍历结果；
- `.env.example` 可按普通文本访问；
- 失败断言同时验证工作区外 sentinel 未改变。

Windows symlink/reparse 用例在无创建权限环境中可以明确 SKIP，但 hard-link、预先准备的 fixture 或受权限运行必须至少提供一个真实 no-follow 进程级证据；SKIP 不能描述为通过该攻击路径。POSIX CI 出现后必须运行对应测试。

### 14.3 读取与搜索测试

- 空文件、LF、CRLF、无末尾换行、Unicode 和 BOM；
- 非 UTF-8、NUL、超 1 MiB、超 64 KiB 单行和特殊文件；
- read line paging、total_lines、next_start_line 和完整文件 hash；
- literal 元字符不被当作 regex；
- case-sensitive/default 和 Unicode simple folding；
- 多文件、多匹配的稳定 path/line/column 顺序；
- 结果数、目录项数、文件数、扫描 bytes 和返回正文边界；
- 部分扫描明确 truncated，不能伪装为零结果。

### 14.4 修改测试

- create、overwrite、单次/多次 replace 和空 content；
- hash 大小写、格式、陈旧 hash、错误 occurrence count 和 overlapping 文本；
- 生成结果超过上限时零修改；
- write/flush/install 失败不返回成功；
- 写入成功后 bytes、SHA-256 和实际内容一致；
- 临时文件不会出现在 list/search 中；
- 并发修改 fixture 证明 stale hash 不静默覆盖；
- symlink/reparse/hard-link sentinel 不被改写；
- Gateway 基础设施异常走 `ToolCallFailed/Failed`，普通 conflict 走 `ToolCallSucceeded` 且 `is_error=true`。

### 14.5 Runtime 与集成测试

使用 Fake Model 和真实 WorkspaceToolGateway 完成以下离线工作流：

1. Agent 调用 `list_files` 找到测试工程文件；
2. 调用 `read_file` 读取缺陷文件并取得 SHA-256；
3. 调用 `replace_text` 用精确匹配修复一处内容；
4. 调用 `read_file` 验证新内容和新 SHA-256；
5. 模型返回最终文本，任务进入 `Completed`；
6. 回读 JSONL，确认工具调用 ID、顺序、结果和终态可重放；
7. 工作区外 sentinel、`.git` 和 runtime events 全部未改变。

该工作流只证明文件工具闭环，不声称已经运行编译或测试。真实“改代码并运行失败测试直到通过”的完整说明书工作流要等结构化进程工具、Recovery 与 Evaluation 完成后再执行和记录。

### 14.6 构建与离线边界

- Windows/MSVC Debug 全构建和 CTest；
- 默认 CTest 不访问 Provider 网络；
- credential-free `agent run` 仍在缺少 Provider 配置时以既有固定错误退出，不因文件工具初始化而提前扫描 workspace；
- `verify-log` 继续 credential-free 且不加载文件工具；
- 测试输出与事件 fixture 不包含绝对用户目录或秘密。

## 15. 初步文件职责

```text
src/
  ports/
    tool_gateway.h                 # ToolExecutionContext + Port
  adapters/
    workspace/
      workspace_tool_gateway.*     # definitions, routing, result envelope
      workspace_tool_policy.*      # path grammar and protected paths
      secure_workspace_fs.*        # handle-anchored traversal and I/O
      utf8_text.*                  # strict validation and line accounting
      sha256.*                     # portable content version digest
  application/
    runtime_engine.cpp             # pass durable workspace context only
  adapters/empty/
    empty_tool_gateway.*           # updated Port signature
  main.cpp                         # production composition
tests/
  adapters/workspace/
    workspace_tool_gateway_test.cpp
    secure_workspace_fs_test.cpp
  application/
    runtime_engine_test.cpp
  integration/
    runtime_integration_test.cpp
```

具体 `.h/.cpp` 拆分可在实施计划中按测试切片调整，但不得把 OS API、JSON 解析或路径策略移入 Domain/Application。

## 16. 里程碑验收标准

本子项目只有同时满足以下条件才算完成：

1. 五个工具 definition 与本文 schema 一致，并有精确测试；
2. 生产 `run` 使用 WorkspaceToolGateway，不再使用 EmptyToolGateway；
3. 工具只能访问 workspace 内允许的普通、单链接 UTF-8 文件；
4. `..`、absolute、symlink/reparse、hard link、Windows ADS/device name 和保护路径攻击均有负例；
5. read/search/list 的数量、扫描量和返回正文上限由边界测试锁定；
6. replace/write 的 SHA-256、occurrence、大小、flush 和最终验证由测试锁定；
7. 普通工具错误不终止任务，Gateway 基础设施失败直接进入既有 `Failed`；
8. Fake Model + 真实文件 Gateway 的完整离线工具循环成功并可从 JSONL 重放；
9. fresh MSVC Debug build 与默认 offline CTest 全部通过；
10. credential-free run、verify-log、live-test-off 和秘密扫描保持既有保证；
11. 设计、实施计划、测试和实现形成可解释 commits，每个 commit 同步到 E 盘 `backup` 并核验 SHA；
12. 不运行真实 Provider、不连接 GitHub，除非用户另行明确授权。

## 17. 后续接口边界

- 下一子项目“结构化进程工具”可以复用工具结果 envelope、workspace policy 和 handle-anchored cwd 验证，但必须单独设计 executable allowlist、参数数组、环境变量、超时、输出截断、进程树终止和 sandbox；不能把本阶段扩展成任意 shell。
- Python RAG/知识库外挂仍通过 `KnowledgeProvider` 接入，计划采用本地 Python 子进程和 NDJSON stdin/stdout；它不通过文件工具偷偷读取任意路径。
- Autonomous Issue Workflow 在文件与进程工具均成熟后编排“理解 → 检索 → 修改 → 测试 → 修复 → 总结”。
- Recovery & Evaluation 再定义崩溃恢复、工具幂等键、重复调用判定、质量评价和真实工作流证据。
- 最终代码说明书以一个真实失败测试为主线，逐层解释 CLI、RuntimeEngine、状态机、事件日志、模型 Adapter、文件工具、进程工具、RAG、恢复与评估的实际调用流；说明书必须以最终代码和真实运行记录为准，不提前用设计承诺冒充实现。
- 多 Agent 只在上述单 Agent 通过验收、说明书和真实工作流后开始设计。
