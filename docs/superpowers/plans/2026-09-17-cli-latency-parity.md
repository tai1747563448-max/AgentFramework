# AgentFramework CLI 延迟对齐 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. 当前仅完成计划，不能据此自动开始执行。

**Goal:** 将普通问答与法规问答的启动、首轮、后续轮、首字及取消体验推进到同机 Codex CLI 的可测量对齐目标。

**Architecture:** 先消除清单解析和数据库扫描，再缩小可信执行载荷；使用按需检索、分阶段 sidecar、任务内证据复用和非阻塞记忆维护。保持证据质量、完整性验证、持久化及故障恢复契约，不以预热或状态提示替代真实 cold/首字验收。

**Tech Stack:** Windows x64、C++17/CMake/MSVC、Python、SQLite、BGE-M3/SentenceTransformer、CPR/libcurl、现有 SSE 和 JSONL。

**Spec:** [设计与验收规范](../specs/2026-09-17-cli-latency-parity-design.md)。执行者必须先读该文件和 [当前调查报告](../../../out/validation/20260917-startup-audit/调查报告.md)。

## Global Constraints

- 本轮只制定计划，不修改源码、配置、模型、知识包或发布程序，不启动在线基准，不提交 Git。
- 实施以 `c1f177c` 为已核验基线；执行时再次检查实际 HEAD/工作树。隔离改造，不接入 Agent_G 未完成迁移。
- 维持 Windows x64/C++17；不默认换模型、减少语料或关闭法规库。
- 超时、取消、依赖失败、普通 no_match、authoritative_no_match 不合并。
- 保留重复 JSON 键拒绝、链接/路径检查、执行载荷验证和 evidence 禁用工具的规则。
- 安全或质量回归失败即停止该阶段，不用放宽检查完成性能指标。
- 计时口径、样本量和最终数值以 Spec 第 2、5 节为准；目标不是已测成绩。
- 修改知识包只在新目录离线构建并验证；源码、sidecar、manifest、固定哈希、EXE 与 active pointer 成套发布和回滚。
- 仅新增本文与设计规范。下面所有代码片段、命令和任务均为未来实施要求。

## 任务依赖和交付顺序

`T0 基准 → T1 线性解析 → T2 统一时限 → T3 数据库 → T4 精简可信包 → T5 按需检索 → T6 后端冷启动 → T7 任务缓存 → T8 记忆/历史 → T9 HTTP/流式 → T10 Ready验收`。

T1/T3 是独立可审查的低风险性能修复；T4–T6 决定真正首轮是否能达到 5–8 秒；T8/T9 防止瓶颈转移到记忆、压缩或网络适配。只有 T10 全部门槛通过才完成整体目标。

## T0：冻结对比基准和真实计时

**文件：** 新建 `benchmarks/cli_latency_benchmark.py`、`benchmarks/fixtures/cli_latency_cases.jsonl`、`src/domain/latency_trace.h`、`tests/benchmark/cli_latency_benchmark_test.py`；修改 `src/main.cpp`、`src/application/runtime_engine.cpp`、`src/cli/terminal_presenter.cpp`、`src/adapters/anthropic/cpr_http_transport.cpp`。已有 `benchmarks/agent_runtime_benchmark.cpp` 继续测内核，不冒充真实 CLI 基准。

**接口：** 新增旁路诊断，不改变持久化事件 schema。

```cpp
struct LatencySample {
    std::string request_id;
    std::string stage; // submit, request_send, first_text_received, first_text_rendered
    std::int64_t monotonic_us;
};
using LatencyObserver = std::function<void(const LatencySample&)>;
```

