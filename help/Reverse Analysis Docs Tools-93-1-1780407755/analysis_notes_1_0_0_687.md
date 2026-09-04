# Crimson Desert v1.0.0.687 分析记录

> **本文档基于游戏版本 1.0.0.687，所有地址均为该版本。**
> IDB路径: `F:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe.i64`
> v534 分析文档: `analysis_notes_1_0_0_534.md`

## v534 → v687 函数地址映射

| 功能                        | v534          | v687                | 验证方式                   |
| ------------------------- | ------------- | ------------------- | ---------------------- |
| per-entity 处理 (area hook) | sub_1406B2450 | sub_1406B7460       | 手动 define_func + 反编译确认 |
| 摄像头射线后处理                  | sub_1407F1D10 | 0x1407F9550 (未定义函数) | 用户 AOB 特征码匹配           |
| 距离检查 Layer 1 (交互提示)       | sub_141ABCF20 | sub_141ABCF20       | IDA 分析                 |
| 距离检查 Layer 2 (交互执行)       | sub_141AD7830 | sub_141AD7830       | IDA 分析                 |
| 世界坐标获取                    | sub_14130C830 | sub_14130C830       | AOB 匹配确认               |
| 四元数/位置变换                  | sub_1402DE690 | sub_1402DE690       | IDA 分析                 |
| 实体哈希查找                    | sub_14031B3F0 | sub_14031E380       | AOB 匹配确认               |

## ★ v534 偏移在 v687 中的验证结论

### 实体结构偏移：全部不变

| 偏移                  | 含义                          | v687 验证位置                                    |
| ------------------- | --------------------------- | -------------------------------------------- |
| entity+0x60         | entity_id (DWORD)           | 不变                                           |
| entity+0x68         | sub_object 指针               | sub_1406B7460 @ 0x1406b7750                  |
| entity+0x88         | 类型信息指针                      | 0x1407F97B3 (switch `[[entity+0x88]+1]`)     |
| [sub_object+0x20]   | comp（含 dead flag）           | 0x1407F98B9                                  |
| [comp+0x26B]        | dead flag (1=dead, 0=alive) | 0x1407F98B9: `cmp byte ptr [rcx+26Bh], 0`    |
| [sub_object+0x30]   | intermediate                | sub_1402C11A0 反编译确认                          |
| [intermediate+0xC0] | entity_data/item_data       | sub_1402C11A0: `*(_QWORD *)(sub_obj+48)+192` |
| [entity_data+0x08]  | ItemKey (WORD)              | 不变                                           |

### 替代访问路径（v687 per-entity 函数使用）

sub_1406B7460 使用了一条**替代路径**访问 dead flag：

```
[sub_object+0x40]+0x1E9  ≡  [sub_object+0x20]+0x26B
```

关系：`[sub_object+0x40] = [sub_object+0x20] + 0x82`（子结构偏移），所以 `0x82 + 0x1E9 = 0x26B`，指向同一个字节。

**证据**:

- 0x1407F98B9（射线后处理函数内 switch case）: `mov rcx, [rax+20h]; cmp byte ptr [rcx+26Bh], 0` — 原始路径
- sub_1406B7460 @ 0x1406b7754: `mov rcx, [rax+40h]; cmp byte ptr [rcx+1E9h], 0` — 替代路径
- 两条路径功能等价，CT 使用原始路径（+0x20/+0x26B）以保持跨版本兼容

### 实体数组：不变

| 偏移          | 含义       | v687 确认                     |
| ----------- | -------- | --------------------------- |
| [rcx+0x110] | 实体指针数组基址 | sub_1406B7460 @ 0x1406b770f |
| [rcx+0x20]  | 玩家检测实体   | sub_1406B7460 @ 0x1406b7758 |

### 世界坐标获取函数：不变

sub_14130C830 的 AOB 在 v687 中唯一匹配：

```
40 53 48 83 EC 50 48 8B 41 68 48 8B DA 48 8D 4C 24 20 48 8B 90 B0 01 00 00 0F 10 82 98
```

内部使用 `[[entity+0x68]+0x1B0]` transform chain，通过四元数变换 + chunk 坐标合成世界 {X, Y, Z}。

## v687 新增分析：sub_1406B7460（per-entity 函数）

### 基本信息

- 地址: 0x1406B7460（需手动 define_func，IDA 默认未识别）
- 大小: 1602 字节 (0x1406b7460-0x1406b7aa2)
- 参数: rcx=entity_list_obj, edx=entity_index, r8=context
- 调用方式: 多线程，不同线程同时调用不同 index

### 代码结构（a2 >= 4 分支，普通实体路径）

```asm
1406b770c  lea     ebx, [rsi-4]             ; entity_index = a2 - 4
1406b770f  mov     rax, [rcx+110h]          ; entity_array
1406b7716  mov     rsi, [rax+rbx*8]         ; entity = array[index]
...
1406b7750  mov     rax, [rax+68h]           ; sub_object = [entity+0x68]
1406b7754  mov     rcx, [rax+40h]           ; comp_sub = [sub_object+0x40]
1406b7758  mov     rax, [rdi+20h]           ; player = [entity_list_obj+0x20]
1406b775c  test    rax, rax
1406b775f  jz      short skip_player_check
1406b7761  cmp     rax, rsi                 ; 跳过玩家自身
1406b7764  jz      player_branch
skip_player_check:
1406b776a  test    rcx, rcx                 ; comp_sub null check
1406b776d  jz      alive_entity_handler
1406b7773  cmp     byte ptr [rcx+1E9h], 0   ; dead flag via 替代路径
1406b777a  jz      alive_entity_handler     ; 0=alive → generic handler
; --- 尸体路径 ---
1406b7780  mov     rax, [rsi+68h]           ; sub_object
1406b7784  mov     rcx, [rax+1C0h]          ; [sub_object+0x1C0]
...
1406b779c  mov     rdi, [rax+20h]           ; [sub_object+0x20] — timer/cooldown
```

