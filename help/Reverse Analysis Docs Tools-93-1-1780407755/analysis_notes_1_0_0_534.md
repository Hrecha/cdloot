# Crimson Desert 拾取动画分析记录

> **本文档基于游戏版本 1.0.0.534，所有地址均为该版本。**
> IDB路径: `F:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe.i64`

## 目标

自动拾取触发 Take 动作时，角色会播放弯腰拾取动画，打断当前动作（如攻击）。目标是让自动拾取不打断当前动作。

## 核心架构

### 客户端-服务端双消息流程（动态调试确认）

Take 拾取需要客户端发送**两条消息**，服务端收到后才处理物品入包：

| 消息                | 类型ID         | 发送函数          | 作用     |
| ----------------- | ------------ | ------------- | ------ |
| InteractionDoStep | 0x9E0 (2528) | sub_1496EE1D0 | 注册拾取意图 |
| FrameEvent确认      | 0xABF (2751) | sub_1496EFD80 | 确认拾取执行 |

**发送顺序**: 9E0 先发 → ABF 后发（动态调试多次确认，稳定复现）
**两条消息缺一不可**（NOP 任一都导致拾取失败）

### Take 执行流程（动态调试确认）

```
sub_140B5D330 (X键回调, 534版)
  → sub_140875D30 (Take 总控)
    → sub_140784C40 (Take handler) @ 0x1408762E6
      ① sub_141A16B20 @ 0x140784E38  ← 写状态机 [r13+178h] + 启动动画
      ② sub_14093BD20 @ 0x140784EA5  ← 发送 9E0
    ← Take handler 返回
  ← Take 总控后续处理

(同帧稍后，独立调用链):
  sub_140887610 → sub_1407869B0 → ActionChart → 发送 ABF
```

### 状态机 `[r13+178h]`

- 写入由 `sub_141B537F0` 执行
- **写入是一次性触发器**：一旦 ActionChart 处理了状态变化，动画系统内部已缓存/复制该状态，后续恢复 [r13+178h] 无法阻止动画

## 关键函数地址表

| 函数               | 地址                            | 角色                                                 |
| ---------------- | ----------------------------- | -------------------------------------------------- |
| X键回调 (534版)      | sub_140B5D330                 | AOB `33 ?? 4C 39 ?? 48 01 00 00 0F 85` @ func+0x15 |
| **Take 总控**      | sub_140875D30                 | 调用 Take handler                                    |
| **Take handler** | sub_140784C40                 | ActionChart + 发送 9E0                               |
| ActionChart 执行   | sub_141A16B20                 | 包装器: 初始化→验证→准备→执行→清理                               |
| **状态机驱动**        | sub_141B537F0                 | 写入 [r13+178h]                                      |
| ActionChart 项处理器 | sub_141D496F0                 | type0=动画, type1=嵌套, type2=FrameEvent               |
| **9E0 发送**       | sub_14093BD20 → sub_1496EE1D0 | InteractionDoStep                                  |
| **ABF 发送**       | sub_14093BF00 → sub_1496EFD80 | FrameEvent确认                                       |
| 服务端 Take handler | sub_142200D00                 | 处理拾取，调用物品入包 (1283 bytes)                           |
| 服务端事件循环          | sub_141FB2710                 | 服务端交互线程事件循环 (465 bytes)                            |
| 服务端交互分发器         | sub_141FB3750                 | 验证+分发 (710 bytes)                                  |
| 服务端 vtable 调度    | sub_141FB3A20                 | vtable+16 间接调用 (56 bytes)                          |
| 实体查找             | sub_1421DEF80                 | 在 qword_145C9C128 查找实体 by ID                       |
| 交互 UI 设置         | sub_140B5E180                 | 2589 bytes, 含 hook 点 AOB                           |

## RTTI 类名

| 类名                                      | 字符串地址       |
| --------------------------------------- | ----------- |
| ClientFrameEventPickUpItem              | 0x1459BEB40 |
| ServerFrameEventPickUpItem              | 0x145AE4180 |
| ClientFrameEventEaseAnimationSpeed      | 0x1459BD1C0 |
| ClientFrameEventTimeScale               | 0x1459BE208 |
| CommonChangeAnimationSpeedBuffProcessor | 0x1459D1CD0 |
| hkaSplineCompressedAnimation            | 0x1459B3F60 |

注: RTTI COL/vtable 在 .rsrc 段中，IDA 未加载。

## 已排除的方案（摘要）

以下方案在 Take 动画消除探索中被排除，结论对添加新动作类型仍有参考价值：

| 方案                       | 结论                                        |
| ------------------------ | ----------------------------------------- |
| NOP 状态机 [r13+178h] 写入    | 动画和拾取都不发生，写入是触发器不可跳过                      |
| Save/Restore [r13+178h]  | 拾取成功但动画仍播放——动画是一次性副作用，恢复状态为时已晚            |
| 直接构造 9E0+ABF 数据包注入       | 数据完全正确，但 **Denuvo dispatch 层过滤**了非正常流程的消息 |
| NOP ActionChart / 动画相关调用 | 动画和拾取紧耦合，无法只去掉动画                          |

**关键结论**: 网络消息路径被 Denuvo 保护，不可注入；客户端 ActionChart 动画不可分离。因此采用**服务端内部事件入队**路径绕过两者。

### 消息处理架构（已确认）

```
┌─ 游戏内部事件路径（★ 我们使用这条）─────────────────────┐
│ sub_141FB2710 → sub_141FB3750 → sub_141FB3A20          │
│ 处理 d3=0x144 格式的内部事件                             │
│ 不经过 Denuvo，可以自由入队                              │
└────────────────────────────────────────────────────────┘

┌─ 网络消息路径（被 Denuvo 保护，不可注入）──────────────────┐
│ sub_1402AF070 → 入队 *(session+0xE0)                    │
│   → [Denuvo 加密 dispatch] ← 过滤非法消息                │
│     → sub_151509760 → sub_142200D00                     │
└────────────────────────────────────────────────────────┘
```

## ★★★ 方案5：服务端事件入队 ★★★

### 核心发现

网络消息 Take 路径之外，存在一条**完全独立的服务端事件入队路径**，而且它**已被正常 Take 使用**：

```
vtable dispatch (sub_141FB3A20)
  → sub_150C70F40 (处理器)
    → sub_1421FE010 (服务端交互总调度, switch a4)
      → case 0: sub_142200B80 → sub_1511B6340
        → 优先: sub_141016960 分配事件 → sub_141FB3620 入队 → 服务端线程处理
        → fallback(sub_141016960返回NULL): dword_145C1A614非零时直接调 sub_142200D00
```

### 优势

- 完全绕过客户端 ActionChart → **无弯腰动画**
- 完全绕过 Denuvo 网络消息验证 → **不受 dispatch 层过滤**
- 由服务端线程处理 → **正确的 TLS 上下文**，无线程安全问题
- 游戏内部已有此路径的使用 → 不是 hack，是复用内部机制

### 完整的函数调用链

```
sub_142281CC0 / sub_142297D00 / sub_150C70F40  (调用者，含 vtable dispatch)
  └→ sub_1421FE010 (服务端交互总调度, switch on a4)
      ├─ case 0 (Take): sub_142200B80 (thunk) → sub_1511B6340 ★
      ├─ case 1,4: 直接在 sub_1421FE010 内事件入队
      ├─ case 2: sub_142202800
      ├─ case 3: 直接在 sub_1421FE010 内事件入队（不同常量）
      └─ case 5: sub_1422D81E0

sub_1511B6340 (Take 事件入队函数, 4个参数, 413字节):
  ├→ sub_1410189E0(registry, 0x806, mask) → 查找 descriptor
  ├→ sub_141016960(?, 13) → 分配128字节事件
  ├→ 填充事件: player_id, item_data, descriptor
  ├→ sub_141FB3620(queue, event, descriptor) → 入队 ★
  └→ fallback: dword_145C1A614 非零时直接调 sub_142200D00
```

