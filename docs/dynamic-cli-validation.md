# 动态 CLI 与流式输出验收

日期：2026-09-14。实现基于 `00a9af4`，保持 C++17 和已有依赖。

## 已实现

- Windows 交互终端原地旋转字符 `| / - \\`、阶段和耗时提示，约每 100 ms 刷新。
- HTTP SSE 到模型适配器、Runtime、Session、终端的真实增量输出；终端由主线程统一写入。
- 工具成功/失败即时留下状态行；中文和 emoji 跨网络块正确衔接，控制字符转义。
- `--ui plain` 关闭动画，`--stream off` 关闭流式；不支持动画的终端和重定向自动使用纯文本。
- 当前轮 Ctrl+C 取消并返回输入提示，下一轮重新开始；失败预览标记为未完成。
- 完整模型响应校验后才允许工具执行；Session 成功提交后才标记完成。RAG 受约束正文暂存至成功提交。
- SSE 行、事件、JSON 深度、工具参数、响应总量和界面队列均有限制；普通调用仍保留原接口。

## 自动化验证

在原 checkout 记录旧二进制基线：47/48 项通过，`reproc_jsonl_process_tests` 的子进程启动断言失败。该历史结果不作为本次通过证据。

隔离 feature checkout 完整 Release 重建后，最终执行：

```powershell
cmake --build build/vs2022 --config Release --parallel 4
ctest --test-dir build/vs2022 -C Release --output-on-failure -j 2
```

结果：**50/50 个 CTest 项通过，117.50 秒**。包含流式协议、真实 loopback HTTP 分块、无数据取消、Runtime/Session、终端、进程、Ready 包契约以及 Python RAG 测试。

初次全量运行 47/50：feature 自动选择了缺少 pytest 的 Anaconda，另有两个启动超时。改为原项目现有 `build/pytest-venv/Scripts/python.exe`，三项串行复测通过；随后完整重建并以上述最终全量结果验收。没有安装全局依赖。

新增场景先记录失败，再修复：显式取消与时间预算同时到达、深层 JSON、结束阶段后重新开启内容块、命令参数解析、UTF-8 尾部、多模型轮次前缀复用、快速工具状态遗漏、真实接口延后上报输入 token。最终适配器 32 个用例、终端呈现器 12 个用例通过。

## Windows 终端实测

使用实际 Windows PowerShell PTY、`TERM=xterm-256color` 和隔离 loopback 服务，观察到：

1. 等待期间字符原地轮换，耗时递增；正文开始时状态行清除。
2. 中文、emoji `🙂` 分段到达并正常显示，最终正文没有重复。
3. `read_file` 工具执行后即时显示 `Tool completed: read_file`，随后继续下一次模型调用并完成。
4. 服务在响应头前等待 10 秒时，约第 1 秒发送 Ctrl+C，约第 2 秒回到输入提示；下一轮工具任务完成。
5. 正文部分到达后 Ctrl+C 显示 `[Incomplete; turn was not committed]`；下一轮普通回答完成。
6. `TERM=dumb` 环境观察到纯文本阶段提示及流式正文。

自动化 HTTP 用例确认首段回调发生在服务端结束响应之前；loopback 请求日志也记录 `stream=true` 和分开的增量发送时刻。窗口宽度裁剪有单元覆盖；未逐一测试所有终端程序的实时拖拽缩放表现。

## 当前模型接口实测

使用现有 Ready 配置中的 MiniMax-M3，在隔离空工作区发送固定短句请求；RAG、记忆和构建工具关闭，未上传项目内容。

首次请求正文已经显示，但结束时被协议校验拒绝。脱敏事件结构证实：`message_start` 的 `input_tokens=0`，结束 `message_delta` 才报告实际输入计数。修复为校验非负、整数范围和累计值不递减，并用最新值覆盖已有计数；保留所有结束生命周期校验。

修复后再次实测：**2.0 秒完成，显示 `LIVE_STREAM_OK`、流式预览及完成标记，stderr 为空**；持久化事件包含 `model_call_succeeded` 和 `task_completed`。这验证当前接口的普通正文流式调用；真实服务工具调用和 RAG 语义质量未在这次短句测试中覆盖。

## 交付与边界

原 checkout 的集成、Release 重建和 Ready 包验证结果在交付时补充。

本期实现动态状态行及流式正文。固定输入框、输入历史编辑、完整 Markdown 布局属于后续 TUI 工作。启动/退出或独立命令触发的记忆整理仍沿用同步流程。取消是协作式的；外部请求超时仍由已有 HTTP 超时控制，总任务时间预算不承诺硬实时抢占。

原始验证记录保存在本地 `out/dynamic-cli-validation/`，该目录不进入 Git；配置内容和凭据不进入验收报告。
