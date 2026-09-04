--[[
  Crimson Desert AutoLoot - 健康检查 / 版本体检脚本
  ===================================================
  新游戏版本出来时,本脚本几秒出一份体检报告:
    [A] 12 个 AOB 各自匹配状态(✅唯一 / ❌失配 / ⚠️多匹配)
    [B] 关键结构偏移活体验证(用真实实体确认偏移没漂移)
    [C] 事件 desc 表能否正常解析,Take/Search/Catch 的 desc_id 当前值

  ★ 使用顺序(以"主表能否激活"分流):
    1. 先尝试激活主表 CrimsonDesert_AutoLoot_v7.0.CT。
    2a. 激活失败 → 主表未装 hook,此时跑本脚本看 [A]:哪些 AOB ❌失配就是要修的
        (这种场景 [A] 最干净,无 hook 干扰;[B] 会提示缺上下文,正常忽略)。
    2b. 激活成功 → 主表已在运行,进可交互状态后跑本脚本,重点看 [B]/[C]
        (此时相机/area hook 点的原始字节已被跳转桩覆盖,[A] 会把这两项标"✅已hook",不是错)。

  全绿 → CT 可用。有红 → 对照"版本适配流程.md"精准处理对应项。

  运行位置:CE 主界面 Table → Show Cheat Table Lua Script,或 Memory View → Tools → Lua Engine。
]]

local MODULE = "CrimsonDesert.exe"

local base = getAddress(MODULE)
if not base then print("[体检] 模块未找到: " .. MODULE); return end
local modSize = getModuleSize(MODULE)

local function isUserPtr(a)
  return a ~= nil and a ~= 0 and a < 0x800000000000
end

-- 规范地址检查(等价 CT 里的 shr r10,2F 判断:高位为 0)
local function isCanonical(a)
  return isUserPtr(a) and (a >> 47) == 0
end

local function resolveRIP(insn_addr, insn_len, disp_off)
  local disp = readInteger(insn_addr + disp_off)
  if disp == nil then return nil end
  if disp >= 0x80000000 then disp = disp - 0x100000000 end
  return insn_addr + insn_len + disp
end

-- ================================================================
-- [A] AOB 体检(加固版特征码,与 v7.0 CT 一致)
-- ================================================================
local AOBS = {
  { name = "相机 hook",        unique = true,  sym = "AT_HOOK2",      pat = "?? 0F B6 ?? 66 48 8B ?? 48 8B ?? FF 90 08 01 00 00" },
  { name = "merge 点",          unique = true,  pat = "4C 8B ?? D0 00 00 00 49 8B ?? D0 00 00 00" },
  { name = "TLS init",          unique = true,  pat = "48 83 EC 28 BA 9C 00 00 00 65 48 8B 04 25 58 00 00 00 48 8B 08 8B 04 0A 39 05 ?? ?? ?? ?? 7E ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? FF 75 ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 83 C4 28 C3 CC CC CC CC CC CC 40 56 45 33 C9 44 0F B7 D2" },
  { name = "desc lookup",       unique = true,  pat = "40 56 45 33 C9 44 0F B7 D2" },
  { name = "event alloc",       unique = true,  pat = "48 89 5C 24 ?? 4C 89 44 24 ?? 57 48 83 EC 20 8B ?? BA 04 02 00 00" },
  { name = "event enqueue",     unique = true,  pat = "48 89 5C 24 08 57 48 83 EC 20 48 8B ?? 38 65 48 8B 04 25 58 00 00 00" },
  { name = "hash lookup",       unique = true,  pat = "48 89 5C 24 08 83 79 64 00 44 8B ?? 4C 8B ??" },
  { name = "GetWorldPos",       unique = true,  pat = "40 53 48 83 EC 50 48 8B 41 68 48 8B DA 48 8B 88 A0 01 00 00 ?? ?? ?? 81 98 00 00 00" },
  { name = "DESC_MASK/QUEUE",   unique = true,  pat = "E8 ?? ?? ?? ?? 44 8B 05 ?? ?? ?? ?? 0F B7 54 24 ?? E8 ?? ?? ?? ?? 4C 8B 25" },
  { name = "area hook",         unique = true,  sym = "AT_AREA_HOOK",  pat = "55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 50 C5 F8 29 74 24 40 4D 8B ?? 8B ?? 48 8B ??" },
  { name = "ITEM_MGR",          unique = false, pat = "56 57 41 56 48 83 EC 40 0F B7 39 48 8B 1D ?? ?? ?? ?? 3B 7B 08" },
  { name = "STEAL_BYPASS",      unique = true,  sym = "STEAL_BYPASS",  pat = "4D 8B C5 49 8B D7 48 8B 89 20 01 00 00 E8 ?? ?? ?? ?? 84 C0 74 ?? B3 02" },
  { name = "STEAL_BYPASS2",     unique = true,  sym = "STEAL_BYPASS2", pat = "4D 8B CD 4D 8B C4 E8 ?? ?? ?? ?? 84 C0 75 ?? 32 DB" },
}