- [ ] 冻结版本、真实 Ready 哈希、语料与正确答案；在隔离运行根记录环境、设备和 provider，日志不包含凭据或完整私密 prompt。
- [ ] 先写夹具：服务器先发 status、再发半个 SSE 帧、最后补齐首个 text；断言 TTFT 使用完整 text，不把 status 当答案。统计脚本测试包含成功、timeout、cancel，失败不得从分母消失。
- [ ] 接入上面四个时间点，并增加 pack_verify/model_load/index_open/retrieve/memory/compaction/tool 阶段。不可观测的 Codex 内部时间填 null。
- [ ] 将基准驱动器的计划接口固定为 `--target agent|codex|provider --scenario-set <jsonl> --iterations <n> --state warm|process-cold --output <json>`。先用假 provider 验证脚本；真实配对测试依照 Spec 的样本量与权限范围运行。
- [ ] 输出 `benchmarks/results/cli-latency-baseline.json`，逐项给出 P50/P95/max、成功率、资源统计；基线只有一个样本的项不得伪造百分位。
- [ ] 独立提交：`test(perf): add reproducible CLI latency baselines`。

## T1：将清单解析改成线性复杂度

**文件：** 新建 `src/adapters/json/strict_json.{h,cpp}`、`tests/adapters/strict_json_test.cpp`；修改 `src/config/runtime_config.cpp`、`src/adapters/rag/native_rag_pack_verifier.cpp`、`CMakeLists.txt`。

**接口：** `Result<nlohmann::json> parse_strict_json(std::string_view bytes, std::size_t maximum_bytes)`。不负责文件可信检查；调用方保留原文件约束和错误文本映射。

- [ ] 写失败测试：顶层/嵌套/转义后同名重复键拒绝；不同对象相同键允许；空输入、尾随内容、截断、非法数字、深度超限拒绝。深度上限定为 128，并保证 SAX 与 DOM 相同语法选项。
- [ ] 写 1k/10k/64k 文件记录基准，复现当前 callback 对长数组的增长；保留原实现结果作对照，不在普通单元测试用脆弱毫秒断言。
- [ ] 采用调查中已验证的两阶段思路：

```text
检查 bytes <= maximum_bytes
SAX：进入对象压入键集合，key 插入失败则拒绝，离开对象弹出
SAX：限制容器嵌套深度，保留严格 UTF-8/数字/尾随内容规则
SAX 成功后使用无 callback 的 JSON DOM 解析
```

- [ ] 接入两处读取函数，保持 pinned manifest SHA 和信任检查。真实清单 Release 解析预算 ≤ 250 ms；不同机器同时报告样本和增长曲线。
- [ ] 构建/测试：`cmake --build build/vs2022 --config Release --target strict_json_tests rag_runtime_config_tests native_rag_pack_verifier_tests`；`ctest --test-dir build/vs2022 -C Release -R "^(strict_json_tests|rag_runtime_config_tests|native_rag_pack_verifier_tests)$" --output-on-failure`。
- [ ] 复测真实菜单和 native 首进度时间，独立提交 `perf(json): validate large manifests in linear time`。

## T2：统一 RAG 时限、取消和阶段状态

**文件：** 新建 `src/ports/operation_context.h`；修改 `src/ports/knowledge_provider.h`、`src/ports/rag_pack_verifier.h`、`src/ports/jsonl_process.h`、相关 Empty/Persistent provider、Native verifier、Reproc process、RuntimeEngine，以及对应 fake/test 实现；修改 `src/config/runtime_config.cpp`、`.env.example`。

**接口：** 为检索/验证增加 `const OperationContext&`；该对象生命周期覆盖同步调用。定义：

```cpp
struct OperationContext {
    const Cancellation* cancellation{nullptr};
    std::chrono::steady_clock::time_point deadline;
    bool cancelled() const noexcept;
    std::int64_t remaining_ms() const noexcept;
};
// KnowledgeProvider::retrieve(const TaskState&, const OperationContext&)
// RagPackVerifier::verify_executable_payload(path, const OperationContext&)
```

