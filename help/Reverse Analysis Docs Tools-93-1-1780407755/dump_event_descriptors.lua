--[[
  Crimson Desert - Event Descriptor Table Dumper
  ================================================
  一次性 dump 整张事件描述符哈希表，输出 desc_id -> 类名/vtable 映射。
  用途：每个新游戏版本只需运行一次，即可拿到所有 desc ID，
        不必再逐个交互动态验证（Take/Search/Gather/Catch 等）。

  用法：
    1. CE 附加 CrimsonDesert.exe
    2. 进游戏到可正常交互的状态（确保描述符表已注册填充）
    3. Memory View -> Tools -> Lua Engine（或主界面 Table -> Show Cheat Table Lua Script）
       粘贴本脚本，执行。结果打印到 Lua Engine 输出框。

  原理（基于 1573 IDB 逆向，sub_1410C1590 / sub_1410C1B30 / sub_1410C1FB0，已验证）：
    描述符管理器是开放寻址哈希表。用 CT 里同款 AOB(patDQ) 动态定位管理器，
    遍历所有桶、所有条目，读 desc_id 和描述符对象，再用对象 vtable 反查 RTTI 类名。

    管理器字段布局（相对 patDQ 解析出的 DESC_MASK 全局，1573 验证）：
      DESC_MASK   = mgr+0x00  (= dword_145C76A0C, patDQ 解析目标)
      bucket_count= mgr+0x04  (dword, = 145C76A10)
      entry_count = mgr+0x08  (dword 低32位, = 145C76A14)
      bucket_array= mgr+0x14  (qword, = 145C76A20)  每桶 256 字节
      value_array = mgr+0x1C  (qword, = 145C76A28)
    桶布局 (256B): [+0]=count(dword), 之后 31 项, 每项 8B: [+8+i*8]=hash, [+0xC+i*8]=slot
    条目: value_array[slot*8] -> entry; entry[+0]=hash, entry[+4]=desc_id(WORD), entry[+8]=描述符对象
    RTTI (标准 MSVC64, 已验证): obj[0]=vftable; vftable[-8]=&COL(_R4);
      COL[+0]=signature(=1); COL[+0xC]=TypeDescriptor 的 image-relative 偏移;
      TypeDescriptor[+0x10]=mangled 名 ".?AV<Class>@pa@@"

  注意：DESC_MASK 由 AOB 动态解析（跨版本稳健），但 +4/+8/+0x14/+0x1C 字段偏移
        与 RTTI 偏移是 1573 静态推得的。若未来版本结构变化，需重新核对这些偏移。
]]

local MODULE = "CrimsonDesert.exe"

-- patDQ: 与 CT 中 AT_DESC_MASK/AT_QUEUE 解析用的同一条 AOB。
-- 命中后 match+5 处是 `mov r8d,[rip+disp]` (7字节, disp 在 +3) -> DESC_MASK 全局地址。
local PAT_DQ = "E8 ?? ?? ?? ?? 44 8B 05 ?? ?? ?? ?? 0F B7 54 24 ?? E8 ?? ?? ?? ?? 4C 8B 25"

local function resolveRIP(insn_addr, insn_len, disp_off)
  local disp = readInteger(insn_addr + disp_off)
  if disp == nil then return nil end
  if disp >= 0x80000000 then disp = disp - 0x100000000 end
  return insn_addr + insn_len + disp
end

local function isUserPtr(a)
  return a ~= nil and a ~= 0 and a < 0x800000000000
end

-- 标准 MSVC64 RTTI 反查；返回 去修饰类名 或 nil。
local function rttiName(objPtr)
  if not isUserPtr(objPtr) then return nil end
  local vtbl = readPointer(objPtr)
  if not isUserPtr(vtbl) then return nil end
  local col = readPointer(vtbl - 8)                -- _R4 CompleteObjectLocator
  if not isUserPtr(col) then return nil end
  if readInteger(col) ~= 1 then return nil end     -- signature 必须为 1 (x64)
  local base = getAddress(MODULE)
  local tdOff = readInteger(col + 0x0C)            -- image-relative TypeDescriptor 偏移
  if not tdOff or tdOff == 0 then return nil end
  local td = base + tdOff
  if not isUserPtr(td) then return nil end
  local s = readString(td + 0x10, 256)             -- mangled 名
  if not s then return nil end
  local clean = s:match("%.%?AV([%w_]+)@") or s:match("%.%?AU([%w_]+)@") or s
  return clean
