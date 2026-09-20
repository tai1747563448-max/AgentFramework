# 长期记忆体系完整实现计划（三层 + 周期 + 沉淀）

> 状态：完整方案 v0.2。涵盖 #1~#9 全部 9 个子想法 + 触发器机制 + 神经科学补丁 6 条。
> 关联：
> - `docs/2026-09-19-memory-retrieval-improvements.md`（检索改进讨论稿）
> - `docs/2026-09-19-consolidation-triggers-v0.5.md`（v0.5 触发器草案，被本文件"阶段 1"吸收）
>
> **设计哲学：借鉴而非复刻**。本计划只采纳有明确工程化收益的神经科学原理（主动遗忘、难度档位、抽象整合、pattern separation、SWR 反哺、矛盾检测）。任何"为了像大脑而做"的设计都被剔除。

## 神经科学借鉴清单（2026-09-19 review）

来自 [Sleep's contribution to memory formation (Physiological Reviews, 2024)](https://journals.physiology.org/doi/full/10.1152/physrev.00054.2024) 等综述。明确采纳与不采纳：

| 原理 | 是否采纳 | 采纳方式 |
|---|---|---|
| 工作记忆 / 海马体 / 新皮层 三层模型 | ✅ | 完整建模（阶段 2） |
| 睡眠双阶段巩固（NREM 稳定 + REM 抽象）| ✅ | 阶段 5 = NREM，阶段 5b = REM |
| 主动遗忘 / Synaptic Homeostasis | ✅ | 阶段 8，**默认关闭** |
| 摘要级保留（hierarchical temporal memory） | ✅ | 阶段 8 核心，**摘要存进 Period.episodic_summary** |
| Bjork storage vs retrieval strength | ✅ | `retrieval_difficulty` 字段 + 阶段 6 排序调整 |
| SWR 双重作用（提取=巩固） | ✅ | 每次检索 `difficulty -= decrement` |
| Pattern separation（DG 抗混淆）| ✅ | dedup 阈值 ≥ 0.95 |
| 矛盾检测 / Schema 一致性 | ✅ | curate prompt 加 `conflicts_with` 槽 |
| 慢波振荡 / theta 节律 | ❌ | 不模拟时间振荡 |
| 神经递质 / 蓝斑核 / VTA 多巴胺 | ❌ | 太底层，无工程映射 |
| 海马 indexing theory | ⚠️ | 部分采纳：`MemoryEntry.memory_id` 当作 index，详情不模拟 |

## 一、整体愿景：三层次记忆

把人类记忆的神经科学模型作为类比，明确每层对应代码里的什么：

```text
┌─────────────────────────────────────────────────────────────────┐
│ 第一层：工作记忆（Working Memory）                            │
│   = 当前 session 的完整 EventStore                            │
│   现状：✅ 已存在（SessionEngine + EventStore）                │
│   大小：受 ContextCompactor 控制                              │
├─────────────────────────────────────────────────────────────────┤
│ 第二层：海马体 / 情景记忆（Episodic Memory）                  │
│   = "周期"内近期 N 场会话的摘要 + 关键事实                    │
│   现状：❌ 完全不存在                                         │
│   新增：PeriodStore + PeriodManager + PeriodProbe             │
│   大小：受 PeriodSummaryPolicy 控制                           │
├─────────────────────────────────────────────────────────────────┤
│ 第三层：新皮层 / 长期记忆（Long-Term Memory）                 │
│   = 精挑细选后的事实条目，按 MemoryCategory 分类              │
│   现状：✅ 部分存在（MemoryEngine + JSONL）                   │
│   升级：curate prompt + 倒排索引 + 强制类别                    │
│   大小：受 MemoryPolicy.max_entry_bytes 控制                  │
├─────────────────────────────────────────────────────────────────┤
│ 睡眠巩固（REM / Consolidation）                                │
│   = 后台 curate 流程，多触发器触发                            │
│   现状：⚠️ 部分存在（ModelMemoryConsolidator）                │
│   升级：触发器机制（v0.5）+ curate prompt 重写 + 节流/兜底     │
└─────────────────────────────────────────────────────────────────┘
```

会话在三层之间的流转：

```text
        ┌────────────────────────────────────┐
        │ 用户开启新 session                  │
        │ (这本身是一次显式的"上下文压缩")   │
        └─────────────┬──────────────────────┘
                      ▼
        ┌────────────────────────────────────┐
        │ PeriodProbe:                        │
        │   "这个新会话属于已有周期吗？"      │
        │   算 weighted Jaccard              │
        │   vs 所有开放周期的摘要 token 集    │
        └─────────────┬──────────────────────┘
                ┌─────┴──────┐
                ▼            ▼
        相似度高         相似度低
        attach 到       start 新周期
        已有周期
                │
                ▼
        ┌────────────────────────────────────┐
        │ 第一层工作：当前 turn 正常推进      │
        │ 累加 input token 到周期计数器       │
        └─────────────┬──────────────────────┘
                      ▼
        ┌────────────────────────────────────┐
        │ 每场 session 结束时：                │
        │   1. 生成 session summary (LLM)     │
        │   2. 累加到所属周期的 summary 池    │
        │   3. 更新 summary_tokens            │
        └─────────────┬──────────────────────┘
                      ▼
        ┌────────────────────────────────────┐
        │ 周期关闭触发器评估（T1/T2/T3 + 兜底）│
        │ 评估时机：每场 session 结束后       │
        └─────────────┬──────────────────────┘
                      ▼
        ┌────────────────────────────────────┐
        │ 后台 curate 任务启动                 │
        │   read → extract → commit 三步      │
        │   按 5 个 MemoryCategory 精挑细选   │
        │   写进第三层 MemoryEngine           │
        └────────────────────────────────────┘
```

## 二、9 个子想法 → 阶段映射

| # | 想法 | 阶段 | 依赖 |
|---|---|---|---|
| 1 | "周期"作为一等公民 | **阶段 2** | 阶段 1（state 字段） |
| 2 | 周期内会话不严格隔离 | **阶段 3**（PeriodProbe） | 阶段 2（PeriodStore） |
| 3 | 新会话=压缩事件 | **阶段 7** | 阶段 3 |
| 4 | 启动时相关性排查 | **阶段 3** | 阶段 2 |
| 5 | 周期结束时沉淀长期记忆 | **阶段 1**（触发器） + **阶段 4**（T1） | 阶段 2 |
| 6 | 精挑细选 vs 简单压缩 | **阶段 5**（curate prompt） | 阶段 1 |
| 7 | 明文保存 | **阶段 0**（已具备） | — |
| 8 | 按类别分类 | **阶段 0**（已具备，阶段 5 强化） | — |
| 9 | 索引快速检索 | **阶段 6**（倒排索引） | 阶段 1 |

阶段 0~7 顺序执行，每完成一个阶段都有可独立 ship 的中间产物。

## 三、数据模型（最终形态）

### 新增：`Period`

文件：`src/domain/period_state.h`（新）

```cpp
struct Period {
    std::string period_id;                       // uuid
    std::string started_at_utc;                  // 起始时间
    std::string ended_at_utc;                    // 关闭时间，开放时为空
    std::vector<std::string> session_ids;        // 关联 session id 列表
    std::string source_session_id_first;         // 周期由哪场 session 开启
    std::string source_session_id_last;          // 周期最近一场 session

    // 海马体索引：每场 session 结束时的 summary 拼接
    std::string episodic_summary;                // 人类可读的周期摘要
    // 预计算的 token 集，给 PeriodProbe 用
    std::set<std::u32string> summary_tokens;     // 来自 MemoryRetriever 的 tokens()

    // 累加器（与 MemoryState 平行的字段，但属于"周期"维度）
    std::size_t tokens_in_period{0};             // 累计 input token
    std::uint64_t sessions_in_period{0};         // session 计数

    enum class State { Open, Consolidating, Closed };
    State state{State::Open};
};
```

### 修改：`MemoryEntry`

文件：`src/domain/memory_state.h`

```cpp
enum class FactLevel {
    Atomic,       // L0：单条事实，不可再分
    Composite,    // L1：多 atomic 抽象合并而成
    Abstract,     // L2：多 composite 进一步归纳（v3+）
};

enum class FactConfidence {
    Grounded,     // 有据可依（来自 curate 或用户显式）
    Deduced,      // 演绎推断：必然且唯一可推出（用户决策）
};

enum class FactState {
    Active,       // 参与 coefficient 竞争
    DeducedInto,  // coefficient 已归 0，被某 Deduced fact 吸收（用户决策）
    Tombstoned,   // 阶段 8 遗忘后状态
    Archived,     // 显式归档
};

struct TemporalContext {
    std::string valid_from_utc;     // 业务有效起始
    std::string valid_until_utc;    // 业务有效截止（默认 "至今"）
    std::string recorded_at_utc;    // 进入系统时间（transaction time）
};

struct MemoryEntry {
    // ... 现有字段 ...
    std::string source_period_id;                // ← 阶段 2
    std::uint64_t last_accessed_at_utc_ms{0};    // ← 阶段 6
    std::uint32_t access_count{0};               // ← 阶段 6
    
    // ↓↓↓ 神经科学补丁 + 事实森林 ↓↓↓
    FactLevel level{FactLevel::Atomic};
    FactConfidence confidence{FactConfidence::Grounded};
    FactState state{FactState::Active};
    TemporalContext temporal;                    // ← 阶段 5b 双时间戳
    
    double durability_score{1.0};                // 阶段 8 UI 用
    double retrieval_coefficient{0.0};          // 阶段 6 v2 归一化系数
    std::size_t curate_boost_remaining{0};      // 阶段 6 v2 boost 计数
    
    std::vector<std::string> conflicts_with;     // 阶段 5b Pass 3
    std::vector<std::string> superseded_by;      // 阶段 5b Pass 4
    std::vector<std::string> cluster_ids;        // 阶段 5b Pass 2
    
    // 事实森林关系
    std::vector<std::string> part_of;            // 父节点（这条 fact 是哪些抽象的子集）
    std::vector<std::string> parts;              // 子节点（这条抽象 fact 包含哪些子集）
    
    // Deduced fact 专用
    std::vector<std::string> deduced_from;       // 演绎来源（仅 Deduced）
    std::string deduction_rule;                  // 演绎规则名（仅 Deduced）
    std::string deduction_justification;         // 演绎推理说明（仅 Deduced）
    
    std::optional<std::string> tombstone_of;     // 阶段 8
};
```

**不变量**：
- 所有 `state == Active` 的 `MemoryEntry::retrieval_coefficient` 之和恒等于 1
- `state == DeducedInto` 的 entry `retrieval_coefficient == 0` 且 `deduced_from` 非空
- Deduced fact 的 `deduced_from` 全部指向 `state == DeducedInto` 的 entry
- 事实森林无环：`part_of` 和 `parts` 是 DAG

### 修改：`CommittedSessionTurn`（在 `session_state.h`）

```cpp
struct CommittedSessionTurn {
    // ... 现有字段 ...
    std::string period_id;                       // ← 新增：这场 turn 属于哪个周期
};
```

### 修改：`MemoryState`（v0.5 已有，新增）

```cpp
struct MemoryState {
    // ... 现有字段 + v0.5 新增的 ...
    std::string current_period_id;               // ← 新增：当前活跃周期（与 PeriodStore 关联）
};
```

## 四、最终配置 Schema

文件：`src/application/memory_policy.h`

```cpp
struct MemoryPolicyConfig {
    std::size_t max_entry_bytes{4096};
    std::vector<std::string> protected_values;
    bool long_term_memory{true};                // 主开关

    ConsolidationTriggers consolidation{
        .token_threshold = 50'000,               // T2
        .time_elapsed = std::chrono::hours{24},   // T3
        .rate_limit = std::chrono::hours{24},    // 上限节流
        .force_periodic_floor = true,            // 下限兜底
        .topic_divergence_threshold = 0.7,       // T1（阶段 4 启用）
    };

    PeriodConfig period{                         // ← 新增
        .auto_attach_threshold = 0.3,            // 阶段 3：attach 阈值
        .summary_max_chars = 800,                // 阶段 4：每场 session 摘要上限
        .max_sessions_per_period = 50,           // 阶段 2：周期内 session 上限
    };

    RetrievalConfig retrieval{                   // ← 新增（阶段 6）
        .use_inverted_index = true,              // 是否启用倒排索引
        .inverted_index_min_token_len = 2,       // 长度 < 2 的 token 不进索引
        .exact_phrase_bonus = 100,               // 短语命中加权（向后兼容）
    };

    CurationConfig curation{                     // ← 新增（阶段 5）
        .enabled = true,                         // curate prompt 是否启用
        .self_check_horizon_months = 3,          // "3 个月后还有用吗"
        .max_entries_per_session = 12,           // 单场 session 最多 curate 多少条
        .pattern_separation_threshold = 0.95,    // ← 神经科学补丁：dedup 阈值
        .enable_conflict_detection = true,       // ← 神经科学补丁：矛盾检测
        .enable_supersede_detection = true,      // ← 神经科学补丁：覆盖检测
    };

    ForgettingConfig forgetting{                 // ← 新增（阶段 8，默认关闭）
        .enabled = false,                        // 主开关，默认关闭
        .preserve_summaries = true,              // 摘要级保留
        .retention_half_life = std::chrono::hours{168}, // 7 天
        .forget_threshold = 0.1,                 // durability < 此值才忘
        .keep_category_always = true,            // Constraint 永不忘
    };

    RetrievalDifficultyConfig difficulty{        // ← 新增（阶段 6）
        .initial_difficulty = 0.5,
        .per_retrieval_decrement = 0.05,         // 每次检索降 0.05
        .min_difficulty = 0.0,
        .max_difficulty = 1.0,
        .top_k_boost_factor = 1.5,               // 难度高 → 优先级 ↑
    };

    RefinementConfig refinement{                 // ← 新增（阶段 5b）
        .enabled = true,                         // 阶段 5b 总开关
        .abstract_threshold = 0.7,               // Jaccard ≥ 此值的多 fact 抽象
        .cluster_entity_overlap = 0.5,           // 提到同一实体的 fact 聚类
        .conflict_detection_min_overlap = 0.4,   // 跨周期矛盾检测阈值
    };
};

struct ConsolidationTriggers {
    bool enabled{true};
    bool trigger_on_topic_divergence{true};     // T1
    bool trigger_on_token_threshold{true};      // T2
    bool trigger_on_time_elapsed{true};         // T3
    std::size_t token_threshold{50'000};
    std::chrono::hours time_elapsed{24};
    std::chrono::hours rate_limit{24};
    bool force_periodic_floor{true};
    double topic_divergence_threshold{0.7};
};

struct PeriodConfig {
    double auto_attach_threshold{0.3};
    std::size_t summary_max_chars{800};
    std::size_t max_sessions_per_period{50};
};

struct RetrievalConfig {
    bool use_inverted_index{true};
    std::size_t inverted_index_min_token_len{2};
    int exact_phrase_bonus{100};
};

struct CurationConfig {
    bool enabled{true};
    std::size_t self_check_horizon_months{3};
    std::size_t max_entries_per_session{12};
    double pattern_separation_threshold{0.95};  // 神经科学补丁
    bool enable_conflict_detection{true};
    bool enable_supersede_detection{true};
};

struct ForgettingConfig {
    bool enabled{false};
    bool preserve_summaries{true};
    std::chrono::hours retention_half_life{168};
    double forget_threshold{0.1};
    bool keep_category_always{true};
};

struct RetrievalDifficultyConfig {
    double initial_difficulty{0.5};
    double per_retrieval_decrement{0.05};
    double min_difficulty{0.0};
    double max_difficulty{1.0};
    double top_k_boost_factor{1.5};
};

struct RefinementConfig {
    bool enabled{true};
    double abstract_threshold{0.7};
    double cluster_entity_overlap{0.5};
    double conflict_detection_min_overlap{0.4};
    
    // 精细做法下的 LLM 调用预算（用户决策）
    bool use_budget{true};                       // 是否启用上限
    std::size_t max_llm_calls_per_period{50};    // 单次 REM 跑动最大 LLM 调用次数
    bool degrade_on_budget_exceeded{true};       // 超预算时降级（只跑 Pass 1）还是硬失败
};

### 检索系数（替代原"难度档位"，神经科学补丁 v2）

**设计哲学**：所有记忆的 `retrieval_coefficient` 总和恒为 1（资源竞争模型）。每次归一化周期让所有记忆的系数趋近均值；高访问次数的记忆衰减速度慢（常识级长寿）；新事实享有临时 boost 防止"一出场就沉底"。

```cpp
struct RetrievalCoefficientConfig {
    // 归一化触发
    std::chrono::hours normalization_interval{24};   // 时间触发
    std::uint64_t normalization_interval_events{100}; // 事件触发

    // 半衰期增长曲线（spacing effect 的工程化）
    double initial_half_life_days{7.0};             // access=0 → 7 天衰减到一半
    double half_life_growth_rate{0.916};            // log(2.5)，控制增长曲线陡峭度
    double max_half_life_days{365.0};               // 上限：1 年

    // 新事实 boost（防止"一出场就沉底"）
    double curate_boost{0.02};                      // 每次归一化加多少
    std::size_t curate_boost_periods{3};            // 持续几个归一化周期

    // 检索时的修正
    double score_multiplier_max{2.0};               // coefficient=1 时分数翻倍
};
```

**算法见阶段 6 节。**
```

## 五、阶段 1：触发器机制（v0.5，~12h）

**目标**：沉淀时机由多触发器 + 节流/兜底联合决定。

**交付物**：
- `MemoryPolicyConfig.long_term_memory` 主开关
- `ConsolidationTriggers` 配置块
- `MemoryState.last_consolidated_at_utc` + `tokens_since_last_consolidation`
- `MemoryMaintenanceScheduler::evaluate()` + `schedule_async()`
- SessionEngine 注入 scheduler，submit_turn 后调用 evaluate
- 测试矩阵：12 个单测 + 3 个集成测

**依赖**：无（独立可做）

**详细设计**：见 `2026-09-19-consolidation-triggers-v0.5.md`（不再赘述）

## 六、阶段 2：Period 作为一等公民（~16h）

**目标**：Period 是持久化的一等数据对象，session 归属某个 period。

### 数据结构
- 新文件 `src/domain/period_state.h`（见第三节）

### 持久化
- 新文件 `src/adapters/persistence/period_store.h/.cpp`
- JSONL 存储：`runtime_data/periods/periods.jsonl`
- 每条事件：`{seq, ts, type, period_id, payload}`

### PeriodManager 服务
- 新文件 `src/application/period_manager.h/.cpp`
- 端口（`ports/period_store.h`）+ 适配器（`adapters/persistence/jsonl_period_store`）
- 公开方法：
  ```cpp
  class PeriodManager {
  public:
      Result<Period> open_period(const std::string& source_session_id);
      Result<void> attach_session(const std::string& period_id,
                                   const std::string& session_id);
      Result<void> close_period(const std::string& period_id);
      Result<void> append_summary(const std::string& period_id,
                                   const std::string& summary);
      Result<std::vector<Period>> list_open_periods() const;
      Result<Period> find(const std::string& period_id) const;
  };
  ```

### SessionStore 关联
- `jsonl_session_store`：每条 session metadata 新增 `period_id` 字段
- `create_session` 接口改为可选接收 `period_id`（默认空 → 由阶段 3 决定）

### build_engine 集成
- 构造 `PeriodManager` + `JsonlPeriodStore`
- 注入到 SessionEngine

### 测试
- 单元：`period_manager_test.cpp`（开 / 附加 / 关闭 / 列出）
- 单元：`jsonl_period_store_test.cpp`（序列化、并发、重放）
- 集成：开引擎 → create_session → attach → close → 验证持久化

## 七、阶段 3：PeriodProbe + 跨会话（~20h）

**目标**：新会话启动时自动判断"是否属于已有周期"，实现"周期内会话不严格隔离"。

### PeriodProbe 算法
- 新文件 `src/application/period_probe.h/.cpp`
- 算法：
  ```cpp
  struct AttachDecision {
      bool attach{false};
      std::string period_id;           // attach=true 时填写
      double similarity{0.0};
  };

  AttachDecision decide(
      const std::vector<Period>& open_periods,
      const std::string& current_prompt,
      const PeriodConfig& config) const;
  ```
- 对每个 open period：
  1. 取 `period.summary_tokens`
  2. 取当前 prompt tokens（沿用 `MemoryRetriever::tokens()`）
  3. 加权 Jaccard：`weighted_intersection / weighted_union`
  4. IDF 加权用 period 内的 token 频率统计（粗略：跨周期语料统计）
- 决策：取相似度最高的周期；超过 `auto_attach_threshold` 则 attach，否则不 attach

### SessionEngine 改造
- `create_session(prompt)` 流程变为：
  ```
  1. 列出 open periods (PeriodManager)
  2. PeriodProbe.decide(...) → decision
  3. 若 attach：attach_session(period_id, session_id)
  4. 若不 attach：open_period(session_id)，然后 attach
  5. session.period_id 写回
  ```

### 新会话 = 压缩事件（部分实现）
- 在 `create_session` 返回前 emit 一个 `SessionCompressionStartedPayload` 事件
- 含义：用户开新会话本身是一次显式压缩
- **完整语义留给阶段 7**

### 测试
- 单元：`period_probe_test.cpp`（相似度计算、attach 决策）
- 集成：模拟"用户开了 3 场同主题会话 + 1 场不相关"，验证 attach 正确
- 集成：attach 后的 session 摘要能影响下一次 PeriodProbe

## 八、阶段 4：T1 触发器 + 周期摘要生成（~16h）

**目标**：周期内累积到一定主题分歧时自动关闭周期、开始新周期。

### Session 摘要生成
- 新文件 `src/application/session_summarizer.h/.cpp`
- 在每场 session 关闭（不是 turn 结束，是 user `exit`/进程退出）时调用
- 调 LLM 生成 ≤800 字的摘要
- 摘要内容：
  - 主要话题（1~3 句）
  - 关键决策（按 bullet）
  - 提到的实体（项目名、文件名、API 名等）

### 摘要持久化
- 摘要写进 `Period.episodic_summary`
- 同时更新 `Period.summary_tokens`（用 `MemoryRetriever::tokens()` 重算）

### T1 触发器
- 修改 `MemoryMaintenanceScheduler::evaluate()`，新增 T1：
  ```cpp
  if (config.trigger_on_topic_divergence) {
      // 当前 session 的 prompt tokens vs 所属 period 的 summary_tokens
      auto divergence = 1.0 - weighted_jaccard(current, period);
      if (divergence >= config.topic_divergence_threshold) {
          return {true, TopicDivergence, ...};
      }
  }
  ```
- 注意：T1 触发后不只是 consolidate，**还要 close current period + open new period**
- 触发顺序：close old period → consolidate old period → start new period → attach current session to new

### 测试
- 单元：`session_summarizer_test.cpp`（prompt 模板、长度限制）
- 单元：`topic_divergence_test.cpp`（加权 Jaccard 计算）
- 集成：模拟"同主题 5 场 session → 第 6 场完全无关"，验证 T1 触发 + 周期切换

## 九、阶段 5：精挑细选 curate（~14h）

**目标**：把现有的"摘要整段对话"升级为"按类别精挑细选"。

### Prompt 重写
- 文件：`src/application/model_memory_consolidator.cpp`
- 新 prompt 模板：
  ```
  You are curating a session's worth of conversation into structured
  long-term memory. Output JSON with 5 categories. For each category,
  list ONLY items that would be useful to remember 3 months from now.

  Categories:
  - preference: user-stated likes/dislikes/style
  - decision: choices made (with reasoning)
  - fact: verified information worth keeping
  - workflow: repeatable procedure
  - constraint: never-violate rule

  Self-check each item: "Would I want this in 3 months?"
  Deduplicate against existing entries (provided).
  Max 12 entries per session.

  Input (this session): {transcript}
  Existing entries (avoid duplicates): {memory_snapshot}

  Output: <json>
  ```
- 用 Anthropic tool_use 强制结构化输出

### 限额与去重
- `max_entries_per_session = 12`（默认）
- 在 prompt 中附带现有 `MemoryEntry` 列表让模型去重
- Consolidator 在 commit 前再校验：相同 `memory_id` 不重复写

### 测试
- 单元：`curation_prompt_test.cpp`（prompt 模板渲染、长度合规）
- 单元：`curation_dedup_test.cpp`（重复检测）
- **人工评估集**：`tests/eval/curation_quality/` — 5 个会话样本 + 期望输出，用于回归测试

## 九点五、阶段 5b：REM 抽象与整合（~14h）— 神经科学补丁

**目标**：在 curate commit 之后，对新写入的事实跑一次 refine pass：抽象、链接、矛盾检测、覆盖检测。这是模仿 REM 睡眠阶段的"抽象 + schema 整合"功能。

### 触发时机
- **每个周期关闭时跑一次**（不是每场 session）。
- 与阶段 1 节流窗口复用：节流窗口内不重复跑。
- 失败不阻塞后续 turn，记录到 `runtime_data/refinement_errors.log`。

### 四个 Pass

```cpp
class RefinementPass {
public:
    Result<RefinementReport> refine(
        MemoryState& state,
        const std::vector<MemoryEntry>& newly_committed,  // 阶段 5 的产物
        const RefinementConfig& config,
        ModelClient& model);
    
    struct RefinementReport {
        std::size_t abstracted{0};          // Pass 1 抽象条数
        std::size_t clustered{0};           // Pass 2 链接条数
        std::size_t conflicts_found{0};     // Pass 3 矛盾条数
        std::size_t supersedes_found{0};    // Pass 4 覆盖条数
    };
};
```

#### Pass 1：抽象（多对一）— 精细做法
- 输入：`newly_committed` 中同 `MemoryCategory` 且同 `period_id` 的事实
- 触发：两两 weighted Jaccard ≥ `abstract_threshold` (0.7)
- **精细粒度**：每个连通分量单独调一次 LLM，不批量
- 输出：1 条更抽象的 fact + N 条原 fact 标 `superseded_by`（保留 lineage）
- LLM 约束：禁止引入组外信息；不确定就跳过

#### Pass 1.5：演绎推断（Deduced）— 精细做法
- 触发：每次周期关闭都跑（与 T1/T2/T3 节流复用）
- 输入：cluster 内全部 grounded atomic facts
- **精细粒度**：每个 cluster 单独调 LLM（per cluster，**不** per pair）
- 严格定义（用户决策）：
  - 规则 1 演绎性：必须从子 facts + 基本常识严格推导
  - 规则 2 唯一性：结果不依赖分支选择
  - 规则 3 充分性：所有支撑 facts 全部在场
  - 规则 4 可重构：从父 fact + 推理依据能完整恢复子 facts
- **coefficient 机制**（用户决策）：
  - 推断 fact 的 coefficient = 支撑 facts 系数之和
  - 支撑 facts coefficient 归 0，**永久退出竞争**
  - 支撑 facts **仍保留**（不删除），可被反向推理回溯
  - **cluster_id 行为（用户决策）**：Deduced fact 的 cluster_id = 支撑 facts cluster 的并集
- 输出：
  ```cpp
  struct DeducedFact {
      std::string content;
      std::vector<std::string> deduced_from;
      std::string deduction_rule;
      std::string justification;
  };
  ```

#### Pass 2：实体 + 链接 — 三阶段混合 + 精细粒度
- **精细粒度**：每条 fact 单独调一次 LLM（per fact）
- Stage 1（规则，cheap，先跑）：
  - 大写 ASCII 连续 token
  - 全大写缩写
  - 路径前缀
  - CJK 连续 ≥ 2 字符
  - 版本号
- Stage 2（LLM per fact，精细）：
  - 输入：单条 fact
  - prompt：识别关键实体（包括隐式）
  - 输出：JSON `{entity_name, entity_type, fact_id}`
  - entity_type 限制：Person / Project / Tool / Concept / File / Date / 其他
- Stage 3（实体合并 + 规则后处理）：
  - **实体合并（用户决策）**：用 LLM 判定 Stage 1 + Stage 2 提取的实体是否指同一概念（精细做法，计入 LLM budget）
  - 实体名 ≤ 4 个单词
  - 实体名去重（大小写不敏感 + 别名映射）
  - 来源校验：必须能溯源到至少 1 条 fact
  - 失败 → 丢弃 + 错误日志
- 聚类：同一实体 + 同一 cluster_id；一条 fact 可属多 cluster

#### Pass 3：矛盾检测（多对多，双时间戳）— 精细做法
- **精细粒度**：每对候选单独调 LLM（per pair）
- temperature = 0（确定性，用户决策）
- 输入：同 category + weighted Jaccard ≥ 0.4 的候选对 + 双方 `TemporalContext`
- 输出：
  ```cpp
  {
      is_conflict: bool,
      conflict_type: "value"|"time"|"context",
      resolution_hint: "newer_valid_time_wins"|"longer_valid_time_wins"|"both_valid_different_context"|"uncertain",
      reasoning: "..."
  }
  ```
- **仅标注，不自动解决，不降权**（保守路线）
- 双时间戳字段：
  ```cpp
  struct TemporalContext {
      std::string valid_from_utc;    // 业务有效起始
      std::string valid_until_utc;   // 业务有效截止
      std::string recorded_at_utc;   // 进入系统时间
  };
  ```

#### Pass 4：覆盖检测（一对一）— 精细做法
- 同 category + 同 cluster + 时间差 > 7 天
- LLM 判定"是否完全覆盖"
- 输出：双向 `superseded_by` / `supersedes`

### 持久化
- RefinementReport 写进事件流：`RefinementCompletedPayload`。
- 矛盾 / 覆盖不删除旧条目，**保留 lineage**，方便回溯。

### 测试
- 单元：`refinement_pass_test.cpp`（每个 Pass 独立测试）
- 单元：`abstract_pass_test.cpp`（多对一合并边界）
- 单元：`conflict_detection_test.cpp`（矛盾判定）
- 集成：模拟"周期内 N 场同主题 session → 关闭 → refine → 验证抽象 + 链接 + 矛盾"
- 人工评估：`tests/eval/refinement_quality/` — 5 样本

## 十、阶段 6：倒排索引检索 + 难度档位（~12h）

**目标**：O(1) 关键词查询取代 O(n) 逐条扫描。

### 索引结构
- 新文件：`src/application/inverted_index.h/.cpp`
- `std::unordered_map<std::u32string, std::vector<std::pair<std::string, std::size_t>>>` 
  - key: token
  - value: [(memory_id, frequency), ...]

### 索引构建
- 在 `MemoryState` 加载完成后构建一次（lazy）
- 增量更新：`MemoryUpserted` / `MemoryForgotten` 时同步更新索引
- 持久化：可选。v0.5 不持久化，启动时重建

### `MemoryRetriever` 改造
- 替换逐条扫描：
  ```
  1. tokenize(query) → query_tokens
  2. 对每个 query_token，查 inverted_index → 候选 memory_id 集合
  3. 对每个候选，按现有算法算 score
  4. 排序、过滤 workspace、装填
  ```
- 保留现有测试断言（输入输出兼容）

### `retrieval_difficulty` 排序调整（神经科学补丁）
- 排序时把 difficulty 也考虑进去：
  ```
  effective_score = base_score + (1 - difficulty) * config.top_k_boost_factor * weight
  ```
- 含义：**难度低（已被反复检索）的事实优先进入 TOP-K**——这就是 Bjork 的 retrieval strength。
- **每次检索命中**：`difficulty = max(0, difficulty - per_retrieval_decrement)`。这就是 SWR 双重作用。

### 索引-条目一致性
- 每次 `MemoryUpserted` 事件都更新索引
- 每次 `MemoryForgotten` 都从索引移除
- 测试：构造一组 upsert + forget 序列，验证索引内容

## 十点五、阶段 8：主动遗忘 + 摘要级保留（~14h）— 神经科学补丁

**目标**：阶段 1 节流窗口打开时，除 consolidate 外，再跑一次遗忘 pass —— 衰减 durability、删除超阈值的 detail、把 detail 提炼成 summary 留在 Period 层。

### 设计哲学（**借鉴而非复刻**）
- **默认关闭**（`forgetting.enabled = false`）—— 主动遗忘对用户来说是反直觉特性，必须 opt-in。
- **摘要级保留**：忘掉 detail ≠ 忘掉全貌。被遗忘的 fact 提炼成 ≤100 字摘要，存进所属 `Period.episodic_summary`，留下 tombstone 指针。
- **Category 保护**：`MemoryCategory::Constraint` 永不忘（用户说了"永远别……"，必须保留）。
- **可恢复**：未来用户问"上次那个 RAG 提案"，系统能从 Period 摘要重建上下文。

### `ForgettingPolicy` 服务

```cpp
class ForgettingPolicy {
public:
    struct ForgetReport {
        std::vector<std::string> forgotten_ids;     // 被遗忘的 fact id
        std::vector<std::string> summary_added;     // 新增的 period summary
        std::size_t durability_decayed;             // 仅衰减但保留的 fact 数
    };

    ForgetReport apply(
        MemoryState& state,
        PeriodManager& periods,
        const ForgettingConfig& config,
        Clock& clock,
        ModelClient& model);  // 用于生成摘要
};
```

### 算法

```
for each active entry:
  if entry.category == Constraint and keep_category_always:
    continue   # 永不忘
    
  age = now - entry.last_accessed_at_utc_ms
  retrieval_loss = exp(-age / config.retention_half_life)
  current_durability = entry.durability_score * retrieval_loss
  
  if current_durability < config.forget_threshold:
    # 提炼摘要
    summary = model.summarize(entry.content, max_chars=100)
    periods.append_to_episodic_summary(entry.source_period_id, summary)
    
    # 标记 tombstone
    tombstone = MemoryEntry {
      content = f"[Forgotten] {summary}",
      tombstone_of = entry.memory_id,
      durability_score = 0,
      retrieval_difficulty = 0,
      category = entry.category,
    }
    state.active_entries[entry.memory_id + "-tombstone"] = tombstone
    
    # 删除原条目
    state.active_entries.erase(entry.memory_id)
    forgotten_ids.push_back(entry.memory_id)
  else:
    entry.durability_score = current_durability
    durability_decayed += 1
```

### 触发时机
- **复用阶段 1 的节流窗口**：evaluate 决策 fire 时，consolidate + refine + forget 三件套可以并行。
- **后台执行**：`schedule_async` 启动 `std::thread`，等所有任务完成再 join。
- **失败容错**：摘要生成失败 → 保留 detail，标 `at_risk: true`，下周期重试。

### 测试
- 单元：`forgetting_policy_test.cpp`（衰减曲线、阈值触发、Constraint 保护）
- 单元：`summary_retention_test.cpp`（摘要生成 + tombstone 创建）
- 单元：`durability_decay_test.cpp`（注入假时钟，验证 7 天后衰减）
- 集成：`tests/integration/forgetting_flow_test.cpp`（完整周期：consolidate → refine → forget → 用户查询 → 摘要回溯）

### CLI 加 `/memory-decay-status`
- 列出当前所有 fact 的 durability、最后一次访问、距遗忘阈值剩余时间
- 列出所有 tombstone + 对应 period summary 引用

## 十一、阶段 7：显式压缩语义（~6h）

**目标**：把"开新会话=压缩事件"做完整，给用户/SDK 一个显式的 API。

### 新 API
- `Engine::compress_session(session_id)`：
  1. 触发当前 session 的摘要生成
  2. 不等 consolidate，立即把摘要写进所属 period
  3. 返回压缩结果（新增的 memory entries）

### CLI 命令
- `/compress` —— 显式压缩当前 session（不等周期结束）
- `/period-status` —— 显示当前 period 状态（session 数、token 累计、已沉淀 facts）
- `/period-close` —— 显式关闭当前 period，强制触发 consolidate

### 测试
- 单元：`compress_session_test.cpp`
- 集成：`/period-status` 命令行验证

## 十二、阶段依赖图

```text
阶段 1 (v0.5)         触发器
    │
    ├─→ 阶段 2         Period 数据结构
    │       │
    │       ├─→ 阶段 3 PeriodProbe + 跨会话
    │       │       │
    │       │       └─→ 阶段 7 显式压缩 API
    │       │
    │       └─→ 阶段 4 T1 + 摘要生成
    │
    ├─→ 阶段 5 curate prompt（带 pattern separation + 矛盾检测）
    │       │
    │       └─→ 阶段 5b REM 抽象与整合（神经科学补丁）
    │
    ├─→ 阶段 6 倒排索引 + 难度档位（神经科学补丁）
    │
    └─→ 阶段 8 主动遗忘 + 摘要级保留（神经科学补丁，依赖 5b 和 6）
```

每个阶段完成后都有可演示的产物：

| 阶段 | 可演示内容 |
|---|---|
| 1 | 配置开关 + 多触发器 + 节流 + 兜底 |
| 2 | Period 持久化 + 管理 |
| 3 | 新会话自动 attach 到相关 period |
| 4 | T1 触发器 + session 摘要池 |
| 5 | curate 质量提升（pattern separation + 矛盾 + 覆盖检测） |
| 5b | REM 抽象 + 链接 + 跨周期矛盾检测 |
| 6 | 检索 O(1) + 难度档位 + SWR 反哺 |
| 7 | `/compress` 命令 |
| 8 | `/memory-decay-status` + 摘要级回溯 |

## 十三、完整文件改动清单（汇总）

| 阶段 | 新增文件 | 修改文件 | 估时 |
|---|---|---|---|
| 1 | `tests/application/consolidation_trigger_test.cpp` | `memory_policy.h`、`memory_state.h`、`memory_event_json.cpp`、`memory_maintenance_scheduler.{h,cpp}`、`engine.{h,cpp}`、`session_engine.{h,cpp}`、`runtime_engine.cpp` | 12h |
| 2 | `domain/period_state.h`、`ports/period_store.h`、`adapters/persistence/jsonl_period_store.{h,cpp}`、`application/period_manager.{h,cpp}`、`tests/application/period_manager_test.cpp`、`tests/application/jsonl_period_store_test.cpp` | `session_state.h`、`jsonl_session_store.cpp`、`memory_event_json.cpp`、`engine.{h,cpp}` | 16h |
| 3 | `application/period_probe.{h,cpp}`、`tests/application/period_probe_test.cpp` | `session_engine.{h,cpp}`、`engine.cpp` | 20h |
| 4 | `application/session_summarizer.{h,cpp}`、`tests/application/session_summarizer_test.cpp`、`tests/application/topic_divergence_test.cpp` | `memory_maintenance_scheduler.cpp`、`period_manager.{h,cpp}` | 16h |
| 5 | `tests/application/curation_prompt_test.cpp`、`tests/application/curation_dedup_test.cpp`、`tests/eval/curation_quality/` | `model_memory_consolidator.cpp`、`memory_policy.h`、`memory_state.h` | 16h |
| **5b**（神经科学）| `application/refinement_pass.{h,cpp}`、`tests/application/refinement_pass_test.cpp`、`tests/application/abstract_pass_test.cpp`、`tests/application/conflict_detection_test.cpp`、`tests/eval/refinement_quality/` | `memory_maintenance_scheduler.cpp`、`model_memory_consolidator.cpp` | **14h** |
| 6 | `application/inverted_index.{h,cpp}`、`tests/application/inverted_index_test.cpp` | `memory_retriever.{h,cpp}`、`memory_state.h` | 12h |
| 7 | `tests/application/compress_session_test.cpp`、`cli/commands/period_commands.{h,cpp}` | `engine.h`、`interactive_cli.cpp` | 6h |
| **8**（神经科学）| `application/forgetting_policy.{h,cpp}`、`tests/application/forgetting_policy_test.cpp`、`tests/application/summary_retention_test.cpp`、`tests/application/durability_decay_test.cpp`、`cli/commands/memory_decay_command.{h,cpp}`、`tests/integration/forgetting_flow_test.cpp` | `memory_maintenance_scheduler.cpp`、`memory_policy.h`、`period_state.h` | **14h** |
| **合计** | ~32 个新文件 | ~17 个修改 | **~126h** |

## 十四、测试策略

### 单元测试（每阶段必做）
- 新数据结构：构造 + 序列化 + 边界值
- 新算法：典型输入 + 边界 case（空集、单元素、极大）
- 配置解析：默认值 + 显式值 + 非法值

### 集成测试（阶段 3+ 必做）
- **场景 A**：用户开 3 场同主题会话 → 验证全部 attach 到一个 period → 关闭 → 验证只有一次 consolidate
- **场景 B**：同主题 2 场 + 不相关 1 场 → 验证第一二场 attach 到 period A，第三场 attach 到 period B
- **场景 C**：长会话 token 累计 → 验证 T2 触发 consolidate
- **场景 D**：闲置 24h → 验证兜底触发
- **场景 E**：`long_term_memory=false` → 验证无 consolidate、无跨会话

### 人工评估（阶段 5 必做）
- `tests/eval/curation_quality/`：5~10 个会话样本 + 期望 curate 输出
- 评估指标：
  - 召回率：期望被保留的 fact 是否被保留
  - 精度：保留的 fact 是否真的有用（人评）
  - 去重：相同 fact 是否被合并

### 性能测试（阶段 6 必做）
- 1000 条记忆下：词袋 vs 倒排索引延迟对比
- 10000 条记忆下：同上
- 索引构建时间（冷启动）

### 回归测试
- 所有现有 `tests/application/*` 测试在新代码下不爆
- 现有 `tests/cli/*` 视觉断言保持不变

## 十五、默认参数一览

| 参数 | 默认值 | 出处 |
|---|---|---|
| `long_term_memory` | true | 主开关 |
| `token_threshold` (T2) | 50000 | 阶段 1 |
| `time_elapsed` (T3) | 24h | 阶段 1 |
| `rate_limit` | 24h | 阶段 1 |
| `force_periodic_floor` | true | 阶段 1 |
| `topic_divergence_threshold` (T1) | 0.7 | 阶段 4 |
| `auto_attach_threshold` | 0.3 | 阶段 3 |
| `summary_max_chars` | 800 | 阶段 4 |
| `max_sessions_per_period` | 50 | 阶段 2 |
| `inverted_index_min_token_len` | 2 | 阶段 6 |
| `exact_phrase_bonus` | 100 | 阶段 6（兼容旧） |
| `max_entries_per_session` | 12 | 阶段 5 |
| `self_check_horizon_months` | 3 | 阶段 5 |

## 十六、风险与权衡

| 风险 | 应对 |
|---|---|
| 阶段 3 PeriodProbe 误判（同主题低重合） | 用户可手动 `/attach --period=<id>` 强制 attach；CLI 提供 `/period-status` 查看决策日志 |
| 阶段 4 session 摘要在长 session 下变贵 | `summary_max_chars` 限制 + 用便宜模型（Haiku 级） |
| 阶段 5 curate 质量取决于 LLM | 阶段 5 自带人工评估集，回归用 |
| 阶段 6 倒排索引内存占用 | 1000 条记忆下约几 MB，可接受；万级以上引入磁盘索引（v3+） |
| 阶段 7 显式压缩需要新 CLI 命令 | 加 `/compress`、`/period-status`、`/period-close` 三个命令 |
| 跨进程一致性 | 所有持久化走 JSONL + SHA-256 校验，与现有模式一致 |
| 周期切换时序（T1 + close period + open new） | 事务化：先写"close 事件"再写"open 事件"，按序应用；失败回滚到上一稳定状态 |

## 十七、不在本计划内的工作

- ❌ **RAG 子系统改动**（按你之前讨论的"RAG 进核心"提案，留到独立子项目）
- ❌ **跨项目 / 跨设备同步**（Cloud memory，参考 MemoryPlugin 的做法）
- ❌ **向量检索**（用现有词袋 + IDF 编码已够，未来看效果再决定）
- ❌ **热度衰减**（last_accessed_at_utc 字段已留，算法 v3+ 再设计）

## 十八、给 review 的建议

1. **先 review 第三节数据模型 + 第四节配置 Schema** —— 这是最大的新字段集合
2. **第十二节依赖图** —— 看每个阶段的产物能否独立 ship
3. **第十三节文件改动清单 + 第十五节默认参数** —— 估工作量和风险
4. **第十六节风险表** —— 看你接受哪些权衡
5. **如果只允许先做某 1~2 个阶段，你会选哪些**？这是给我做迭代顺序用的

> 等你回这五个问题 + 哪几个阶段先做，我就开 task list，按你选的顺序逐个推进。