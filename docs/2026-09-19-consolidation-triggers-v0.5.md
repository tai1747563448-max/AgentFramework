# 长期记忆沉淀触发器 v0.5 实施计划

> 状态：方案草案，等你 review 后再动代码。
> 关联：`docs/2026-09-19-memory-retrieval-improvements.md`（长期记忆讨论稿 v0.1）
> 不修改 RAG 子系统，不引入"周期摘要池"，T1 完整版留到 v1。

## 一、目标 / 非目标

### 目标（v0.5 必须完成）
1. **多触发器**沉淀长期记忆：T2（token 阈值）+ T3（时间窗）。每个触发器可独立开关。
2. **上限节流**：两次沉淀之间的最短间隔（默认 24h）。
3. **下限兜底**：即使所有触发器都没触发，到时间也强制沉淀一次。
4. **后台执行**：触发后**不阻塞**前台 turn。
5. **主开关**："长期记忆" on/off。off 时不构建任何 consolidate 相关对象。

### 非目标（明确排除，留给后续）
- ❌ **T1（主题显著无关）** 的完整实现 —— 需要"周期摘要池"作为对照集，是独立子项目。
- ❌ **周期摘要池 / 海马体层** 的持久化。
- ❌ **curate prompt 重写**（精挑细选 vs 摘要）。
- ❌ **RAG 子系统的任何改动**。
- ❌ **运行时热切换配置**。

## 二、配置 Schema

### `MemoryPolicyConfig` 新增字段

文件：`src/application/memory_policy.h`

```cpp
struct MemoryPolicyConfig {
    std::size_t max_entry_bytes{4096};
    std::vector<std::string> protected_values;
    // ↓↓↓ 新增 ↓↓↓
    bool long_term_memory{true};            // 主开关：关掉 = 无跨会话记忆
    ConsolidationTriggers consolidation{};   // 主开关为 true 时生效
};

struct ConsolidationTriggers {
    bool enabled{true};                       // 子开关（默认与 long_term_memory 同语义）
    bool trigger_on_token_threshold{true};   // T2
    bool trigger_on_time_elapsed{true};      // T3
    std::size_t token_threshold{50'000};      // T2 阈值
    std::chrono::hours time_elapsed{24};      // T3 阈值（同 time_elapsed）
    std::chrono::hours rate_limit{24};       // 上限节流窗口
    bool force_periodic_floor{true};         // 下限兜底开关
    double topic_divergence_threshold{0.7};  // T1 阈值（v0.5 不读，仅占位）
};
```

### 字段语义说明

| 字段 | 含义 |
|---|---|
| `long_term_memory` | 主开关。`false` → `build_engine` 不构造 `MemoryConsolidator` / `MemoryMaintenanceScheduler`，整个引擎退回 DeepSeek 模式（纯事件日志） |
| `consolidation.enabled` | 子开关。如果为 `false`，跳过评估逻辑但其他对象仍构造（用于调试） |
| `trigger_on_token_threshold` | T2 是否启用 |
| `trigger_on_time_elapsed` | T3 是否启用 |
| `token_threshold` | T2 阈值，单位是"周期内累计输入 token 数" |
| `time_elapsed` | T3 阈值，也是下限兜底的判定窗口 |
| `rate_limit` | 上限节流窗口。两次沉淀至少间隔此时间 |
| `force_periodic_floor` | 下限兜底开关。true 时强制每 `time_elapsed` 至少沉淀一次 |
| `topic_divergence_threshold` | 占位字段，v0.5 不读 |

## 三、数据结构变更

### `MemoryState` 新增字段

文件：`src/domain/memory_state.h`

```cpp
struct MemoryState {
    std::uint64_t last_sequence{0};
    std::string last_timestamp_utc;
    std::map<std::string, MemoryEntry> active_entries;
    std::map<std::string, std::uint64_t> session_checkpoints;
    std::set<std::string> used_memory_ids;
    // ↓↓↓ 新增 ↓↓↓
    std::string last_consolidated_at_utc;          // 上次沉淀时间，ISO 8601；空表示从未沉淀
    std::size_t tokens_since_last_consolidation{0}; // 累计输入 token，重置于每次沉淀
};
```

### JSON 序列化兼容

