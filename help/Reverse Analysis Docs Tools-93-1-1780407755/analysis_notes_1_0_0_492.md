# Crimson Desert v1.0.0.492 分析记录（已归档）

> **本文档为初始探索版本的历史归档。所有可用内容已迁移至 v534 / v687 文档。**
> 当前实现（服务端事件入队方案）见 `analysis_notes_1_0_0_534.md`。

## 历史摘要

v492 阶段探索了以下方案，均已被 v534 的服务端事件入队方案取代：

| 方案 | 结论 |
|------|------|
| Patch 客户端双层距离检查 (jbe→jmp) | 有效但影响所有交互类型，且仍需手动按键 |
| 修改长按计时器阈值为 0 | 有效但仍需手动面对目标 |
| Hook X 键回调 (sub_140B58E70) 自动触发 | 有效但有弯腰动画、打断动作 |

**关键结论**: 客户端 Take 路径的动画和拾取紧耦合，无法只去掉动画。最终采用绕过客户端直接向服务端事件队列入队的方案（v534 实现）。

## CE 脚本通用注意事项（v492 发现，仍适用）

1. `aobscanmodule` 对 Denuvo 保护的可执行页无效，需用 Lua `AOBScan(pattern, "+X-C", 0, lo, hi)` 限定模块地址范围
2. CE auto-assembler 中 `movss xmm0, [symbol]` 不支持 module+offset 寻址，需用寄存器间接: `push rax; mov rax, addr; movss xmm0, [rax]; pop rax`
3. CE 地址表达式中的数字默认十六进制，Lua 中默认十进制
