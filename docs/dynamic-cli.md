# 动态交互 CLI

无参数启动 `AgentFramework.exe` 即进入交互模式。支持 VT 的终端中，等待模型或工具时显示原地旋转的字符和耗时；模型返回正文时逐段显示。

```powershell
# 在 Ready 目录启动，沿用现有配置
.\AgentFramework.exe

# 保持流式正文，关闭终端动画
.\AgentFramework.exe --ui plain

# 等待完整响应后再输出
.\AgentFramework.exe --stream off

# 显式使用配置文件
.\AgentFramework.exe --env-file 'E:\path\to\.env'
```

`--ui auto|plain` 和 `--stream auto|off` 仅用于交互入口。默认均为 auto：终端有相应能力时显示动画，模型支持时流式显示。重定向到文件/管道时自动关闭终端动画；非交互 `run`、`resume`、`verify-log` 和 `evaluate-log` 的输出格式保持原有约定。

运行中按 Ctrl+C 请求取消当前轮，看到取消结果并回到输入提示后可继续下一轮。空闲输入时 Ctrl+C 退出。原有 `/help`、`/status`、会话与记忆命令继续可用。

`[Generating; provisional]` 表示当前显示的是正在生成的内容。若随后断流、取消或提交失败，已显示内容会标记为未完成；不会作为成功会话轮次提交。工具仅在完整模型响应验证通过后执行。

有检索证据约束的 RAG 回答先缓冲，成功校验并提交后显示；等待期间仍有动态提示。当前轮内的上下文压缩和记忆整理只显示阶段，不展示其内部模型正文。启动、退出或单独命令触发的记忆整理沿用同步流程，尚未接入动画。

中文和 emoji 支持跨网络块接收，模型/工具带来的终端控制字符会被转义。每条模型消息的终端显示维持 8192 渲染字节上限，超过后显示一次截断提示；该显示限制不等于响应持久化长度。
