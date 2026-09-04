# Crimson Desert v1.0.0.978 分析记录

> 本文档只保留 `CrimsonDesert_AutoLoot_v6.2.CT` 当前可用结论和后续排查边界。被推翻的中间方案已压缩到“弃用规则”。

## 当前结论

| 功能 | 状态 | 关键点 |
| --- | --- | --- |
| 区域搜索尸体 | 可用 | desc `0x07E7`, size `0x07` |
| 区域拾取物品 | 可用 | desc `0x0807`, size `0x0D`, mode `0` |
| 区域采集植物 | 可用 | desc `0x0807`, size `0x0D`, mode `5` |
| 相机模式 | 已恢复 | v978 目标 EID 优先读 `[r14+0x180]` |
| 自动捕虫 | 已验证 | desc `0x07FE`, size `0x08`, `event+0x58=player_eid` |
| 任务/脚本绑定物 | 当前规避 | `comp+0x2C8 == 1` 时跳过自动 Take |

## 区域 Hook

| 项目 | v978 |
| --- | --- |
| per-entity 区域遍历函数 | `sub_1406EF7A0` |
| CT AOB 命中位置 | `0x1406EF7AF` |
| CT 实际 hook 地址 | `0x1406EF7A0` |
| CT 返回地址 | `0x1406EF7AF` |

```asm
55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 50
C5 F8 29 74 24 40 4D 8B E8 8B F2 48 8B F9
```

调用约定：

```text
rcx = entity_list_obj
edx = entity_index
r8  = context
```

area hook 入口缓存：

```asm
AT_HASH_TABLE = [rcx+0x08]
AT_PLAYER_OBJ = [rcx+0x20]
```

区域模式启用条件：

```text
AT_ENABLED != 0
AT_AREA_ON != 0
entity_index >= 4
```

植物 hash-table 扫描只在 `entity_index == 4` 执行，避免每个 entity slot 都全表扫描。

## 结构偏移

### entity_list_obj

| 偏移 | 含义 | 用途 |
| --- | --- | --- |
| `+0x08` | 全实体 hash table | 植物扫描 |
| `+0x20` | player entity | 事件路由字段 |
| `+0x110` | 普通 entity 指针数组 | 尸体、物品路径 |

### entity

| 偏移 | 含义 |
| --- | --- |
| `+0x58` | route/session id；玩家实体上用于普通事件 `event+0x58` |
| `+0x60` | entity id |
| `+0x68` | sub_object |
| `+0x88` | type info pointer |
| `[[entity+0x88]+1]` | entity type byte；植物为 `7` |

### sub_object / comp / intermediate

| 路径 | 含义 | 用途 |
| --- | --- | --- |
| `[entity+0x68]+0x20` | comp | 尸体、过滤、交互分类 |
| `[entity+0x68]+0x30` | intermediate | 物品/植物数据 |
| `[entity+0x68]+0x1A0` | world transform chain | `GetWorldPos` 距离过滤 |
| `comp+0x273` | corpse/dead flag | `==1` 时 Search |
| `comp+0x2C8` | interaction class/status | 物品、捕虫、任务过滤 |
| `intermediate+0xC0` | item_data | 非空则普通物品拾取 |
| `intermediate+0xE0` | char/gather data | 植物判断要求非空 |
| `intermediate+0x3E2` | locked / not-interactable | `==1` 时跳过 |

`comp+0x2C8` 已确认值：

```text
0x01 -> 特殊/附属/绑定交互对象，跳过自动 Take
0x09 -> v978 昆虫捕捉交互类
0x0F -> NPC 摆放/商店物品，跳过
0x11 -> 世界装饰/固定物品，跳过
0x16 -> 玩家丢弃物品，由 AT_NO_PLAYER_DROP 控制
```

物品价格过滤：

```text
item_key = WORD [item_data+0x08]
item_info price_count = [item_info+0x280]
item_info price_array = [item_info+0x278]
price entry stride = 0x18
price value = [entry+0x08]
```