> **添加新动作类型参考**: `sub_1421FE010` 的 switch 结构是服务端交互总调度，每个 case 对应一种交互类型。添加 Catch 等新类型时，需在此函数中找到对应的 case 分支，分析其事件入队函数（类似 sub_1511B6340），获取消息类型、事件类型和 buffer 格式。

### 事件结构（128字节，由 sub_141016960 分配）

```
偏移    大小    值                    说明
+0x30   4    1                       action_type = 1
+0x40~48     0                       清零
+0x50   4    player_entity_id        ★ 玩家 entity ID
+0x54   4    0                       清零
+0x58   4    player_entity_id        ★ routing ID = player ID
+0x60   8    descriptor              ★ 动作描述符指针
+0x68   2    event_type              ★ 事件类型 (Search=7, Take=0xD)
+0x70   8    buffer_ptr              (由 sub_141016810 自动分配)
+0x78   1    1                       标志
```

### Take 事件缓冲区（13字节）

```
偏移    大小    值              说明
+0x00   4    0x00FF0806       常量
+0x04   4    item_entity_id   ★ 物品 entity ID = handler+0x180 (唯一变化字段)
+0x08   4    0xFF000101       常量
+0x0C   1    0x00             常量
```

### 分发器处理流程（sub_141FB3750）

```
1. 读取 action_descriptor = event+0x60
2. 读取 entity_id = event+0x50
3. 如果 entity_id != 0:
   → sub_141FB4CA0(..., &event+0x50, descriptor+22)  // 自动查找实体
4. 调用 sub_141FB3A20(&result, descriptor)  // vtable 调度到实际处理器
```

### 线程路由机制（sub_141FB3620 → sub_141017C70）

```
thread_selector = *(DWORD*)(descriptor + 28)
- 0: 当前线程直接处理（危险，TLS 不匹配）
- 1: 入队到服务端线程（安全，正确 TLS）★ 必须=1

路由键 = event+0x58 (player_routing_id)
- (routing_id >> 28) & 3: 选择子队列
- routing_id & 0xFFFFF: 哈希索引
```

## ★★★ 已实现：Auto Loot (Server Event Queue - Relocated Hook) ★★★

**CT 文件**: `G:\Claude\reverse\CrimsonDesert_AutoLoot_v5.0.CT`（含价格过滤 + 近距离自动拾取）
**旧版本**: `v4.4.CT` / `v4.3.CT` / `v4.2.CT` / `v4.1.CT` / `v4.CT` / `v3.CT`

**效果**: Search（搜索尸体）和 Take（拾取物品）均通过服务端事件入队实现，无动画，不打断动作。

**实现方式**: 在 type-byte 加载点（`movzx ebx,[rax+66h]; cmp bl,2`）hook，检测 TypeID 后分配对应的服务端事件并入队。

**v5 vs v4 变更**: Hook 点从 timer store 区域（`test bl,bl; jnz; movss...`，0x140B5E9B9）迁移到上方 ~0x76 字节处的 type-byte 加载点（0x140B5E943），**timer 区域完全不被修改**，避免与其他 mod 的 hook 冲突。

### Search 事件（搜索尸体，TypeID=1）

- **构造函数参考**: `sub_14221E940`
- **消息类型**: 0x07E7 (2023)
- **事件类型**: 7（`sub_141016960(?, 7)`）
- **Buffer**: 7 字节 = `E7 07 FF` + 尸体 entity ID (4字节, handler+0x180)
- **服务端处理**: `sub_14233F7F0`（processLootingDrop 等价物）→ `sub_1422B33A0` → `sub_1422B4460`

### Take 事件（拾取物品，TypeID=4）

- **构造函数参考**: `sub_1511B6340`
- **消息类型**: 0x0806 (2054)
- **事件类型**: 0xD (13)（`sub_141016960(?, 0xD)`）
- **Buffer**: 13 字节 = `06 08 FF 00` + 物品 entity ID (4字节, handler+0x180) + `01 01 00 FF 00`
- **服务端处理**: `sub_150C70F40` → `sub_1421FE010` case 0 → `sub_142200D00`

### 添加新动作类型的步骤

1. 在 hook 点用 `word([r15+r12+10])` 读取新交互的 **TypeID**
2. 在 `sub_1421FE010` 的 switch 中找到对应的 **case 分支**
3. 追踪该分支的事件入队函数，记录 **消息类型**（如 0x0806）和 **事件类型**（如 0xD）
4. 在入队函数中抓取 **buffer 格式**（用 x64dbg 日志断点在 sub_141FB3620 对比）
5. 在 code cave 中添加新的 TypeID 分支，复用共享基础设施

### 共享基础设施

两种事件使用相同的入队机制：

```
sub_141018980()                         // TLS 初始化
sub_1410189E0(?, msg_type, mask)        // descriptor 查找
sub_141016960(?, event_type)            // 事件分配
sub_141FB3620(queue, event, descriptor) // 入队
```

### AOB 特征码

| 函数                            | AOB                                                                                 | 说明                                                                |
| ----------------------------- | ----------------------------------------------------------------------------------- | ----------------------------------------------------------------- |
| Hook 点 (type-byte load)       | `0F B6 58 66 80 FB 02 0F 84`                                                        | `movzx ebx,[rax+66h]; cmp bl,2; jz` — timer 区域之前，不与其他 mod 冲突      |
| Merge 点 (after timer)         | `49 8B B6 D0 00 00 00 48 8B 86 D0 00 00 00`                                         | `mov rsi,[r14+D0h]; mov rax,[rsi+D0h]` — auto-loot 跳过 timer 后的合流点 |
| ~~Hook 点 (timer store, v4旧)~~ | ~~`F3 0F 11 ?? C8 01 00 00 EB ?? F3 0F 10 05 ?? ?? ?? ?? F3 0F 11 ?? C8 01 00 00`~~ | ~~v4 使用，已弃用，与其他 mod 冲突~~                                          |
| sub_141018980 (TLS init)      | `48 83 EC 28 BA 9C 00 00 00 65 48 8B 04 25 58 00 00 00`                             |                                                                   |
| sub_1410189E0 (desc lookup)   | `56 45 33 C9 44 0F B7 D2`                                                           |                                                                   |
| sub_141016960 (event alloc)   | `48 89 5C 24 ?? 4C 89 44 24 ?? 57 48 83 EC 20 8B FA BA 04 02 00 00`                 |                                                                   |
| sub_141FB3620 (event enqueue) | `48 89 5C 24 ?? 44 88 4C 24 ?? 57 48 83 EC 20 48 8B 5A 38`                          |                                                                   |

### 全局变量

| 变量              | 偏移 (base+) | 用途                    | 查找方式                                                 |
| --------------- | ---------- | --------------------- | ---------------------------------------------------- |
| qword_145C9BDA0 | +0x5C9BDA0 | 世界管理器 -> 玩家 entity ID | 搜索 RTTI `WorldManager` 或从 sub_14221E940 的引用链追踪       |
| qword_145C9C110 | +0x5C9C110 | 服务端事件队列               | sub_1511B6340 / sub_14221E940 中 `mov r14, cs:[addr]` |
| dword_14597FB2C | +0x597FB2C | descriptor 查找 mask    | sub_1410189E0 中 `mov r8d, cs:[addr]`                 |
| qword_145C9C560 | +0x5C9C560 | ItemInfoManager       | 从 sub_1402D53B0 中 `mov rbx, cs:[addr]`               |

### 全局变量动态 AOB 解析方案（v4 CT 已实现）

**目标**: 不再硬编码全局变量偏移，通过 AOB 扫描 + RIP-relative 位移计算动态定位。

#### AT_WORLD (WorldManager singleton)

- **锚定函数**: `sub_1402BCA10`（30 字节小函数，唯一匹配）
- **AOB**: `48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 8B 49 ?? E8 ?? ?? ?? ?? 84 C0 0F 94 C0 48 83 C4 28 C3`
- **提取**: offset +4 处 `mov rcx, cs:[rip+disp32]`，disp32 在 offset +7

