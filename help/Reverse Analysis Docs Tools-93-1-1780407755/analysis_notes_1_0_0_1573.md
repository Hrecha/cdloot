# Crimson Desert v1.0.0.1573 分析记录

> 本文档记录 978→1573 移植（AutoLoot v7.0 CT）。所有逆向结论经独立对抗性 agent 复核。
> 事件 descriptor / buffer 常量经 x64dbg 实机验证（在 AT_ENQUEUE = sub_14210B130 设日志断点）。
> IDB: `F:\SteamLibrary\steamapps\common\Crimson Desert\bin64\idb\CrimsonDesert.exe-1573.i64`
> 上一版文档: `analysis_notes_1_0_0_978.md`

## 移植总览

| 维度   | 结论                                                                            |
| ---- | ----------------------------------------------------------------------------- |
| 区域模式 | 几乎零改动：结构偏移全部未漂移，只换 2 个 AOB（area hook 实际未变，GetWorldPos 变）                      |
| 相机模式 | 需寄存器/偏移重映射：context r14→rsi、type-byte esi→r15d、偷取 16→17 字节、legacy EID 偏移 +0x10 |
| 事件协议 | **全部 desc ID 变化**（实测），且 gather 事件格式改为与 take 相同                                |
| 偷窃门  | 函数地址变，AOB 重写，patch 偏移不变                                                       |

## AOB 变更表（978 → 1573）

| 用途                       | 状态    | v1573 地址 / 新 AOB                                                                                    |
| ------------------------ | ----- | --------------------------------------------------------------------------------------------------- |
| TLS init                 | 不变    | 0x1410C1530                                                                                         |
| descriptor lookup        | 不变    | 0x1410C1590                                                                                         |
| event allocator          | 不变    | 0x1410BF4D0                                                                                         |
| event enqueue            | 不变    | 0x14210B130（rdx=event 对象，约定不变）                                                                      |
| entity hash lookup       | 不变    | 0x140346520                                                                                         |
| area hook                | 不变    | 0x1406EA67F（fn start 0x1406EA670；hook=match-0xF，ret=match）                                          |
| DESC_MASK/QUEUE resolver | 不变    | 0x14210C2A3 → DESC_MASK=0x145C76A0C, QUEUE=0x145FA8798                                              |
| ITEM_MGR                 | 不变    | 运行时打分，多匹配正常                                                                                         |
| **GetWorldPos**          | **变** | `40 53 48 83 EC 50 48 8B 41 68 48 8B DA 48 8B 88 A0 01 00 00 C5 FC 10 81 98 00 00 00` @ 0x1413CDA50 |
| **相机 hook**              | **变** | `44 0F B6 78 66 48 8B 06 48 8B CE FF 90 08 01 00 00` @ 0x140BA5415（17 字节）                           |
| **merge 点**              | **变** | `4C 8B B6 D0 00 00 00 49 8B 86 D0 00 00 00` @ 0x140BA56A2                                           |
| **STEAL_BYPASS**         | **变** | `4D 8B C5 49 8B D7 48 8B 89 20 01 00 00 E8 ?? ?? ?? ?? 84 C0 74 04 B3 02` @ 0x1423755E1             |
| **STEAL_BYPASS2**        | **变** | `4D 8B CD 4D 8B C4 E8 ?? ?? ?? ?? 84 C0 75 4F 32 DB` @ 0x14228509A                                  |

GetWorldPos 注意：1573 版本相对 978 寄存器从 rdx→rcx（`48 8B 90`→`48 8B 88`），且该函数含速度外推（velocity*dt），但 transform 偏移仍为 +0x1A0，世界坐标用途正确。

## 结构偏移：区域模式全部不变（已验证）

所有 978 偏移在 1573 中**未漂移**（与"687→978 漂移 +8、978→1573 未再漂移"吻合）：