## 事件格式

所有普通区域事件：

```text
event+0x50 = player_eid
event+0x58 = player_route_id
```

| 类型 | 函数 | desc / size | payload |
| --- | --- | --- | --- |
| Search 尸体 | `sub_142356E50` | `0x07E7` / `0x07` | `E7 07 FF <target_eid:dword>` |
| Take 物品 | `sub_142334AF0` | `0x0807` / `0x0D` | `07 08 FF 00 <target_eid:dword> 01 01 00 FF 00` |
| Gather 植物 | `sub_142196960` | `0x0807` / `0x0D` | `07 08 FF 05 <target_eid:dword> 00 00 01 FF 00` |

等价 dword 写法：

```asm
; Search
mov byte ptr [rdx+0], E7
mov byte ptr [rdx+1], 07
mov byte ptr [rdx+2], FF
mov [rdx+3], ebx

; Take
mov dword ptr [rdx+0], 00FF0807
mov [rdx+4], ebx
mov dword ptr [rdx+8], FF000101
mov byte ptr [rdx+0C], 0

; Gather
mov dword ptr [rdx+0], 05FF0807
mov [rdx+4], ebx
mov dword ptr [rdx+8], FF010000
mov byte ptr [rdx+0C], 0
```

备注：Gather 真实函数尾部 word 有额外计算路径，但当前简化尾部在区域采集中实测可用。

## 植物 Hash Table

植物不走 `entity_list_obj+0x110` 普通数组，而是在全实体 hash table 扫描。

```text
hash_table+0x60 : DWORD bucket_count
hash_table+0x70 : QWORD bucket_array
hash_table+0x78 : QWORD value_array
```

bucket 布局：

```text
bucket+0x00 : DWORD entry_count
bucket+0x08 : entry[0].entity_id
bucket+0x0C : entry[0].slot_index
bucket+0x10 : entry[1].entity_id
bucket+0x14 : entry[1].slot_index
...
```

安全限制：

```text
bucket_count <= 0x1000
每个 bucket 最多扫描 0x1E 个 entry
```

植物判定：

```text
entity_type == 7
[entity+0x68] != NULL
[[entity+0x68]+0x30] != NULL
[[[entity+0x68]+0x30]+0xC0] == NULL
[[[entity+0x68]+0x30]+0xE0] != NULL
```

## 距离、去重、Burst

世界坐标函数：

```text
GetWorldPos = sub_1413CA000
transform = [[entity+0x68]+0x1A0]
```

CT AOB：

```asm
40 53 48 83 EC 50 48 8B 41 68 48 8B DA 48 8D 4C 24 20
48 8B 90 A0 01 00 00 C5 FC 10 82 98 00 00 00
```

距离过滤：

```text
dist2 = dx*dx + dy*dy + dz*dz
dist2 <= AT_LOOT_RANGE * AT_LOOT_RANGE
AT_LOOT_RANGE == 0 时不限制距离
```

区域模式共享去重：

```text
dedup_data:
  +0x00 DWORD count
  +0x04 DWORD write_index
  +0x08 DWORD entity_id[0x400]
```

成功 enqueue 后 `lock inc [AT_BURST_COUNT]`。默认 `AT_BURST_MAX = 5`，Lua timer 每 `200ms` 清零。

## 捕虫

手动捕虫函数：

```text
sub_1423C15A0
desc_id  = 0x07FE
buf_size = 0x08
payload  = FE 07 FF <target_eid:dword> 03
```

捕虫事件路由：

```text
event+0x50 = player_eid
event+0x58 = player_eid   ; 不使用普通 Search/Take/Gather 的 route/session 字段
event+0x60 = descriptor(0x07FE)
event+0x68 = 8
```

已验证样本：

```text
target EID = 0xB010174E
payload = FE 07 FF 4E 17 10 B0 03
manual event+0x50 = 0xA0100001
manual event+0x58 = 0xA0100001
[[target+0x68]+0x20]+0x2C8 == 0x09
```