- [ ] 先写验证中/启动中/等待查询中取消和 deadline 过期测试；包括 Ctrl+C 后立即下一问，前次响应不得污染新请求。
- [ ] 循环按块或有界间隔检查取消；进程 start/exchange 接收剩余时间，整个任务时限取各阶段与父 deadline 的较早者。阻塞 SDK 不能取消时，停止自有 sidecar，而非留下线程。
- [ ] 确认取消后的只读句柄和子进程释放；检查 stop 与 operation mutex 不形成死锁。若系统调用不能满足 500 ms 前台预算，使用可中断工作线程并保证最多 2 s 回收，资源不得交给下一请求复用。
- [ ] 旧 `AGENT_RAG_TIMEOUT_SECONDS` 仅在新 query 配置未设置时作为兼容别名；新旧同时出现以新项优先，发一次不含敏感值的弃用提示。它不充当 bootstrap 总时限。新增总初始化预算默认 8 s；在完成性能优化前只在候选配置启用，避免中间版本全部超时。
- [ ] 运行 runtime_engine、rag_runtime_config、native verifier、persistent provider、reproc process、interactive_process 相关测试及真实取消场景。
- [ ] 独立提交 `feat(runtime): bound and cancel the complete RAG operation`。

## T3：去掉法规数据库启动全表扫描

**文件：** 修改 `rag/agent_rag/hybrid_index.py`、`hybrid_retriever.py`、`pack.py`、`sidecar.py`；测试 `rag/tests/test_hybrid_index.py`、`test_hybrid_retriever.py`、`test_pack.py`。新增 `scripts/upgrade_rag_index_metadata.py`，只针对新候选包运行。

**接口：** 新版索引 metadata 增加精确 `row_count`、`sum_token_count`；使用独立索引 schema 3。reader 明确支持旧/新版；旧版保留原校验慢路径并标记 legacy，不能自动修改旧包。

- [ ] 写失败测试：统计缺失/不一致、向量行跳号/重复、错模型身份均拒绝；用 SQL trace 证明新版启动不执行 AVG 全表扫描。
- [ ] 构建期累计统计并创建覆盖索引：

```sql
CREATE INDEX chunks_vector_row_cover ON chunks(vector_row, chunk_id);
-- 构建/升级时计算一次并写 metadata；运行时读取数字，拒绝非法值。
```

- [ ] 在发布验证中全量验证连续映射；运行时通过覆盖索引读取紧凑映射，检查长度和边界。平均长度由 sum/count 计算，禁止从历史统计不明来源推断。
- [ ] 对旧库副本生成新 metadata/索引，不重新编码全部法规向量；确认 document/chunk 内容 hash 和向量行对应不变。更换 embedding 后端时由 T6 另建对应向量。
- [ ] 运行 `build/pytest-venv/Scripts/python.exe -m pytest rag/tests/test_hybrid_index.py rag/tests/test_hybrid_retriever.py rag/tests/test_pack.py`；真实包初始化数据库阶段目标 ≤ 300 ms，记录查询计划和读取量。
- [ ] 独立提交 `perf(rag): publish startup statistics and covering row mappings`。

## T4：精简执行载荷并加速完整性验证

**文件：** 修改 `scripts/build_ecfr_knowledge_pack.ps1`、`rag/agent_rag/pack.py`、`src/adapters/rag/native_rag_pack_verifier.{h,cpp}`、`CMakeLists.txt`；新增 `src/adapters/rag/verified_pack_lease.{h,cpp}`、`tests/adapters/verified_pack_lease_test.cpp`。保留现有 pack 构建契约测试。

**接口：** `VerifiedPackLease` 为不可复制、可移动 RAII 对象，拥有规范化根目录、manifest_sha256、经验证文件句柄及后端身份；只能由 Native verifier 创建并作为其成员持有。T2 的 `Result<void> verify_executable_payload(path, context)` 返回类型保持不变，成功即建立 lease，失败清空；provider 继续引用 verifier。由 main 的现有所有权顺序保证先销毁 provider/停止 sidecar，再销毁 verifier/释放 lease。不跨未知进程复用。