#### AT_DESC_MASK + AT_QUEUE（从同一函数同时提取）

- **锚定函数**: `sub_141FB47E0`（事件构造辅助函数，唯一匹配）
- **AOB**: `E8 ?? ?? ?? ?? 44 8B 05 ?? ?? ?? ?? 0F B7 54 24 ?? E8 ?? ?? ?? ?? 4C 8B 25`
- **AT_DESC_MASK**: offset +5 处 `mov r8d, cs:[rip+disp32]`，disp32 在 offset +8
- **AT_QUEUE**: offset +22 处 `mov r12, cs:[rip+disp32]`，disp32 在 offset +25

#### AT_ITEM_MGR (ItemInfoManager singleton)

- **锚定函数**: `sub_1402D53B0` (GetItemInfo) 及同模板兄弟函数（~5 个匹配）
- **AOB**: `56 57 41 56 48 83 EC 40 0F B7 39 48 8B 1D ?? ?? ?? ?? 3B 7B 08`
- **提取**: offset +11 处 `mov rbx, cs:[rip+disp32]`，disp32 在 offset +14
- **消歧**: 多个匹配引用不同 manager，运行时读取 `[global]+8`（item_count），**取最大值**的即为主 ItemInfoManager
- **原因**: 同模板类 `StaticInfoManager2<ItemKey, ItemInfo, ItemInfoManager, G>` 生成多个实例，代码结构完全一致，无法静态区分

### 事件结构字段偏移（版本适配参考）

事件对象由 `sub_141016960` 分配（128字节），字段偏移：

```
+30: action_type (DWORD, 固定=1)
+40: cleared (DWORD, =0)
+48: cleared (QWORD, =0)
+50: player_entity_id (DWORD, 从世界管理器链读取)
+54: zero (DWORD, =0)
+58: routing_id (DWORD, 从世界管理器链读取, 通常=player_entity_id)
+60: descriptor (QWORD, sub_1410189E0 返回值)
+68: event_type (WORD, Search=7, Take=0xD)
+70: buffer_ptr (QWORD, 由 sub_141016960 内部分配)
+78: flag (BYTE, =1)
```

玩家信息获取路径：

```
world_mgr = [qword_145C9BDA0]
player_obj = [[world_mgr + 0x30] + 0x20]
player_entity_id = [player_obj + 0x60]   // DWORD, 写入 event+50
routing_id       = [player_obj + 0x58]   // DWORD, 写入 event+58
```

### 版本适配排查指南

游戏更新后若功能失效，按以下优先级排查：

**1. AOB 扫描失败**

- Hook 点 AOB: 搜索 `movzx ebx,[rax+66h]; cmp bl,2` 模式（`0F B6 58 66 80 FB 02`），这是 type-byte 加载点
- Merge 点 AOB: 搜索 `mov rsi,[r14+D0h]; mov rax,[rsi+D0h]` 模式（`49 8B B6 D0 00 00 00 48 8B 86 D0 00 00 00`）
- 函数 AOB: 各函数入口特征可能因编译器优化变化，用 RTTI 字符串定位：
  - `UIGamePlayControlCommon_Interaction` → hook 点所在函数
  - `TrocTrProcessLootingDeadDropOnceTimer` → Search 服务端处理链
  - `TrocTrProcessPickUpItemOnceTimer` → Take 服务端处理链

**2. 全局变量偏移变化**

- 在 sub_1511B6340 等价函数中找 `mov r14, cs:qword_XXXX`（事件队列）
- 在 sub_1410189E0 等价函数中找 `mov r8d, cs:dword_XXXX`（mask）
- 世界管理器：从 sub_14221E940 的玩家信息读取链追踪
- ItemInfoManager：从 sub_1402D53B0 中找 `mov rcx, cs:qword_XXXX`

**3. 事件结构偏移变化**

- 在 sub_1511B6340 / sub_14221E940 中观察事件字段写入的偏移
- 关键模式：`mov dword ptr [rax+30h], 1` (action_type)
- 关键模式：`mov word ptr [rcx+68h], 0Dh` (event_type)

**4. Buffer 常量变化**

- 在 sub_1511B6340 中观察 `LOWORD(v20) = ???` 和 `LOWORD(v21) = ???`
- 在 sub_14221E940 中观察 `*(_WORD *)v17 = ???`
- 用 x64dbg 日志断点在 sub_141FB3620 抓取实际 buffer 内容对比

**5. TypeID 变化**

- 在 hook 点用 `word([r15+r12+10])` 读取各交互的 TypeID
- Search 当前=1，Take 当前=4，Carry 当前=0x23

**6. handler 结构偏移变化**

- handler+0x180: 交互目标 entity ID（最关键）
- 从 RTTI `UIGamePlayControlCommon_Interaction` 的 vtable 追踪构造函数确认偏移

**7. 价格过滤偏移变化**

- Entity 解引用链：在 sub_140B5DC60 的 0x140B5DE17 附近观察 `[r9+??]`, `[rax+??]`, `[rbx+??]` 偏移
- ItemInfo 价格数组：在 sub_14FB1D490 中观察 `[rsi+???h]` 读取价格数组指针和计数的偏移
- 价格条目大小：在 sub_14FB1D490 中观察 `add rdi, ??h` 的步进值（当前 0x18 = 24 字节）

---

## ★★★ 价格过滤系统（Auto Loot v4 新增）★★★

**CT 文件**: `G:\Claude\reverse\CrimsonDesert_AutoLoot_v4.2.CT`

### 目标

在 auto-loot Take 路径中，根据物品买价过滤低价值物品。

### Entity ID → ItemKey 完整链路（已验证 ✓）

#### 客户端实体哈希查找

```
hash_table = [[[qword_145C9BDA0] + 0x30] + 0x08]
inner_obj  = sub_14031B3F0(hash_table, entity_id)
```

**sub_14031B3F0** (0x14031B3F0, 非 Denuvo, 只读, 无锁):

```c
bucket_count = dword:[hash_table + 0x60]
table        = [hash_table + 0x70]
idx_array    = [hash_table + 0x78]
bucket_base  = table + (entity_id % bucket_count) * 256
entry_count  = dword:[bucket_base]

for i = 0..entry_count:
    if dword:[bucket_base + 8 + i*8] == entity_id:
        slot = dword:[bucket_base + 12 + i*8]
        hash_entry = [idx_array + slot * 8]
        if dword:[hash_entry + 4] == entity_id:
            return [hash_entry + 8]   // → inner_obj
return NULL
```

| AOB                                            | 说明               |
| ---------------------------------------------- | ---------------- |
| `48 89 5C 24 08 83 79 64 00 44 8B C2 4C 8B D1` | sub_14031B3F0 入口 |

#### Inner Object → ItemKey（4 级解引用）

**来源**: sub_140B5DC60 @ 0x140B5DE17-0x140B5DE50

```
component   = [inner_obj + 0x68]
context     = [component + 0x30]
entity_data = [context + 0xC0]
ItemKey     = word:[entity_data + 0x08]   ★
```

**动态验证** (x64dbg 日志断点):

```
[CHAIN] inner_obj=347C5748280 → component=347C4E6CC00
        → context=347C404A800 → entity_data=34711377E00
        → ItemKey=0x353
[SERVER-TAKE] entity_data=347BA32AC80, ItemKey=0x353  ★ 一致
```

### ItemKey → 买价完整链路（已验证 ✓）

#### 直接数组查找 ItemInfo

```
item_mgr   = [qword_145C9C560]
item_array = [item_mgr + 0x50]
ItemInfo   = [ItemKey * 8 + item_array]
```

#### ItemInfo 上的价格数组

价格数据直接在 ItemInfo 上，不需要 category key 或调用任何函数：

```
price_array = [ItemInfo + 0x278]       // 价格条目数组指针
price_count = dword:[ItemInfo + 0x280] // 条目数量
```

每个条目 **24 字节**:

