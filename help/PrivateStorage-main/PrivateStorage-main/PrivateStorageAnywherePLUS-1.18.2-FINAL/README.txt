================================================================================
 Private Storage Anywhere PLUS - unofficial Crimson Desert 1.18.2 build
================================================================================

 Original mod : PrivateStorageAnywherePLUS v1.5.10
 Author       : Stevi  (Nexus Mods, Crimson Desert, mod 388)
 This build   : unofficial compatibility patch for game version 1.18.2
 Build tag    : CD 1.18.AS
 Date         : 22 August 2026

 All the credit belongs to Stevi. This is their mod. Everything below is a
 patch applied on top of their v1.5.10 release to make it run on 1.18.2, plus
 one optional extra that is clearly marked as not being theirs.


--------------------------------------------------------------------------------
 PLEASE READ FIRST - DISTRIBUTION
--------------------------------------------------------------------------------

 This is a PERSONAL build. Do not re-upload or post it.

 The author's permissions on the Nexus mod page say, in their words:

   Upload permission
     "You are not allowed to upload this file to other sites under any
      circumstances"

   Modification permission
     "You must get permission from me before you are allowed to modify my
      files to improve it"

   Asset use permission
     "You must get permission from me before you are allowed to use any of
      the assets in this file"

 This build is a modified copy of their file, so sharing it publicly would go
 against the first two. If you want it out in the world, the right move is to
 ask Stevi - either for permission to post a patch, or by handing them the work
 so they can fold it into an official release. A ready-to-send message is in
 MESSAGE-TO-AUTHOR.txt next to this package, and the complete source and
 technical notes are in the source archive so they can verify or reuse anything.

 Nexus page: https://www.nexusmods.com/crimsondesert/mods/388


--------------------------------------------------------------------------------
 WHAT THIS FIXES
--------------------------------------------------------------------------------

 On game version 1.18.2 the released v1.5.10 stopped working: the storage
 hotkeys did nothing, and forcing the issue crashed the game. Four things were
 wrong. Short version here, full detail in CHANGELOG-1.18.2.txt.

 1. No panel opened at all.
    The mod finds the game's mode and sub-mode fields by scanning a fixed
    window of memory offsets. On 1.18.2 those fields moved outside that window,
    so the lookup quietly found nothing and the game was never told to switch
    into "store" mode. No error, no panel.

 2. The game crashed on the first open.
    Two writes that were harmless on older builds became fatal. One wrote a
    placeholder value into a field that is now a live pointer; the other wrote
    zero into a pointer the game then uses without checking. Both are now
    skipped.

 3. The storage panel lost a fight with the cinematic mode.
    Opening a panel asked the game's menu layer for a state that makes the game
    flag you as being in a cutscene, and cutscene mode outranks store mode. The
    mod now leaves that layer alone.

 4. The housing chest 10 -> 1000 expansion had silently stopped applying.
    A small code-shape change in 1.18.2 made the mod's own detector reject the
    instruction it was looking for, and the table it walks moved by 8 bytes.

 Everything else in v1.5.10 is untouched and works as the author documented:
 all six panels, controller and PS5/PS4 bindings, ESC to close, the game's
 native warehouse UI, and F11 to reload the INI without restarting.


--------------------------------------------------------------------------------
 REQUIREMENTS AND INSTALL
--------------------------------------------------------------------------------

 Same as the original mod.

 1. Ultimate ASI Loader (ThirteenAG) - version.dll must be in:
       [Steam]\steamapps\common\Crimson Desert\bin64\
    If you already run other ASI mods it is probably there.

 2. Copy PrivateStorageAnywhere.asi and PrivateStorageAnywhere.ini into that
    same bin64 folder, replacing the v1.5.10 files.

 3. Launch the game.
       F4  Private Storage
       F5  Gatherables Chest
       F6  Dresser
       F7  Refrigerator
       F8  Symbol Storage
       F9  Collecting Storage
       F11 reload the INI without restarting
       ESC close

 Uninstall: delete the .asi and .ini (and the .log if present) from bin64, and
 put back the original v1.5.10 files if you want them.


--------------------------------------------------------------------------------
 THE ONE NEW OPTION - PrivateStorageSlots