### sub_object 各偏移用途（v687 确认）

| 偏移     | 用途                                             | 访问位置                     |
| ------ | ---------------------------------------------- | ------------------------ |
| +0x20  | comp 基础结构（dead flag 在 +0x26B）                  | 0x1407F98B9, 0x1406b779c |
| +0x30  | intermediate（item_data 在 +0xC0）                | sub_1402C11A0            |
| +0x40  | comp 子结构（= comp_base+0x82, dead flag 在 +0x1E9） | 0x1406b7754              |
| +0x50  | 某个组件（传给 sub_141A6ABB0）                         | 0x1407F9637              |
| +0x78  | 某个列表指针                                         | 0x1406b77fb              |
| +0x88  | 某个组件指针                                         | 0x1406b7847              |
| +0x90  | 某个组件（传给 sub_1407EFBC0）                         | 0x1406b77eb              |
| +0x1C0 | 尸体处理相关组件                                       | 0x1406b7784              |

## v687 新增分析：0x1407F9550（射线后处理）

### 基本信息

- v534 对应: sub_1407F1D10
- v687 地址: 0x1407F9550 - 0x1407F9E29 (0x8D9 = 2265 字节)
- IDA 未自动定义为函数
- 紧接其后是 sub_1407F9E30（另一个独立函数，也有 entity type switch 和 dead flag check）

### entity type switch

```asm
1407f97b0  mov     rdi, [rbx]               ; entity
1407f97b3  mov     rax, [rdi+88h]           ; type_info = [entity+0x88]
1407f97ba  movzx   ecx, byte ptr [rax+1]    ; type = [[entity+0x88]+1]
1407f97be  add     ecx, 0FFFFFFFDh          ; ecx = type - 3
1407f97c1  cmp     ecx, 8                   ; switch 9 cases (types 3-11)
1407f97c4  ja      default_case
```

### dead flag check（在 switch case 内部）

位于跳转表数据区 0x1407F98B1（IDA 显示为数据，实际是代码）:

```
48 8B 47 68          mov rax, [rdi+68h]       ; sub_object
48 8B 48 20          mov rcx, [rax+20h]       ; comp = [sub_object+0x20]
80 B9 6B 02 00 00 00 cmp byte ptr [rcx+26Bh], 0  ; dead flag
0F 85 8B 00 00 00    jnz ...                  ; dead → skip
```

**确认**: v534 的 `[sub_object+0x20]+0x26B` dead flag 检查在 v687 中**完全不变**。

### 其他 sub_object 访问

| 地址          | 代码                                                 | 偏移                               |
| ----------- | -------------------------------------------------- | -------------------------------- |
| 0x1407f95bf | `mov rcx, [rax+20h]; add rcx, 30h`                 | sub_object+0x20 (+0x30 内部偏移)     |
| 0x1407f9637 | `mov rcx, [rax+50h]`                               | sub_object+0x50                  |
| 0x1407f96e9 | `mov rdx, [rax+20h]; cmp byte ptr [rdx+2C0h], 11h` | sub_object+0x20, 检查 +0x2C0==0x11 |
| 0x1407f9b6c | `mov rdx, [rax+40h]`                               | sub_object+0x40                  |

## v687 新增分析：sub_1402C11A0（entity 数据访问验证）

该函数验证了 intermediate chain 在 v687 中不变:

```c
// 反编译代码关键行:
v11 = *(_QWORD *)(*(_QWORD *)(v9[13] + 48LL) + 192LL);
// 展开:
// v9[13] = entity + 13*8 = entity + 0x68 = sub_object
// sub_object + 48(dec) = sub_object + 0x30 = intermediate
// intermediate + 192(dec) = intermediate + 0xC0 = entity_data
```

同函数中还验证了:

- `v9[17]` = entity + 0x88 → `byte [v9[17]+1] == 7` → entity type 检查
- `*(_WORD *)(*(_QWORD *)(v9[13] + 48) + 72)` = `[[sub_object+0x30]+0x48]` → 某个 WORD 字段

## v687 新增分析：世界坐标获取

### sub_14130C830（GetWorldPos）

纯函数，签名: `void GetWorldPos(entity, float* out_xyz)`

内部流程:

```
transform = [[entity+0x68]+0x1B0]
1. 从 transform+0x98 读取 16 字节 (本地坐标 + 四元数相关)
2. 从 transform+0xA8 读取 16 字节
3. 从 transform+0x8C 读取 chunk 坐标 (int16 × 3)
4. 调用 sub_1402DE690 进行四元数变换
5. 合成世界坐标: world = chunk_coord × 1000.0 + local_offset
6. 写入 out_xyz: {X, Y, Z} 各 float32
```