```
+0x00: DWORD 关联 ItemKey 的首 DWORD (匹配用)
+0x08: QWORD 基础买价 ★
+0x10: 其他数据
```

**来源**: sub_14FB1D490（sub_141D2B010 的真实实现）

#### 验证结果（4 个物品）

| 物品     | ItemKey | buy_price (十进制) | 游戏内卖价 | 卖/买比率 |
| ------ | ------- | --------------- | ----- | ----- |
| 卖价 171 | 0x1595  | 360             | 171   | 47.5% |
| 卖价 95  | 0x353   | 250             | 95    | 38%   |
| 卖价 10  | 0x36C   | 25              | 10    | 40%   |
| 无法出售   | 0x622   | 20              | -     | -     |

**结论**: 卖出比率因物品/区域不同而变化（38%-47.5%），但 buy_price 与物品价值严格正相关。直接用 buy_price 作为过滤阈值即可。

#### 排除的价格路径

| 路径                                     | 结论                                               |
| -------------------------------------- | ------------------------------------------------ |
| qword_145CA3668 (ItemGroupDataManager) | 只有 254 条目，按货币/类别索引，不能用 ItemKey 或 ItemGroupKey 查找 |
| ItemGroupInfo + 0x380 (价格结构)           | 测试物品返回 NULL                                      |
| CalcUnitPrice (sub_141B35B60)          | 需要 category key + 复杂调用链，不适合 code cave 内联         |

### 价格过滤实现（v4 code cave）

在 TypeID=4 (Take) 入队前插入过滤逻辑：

```
1. 检查 AT_MIN_PRICE 阈值（0 = 不过滤）
2. entity_id = [r14+180h]
3. hash_table = [[[AT_WORLD]+30h]+08h]
4. call AT_HASH_LOOKUP(hash_table, entity_id) → inner_obj
5. inner_obj → [+68] → [+30] → [+C0] → word:[+08] = ItemKey
6. ItemInfo = [ItemKey*8 + [[AT_ITEM_MGR]+50h]]
7. 遍历 [ItemInfo+278h] 价格数组，取最大 buy_price
8. buy_price < AT_MIN_PRICE → 跳到 AT_MERGE（跳过）
9. 否则 → 正常入队 Take 事件
```

每一步都有 NULL 检查，失败时默认放行（不跳过）。
price_count <= 1 的物品默认放行（可能是任务物品/特殊物品）。

### v4.3 稳定性修复（快速拾取崩溃）

**问题**: v4.2 的 VEH 在调试器附加时不生效（first-chance exception 被调试器拦截），且 VEH 的 timer 延迟注册机制存在时序窗口。实测在快速拾取大量物品时仍然崩溃。

**崩溃现场分析**:

```
ExceptionAddress: 13FFE01BE (price_loop: mov rax, [rsi+8])
rsi = 0x1251000F631C0016 (非规范地址，垃圾数据)
edx = 0x542F3E9 (price_count = 88M，垃圾数据)
```

**根因**: ItemInfo 对象在 price_check 读取 price_count 和 price_array 之前被另一线程释放。NULL 检查通过（垃圾值非零），但数据已是垃圾：

- 垃圾 price_count (88M) > 1，通过 `cmp edx, 1; jle` 检查
- 垃圾 price_array (0x1251...) 非零，通过 `test rsi, rsi; jz` 检查
- price_loop 用垃圾 rsi 读内存 → ACCESS VIOLATION

**修复方案 (v4.3)**:

1. **规范地址验证** — 每个解引用的指针在 NULL 检查后，额外检查是否为合法用户态地址：
   
   ```asm
   mov r10, rax
   shr r10, 2F        // 右移47位
   test r10, r10
   jnz price_ok       // 非零 = bits 47-63 有值 = 非规范/内核地址
   ```
   
   有效用户态地址 bits 47-63 全为零（最大 128TB）。垃圾值如 `0x1251000F631C0016` 的高位非零，被立即拒绝。
   验证点：hash_lookup 结果、inner_obj、component、context、entity_data、ItemInfo、price_array（共 7 处）。

2. **price_count 上限检查** — 正常物品最多 2-3 个价格条目，添加 `cmp edx, 14; ja price_ok`（cap 20）。
   垃圾 count 如 88M 被立即拒绝，防止价格循环失控。

3. **保留 VEH** — 作为最后安全网，处理上述检查未能覆盖的边缘情况（如已释放内存被重新分配为另一个合法对象，地址合法但数据错误）。

**此崩溃的具体防御层**:
| 防御层 | 对此崩溃的效果 |
|--------|--------------|
| rsi 规范地址检查 | ✓ 0x1251... >> 47 = 0x24A2 ≠ 0，被拦截 |
| price_count cap | ✓ 88M > 20，被拦截 |
| VEH (backup) | ✓ 即使前两层失效，AV 仍被 VEH 捕获重定向到 price_ok |

### v4.2 稳定性修复

**问题**: 插入服务端消息实现自动拾取后，偶尔导致游戏崩溃。

**根因分析**:

| 问题         | 风险  | 说明                                                                                                                  |
| ---------- | --- | ------------------------------------------------------------------------------------------------------------------- |
| 价格过滤竞态条件   | 高   | price_check 的 4 级指针链 (inner_obj→component→context→entity_data) 在 UI 线程执行，无同步保护。其他线程可能同时销毁该实体，导致 use-after-free → AV |
| ItemKey 越界 | 中   | 竞态导致读到垃圾 entity_data 时，ItemKey 可能超出 item_array 边界，`[rcx*8+rax]` 访问非法内存                                              |
| 单帧事件爆发     | 中   | 大量物品在场时，hook 在同一帧循环中为每个交互条目入队事件，可能瞬间入队几十个事件，超出服务端处理预期                                                               |
| 服务端实体不存在   | 安全  | `sub_142200D00` 入口有 `cmp [r12],0; jz cleanup`，`sub_141FB3750` 分发前先查找实体，NULL 时安全返回                                   |

**修复方案 (v4.2)**:

1. **VEH 崩溃保护** — 通过 `AddVectoredExceptionHandler` 注册异常处理器，当 Access Violation 发生在 `[price_check, price_ok)` 地址范围内时，将 `CONTEXT.Rip` 重定向到 `price_ok`（安全回退：放行该物品）。VEH handler 在 Lua 延迟定时器（100ms）中通过 `autoAssemble` 创建并注册，DISABLE 时通过 `RemoveVectoredExceptionHandler` 移除。

2. **ItemKey 边界检查** — 在 `[rcx*8+rax]` 之前增加 `cmp ecx, [item_mgr+8]`（item_count），与游戏原始 `GetItemInfo`（`sub_1402D53B0`）的边界检查逻辑一致。越界时跳转 `price_ok` 放行。

3. **事件入队频率限制** — 新增 `AT_BURST_COUNT`（当前计数）和 `AT_BURST_MAX`（上限，默认 5，CE 中可调）。每次成功入队后 `lock inc` 原子递增；Lua 定时器每 200ms 重置为 0。TypeID 分发确认后、进入 do_search/do_take 前检查 `count >= max` 则走原始代码路径。效果：最多 25 事件/秒。

**事件结构正确性验证** (v4.2 分析确认):

- code cave 设置的事件字段与原始游戏代码（`sub_1511B6340` / `sub_14221E940`）完全一致
- `event+0x38`（自引用指针）由分配器 `sub_141016810` 正确初始化，code cave 不覆盖
- 入队函数第 4 参数（r9）：原始代码也未显式设置，code cave 传 0 安全
- `sub_141017C70` 使用 vtable 锁 + 原子操作，跨线程入队线程安全

### 价格过滤新增的全局变量/AOB

| 变量/函数           | 偏移 (base+) / AOB                                    | 用途              |
| --------------- | --------------------------------------------------- | --------------- |
| qword_145C9C560 | +0x5C9C560                                          | ItemInfoManager |
| sub_14031B3F0   | AOB: `48 89 5C 24 08 83 79 64 00 44 8B C2 4C 8B D1` | 实体哈希查找          |