print("================ [A] AOB 体检 ================")
print("  (主表/子条目激活后,被 hook 或 patch 的点字节会变 → AOB 失配。若对应符号已注册,")
print("   说明该点已被成功定位并接管,标 '✅已接管',非错误。)")
local aobRed = 0
local descMaskMatch = nil

for _, a in ipairs(AOBS) do
  local scan = AOBScan(a.pat, "+X-C-W", 0, base, base + modSize)
  local cnt = (scan and scan.Count) or 0
  local first = (cnt > 0) and ("0x" .. scan[0]) or nil
  if scan then
    if a.name == "DESC_MASK/QUEUE" and cnt >= 1 then descMaskMatch = tonumber(first) end
    scan.destroy()
  end
  -- 该点是否已被激活的 CT 接管(符号已注册)。比读桩字节更可靠:
  -- 相机/area hook 装了 FF25 跳转桩,STEAL_BYPASS 改了 jz/jnz 字节,
  -- 两种都会让 AOB 失配,但只要符号在 = CT 已成功处理该点。
  local taken = a.sym and (getAddressSafe(a.sym) ~= nil)
  local status
  if a.unique then
    if cnt == 1 then status = "✅"
    elseif cnt == 0 then
      if taken then status = "✅已接管(CT 激活中,原始字节被 hook/patch 覆盖,正常)"
      else status = "❌失配"; aobRed = aobRed + 1 end
    else
      -- 多匹配:若该点已接管,通常是桩/patch 制造的额外匹配,降级为提示而非错误
      if taken then status = "✅已接管("..cnt.."处匹配,CT 激活中)"
      else status = "⚠️多匹配("..cnt..")"; aobRed = aobRed + 1 end
    end
  else
    if cnt >= 1 then status = "✅("..cnt.."个,运行时打分)"
    else status = "❌失配"; aobRed = aobRed + 1 end
  end
  print(string.format("  %-18s %s %s", a.name, status, first or ""))
end
print(string.format("[A] 结论: %s", aobRed == 0 and "全部通过" or ("有 "..aobRed.." 项异常,见上方 ❌/⚠️")))
print("  注:'已接管'依赖符号注册。激活失败时该点符号不会注册 → 会如实报 ❌失配。")
print("     要纯净验证全部 AOB(含 hook/steal 原始字节),在主表未激活时跑本脚本。")

-- ================================================================
-- [B] 结构偏移活体验证(需主表已激活以填充上下文)
-- ================================================================
print("\n================ [B] 结构偏移活体验证 ================")
local playerObjSym = getAddressSafe("AT_PLAYER_OBJ")
local hashTblSym   = getAddressSafe("AT_HASH_TABLE")
local playerObj = playerObjSym and readPointer(playerObjSym) or nil
local hashTbl   = hashTblSym and readPointer(hashTblSym) or nil

if not isCanonical(playerObj) and not isCanonical(hashTbl) then
  print("  ⚠️ 未取到上下文缓存。请先激活主表(Auto Loot 区域模式),进游戏后再跑本脚本。")
  print("     (主表的 area hook 会把 entity_list 缓存到 AT_PLAYER_OBJ/AT_HASH_TABLE)")
