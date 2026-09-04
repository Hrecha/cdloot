# CDAutoLoot 1.0.4

## EN

**Works on Crimson Desert 2.01.00.** `bin64\CrimsonDesert.exe` file version
1.0.0.2760, 379,781,016 bytes, PE TimeDateStamp 0x6A998DC4 (2026-09-03 15:09:56
UTC). 1.0.2 does not run on this build at all: two required signatures fail to
resolve, `resolve_functions()` returns false, and `CoreStart` logs
`[core] не запускаюсь` and returns 0. The mod loads and does nothing.

**Built from the repo source at 1.0.2**, commit 85e99af. The number skips to
1.0.4 because a 1.0.3 build exists that was never pushed here and carries no
changelog, so nothing from it is included and none of it has been merged. If
1.0.3 changed anything below, that has to be reconciled by hand.

**Signatures.** Four of the eight patterns stopped matching. Old addresses come
from a 2.00.02 session log, new ones from 2760.

| Pattern | 2.00.02 | 2.01.00 | What changed |
| --- | --- | --- | --- |
| tls+desc | +0x12256F0 | +0x13AB990 | last byte of the `desc_lookup` prologue |
| alloc_event | +0x1223770 | +0x13A9790 | nothing, moved only |
| enqueue | +0x236A030 | +0x26B0F30 | nothing, moved only |
| area_sweep | +0x75D780 | see below | register allocation after the prologue |
| own_check | +0x2236670 | +0x251BA50 | one register byte, plus ambiguity |
| get_pos | +0x15A2BE0 | +0x17630D0 | nothing, moved only |
| desc_mask+queue | +0x236B193 | +0x26B21C3 | nothing, moved only |
| node arm | +0x1DE1350 | +0x205CD60 | recompiled, new prologue |

**tls+desc.** The pattern ends with the `desc_lookup` prologue that sits 0x60
bytes after `tls_init`. Its last byte is the register in `movzx r??d,dx`: 0xD2
(r10d) before, 0xDA (r11d) on 2760. That byte is now a wildcard.

**own_check.** `4C 8B FA` (mov r15,rdx) became `4C 8B F2` (mov r14,rdx).
Wildcarding the register bytes alone leaves three matching prologues, so the
pattern now runs on to `0F B6 7D 50`, the read of the fifth argument from
`[rbp+0x50]`. That is the constant 7 the caller passes, and it makes the match
unique again.

**Node arming.** The dispatcher was rebuilt rather than moved. The old prologue
(`48 89 5C 24 18 88 54 24 10 55 56 57 41 54 ...`) is gone; the function now
starts `88 54 24 10 48 89 4C 24 08 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 58`.
The hook takes 15 bytes instead of 16, which lands on an instruction boundary:
4 + 5 + 4 + 2. Taking 16 would split `41 55` (push r13) in half. The virtual
call inside it shifted from slots 0x830/0x838 to 0x838/0x840, call site
+0x205D0F3, so a method was inserted in that vtable. `DESC_MASK` resolves to
+0x6933660 and the event queue to +0x6C29C50.

**The scene sweep, and why a unique signature was still wrong.** The old
`area_sweep` pattern matched nothing on 2760. A relaxed version, with the tail
changed from `4D 8B ?? 44 8B ?? 48 8B ??` to `4C 8B ?? 48 8B ?? 48 8B ?? 44 8B`,
matched exactly one address: +0x379436F, function start +0x3794360. That
function has the right shape. It keeps a count at `[rcx+0x108]` and an array at
`[rcx+0x110]`, the game calls it on the game thread several thousand times a
second, and hooking it is stable. It is still the wrong function, because the
object arriving in `rcx` is not the scene context. The log looked healthy:
eight signatures unique, four hooks installed, five of five event descriptors
resolved, sending permitted. The mod then found no player and collected nothing,
reporting only `[игрок] сущности с меткой 0xA0 в контексте нет` under F9. A
unique match is not a correct one, and the address should have been the tell:
every other function in this patch moved by 1.8 to 2.9 MB, while that one
appeared to move 50 MB.

**The scan no longer takes its context from the hooked function.** It resolves
`ClientActorManager` by RTTI class name, which comes from the game's own symbols
and survives patches:

```
".?AVClientActorManager@pa@@"     name string, TypeDescriptor is name - 0x10
  -> TypeDescriptor  +0x6981370
  -> COL             +0x5CAD518   (signature 1 at +0x00, td RVA at +0x0C)
  -> vtable          +0x54A4440   (the qword before it points at the COL)
  -> global          +0x6C29FC8   (holds an object whose first qword is that vtable)
```