### 价格相关关键函数

| 函数                 | 地址            | 作用                                            |
| ------------------ | ------------- | --------------------------------------------- |
| GetItemInfo        | sub_1402D53B0 | ItemKey(WORD) → ItemInfo*                     |
| GetItemGroupInfo   | sub_1402D3990 | ItemGroupKey(WORD) → ItemGroupInfo*           |
| CalcUnitPrice      | sub_141B35B60 | 计算单价（含货币匹配、税率）                                |
| CalcSellPrice (实际) | sub_14FB1D490 | sub_141D2B010 真实实现，遍历 ItemInfo+0x278          |
| ApplyTax           | sub_141D292B0 | 加税（price += price × tax_rate）                 |
| PriceMultiply      | sub_141B31E30 | sell_price = buy_price × multiplier / 1000000 |
| GetSellMultiplier  | sub_141B36910 | 返回卖出倍率 + 1000000                              |
| FixedPointMul      | sub_140706970 | 定点乘法（大数安全）                                    |
| EntityHashLookup   | sub_14031B3F0 | entity_id → inner_obj（哈希表查找）                  |

### ItemInfo 关键偏移

| 偏移     | 大小    | 说明               |
| ------ | ----- | ---------------- |
| +0x00  | DWORD | 首 DWORD（价格条目匹配用） |
| +0x128 | WORD  | ItemGroupKey     |
| +0x278 | QWORD | 价格条目数组指针 ★       |
| +0x280 | DWORD | 价格条目数量 ★         |

### Entity 解引用链偏移

| 步骤                    | 偏移           | 说明  |
| --------------------- | ------------ | --- |
| inner_obj → component | +0x68        |     |
| component → context   | +0x30        |     |
| context → entity_data | +0xC0        |     |
| entity_data → ItemKey | +0x08 (WORD) |     |



---

---

## ★★★ 出售价格完整分析（v4.3 研究成果）★★★

### 核心发现：出售价格 ≠ price_array 单一条目

出售价格是**两步计算**的结果，不能直接从 price_array 读取：

```
sell_price = PriceMultiply(buy_price, sell_rate + 1000000)
           = buy_price × (sell_rate + 1000000) / 1000000
```

其中：

- `buy_price` = CalcSellPrice 的返回值（从 price_array 匹配对应货币条目）
- `sell_rate` = GetSellMultiplier 的返回值 - 1000000（来自 buff/属性系统，**每物品不同**）

### 验证数据

| ItemKey | buy_price | sell_rate | 计算                 | 游戏显示卖价  |
| ------- | --------- | --------- | ------------------ | ------- |
| 0x353   | 250       | 380000    | 250×380000/1000000 | = 95 ✓  |
| 0x1595  | 360       | 475000    | 360×475000/1000000 | = 171 ✓ |
| 0x36C   | 25        | 400000    | 25×400000/1000000  | = 10 ✓  |

### price_array 条目结构（已通过内存 dump 确认）

ItemKey=0x353 的 price_array（2 个条目，每个 24 字节）:

```
Entry 0: 20 06 00 00 00 00 00 00  FA 00 00 00 00 00 00 00  00 00 00 00 20 06 00 00
         key=0x0620              price=250                              key重复

Entry 1: 2C 06 00 00 00 00 00 00  3F 00 00 00 00 00 00 00  00 00 00 00 2C 06 00 00
         key=0x062C              price=63                               key重复
```

24 字节条目完整结构:

```
+0x00: DWORD  货币/价格类型 key（前2字节被 CalcSellPrice 读为 WORD）
+0x04: DWORD  padding (0)
+0x08: QWORD  价格值
+0x10: DWORD  unknown (0)
+0x14: DWORD  key 重复
```

注意: 两个 currency key (0x0620, 0x062C) 都 < item_count(5993)，是合法 ItemKey。
Entry 0 price=250 = 买价，Entry 1 price=63 = 某种基础价（非直接卖价）。

### CalcSellPrice 匹配机制（已验证推翻原假设）

**原假设（错误）**: CalcSellPrice 用默认出售货币 (0xC470) 匹配 → 找到卖价条目。

**实际情况**: CalcSellPrice 在 sub_140B47530 中被调用时，使用**物品元数据 +0x12** 的货币 hint，不使用默认出售货币。匹配逻辑：

```
1. 调用者传入 currency_hint（来自 sub_1404B8440(group_key) 的 +0x12 字段）
2. CalcSellPrice 内部: sub_1404B8440(currency_hint) → [result+0x12] = 真实货币 ItemKey
3. GetItemInfo(真实货币ItemKey) → 取首 DWORD 作为匹配目标
4. 遍历 price_array 条目, GetItemInfo(WORD:[entry]) 取首 DWORD 对比
5. 匹配成功 → 返回 entry+0x08 的价格 = buy_price
```

**关于默认出售货币 0xC470**:

- `word:[[[[145C9C018]+8]+30]+E8]` = 0xC470 (50288)
- 远超 item_count (5993)，GetItemInfo 返回默认 ItemInfo（首 DWORD=0）
- price_array 两个条目的货币 ItemInfo 首 DWORD 分别是 1 和 0xE，均不匹配
- 所以 CalcSellPrice(ItemInfo, 0xFFFF, 0, &out) **返回 0**（匹配失败）
- 说明 0xFFFF（默认出售货币）路径在当前版本不用于计算背包出售价

### sub_1404B8440 = ItemGroupDataManager 查找

**真实实现**: sub_1481181B0，使用 `qword_145CA3668` (ItemGroupDataManager)

```c
v1 = *a1;  // 读取 WORD key
if ( v1 >= dword:[qword_145CA3668 + 8] )  // 边界检查 (count=254)
    return qword_145C38BB8;               // 默认值
return [v1 * 8 + [qword_145CA3668 + 0x50]];
```

| 全局变量            | 地址          | 用途                      |
| --------------- | ----------- | ----------------------- |
| qword_145CA3668 | 0x145CA3668 | ItemGroupDataManager 指针 |
| qword_145C38BB8 | 0x145C38BB8 | 默认 ItemGroupInfo        |

注意: ItemGroupDataManager 只有 **254 条目**，大多数 ItemKey 和 ItemGroupKey 都 > 254，
会返回默认值 qword_145C38BB8。

### GetSellMultiplier 调用链

```c
__int64 sub_141B36910(__int64 state_obj, WORD ItemKey, SHORT currency, WORD extra_key, char flag)
{
    if ( byte:[sub_1404B8440(&currency) + 0x30] != 1 )
        return 1000000;  // 100%, 不修改

    ctx = [state_obj + 8];
    base_rate = sub_141301DF0(ctx, 10);         // 从属性系统查 type=104/subtype=10
    base_rate += sub_141A72B90(buff, ItemKey, extra_key);  // buff 修正（per-item）
    return base_rate + 1000000;
}
```

**阻塞点**: `state_obj` 来自 UI 层 `[r12+0x508]`（sub_140C19090），通过调用链传递，**无已知全局入口**。

sub_141301DF0 → sub_14C5B40C0: 遍历 `[state_obj+8]` 上的属性条目，筛选 type=104/subtype=10 并累加 qword:[entry+152]。

sub_141A72B90: 接受 ItemKey 参数，提供**每物品不同**的 buff 修正值（这是卖出比率因物品而异的原因）。

### 出售价格计算完整调用链（sub_140B47530，背包价格显示函数）

```
sub_140C19090 (背包 UI)
  └→ sub_140B47530 (价格显示, 3172 bytes, 143 basic blocks)
      ├→ sub_1404B8440(group_key) → metadata
      │   └→ metadata[+0x12] = currency_hint (di 寄存器)
      ├→ sub_1402D53B0(some_key) → ItemInfo
      ├→ sub_141D2B010(ItemInfo, di, 0, &base_price) → CalcSellPrice
      │   └→ base_price = buy_price (如 250)
      ├→ 16 字节条目数组循环 @ 0x140B47A90: 查找 sell_rate
      │   └→ [matched_entry+8] = raw_rate (如 -620000)
      │   └→ add rdx, 0xF4240 → multiplier = rate + 1000000 (如 380000)
      ├→ sub_141B31E30(base_price, multiplier) → PriceMultiply @ 0x140B47AC7
      │   └→ sell_price = 250 × 380000 / 1000000 = 95
      └→ sub_141B36910(...) → GetSellMultiplier @ 0x140B47D32 (第二次计算?)
```