- [ ] 写真实大小载荷和假篡改测试：改 1 字节但保持文件大小/mtime、替换同名文件、插入可导入模块、junction/hardlink、锁后替换、取消中断；未验证内容不能执行。
- [ ] 用 Windows CNG 的 SHA256 算法提供器按大块读取，逐块检查 OperationContext；对相同 bytes 与已有 SHA 实现做已知向量/多块/空文件一致性检查。先不删除旧实现，保留对照。
- [ ] 生成 pure-Python 归档和最小依赖闭包；保留必需的 DLL/pyd、tokenizer、所选模型后端。扫描实际 imports 和运行正反向场景，不能按文件名批量删依赖。
- [ ] 启动 Python 使用隔离模块路径；归档和原生扩展目录都纳入验证，不加载工作目录/用户 site/未知 `.pth`。目录改变触发拒绝，不以“此前检查过”接受新文件。只消除一次有效 lease 内的重复路径工作。
- [ ] 将原生可执行文件验证对象数量、总字节与 elapsed 写入基准；目标 native 校验 P95 ≤ 1 s 是阶段预算，达不到则先压缩载荷/改善读路径，不取消哈希。
- [ ] 跑 native verifier、pack、build-script contract 与真实 sidecar import/查询；完整性测试全绿后独立提交 `perf(pack): minimize and verify the executable runtime payload`。

## T5：检索路由与轻量 sidecar 就绪

**文件：** 新增 `src/application/retrieval_policy.{h,cpp}`、`tests/application/retrieval_policy_test.cpp`；修改 RuntimeEngine、SessionEngine、RagConfig/RuntimeConfig、InteractiveCli、Persistent provider；修改 `src/adapters/rag/rag_protocol.{h,cpp}`、`rag/agent_rag/protocol.py`、`sidecar.py`、`hybrid_retriever.py` 与现有协议/sidecar 测试。

**接口：**

```cpp
enum class RetrievalPolicy { Auto, Always, Off };
enum class RetrievalNeed { None, ExactReference, Semantic };
struct RetrievalDecision { RetrievalNeed need; std::string reason_code; };
// decide_retrieval(text, explicit_policy, session_regulatory_context)
```

协议 v3 的 ready capability 明确区分 `index_ready` 与 `embedding_ready`；启动请求的目标 capability 和错误结果版本一致。旧 v2 只用于旧包，不能把未加载模型的 v3 就绪帧送给 v2 decoder。

- [ ] 冻结中英文路由集：法规省略表达、追问“那例外呢”、引用、法规相关代码混合问题全部必须查询；明确普通任务才跳过。测试 explicit always 优先和 off 只由用户选择。
- [ ] 保守本地规则决定 None/Exact/Semantic；不为路由新增网络调用。法规会话标签只帮助保守地“继续检索”，不凭摘要缺少关键词跳过。
- [ ] 拆开加载索引和加载 embedding，精确定位在小映射里完成。现有 dense/hybrid 默认不因精确命中自动改为 lexical；等价/质量验证通过才启用精确快速路径。
- [ ] 为 `authoritative_no_match` 写证据闭包测试：只能来自有明确范围的权威映射判定；初始化失败、语义未命中或超时不能使用该状态。
- [ ] 对不同协议版本、缺失 capability、准备期间取消、下一问由 None→Semantic 的状态切换运行真实进程测试。
- [ ] 路由法规正例零漏判，精确/语义质量门槛通过后，独立提交 `feat(rag): prepare only the retrieval capabilities a turn requires`。

## T6：满足真正冷启动的编码后端预算

**文件：** 修改 `rag/agent_rag/embedding.py`、`hybrid_index.py`、`pack.py`、模型锁/包构建脚本；新增 `benchmarks/rag_backend_latency.py`、`rag/tests/test_embedding_backend_identity.py`。候选模型资产只进新包。

**接口：** 后端身份固定为 `(model_id, revision, backend, precision, tokenizer_sha256, dimensions)`，进入 index metadata、retrieval_revision 和 cache key。没有身份匹配则拒绝读旧向量。

