================================================================================
  PrivateStorageAnywherePLUS  -  Crimson Desert 2.00
  Based on Private Storage Anywhere v1.5.10
================================================================================

Open your storage from anywhere in the world, without walking back to camp.

Six panels on F4 to F9. Private Storage can optionally be given a larger
capacity. The five housing chests are expanded to 1000 slots each.

This is a compatibility build for Crimson Desert 2.00. The original v1.5.10
release does not run on 2.00 - it crashes about a second after loading, even
with every feature switched off. See CHANGELOG-2.00.txt for what was wrong.


--------------------------------------------------------------------------------
  COMPATIBILITY
--------------------------------------------------------------------------------

Game version   Crimson Desert 2.00
Executable     bin64\CrimsonDesert.exe, SizeOfImage 0x16499000

This build derives every game address it needs from the executable at build
time. It is matched to 2.00 and has been tested only on 2.00.

It will NOT work on 1.18.2 or earlier. Use the 1.18.2 release for those.

A future game patch will almost certainly move things again. If that happens
the mod logs what it could not find rather than crashing blindly - send the log.


--------------------------------------------------------------------------------
  INSTALLING
--------------------------------------------------------------------------------

With DMM (recommended)

  1. Import PrivateStorageAnywherePLUS-CD-2.00-FINAL.zip as a mod.
  2. IMPORTANT: disable or remove every other Private Storage Anywhere entry.
     Only one may be active at a time. Two copies will fight over the same
     hooks and the result is undefined.
  3. Enable this one and launch the game.

Manually

  1. Copy PrivateStorageAnywhere.asi and PrivateStorageAnywhere.ini into
       <game>\bin64\
  2. Make sure no other PrivateStorageAnywhere.asi is present in that folder.
  3. You need an ASI loader already installed (the usual dinput8/ultimate-asi
     setup that other Crimson Desert ASI mods use).

Updating from an earlier build

  Replace both files. Keep your own PrivateStorageAnywhere.ini if you have
  edited it - nothing in the settings changed for this release. If you are
  unsure, take the new INI; the default is the safe one.

Removing

  Delete PrivateStorageAnywhere.asi (and the .ini and .log if you want a clean
  folder), or disable the entry in DMM.

  Nothing is written to your save. Everything this mod changes lives in memory
  and is gone when the game closes. Removing it cannot corrupt a save.


--------------------------------------------------------------------------------
  CONTROLS
--------------------------------------------------------------------------------

  F4    Private Storage
  F5    Gatherables Chest
  F6    Dresser
  F7    Refrigerator
  F8    Symbol Storage
  F9    Collecting Storage

  F11   Reload PrivateStorageAnywhere.ini without restarting the game

Each key toggles: press it to open that panel, press it again to close. To
switch from one chest to another, close the first, then press the second key.

Open from normal gameplay - walking around, no menu open, not in a cutscene and
not loading. If the game is not in a state where a storage panel can safely be
mounted, the mod refuses and writes a BLOCKED line to the log rather than
forcing it.

All six keys are configurable in the INI under "Panel Bindings", along with
controller bindings. The reload key itself is ReloadKey (default 7A = F11).

Known cosmetic issue: the startup banner in the log says "Press F6 to open
Private Storage". It is wrong - Private Storage is F4. That string is inherited
from the original mod and changing it would mean changing a binary that has
been tested as-is.


--------------------------------------------------------------------------------
  CAPACITY - TWO SEPARATE FEATURES
--------------------------------------------------------------------------------

These are independent. Read both; they are often confused.


1. The five housing chests (F5-F9) - FIXED at 1000 slots

   Gatherables, Dresser, Refrigerator, Symbol and Collecting are expanded from
   their default 10 slots to 1000 slots each.

   This is automatic, always on, and has no setting. It is applied once when
   the game finishes loading its data.

   It is NOT affected by PrivateStorageSlots. Setting PrivateStorageSlots to 0
   does not reduce these chests. The expansion runs before that setting is even
   read, on a separate code path.