- 旧 JSONL 事件流读到 `MemoryState` 时，没有这两个字段 → 默认值即可（空串 + 0）。
- 写入路径不变（`MemoryState` 不进事件流，事件流是 `MemoryEvent`）。
- 持久化的 `MemoryState` 在 `adapters/persistence/memory_event_json.cpp` 序列化时新增这两个字段。

## 四、调度器逻辑（核心伪代码）

文件：`src/application/memory_maintenance_scheduler.h/.cpp`

```cpp
enum class ConsolidationReason {
    None,                       // 未触发
    TokenThresholdExceeded,     // T2
    TimeElapsedSinceLast,       // T3
    FloorForced,                // 下限兜底
    Throttled,                  // 上限节流生效
    DisabledByConfig,           // 主/子开关关闭
};

struct ConsolidationDecision {
    bool should_fire{false};
    ConsolidationReason reason{ConsolidationReason::None};
    std::string reason_detail;  // 调试用人类可读串
};

class MemoryMaintenanceScheduler {
public:
    // 在 submit_turn 完成后、ScheduleAsync 前调用
    ConsolidationDecision evaluate(
        const MemoryState& state,
        const ConsolidationTriggers& config,
        std::size_t tokens_in_period,
        std::chrono::steady_clock::time_point now_utc) const;

    // 后台异步执行（沿用三步 API）
    void schedule_async(ConsolidationReason reason);
};
```

### 评估算法

```cpp
ConsolidationDecision evaluate(state, config, tokens, now):
    # 主开关
    if !config.long_term_memory:
        return {false, DisabledByConfig, "long_term_memory off"}

    # 子开关
    if !config.enabled:
        return {false, DisabledByConfig, "consolidation.enabled off"}

    # 首次沉淀特殊处理
    if state.last_consolidated_at_utc.empty():
        # 没有"上一次"，节流和兜底都不适用
        if config.trigger_on_token_threshold && tokens >= config.token_threshold:
            return {true, TokenThresholdExceeded, "first-ever, T2"}
        # 首次沉淀不强制兜底（避免启动 24h 后立刻花 LLM 成本）
        return {false, None, "first-ever, waiting for trigger"}

    # 后续沉淀
    time_since_last = now - parse(state.last_consolidated_at_utc)

    # 上限节流
    if time_since_last < config.rate_limit:
        return {false, Throttled,
                "throttled (" + time_since_last + " < " + rate_limit + ")"}

    # T2
    if config.trigger_on_token_threshold && tokens >= config.token_threshold:
        return {true, TokenThresholdExceeded, ...}

    # T3
    if config.trigger_on_time_elapsed && time_since_last >= config.time_elapsed:
        return {true, TimeElapsedSinceLast, ...}

    # 下限兜底
    if config.force_periodic_floor && time_since_last >= config.time_elapsed:
        return {true, FloorForced, ...}

    return {false, None, "no trigger fired"}
```

### Token 计数来源

`RuntimeUsageDelta.input_delta`（已在 `RuntimeEngine::run()` 中每个事件报出），累加到 `MemoryState.tokens_since_last_consolidation`。

注意：**只累加 input token**。output 不计入（避免"模型啰嗦"导致沉淀）。

## 五、Engine 集成

### `build_engine` 改动

文件：`src/composition/engine.cpp`

```cpp
std::unique_ptr<Engine> build_engine(const EngineConfig& config) {
    // ... 现有逻辑 ...

    std::unique_ptr<MemoryEngine> memory;
    std::unique_ptr<MemoryMaintenanceScheduler> scheduler;  // ← 新增
    std::shared_ptr<MemoryStore> memory_store;
    std::shared_ptr<SessionStore> session_store;

    if (config.memory_enabled) {
        memory_store = std::make_shared<JsonlMemoryStore>(config.runtime_root);
        session_store = std::make_shared<JsonlSessionStore>(config.runtime_root);
        MemoryPolicy policy(config.memory_policy);
        MemoryRetriever retriever;

        if (config.memory_policy.long_term_memory) {
            // 主开关开 → 构造 consolidator + scheduler
            auto consolidator = std::make_unique<ModelMemoryConsolidator>(
                *model_owned, ModelMemoryConsolidatorConfig{...});
            memory = std::make_unique<MemoryEngine>(
                *memory_store, *session_store, *consolidator, retriever,
                policy, *clock_ptr, *ids_ptr);
            scheduler = std::make_unique<MemoryMaintenanceScheduler>(
                *memory, *clock_ptr, config.memory_policy.consolidation);
        } else {
            // DeepSeek 模式：不构造 consolidator，传 no-op
            memory = std::make_unique<MemoryEngine>(
                *memory_store, *session_store, /*no-op consolidator*/,
                retriever, policy, *clock_ptr, *ids_ptr);
        }
    }

    auto runtime = std::make_unique<RuntimeEngine>(...);

    // 把 scheduler 注入 SessionEngine
    if (scheduler) {
        session_engine->attach_maintenance_scheduler(std::move(scheduler));
    }

    return ...;
}
```