- [ ] 用 baseline BGE-M3 分别测 hash、Python import、权重映射、首次 encode 和后续 encode，冷/暖分开。CPU 线程测试 1/4/8/默认，记录整机资源而非只看最快单点。
- [ ] 按顺序评估安全且支持的权重格式、删去未使用 ONNX 副本、CPU 调优；CUDA 仅在设备/显存与稳定性测试通过后成为可选 auto 结果。
- [ ] 若 T1–T5 后 machine-cold 仍超 8 s，评估 BGE-M3 量化后端或较小编码模型；每个候选单独重建向量并跑 Spec 质量集，不能复用不兼容浮点向量假装等价。
- [ ] 为每个候选输出统一表：冷/暖延迟、RSS/VRAM、Recall@5、MRR、引用正确率、错误率。同一 BGE-M3 的后端候选只有所有质量门槛通过且改善冷启动才可进入发布；更换 embedding 模型的候选仅形成评测结果，需单独明确选择才更换默认。无候选通过则保持原后端并将整体目标标为未达标。
- [ ] 不使用跨 EXE 常驻进程掩盖本阶段失败。提交仅包含胜出且可复现的实现/锁文件，命名 `perf(rag): reduce verified embedding cold-start cost`。

## T7：消除工具轮中的重复法规检索

**文件：** 新增 `src/adapters/rag/task_evidence_cache.{h,cpp}`、`tests/adapters/task_evidence_cache_test.cpp`；修改 Persistent provider、RuntimeEngine 任务终结/恢复接线。

**接口：** `EvidenceCacheKey { task_id, workspace, query, retrieval_revision, mode, top_k, max_total_bytes }`；每任务最多一份证据，内容上限沿用 32,768 字节，不新增跨会话全局 cache。

- [ ] 用 fake retriever 计数：同任务连续三个工具轮只检索一次；修改 query/workspace/revision 任意项必须再次检索。
- [ ] 写失败结果不缓存、任务结束释放、恢复后重新建立 lease、显式刷新失效测试。
- [ ] 顺序固定为先确认当前 lease/revision 有效，再查任务 cache；避免先从 cache 返回已经失效的旧包证据。
- [ ] 运行 runtime_engine 与 provider 测试，真实多工具场景验证检索次数、证据一致性及本地额外开销。
- [ ] 独立提交 `perf(rag): reuse immutable evidence within a task`。

## T8：移除前台记忆维护与历史回放长尾

**文件：** 新增 `src/application/memory_maintenance_scheduler.{h,cpp}`、`tests/application/memory_maintenance_scheduler_test.cpp`；修改 InteractiveCli、MemoryEngine、main.cpp；有基准证据时再修改 JsonlSessionStore/JsonlMemoryStore 并新增版本化投影测试。

**接口：** scheduler 接收 `(session_id, through_sequence)`；worker 输出候选与所依据的序号，正式 MemoryStore 的写入只能由单一 owner 提交。不能直接把现有 `consolidate()` 扔进并发线程。

- [ ] 失败测试：启动、退出、换会话不等待 fake 模型；前台请求抢占维护；忘记事件发生后旧维护结果不得恢复该记忆；checkpoint 不重复推进。
- [ ] 将 `MemoryEngine::consolidate` 分成读取固定转录、抽取候选、检查版本并提交三个步骤；抽取使用独立可取消 ModelClient，主模型连接不共享可变状态。
- [ ] 维护可由持久化会话日志和 checkpoint 恢复，退出只记录未完成范围，不等网络；显式 remember/forget 继续同步且成功后可立即读取。
- [ ] 对 10/1,000/10,000 会话跑菜单和提交基准；超过预算则增加由 JSONL 重建的版本化投影和有界分页，验证 tail replay、截断/损坏和新旧投影兼容，不直接合并 Agent_G。
- [ ] 长会话 compaction 维持 hard limit 与承诺对话完整性；单独测不可避免的压缩模型时间，不能把丢历史当优化。
- [ ] 运行 memory/session/interactive 相关测试与中断恢复测试，独立提交 `perf(session): keep maintenance off the foreground path`。

## T9：核实 HTTP 连接复用和首字真正及时显示

**文件：** 修改 `src/adapters/anthropic/cpr_http_transport.{h,cpp}`、`src/adapters/anthropic/anthropic_messages_client.cpp`、`src/adapters/anthropic/anthropic_stream_assembler.cpp`、`src/cli/terminal_presenter.cpp`、`src/cli/streaming_terminal_text.cpp`；扩展 `tests/adapters/anthropic_messages_client_test.cpp`、`tests/cli/terminal_presenter_test.cpp` 和本地 SSE 夹具。

