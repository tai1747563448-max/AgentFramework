# 实验性分支 ctest 基线 — 2026-09-20

本会话针对 `AgentFramework-latency` 分支 (`perf/cli-latency-parity`) 做 BUG 修复与基线恢复。
当前 ctest 总览:**73 个 test,0 失败 (73/73 PASS)**,连续两轮全绿,总耗时约 19–22 秒。

> **重要前提:先前会话报的 "4 fail" / "3 fail" 都不可信。** 那些结论是在部分 object
> 文件过期(见 §1)的构建上测出来的,二进制里混着同一个 struct 的两种内存布局。
> 本会话先修好依赖追踪,再做一次干净重建,才拿到可信基线:**当时真实失败是 8 个**。

## 1. 根因:头文件依赖从未被追踪(所有"基线漂移"的元凶)

`ninja` 对 MSVC 靠解析 `cl /showIncludes` 的输出来记录头文件依赖,只认前缀
`Note: including file:`。本机 MSVC 是中文版,实际打印的是 GBK 的 `注意: 包含文件:`。

CMake 探测这个前缀时按 UTF-8 解码,两个 GBK 字符被替换成 U+FFFD,于是
`CMakeFiles/rules.ninja` 里写下的前缀永远匹配不上,`ninja` 记录到的依赖数是 **0**:

```
$ ninja -t deps CMakeFiles/agent_kernel.dir/src/application/state_reducer.cpp.obj
...: #deps 0, deps mtime ... (VALID)
```

后果:构建照样"成功",但改任何一个头文件都不会重编依赖它的 object。二进制里
`TaskState` 一半是新布局一半是旧布局 → 堆损坏 (`0xc0000374`)、`bad_variant_access`、
段错误,而且报错位置和改动的头文件毫无关系。这正是过去被反复误判成"测试 flaky /
baseline drift"的东西。

修复:`scripts/patch_ninja_deps_prefix.py` 反向探测 cl.exe 真实输出的前缀字节(不写死
代码页),重写 `rules.ninja` 的 `msvc_deps_prefix` 行。幂等,已接进构建脚本。修完:

```
...: #deps 9, deps mtime ... (VALID)
touch src/domain/task_state.h   # 重新编译 60 个 object,不再是 0 个
```

**注意:** 该 patch 写在生成的 `rules.ninja` 里,`cmake` 重新 configure 会覆盖它
(和既有的 vs_link_exe 直连 link.exe patch 一样)。`scripts/_session_build.cmd` 每次
构建前会自动重新应用。

## 2. 构建入口

`scripts/_session_build.cmd` 是本分支现在的标准构建入口:

1. 跑 `scripts/patch_ninja_deps_prefix.py` 修复/校验 `msvc_deps_prefix`;
2. `cmake --build . --config Release -j 4 -- -k 0`。

**自愈逻辑。** patcher 用退出码区分两种情况(见脚本头部注释):

| 退出码 | 含义 | wrapper 的动作 |
|---|---|---|
| 0 | 前缀本来就是对的 | 直接增量构建 |
| 2 | 前缀被 configure 冲掉了 | **删除本项目所有 object 目录后重建** |
| 1 | 探测 cl.exe 失败 | 直接报错退出 |

退出码 2 那一支是关键:补丁写在生成文件里,`cmake` 重新 configure 必然冲掉它;此时磁盘上
已有的 object 全是在"依赖追踪失效"的状态下编出来的,不可信,所以只能整片重编。代价是
每次 reconfigure 后第一次构建要多花一次全量编译的时间,这是唯一安全的选择。

**构建失败检测。** 因为用了 `-k 0`,ninja 的退出码非 0 可能只是 curl docs 规则(见下),
所以脚本改为检查日志里有没有 `error C` / `LNK` / `fatal error` —— 只有真正的编译/链接
错误才会中止并拒绝跑测试。**这是有意的**:静默地测试一个过期二进制正是本文件存在的
理由。

用 `-k 0` 的原因:内置 curl 的 `docs` 自定义命令会调一个生成的 `.bat` 去跑 perl,
PATH 上是 msys 的 `/usr/bin/perl`,cmd.exe 解析不了,裸跑 `cmake --build .` 会在这一条
规则上中断整个 ninja 图。那个 docs 规则只生成 man page,不是任何测试二进制的输入。