结论：

```text
v978 捕虫不是旧版 0x07FD，也不是 0x07F5 result 包。
CT 应创建 0x07FE trigger 包。
Catch routing 必须填 player_eid。
捕虫交互分类使用 comp+0x2C8 == 0x09。
```

## 相机模式

旧路径从 UI row 读取：

```text
[rdi+0x210] 或 [[rdi+0x1E0]+0x2C]
```

v978 中这些值可能是 `0xFFxxxxxx` camera internal key，不是服务器实体 EID。真实目标 EID 位于：

```text
[r14+0x180]
```

示例：

```text
camera key = 0xFF1A6875
target EID = 0xB010171A
```

`load_camera_eid` 应优先读 `[r14+0x180]`，仅在为空或明显无效时回退旧 UI row 路径。

## 任务/脚本绑定物过滤

问题：部分任务物品被 CT 自动 Take 后不会播放任务动画，也不会推进任务状态。已知样本包括任务武器；任务书籍、工具、扫帚等同类物品也需要保守规避。

关键结论：

```text
0x0807 Take 包格式本身正确。
卡任务原因是 CT 直接发送 Take，绕过 native interaction / gimmick 侧动画和任务状态逻辑。
这类物品应跳过自动拾取，保留游戏原生手动交互。
```

当前 CT 规则：

```text
仅检查 comp+0x2C8 == 1。
命中时跳过自动 Take。
不叠加 related EID / native+0x2A8 / ItemKey 条件。
```

已知样本：

```text
任务物品:
  B010006D key=1803 comp2C8=1 rel=B010008E -> 命中/跳过

普通 related-EID 样本:
  B01003F1 key=0C06 comp2C8=0 rel=B01003F3 -> 不命中/不跳过
  B0100405 key=047F comp2C8=0 rel=B01003FD -> 不命中/不跳过

普通玩家丢弃武器:
  B0100195 key=1815 comp2C8=0 -> 不应按 ItemKey 跳过
```

IDA 静态依据：

```text
sub_141E12C80 @ 0x141E12CD0
sub_142335450 @ 0x14233548C

comp2C8==1 时不走普通参数来源，而是：
  sub_object = [entity+0x68]
  special = [sub_object+0xB0]
  id = [special+0x2C0]

否则才从普通交互参数取 id。
```

这说明 `comp2C8==1` 很可能是附属/绑定/特殊交互对象，不是普通掉落物。

弃用规则：

```text
native+0x2A8 != 0
  该字段更像当前持有者/占用者/交互者 EID，不能表示任务绑定。

ItemKey 0x1801..0x181C 硬跳过
  普通玩家丢弃武器也会落在该范围，会误伤。

owner+0x5B0 == owner+0x5B4 且 related EID != self
  普通物品也可能满足，不能单独作为任务实例过滤。
```

注意：如果进程里已启用的是旧 CE 代码洞，需要重新启用/重载脚本，才会使用 CT 文件里的 `comp2C8-only` 过滤。

## 后续排查边界

不要反复重查下列已生效部分：

```text
区域 Search: 0x07E7 / size 7 / target EID at payload+3
区域 Take:   0x0807 / size 0x0D / 00FF0807
区域 Gather: 0x0807 / size 0x0D / 05FF0807
区域 Catch:  0x07FE / size 0x08 / event+0x58=player_eid / FE 07 FF <eid> 03
区域 hash table 植物扫描: +0x60/+0x70/+0x78
区域普通实体路径: entity_list_obj+0x110
玩家上下文: entity_list_obj+0x20 -> player entity
相机目标 EID: [r14+0x180]
捕虫分类: comp+0x2C8 == 0x09
任务/脚本绑定物过滤: comp+0x2C8 == 1
```

后续若继续验证任务书籍、工具、扫帚等样本，优先记录：

```text
self EID
item_key
comp+0x2C8
native+0x3E2
owner+0x5B0/0x5B4
是否手动交互触发任务动画或任务状态推进
```