关键代码（0x140B47AC7 处的 PriceMultiply 调用）:

```asm
140b47ab3  mov     rdx, [r15+8]            ; rdx = raw sell rate from 16-byte entry
140b47abc  add     rdx, 0F4240h            ; rdx += 1000000
140b47ac3  mov     rcx, [rbp+510h+var_570] ; rcx = CalcSellPrice 结果 (buy_price)
140b47ac7  call    sub_141B31E30           ; PriceMultiply → rax = sell_price
140b47acc  mov     rdi, rax               ; rdi = sell_price
```

### 价格相关关键函数

| 函数                 | 地址            | 作用                                            |
| ------------------ | ------------- | --------------------------------------------- |
| GetItemInfo        | sub_1402D53B0 | ItemKey(WORD) → ItemInfo*                     |
| GetItemGroupInfo   | sub_1402D3990 | ItemGroupKey(WORD) → ItemGroupInfo*           |
| CalcUnitPrice      | sub_141B35B60 | 计算单价（含货币匹配、税率）                                |
| CalcSellPrice (实际) | sub_14FB1D490 | sub_141D2B010 真实实现，遍历 ItemInfo+0x278          |
| ApplyTax           | sub_141D292B0 | 加税（price += price × tax_rate）                 |
| PriceMultiply      | sub_141B31E30 | sell_price = buy_price × multiplier / 1000000 |
| GetSellMultiplier  | sub_141B36910 | 返回卖出倍率 + 1000000                              |
| FixedPointMul      | sub_140706970 | 定点乘法（大数安全）                                    |
| EntityHashLookup   | sub_14031B3F0 | entity_id → inner_obj（哈希表查找）                  |

### 方案可行性评估

| 需要的函数                             | 可行性    | 说明                  |
| --------------------------------- | ------ | ------------------- |
| sub_1404B8440 (元数据查询)             | ✓ 可调用  | 只读查找，无副作用           |
| sub_141D2B010 (CalcSellPrice)     | ✓ 可调用  | 需要 currency_hint 参数 |
| sub_141B31E30 (PriceMultiply)     | ✓ 完全安全 | 纯计算                 |
| sub_141B36910 (GetSellMultiplier) | ✗ 不可行  | 需要 state_obj，无全局入口  |

**结论**: 直接在 code cave 中调用完整出售价格计算链**不可行**（GetSellMultiplier 的状态对象从 UI 层传入，无全局入口）。当前使用 `buy_price` 作为过滤阈值。

### 可能的替代方案

1. **缓存 hook**: 在 PriceMultiply 调用点 (0x140B47AC7) 设二级 hook，捕获 ItemKey→sell_price 映射并缓存到哈希表，auto-loot 从缓存读取
2. **买价过滤 + 可配置比率**: 用 buy_price × configurable_ratio / 100 近似
3. **进一步研究**: 查找 sell_rate 在 16 字节条目数组中的来源，可能有静态数据路径

---

## ★★★ 范围自动拾取系统（v5.0）★★★

**CT 文件**: `G:\Claude\reverse\CrimsonDesert_AutoLoot_v5.0.CT`

**效果**: 不需要看向物品/尸体，只要在附近就自动触发 Take/Search 事件。

### 核心发现：实体数组

摄像头射线检测之前，游戏通过 `sub_1406B2450` 遍历**所有附近实体**。

```
调用链:
sub_1406B2AA0 (帧tick)
  → sub_1406B2CA0 (帧更新)
    → sub_140FE3800 (检测系统分发器)
      → sub_1406B2450 (per-entity处理, rcx=a1, edx=index)
        → sub_1407E88E0 (检测更新)
          → sub_1407F0F90 (摄像头射线测试)
            → 只有射线命中的实体才生成ring buffer事件
```

**关键**: `sub_1406B2450` 是**按单个实体调用**的，`edx`=实体索引。每帧被调用 N 次（每个实体一次），且**多线程并发调用**。

**实体数组结构** (rcx = 第一个参数):

| 偏移        | 类型    | 说明            |
| --------- | ----- | ------------- |
| rcx+0x110 | QWORD | 实体指针数组基址      |
| rcx+0x118 | DWORD | 实体总数（含4个特殊索引） |

- 实际实体数 = `[rcx+0x118] - 4`
- 实体指针 = `[[rcx+0x110] + 8 * (edx - 4)]`，edx 从 4 开始为普通实体
- IDA确认: `0x1406B26FF: mov rax, [rcx+110h]` 和 `0x1406B2706: mov rsi, [rax+rbx*8]`

**实体对象关键偏移**:

| 偏移    | 类型    | 含义                                            |
| ----- | ----- | --------------------------------------------- |
| +0x00 | QWORD | vtable（检测实体: `0x144751240`，玩家: `0x144751688`） |
| +0x60 | DWORD | **entity_id**                                 |
| +0x64 | DWORD | 版本/generation计数器                              |
| +0x68 | QWORD | sub_object指针（检测链入口）                           |
| +0x88 | QWORD | 类型信息指针 → `[[+0x88]+1]` = type byte            |

### 实体分类算法（动态验证确认）

从 entity 到中间对象的链路:

```
entity → [+0x68] → sub_object → [+0x30] → intermediate
```

intermediate 上的关键偏移:

| 偏移    | 类型            | 物品        | NPC/角色    | 环境/其他 |
| ----- | ------------- | --------- | --------- | ----- |
| +0xC0 | QWORD（物品数据指针） | **非NULL** | NULL      | NULL  |
| +0xE0 | QWORD（角色数据指针） | NULL      | **非NULL** | NULL  |

### ★ 死活区分字段：byte `[sub_object+0x20]+0x26B`

**发现过程**: 分析 `sub_1407F1D10`（摄像头射线后处理）的过滤链时找到。

**动态验证**（0x1407F2079 处日志断点）:

| 实体类型    | b26B  | 行为                             |
| ------- | ----- | ------------------------------ |
| 尸体      | **1** | 被排除在标准交互管线外，Search走另一条管线       |
| 活敌人/NPC | **0** | 通过全部过滤层，显示Attack/Talk等标准交互     |
| 物品      | -     | 不依赖此字段（用 intermediate+0xC0 判断） |

**注意**: 尸体的 intermediate（`[sub_object+0x30]`）**可能为NULL**，所以分类时必须先检测 b26B，再检测 intermediate。

**偏移路径**: `entity → [+0x68] → sub_object → [+0x20] → comp20 → byte [+0x26B]`

### 已排除的死活区分字段

以下字段在 `0x1406B2748` 处动态验证，活敌人和尸体**完全相同**，无法区分：

| 字段                   | 偏移路径                      | 值       |
| -------------------- | ------------------------- | ------- |
| state +0x230/+0x238  | [sub_object+0xB0]+0x230   | 0（非HP）  |
| sub_141908380 返回值    | 调用 [sub_object+0xB0]      | 1（所有实体） |
| byte 0x26E/0x31C     | [sub_object+0x20]+0x26E   | 0       |
| bits 0x368           | [sub_object+0x20]+0x368   | 0       |
| type88               | [[entity+0x88]+1]         | 3       |
| intDef/check1/check2 | [sub_object+0x1B8]→...    | 相同非NULL |
| flag +0x1E9          | [[sub_object+0x40]+0x1E9] | 0       |

### 完整分类算法（v5.0 CT 使用）