**链接规则保持 CMake 原样。** `cmake -E vs_link_exe` 只要在 vcvars64 环境里跑就是好的
(见 §4),所以 `rules.ninja` 里**不再需要**"直连 link.exe"那处补丁了 —— 之前那份补丁
只是在补偿"绕过 vcvars 直接跑 ninja",而那种用法本来就无法工作(LNK1181 找不到
`ws2_32.lib`)。

**重新 configure 后**:直接跑 `scripts/_session_build.cmd`,它会自己恢复前缀、丢掉旧
object、全量重建。不要手动去改 `rules.ninja`。

## 3. 本会话修复清单

### 3.1 runtime_engine / state_reducer — 8 个子用例 → 全绿

- **`ToolCallStarted` 的批派发契约错了。** reducer 原来用 `next_tool_index`(完成游标)
  去索引"下一个可 Started 的槽",两个游标在批派发窗口内不相等,第二个 `Started`
  必然 `InvalidTransition`。新增独立游标 `TaskState::tool_dispatch_index`
  (`src/domain/task_state.h`),`Started` 用它索引,`Succeeded` 用一个
  "该槽已有 durable 的 Started" 的判断来维护 `active_tool_call_id`
  (恢复路径重放的就是这个 head)。
- **`active_tool_call_id` 指向错误。** 原实现每完成一个调用就把 active 指向下一个
  未开始的 pending 调用,于是 runtime 误判"有一个 Started 没完成",走单调用恢复
  路径重放它,直接吞掉了本该发出的第二个 `ToolCallStarted`。表现就是事件序列
  变成 `Started/Succeeded/Succeeded`(只 13 个事件而不是 14 个)。
- **批派发窗口只该包含连续的可并发调用。** 非 `is_concurrency_safe` 的调用现在单独
  成窗。这样可变工具在事件流里永远是"一个 Started 配一个完成",崩溃后不会留下一堆
  没有结果的 mutating Started;串行工具的事件序列也回到 `S,OK,S,OK`。
- **工具预算检查不再越过取消/超时。** 原来 `usage.tool_calls + window_size > limit`
  的预检查排在 `guard_external_call` 之前,把"用户已 Ctrl+C"或"墙钟已超"的判决
  改判成了 `max_tool_calls`。现在 guard 先行,窗口按剩余预算**截断**而不是拒绝
  (只有 guard 允许发 `max_tool_calls` 终态,reducer 也要求此时 `usage` 已到上限)。
- **抛异常的 text_observer 现在真的被摘掉。** `handle_awaiting_model` 接
  `RuntimePresentationOptions&`,catch 里把 `text_observer` 置空,行为回到
  "本任务剩余轮次不再预览",而不是每轮重抛一次。

### 3.2 RAG v3 ready 帧 — 2 个测试 → 全绿

`decode_v3_ready` 把 `backend` / `precision` 加进了 `exact_keys`(**必需**)键集合,
但两个字段其实是可选的。新增 `keys_within(required, optional)` 辅助函数,并镜像
`rag/agent_rag/hybrid_retriever.py` 的契约:两个字段要么都在、要么都不在,且都必须
是非空字符串(这样半更新的 pack 仍会被拒)。

### 3.3 `jsonl_event_store` — Windows flush 路径从来没成功过

`secure_flush_log`(T07,每 32 事件或 500 ms 触发)用
`FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES` 打开句柄。MSVC 能打开,但
`FlushFileBuffers` 要求写数据权限,句柄没带,于是每次 flush 都返回
`ERROR_ACCESS_DENIED` → `"failed to flush event log"`。

实测最小可用权限(probe 过 `FILE_WRITE_ATTRIBUTES` / `FILE_APPEND_DATA` /
`FILE_WRITE_DATA` / `GENERIC_WRITE`):只有前两者之一起作用。改成
`FILE_READ_ATTRIBUTES | FILE_APPEND_DATA`,与 `secure_append_line` 请求的权限一致。

这是 `autonomous_issue_workflow_tests` 里 "resume 后 fatal_error" 的真实原因:恢复
路径写到第 32 个事件时触发 flush 失败,把整个 resume 判成持久化故障。

### 3.4 `interactive_process_tests` — 3 个子用例 → 全绿

- **退出时的 drain 被写成了 0 ms。** `consolidate(current)` 只是把维护请求排进队列,
  紧接着 `drain_for_exit(milliseconds(0))` 立刻返回,进程随即退出,destructor 再把
  在途请求取消掉 —— 于是最后一次记忆巩固的模型调用永远不会发生。三个失败用例
  全都表现为"脚本化 provider 少收到一次调用"。改成 5 秒上限的有界等待。