output 布局 (12 字节):

```
+0x00: float X
+0x04: float Y
+0x08: float Z
```

### 距离检查方式

游戏内部的交互距离检查（sub_141ABCF20, sub_141AD7830）使用 XZ 平面 2D 距离。

CT v5.1 的范围过滤使用 **3D 距离**（XYZ 全轴）:

```
dist² = (entity_X - player_X)² + (entity_Y - player_Y)² + (entity_Z - player_Z)²
```

与 range² 比较，超出范围则跳过。

## ★★★ 植物自动采集（v6.0 新增）★★★

### 核心结论

| 发现               | 详情                                                                 |
| ---------------- | ------------------------------------------------------------------ |
| 采集事件格式           | 与 Take **完全相同**（buf_size=D, desc_id=0x0806, buf_dw4=entity_id）   |
| 植物 entity_type   | **7**（与物品相同）                                                       |
| 植物 Camera TypeID | **0xF (15)**                                                       |
| 植物存储位置           | `[entity_list_obj+0x8]` 哈希表中（**不在** +0x110 数组中）                    |
| 植物判定条件           | type=7 + `[intermediate+0xC0]==NULL` + `[intermediate+0xE0]!=NULL` |

### entity_list_obj 完整结构

```
entity_list_obj (rdi in sub_1406B7460)
  +0x08:  QWORD hash_table_ptr     ★ 实体哈希表（包含所有实体，含植物）
  +0x20:  QWORD player_entity      玩家实体
  +0x110: QWORD entity_array_ptr   实体指针数组（sub_1406B7460 使用，不含植物）
  +0x170: QWORD entity_array_2     sub_1406B7260 使用
  +0x178: DWORD count_2
  +0x180: QWORD entity_array_3     sub_1406B7360 使用
  +0x188: DWORD count_3
  +0x198: DWORD count_4
```

**关键**: +0x110 数组只是哈希表的一个子集。植物在哈希表中但不在 +0x110 中。Area 模式需遍历哈希表才能发现植物。

### 实体哈希表结构（sub_14031E380）

```
hash_table + 0x60: DWORD bucket_count
hash_table + 0x64: DWORD total_count
hash_table + 0x70: QWORD bucket_array_ptr   (每桶 256 字节)
hash_table + 0x78: QWORD value_array_ptr

桶结构 (256 字节):
  [+0]:       DWORD entry_count
  [+8+i*8]:   DWORD entity_id
  [+C+i*8]:   DWORD slot_index

value_array[slot_index * 8] → entity_entry:
  [+4]: DWORD entity_id  (验证用)
  [+8]: QWORD entity_ptr (实际实体指针)
```

遍历算法（CT v6.0 area_plant_scan 使用）:

```
for bucket_idx = 0 .. bucket_count-1:
    bucket = bucket_array + bucket_idx * 256
    entry_count = min(dword:[bucket], 30)   // 安全上限
    for i = 0 .. entry_count-1:
        entity_id  = dword:[bucket + 8 + i*8]
        slot_index = dword:[bucket + 0xC + i*8]
        entity_entry = [value_array + slot_index * 8]
        if dword:[entity_entry+4] == entity_id:
            entity = [entity_entry + 8]
            → 对 entity 执行判定 + 距离过滤 + 入队
```

### 植物实体判定（已验证）

对比多个实体字段确定的判定条件：

| 实体类型   | entity_type | itemdata (+0xC0) | chardata (+0xE0) |
| ------ | ----------- | ---------------- | ---------------- |
| 普通物品   | 7           | **非NULL**        | NULL             |
| 植物     | 7           | **NULL**         | **非NULL**        |
| NPC/角色 | 3-6,10,11   | NULL             | 非NULL            |
| 环境/其他  | 各种          | NULL             | NULL             |

**植物判定链**（每步需 NULL + 规范地址检查）：

```
entity_type = [[entity+0x88]+1] == 7
sub_obj = [entity+0x68]              ≠ NULL
intermediate = [sub_obj+0x30]        ≠ NULL
itemdata = [intermediate+0xC0]       == NULL   ← 无物品数据
chardata = [intermediate+0xE0]       ≠ NULL   ← 有角色数据
```

注意: flags5E(0x10)/flagC2(1)/flagCF(1) 对所有实体相同，不可用于区分。

### Camera 模式植物检测

在 camera hook 点 0x140B6BA43：

```
movzx ebx, byte ptr [rax+66h]  ; ★ camera hook 点
```

- `word:[r15+r12+10]` = CameraTypeID

已知交互类型索引值：

| CameraTypeID | 含义            | 事件格式                                        |
| ------------ | ------------- | --------------------------------------------- |
| 1            | Search (搜刮尸体) | desc=0x07E7, buf_size=7                        |
| 4            | Take (拾取物品)   | desc=0x0806, buf_size=0xD                      |
| 0xF (15)     | Gather (采集植物) | desc=0x0806, buf_size=0xD（与 Take 相同）           |
| 0x18 (24)    | Catch (捕捉昆虫)  | desc=0x07FD, buf_size=8（trigger 事件，详见 Catch 章节） |
| 0x23 (35)    | Carry         | 未分析                                           |

### ScopeAttacher 类型层次