### `SessionEngine` 改动

文件：`src/application/session_engine.h/.cpp`

```cpp
class SessionEngine {
public:
    // 新增：在 submit_turn 完成后调用 evaluate + 可选 schedule_async
    void attach_maintenance_scheduler(
        std::unique_ptr<MemoryMaintenanceScheduler> scheduler);

    SessionTurnResult submit_turn(...) {
        auto result = runtime.run(...);

        // ↓↓↓ 新增 ↓↓↓
        if (maintenance_scheduler_) {
            const auto decision = maintenance_scheduler_->evaluate(
                current_memory_state(),
                trigger_config_,
                accumulated_input_tokens_,
                clock_.now());
            if (decision.should_fire) {
                maintenance_scheduler_->schedule_async(decision.reason);
            }
        }

        return result;
    }
};
```

### 调用时机

`evaluate` 在 `submit_turn` **完成之后**调用 —— 不阻塞前台 turn 返回。
`schedule_async` 启动后台线程跑三步 API（read → extract → commit）。

## 六、文件改动清单

| 文件 | 改动 | 估时 |
|---|---|---|
| `src/application/memory_policy.h` | 新增 `ConsolidationTriggers` struct + `long_term_memory` 字段 | 0.5h |
| `src/domain/memory_state.h` | 新增两个字段 + 重载 `operator==` | 0.25h |
| `src/adapters/persistence/memory_event_json.cpp` | 新字段的序列化 | 0.5h |
| `src/application/memory_maintenance_scheduler.h/.cpp` | 新增 `ConsolidationDecision`、`evaluate()`、`schedule_async()` | 4h |
| `src/composition/engine.h/.cpp` | DeepSeek 模式分支 + scheduler 注入 | 1h |
| `src/application/session_engine.h/.cpp` | `attach_maintenance_scheduler` + `submit_turn` 调用 evaluate | 1h |
| `src/application/runtime_engine.cpp` | token 计数累加器（已部分实现，需补完整） | 0.5h |
| `tests/application/consolidation_trigger_test.cpp` | 新增测试文件 | 3h |
| `tests/application/memory_policy_test.cpp` | 配置解析测试 | 1h |
| **合计** | | **~12h** |

## 七、测试策略

### 单元测试矩阵（`consolidation_trigger_test.cpp`）

| 用例 | 期望 |
|---|---|
| 主开关 off | 不调用 evaluate（构造时跳过） |
| 子开关 off | 返回 `DisabledByConfig` |
| 首次沉淀 + T2 触发 | 触发，reason=`TokenThresholdExceeded` |
| 首次沉淀 + T3 已满足但 T2 未满足 | 不触发（T3 在首次不生效） |
| 首次沉淀 + 兜底开关开 | 不触发（首次不兜底） |
| T2 触发 + 节流窗口内 | 不触发，reason=`Throttled` |
| T2 触发 + 节流窗口外 | 触发 |
| T3 触发 + 触发器时间窗内 + 节流窗口外 | 触发，reason=`TimeElapsedSinceLast` |
| 全部触发器 off + 兜底开 + 时间到 | 触发，reason=`FloorForced` |
| 全部触发器 off + 兜底关 | 不触发 |
| token 累加重置于每次沉淀 | state 累加器在 commit 后归零 |

### 集成测试（`tests/integration/`）

- **场景 A**：模拟一次完整 turn → 触发 T2 → 后台调度 → commit → 验证 `MemoryState.last_consolidated_at_utc` 更新。
- **场景 B**：模拟连续 3 次 turn，每次都触发 T2，验证节流只让第一次 commit 发生。
- **场景 C**：`long_term_memory=false` → 启动 → 完成 turn → 验证无 scheduler 调用、无 consolidate 事件。