```
sub_obj = [entity+0x68]
if sub_obj == NULL → 跳过

// 1. 尸体检测优先（尸体的 intermediate 可能为NULL）
comp20 = [sub_obj + 0x20]
if comp20 != NULL AND byte:[comp20 + 0x26B] == 1:
    → ★ 尸体 → enqueue Search 事件

// 2. 物品检测
intermediate = [sub_obj + 0x30]
if intermediate != NULL AND [intermediate + 0xC0] != NULL:
    → ★ 物品 → enqueue Take 事件（可含价格过滤）

// 3. 其他
→ 跳过 (活角色/环境物体)
```

### 检测系统内部架构

> **结论**: 环形缓冲区中没有"附近所有可拾取物品列表"，必须从 `sub_1406B2450` 的实体数组层面获取。

### TypeID 分配管线（从摄像头射线到 UI 显示）

```
sub_1407F0F90 (摄像头射线测试, 在玩家检测子系统上执行)
  → sub_1407F1D10 (后处理, 过滤命中实体)
    → 过滤链:
      1. switch [[entity+0x88]+1] → 只处理 {3,4,5,6,10,11}
      2. 距离检查
      3. byte [[entity+0x68]+0x20]+0x26B == 0 ?   ← 死活过滤
      4. sub_141B50F00(detSys, entity, 0) == 1 ?
      5. vtable+0x148(detSys, entity, 0, 0) == 1 ? (交互列表构建器)
      6. sub_141308F60(...) == 1 ?                   (事件构建)
    → 写入环形缓冲区 24字节 entry → 回调分发 → UI更新
```

TypeID 在 hook 点 `0x140B5E943` 处读取:

```
interaction_info = [[qword_145C9C5D8 + 0x50] + WORD:[entry+16] * 8]
TypeID = BYTE:[interaction_info + 0x66]
```

检测系统执行路径（`sub_1406B2450` IF 分支）:

- 射线检测通过**玩家实体**(edx=0)的 `sub_1407E88E0` 执行
- `[sub_object+0x40]+0x1E9` 对所有非玩家实体 = 0 → per-entity 路径从不触发
- 动态验证: [A]=4792次, [B]=4644次, [C]=3589次, [D]=0次

### v5.0 Area Loot 实现方案

**Hook 点**: `sub_1406B2450` 入口（15字节函数开头）

**AOB**: `55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 50 0F 29 74 24 40 4D 8B E8 8B F2 48 8B F9`

- 匹配点 = `push rbp` (offset 0x0F from function start)
- Hook address = match - 0x0F
- Return address = match (push rbp)

**被替换的 15 字节**:

```asm
48 89 5C 24 08   ; mov [rsp+08h], rbx
48 89 74 24 10   ; mov [rsp+10h], rsi
48 89 7C 24 18   ; mov [rsp+18h], rdi
```

**每次调用的流程**:

```
1. 执行被替换的 3 条 mov 指令
2. 检查 master switch + area-loot mode → 关闭则跳过
3. edx < 4 → 跳过（特殊实体）
4. 保存寄存器 → 从栈上 [rbp+40]=rcx, [rbp+38]=edx 读参数
5. 从 [rcx+0x110] 获取实体数组，用 edx 索引当前实体
6. Dedup 检查（lockless 读）→ 已处理则跳过
7. 分类: b26B==1→Search, [intermediate+C0]→Take, 其他→跳过
8. 构造服务端事件 + 入队
9. Dedup 添加（spinlock 保护写）
10. 恢复寄存器 → 跳转到 push rbp 继续原函数
```

**线程安全措施**:
| 问题 | 方案 |
|------|------|
| 参数传递 | 用栈上 `[rbp+40]`/`[rbp+38]` 读取，每线程独立栈 |
| Dedup 读 | Lockless（只读竞态无害，最坏情况多发一个事件） |
| Dedup 写 | `xchg [area_lock], eax` 自旋锁保护 |
| Dedup 策略 | **只在成功入队后才添加**，活NPC不占槽位，确保敌人死亡后能被检测 |
| Burst counter | `lock inc`（原子递增） |

**CE 汇编注意事项**:

- `mov r15, [r12 + r14*8]` 等使用 r8-r15 的 scaled index 寻址**无法编译**
- 替代方案: `shl rcx, 3; add rax, rcx; mov rsi, [rax]`
- `[rcx*8+rax]`（使用 rax-rdi）可以编译

### Area Loot 相关地址汇总 (v534)

| 功能            | 地址/偏移                      | 备注                                         |
| ------------- | -------------------------- | ------------------------------------------ |
| sub_1406B2450 | 0x1406B2450                | per-entity 处理 (1602字节, 多线程调用)              |
| sub_1407F1D10 | 0x1407F1D10                | 射线后处理（含死活过滤链）                              |
| sub_1407F0F90 | 0x1407F0F90                | 摄像头射线测试 (3444字节)                           |
| sub_1407E88E0 | -                          | 检测更新                                       |
| sub_1405E46D0 | 0x1405E46D0                | 检测循环 (ring buffer)                         |
| entity_id     | entity+0x60                | DWORD                                      |
| sub_object    | entity+0x68                | QWORD指针                                    |
| comp20        | [sub_object+0x20]          | QWORD指针                                    |
| **死活字节**      | **[comp20+0x26B]**         | **BYTE: 1=尸体, 0=活**                        |
| intermediate  | [sub_object+0x30]          | QWORD指针（尸体可能为NULL）                         |
| 物品数据          | [intermediate+0xC0]        | QWORD（非NULL=物品）                            |
| 角色数据          | [intermediate+0xE0]        | QWORD（非NULL=角色类）                           |
| ItemKey       | [[intermediate+0xC0]+0x08] | WORD                                       |
| 实体数组          | [rcx+0x110]                | 从函数参数获取                                    |
| 实体计数          | [rcx+0x118]                | DWORD（含4个特殊索引）                             |
| Lootable查找    | sub_1406AC320              | 使用 entity+0x5E/+0xC2/+0xCF（检测数组内全部通过，无法区分） |

### Area Loot 版本适配指南

**1. Hook AOB 变化**

- 搜索 `push rbp; push r12-r15; mov rbp,rsp; sub rsp,50h; movaps...` 模式
- 函数特征: 第一个参数是实体管理对象，第二个参数是实体索引
- RTTI: 从 `sub_1406B2AA0` 或 `sub_140FE3800` 的调用链追踪

**2. 实体数组偏移变化**

- 在 `sub_1406B2450` 等价函数中找 `mov rax, [rcx+???h]`（实体数组指针）
- 紧接其后 `mov rsi, [rax+rbx*8]`（实体访问）
- `lea ebx, [rsi-4]`（索引调整）

**3. 分类偏移变化**

- `[sub_object+0x30]` → intermediate（从价格链的 inner_obj+0x68→+0x30 验证）
- `[intermediate+0xC0]` / `[intermediate+0xE0]`（在 `sub_1406AC320` 中确认）
- 死活字节 `[sub_object+0x20]+0x26B`（在 `sub_1407F1D10` 的 `cmp byte ptr [rcx+26Bh], 0` 找）

**4. 线程模型变化**

- 验证 sub_1406B2450 是否仍为多线程调用（x64dbg 查看不同线程 ID）
- 如变为单线程，可移除 spinlock 简化代码

### 服务端事件结构参考（Area Loot 复用）

Search (搜刮尸体):

```
descriptor ID = 0x07E7, event_type = 7
payload = [E7 07 FF <entity_id:4>]
```

Take (拾取物品):

```
descriptor ID = 0x0806, event_type = 0xD (13)
payload = [06 08 FF 00 <entity_id:4> 01 01 00 FF 00]
```

Session info 路径: `[[AT_WORLD]+0x30]+0x20] → +0x60=session_id, +0x58=routing_id`

服务端安全性:

- Take 对非物品: 服务端查找失败 → 忽略
- Search 对活NPC: processLootingDrop 检查掉落数=0 → 直接返回
- Search 对尸体: 掉落数>0 → 处理搜刮
- entity_id 去重后，最多只发一次无效请求