```
CommonBaseActor ← sub_1406B0180 使用 ScopeAttacher<CommonBaseActor>
  └→ CommonActor ← sub_141A3A990 使用 ScopeAttacher<CommonActor>
      └→ ClientActor ← sub_1406B7460 使用 ScopeAttacher<ClientActor>
```

sub_1406B7460 中的 ScopeAttacher<ClientActor> 在 0x1406B7721，对非 ClientActor 实体返回失败→跳过。但实测植物根本不在 +0x110 数组中，此过滤不是原因。

### 手动采集的服务端调用栈

在 141FD6100（事件入队）断下：

```
1410067A2  ← 底层事件循环
1410280B3
141FD56D7
141FD5E6A
141FD6524  ← sub_141FD6500
1421517F4
1423745DC  ← sub_1423743A0（交互处理器）
1512F1658  ← sub_1512F1450（实体遍历+处理）
14220C5F6  ← sub_14220C080（交互上下文处理）
1512F1A86  ← sub_1512F1A20
1510B377A
142224EEA  ← sub_142224D60（★ 服务端交互总调度, v687 等价 v534 的 sub_1421FE010）
1515C496C  ← sub_1515C4800（★ 事件构造, buf={00FF0806, eid, FF000101, 00}）
141FD6100  ← sub_141FD6100（事件入队）
```

关键函数：

- **sub_142224D60**: switch on a4 (交互类型), case 0 = Take/Gather → sub_142227980 → sub_1515C4800
- **sub_1515C4800**: 事件构造函数，植物实体在 `[a1+8]`, entity_id = `[[a1+8]+0x60]`

### 植物搜索过程中排除的路径

| 排除的路径                                | 原因                           |
| ------------------------------------ | ---------------------------- |
| entity_list_obj+0x110 数组             | 植物不在此数组中                     |
| entity_list_obj+0x170/0x180 数组       | 断点从未命中                       |
| 摄像头射线系统 (0x1407F9550)                | 植物 EID 未出现                   |
| 交互候选管线 (sub_141A3A990/sub_141A4AF50) | 断点从未命中                       |
| sub_1406B0180 入口                     | 177个 xref 但入口断点不命中（内部代码确实执行） |

### v6.0 CT 实现方案

**Camera 模式**: `check_gather` 分支检测 TypeID=0xF → 跳过价格检查 → 复用 `do_take` 路径

**Area 模式**: `area_plant_scan` 在 entity_index==4 时触发（每帧只扫描一次），遍历哈希表找植物，复用 dedup/burst/distance/enqueue 基础设施

**CT 文件**: `CrimsonDesert_AutoLoot_v6.0.CT`

### CE 汇编注意事项（v6.0 踩坑记录）

| 问题               | 后果                                             | 修复                                         |
| ---------------- | ---------------------------------------------- | ------------------------------------------ |
| `jbe +2`（相对偏移跳转） | CE 将 `+2` 解释为**绝对地址 0x2** → 跳转到地址 2 → DEP 异常崩溃 | 使用 `label` 声明 + `jbe label_name`           |
| 前向引用 label 未声明   | CE 汇编失败                                        | 所有前向引用的 label 必须在顶部用 `label(name)` 声明      |
| 哈希表遍历中桶条目数无上限    | 损坏数据可能导致死循环                                    | `cmp eax, 1E; jbe ok; mov eax, 1E`（cap 30） |

## ★★★ 添加新交互类型的通用指南 ★★★

以植物采集为例，总结添加新自动交互类型（如捕捉昆虫 Catch 等）的完整流程。

### 第一步：确认事件格式

1. 在 `sub_141FD6100`（服务端事件入队）设日志断点
2. 手动执行目标交互，记录事件字段：
   - `buf_size={word:[rdx+68]}` — buffer 大小（**注意：不是消息类型！**）
   - `desc_id={word:[[rdx+70]]}` — 消息描述符 ID（buffer 前 2 字节，真正的消息类型标识）
   - `buf_w0`, `buf_dw4`, `buf_dw8` 等 — buffer 内容
3. 对比已知事件格式（Search: desc=0x07E7/buf_size=7, Take: desc=0x0806/buf_size=0xD, Catch: desc=0x07FD/buf_size=8）
4. 如果格式相同，可复用现有入队代码；如果不同，需新建事件构造代码
5. **⚠️ 注意区分 trigger 事件和 result 事件**：有些交互（如 Catch）使用 trigger → handler → result 两阶段架构，CT 必须创建 trigger 事件，不是 result 事件

### 第二步：确认 Camera TypeID

1. 在 camera hook 点（`movzx ebx, byte ptr [rax+66h]`）设日志断点
2. 面对目标实体，记录 `word:[r15+r12+10]` 的值 = TypeID
3. 添加到 camera cave 的 TypeID dispatch 中

### 第三步：确认实体存储位置

这是最关键也最可能遇到困难的步骤。已知的实体存储：

| 存储位置                     | 包含的实体                       | 访问方式   |
| ------------------------ | --------------------------- | ------ |
| entity_list_obj+0x110 数组 | 物品、NPC、尸体（ClientActor 类型）   | 直接索引   |
| entity_list_obj+0x8 哈希表  | **所有实体**（含植物等非 ClientActor） | 遍历桶+条目 |

**排查路线**（按效率排序）：