### 现有测试影响

| 文件 | 影响 |
|---|---|
| `memory_engine_test.cpp` | 新增字段不破坏现有断言（默认空值不影响） |
| `memory_retriever_test.cpp` | 无影响（retriever 不读新字段） |
| `memory_reducer_test.cpp` | replay 路径需要测试"空 last_consolidated_at_utc" 兼容 |
| `interactive_process_tests`（verifier） | 不涉及 |

## 八、边界 case 与默认参数

| Case | 行为 |
|---|---|
| 首次启动，`MemoryState` 从无到有 | `last_consolidated_at_utc` = 空，不触发 |
| 用户在 24h 内连续多次手动 consolidate | 节流生效，第二次直接拒 |
| Token 阈值极小（= 0） | 几乎每次 turn 都触发 T2 → 节流保护 |
| Token 阈值极大（= ∞） | T2 永不触发，靠 T3 + 兜底 |
| 时间窗口被改小（如 1h） | T3 + 兜底每 1h 触发一次；节流也按 1h 算 |
| 跨进程：用户关掉程序再开 | `MemoryState` 持久化，新 token 累加从 0 开始，time_since_last 跨进程计算 |
| 配置从 on 切到 off（运行时） | **不支持**。重启引擎生效（v0.5 不做热切换） |
| consolidate 后台失败（LLM 超时） | `schedule_async` 失败 → 记日志但不重试，下一次 turn 重新评估 |

## 九、对外可见行为变化

### CLI 用户视角

- 长期记忆配置块（`.env`）：
  ```env
  AGENT_MEMORY_LONG_TERM=on   # 或 off
  AGENT_MEMORY_CONSOLIDATION_TOKEN_THRESHOLD=50000
  AGENT_MEMORY_CONSOLIDATION_TIME_ELAPSED_HOURS=24
  AGENT_MEMORY_CONSOLIDATION_RATE_LIMIT_HOURS=24
  AGENT_MEMORY_CONSOLIDATION_FLOOR=on
  ```
- 运行日志新增 `consolidation_decision` 字段（reason / 详细说明）。
- `agent_ready_package_verify` 新增断言：默认配置下启动 24h 后必须至少沉淀一次（注入假时钟测试）。

### SDK 视角

- `EngineConfig.memory_policy` 新增字段。
- 旧调用方不传新字段 → 默认值生效，行为保持兼容。

## 十、后续工作（明确不在 v0.5）

### v1：T1 完整实现
- 引入"周期摘要池"（海马体层）：每场会话结束时由模型生成 ≤500 字的摘要，存入 `runtime_data/periods/<period_id>/summaries.jsonl`。
- `evaluate()` 增加 T1：算加权 Jaccard 当前 prompt 与周期摘要池，触发条件同 T2/T3。
- 新文件：`application/period_probe.{h,cpp}`、`adapters/persistence/period_summary_store.{h,cpp}`。

### v2：curate prompt 重写
- `ModelMemoryConsolidator` prompt 从"摘要整段对话"改为"按 5 个 `MemoryCategory` 槽精挑细选 + 自查 3 个月后是否有用"。
- 测试用例扩展，新增"是否保留哪些事实"的人工评估脚本。

### v3：RAG 集成（按你之前讨论的"RAG 进核心"提案）
- `MemoryEngine` 接入向量索引，记忆检索走 RAG。
- 与本文件无依赖。

## 十二、待你拍板的点

1. **是否同意 v0.5 的范围**（T2 + T3 + 节流 + 兜底 + 主开关，无 T1）？
2. **默认参数**：`token_threshold=50000`、`time_elapsed=24h`、`rate_limit=24h`、`force_periodic_floor=true` —— OK 吗？还是要更激进？
3. **节流窗口**和**T3 阈值**默认共用 24h 还是分开成两个独立配置？我倾向前者（少一个参数），但你定。
4. **是否同意把 `attach_maintenance_scheduler()` 加到 SessionEngine**，而不是 Engine facade？因为 Engine::ask 是 stub，SessionEngine 是真正干活的层。
5. **`schedule_async` 用 std::thread 还是 std::async？**我倾向 `std::thread` + 显式 join at shutdown（生命周期可控）。同意吗？

> 等你 review 完回这五个，我再开工。