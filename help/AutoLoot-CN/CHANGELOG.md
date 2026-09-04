# Changelog

## v0.1.17

- Updated the supported Crimson Desert build timestamp for the June 6, 2026 game update.
- Refreshed the prompt resolver target and expected resolver bytes for the current client.
- Kept prompt text, prompt branch, skip, literal, and call targets unchanged because they still match the current executable.
- Updated release binaries and package.

## v0.1.16

- Updated the supported Crimson Desert build timestamp for the June 5, 2026 game update.
- Updated prompt update, prompt text A/B, prompt branch, skip target, literal target, and call target constants for the current installed game executable.
- Refreshed prompt text and resolver expected bytes for the current client while keeping the existing `bpl` branch-hook behavior.
- Updated release binaries and package.

## v0.1.15

- Updated the supported Crimson Desert build timestamp for the May 30, 2026 game update.
- Updated prompt update, prompt text A/B, prompt branch, skip target, literal target, and call target constants for the current installed game executable.
- Updated the prompt branch hook stub for the current `bpl` interaction-type register allocation.
- Expanded hook regression coverage for the relaxed prompt text signature, backward prompt branch search, and resolver-call layout.
- Updated release binaries and package.

## v0.1.14

- Fixed current-client `type 2` ground equipment prompts so filtered ground loot can pick up visible gloves, armor, helmets, shields, and weapons without enabling broad scene interactions.
- Kept `type 2` scene props guarded by text and category checks so chairs, cooking pots, wall notes, and other non-loot interactions are not auto-triggered.
- Reclassified high-ID equipment rows whose database `slotType` is missing by using internal item-name fallbacks for helmets, chest armor, gloves, boots, cloaks, shields, bows, and weapons.
- Added regression coverage for prompt-gated ground filtering and equipment category fallback.
- Updated release binaries and package.

## v0.1.13

- Updated the supported Crimson Desert build timestamp for the May 22, 2026 game update.
- Updated prompt update, prompt text A/B, prompt branch, skip target, literal target, and call target constants for the current installed game executable.
- Updated prompt text and branch hook stubs for the current compiler register allocation around the prompt owner/context.
- Added the current-client corpse prompt interaction `type 2` to the long-hold corpse loot path.
- Expanded hook regression coverage for prompt update and branch expected bytes.
- Updated release binaries.

## v0.1.12

- Updated the supported Crimson Desert build timestamp for the May 15, 2026 game update.
- Updated prompt update, prompt text A/B, prompt branch, skip target, literal target, and call target constants for the current installed game executable.
- Updated release binaries.

## v0.1.11

- Updated the supported Crimson Desert build timestamp for the May 12, 2026 game update.
- Updated the prompt text A literal target and expected hook bytes for the current installed game executable.
- Updated release binaries.

## v0.1.10

- Fixed skinning after the May 11, 2026 game update by adding observed interaction types `172` and `173` to the long-hold interaction path.
- Added regression coverage for the new skinning interaction IDs.
- Updated release binaries.

## v0.1.9

- Updated the supported Crimson Desert build timestamp and prompt hook RVAs for the May 11, 2026 game update.
- Updated prompt update, prompt text A/B, prompt branch, skip target, literal target, and call target constants for the current installed game executable.
- Added regression coverage for the prompt update hook entry so future game updates catch this hook instead of only validating prompt text hooks.
- Updated release binaries.

## v0.1.8

- Added English and Chinese installation, runtime requirements, update, troubleshooting, and uninstall guides.
- Documented Ultimate ASI Loader v9.7.1 as the recommended ASI loader source.
- Fixed in-game F9/F10 status toast language: English UI users now see English enable/disable and config-window messages.
- Updated release packaging to include the `docs` directory.

## v0.1.7