1. **先检查 +0x110 数组**: BP@1406B771A（`mov rsi, [rax+rbx*8]` 后），查看目标实体 EID 是否出现
2. **若不在 +0x110，检查哈希表**: BP@14031E380 设 `edx==<目标EID>` 条件，确认是否在哈希表中
3. **若哈希表也没有**: 在事件入队处（141FD6100）设条件断点断下，查看完整调用栈，从上层追踪实体来源

### 第四步：确认实体判定条件

在哈希查找返回处（如 0x1406B0278）设断点，对比目标实体与其他实体的字段差异：

```
bp 1406B0278
bplog 1406B0278, "[Entity] eid={edi} type={byte:[[rax+88]+1]} idata={qword:[[rax+68]+30]+C0]} cdata={qword:[[[rax+68]+30]+E0]}"
bpcnd 1406B0278, "edi==<目标EID> && rax!=0"
```

已知的实体分类模式：

| 分类  | entity_type | itemdata (+0xC0) | chardata (+0xE0) | 示例        |
| --- | ----------- | ---------------- | ---------------- | --------- |
| 物品  | 7           | 非NULL            | NULL             | 掉落物、NPC物品 |
| 植物  | 7           | NULL             | 非NULL            | 草药、矿石？    |
| NPC | 3-6,10,11   | NULL             | 非NULL            | 敌人、商人     |
| 环境  | 各种          | NULL             | NULL             | 装饰物       |
| 昆虫  | **3**       | —                | —                | 蝴蝶、萤火虫等（判定用 comp+0x2C0==5） |

### 第五步：实现

**Camera 模式**（简单）:

- 在 `check_gather:` 后添加 `check_catch:` 分支
- 对比 CameraTypeID，通过后跳到 `do_take` 或新建事件构造路径

**Area 模式**（两种策略）:

| 策略                 | 适用场景                 | 复杂度 |
| ------------------ | -------------------- | --- |
| 在 +0x110 遍历中添加分类分支 | 目标实体在 +0x110 数组中     | 低   |
| 哈希表遍历扫描            | 目标实体不在 +0x110 中（如植物） | 高   |

如果新类型也不在 +0x110 中，可复用 v6.0 的 `area_plant_scan` 框架，修改判定条件即可。

### 研究工具参考

推荐的 x64dbg 断点模板：

```
// 事件入队监控（确认事件格式）
bp 141FD6100
bplog 141FD6100, "[Enqueue] buf_size={word:[rdx+68]} eid={dword:[rdx+50]} desc_id={word:[[rdx+70]]} buf={dword:[[rdx+70]]}"
bpcnd 141FD6100, "0"

// Camera TypeID 监控
bp <camera_hook>
bplog <camera_hook>, "[Camera] CameraTypeId={word:[r15+r12+10]} EID={dword:[r14+180]}"
bpcnd <camera_hook>, "0"

// 哈希表查找监控（特定 EID）
bp 14031E380
bplog 14031E380, "[Hash] eid={edx} table={rcx} r15={r15}"
bpcnd 14031E380, "edx==<target_eid>"

// 哈希查找返回后检查实体字段
bp 1406B0278
bplog 1406B0278, "[Entity] eid={edi} entity={rax} type={byte:[[rax+88]+1]}"
bpcnd 1406B0278, "edi==<target_eid> && rax!=0"
```

## AOB 特征码汇总（v687 验证状态）

| 功能                        | AOB                                                                                      | v687 匹配       |
| ------------------------- | ---------------------------------------------------------------------------------------- | ------------- |
| Hook 点 (camera type-byte) | `0F B6 58 66 80 FB 02 0F 84`                                                             | ✅ CT 动态解析     |
| Merge 点 (after timer)     | `49 8B B6 D0 00 00 00 48 8B 86 D0 00 00 00`                                              | ✅ CT 动态解析     |
| TLS init                  | `48 83 EC 28 BA 9C 00 00 00 65 48 8B 04 25 58 00 00 00`                                  | ✅ CT 动态解析     |
| Descriptor lookup         | `56 45 33 C9 44 0F B7 D2`                                                                | ✅ CT 动态解析     |
| Event allocator           | `48 89 5C 24 ?? 4C 89 44 24 ?? 57 48 83 EC 20 8B FA BA 04 02 00 00`                      | ✅ CT 动态解析     |
| Event enqueue             | `48 89 5C 24 ?? 44 88 4C 24 ?? 57 48 83 EC 20 48 8B 5A 38`                               | ✅ CT 动态解析     |
| Entity hash lookup        | `48 89 5C 24 08 83 79 64 00 44 8B C2 4C 8B D1`                                           | ✅ 0x14031E380 |
| GetWorldPos               | `40 53 48 83 EC 50 48 8B 41 68 48 8B DA 48 8D 4C 24 20 48 8B 90 B0 01 00 00 0F 10 82 98` | ✅ 0x14130C830 |
| Area hook 点               | `55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 50 0F 29 74 24 40 4D 8B E8 8B F2 48 8B F9` | ✅ CT 动态解析     |

## 版本适配备忘

### 已确认不变的偏移

所有 v534 实体结构偏移在 v687 中**完全不变**，包括：