There is exactly one instance of the class in the process, and three globals
point at it (+0x6C29FC8, +0x6C9A030, +0x6C9A170). Its entity list sits at
manager+0x190 as the usual `{count, capacity, array}` triple, so the existing
0x100..0x200 probe in `area_body` finds it with no change.

**Three details that matter if this needs revisiting.** The manager does not
exist when the mod loads, because the world is built well after `CoreStart`, so
resolving once at startup always fails; the lookup retries every three seconds
until it succeeds, while the class and vtable half is done once since that data
does not move. The global scan covers writable non-executable sections only,
which is `.srdata` at 8.3 MB, because scanning the whole image would include
`.debug$P` at 246 MB marked both writable and executable. The scan does not call
`readable()` per candidate: `.srdata` holds 146,131 pointer-shaped values and
`VirtualQuery` measures at about 300 microseconds in this process, roughly 44
seconds per attempt, and the read is already covered by an exception handler.
The section walk does use `VirtualQuery` once per committed sub-range, because
`.srdata` has a virtual size of 0x84B000 against 0x359800 of file data and the
tail would otherwise fault mid-loop.

**The old hook is still installed.** It supplies the game thread id and drives
`drain_pending`, which is all it is used for now. Replacing it with the real
per-frame function is worth doing, and the context no longer depends on it.

**The load banner is in English again.** The text shown when the mod finishes
loading was hardcoded Russian (`hud_set("CDLoot готов")`) and was the one
player-visible string that missed the `T(en, ru)` macro, so an English install
was greeted in Russian. It now follows the `Language` key like everything else.

**F10 and F11 show their own message again.** `notice()` stored the message and
turned the banner on, but the `hud_set()` that paints it lived at the end of
`area_body`, past the early returns for an unreadable context and for the player
not being found. Pressing a key made the window appear immediately with whatever
was written last, usually the startup line, so the mod looked like it was
ignoring the key. `notice()` now writes its own text, and the banner expires in
the worker loop, so neither depends on the sweep completing. `g_hudCs` is
initialized before the worker thread starts, since the worker can now reach
`hud_set` on its first pass.

**Diagnostics, under `Debug=1`.** F9 dumps the context when no player is found:
the context pointer, its RTTI class if it has one, every
`{count, capacity, array}` triple in 0x100..0x200, and the class name plus id
bytes of the first entries in each. Class names are stable across versions, so
this separates "wrong object" from "entity layout moved" in one keypress. The
`[авто]` line now prints `род=` (parent id), `кат=` and `к2=` alongside the
existing fields, so it is possible to tell what an object is attached to at the
moment it is taken. A `[частота]` line every five seconds reports how often each
hook fires, how many auto-loot ticks ran, how many candidates were examined and
how many events were sent.

**Verified unchanged on 2760.** Event descriptor ids and sizes are identical,
resolved by class name at runtime: 0x07E8/7
`TrocTrProcessLootingDeadDropOnceTimer` at +0x6944780, 0x0800/8
`TrocTrPushCharacterToInventoryOnceTimer` at +0x6945470, 0x0809/13
`TrocTrProcessPickUpItemOnceTimer` at +0x6945980, 0x080D/3
`TrocTrInteractionDoStepDoInteractionOnceTimer` at +0x6945BC0, 0x0813/16
`TrocTrStealItemByFrameEventOnceTimer` at +0x6945F20. Entity layout is intact:
`ent+0x68` to `sub+0x1A0` still holds in `get_pos`, the entity id is still at
`+0x60` with 0xA0 for the player and 0xB0 for world objects, and the player
route is still 0x90100000 at `+0x90`.

**How to verify.** Point `EXE` in `sigcheck.py` at the 2760 executable and run
it; all eight patterns should report `уникально`. `descmap.py` and
`descdump.py` confirm the descriptor ids above without launching the game. In
game, `CDLoot.log` should show eight `[aob]` lines with addresses followed by
`ключевые функции найдены`, four `[hook] ... пролог перехвачен` lines, then
`[тест] ПОРЯДОК: дескрипторов 5 из 5, отправка разрешена`. Within a few seconds
of the world loading there should be
`[менеджер] найден: глобал +0x6C29FC8 -> ...`, and F9 should print
`[игрок] eid=A0100001 route=90100000 ... позиция (...)` followed by the object
list rather than the missing-player line.