| 偏移                                  | 含义                         | 验证                                  |
| ----------------------------------- | -------------------------- | ----------------------------------- |
| entity+0x58                         | route/session id           | 不变                                  |
| entity+0x60                         | entity id                  | 不变                                  |
| entity+0x68                         | sub_object                 | 不变（area hook fn 确认）                 |
| entity+0x88                         | type info；[[+0x88]+1]=type | 不变                                  |
| sub_obj+0x20                        | comp                       | 不变                                  |
| sub_obj+0x30                        | intermediate               | 不变                                  |
| [sub_obj+0x68]+0x1A0                | world transform            | 不变（GetWorldPos 确认）                  |
| comp+0x273                          | dead flag (==1→Search)     | 不变（cmp [reg+273],1 @0x14234C229 等）  |
| comp+0x2C8                          | 交互类别                       | 不变（cmp [reg+2C8],0F @0x14236A133 等） |
| intermediate+0xC0                   | item_data                  | 不变                                  |
| intermediate+0xE0                   | char/gather data           | 不变                                  |
| intermediate+0x3E2                  | locked/不可交互                | 不变（movzx [reg+3E2] @0x1423695A1）    |
| item_data+0x08                      | item_key (WORD)            | 不变                                  |
| item_info +0x08/+0x50/+0x278/+0x280 | count/array/price          | 不变（stride 0x18）                     |
| entity_list +0x08/+0x20/+0x110      | hash/player/array          | 不变                                  |
| hashtable +0x60/+0x70/+0x78         | bucket_count/array/value   | 不变                                  |
| 事件结构 +0x30~+0x78                    | 见下                         | 不变                                  |

comp+0x2C8 交互类别值（实测沿用 978，未变）：

```
0x01 -> 任务/脚本绑定，跳过自动 Take
0x09 -> 昆虫捕捉交互类
0x0F -> NPC 摆放/商店，跳过
0x11 -> 世界装饰/固定，跳过
0x16 -> 玩家丢弃，由 AT_NO_PLAYER_DROP 控制
```

## ★ 事件协议变更（x64dbg 实测，非静态）★

在 AT_ENQUEUE（0x14210B130，入口 rdx=event）设日志断点，手动交互记录：

| 交互   | size | desc 978→1573       | buffer (1573)                             |
| ---- | ---- | ------------------- | ----------------------------------------- |
| 搜刮尸体 | 7    | 0x07E7 → **0x07E8** | `E8 07 FF <eid>`                          |
| 拾取物品 | 0xD  | 0x0807 → **0x0809** | p0=`00FF0809`, p8=`FF000101`              |
| 采集植物 | 0xD  | 0x0807 → **0x0809** | **与拾取完全相同**（p0=`00FF0809`, p8=`FF000101`） |
| 捕捉昆虫 | 8    | 0x07FE → **0x0800** | `00 08 FF <eid> .. .. .. 03`              |

**关键变化**：

1. 所有 desc ID 变化（Search +1，Take/Gather/Catch +2）。size 全部不变。
2. **采集植物事件格式 = 拾取物品**（978 用 mode=5 `05FF0807`/`FF010000`，1573 统一为 `00FF0809`/`FF000101`，无 mode 字节）。服务端改为按实体类型区分 gather/take。
3. 捕虫 `event+0x58 = player_eid`（不变）。

实测样本日志：

```
拾取: size=D desc=809 p0=FF0809 p4=B01001A4 p8=FF000101 eid50=A0100001 r58=A0100001
采集: size=D desc=809 p0=FF0809 p4=B01000DA p8=FF000101 eid50=A0100001 r58=A0100001
捕虫: size=8 desc=800 p0=67FF0800 p4=3B01002 p8=1A11DB0 eid50=A0100001 r58=A0100001
搜刮: size=7 desc=7E8 p0=FF07E8 p4=B0100A   p8=2FB4DAC0 eid50=A0100001 r58=A0100001
```

事件结构字段（rdx，与 978 一致）：

```
+0x30=1, +0x40=0, +0x48=0(qword), +0x50=player_eid, +0x54=0,
+0x58=route(普通)/player_eid(catch), +0x60=desc对象, +0x68=buf_size(WORD),
+0x70=buffer指针, +0x78=1(byte)
```