- **启动期 catch-up 与第一条命令竞态。** 启动时对已有 session 排的 catch-up 是异步
  的,用户敲的第一条命令(用例里是 `/forget`)可能先落盘,导致记忆事件顺序和预期
  不符。现在 catch-up 之后、打印首个 prompt 之前也做一次同样的有界等待,让第一条
  命令看到的是已经结算的记忆存储。

### 3.5 `knowledge_pack_build_script_contract` — 1 个用例

PowerShell 会在宿主控制台宽度处折行,而且会**折在词中间**:错误信息里的
`ValidateSet` 变成 `Validat\neSet`,正则永远匹配不上。之前的诊断("cmake 把
`${SOURCE_DIR}/../pack` token 化错了")是误判 —— 实测脚本确实以 `ValidateSet`
拒绝了,`result` 也确实是 1。

用例现在把比较双方的所有空白都去掉再匹配;同时把"退出码非 0"和"理由不对"拆成
两条不同的错误信息,免得下次再被误导。`tests/rag/.../*.cmake` 注释里说明了原因。

### 3.6 构建期编译错误(与测试无关,但挡住了干净重建)

`ModelResponse` 迁到 v2(去掉 `raw_stop_reason` 字符串)时漏了三个文件,它们还在用
6 元素聚合初始化,导致 `C2440`:

- `tests/application/model_context_compactor_test.cpp` —— 同时删掉了
  "provider 字符串与 canonical enum 不一致"这一行;该情形在 v2 类型里已无法表达,
  那类校验现在归 `stop_reason_codec` 测试。
- `tests/integration/runtime_integration_test.cpp`
- `tests/integration/autonomous_issue_workflow_test.cpp`

`tests/test_main.cpp` 的 `'\n'` 改成 `std::endl`(逐行 flush):否则崩溃时 stdout
缓冲区里的全部历史一起丢失 —— 正是排查那几次堆损坏时最需要的信息。

### 3.7 `agent` 目标链接失败(发现于"删掉 link patch"之后的验证)

`rules.ninja` 恢复原版之后,`AgentFramework.exe` 的链接边**每次都报失败**(exit 255),
报错文本却和链接无关:

```
FAILED: [code=255] AgentFramework.exe
... -- link.exe ... && cmd.exe /C "cd /D E:\... && cmake "-DDLLS=a|b" -DTARGET_DIR=... -P ..."
'E:' 不是内部或外部命令
```

`link.exe` 其实**成功了**(exe 已经落盘),失败的是挂在同一个边后面的 POST_BUILD
(把 cpr.dll / libcurl.dll 复制到 exe 旁边)。

根因是**两层嵌套的 `cmd /C`**。Ninja 生成器把 POST_BUILD 命令作为
`cmd.exe /C "cd /D <绝对路径> && <命令>"` 追加到链接命令后面,而链接命令本身也已经是
`cmd.exe /C "cd . && cmake -E vs_link_exe ..."`。cmd.exe 遇到以引号开头的 `/C` 参数时
会"剥掉第一个和最后一个引号"来还原命令;而 `-DDLLS=a|b` 这个参数因为含 `|` 被 CMake
额外加了引号,于是字符串里多出一对引号,首尾剥离就错位了 —— `cd /D ` 被吃掉,
`E:` 成了命令名。

修法:不要再传单个 `-DDLLS=a|b` 参数。改成让 genex 直接展开成参数列表:

```cmake
add_custom_command(TARGET agent POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_RUNTIME_DLLS:agent>" "$<TARGET_FILE_DIR:agent>"
    COMMAND_EXPAND_LISTS)
```

这样生成的命令里没有任何内嵌引号,cmd 的首尾剥离恢复正常。`cmake/copy_runtime_dlls.cmake`
(存在的唯一理由就是拆那个 `|` 列表)随之删除。验证:删掉两个 dll 后
`ninja AgentFramework.exe` 退出码 0,三个文件都就位;`scripts/_stage_ready_package.cmd`
也从"永远失败"变成正常产出 `out/AgentFramework-Ready/`。

**顺带补上 guard 的漏洞。** 这个失败此前没被 `_session_build.cmd` 拦住,因为 guard 只
grep `error C` / `LNK` / `fatal error`,而这条失败的文本里一个都没有。现在改为检测
ninja 的 `FAILED:` 行(排除 curl docs 那条预期失败)—— 覆盖所有失败形态。四种合成日志
(只有 curl 失败 / 编译错误 / 上面这种 shell 层 255 / 干净)都验证过。

## 4. 副产物文件与本次清理

- `build/vs2022/CMakeFiles/rules.ninja` —— **现在只剩一处本地 patch**
  (`msvc_deps_prefix`),由 `_session_build.cmd` 每次构建自动重打并校验。
  链接规则已恢复成 CMake 原版的 `vs_link_exe`(含 §3.7 的 POST_BUILD 修复),
  **不再需要手工干预或直连 link.exe**。
  验证方式:重新 configure 后跑一次 `_session_build.cmd`,全量重编,68 个可执行文件
  加 `AgentFramework.exe` 全部经 `vs_link_exe` 链接成功,ctest 73/73。
- `scripts/_session_build.cmd`、`scripts/patch_ninja_deps_prefix.py` —— 新增。
- `cmake/copy_runtime_dlls.cmake` —— 删除(见 §3.7,它存在的唯一理由已消失)。

### `scripts/` 清理(28 个删除)

清完之后 `scripts/` 只剩 8 个文件,每个都有明确职责:

| 保留 | 理由 |
|---|---|
| `_session_build.cmd` | **唯一构建入口** |
| `patch_ninja_deps_prefix.py` | 依赖追踪补丁 |
| `configure_msvc.cmd` | 全新 configure(含 FetchContent 的 GitHub 镜像) |
| `_stage_ready_package.cmd` | 产出 Ready 包(已修掉硬编码 cmake 路径 + `cmake -E chdir` 误用) |
| `setup_msvc_env.sh` | bash 侧加载 vcvars 环境 |
| `build_candidate_pack.py` | 候选 RAG 包 |
| `build_ecfr_knowledge_pack.ps1` | **被 `knowledge_pack_build_script_contract` 测试引用** |
| `upgrade_rag_index_metadata.py` | RAG 索引元数据迁移 |

删除的 28 个其实是 5 类重复:19 个 `_tXX_*`(每任务一个"只编某几个 target"的脚本)、
5 个逐字相同的 `link.exe` 复制粘贴(`_t04_link` / `build_ninja` / `link_all` /
`link_msvc` / `manual_link`)、3 个裸 `cmake --build` 包装(`build_all` / `build_msvc` /
`build_target`)、`_rebuild_agent`(就是 `--target agent`)、`_t22_full`。

其中**三个是活的雷**:`_t13_reconfig.cmd`、`_t14_build.cmd`、`_t17_build_test.cmd` 里都
含 `cmake .`——一跑就静默冲掉依赖追踪补丁,正好复现 §1。删掉它们本身也是安全性提升。

`build_ecfr_knowledge_pack.ps1` 之外,全仓库(CMakeLists / tests / docs)对这 28 个文件
**零引用**,删除无风险。

也删了 `diag_ps.cmake` / `diag_ps2..4.cmake` / `run_diag.bat`(上个会话的诊断脚本,
结论已证伪,见 §3.5)和 `scripts/__pycache__`(gitignore 内的本地缓存)。

## 5. 后续建议

1. **不要把"依赖追踪正常"当成默认前提。** 它依赖一个会被 configure 清掉的生成文件补丁。
   现在有两道防线:patcher 每次构建自动重打,以及"退出码 2 → 丢弃旧 object"的自愈分支;
   加上 guard 现在会拦住任何 ninja `FAILED:` 边。如果哪天又出现"测试莫名失败但改动看着
   无关",**第一件事是查 `ninja -t deps <任一 obj>` 是否非 0**,而不是去读测试代码。
2. **改 `CMakeLists.txt` 里任何 `add_custom_command` 时,注意别让生成的参数里出现内嵌
   引号**(§3.7)。含 `|` `;` 或空格的参数会被 CMake 加引号,而 CMake 给 Ninja 生成的
   POST_BUILD 命令本身已经嵌了一层 `cmd /C`——两层嵌套 + 内嵌引号 = cmd 解析错位。
3. `anthropic_adapter_tests` 仍是已知 flake(本会话多轮全绿,历史上偶发),
   与本轮改动无关。
4. `perf/cli-latency-parity` 上仍有大量未提交的工作区改动(T20/T24/T25 等)。
   建议尽快把这些改动固化成一个 commit,否则下一次"基线漂移"还是没法用 git 定位。
   注意这些脚本的删除发生在 latency 分支上,`main` 分支仍保有全套 36 个脚本。
