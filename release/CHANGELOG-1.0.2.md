# CDAutoLoot 1.0.2

## EN

**Performance.** The scene sweep now runs on the mod's own thread instead of
inside the game's frame — that was the stutter in combat and towns. Blacklisted
objects are no longer re-parsed every tick. `ScanOffThread=0` returns the old
behaviour.

**No longer takes:**

- Memories (`Visione_Chip_*`) — the ones a lantern reveals.
- Abyss progression modules (`AbyssGear`).
- Scenery and vendor tables (`Item_Background_*`).
- Lamps, candles, plates, bowls, cups, pictures, flowerpots, ceramics — under
  `LootFurniture`, off by default.
- Tools: shovel, pickaxe, hoe, felling axe, work hammer — under the new
  `LootTools`, off by default.
- Mechanism containers, a well bucket in particular. `ArmContainers=1` still
  lets the mod make them ready; the contents you take by hand.

**Keys.** `KeyAutoLoot` and `KeyBurst` can be reassigned: F-keys, letters,
digits, numeric keypad, Home/End/Insert/Delete/PageUp/PageDown, Tab, Space,
mouse buttons 3-5.

**Gamepad.** `PadAutoLoot` and `PadBurst`, alongside the keyboard rather than
instead of it. Any XInput controller. Combinations with a plus sign —
`PadAutoLoot=PAD_LB+PAD_RB` — since single buttons are nearly all taken by the
game.

**The ini is re-read while the game runs.** Change a value, wait two seconds.

**Known and unfixed:** chests and storage boxes are not looted; there is no
line-of-sight check, so the mod knows distances but not walls.

---

## RU

**Производительность.** Обход сцены считается в собственном потоке мода, а не
внутри игрового кадра — из-за этого и были подтормаживания в бою и в городе.
Объекты из чёрного списка больше не разбираются заново каждый такт.
`ScanOffThread=0` возвращает прежнее поведение.

**Больше не берёт:**

- Воспоминания (`Visione_Chip_*`) — те, что подсвечиваются фонарём.
- Модули прогрессии Бездны (`AbyssGear`).
- Обстановку и столы торговцев (`Item_Background_*`).
- Лампы, свечи, тарелки, миски, чашки, картины, горшки, керамику — под
  `LootFurniture`, выключен по умолчанию.
- Инструменты: лопату, кирку, мотыгу, топор дровосека, рабочий молот — под
  новым ключом `LootTools`, тоже выключен.
- Ёмкости механизмов, в первую очередь колодезное ведро. `ArmContainers=1`
  по-прежнему позволяет моду подготовить их к использованию, содержимое
  берёте руками.

**Клавиши.** `KeyAutoLoot` и `KeyBurst` переназначаются: функциональные,
буквы, цифры, цифровой блок, Home/End/Insert/Delete/PageUp/PageDown, Tab,
пробел, кнопки мыши 3-5.

**Геймпад.** `PadAutoLoot` и `PadBurst`, вместе с клавиатурой, а не вместо
неё. Любой контроллер XInput. Сочетания через плюс —
`PadAutoLoot=PAD_LB+PAD_RB`, — потому что одиночные кнопки почти все заняты
игрой.

**Ini перечитывается на ходу.** Поправил значение, подождал две секунды.

**Известно и не исправлено:** сундуки и короба не лутаются; проверки прямой
видимости нет — мод знает расстояния, но не стены.
