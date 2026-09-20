# 长期记忆检索：现状讨论与改进方向

> 状态：草案 v0.1，讨论中。下面写的"问题"和"改进"不一定都对，需要你校准。
> 锚点代码：`src/application/memory_retriever.{h,cpp}`、`src/domain/memory_state.h`、`tests/application/memory_retriever_test.cpp`。

## 一、现状摘要（先对齐事实）

`MemoryRetriever::retrieve(state, workspace, query, top_k, injected_byte_budget)` 走的是 **词面 token 重合 + 时间倒序**：

1. **分词**（`memory_retriever.cpp:89` 的 `tokens()`）
   - UTF-8 解码后，逐字符判断：
     - ASCII 字母/数字 → 累积成一个小写单词，遇到非字母数字就 flush（连续大块字母视作一个词）。
     - CJK 区间 → 同时记成单个 unicode 码点 **和** 与下一个 CJK 码点组成的二元组（大五）。
     - 其他非空白非标点 unicode 字符 → 当成单字符 token。
   - 返回 `std::set<std::u32string>`，集合语义自带去重。

2. **打分**（`:180` 附近）
   - 对每条 `active_entry`：
     - 跳过 `scope_utf8` 非空且不等于当前 workspace 的条目（全局或本工作区二选一）。
     - `overlap = |entry_tokens ∩ query_tokens|`，**零重合直接出局**。
     - `exact_phrase`：query 的小写形式在 entry content 的小写形式中作为子串出现 → +100。
     - `score = overlap + (exact_phrase ? 100 : 0)`。
   - "forgotten" 条目在 `replay_memory_events` 后就不在 `active_entries` 里了，检索层不另处理。

3. **排序**（`:196`）
   - 优先级：`score 倒序` → `normalized_updated_at_utc 倒序` → `memory_id 字典序`。
   - 三级排序保证结果稳定可重放（这也是 `memory_retriever_test.cpp:74` 那种 "release" 查询两个 id 谁的字典序在前就有意义的根因）。

4. **装填**（`:204`）
   - 按排序结果贪心取，遇到 `selected.size() == top_k` 或 `candidate.rendered.size() + 分隔符 > injected_byte_budget - bytes` 就停。
   - 渲染格式固定为 `"- [<id>] (<category>) <content>"`，分隔符是换行符。

5. **错误路径**
   - 任何一段 UTF-8 解码失败 → `Result::failure(InvalidInput, …)`。
   - 没有"找不到任何匹配"和"全部塞不下"的区分：前者就是返回空数组。

## 二、这套机制让我觉得"不够稳"的地方（不一定对）

按严重程度从高到低排，都是观察，**欢迎反驳**：

1. **没有 IDF / 长度归一化**。
   - 长条目天然 token 多，重合概率高；很常见的 token（"build"、"test"、"使用"）和稀有 token 权重相同。
   - 一个 2KB 的"工作流规范"很容易压过一个 200 字节的精准事实。

2. **没有语义相似度**。
   - 词袋不认同义、近义、跨语言。比如 "preferences" vs "设置偏好"、"不要" vs "禁止" 是命中不到的。
   - 仓库里已经有 RAG 子系统（BGE-M3 + vectors.f16），但记忆检索完全没用它。

3. **时间倒序当 tiebreaker 是把双刃剑**。
   - 它的好处：新近被 consolidate 刷新过的偏好会更靠前。
   - 它的代价：每次后台 consolidate 批量 touch 一批条目的 `updated_at` 后，**看似更新鲜、其实只是被同步刷了一遍**的条目会突然排到前面。

4. **类别没有任何优先级**。
   - `Constraint` / `Preference` 这类"无论 query 是什么都该出现"的条目没有 hook 能强制入选；
   - 它们的命运完全取决于和 query 的 token 重合度。

5. **`scope_utf8` 是精确字符串匹配的工作区路径**。
   - 父目录、子目录、软链、`./E:/work` vs `E:/work` 之类的小差异都会让条目被整体跳过。
   - 也意味着"按项目分组"很难 —— 你要么全要，要么全丢。

6. **exact_phrase 加 100 是魔法数**。
   - 没有文档解释为什么 100 "够大到压过合理重合"，也意味着改成 BM25 之类的连续分数后会失去这条 boost，需要重新校准。

7. **CJK 只有 bigram，并且只在相邻时才记 bigram**。
   - query 里的 "中文回答"（相距两字符）是否能跟 entry 里的 "中文回答" 形成 bigram 重合？需要再细看 —— 我读下来当前实现只把"相邻一对 CJK"作为一个 token，**query 和 entry 两端的切分是否一致没看到保证**，可能存在"两边切出来的 bigram 集不同"的隐患。