else
  -- 玩家实体偏移验证
  if isCanonical(playerObj) then
    local eid   = readInteger(playerObj + 0x60)
    local subobj= readPointer(playerObj + 0x68)
    local tinfo = readPointer(playerObj + 0x88)
    local tbyte = isCanonical(tinfo) and readSmallInteger(tinfo + 1) or nil
    print(string.format("  玩家实体 @%X", playerObj))
    print(string.format("    +0x60 EID      = %s %s", eid and string.format("%08X", eid) or "nil",
          (eid and (eid >> 24) >= 0xA0) and "✅(高字节>=A0,EID格式合理)" or "⚠️格式存疑"))
    print(string.format("    +0x68 sub_obj  = %s %s", subobj and string.format("%X", subobj) or "nil",
          isCanonical(subobj) and "✅规范指针" or "❌非规范"))
    print(string.format("    [+0x88]+1 type = %s", tbyte and string.format("%d", tbyte) or "nil"))
    if isCanonical(subobj) then
      local comp = readPointer(subobj + 0x20)
      local inter= readPointer(subobj + 0x30)
      local tf   = readPointer(subobj + 0x1A0)
      print(string.format("    +0x68→+0x20 comp        = %s %s", comp and string.format("%X",comp) or "nil", isCanonical(comp) and "✅" or "❌"))
      -- 注意:intermediate 存物品/植物数据,玩家(type1)等非物品实体本就为 NULL,
      -- 不是错误。intermediate 链的真正验证在下方哈希表抽样里对 type7 物品实体进行。
      print(string.format("    +0x68→+0x30 intermediate= %s %s", inter and string.format("%X",inter) or "nil",
            isCanonical(inter) and "✅" or "(玩家无物品数据,正常)"))
      print(string.format("    +0x68→+0x1A0 transform   = %s %s", tf and string.format("%X",tf) or "nil", isCanonical(tf) and "✅(GetWorldPos用)" or "⚠️"))
    end
  end

  -- 哈希表遍历:抽样验证 comp+0x2C8 取值是否在已知集合
  if isCanonical(hashTbl) then
    local bucketCount = readInteger(hashTbl + 0x60)
    local bucketArr   = readPointer(hashTbl + 0x70)
    local valueArr    = readPointer(hashTbl + 0x78)
    print(string.format("  哈希表 @%X bucket_count=%s", hashTbl, tostring(bucketCount)))
    if bucketCount and bucketCount > 0 and bucketCount <= 0x10000 and isCanonical(bucketArr) and isCanonical(valueArr) then
      local known2C8 = { [0x01]=true,[0x09]=true,[0x0F]=true,[0x11]=true,[0x16]=true }
      local sampled, ok2c8, weird2c8 = 0, 0, 0
      local typeHist = {}
      local itemSeen, itemDataOk, itemDataBad = 0, 0, 0   -- type7 物品的 intermediate 链验证
      for b = 0, math.min(bucketCount, 200) - 1 do
        local bucket = bucketArr + b * 0x100
        local cnt = readInteger(bucket)
        if cnt and cnt > 0 and cnt <= 31 then
          for i = 0, cnt - 1 do
            local slot = readInteger(bucket + 0xC + i * 8)
            local entry = slot and readPointer(valueArr + slot * 8) or nil
            if isCanonical(entry) then
              local ent = readPointer(entry + 8)
              if isCanonical(ent) and sampled < 60 then
                local so = readPointer(ent + 0x68)
                if isCanonical(so) then
                  local comp = readPointer(so + 0x20)
                  local tinfo= readPointer(ent + 0x88)
                  local tb
                  if isCanonical(tinfo) then
                    tb = readSmallInteger(tinfo + 1)
                    typeHist[tb] = (typeHist[tb] or 0) + 1
                  end
                  if isCanonical(comp) then
                    local v = readSmallInteger(comp + 0x2C8) & 0xFF
                    sampled = sampled + 1
                    if known2C8[v] then ok2c8 = ok2c8 + 1
                    elseif v ~= 0 then weird2c8 = weird2c8 + 1 end
                  end
                  -- intermediate 链验证(只对 type7 物品):inter=[so+0x30],
                  -- item_data=[inter+0xC0] 非空时必须是规范指针,否则说明 +0x30/+0xC0 偏移漂移
                  if tb == 7 then
                    local inter = readPointer(so + 0x30)
                    if isCanonical(inter) then
                      itemSeen = itemSeen + 1
                      local idata = readPointer(inter + 0xC0)
                      if idata == 0 then
                        -- 物品也可能 itemdata 为空(如植物:+0xC0 空、+0xE0 非空),不算坏
                      elseif isCanonical(idata) then
                        itemDataOk = itemDataOk + 1
                      else
                        itemDataBad = itemDataBad + 1   -- 非空但非规范 → 偏移可疑
                      end
                    end
                  end
                end
              end
            end
          end
        end
      end
      print(string.format("    抽样 %d 个实体 comp+0x2C8: %d 个落在已知交互类集合{1,9,F,11,16}, %d 个其它非零",
            sampled, ok2c8, weird2c8))
      local th = {}
      for k,v in pairs(typeHist) do table.insert(th, string.format("type%d×%d", k, v)) end
      table.sort(th)
      print("    实体类型分布: " .. table.concat(th, " "))
      if sampled > 0 and ok2c8 == 0 then
        print("    ⚠️ 没有任何实体落在已知 comp+0x2C8 集合 → 该偏移可能漂移,需用 x64dbg 在已知实体上探邻近偏移")
      else
        print("    ✅ comp+0x2C8 偏移看起来正常")
      end
      -- intermediate 链(+0x30→+0xC0)验证结果
      print(string.format("    intermediate 链(type7 物品): 检查 %d 个, item_data 有效 %d, 异常(非空非规范) %d",
            itemSeen, itemDataOk, itemDataBad))
      if itemSeen == 0 then
        print("    ⚠️ 抽样里没有 type7 物品,无法验证 +0x30/+0xC0(附近多刷点可拾取物再跑)")
      elseif itemDataBad > 0 then
        print("    ❌ 有物品 item_data 非空却非规范指针 → +0x30 或 +0xC0 偏移可能漂移")
      elseif itemDataOk > 0 then
        print("    ✅ intermediate(+0x30) 与 item_data(+0xC0) 偏移正常")
      else
        print("    ⚠️ type7 物品的 item_data 都为空(可能都是植物),+0xC0 未实证,但 +0x30 可达")
      end
    else
      print("    ⚠️ 哈希表元数据异常,跳过 2C8 抽样")
    end
  end