**Duplication was checked and did not happen.** Two F9 inventory dumps taken
before and after a collection run were diffed by instance id: 58 events produced
15 new instances, with no instance appearing twice and none disappearing. World
objects are consumed on pickup. The bag dump lists instances and not quantities,
so stackable items such as potatoes merge into an existing instance and cannot
be counted this way.

**Known and unfixed:**

- Collection arrives in bursts rather than a steady stream. The scan runs at the
  configured rate, 24 to 25 ticks per five seconds at `MaxLootsPerSec=5`, and
  examines hundreds of candidates per tick, but sends nothing most of the time.
  The gate is `ArmRange=8` against `ItemRange=15` and `GatherRange=20`: an
  object is only takeable once its node has been armed, so candidates wait until
  the player walks within 8 metres, then a cluster arms and is collected at
  once. Raising `ArmRange` smooths it out at the documented cost of collecting
  through walls.
- The mod takes everything inside its radius, not only what the player was
  looking at. Breaking a pot in a kitchen also collects the loose vegetables
  already lying around the room. That is `ItemRange` and `GatherRange` working
  as configured.
- Chests and storage boxes are still not looted, and there is still no
  line-of-sight check.
- The parent check rejects an object only when its parent is the player.
  Anything parented to a container or another character passes. That did not
  cause trouble in testing, where collected items reported `род=00000000`, but
  the gap is real.

---

## RU

**Работает на Crimson Desert 2.01.00.** `bin64\CrimsonDesert.exe` версии
1.0.0.2760, 379 781 016 байт, PE TimeDateStamp 0x6A998DC4 (2026-09-03 15:09:56
UTC). Версия 1.0.2 на этой сборке не работает вовсе: две обязательные сигнатуры
не находятся, `resolve_functions()` возвращает false, `CoreStart` пишет
`[core] не запускаюсь` и возвращает 0. Мод загружается и ничего не делает.

**Собрано из исходников репозитория версии 1.0.2**, коммит 85e99af. Номер
перескакивает на 1.0.4, потому что сборка 1.0.3 существует, но сюда не
выкладывалась и changelog к ней нет: ничего из неё не взято и не слито. Если в
1.0.3 менялось что-то из описанного ниже, это придётся свести вручную.

**Сигнатуры.** Четыре шаблона из восьми перестали совпадать. Старые адреса взяты
из лога сессии на 2.00.02, новые с 2760.

| Шаблон | 2.00.02 | 2.01.00 | Что изменилось |
| --- | --- | --- | --- |
| tls+desc | +0x12256F0 | +0x13AB990 | последний байт пролога `desc_lookup` |
| alloc_event | +0x1223770 | +0x13A9790 | ничего, только переехала |
| enqueue | +0x236A030 | +0x26B0F30 | ничего, только переехала |
| area_sweep | +0x75D780 | ниже | раскладка регистров после пролога |
| own_check | +0x2236670 | +0x251BA50 | один байт регистра и неоднозначность |
| get_pos | +0x15A2BE0 | +0x17630D0 | ничего, только переехала |
| desc_mask+queue | +0x236B193 | +0x26B21C3 | ничего, только переехала |
| зарядка узла | +0x1DE1350 | +0x205CD60 | пересобрана, новый пролог |

**tls+desc.** Хвост шаблона это пролог `desc_lookup`, лежащего через 0x60 после
`tls_init`. Последний байт там задаёт регистр в `movzx r??d,dx`: было 0xD2
(r10d), на 2760 стало 0xDA (r11d). Теперь этот байт подстановочный.

**own_check.** Инструкция `4C 8B FA` (mov r15,rdx) стала `4C 8B F2`
(mov r14,rdx). Одних подстановочных регистров мало: таких прологов три. Поэтому
шаблон продлён до `0F B6 7D 50`, чтения пятого аргумента из `[rbp+0x50]`. Это та
самая семёрка, которую передаёт вызывающий, и с ней совпадение снова
единственное.

**Зарядка узла.** Диспетчер не переехал, а был пересобран. Прежнего пролога
(`48 89 5C 24 18 88 54 24 10 55 56 57 41 54 ...`) нет; функция начинается с
`88 54 24 10 48 89 4C 24 08 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 58`.
Перехват забирает 15 байт вместо 16, и это ровно по границе инструкций:
4 + 5 + 4 + 2. На 16 разрезалось бы пополам `41 55` (push r13). Виртуальный
вызов внутри съехал со слотов 0x830/0x838 на 0x838/0x840, место вызова
+0x205D0F3: в таблицу добавили метод. `DESC_MASK` находится по +0x6933660,
очередь событий по +0x6C29C50.

