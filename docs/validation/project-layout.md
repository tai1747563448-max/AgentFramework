# 项目目录整合与发布验证（2026-09-15）

## 最终布局

当前结构和运行方法见根目录 [README](../../README.md)。知识包已从项目旁的 `AgentFramework-Knowledge/` 迁入 `AgentFramework/knowledge/`，程序通过唯一的 `knowledge/active-pack.json` 定位相对路径 `ecfr-2026-09-03`。

- C++ 源码：`src/`；Python 检索源码：`rag/`。
- 正式数据、模型和运行依赖：`knowledge/ecfr-2026-09-03/`。
- 当前可运行程序：`out/AgentFramework-Ready/`。
- 本机验证证据：`out/validation/`；历史验证文档：`docs/validation/`。
- 会话、任务、记忆：`runtime_data/`，原有数据保留。

知识包不进入 Git。其 `sidecar/` 是与程序配套的发布副本；本次逐文件核对，内容与 `rag/` 中相应源码一致。当前发布包移除了旧 `indexer.py`、`retriever.py`，加入独立 `tokenizer.py`。

## 发布身份

- 文档：30,000；文本块：146,189；向量：BGE-M3、1024 维。
- 包文件：64,035；总字节数：14,783,203,959。
- 保留原数据身份：`pack-5b67b16999249e95f2a8fe67008faf61`。
- 新 `pack.json` SHA-256：`e3f13d34ef0e6958df56f114690afcfb21b34d6803df0957cff8c44ac9555ad0`。
- C++ 受信清单哈希同步更新，完整性检查逻辑保留。
- Ready EXE SHA-256：`d4f5b8a1c9877a869ced9f010448c102a001112555cd7064bbd87b6973434cd7`。

重新发布时更新了构建意图、运行锁关联和清单；64,018 条未修改资源记录保留原哈希。数据库、向量、语料、原始来源和历史评测报告保持不变。历史评测报告不代表对新发布版本重新执行了整套质量评测。

## 验证

1. 新目录发现测试先在旧实现上失败，再在新实现上通过。覆盖根目录 EXE、`build/Release`、`build/vs2022/Release`、Ready 路径，及相对路径逃逸和损坏的近层入口。
2. 最终 Release 构建成功；CTest **49/49**，其中 Python 检索测试 **132 passed**。存在一条原有 `.pytest_cache` 访问权限警告，不影响测试通过。
3. `agent_ready_package_verify` 通过：四个受管理文件、构建/发布文件哈希一致，以及使用合成配置的跨工作目录启动测试。
4. 真实知识包 `verify-pack` 全量校验通过：全部资源清单、30,000 篇文档来源哈希、模型锁及向量校验，耗时约 456 秒。
5. 使用真实随包 Python、BGE-M3 CPU 和当前数据库：`29 CFR 1910.1200` 的 dense/lexical 检索均 matched、返回 5 条证据；`99 CFR 999999.999` 返回 authoritative_no_match、0 条证据。
6. 从临时工作目录启动新 Ready EXE，未设置 `AGENT_RAG_PACK_ROOT`；它通过新相对入口完成 33,854 个可执行资源校验、模型加载和真实负例检索，任务完成且没有回答模型调用。测试使用不可达的本地回答端点和临时会话目录，不修改用户已有会话。
7. 只读代码审查完成；外部 `-KnowledgeRoot` 需显式启用的提示和文档已补齐。

本轮覆盖本机发布和本地真实检索，没有执行远端回答模型调用或干净机器验收。详细证据保存在本机 `out/validation/layout-20260915/`。

## 清理与可恢复归档

- 旧程序目录及其历史运行数据归档到 `E:\GitBackups\AgentFramework-layout-20260915-220431\retired-bundle`。
- 旧控制文件和发布代码保存在同一归档根目录。
- 移除项目中的重复 `out/AgentFramework-Knowledge/` 入口；旧指针留有归档。
- `.build-cache` 和 `.staging` 共 **20,776,187,648 字节**，已移至上述归档根目录的 `rebuildable-cache/`。
- 零散烟雾测试输入移入 `out/validation/legacy-inputs/`，历史 CLI 证据移入 `out/validation/dynamic-cli/`。
- 现有开发工作树保留。

自动审批检查拒绝了永久删除缓存，返回 `blocked by policy`，因此采用项目外的可恢复归档；这部分暂未释放磁盘空间。受访问权限保护的根目录 `.pytest_cache` 保留。