2. Private Storage (F4) - OPTIONAL, configurable

   Controlled by PrivateStorageSlots in the [Settings] section of the INI.

     PrivateStorageSlots=0        <- the default

       Leave Private Storage exactly as the game intends: your base capacity
       plus whatever expansion slots you have bought. If the mod had previously
       raised it in this session, this puts it back.

     PrivateStorageSlots=1000

       Make the TOTAL exactly 1000, expansions included. If you have bought 200
       expansion slots you still end up with 1000, not 1200. Any number works;
       1000 is the game's own ceiling for this container.

   The setting is re-read every time you open a panel, so you can edit the INI
   while the game is running and just close and reopen the panel. F11 also
   works.

   PrivateStorageExpansions is a manual override for the number of expansion
   slots you have bought. Leave it at -1; the mod works it out itself. It only
   exists in case the automatic figure is ever wrong.

   Note that the game raises Private Storage to 1000 on its own near the end of
   the story, so this setting is mainly useful if you want the space early.


--------------------------------------------------------------------------------
  KNOWN LIMITATIONS
--------------------------------------------------------------------------------

None of these stop the mod working. All are visible in the log and all have a
working fallback.

  * "SetDonationFaction: FAIL" and "SetTitleDir: FAIL"
    Two optional helper functions are not located on 2.00. The mod falls back
    to a fixed offset (logged as "DonationOff: FALLBACK") and to leaving the
    title direction alone. No visible effect.

  * "Type resolver: DISABLED"
    The housing-chest type lookup is not re-resolved on 2.00, so the mod uses
    the INI fallback instead. The chests work; this is how they are identified.

  * "Gatherables panel-id lookup FAILED (hash-map not yet populated?)"
    Expected on the first housing panel opened in a session. The mod falls back
    to the INI value and the panel opens normally.

  * Several log messages name old game versions - "1.06", "Apr-23 1.0.4.1",
    "1.13+ layout". These are inherited strings and version-gated workarounds
    from the original mod, not statements about your game.

  * The log banner says F6 for Private Storage. It is F4. See CONTROLS.

  * A diagnostic probe is present but only runs when an open is refused. In
    normal use it produces no output. It is deliberately left in: it is what
    identified the 2.00 problem, and it makes any future breakage
    self-diagnosing from the log alone.


--------------------------------------------------------------------------------
  IF SOMETHING GOES WRONG
--------------------------------------------------------------------------------

  bin64\PrivateStorageAnywhere.log is written every launch and overwritten on
  the next one. If you hit a problem, copy that file before starting the game
  again - it is the only record of what happened.

  The two lines that matter most:

    "=== READY! ..."                     the mod loaded and resolved everything
    "Warehouse opened (mode=0x04 ...)"   a panel actually opened

  A "BLOCKED: unsafe state" line means the mod refused to open because the game
  was not in a safe state. It writes nothing to the game when it refuses.


--------------------------------------------------------------------------------
  CREDITS
--------------------------------------------------------------------------------

  Private Storage Anywhere v1.5.10 is by its original author. This is an
  unofficial compatibility build for Crimson Desert 2.00, produced by patching
  the pristine v1.5.10 ASI. The housing-chest expansion and the configurable
  Private Storage capacity are local additions and are not part of the original
  mod.


--------------------------------------------------------------------------------
  HASHES
--------------------------------------------------------------------------------

  PrivateStorageAnywhere.asi  (this release)
    C2B9848F33A3822532C563C31965F4DFF74B0D02A369AE7626D3E43056C49840

  PrivateStorageAnywhere.ini  (this release)
    9A949FD3FCD3845C1FD8FB93247620F0D8DADB17C8DCC2DB5F3DDE4AF4EC8FCD

  Pristine v1.5.10 input ASI this was built from
    4F514298B2BC5DB7E804B0166AD2269BAE97A414F7DAE425A0F736CDA7F56F3E

  The ASI in this package is byte-identical to the build that was tested in
  game on Crimson Desert 2.00. See BUILD.txt to reproduce it yourself.