8. **没有"强制保留"或"强制屏蔽"通道**。
   - `MemoryPolicy` 只在写入时校验字节数和敏感串；检索时不消费 policy。
   - 用户写了"别再建议 X"的记忆，下次 query 含 X 时照样可能被召回。

9. **检索是同步阻塞的，没有缓存**。
   - 同一会话每轮都重算；测试断言稳定序就是隐式依赖"重算也是同样结果"，但任何引入随机或时间相关的因素都会爆。

## 三、候选改进方向（按落地成本排序）

> 这些是"如果改，可以往这个方向走"的清单，不是承诺。具体怎么改还要看你的优先级。

### A. 评分换 BM25 / TF-IDF（成本：中）
- 把 `overlap` 替换成经典 BM25：TF 饱和、IDF 权重、文档长度归一化。
- 不引入新依赖，纯 C++ 重写，几十万条记忆规模下足够。
- 配套要：往 `MemoryState` 加 corpus 级 IDF 统计（增量维护），把 `MemoryPolicy` 扩展出一组"必含类别"配置。

### B. 强制 / 屏蔽通道（成本：低）
- 在 `MemoryEntry` 或 `MemoryState` 上加 `must_include` / `must_exclude` 标记位（或借 `MemoryCategory::Constraint` + `Preference` 当隐式信号）。
- 检索阶段：先按现有规则挑 `top_k` 候选，再保证"所有 must_include 进入渲染队列，即使会顶掉字节预算"；"所有 must_exclude 永远不进入候选"。
- 写入端保留 `MemoryPolicy::protected_values` 做敏感串，但检索端要新增"屏蔽词"配置。

### C. 复用 RAG 通道做语义召回（成本：高）
- 把现有 RAG sidecar 也作为长期记忆的"向量索引"后端：记忆 consolidate 时同步写一份 embedding，retrieve 时先做向量召回，再做词面过滤。
- 需要：embedding 模型版本对齐、知识包 schema 扩展、写入路径分叉、跨进程一致性。
- 收益最大，但要动 RAG 子系统，工作量明显上升。

### D. 类别感知排序（成本：中）
- 给每个 `MemoryCategory` 配一个静态先验权重，例如 `Constraint > Preference > Fact/Decision > Workflow`。
- 或者更细：在同一 `score` 区间内，用类别权重 + 时间衰减的线性组合破平。
- 不需要新依赖，影响面小。

### E. 时间衰减（成本：低）
- `updated_at` 不直接做比较，而是 `decay(updated_at, half_life_days)` → 一个 0~1 的新鲜度因子。
- 注意：会和当前的 tiebreaker 语义冲突，要重新写测试。

### F. 工作区分层（成本：中）
- 把 `scope_utf8` 改成 "作用域列表 + 匹配模式"（精确 / 前缀 / 正则）。
- 或者引入第二层 `session_id` 维度：单会话内临时记忆 vs 跨会话持久记忆。
- 会改 `MemoryEntry` 字段，是 breaking change，需要 schema 迁移。

### G. 缓存层（成本：中）
- 给 `MemoryRetriever` 包一层 LRU，key = `(workspace_utf8, query, top_k, byte_budget, state.last_sequence)`。
- 注意：状态流式更新下，缓存失效逻辑要小心设计。

### H. 重新审视 exact_phrase 的 +100
- 如果切到 BM25，phrase 命中可以转成"在 entry 中找到 query 完整子串 → 乘以一个常数或加一个额外 boost"。
- 也可以引入 n-gram 命中（不只 unigram）作为连续特征。

## 四、开放问题（请你定夺）

1. **改进的优先级**：上面 A~H 你心里最想先动哪条？还是先都不动，等某个具体痛点出现再说？
2. **是否接受 breaking change**：例如 `MemoryEntry` 改 schema、`retrieve` 签名变。
3. **是否复用 RAG 子系统**：长期记忆是不是要走"和领域知识检索同一套 embedding 后端"的路线？这决定 C 是不是主线。
4. **"用户说别建议 X"这条记忆**：你期望它在未来 query 含 X 时完全不出现（must_exclude），还是只是相关性降低？
5. **CJK 分词粒度**：现在是 unicode bigram，要不要切到 jieba / 句子级？规模上来后有没有分词质量瓶颈？
6. **时间衰减的半衰期**：你心里有没有一个直觉值？比如 30 天 / 90 天？

## 五、接下来怎么推进

我建议下一步这样：

- **第一步**：你逐条回上面六个开放问题，把范围收一收。
- **第二步**：选定 1~2 个方向后，我先在 `docs/` 里开一份"改进方案 v0.1"，把数据流、字段变更、测试改造点都写清楚再动代码。
- **第三步**：代码改动以小步 PR 形式推进，每个 PR 自带测试用例，对照 `tests/application/memory_retriever_test.cpp` 现有断言评估兼容性。

> 等你给反馈。