- 实体数组 [rcx+0x110]
- entity_id [entity+0x60]
- sub_object [entity+0x68]
- entity_type [[entity+0x88]+1]
- comp [sub_object+0x20], dead_flag [comp+0x26B]
- intermediate [sub_object+0x30], item_data [intermediate+0xC0]
- 世界坐标 transform [[entity+0x68]+0x1B0]
- 玩家检测实体 [entity_list_obj+0x20]

### 需运行时验证的项目

- 世界/Session 链: `[[AT_WORLD]+0x30]+0x20]+0x60/+0x58`
- 事件描述符 ID: Search=0x07E7, Take=0x0806, Catch_trigger=0x07FD, Catch_result=0x07F5
- 事件结构字段偏移 (+0x30, +0x40, +0x48, +0x50, +0x58, +0x60, +0x68, +0x78)
- Buffer 常量
- AT_LOOT_RANGE 单位标定（游戏内多少单位对应实际距离）

## 区域拾取过滤（v5.2~v5.4）

### `comp+0x2C0` 字段含义（实测确认）

`[[sub_object+0x20]+0x2C0]` 是实体的交互分类字节，不同值代表不同物品状态：

| 值 (hex) | 含义                          | 是否拾取                     |
| ------- | --------------------------- | ------------------------ |
| 0x0F    | NPC 摆放/商店物品                 | **否**                    |
| 0x11    | 世界装饰/固定物品                   | **否**                    |
| 0x16    | 玩家丢弃的物品                     | 可选（AT_NO_PLAYER_DROP 控制） |
| 0x1D    | PickUpItemProcessor 明确拒绝的类型 | **否**（代码层面拒绝）            |
| 其他      | 怪物掉落/可拾取物品                  | **是**                    |

### `intermediate+0x3DA` 字段含义（实测确认）

`[[sub_object+0x30]+0x3DA]` 是物品的所有权/交互条件标志：

| 值 (hex) | 含义      | 是否拾取  |
| ------- | ------- | ----- |
| 0x01    | 不可交互/锁定 | **否** |
| 其他      | 正常      | 是     |

### v5.2/v5.3 尝试但无效的过滤字段

以下检查在 IDA 分析中存在于验证管线中，但实测对异常物品过滤效果不佳：

- 实体类型 `[[entity+0x88]+1] == 7` — 异常物品也是 type 7
- 可拾取标志 `[intermediate+0x281] & 0x87 == 0x01` — 异常物品也通过
- 可见性 `comp[0x31C] == 0` — 异常物品不隐藏
- 状态标志 `comp[0x26E]`, `comp[0x370]`, `comp[0x368|0x36C]` — 无区分效果
- 交互组件 `[sub_object+0x58]` — 对物品实体始终为 NULL

## 已知事件描述符汇总

> **⚠️ 关键纠正**: `[event+0x68]` 是消息 buffer 的大小 (buf_size)，不是消息类型。真正的消息类型标识是 buffer 前 2 字节的 desc_id。事件结构完整字段见 `analysis_notes_1_0_0_534.md` 的事件结构章节。

| desc_id | buf_size | 含义           | Handler 函数            | 备注              |
| ------- | -------- | ------------ | --------------------- | --------------- |
| 0x07E7  | 7        | Search (搜刮)  | 未详细追踪                 | —               |
| 0x07F5  | 11 (0xB) | Catch result | handler@1423690A0     | CatchAction 的输出 |
| 0x07FD  | 8        | Catch trigger | handler@14236AE90    | CatchAction 的输入 |
| 0x0806  | 13 (0xD) | Take/Gather  | 未详细追踪                 | 拾取/采集共用         |

## ★★★ 昆虫自动捕捉（v6.0 Catch 分析）★★★

### 核心发现

| 发现                | 详情                                                                        |
| ----------------- | ------------------------------------------------------------------------- |
| **两阶段事件架构**       | Catch 使用 trigger→handler→result 两阶段模型，CT 必须创建 trigger 事件                   |
| Catch trigger 事件  | desc_id=0x07FD, buf_size=8                                                |
| Catch result 事件   | desc_id=0x07F5, buf_size=11（CatchAction 处理后的输出，不应由 CT 创建）                  |
| 昆虫 CameraTypeID   | **0x18 (24)**                                                             |
| 昆虫 entity_type    | **3**（不同于物品/植物的 7）                                                      |
| 昆虫实体判定            | entity_type==3 + comp+0x2C0==5（catch interaction class）                    |
| 昆虫存储位置            | 在哈希表 `[entity_list_obj+0x8]` 中（不在 +0x110 数组中）                              |

### Catch 两阶段事件架构

手动捕捉昆虫时的完整事件流：

```
1. 上层交互系统创建 trigger 事件 (desc_id=0x07FD, buf_size=8)
     ↓
2. sub_141FD6100 入队
     ↓
3. sub_141FD51F0 出队 → sub_141FD6230 分发
     ↓
4. sub_141FD6500 vtable 派发 → handler@14236AE90 (.didata 段)
     ↓
5. handler@14236AE90 调用 sub_1422AFD80 (CatchAction, 4656 字节)
     ↓
6. CatchAction 处理捕捉逻辑:
   → sub_141FDF780 → sub_1420059C0 (与 Take 共用的底层函数)
     ↓
7. CatchAction 产生 result 事件 (desc_id=0x07F5, buf_size=11) → 入队
```