---

## ★★★ 偷窃过滤（v5.0 研究记录）★★★

### 问题

area-loot 自动拾取会 Take 有所有权的物品（NPC 房屋/商店中的物品），导致角色自动偷窃、贡献度下降。

### 核心发现：type_byte = 7 的物品可能是可偷物品

通过 x64dbg 在贡献度写入函数 `sub_141B37B80` 下断点，触发偷窃后分析调用栈和实体数据，确认：

**可偷物品的 `[[entity+0x88]+1] = 7`（type_byte = 7），但 type 7 不代表一定可偷。**

实测数据（NPC 桌上的可偷物品）:

```
type_byte = 07
b26B = 00 (alive)
comp20+2C0 = 0F
entity_data+00 = FFFFFFFF
entity_data+04 = FFFFFFFF
```

正常掉落物 type_byte ≠ 7（在 {3,4,5,6,A,B} 中）。

**重要修正**：x64dbg 在偷窃判定门 `0x1422026AE` 处的实测表明，r12（物品实体）在偷窃和正常拾取时**都是 type 7**。type 7 是 NPC 相关物品的通用类型，包含可偷和不可偷两种。真正区分可偷/不可偷的是 `sub_141EB39E0` 的返回值（al=1 可偷，al=0 不可偷），该函数内部通过 `sub_14E9CF850` 检查物品的所有权列表。

### 为什么相机模式不受影响

相机检测管线 `sub_1407F1D10` 的类型过滤只处理 {3,4,5,6,10,11}：

```c
switch ( [[entity+0x88]+1] ) {
  case 3: case 4: case 5: case 6: case 0xA: case 0xB:
    // 处理交互
  default:
    break;  // type 7 在此跳过
}
```

type 7 天然被排除在相机检测之外，因此相机模式（v4.4）从不会触发偷窃。

### 服务端偷窃处理（调用栈确认）

即使用 a4=0（正常 Take）发送事件，服务端也会检测物品的所有权状态并触发贡献度扣减。

贡献度扣减调用链（x64dbg 实测）:

```
sub_141B37B80  ← 贡献度值写入
sub_141B38D60  ← 贡献度更新
sub_142320FF3  ← 偷窃后果处理
sub_151509D5C  ← 事件处理
sub_141FB3A20  ← vtable 分发（偷窃后果事件）
sub_141FB3750  ← 事件分发器
...
sub_142201210  ← 核心 Take 处理（内部检测所有权，触发偷窃后果事件）
sub_142200D00  ← Take handler
sub_151509760  ← 网络消息处理
sub_141FB3A20  ← vtable 分发（原始 Take 事件）
```

关键：`sub_142201210`（核心 Take 处理，5605字节）在内部检测物品所有权状态，无论 a4=0/5 都会触发偷窃后果事件链。

### 偷窃判定门（steal gate）

在 `sub_142201210` 内部 `0x1422026AE` 处调用 `sub_141EB39E0`：

```asm
1422026A1  mov     r8, r12          ; r12 = 物品实体 (type 7)
1422026A4  mov     rdx, r14         ; r14 = 服务器请求上下文
1422026A7  mov     rcx, [rax+120h]  ; 交互会话对象
1422026AE  call    sub_141EB39E0    ; 偷窃判定
1422026B3  test    al, al
1422026B5  jz      loc_142202729    ; al=0 → 跳过偷窃路径（正常物品）
```

- **al=1**：可偷物品 → 进入偷窃路径 → `call r10` @ 0x142202726 → 贡献度扣减
- **al=0**：正常物品 → 跳过偷窃路径

**x64dbg 实测确认**:

```
偷窃: r12_type=7, r12_inter=非空, al=1
正常: r12_type=7, r12_inter=非空, al=0
```

注意：r12 在偷窃和正常拾取中**都是 type 7**，区别在于 `sub_141EB39E0` 的返回值。

### sub_141EB39E0 内部逻辑（偷窃判定函数）

签名：`char sub_141EB39E0(a1=会话上下文, a2=服务器请求上下文, a3=实体指针)`

对 type 7 实体的判定流程：

```
1. 预检查（所有类型）:
   - [sub_object+240]+376 != 0 → return 0 (不可偷)
   - [sub_object+32]+789 != 0 → return 0 (不可偷)
   - sub_141EB3DC0(a1, ...) → return 0 (需要 a1 会话上下文)

2. type == 7 时调用所有权检查:
   sub_141A924C0([[entity+0x68]+0x30], a2, ...)
     → jmp sub_14E9CF850 (thunk)
   - 返回 1 → 不可偷（物品在所有权列表中）
   - 返回 0 → 可偷（物品不在所有权列表中）→ 继续返回 1

3. 最终: return 0=不可偷, return 1=可偷
```

### sub_14E9CF850 所有权检查函数

签名：`char sub_14E9CF850(a1=intermediate, a2=context, a3=flag, a4=output_byte)`

- 检查 `intermediate+0x320`（数组基址）/ `+0x328`（数组计数）
- 通过 `sub_140444BE0`（全局管理器 `qword_145CA0CB8`）查找对象
- a2 控制 vtable 路径：a2!=0 → vtable+16（上下文匹配），a2=0 → vtable+8（简单存在检查）
- 返回 1 = 找到匹配（不可偷），返回 0 = 未找到（可偷）

**注意：该函数位于 .bss 段，CE 的 AOBScan 无法直接扫描到**（Denuvo 保护）。
通过 `sub_141EB39E0` 内部的 call 指令间接定位 thunk `sub_141A924C0`（.sdata 段）。

### 客户端过滤方案（已失败）

尝试从 area-loot hook 的 code cave 调用 `sub_14E9CF850` 过滤可偷物品，未成功：

| 问题                     | 原因                                                                |
| ---------------------- | ----------------------------------------------------------------- |
| 玩家 intermediate 为 NULL | `[[[detection_obj+0x20]+0x68]+0x30] = 0`，首个实现用了玩家实体的 intermediate |
| AOB 匹配错误函数             | 旧 AOB 模式匹配到 `0x141D1E8B0` 而非 `0x14E9CF850`（两函数前 39 字节相同）          |
| .bss 段不可扫描             | CE AOBScan 带 `-W` 标志跳过可写页，sub_14E9CF850 在 .bss 段                  |
| a2 参数不匹配               | 服务端传 server context，code cave 传 entity/0 → vtable 匹配逻辑不同 → 全部返回 0 |
| 函数被广泛调用                | sub_141A924C0 每帧被大量调用，无法用断点精确调试 code cave 的调用                     |

**结论：从客户端 code cave 调用服务端偷窃判定函数不可行**（缺少正确的服务器上下文参数）。

### 最终方案：服务端偷窃门 patch

直接 patch `0x1422026B5` 处的条件跳转：`jz` (74) → `jmp` (EB)。

效果：`sub_141EB39E0` 的返回值被忽略，偷窃路径永远被跳过 → 拾取 NPC 物品不再扣减贡献度。

```
AOB: 49 8B D6 48 8B 88 20 01 00 00 E8 ?? ?? ?? ?? 84 C0 74 72
Patch offset: +0x11 (第 17 字节, 即 jz 的操作码)
Enable:  74 → EB
Disable: EB → 74
```

CT 中作为独立 CheatEntry 实现（ID=11，"Skip Steal Penalty"）。

### 已排除的方案汇总

| 方案                             | 结论                                     |
| ------------------------------ | -------------------------------------- |
| intermediate+0x2A8 所有权字段       | 实测 = 0（仅适用于 type 7 检测管线 a3==3 路径）      |
| 服务端 a4 参数区分                    | 无效：sub_142201210 内部独立检测所有权，与 a4=0/5 无关 |
| 客户端 code cave 调用 sub_14E9CF850 | 无效：缺少服务器上下文参数，.bss 段 AOB 不可达           |
| AT_NO_STEAL type 7 前置过滤        | 可防止拾取 type 7 物品，但无法区分可偷/不可偷的 type 7 物品 |