end

-- ---- 主流程 ----
local base = getAddress(MODULE)
if not base then print("[Dump] 模块未找到: " .. MODULE); return end
local modSize = getModuleSize(MODULE)

local scan = AOBScan(PAT_DQ, "+X-C-W", 0, base, base + modSize)
if not scan or scan.Count == 0 then
  if scan then scan.destroy() end
  print("[Dump] patDQ AOB 未命中 —— 游戏版本可能变化，需重新提取特征码")
  return
end
local m = tonumber("0x" .. scan[0])
scan.destroy()

local descMask = resolveRIP(m + 5, 7, 3)           -- mov r8d,[rip+disp] -> DESC_MASK
if not descMask then print("[Dump] DESC_MASK 解析失败"); return end

local mgr         = descMask
local bucketCount = readInteger(mgr + 0x04)
local entryCount  = readInteger(mgr + 0x08)
local bucketArray = readPointer(mgr + 0x14)
local valueArray  = readPointer(mgr + 0x1C)

print(string.format("[Dump] DESC_MASK=%X bucket_count=%s entry_count=%s bucket_arr=%X value_arr=%X",
      descMask, tostring(bucketCount), tostring(entryCount), bucketArray or 0, valueArray or 0))

if not bucketCount or bucketCount == 0 or not isUserPtr(bucketArray) or not isUserPtr(valueArray) then
  print("[Dump] 表尚未初始化/填充。请在游戏内进入可交互状态后再运行。")
  return
end
if bucketCount > 0x100000 then
  print("[Dump] bucket_count 异常("..bucketCount.."), 疑似偏移解析错误, 已中止以防误读。")
  return
end

local results = {}
for b = 0, bucketCount - 1 do
  local bucket = bucketArray + b * 0x100           -- 每桶 256 字节
  local cnt = readInteger(bucket)
  if cnt and cnt > 0 and cnt <= 31 then            -- 每桶上限 31 项
    for i = 0, cnt - 1 do
      local slot = readInteger(bucket + 0xC + i * 8)   -- [+0xC + i*8] = slot_index
      if slot then
        local entry = readPointer(valueArray + slot * 8)
        if isUserPtr(entry) then
          local descId = readSmallInteger(entry + 4)   -- WORD desc_id
          local obj    = readPointer(entry + 8)        -- 描述符对象
          if descId then
            local vt = isUserPtr(obj) and readPointer(obj) or 0
            table.insert(results, {
              id = descId,
              name = rttiName(obj) or "?",
              obj = obj or 0,
              vtable = vt or 0,
            })
          end
        end
      end
    end
  end
end

table.sort(results, function(a, b) return a.id < b.id end)
print(string.format("[Dump] 共 %d 个描述符:", #results))
print("desc_id | RTTI 类名 | 描述符对象 | vtable(可在 IDA 反查 ??_7 符号)")
for _, r in ipairs(results) do
  print(string.format("0x%04X | %s | %X | %X", r.id, r.name, r.obj, r.vtable))
end

-- 便捷：高亮 AutoLoot 关心的类别
print("\n[Dump] AutoLoot 相关候选 (PickUp/Take/Gather/Catch/Search/Loot/Corpse/Insect/Interact):")
for _, r in ipairs(results) do
  local n = r.name or ""
  if n:find("PickUp") or n:find("Take") or n:find("Gather") or n:find("Catch")
     or n:find("Search") or n:find("Loot") or n:find("Corpse") or n:find("Insect")
     or n:find("Interact") then
    print(string.format("  0x%04X | %s | vtable=%X", r.id, n, r.vtable))
  end
end