**CT v6.0 之前的错误**: 创建了 result 事件 (desc_id=0x7F5, buf_size=11)，该事件路由到 handler@1423690A0，不触发 CatchAction。
**修复**: 改为创建 trigger 事件 (desc_id=0x7FD, buf_size=8)，正确触发 CatchAction。

### Catch Handler 调用栈（手动捕捉时）

```
142005D65  ← sub_1420059C0 内部（共用底层）
141FDF7EA  ← sub_141FDF780 内部
1422B0BDA  ← sub_1422AFD80 (CatchAction) 内部
14236AF13  ← .didata handler@14236AE90 → call sub_1422AFD80
141FD6524  ← sub_141FD6500 (vtable dispatch): call [rax+10h]
141FD6439  ← sub_141FD6230 (handler dispatcher)
141FD535B  ← sub_141FD51F0 (dequeue loop)
150EF766B  ← 上层调度
1410280B3
1410067A2  ← 底层事件循环
```

### Trigger 事件 Buffer 格式（8 字节）

```
偏移  大小    值           含义
[0]   WORD   0x07FD       desc_id（消息描述符 ID）
[2]   BYTE   0xFF         flag
[3]   DWORD  target_EID   目标昆虫实体 EID（little-endian）
[7]   BYTE   0x03         sub-type flag
```

CT 构造代码：

```asm
mov rdx, [rax+70]              ; buffer 指针
mov byte ptr [rdx+0], FD       ; desc_id low byte
mov byte ptr [rdx+1], 07       ; desc_id high byte
mov byte ptr [rdx+2], FF       ; flag
mov ecx, [rsp+20]              ; target EID (camera mode) / ebx (area mode)
mov [rdx+3], ecx               ; target EID at buf[3:7]
mov byte ptr [rdx+7], 03       ; sub-type flag
```

### Result 事件 Buffer 格式（11 字节，仅供参考）

```
偏移  大小    值           含义
[0]   WORD   0x07F5       desc_id
[2]   BYTE   0xFF         flag
[3]   DWORD  hash?        某种哈希或标识
[7]   DWORD  target_EID   目标实体 EID
```

### .didata 段的特殊性

Catch handler (`14236AE90`) 位于 `.didata` 段，该段包含**运行时解密的代码**：
- IDA 静态分析无法解码这些函数
- 必须使用 x64dbg 在运行时分析（附加后读取解密后的代码）
- 解密后的代码是标准 x86-64 指令，可正常反汇编

### Catch vs Take 对比

| 维度            | Take (拾取)          | Catch (捕捉)               |
| ------------- | ------------------- | ------------------------- |
| desc_id       | 0x0806              | 0x07FD (trigger)          |
| buf_size      | 13 (0xD)            | 8                         |
| 事件架构          | 单阶段（直接处理）           | 两阶段（trigger → result）     |
| 核心 handler    | 未详细追踪                | sub_1422AFD80 (CatchAction) |
| 底层共用函数        | sub_141FDF780 → sub_1420059C0 | 同左                        |
| CameraTypeID  | 4                   | 0x18 (24)                 |
| entity_type   | 7                   | **3**                       |
| 判定条件          | itemdata!=NULL       | entity_type==3 + comp+0x2C0==5 |
| 实体存储          | +0x110 数组            | 哈希表 (+0x8)                |

### v6.0 CT Catch 实现

**Camera 模式**: `check_catch` 分支检测 TypeID=0x18 → 跳过价格检查 → `do_catch` 构造 trigger 事件

**Area 模式**: 独立的 `area_insect_scan` 遍历哈希表，判定条件为 entity_type==3 + comp+0x2C0==5（与植物判定完全不同），在 `insect_range_ok` 标签下构造 trigger 事件

### 调试 Catch 的 x64dbg 断点模板

```
// Handler 分发监控（追踪哪个 handler 处理事件）
bp 141FD6500
bplog 141FD6500, "[Dispatch] buf_size={word:[rdx+68]} handler_fn={qword:[[rdx+60]]}"
bpcnd 141FD6500, "0"

// CatchAction 入口监控
bp 1422AFD80
bplog 1422AFD80, "[CatchAction] enter rcx={rcx} rdx={rdx}"
bpcnd 1422AFD80, "0"

// Catch trigger 事件入队（desc_id=0x7FD 专用）
bp 141FD6100
bplog 141FD6100, "[Enqueue] buf_size={word:[rdx+68]} desc_id={word:[[rdx+70]]} buf_lo={dword:[[rdx+70]]} buf_hi={dword:[[rdx+70]+4]}"
bpcnd 141FD6100, "word:[[rdx+68]]==8"
```

## ★★★ 植物采集偷窃惩罚修复（v6.1 新增）★★★

### 问题

CT v6.0 的 `Skip AutoLoot Steal Penalty` 仅 patch 了 `sub_142228010`（核心 Take 处理函数）中的偷窃门。但植物自动采集走的是**完全不同的代码路径**，不经过该函数，因此仍然触发偷窃惩罚。

### v534 → v687 偷窃相关函数映射

| 功能 | v534 | v687 |
|------|------|------|
| 偷窃判定函数 | sub_141EB39E0 | sub_141ED63D0 |
| 核心 Take 处理（偷窃门 1） | sub_142201210 | sub_142228010 |
| 偷窃门 2（植物路径） | — | sub_142140470 |
| 贡献值写入 | sub_141B37B80 | sub_141B571F0 |
| 贡献更新 | sub_141B38D60 | sub_141B583F0 |
| 偷窃后果处理 | sub_142320FF3 | sub_14234A950 |

