# AgentFramework

C++17 Agent 运行框架，包含会话、工具调用、事件恢复、长期记忆，以及 eCFR 法规 RAG。

## 文件结构

```text
AgentFramework/
├── src/                    C++ 源码：领域、运行时、会话、记忆、接口与适配器
├── rag/                    Python 检索源码、依赖锁、评测用例与单元测试
├── knowledge/              本机知识包（不提交 Git）
│   ├── active-pack.json    当前知识包的相对路径入口
│   └── ecfr-2026-09-03/
│       ├── index/          metadata.sqlite3、vectors.f16、vectors.json
│       ├── corpus/         已整理法规文本
│       ├── raw/            原始法规快照
│       ├── manifest/       文档来源与清单
│       ├── model/          BGE-M3 模型
│       ├── runtime/        随包 Python 和依赖
│       ├── sidecar/        从 rag/ 发布的运行代码
│       ├── eval/           冻结评测用例
│       ├── reports/        历史评测报告
│       └── *.json          完整性清单、构建意图、模型与依赖锁
├── tests/                  C++、进程、CLI、集成与打包测试
├── benchmarks/             性能基准及历史结果
├── scripts/                知识包构建脚本
├── cmake/                  构建和 Ready 打包支持
├── docs/
│   ├── dynamic-cli.md      交互界面用法
│   └── validation/         带日期的验证记录
├── out/                    本机发布与验证产物（不提交 Git）
│   ├── AgentFramework-Ready/  可运行程序、两个 DLL、运行配置
│   └── validation/         本机验证证据
├── build/                  本机构建目录与测试环境（不提交 Git）
├── runtime_data/           会话、任务事件和长期记忆（不提交 Git）
├── .env                    本机配置与凭据（不提交 Git）
├── .env.example            配置示例
├── CMakeLists.txt          C++ 构建入口
└── requirements-dev.txt    测试依赖
```

隐藏的 `.git/`、`.worktrees/` 及本地审计目录用于开发管理；已有工作树和会话保留。源码改动在 `src/`、`rag/` 等目录完成，`knowledge/.../sidecar/` 是独立运行和完整性校验需要的发布副本。

## 运行与知识包定位

在项目根目录启动：

```powershell
.\out\AgentFramework-Ready\AgentFramework.exe
```

程序按可执行文件位置，在其所在目录及向上三层查找 `knowledge/active-pack.json`，不依赖当前工作目录。当前入口内容为：

```json
{"schema_version":1,"pack_root":"ecfr-2026-09-03"}
```

相对路径以 `active-pack.json` 所在目录为基准。整体复制项目时应同时保留 `knowledge/`、`out/AgentFramework-Ready/` 和所需的本机配置；数据库与模型不会随 Git 克隆自动获取。显式设置的 `AGENT_RAG_PACK_ROOT` 必须是绝对路径，搬迁时需相应调整。运行数据由 `AGENT_RUNTIME_ROOT` 配置控制；若该值使用绝对路径，搬迁时也需调整，避免继续写入旧位置。

旧版 `AgentFramework-Knowledge/active-pack.json` 仅保留代码兼容入口，本项目只维护 `knowledge/active-pack.json`。

## 构建与验证

需要 CMake、支持 C++17 的编译器，以及安装了 `requirements-dev.txt` 的 Python。Windows 的现有构建示例：

```powershell
cmake -S . -B build/vs2022 -DPython3_EXECUTABLE="$PWD/build/pytest-venv/Scripts/python.exe"
cmake --build build/vs2022 --config Release -j 4
ctest --test-dir build/vs2022 -C Release --output-on-failure
cmake --build build/vs2022 --config Release --target agent_ready_package_verify
```

Ready 打包默认读取根目录 `.env`，可通过 CMake 的 `AGENT_READY_ENV_FILE` 指定已有配置。打包仅替换受管理的程序文件。

知识包构建默认写入本项目 `knowledge/`：

```powershell
.\scripts\build_ecfr_knowledge_pack.ps1 -Snapshot 2026-09-03 -DocumentCount 30000
```

该命令会下载并构建大型资源；已有包的构建意图与源码不一致时会拒绝覆盖。自定义外部 `-KnowledgeRoot` 时，运行程序需显式配置 `AGENT_RAG_PACK_ROOT`。知识包内容受 SHA-256 清单校验；完整验证新包后，还需更新 `src/adapters/rag/native_rag_pack_verifier.h` 中固定的清单哈希并重新打包程序，保证程序与知识包配套发布。