**接口：** 保持 ModelClient 输出事件契约；transport 若新增连接池，每个并发调用独占 session，重置 headers/body/callback/cancel，禁止跨源重定向。维护流量与前台流量不共用同一有锁阻塞 session。

- [ ] 写 SSE 分帧/多字节 UTF-8/长停顿/错误状态/取消测试，测文本事件到终端的延迟；未完成或非法帧不显示为最终成功文本。
- [ ] 先测 DNS/TLS/连接建立与复用收益。只有测量显示连接创建是热点才持久化 cpr session；测试切换 API key/base_url 不复用旧凭据。
- [ ] 保持现有输出长度与模型质量，不默认切换模型或削减答案。前台 prompt 稳定前缀和去重上下文可以调整，但不能删除任务约束、证据或必需历史；provider 缓存能力须核对其自身文档后使用，不能套用 OpenAI 配置到 MiniMax。
- [ ] 与同 provider 的直连基线比较 T_send→first_text，若模型服务本身仍使体验超标，在报告中列出差距和候选后端方案，不能把原因改标成本地已全部完成。
- [ ] 通过流式/工具调用/取消回归后，独立提交 `perf(stream): bound transport and first-text presentation overhead`。

## T10：成套发布、回滚与最终对齐验收

**文件：** 修改 `cmake/stage_ready_package.cmake`、`verify_ready_package.cmake`、`tests/cli/ready_package_contract_test.cmake`、`tests/cli/interactive_startup_process_test.cmake`；更新新包 manifest 固定哈希；新建 `docs/validation/2026-09-17-cli-latency-parity.md`。

- [ ] 候选包在新目录离线生成，一次性升级索引/后端/协议；不在用户每次问答时升级。记录源包/新包内容计数、抽样或全量内容 hash，以及向量/模型身份。
- [ ] 首先通过完整包验证，再生成固定 manifest SHA 的 EXE；Ready 仍保持四文件交付，知识包在外部。源码测试通过不等于交付成功。
- [ ] 执行适当完整 CTest/Python 测试；保存命令、退出码、明确失败清单。没有干净机器测试就不能写成干净机器验收。
- [ ] 在实际 Ready 上执行 Spec 全场景矩阵与 Codex 配对比较，保留全部原始 JSONL；machine-cold 运行需在实施阶段另行安排可中断机器使用的时段。
- [ ] 用成套旧 EXE+旧知识包+旧 pointer 回滚；先停止关联进程再切换活动版本，校验 `.env` 和 runtime_data 保留，不能只回滚 EXE 而留下不兼容协议/索引。
- [ ] 报告逐项列出“已达标 / 未达标 / 尚未测量”，给出成本和质量变化。即使从 173 秒降到 10 秒，也只有实际满足全部目标才算完成。
- [ ] 验收通过后才进行正式 Ready 替换与发布提交；当前计划回合不执行这些动作。

## 自审映射与停止条件

| 需求 | 对应任务 | 不能替代的验证 |
|---|---|---|
| Codex 对齐与真实 TTFT | T0、T9、T10 | 同机配对、模型差异披露 |
| 首轮与重启延迟 | T1、T3、T4、T5、T6 | 无 sidecar 的立即首问与 machine-cold |
| 同会话与工具轮 | T5、T7 | query/revision 变化与失效 |
| 历史/记忆长尾 | T8 | 多会话规模、忘记与并发恢复 |
| 取消与超时 | T2、T5、T8、T9 | 前台恢复和实际资源回收 |
| 法规质量和安全 | T1、T3–T7、T10 | 引用、召回、完整性拒绝、证据工具边界 |
| 发布一致性 | T10 | 真实 Ready+新包，成套回滚 |

停止实施并报告的条件：正确性或可信执行约束被削弱；需要用户未授权的模型/成本改变；真实 cold 指标仍失败但拟以 warm 代替；新包改变了语料/引用且无法解释；机器资源不足以完成可比较测量。不能因为工期压力把未测项勾成通过。

本文所有任务保持未勾选。下一步需要的是单独的执行指令；用户当前要求的“仅制定修改计划”已在此完成，不自动进入实施。