- Updated the supported Crimson Desert build timestamp and prompt hook RVAs for the May 3, 2026 game update.
- Verified the current prompt text signatures, branch signature, derived return addresses, skip target, prompt literal target, and prompt text call targets against the installed game executable.
- Fixed the prompt text A installation guard so corpse prompt text is captured again after the May 3 update.
- Added current-client skinning interaction `type 171` to the long-press interaction path.
- Stopped short blocked pointer-text matches, such as stale nearby material names, from overriding an allowed weapon/armor/tool numeric category during ground-loot filtering.
- Updated release binaries.

## v0.1.6

- Updated the prompt text and interaction hook addresses for the May 2, 2026 Crimson Desert game update.
- Added observed current-client ground loot interaction `type 5`, fixing prompts that were logged as seen but never triggered.
- Added a local regression test that scans the installed `CrimsonDesert.exe` and verifies hook constants, derived return targets, prompt literal target, and prompt text call targets.
- Fixed PE raw-file-offset to runtime-RVA conversion for current game builds where section raw offsets are not equal to RVAs.
- Updated release binaries.

## v0.1.5

- Fixed configuration UI integer and checkbox reads for UTF-8 BOM INI files.
- Prevented the configuration UI from falling back to default checkbox values and rewriting them when WinAPI cannot read the first INI section.
- Added regression coverage for the shared manual INI fallback path.

## v0.1.4

- Fixed `InteractKey` changes not working for full key names such as `Space`, `Tab`, `Insert`, `Delete`, arrow keys, and function keys.
- Stopped the configuration UI from truncating `InteractKey` to the first character.
- Added automatic Chinese/English configuration UI language detection: Chinese Windows defaults to Chinese, other systems default to English.
- Added a language selector in the configuration UI with `Auto`, `Chinese`, and `English`.
- Added English category labels, UI text, status messages, and English item-name display.
- Added `Language=Auto` to the default configuration.
- Added regression tests for interact-key parsing and UI language switching.

## v0.1.3

- Fixed small animals such as geese and hedgehogs being caught by mistake when corpse looting was enabled.
- Removed animal catch interaction IDs `38` and `39` from the corpse-loot classifier.
- Tightened the ambiguous `type 1` corpse fallback so it only runs after a recent corpse/search prompt action is seen.
- Expanded unsafe prompt-action fallback guards for animal, critter, insect, gather, greeting, skinning, chest, and carry interaction types.
- Updated interaction regression tests and release binaries.

## v0.1.2

- Fixed human NPC corpse looting on current game builds where corpse prompts can be reported as ground interaction `type 1`.
- Changed corpse interaction input from a short tap to a held interact key so "search/loot corpse" prompts complete reliably.
- Added current-client corpse interaction candidates `38`, `39`, and `168` while keeping the original corpse/gather type.
- Added a safe fallback for ambiguous `type 1` corpse prompts without bypassing item filters for confirmed ground items.
- Improved ground item filtering so unknown numeric object matches are not treated as real item IDs.
- Added generic equipment prompt classification for names such as faction-prefix plate helmets, shields, spears, swords, bows, armor, gloves, boots, and cloaks.
- Added regression tests for interaction type handling, corpse key hold behavior, and generic equipment category matching.
- Updated release binaries.

## v0.1.1

- Improved item filtering reliability by requiring confirmed item-name resolution when filters are enabled.
- Added candidate-string resolution before broad memory text scanning.
- Moved expensive ground-item text resolution out of the game hook callback and into the plugin worker thread.
- Reduced repeated text scans for cached ground-item results.
- Fixed `DebugLog=0` so runtime logging can be disabled completely.
- Kept combat-pause / weapon-state probing disabled for stability.

## v0.1.0

- Added Chinese AutoLoot ASI plugin.
- Added Chinese configuration UI.
- Added ground loot and corpse interaction automation.
- Added category-based item filter and per-item block list.
- Added hotkeys for plugin toggle and config panel.
- Removed combat-pause and weapon-state probing logic from the public build.
