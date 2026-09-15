# RAG 统一与删减记录（2026-09-14）

本次从 `main@2de49c2` 开始，将仓库中的知识检索实现统一为：

`RuntimeEngine → PersistentRagKnowledgeProvider → JSONL v2 sidecar → HybridRetriever`

主程序原本已经使用这条链路；本次完成旧实现、旧入口和旧测试依赖的清理。

## 删除与保留

| 范围 | 处理 |
| --- | --- |
| `PythonRagKnowledgeProvider` 的头文件、实现及专用测试 | 删除；从 CMake 构建列表移除 |
| 旧 Python `indexer.py`、`retriever.py` | 删除旧目录扫描、SQLite v1 构建与单次查询实现 |
| Python CLI `build --source … --index …`、`query --index …` | 退役；返回固定错误，不再生成旧格式索引 |
| v1 `QueryRequest`、解析/响应函数、闲置版本常量 | 删除；保留 v2 协议 |
| 分词逻辑 | 原样提取到 `tokenizer.py`；保留中英文、camelCase 和 snake_case 回归测试 |
| 新版检索模式 | 保留 `lexical`、`dense`、`hybrid` |
| eCFR 下载、语料构建、混合索引、评测及知识包校验 | 保留 |
| `DirectProcessRunner`、Runtime 和 Memory | 保留；仍被编程工具等路径使用 |

新知识包继续使用现有构建脚本及 `build-corpus`、`build-index`、`serve` 等入口。
不再提供旧版“任意目录扫描为 v1 SQLite 索引”的能力，旧索引也不会自动转换为新版知识包。

## 测试迁移

- 旧 Provider 的输入预校验、重复证据、空/超限内容、UTF-8 检查迁到新版 Provider 和 v2 协议测试。
- RAG 集成测试迁到新版，覆盖中文证据、进程复用、证据进入模型、工具禁用、JSONL 落盘/重放、指定法规不存在时跳过模型、索引损坏时提前失败。
- 该集成测试运行真实 Python 子进程、`run_sidecar`、混合索引构建、`HybridRetriever`、C++ Provider、Runtime 和日志组件；使用小型合成语料与确定性向量，替换发行知识包校验和 BGE 模型加载。握手的发行身份字段是测试数据，不代表加载了 30,000 篇真实法规。
- 编程工作流改用 `EmptyKnowledgeProvider`，保留真实文件修改、写后落盘失败、哈希冲突恢复、CMake/CTest、日志重放和目录保护检查。新版非空证据禁止工具调用，因此不再沿用旧版“非空证据加编程工具”测试假设。
- 编程恢复测试移出 Python 条件分支；`evidence_rounds` 仍为 7（它计数上下文准备事件），证据条目和带证据的模型请求为 0。
- 旧目录扫描与 v1 格式专属测试随退役功能删除；新版原有测试继续覆盖索引原子更新与陈旧条目清理。补充新版语料构建 CLI 的真实入口验证。

## 验证结果

- 修改前：原主分支 Release CTest **50/50**。
- 修改后：独立工作区 Release 构建通过，CTest **49/49**，Python **132 passed**。
- CTest 数量减少一组，原因是删除 `python_rag_adapter_tests`；新版集成与自主恢复测试均保留。
- `git diff --check` 通过；现行源码无旧 Provider / v1 查询导入残留；分词实现与基线逐字比较一致。
- 独立代码审查无 P0–P2 问题；审查员检查源码，测试结果由本次执行提供。

本次验证范围是源码和本地离线链路，没有运行联网模型或重新发布完整 eCFR 知识包。现有已发布知识包是独立带哈希的产物，不原地修改其中的 sidecar；之后重建发布时会从当前源码纳入删减结果。