## 相机模式 hook 移植（978 → 1573）

相机 UI 函数：978 `sub_140BD0C70` → 1573 `sub_140BA4C90`（靠字符串
"InteractionRightRowCoin"/"KeyGuideButtonInteraction"/"KeyGuideButtonWidget" 定位）。

| 项            | v978               | v1573                               |
| ------------ | ------------------ | ----------------------------------- |
| hook 地址      | 0x140BD141E        | 0x140BA5415                         |
| 偷取长度         | 16 字节              | **17 字节**（14 跳转 + 3 NOP）            |
| ret 偏移       | hook+0x10          | **hook+0x11** = 0x140BA5426         |
| context 寄存器  | r14                | **rsi**                             |
| type-byte 目标 | esi                | **r15d**（REX.R 前缀）                  |
| 交互行指针        | r13                | r13（不变，`word[r13+10]`=TypeID）       |
| manager 寄存器  | rdi                | rdi（不变，与 context 不同对象）              |
| 主相机 EID      | [r14+180]          | **[rsi+180]**（偏移 0x180 不变）          |
| legacy flag  | [rdi+20D]          | **[rdi+21D]**                       |
| legacy EID   | [rdi+210]          | **[rdi+224]**                       |
| legacy ptr   | [rdi+1E0]→[rcx+2C] | **[rdi+1F0]**→[rcx+2C]（内层 +0x2C 不变） |

被偷取的 17 字节（v1573）：

```
44 0F B6 78 66        movzx r15d, byte ptr [rax+66h]   ; +0  (5B)
48 8B 06              mov   rax, [rsi]                  ; +5  (3B)
48 8B CE              mov   rcx, rsi                    ; +8  (3B)
FF 90 08 01 00 00     call  qword ptr [rax+108h]        ; +11 (6B), 结束 +0x11
```

TypeID 值不变：1=Search, 4=Take, 0xF=Gather, 0x18=Catch。

### CT 实现要点

1. **context 捕获**：cave 入口 `mov r14, rsi`，把相机 context 存进 cave 从不破坏的 r14。
   这样 load_camera_eid 的 `[r14+180]` 不变。AT_MERGE（`mov rsi,[r14+D0]`）也读 r14 作为
   context，所以 autoloot 路径 jmp AT_MERGE 时必须 r14=context——`mov r14,rsi` 是必需的。
2. **type-byte 进 r15d**：not_autoloot 重放块用 `movzx r15d,...`，并保证 r15 在 push/pop 集中。
3. **rax 依赖**：`[rax+66]` 来自 hook 前的 `call sub_1402FBBC0`，cave 入口须保留 rax。
4. 

## 偷窃门（v1573）

| 功能             | 978 函数        | 1573 函数           | patch                             |
| -------------- | ------------- | ----------------- | --------------------------------- |
| 偷窃判定           | sub_141FCE880 | **sub_142007490** | —                                 |
| 核心 Take（门 1）   | sub_142337F90 | **sub_1423752F0** | STEAL_BYPASS+0x14 `74`→`EB`       |
| 植物 Gather（门 2） | sub_142249960 | **sub_142284F40** | STEAL_BYPASS2+0xD `75 4F`→`90 90` |

STEAL_BYPASS2 的 jnz 是短跳（2 字节），NOP 写 `90 90` 正确。两个门的 call 都解析到 sub_142007490。

## 待运行时确认 / 后续

- v7.0 CT 已写好但**相机模式未实机测试**（区域模式为默认、改动最小）。相机模式建议实测确认：
  - TypeID dispatch（[r13+10] 值 1/4/0xF/0x18）
  - load_camera_eid 的 [rsi+180]（经 r14）能取到正确 server EID（非 0xFFxxxxxx）
- 区域模式可直接用（结构偏移 + 事件协议均已确认）。

## 共享 IDA session 经验

- 多 agent 共享同一 idalib session 会串行排队，大库（3.5GB）下单次调用可能很慢
  （单个审核 agent 跑了 21~37 分钟），不是卡死。