**Обход сцены, и почему уникальная сигнатура всё равно оказалась не той.** Старый
шаблон `area_sweep` на 2760 не совпал ни с чем. Ослабленный вариант, у которого
хвост `4D 8B ?? 44 8B ?? 48 8B ??` заменён на
`4C 8B ?? 48 8B ?? 48 8B ?? 44 8B`, совпал ровно с одним адресом: +0x379436F,
начало функции +0x3794360. Форма у неё подходящая: счётчик по `[rcx+0x108]`,
массив по `[rcx+0x110]`, игра зовёт её на игровом потоке несколько тысяч раз в
секунду, перехват встаёт устойчиво. Функция всё равно не та, потому что в `rcx`
приходит не контекст сцены. Лог при этом выглядел здоровым: восемь сигнатур
уникальны, четыре перехвата встали, пять из пяти дескрипторов разобраны,
отправка разрешена. Игрок не находился, не собиралось ничего, и по F9 в логе
была единственная строка `[игрок] сущности с меткой 0xA0 в контексте нет`.
Уникальное совпадение не значит верное, и подсказкой был адрес: все остальные
функции в этом патче сместились на 1.8-2.9 МБ, а эта будто бы на 50 МБ.

**Контекст обхода больше не берётся из аргумента перехваченной функции.** Мод
находит `ClientActorManager` по имени класса в RTTI: имена приходят из
исходников игры и переживают патчи.

```
".?AVClientActorManager@pa@@"     строка имени, TypeDescriptor это имя - 0x10
  -> TypeDescriptor  +0x6981370
  -> COL             +0x5CAD518   (сигнатура 1 по +0x00, RVA дескриптора по +0x0C)
  -> таблица методов +0x54A4440   (перед ней лежит указатель на COL)
  -> глобал          +0x6C29FC8   (в нём объект, у которого в начале эта таблица)
```

Экземпляр класса в процессе ровно один, и на него указывают три глобала
(+0x6C29FC8, +0x6C9A030, +0x6C9A170). Список сущностей лежит по менеджер+0x190
обычной тройкой `{штук, мест, массив}`, поэтому прежний перебор 0x100..0x200 в
`area_body` находит его без изменений.

**Три подробности на случай, если к этому придётся вернуться.** Менеджера при
загрузке мода ещё нет, мир строится заметно позже `CoreStart`, поэтому разовый
поиск при старте всегда впустую: поиск повторяется раз в три секунды до успеха,
а часть с классом и таблицей методов делается один раз, эти данные не двигаются.
Перебор глобалов идёт только по записываемым секциям без кода, то есть по
`.srdata` на 8.3 МБ, потому что перебор всего образа захватил бы `.debug$P` на
246 МБ, помеченную и на запись, и на исполнение. В переборе нет вызова
`readable()` на каждого кандидата: в `.srdata` 146 131 значение, похожее на
указатель, `VirtualQuery` в этом процессе занимает около 300 микросекунд, то
есть примерно 44 секунды на попытку, а чтение и так укрыто перехватом
исключения. Сам обход секции `VirtualQuery` использует, но один раз на отданный
кусок: у `.srdata` виртуальный размер 0x84B000 против 0x359800 данных из файла,
и на хвосте перебор иначе упирается в незанятую страницу.

**Старый перехват остался на месте.** Он даёт идентификатор игрового потока и
вызывает `drain_pending`, и только для этого теперь и нужен. Заменить его на
настоящую покадровую функцию стоит, но контекст от него больше не зависит.

**Табличка загрузки снова на английском.** Текст, который показывается после
загрузки мода, был зашит по-русски (`hud_set("CDLoot готов")`) и оказался
единственной видимой игроку строкой мимо макроса `T(en, ru)`: на английской
системе мод здоровался по-русски. Теперь строка слушается ключа `Language`, как
и всё остальное.

**F10 и F11 снова показывают своё сообщение.** `notice()` складывал сообщение и
включал табличку, а рисующий её `hud_set()` стоял в конце `area_body`, после
ранних выходов по нечитаемому контексту и по ненайденному игроку. По нажатию
окно появлялось сразу, но со строкой, записанной последней, обычно со
стартовой, и выглядело это так, будто клавишу не заметили. Теперь `notice()`
пишет свой текст сам, а гаснет табличка в рабочем потоке, и ни то, ни другое не
ждёт конца обхода. `g_hudCs` создаётся до запуска рабочего потока, потому что
тот с первого же оборота может дойти до `hud_set`.