end

-- ================================================================
-- [C] 事件 desc 表自检
-- ================================================================
print("\n================ [C] 事件 desc 表自检 ================")
if not descMaskMatch then
  print("  ❌ DESC_MASK/QUEUE AOB 失配,无法解析 desc 表")
else
  local descMask = resolveRIP(descMaskMatch + 5, 7, 3)
  if not isUserPtr(descMask) then
    print("  ❌ DESC_MASK 解析失败")
  else
    local bucketCount = readInteger(descMask + 0x04)
    local bucketArr   = readPointer(descMask + 0x14)
    local valueArr    = readPointer(descMask + 0x1C)
    print(string.format("  DESC_MASK=%X bucket_count=%s", descMask, tostring(bucketCount)))
    if not bucketCount or bucketCount == 0 or not isUserPtr(bucketArr) or not isUserPtr(valueArr) then
      print("  ⚠️ desc 表未填充。进游戏到可交互状态后重试。")
    elseif bucketCount > 0x100000 then
      print("  ❌ bucket_count 异常,desc 表偏移可能变化(需重核 DESC_MASK+4/+0x14/+0x1C)")
    else
      -- 反查目标类名 → desc_id
      local want = {
        ["ProcessPickUpItemOnceTimer"]       = "Take/Gather 拾取/采集",
        ["ProcessLootingDeadDropOnceTimer"]  = "Search 搜刮尸体",
        ["PushCharacterToInventoryOnceTimer"]= "Catch 捕捉昆虫",
      }
      local found = {}
      local function rttiName(obj)
        if not isUserPtr(obj) then return nil end
        local vt = readPointer(obj); if not isUserPtr(vt) then return nil end
        local col = readPointer(vt - 8); if not isUserPtr(col) then return nil end
        if readInteger(col) ~= 1 then return nil end
        local off = readInteger(col + 0x0C); if not off or off == 0 then return nil end
        local td = base + off; if not isUserPtr(td) then return nil end
        local s = readString(td + 0x10, 256); if not s then return nil end
        return s:match("%.%?AV([%w_]+)@") or s
      end
      for b = 0, bucketCount - 1 do
        local bucket = bucketArr + b * 0x100
        local cnt = readInteger(bucket)
        if cnt and cnt > 0 and cnt <= 31 then
          for i = 0, cnt - 1 do
            local slot = readInteger(bucket + 0xC + i * 8)
            local entry = slot and readPointer(valueArr + slot * 8) or nil
            if isUserPtr(entry) then
              local nm = rttiName(readPointer(entry + 8))
              if nm then
                -- 子串匹配:真实类名带 TrocTr 前缀(如 TrocTrProcessPickUpItemOnceTimer),
                -- 用 find 而非精确相等,对前缀变化也稳健。
                for key in pairs(want) do
                  if nm:find(key, 1, true) then
                    found[key] = readSmallInteger(entry + 4)
                  end
                end
              end
            end
          end
        end
      end
      for cls, desc in pairs(want) do
        local id = found[cls]
        if id then
          print(string.format("  ✅ %-34s desc_id=0x%04X  (%s)", cls, id, desc))
        else
          print(string.format("  ❌ 未找到 %s (%s) —— 类名可能变了,跑 dump_event_descriptors.lua 全表查", cls, desc))
        end
      end
      print("  提示:把上面 desc_id 与 CT 内 mov edx,XXXX / buffer 常量核对;不一致则更新 CT。")
    end
  end
end

print("\n================ 体检完成 ================")
print("全绿 → CT 可直接用。有 ❌/⚠️ → 对照 '版本适配流程.md' 处理对应项。")
