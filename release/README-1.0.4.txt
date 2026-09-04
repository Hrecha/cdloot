================================================================================
  CDAutoLoot 1.0.4  -  Crimson Desert 2.01.00
================================================================================

Auto-loot for Crimson Desert. Picks up ground items, gathers plants, catches
insects and searches animal carcasses around the player. Settings live in
CDLoot.ini and are re-read while the game runs.

Keys (change them in CDLoot.ini):
  F10   toggle auto-loot
  F11   burst: loot everything in range once

--------------------------------------------------------------------------------
  COMPATIBILITY
--------------------------------------------------------------------------------

Game version   Crimson Desert 2.01.00 (September 4, 2026)
Executable     bin64\CrimsonDesert.exe, file version 1.0.0.2760

1.0.2 does not start on 2.01.00: two of the byte signatures it looks for no
longer match, so it logs "не запускаюсь" and does nothing. 1.0.4 updates the
signatures and changes where the scene scan gets its context, so that it no
longer depends on a byte pattern at all. See CHANGELOG-1.0.4.md.

--------------------------------------------------------------------------------
  INSTALLING
--------------------------------------------------------------------------------

With Definitive Mod Manager (DMM)

  1. Import CDLoot1.0.4.zip (drag it onto the DMM window, or use Import).
     DMM registers it as an ASI add-on: CDLoot.asi plus CDLoot.ini.
  2. Remove or disable any older CDLoot entry so only one is active.
  3. Enable it and Mount. DMM's own ASI loader handles the rest.

Manually

  1. Copy CDLoot.asi and CDLoot.ini into <game>\bin64\
  2. You need an ASI loader in bin64 already (Ultimate ASI Loader as winmm.dll,
     version.dll or dinput8.dll, the same setup other Crimson Desert ASI mods use).

Updating from 1.0.2 or an unreleased 1.0.3

  Replace CDLoot.asi. Keep your CDLoot.ini if you have edited it; no setting
  changed in this release.

Removing

  Delete CDLoot.asi (and CDLoot.ini, CDLoot.log if you want it clean) from
  bin64, or disable and unmount it in DMM.

--------------------------------------------------------------------------------
  CHECKING THAT IT RUNS
--------------------------------------------------------------------------------

bin64\CDLoot.log is rewritten on every launch. A healthy start shows eight
"[aob]" lines with addresses, then "ключевые функции найдены", then four
"[hook] ... пролог перехвачен" lines. If any "[aob]" line says "НЕ НАЙДЕНА",
the game was patched again; send the log.

Known and unchanged: chests and storage boxes are not looted, and there is no
line-of-sight check. If another mod that hooks the same functions is present
(the original AutoLoot.asi, for example), the ownership check may fail to
resolve and "LootOwned" protection is then off.

================================================================================
  RU
================================================================================

Автосбор для Crimson Desert. Подбирает вещи с земли, собирает растения, ловит
насекомых и обыскивает звериные туши вокруг игрока. Настройки в CDLoot.ini,
файл перечитывается на ходу.

Клавиши (меняются в CDLoot.ini): F10 включить/выключить автосбор,
F11 собрать всё в радиусе один раз.

Совместимость: игра 2.01.00 (4 сентября 2026), exe версии 1.0.0.2760.
На 2.01.00 версия 1.0.2 не запускается: две сигнатуры перестали совпадать,
в логе "не запускаюсь". В 1.0.4 сигнатуры обновлены, больше ничего не менялось.

Установка через DMM: перетащите CDLoot1.0.4.zip в окно DMM или импортируйте
его. DMM запишет его как ASI-дополнение (CDLoot.asi и CDLoot.ini). Старую
запись CDLoot отключите, включите новую и нажмите Mount.

Вручную: скопируйте CDLoot.asi и CDLoot.ini в <игра>\bin64\, где уже стоит
ASI-загрузчик (winmm.dll, version.dll или dinput8.dll).

Обновление с 1.0.2: заменить CDLoot.asi, свой CDLoot.ini можно оставить.

Проверка: в bin64\CDLoot.log после запуска восемь строк "[aob]" с адресами,
затем "ключевые функции найдены" и четыре строки "[hook] ... пролог
перехвачен". Если где-то "НЕ НАЙДЕНА", игру снова обновили, пришлите лог.