### 偷窃判定函数 `sub_141ED63D0` 的调用者（v687）

| 调用地址 | 所在函数 | 覆盖路径 |
|----------|----------|----------|
| 0x142228281 | sub_142228010 | 物品 Take（偷窃门 1a） |
| 0x1422294B9 | sub_142228010 | 物品 Take（偷窃门 1b，现有 STEAL_BYPASS） |
| 0x1421405D0 | sub_142140470 | **植物 Gather（偷窃门 2，v6.1 新增 STEAL_BYPASS2）** |
| 0x14222326D | sub_142222BD0 | 未分析 |
| 0x142226E82 | sub_142226980 | 未分析 |
| 0x141BD0615 | sub_141BD0280 | 未分析 |

### 植物采集时的偷窃惩罚调用栈（x64dbg 实测）

在贡献值写入函数 `sub_141B571F0` 下断，采集有所有权的植物时断下两次：

**调用栈 1（事件驱动路径）:**

```
sub_141B571F0          ← 贡献值写入
sub_141B583F0 +3CB     ← 贡献更新（调用 sub_141B571F0 @ 0x141b587b6）
sub_14234A950 +263     ← 偷窃后果处理（调用 sub_141B583F0 @ 0x14234abae）
[0x1518F89A8]          ← 偷窃后果事件 handler（Denuvo 加密段）
sub_141FD6500 +24      ← vtable 分发（第二层：偷窃后果事件）
sub_141FD6230 +209     ← handler 分发
sub_141FD6100 +3D      ← 事件入队（内联处理）
sub_1518898F0 +14E     ← 创建偷窃后果事件
[0x15188AA31]          ← Denuvo 加密段
[0x1414FBAC7]          ← Denuvo 加密段
[0x14236B1E4]          ← .didata Take/Gather handler
sub_141FD6500 +24      ← vtable 分发（第一层：Take/Gather 事件）
sub_141FD6230 +209
sub_141FD51F0 +20B     ← 事件出队
```

**调用栈 2（非事件路径）:**

```
sub_141B571F0          ← 贡献值写入
sub_141B583F0 +3CB     ← 贡献更新
[0x1407EBD15]          ← .didata 加密段（IDA 无法分析）
... 全部在加密段中 ...
__scrt_common_main_seh ← 程序入口
```

### 根因分析

| 发现 | 详情 |
|------|------|
| 现有 STEAL_BYPASS 位置 | `sub_142228010`（核心 Take 处理），AOB 匹配 `0x1422294AF` |
| 植物路径是否经过 | **否** — 植物走 `.didata` handler `14236B1E4`，不经过 `sub_142228010` |
| 植物偷窃判定位置 | `sub_142140470` @ `0x1421405D0` 调用 `sub_141ED63D0` |
| 贡献写入函数调用者 | `sub_141B583F0`（偷窃路径）、`sub_14234B0E0`（其他更新） |
| `sub_141B583F0` 调用者 | 仅 `sub_14234A950` 和 `sub_14234BC80`（均为偷窃后果处理） |

### 植物偷窃门（sub_142140470 @ 0x1421405D0）

```asm
1421405CA  mov     r9, r13              ; a4
1421405CD  mov     r8, r12              ; entity (a3)
1421405D0  call    sub_141ED63D0        ; 偷窃判定
1421405D5  test    al, al
1421405D7  jnz     short loc_142140617  ; al=1 → 可偷 → LABEL_28 (返回 true)
1421405D9  xor     bl, bl              ; al=0 → 不可偷 → 返回 false
```

反编译逻辑：

```c
// sub_142140470 内部（植物路径的偷窃检查）
if ( a3 != NULL ) {
    if ( sub_141ED63D0(a1, a2, a3, a4, a5) )  // 偷窃判定
        goto LABEL_28;  // v20 = 1 → 可偷
}
v20 = 0;  // 不可偷
```

**注意**：此处逻辑与 `sub_142228010` 中的偷窃门**相反**：
- `sub_142228010`: `call; test al, al; jz skip_steal` — 补丁将 jz 改为 jmp（永远跳过偷窃路径）
- `sub_142140470`: `call; test al, al; jnz is_stealable` — 补丁将 jnz NOP 掉（永远不标记为可偷）

### v6.1 修复方案

**STEAL_BYPASS2**: NOP `sub_142140470` 中 `0x1421405D7` 的 `jnz`

```
AOB: 4D 8B CD 4D 8B C4 E8 ?? ?? ?? ?? 84 C0 75 3E 32 DB
Patch offset: +0xD（第 14 字节，即 jnz 的操作码）
Enable:  75 3E → 90 90
Disable: 90 90 → 75 3E
```

AOB 在 v687 中唯一匹配 `0x1421405CA`。

### CT v6.1 变更汇总

| 变更 | 详情 |
|------|------|
| 新增 STEAL_BYPASS2 | NOP `sub_142140470` 中植物路径的偷窃判定跳转 |
| Skip Steal Penalty 自动激活 | 激活 Auto Loot 时自动激活偷窃惩罚跳过 |
| 版本号 | v6.0 → v6.1 |