- `search_text`（find_text 全库文本扫描）在大库上 60s 超时；改用 `find_bytes`（按字节）。

## ★★★ 静态获取 desc ID 的标准流程（dump 脚本，替代逐个动态调试）★★★

> 解决"每个新版本都要动态调试找 desc ID"的问题。desc ID 是引擎启动时按注册顺序
> 动态分配的（这就是它每版 +1/+2 的根因），**纯静态读不到具体值**，但可以一次性
> dump 全表，按稳定的 RTTI 类名反查 desc_id，把动态成本从 N 次降到 1 次。

### 描述符表机制（IDB 逆向，已验证）

desc lookup `sub_1410C1590` 是开放寻址哈希表查找。管理器字段（相对 patDQ 解析出的
DESC_MASK 全局 = dword_145C76A0C）：

```
DESC_MASK    = mgr+0x00   (patDQ 解析目标, = 145C76A0C)
bucket_count = mgr+0x04   (dword, = 145C76A10)
entry_count  = mgr+0x08   (dword 低32位, = 145C76A14)
bucket_array = mgr+0x14   (qword, = 145C76A20)   每桶 256 字节
value_array  = mgr+0x1C   (qword, = 145C76A28)
```

- 桶布局 (256B)：`[+0]`=count(dword)，之后 31 项每项 8B：`[+8+i*8]`=hash, `[+0xC+i*8]`=slot
- 条目：`value_array[slot*8]` → entry；`entry[+0]`=hash, `entry[+4]`=desc_id(WORD), `entry[+8]`=描述符对象
- 描述符对象 RTTI（标准 MSVC64，已验证）：`obj[0]`=vftable；`vftable[-8]`=&COL(_R4)；
  `COL[+0]`=signature(=1)；`COL[+0xC]`=TypeDescriptor 的 image-relative 偏移；
  `TypeDescriptor[+0x10]`=mangled 名 `.?AV<Class>@pa@@`。
  注意：COL+0x14 是 pSelf(指向自己)，**不是** TD；TD 在 +0x0C。

### 使用流程（每个新版本）

1. CE 附加游戏，进可交互状态（确保表已注册填充）
2. 运行 `G:\Claude\reverse\dump_event_descriptors.lua`（CE Lua Engine）
3. 在输出里按**稳定类名**搜对应 desc_id（类名是源码符号，跨版本不变；变的只是数值）

### 1573 dump 实测结果（与动态测量完全吻合，互相印证）

| 交互          | desc_id          | RTTI 类名（稳定锚点）                             |
| ----------- | ---------------- | ----------------------------------------- |
| 拾取物品 Take   | **0x0809**       | `TrocTrProcessPickUpItemOnceTimer`        |
| 搜刮尸体 Search | **0x07E8**       | `TrocTrProcessLootingDeadDropOnceTimer`   |
| 捕捉昆虫 Catch  | **0x0800**       | `TrocTrPushCharacterToInventoryOnceTimer` |
| 采集植物 Gather | (复用 Take 0x0809) | 无独立 desc，印证 gather=take                   |

其它可能有用的：`0x07F4 ItemCatchFromInventoryOnceTimer`、
`0x0B0E LootingDropFromDeadBodyReq`、`0x09F1 AutoPickUpItemToTargetFailAck`。
全表共 1030 个描述符，desc_id 范围 0x03EA~0x0BC9 + 0x270C~0x270F。

### 边界与稳健性

- DESC_MASK 由 patDQ AOB 动态解析（跨版本稳健）。
- 但字段偏移（+4/+8/+0x14/+0x1C）和 RTTI 偏移（COL+0xC/TD+0x10）是 1573 静态推得；
  未来版本若管理器结构变化需重核。脚本内置 bucket_count 异常检测（>0x100000 中止），
  偏移错位时会立即报错，不会静默误读。
- 脚本同时输出每个对象的 **vtable 地址**：即使 RTTI 名解析失败（如自定义布局），
  也能拿 vtable 地址在 IDA 反查 `??_7...` vftable 符号（IDB 里符号齐全）。