--------------------------------------------------------------------------------

 NOT part of the original mod. A local addition. Ships turned OFF.

 The game gives Private Storage a base capacity and adds any expansion slots
 you have bought on top. This option lets you set the TOTAL directly:

     PrivateStorageSlots=1000        ; exactly 1000 slots, expansions included
     PrivateStorageSlots=0           ; off - put it back the way the game had it

 Set it to the number you actually want to see. If you own 200 bought expansion
 slots, 1000 means 1000, not 1200 - the mod works out the difference itself.

     PrivateStorageExpansions=-1     ; -1 = work it out automatically (default)

 Only set a number there if the automatic figure is ever wrong; your requested
 total minus that number becomes the base capacity.

 Changes apply while you play: save the INI, then close and reopen the panel.

 Setting it back to 0 genuinely undoes it - the mod records the game's own base
 capacity the first time it looks, before writing anything, so it always knows
 what to restore. Both writes are skipped when the values already match, so if
 you never turn the option on, nothing is touched at all.

 The five housing chests (F5-F9) are the author's own 10 -> 1000 feature and
 are not affected by this setting either way.


--------------------------------------------------------------------------------
 KNOWN LIMITATIONS
--------------------------------------------------------------------------------

 These are real and worth knowing about. None of them stop the mod working.

 - Language auto-detect does not resolve on 1.18.2. The log line reads
   "LangByte: FAIL" and titles fall back to the default language index. Panel
   titles still get set; they may not follow your game language the way the
   author intended.

 - The housing panel type resolver is disabled on this build, so all five
   housing chests share one fallback panel value from the INI. They open and
   work correctly. You will see a line like "Gatherables panel-id lookup
   FAILED" in the log - that is the expected fallback, not a problem.

 - The donation-state clear is switched off, because on 1.18.2 that field is a
   live pointer and clearing it was one of the two crashes. A donation-related
   footer may show on the Private panel as a result. Cosmetic.

 - Housing chests still need one NPC visit per game session before their
   contents display. That is the author's original limitation, unchanged.

 - Untested with other inventory or storage expansion mods. The author states
   the same about v1.5.10 and does not support that combination - and the new
   option above is exactly that kind of feature, so expect conflicts if you
   stack them.


--------------------------------------------------------------------------------
 NOT VERIFIED - WORTH CHECKING YOURSELF
--------------------------------------------------------------------------------

 Two things that were not re-tested on this build. They are listed as checks to
 run, not as things known to be broken, and not as things known to work.

 - The v1.5.6 regression class. After using a mod storage, open a vendor, camp
   provisions, the bank, mission dispatch, or a cooking result screen. On older
   builds a leftover menu state could make those open the NPC without showing
   any UI. The cause of that bug is addressed by fix 3 above, but it has not
   been specifically re-tested here.

 - Whether the storage total reaches your save file. Set PrivateStorageSlots=0,
   quit the game completely, restart, and open Private Storage. It should read
   your original number. If it does not, the game is persisting the value and
   that is worth knowing.

 Back up your save before using the new option the first time.


--------------------------------------------------------------------------------
 FILES AND HASHES
--------------------------------------------------------------------------------

 PrivateStorageAnywhere.asi
   SHA-256  A7F7D38169250E1FE1D570613369022CA4933721943E5C113687B7F9D0AA51EE
   264,192 bytes

 Built from the pristine Nexus v1.5.10 release:
   SHA-256  4F514298B2BC5DB7E804B0166AD2269BAE97A414F7DAE425A0F736CDA7F56F3E
   256,000 bytes

 If your v1.5.10 download does not match that input hash, this patch was not
 built from the same file you have.


--------------------------------------------------------------------------------
 HOW IT WAS BUILT
--------------------------------------------------------------------------------

 Not hand-edited. A Python script applies every change to the pristine v1.5.10
 binary, verifies the input by SHA-256 first, and checks the exact original
 bytes at each site before writing - so it fails loudly rather than silently
 patching the wrong thing.

 Every address and field offset is derived from the game's own code and data
 rather than hardcoded, in the same spirit as the author's own update-proofing.

 The new option lives in two sections appended to the file rather than squeezed
 into existing space, so the original code is left exactly as it was: outside
 two PE header fields, the two appended section headers, two call
 displacements and the build tag, the binary is byte-identical to the
 known-good baseline.

 The patcher and the full technical write-up (how each fix was found, including
 the wrong turns) are in the source archive:

   PrivateStorageAnywherePLUS-1.18.2-FINAL-source.zip

 Antivirus note, same as the author's: this mod hooks internal game functions
 because the game has no mod API for this, and that can look suspicious to
 scanners.