**Разведка, при `Debug=1`.** F9 выкладывает контекст, если игрок не нашёлся:
указатель, имя класса из RTTI, если оно есть, все тройки `{штук, мест, массив}`
в 0x100..0x200 и имена классов с байтами идентификатора у первых элементов
каждого массива. Имена классов между версиями не меняются, поэтому одно нажатие
отличает "не тот объект" от "уехала раскладка сущности". В строке `[авто]`
теперь печатаются `род=` (родитель), `кат=` и `к2=` рядом с прежними полями, так
что видно, к чему прицеплен объект в момент взятия. Строка `[частота]` раз в
пять секунд показывает, как часто срабатывает каждый перехват, сколько было
тактов автосбора, сколько кандидатов разобрано и сколько событий отправлено.

**Проверено и не менялось на 2760.** Номера и размеры дескрипторов событий те
же, разбираются по именам классов на ходу: 0x07E8/7
`TrocTrProcessLootingDeadDropOnceTimer` по +0x6944780, 0x0800/8
`TrocTrPushCharacterToInventoryOnceTimer` по +0x6945470, 0x0809/13
`TrocTrProcessPickUpItemOnceTimer` по +0x6945980, 0x080D/3
`TrocTrInteractionDoStepDoInteractionOnceTimer` по +0x6945BC0, 0x0813/16
`TrocTrStealItemByFrameEventOnceTimer` по +0x6945F20. Раскладка сущности цела:
связка `ent+0x68` и `sub+0x1A0` в `get_pos` работает, идентификатор по-прежнему
по `+0x60` с меткой 0xA0 у игрока и 0xB0 у мира, route игрока по-прежнему
0x90100000 по `+0x90`.

**Как проверить.** Направьте `EXE` в `sigcheck.py` на исполняемый файл 2760 и
запустите: все восемь шаблонов должны показать `уникально`. `descmap.py` и
`descdump.py` подтверждают номера дескрипторов выше без запуска игры. В игре в
`CDLoot.log` должны быть восемь строк `[aob]` с адресами и следом
`ключевые функции найдены`, четыре строки `[hook] ... пролог перехвачен`, затем
`[тест] ПОРЯДОК: дескрипторов 5 из 5, отправка разрешена`. Через несколько
секунд после загрузки мира должна появиться строка
`[менеджер] найден: глобал +0x6C29FC8 -> ...`, а по F9 в логе
`[игрок] eid=A0100001 route=90100000 ... позиция (...)` и список объектов вместо
строки о ненайденном игроке.

**Дублирование проверялось и не подтвердилось.** Два дампа сумки по F9, до и
после сбора, сравнивались по идентификаторам экземпляров: 58 событий дали 15
новых экземпляров, ни один не появился дважды и ни один не пропал. Объект в мире
после подбора исчезает. Дамп сумки перечисляет экземпляры, а не количества, поэтому
складываемые предметы вроде картошки попадают в уже существующий экземпляр, и так
их не сосчитать.

**Известно и не исправлено:**

- Сбор идёт всплесками, а не ровным потоком. Обход работает на заданной частоте,
  24-25 тактов за пять секунд при `MaxLootsPerSec=5`, разбирает сотни кандидатов
  за такт и почти всё время не отправляет ничего. Держит `ArmRange=8` против
  `ItemRange=15` и `GatherRange=20`: объект берётся только после зарядки узла,
  поэтому кандидаты ждут, пока игрок подойдёт на 8 метров, а потом целая пачка
  заряжается и собирается разом. Увеличение `ArmRange` это сглаживает, платой
  идёт описанный сбор сквозь стены.
- Мод берёт всё в радиусе, а не то, на что игрок смотрел. Разбитый в кухне
  горшок означает, что заодно уедет и разложенная по комнате снедь. Так и
  настроено ключами `ItemRange` и `GatherRange`.
- Сундуки и короба по-прежнему не лутаются, проверки прямой видимости
  по-прежнему нет.
- Проверка родителя отсекает объект только тогда, когда родитель это игрок. Всё,
  что подвешено к ёмкости или к чужому актору, её проходит. На тестах это не
  навредило, у собранных предметов стояло `род=00000000`, но брешь настоящая.
