# How the game runs

What the game's own code does, as far as it has names. The names come from `game/symbols.tsv`
(functions) and `game/globals.tsv` (data), rows with the source `user`. Ghidra's analysis applies
them (`tools/ghidra/analyze.sh`), and `tools/decomp/convert.py` carries them into the generated
code. Add a name there, not in the generated code.

`game/types.h` declares the game's own types and the prototypes of part of the named functions. The
analysis gives each of these functions its prototype. A fourth column in `game/globals.tsv` gives
the data a type from that header or from `game/sdk.h`.

## The frame

`gameMain` sets Ninja and the game up, then runs one game frame at a time until `gameExitRequest` is
set and no write to the memory card runs. An open disc tray sets `gameExitRequest`, and so does the
soft reset in scene 1 and while `gameDemo` is set. In a game, the soft reset changes to the title
instead (`gameTaskFrame`). Each frame:

1. `pdExecPeripheralServer` reads the controllers.
2. `gameFrame` runs the frame.
3. `kmWaitVBlank` waits for the vertical blank.

`gameInit` sets the game up before the first frame, and `gameRelease` frees what it took.

`gameFrame` copies each controller into the game's own copy (`gameReadPad`, into `gamePads` from
`gamePeripherals`). With `optionRotateButtons`, it turns the directions for the vertical screen.
Then it acts on `sceneRequest`:

- **0**: the scene runs. `gameTaskFrame` checks the soft reset, runs the task lists, opens the
  frame (`njStartDraw`), draws the task lists and renders (`njRender`).
- **1 to 12**: `gameEnterScene` changes to that scene, with the scene's start function that
  `gameFrame` gives it for the number. It removes every task, saves to the memory card when
  `saveRequest` and `optionAutosave` are set, makes the request the current scene
  (`sceneCurrent`) and calls the start function, whose result is the next request.

| Scene | Start function | What it is |
| --- | --- | --- |
| 1 | `sceneBootStart` | Loads the common data (`COMDAT`) and the notice (`WARNDAT_DC`). Its task shows the notice and loads the save from the memory card in drive 0. When the card has no save and has room, it makes a new one. Then it waits for Start. |
| 2 | `sceneLogosStart` | The logos |
| 3 | `sceneRankingStart` | The ranking lists, after a game outside the practice mode first the play data. VIEW RANKING and the end of the attract demo also lead here. |
| 4 | `sceneTitleStart` | The title and the main menu |
| 5 | `sceneModeSelectStart` | The mode select after GAME START, when DIFFICULTY is USER SELECT or TRIAL GAME is on. Else GAME START goes to scene 6. |
| 6 to 10 | `sceneChapter1Start` to `sceneChapter5Start` | The five chapters |
| 11 | `sceneTutorialStart` | The tutorial |
| 12 | `sceneEndingStart` | The ending. It sets `gameClearFlags`. |

An autoplay game goes through the scenes 1, 2, 4, 5, 6 to 10, 12 and 3. RANKING and PRACTICE in
the main menu stay in scene 4.

A chapter's start loads the chapter's sound banks (`soundLoadBanks`), sets the fade to black and
sets `sceneNext`, the scene to change to when the chapter ends. Then `stageStart` sets the stage up
with the chapter's stage data.

## The task system

A task is a block from `syMalloc`: a header of 0x18 bytes, then the task's own data. Its functions
get the data's address.

| Offset | Size | Content |
| --- | --- | --- |
| 0x00 | 1 | Level: 0x80 when made. The removals by level take the tasks up to a level. |
| 0x01 | 1 | Removal countdown: 0 while the task lives. The list's run counts it down, and at 0 removes the task. |
| 0x04 | 4 | Update function, or 0 |
| 0x08 | 4 | Draw function, or 0 |
| 0x0C | 4 | Function called at the removal, or 0 |
| 0x10 | 4 | The task before it in its list |
| 0x14 | 4 | The task after it in its list |
| 0x18 | | The task's data |

`Task` in `game/types.h` is this header. `taskLists` holds 7 lists (`TaskList`), each its first and
last task (8 bytes). `gameTaskFrame` runs lists 0 to 5, then draws them. While the game is paused
(`pauseState` 1), it runs only list 6 and draws lists 0 to 6.

| Function | What it does |
| --- | --- |
| `taskInitLists` | Empties the 7 lists. |
| `taskCreate(update, draw, size, list)` | Makes a task with `size` bytes of data at the end of the list. Returns the data's address. |
| `taskRunList(list)` | Calls each living task's update function, and removes the tasks whose countdown ends. |
| `taskDrawList(list)` | Calls each living task's draw function. |
| `taskRemove(list, task)` | Calls the removal function, unlinks the task and frees it. Returns the next task. |
| `taskRemoveLevel(list, level)` | Removes the tasks up to `level` now. |
| `taskKillLevel(list, level)` | Marks the living tasks up to `level` for removal at the next run. |
| `taskKillUpdate(list, update)` | Marks the living tasks with that update function for removal at the next run. |

## The player

`playerCreate(index)` makes a player's ship at a chapter's start: a task in list 2 whose data is a
`Player` (`game/types.h`). `players` points at each player's. `playerUpdate` runs the ship by its
`state`:

| State | What happens |
| --- | --- |
| 0 | A new life. The ship gets 200 frames of invulnerability, which count down in state 2, loses steering and buttons, and its chain starts again (`chainReset`). |
| 1 | The fly-in from below, free of the screen's bounds. At its end, steering and buttons come back. |
| 2 | In play. `playerMove` reads the controller (`playerReadInput`), steers, and shoots, releases and switches (`playerButtons`). A hit or the scenery while not invulnerable leads to state 3. A ship on its last life then leaves `playersInGame`. |
| 3 | Hit. After 60 frames, a new life with one life less, or `playerOut` and state 4 when no lives are left. In the attract demo and in chapter 1 of a trial game, the ship loses no life. |
| 4 | Out of the game. Once its player's bit in `playersInGame` is set again, `playerDraw` moves the ship to state 5 while its bit in 0x8C15C590 or 0x8C15C58C is set, else to state 0. In chapter 1's first part at `stagePhase` 1, it moves to state 2 instead. |
| 5 | Held back while the player's bit in 0x8C15C590 or 0x8C15C58C is set (the tutorial sets both bits in 0x8C15C58C). Then state 0. |

`playerReadInput` turns the controller into `playerInput`: the directions from the pad, and shot,
switch and release from the buttons that `buttonShot`, `buttonSwitch` and `buttonRelease` choose in
`buttonMasks` (A, B, R or C, X, Y, L or Z, or none). With `buttonShotSwitch` set, shot and switch
pressed in the same frame or in two frames in a row release too.

`polarity` is 0 for white and 1 for black. A switch turns the ship, and halfway through the turn
the polarity changes. `playerSetPolarity` then sets the ship's hit attribute and keeps the polarity
in `playerPolarity`, which `playerCreate` gives the ship at the next stage's start.

A release turns the energy in `playerEnergy` into homing lasers (`playerLaserCreate`):
`playerButtons` fires one for each 10, up to 12, and takes 10 for each (`playerSpendEnergy`). The
gauge at the screen's side has a box for each laser.

`playerStart(index)` puts a player into the game: it sets the player's bit in `playersInGame`,
clears the scores (`playerScoresClear`), and gives the lives from `gameLives` and the first extend
from `gameExtend`. The score starts at the player's continues, up to 9. `playerJoin` also gives the
player the buttons from the options. A player who presses Start during a game joins with a credit
(`playerJoinCheck`, `creditUse`).

When a player loses the last life, `playerOut` starts the name entry if a score places in the
rankings and the game enters them, and the name entry starts the continue when it ends. Else
`playerOut` starts the continue at once (`continueStart`): it sets `continueTimer` to 9 and makes
the continue's task (`continueTask`, `continueScreen`, `continueEnd`), which shows CONTINUE? and the
count. The stage's draw (`stageDraw`) counts `continueTimer` down, one count each 128 frames. Start
with a credit continues: the player joins again, and `playerContinueAdd` counts the continue in
`playerContinues`, because `continueNext` is 1 from the first game over on. `playersContinuing` has
a bit for each player at the continue. When a continue's count runs out, `continueTask` calls
`gameOverCheck`. It starts the game over (`gameOver`, `gameOverTask`) when no player is in the game,
no ship is dying on its last life and no name entry waits: GAME OVER, a fade to black over 180
frames and sound 0x3b.

## The stage scripts

`stageStart` makes the stage's task. Each frame its update, `stageUpdate`, moves `scrollSpeed`
toward `scrollTarget` by `scrollStep`, adds it to `scrollPosition`, and stops the scroll at the
stage's end. It also clears `stageEvents`, whose bits the frame then sets. 0x02 makes the enemies'
bullets vanish: a ship's fly-in sets it, and so do the results (`resultsStart`) and other code.

A stage's enemies come from scripts. A script is an array of `ScriptRecord`s (`game/types.h`), 0x4C
bytes each, that `scriptStart` gives to a task in list 3. Each frame, `scriptRun` takes the current
record through `scriptStep`. A record runs once one of its conditions holds:

| Flag | Condition |
| --- | --- |
| 0x0001 | `scrollPosition` has passed `value`. |
| 0x0004 | The record has waited `value` frames. |
| 0x2000 | The script has run `value` frames. |
| 0x0080 | The group in bits 4 and 5 is empty. |

Running calls the record's `function` with its `data`, and the script moves on by `next` records, to
the next record in the same frame. The flags 0x0002, 0x0008 and 0x1000 test the conditions of
0x0001, 0x0004 and 0x2000, and move on by `branch` records without the call. A record with 0x8000
ends the script once it runs or branches. It still waits for its condition and calls its function
first.

A record function makes something, mostly an enemy, and returns it. With 0x0800, the result joins
the group in bits 8 and 9 (`scriptGroupAdd`). There are 4 groups of up to 256 members
(`scriptGroups`, `scriptGroupCounts`). `scriptGroupsUpdate` runs each frame and drops the members
whose task is marked for removal, so 0x0080 waits until a group's enemies are all gone.
`scriptGroupsInit` empties the groups at each `stageStart`.

The game's code starts 16 scripts, with 792 records and 56 record functions in all.

## The stage program

`tools/game/stage.py program 1` prints chapter 1's stage program with names, and
`tools/game/stage.py script ADDRESS` a stage script.

A chapter's start gives `stageStart` the chapter's stage program, a list of commands in words. The
stage's task (`Stage`) runs it each frame (`stageProgramRun`) before it moves the camera along the
path. Each command is a number into `stageCommands` and the command's words. A command moves the
program on by its words, or waits for the next frame. Positions and other numbers that the
commands give as floats are in units of 1/10,000.

| Command | What it does |
| --- | --- |
| 0 | Stops the program. |
| 1 | Builds the camera's path (`pathCreate`). |
| 2 | Raises the first byte of `stageLocks`. `stageUnlock` lowers it, for example at the end of a chapter's results. |
| 3 | Waits until that byte is 0, then raises `stagePhase`. |
| 4 | Waits until the lock that it names is 0. |
| 5 | Waits until `scrollPosition` reaches its position. |
| 6 | Raises `stagePart` and sets `stagePhase` to 0. |
| 7 | Ends the stage: changes to `sceneNext`. In the practice mode (`gamePractice`) it adds the play time and changes to the title. |
| 8 to 12 | Calls a function with the stage's data, or with 1 to 4 of its words. Some of the functions start the stage scripts, through small functions that call `scriptStart`. |
| 13 to 17 | The same, once the scroll reaches its position. |
| 18, 22 | Sets a float or a word. |
| 20, 24 | Moves a float or a word to a value in a number of frames, on a cosine curve (`easeFloat`, `easeWord`). The move waits until the lock that it names is 0, then holds it while it runs. |
| 19, 21, 23, 25 | The same as the command before, once the scroll reaches its position. |
| 26 | Slows the scroll down to stop at a position (`scrollStopAt`). |

Chapter 1's program has 100 commands and 34 calls. It starts with commands 18 and 22, which set the
camera's values, builds the path with command 1, and ends with command 7. Two later commands 1 build
a second path.

## The camera path

`scrollPosition` is the distance along the stage's camera path. Command 1 of the stage program
builds the path from its points (`pathCreate`, `PathPoint`): seven values for each point, the
camera's position, three angles and one more value. `pathCreate` works out the distance along the
path at each point. Each frame, `stageProgramRun` finds the camera on the path at `scrollPosition`
(`pathAt`): `pathPosition`, `pathAngles` and `pathValue`. Between two points, each value moves in a
straight line, or on a cubic curve where the point's flags have the value's bit. `stageUpdate` then
adds the stage program's offsets to the three angles and the value, and copies them into the camera
(`Camera`) that `sceneCamera` points at as the four angles of its orbit. From the values that the
stage program sets, it also copies the orbit's distance, the offset, the perspective (`unknown_90`,
which `cameraOrbit` copies to `perspective`), the flat view's perspective and the depth that the
objects take from their z when they draw. The orbit turns about `pathPosition`, which `stageStart`
gives the camera as `follow`.

`cameraCreate` makes the camera, a task in list 0 whose data `sceneCamera` points at, at the start
of the mode select, of each stage (`stageStart`, the chapters and the tutorial), of the ranking
scene and of the ending. `cameraUpdate` places it on its orbit about the point `follow`
(`cameraOrbit`), moves it toward a target view by `blend`, and builds its view matrix
(`cameraView`). `cameraDraw` sets the perspective and the view for the draws, and `cameraEnd` clears
`sceneCamera` at its removal. A second task, in list 1, clears the scenery polygons at each frame
(`sceneryClear`), and with the camera's mode 1 gives the draws of list 1 a flat view, turned for a
vertical screen (`cameraFlat`).

## Enemies and hits

A stage script's record function makes an enemy: a task in list 3 whose data starts with an `Enemy`
(`game/types.h`), and continues with the enemy's own data. The enemies of `enemyModel4Create` have a
common part of 0x78 bytes instead, with the behaviour at 0x70 and the data pointer at 0x74.
`enemyInit` fills the common part from the record's data: the position, the angles, the color from
the data's byte 0x38, and the hit record. Each frame the enemy's update calls its `behaviour`, and a
result other than 0 ends the enemy. Some behaviours add the velocity to the position (`enemyMove`).
The draw sets `offscreen` while the model is out of view, and some behaviours end the enemy then.
When `hit.hp` reaches 0, the update calls `enemyDestroyed`, plays a sound and ends the task
(`taskEnd`).

A `HitRecord` says what an object hits and is hit on: its attribute, its flags, the damage it deals
(`power`), its hit points and two hit shapes. Shape 0 is what it hits with, and shape 1 is where it
is hit. A shape is a list of entries (`HitEntry`): each is a sphere at the object's origin with a
radius, or points at a sphere (the SDK's `NJS_SPHERE`). `hitShapeInit` finds the sphere around them
all.

Each frame, an object's draw files its record (`hitFile`) in one of the 5 `hitLists`, with its
spheres moved to where the object was drawn. `hitFile` skips a record without hit points, and one
whose shapes are both out of view unless it has flag 0x100:

| List | Holds |
| --- | --- |
| 0 | The ships (`Player.hit`) |
| 1 | The ships' fields (`Player.field`), which absorb the bullets of the ship's color |
| 2 | The players' shots and homing lasers |
| 3 | The enemies |
| 4 | The enemies' bullets |

`hitPass` then tests the shots against the enemies, the fields against the bullets, the bullets
against the ships, and the enemies against the ships (`hitTestLists`). A record hits another that
still has hit points when the low 4 bits of its attribute share a bit with the high 4 bits of the
other's, and a sphere of its shape 0 meets a sphere of the other's shape 1. A hitter with flag 0x10
hits only records with flag 0x20, and a hitter with a `lock` hits only the record in that slot.
`hitApply` then takes the hitter's power from the target's hit points, twice the power when their
colors differ. A target with flag 0x02 or 0x04 loses nothing. A white ship (0x60) is hit by black
bullets and by enemies, and not by white bullets. A hitter with flag 0x01 scores 10 points for each
point of damage that it deals, unless the target has flag 0x04 or 0x08. The target's `hitBy` tells
which player hit it.

`hitSlots` holds the objects that homing lasers can aim at, up to 200 (`hitRegister`). Each slot
keeps where its object was drawn, and `aimed`, the damage of the lasers on their way to it.
`laserTarget` picks the nearest object in view that still needs damage, or the nearest object in
view when none does, and skips records with flag 0x80. `hitSlotAim` adds a laser's 10, or 20 against
the other color. `gaugeSlot` is the slot of the object hit this frame, and the gauge shows its hit
points.

`sceneryHit` tells whether a point is inside the scenery. A shot stops there, and a ship that is
not invulnerable dies there.

When an enemy dies, `enemyExplosion` shows the explosion (`effectExplosion`, `effectParticles`), and
`enemyDeathBullets` fires bullets of the enemy's color at the player who made the kill. How many
depends on `difficulty`: none on easy, on normal only after a kill with the enemy's own color, and
on hard always, half as many and slower after a kill with the other color.

`hpGaugeCreate` makes the gauge of hit points at each `stageStart`, and `hpGauge` points at its
data. Each frame, an enemy that was hit (its slot is `gaugeSlot`) puts its hit points into the
gauge (`hpGaugeSet`).

## The chain and the score

`enemyDestroyed` counts the kill for the chain (`chainCount`), for the player whose hit ended it
(bit 3 of the record's `hitBy`) and by the enemy's color, 1 white or 2 black. `chainSet` holds each
player's current set: its color in the high 4 bits and its kills in the low 4. The chain status on
the screen shows the set. Three kills of one color complete it: `chain` grows by 1, `chainMax`
follows, and `chainBonus` gives 100 points, doubled for each chain after the first, up to 25,600
from chain 9. A kill of the other color before a set is complete sets `chain` to 0.

`scoreAdd` adds to `playerScore`, and at `extendScore` gives a life. It adds nothing once the game
over runs (`gameOver`), or while the player's last ship explodes. With `gameExtend` 1 the next
extend comes 2,000,000 points later, with 2 5,000,000 later, and with 3 at 15,000,000 and then no
more.

## Bullets

An enemy fires with `bulletFire(attribute, position, angle, speed)`: a task in list 4 whose data is
a `Bullet` (`bulletCreate`). The attribute is the color, 0x11 white or 0x22 black. `vectorFromAngle`
turns an angle and a speed into a velocity, with angle 0 pointing up. `aimAtPlayer(which, point)`
gives the angle from a point to a ship: to player 1's with `which` 0, or to player 2's with 1, when
that player is in the game (`playersInGame`, 0x01 for player 1 and 0x02 for player 2). Else it aims
at the other ship when that player plays, and at the nearer of the two (`nearestPlayer`) when
neither plays. With `which` 2 it aims at the nearer of two (`nearestPlayer`) when both or neither
play, else at the one that plays.

Each frame `bulletUpdate` moves the bullet. It ends the bullet:

- at the screen's edge, `margin` beyond it.
- when its hit points are 0: a ship's field of the same color absorbed it (`bulletAbsorbed`).
  `bulletAbsorb` sends a spark to the ship, and `playerAbsorb` gives the player one point of energy,
  up to 120, and 100 points. While the flag at 0x8C1D1417 is set, it gives only the points.
- when it lies in the scenery, when its hit record shows a hit, or while `stageEvents` has 0x02,
  as while a ship flies in. `bulletVanish` makes its vanishing effect.

Two more kinds of bullet follow the same rules. `bulletSpriteCreate` makes one with a sprite that
the caller picks, which `bulletSpriteUpdate` animates through 4 frames. `bulletSpinCreate` makes one
with a fixed sprite that `bulletSpinUpdate` turns at each frame. Their draws (`bulletSpriteDraw`,
`bulletSpinDraw`) give white and black bullets two depths that change places from frame to frame,
and file the bullet's hit record.

In the prototype mode (`gamePrototype`), shots cost energy, absorbing gives three times the energy,
and the energy goes up to 999.

## Pause and the attract demo

`pauseUpdate` runs each frame. Start on a player's pad pauses the game, unless the player's bit in
`pauseLocked` is set, as it is while the player is out of the game. `pausePlayer` keeps who paused.
`pauseState` is then 1: `gameTaskFrame` runs only task list 6, and draws lists 0 to 6. Outside the
tutorial (scene 11), the sound's sequences and effects pause (`soundPauseSequences`,
`soundPauseEffects`), and the vibration stops (`vibrationStopAll`). A 1 in `pauseEnd` ends the
pause. When both players are out of the game and no game over runs, `stageUpdate` sets `pauseState`
to 2, and the game pauses at the next frame. It does not do this in the attract demo or in the
tutorial. The tutorial's own code sets 2 too.

`gameDemo` is 1 in the attract mode: after the boot's card check, during the logos and the title
until Start, and in the demos. A chapter or the tutorial then starts from recorded data
(`demoStart`): the difficulty, the lives, the random seed and the input in `demoInput`, which player
1's ship reads instead of the pad (`demoInputRead`). In a chapter's demo, `stageUpdate` fades out
after 3,810 frames and changes to scene 3 at 3,900. The tutorial's demo changes to scene 3 at its
end (`sceneNext`). The title starts the demos in turns (`demoCount`, see the title's menus below).

## Sound

`soundInit` opens the sound library's 8 MIDI sequence players and 20 one-shot players, and loads
`SFX_DEFAULT.BIN` into slot 0 of `soundBankLists`. The start of a chapter, of the tutorial and of
the mode select loads its own list into slot 1 (`soundLoadBanks`), which sends each bank of the list
to the sound driver. A sound's id names the slot in its high byte and an entry of that list in its
low byte.

`soundPlay(position, id)` starts a sequence, and `soundEffect(position, id)` a one-shot sound. Each
takes a voice slot (`SoundSlot`) in the range that the entry gives, or else takes over another
sound by priority and age (`soundSlotFind`). With a position and `soundStereo` on, the sound gets a
pan from the position's x (`soundPan`) and a volume (`soundLevel`). While `soundOff` is set, no
sound starts.

`soundServer` runs each frame. It starts the sounds that wait in state 2 and ends the ones that the
library reports done. For the pause (`soundPauseSequences`, `soundPauseEffects`) it pauses the
sequences and mutes the one-shot sounds, and after the pause it continues both.

`soundSetStereo` and `soundSetMono` set `soundStereo`, the library's pan mode and volumes, and the
console's sound setting. `optionsApply` picks one from the console's setting.

## Vibration

`vibrationStart(player, pattern, time)` makes a controller vibrate, while the player's
`optionVibration` is on and `gameDemo` is 0. `vibrationPort` finds the vibration unit in one of the
controller's two sockets, and `vibrationIndex` gives the unit's index. A task
(`vibrationTaskCreate`, `vibrationTask`) sets the unit's stop time and starts it. A newer request
for the same unit raises its count in `vibrationRequests`, and the older task ends without starting.

## The title and its menus

`sceneTitleStart` makes the title's task (`titleTask`, removed by `titleDestroy`). Its draw runs one
screen at a time, by `titleState`, and `menuItem` is the cursor of the screen that runs:

| State | Screen |
| --- | --- |
| 0 | `titlePressStart` waits for Start. After 900 frames it plays the attract demo, in turns the tutorial, chapter 1 and chapter 2 (`demoCount`). |
| 1 | `mainMenu`: GAME START, PRACTICE, RANKING, CONFIG, TUTORIAL, APPENDIX, BACKUP and EXIT. B or EXIT leaves it. |
| 2 | `configButtons`: CONTROLLER SETTING, each player's buttons and vibration |
| 3 | `rankingMenu`: VIEW RANKING, which changes to scene 3, CLEAR RANKING and EXIT |
| 4 | `rankingResetScreen`: "Are you sure?", then `rankingDefaults` |
| 5 | `configMenu`: MONITOR TYPE, SOUND, DIFFICULTY, TRIAL GAME, EXTEND, PLAYER STOCK, CONTROLLER SETTING, DEFAULT and EXIT |
| 6 | `practiceMenu`: chapters 1 to 5 of each difficulty, up to `chapterReached` |
| 7 | `backupMenu`: AUTO SAVE, LOAD, SAVE and EXIT, with the play time |
| 8 | `overwriteConfirm`: the backup menu's question before a save overwrites the file on the card |
| 9 | `appendixMenu`: GALLERY 1, GALLERY 2, SOUND TEST and GAME MODE, each shown after its bit of `gameClearFlags`, such as for a trial cleared without a continue or 5 hours of play. GAME MODE switches `optionPrototype` between NAOMI and PROTOTYPE. |
| 10, 11 | A wait, then `appendixViewer`: GALLERY 1, GALLERY 2 or the SOUND TEST |
| 12, 13 | The fade, then the change to the chosen scene |

`sceneModeSelectStart` makes the mode select's task (`modeSelectUpdate`, `modeSelectDraw`).
With USER SELECT it asks for the difficulty, and with TRIAL GAME on for the normal or the trial
game. `modeSelectCursor` moves the choice with up and down, and `modeSelectDifficulty` sets
`difficultyChoice` and `difficulty` from it. Then the task requests chapter 1, or the tutorial for
a trial.

## Options and the save

The options and the records are one block of 0x3C34 bytes at 0x8C1CD758, which the memory card
file IKARUGA_DATA holds. It starts with the identifier `saveId` ("IK-BKUP 1.2") and `saveSum`, the
sum of the block's other bytes. `saveReset` writes the defaults: `optionDefaults` sets the options
and `rankingDefaults` the ranking lists.

`saveWrite(force)` makes the file's image in `saveImage` with the header `saveHeader`
(`saveImageMake`) and starts to write it. Without `force`, it writes nothing when `saveCalcSum`
equals `saveSum`. `saveRead` starts to read the file into `saveImage`. When the card is
ready again, `saveWriteDone` and `saveReadDone` take the result. `saveReadDone` copies the block
from the image when the identifier matches. `optionsApply`, which runs after a load and at other
times, tests the identifier with `saveCheck` and writes the defaults when it does not match.

`vmuScreenUpdate(port)` runs the picture on the screen of the VMU in a controller, with a state for
each port in `vmuScreenState`. Other code tells it what to show through `vmuScreenEvent(port,
event)`: 0 the default picture, at each scene change and when a chain breaks, 1 the chain after a
completed set (`chainCount`), 2 when a save starts, 3 when a load starts, 4 when the card's work
went well and 5 when it did not.

`optionsApply` gives the game the options after a load. `difficulty` comes from `optionDifficulty`
or, with USER SELECT (3), from the player's choice at the start (`difficultyChoice`). `gameLives`,
`gameExtend`, `screenHorizontal` and `screen75` are the game's copies of the other options.

With `optionTrial` on, the game start offers the trial game next to the normal one. A trial plays
the tutorial, then chapters 1 and 2, with unlimited lives in chapter 1, and then the ending. In
such a game `gameTrial` is 1, and chapter 2's start sets the ending as `sceneNext`.

`playTime` counts the seconds of play, up to 1,000 hours, and `playTimeAdd` adds a game's
`gameFrames`, divided by 60, when the game ends. Each hour of play gives one more credit
(`creditsPerGame`, raised at `playTimeNextCredit`). The hour after 9 credits gives free play, 0xFF.
A game starts with `credits` from `creditsPerGame`, and a practice with free play (0xFF).
`playerContinues` counts each player's continues.

`rankings` holds 4 tables of ranking lists, and the game's difficulty picks the table. A table has
12 lists of 20 entries (`RankEntry`). The first 6 lists are ALL and the chapters 1 to 5 of games
with one player, and the other 6 those of games that both players took part in (`gameTwoPlayers`).
When a player goes out and at the ending, `rankingFindPlaces` looks for the places of the player's
scores (`rankPlaces`). In a game with two players it waits until both are out, and looks for the
places of their totals (`chapterScoresTotal`) in lists 6 to 11. A player with a place gets the name
entry (`nameEntryStart`, `nameEntryTask`, `nameEntryDraw`, `nameEntryPlayers`), after which
`rankingAdd` puts the scores into the lists. A game in the practice or the trial mode enters no
ranking (`gameNoRanking`). A default entry has the score 100,000 minus 1,000 for each place, the
name TRE and the date September 5, 2002.

## The HUD and the results

`hudCreate(player)` makes a task in list 5 for each player's HUD. `hudDraw` draws the score ("1P" or
"2P"), the lives, the chain status, and the energy: the gauge of homing lasers (`energyGaugeBuild`),
or a number in the prototype mode. `hudPlace` places the gauge's frames by the screen's orientation
and `optionScreenX` and `optionScreenY`. `hudUpdate` follows `chainSet`, and keeps a complete set of
three on the HUD for 62 frames. A game with two players shows a TOTAL too: `scoreTotal` adds the two
scores, with the sum of their last digits held at 9 or less. A score's last digit starts as the
player's continues (`playerStart`). `hudPressStart` blinks PRESS START, PLEASE WAIT or, when no
credit is left, GAME OVER in the place of a player who is not in the game. `frameCount` counts the
frames from `gameInit` on.

A boss's fight has a time limit: `bossTimerCreate(owner, start, end, seconds)` makes the timer,
which waits for the flag at `start`, counts TIME down (`bossTimerUpdate`, `bossTimerDraw`, the
seconds in `bossTimer`), and sets the flag at `end` when the time runs out. When the boss dies
first, `bossTimeAdd` adds the seconds left plus 1 to `bossTimeLeft`, which `stageStart` clears
(`bossTimeClear`). At a chapter's end, unless the game is over, `resultsStart` shows the results:
the BOSS DESTROY BONUS of 10,000 points for each second left, then YOUR SCORE(This Chapter) with the
chapter's longest chain, and YOUR PLAY RANK. The REMAIN bonus is a task of its own
(`remainBonusStart`). `resultsUpdate` runs its steps (`resultsStep`), five a frame while a player in
the game holds A or B. `resultsDraw` draws its two sprites, and each text is a task of its own
(`resultsTextCreate`).

While a chapter's opening sequence flies the ships in, `shipSideEffectCreate` makes an effect at
each side of each player's ship (`shipSideEffectUpdate`, `shipSideEffectDraw`).

## Drawing

The graphics come in 16 banks of 0x40 bytes (`banks`, laid out as `Bank`). Each bank holds a list of
models, a list of sprites and a texture list. `banksInit` opens the disc's directories
(`fileDirsOpen`, `fileDirs`) and clears the banks (`bankClear`). A sprite's id gives the bank in its
high 16 bits, the sprite in the next 8 and the frame in the low 8. Its flags give the blending in
bits 24 and 25, no texture filter with bit 26, fog with bit 30 (`drawFog`), and the vertex builder
in bits 1 to 3.

`spriteDraw(flags, id, position, angle)` sets the texture (`spriteTexture`), lets the vertex builder
make the four corners at the screen position, and draws them (`spriteMode`). `spriteAtMatrix`,
`spriteAtMatrixOffset` and `spriteAtMatrixDepth` draw a sprite at the current matrix's position,
projected to the screen. `spriteDraw3D` draws one as a quad in the current matrix. `modelDraw(flags,
id)` draws a model of a bank after `modelSetup`, which leaves out a model outside the view when bit
27 is set and bit 31 is clear.

A model's motion is in its bank too. `modelDrawMotion` draws a model at a frame of a motion, and
`modelDrawNodes` does so with functions for each node of the model (`modelTreeDraw`).
`modelMotionFrames` gives a motion's length, and `modelMotionStep` moves the frame by a step,
backward with flag 0x02. At the end it starts again with flag 0x01, or else stops there and returns
1. `enemyModelDraw` and `enemyModelDrawMotion` draw an enemy's model with the flags that
`enemyModelFlags` adds: the enemy's color and, in a frame in which something hit it (`hitBy`) and
its record takes damage, a light that makes it flash (`lightOn`, `lightOff`). `lightsReset` sets the
lights to their defaults, and `lightsApply` gives them to Ninja's draws: the main light's direction
and color and the ambient light, with the flash's light off. `cameraUpdate` turns the main light's
direction with the view (`lightAim`).

`textDraw(flags, text, position, angle)` draws a text from the font's sprites, and `textWidth`
measures one. `viewPlayArea` sets the clipping (`clipPlayArea`) and the aspect for the play area,
and `clipOff` turns the clipping off for the whole screen.

On a vertical screen (`screenHorizontal` 0) the game turns positions by 90 degrees.
`matrixTranslation` gives the current matrix's position, and `matrixCalcPoint` a point that the
matrix transforms, both turned this way.

`sphereInView(sphere)` tests whether a sphere (x, y, z, radius) reaches the plane at the depth of
`sceneCamera`, with x within 27.5 and y within 37.5, each widened by the radius.
`polygonContains(polygon, origin, point)` tests a point against a polygon of the scenery
(`Polygon`). Objects of the stages add their polygons to `sceneryPolygons` with `sceneryAdd`, which
has room for 128 and does not check it (`sceneryCount`). `sceneryHit` tests a point against each.

`modelFileLoad(job)` loads a model file in steps and returns 1 while it waits for the disc, 0 when
it is done and -1 on an error. `chunkNext` walks the file's chunks: the model (NJCM), the
texture list (NJTL) and the motions (NMDM). Then the step loads the textures. The disc reads go
through `fileReadStart`, `fileReadPoll` and `fileReadEnd`, which keep one file in
`fileReadHandle`, `fileReadBuffer` and `fileReadSize`. `fileSetDir(n)` changes to a directory of
`fileDirs`.

## Effects

An effect is a task whose data starts with an `Animation`: a list of sprites and the frames to show
each. `animStart` starts it, and `animStep` moves on at each frame and gives 1 at the list's end.
`effectBurst` makes a sprite that flies with a velocity, and `effectFollow` one that follows another
task's position, when it has one, and ends when that task's removal starts. Both are
`EffectSprite`s. They grow by 5% at each frame. After their animation's end, they lose 0x40 of alpha
at each frame until they are gone. One of `effectSpriteDraw`, `effectSpriteDrawDepth` and
`effectSpriteDrawPlain` draws them, by the flags they were made with. `effectParticle` makes a
`Particle`, which flies, slows and shrinks until it is gone or off the screen. `explosionUpdate` and
`explosionDraw` run the tasks of `effectExplosion`. `flashUpdate` and `flashDraw` run those of
`effectFlash`.

`offScreen(point)` tells whether a screen position lies outside the screen and a margin around
it: x from -320 to 960, y from -240 to 800. `matrixOffScreen` and `pointOffScreen` test the
current matrix's position and a transformed point. `pointToPlane` gives a point's place on the
ships' plane, in the units of their positions: the ships stay within ±25 by ±35.

## Helpers

| Function | What it does |
| --- | --- |
| `fileLoad(name, size)` | Reads a whole file from the disc into a block from `syMalloc` and returns the block. It puts the file's size in `size` when that is not 0. |
| `fadeSet(a, r, g, b)` | Gives the fade one color. |
| `fadeTo(c0, c1, c2, c3, frames)` | Makes a task that changes the fade's four corners to these ARGB colors in `frames` frames. Once the game over's fade has started, it makes none until the next scene. |
| `fadeDraw` | Draws the fade, a polygon over the whole screen with the four corners of `fadeQuad`, while the first corner's alpha is not 0. |
| `stringCopy(to, from)` | Copies a string with its end. |

## The named data

| Name | Address | Content |
| --- | --- | --- |
| `sceneCurrent` | 0x8C15C560 | The scene that runs |
| `sceneRequest` | 0x8C15C564 | The scene to change to, 0 for none |
| `sceneNext` | 0x8C1D13FE | The scene that follows the stage, which each chapter's start and the tutorial's start set |
| `fadeQuad` | 0x8C0CFA40 | The fade's four corners (`NJS_POLYGON_VTX[4]`) |
| `scriptGroupCounts`, `scriptGroups` | 0x8C1E94A4, 0x8C1E94A8 | The members of the stage script's 4 groups |
| `soundSequenceSlots`, `soundEffectSlots` | 0x8C1CB32C, 0x8C1CB61C | The voice slots of the 8 sequence players and the 20 one-shot players |
| `soundBankLists`, `soundBankNames` | 0x8C1CB21C, 0x8C1CB19C | The 4 loaded lists of sound banks, and their file names |
| `soundOff`, `soundStereo` | 0x8C1CB73C, 0x8C1CB738 | 1: no sound starts. 1: sounds get a pan and a volume from their position |
| `gamePads`, `gamePeripherals` | 0x8C15C4FC, 0x8C15C504 | The game's copies of the two controllers, and Shinobi's records of them |
| `taskLists` | 0x8C15C594 | The 7 task lists |
| `gameExitRequest` | 0x8C1520C8 | Set: the frame loop ends, but not while a save is being written |
| `saveRequest` | 0x8C1D138C | Set: the next scene change saves to the card, with autosave on |
| `gameClearFlags` | 0x8C1CD778 | Four bits that open the appendix's items, which the ending and the hours of play set |
| `players` | 0x8C1D1548 | Each player's ship (`Player`) |
| `playerInput` | 0x8C1D1544 | Each player's buttons in this frame (`PlayerInput`) |
| `playerLives` | 0x8C1D13EC | Each player's spare lives, without the ship in play |
| `playerEnergy` | 0x8C1D13E8 | Each player's energy for homing lasers |
| `playerPolarity` | 0x8C1D13EE | Each player's polarity, which the ship of the next stage takes (`playerCreate`) |
| `buttonShot`, `buttonSwitch`, `buttonRelease`, `buttonShotSwitch` | 0x8C1D13F6 to 0x8C1D13FD | Each player's buttons from CONFIG: entries of `buttonMasks`, and in `buttonShotSwitch` a 1 when shot and switch together release |
| `playerScore` | 0x8C1D144C | Each player's score |
| `extendScore` | 0x8C1D1454 | Each player's score for the next extra life |
| `chainSet`, `chain`, `chainMax` | 0x8C1D13F4, 0x8C1D1424, 0x8C1D1448 | Each player's chain: the current set, the chain and the longest chain in this stage |
| `difficulty` | 0x8C15C53C | 0 easy, 1 normal, 2 hard |
| `randomState` | 0x8C1CB8D0 | The random numbers' state (`randomSeed`, `randomNext`, `randomFloat` from -1 to 1) |
| `hpGauge` | 0x8C1E94A0 | The gauge of hit points |
| `gameExtend`, `gameLives` | 0x8C15C534, 0x8C15C540 | The game's EXTEND and PLAYER STOCK, from the options. A demo takes `gameLives` from its record. |
| `hitSlots` | 0x8C1E7598 | The objects that homing lasers can aim at |
| `hitLists` | 0x8C1D1568 | This frame's hit records, in 5 lists |
| `gaugeSlot` | 0x8C0D139C | The slot of the object hit this frame, whose hit points the gauge shows |
| `scrollPosition`, `scrollSpeed` | 0x8C1D1428, 0x8C1D142C | The stage's scroll, floats. Stage commands and scripts wait until it reaches their positions. |
| `scrollTarget`, `scrollStep` | 0x8C1D1430, 0x8C1D1434 | The speed that the scroll moves toward, and by how much a frame |
| `stageEvents` | 0x8C1D1416 | What happened in this frame: 0x02 the bullets vanish, 0x04 a ship was lost, 0x08 the shots and lasers vanish |
| `stageLocks` | 0x8C1D1410 | 20 bytes that the stage program raises and waits on |
| `stagePhase`, `stagePart` | 0x8C1D1400, 0x8C1D13FF | Counters that the stage program raises |
| `pathPosition`, `pathAngles`, `pathValue` | 0x8C1D1538, 0x8C1D1528, 0x8C1D1534 | The camera on its path at `scrollPosition` |
| `sceneCamera` | 0x8C1D1550 | The camera's structure |
| `gamePractice` | 0x8C15C55C | 1 in the practice mode, which returns to the title after the chapter |
| `playersInGame` | 0x8C15C588 | 0x01: player 1 is in the game, 0x02: player 2 |
| `gamePrototype` | 0x8C15C558 | 1 in the prototype mode |
| `gameDemo`, `demoInput` | 0x8C15C584, 0x8C1D143C | 1 in the attract mode, and the read position in the recorded input of a demo or the tutorial |
| `pauseState`, `pausePlayer`, `pauseEnd`, `pauseLocked` | 0x8C15C578, 0x8C15C574, 0x8C15C56C, 0x8C15C570 | The pause (1 paused, 2 at the next frame), who paused, a 1 that ends it, and the players who cannot pause |
| `optionHorizontal` | 0x8C1CD768 | 1: a horizontal screen, 0: vertical |
| `optionRotateButtons` | 0x8C1CD769 | 1: the directions turn with the vertical screen |
| `optionAutosave` | 0x8C1CD76A | 1: the game saves on its own |
| `optionScreen75` | 0x8C1CD76B | 1: the horizontal screen at 75%, 0: at 80% |
| `optionPrototype` | 0x8C1CD77F | 1: the prototype mode, in which each shot costs energy. A shot without enough energy fades out and ends after a few frames. |
| `optionTrial` | 0x8C1CD76C | TRIAL GAME: 1, the game start offers the trial game |
| `optionDifficulty` | 0x8C1CD76D | EASY, NORMAL, HARD, or 3: USER SELECT |
| `optionLives` | 0x8C1CD76E | PLAYER STOCK, the lives at the start |
| `optionExtend` | 0x8C1CD76F | EXTEND: 0 none, 1 every 2,000,000, 2 at 3,000,000 and every 5,000,000, 3 at 6,000,000 and 15,000,000 |
| `optionButtonShot`, `optionButtonSwitch`, `optionButtonRelease`, `optionButtonShotSwitch` | 0x8C1CD770 to 0x8C1CD777 | Each player's buttons in CONFIG, which `buttonShot` and the others get at the start |
| `optionScreenX`, `optionScreenY` | 0x8C1CD77A, 0x8C1CD77B | The offset of the HUD and the other texts over the play, in steps of 4 from -28 to 12, which the pause menu changes |
| `optionVibration` | 0x8C1CD77C | Each player's vibration, on or off |
| `creditsPerGame`, `credits` | 0x8C1CD77E, 0x8C15C50C | The credits that a game starts with, and the ones left. 0xFF: free play |
| `playTime`, `playTimeNextCredit` | 0x8C1CD788, 0x8C1CD784 | The seconds of play, and the time of the next credit |
| `chapterReached` | 0x8C1CD780 | For each difficulty, the highest chapter that a game reached without a continue, outside the practice mode |
| `rankings` | 0x8C1CD78C | The ranking lists, laid out as `RankEntry[4][12][20]` |
| `rankPlaces` | 0x8C1D1406 | This game's place in each of the 6 lists, 0xFF for none |
| `saveHeader`, `saveImage` | 0x8C1CB8D4, 0x8C1CB934 | The card file's header, and its image while the card reads or writes it |
| `difficultyChoice` | 0x8C15C538 | The difficulty that the player chose at the start |
| `gameFrames` | 0x8C15C51C | The frames of this game, without those while a player holds the pause (`pausePlayer`) |
| `gameTwoPlayers` | 0x8C1D1402 | 1 once both players are in the game |
| `gameTrial`, `gameNoRanking` | 0x8C15C57C, 0x8C1D1401 | 1 in a trial game. 1 in a game that enters no ranking: a practice or a trial |
| `screenHorizontal`, `screen75` | 0x8C1CB150, 0x8C1CB14C | The game's copies of `optionHorizontal` and `optionScreen75` |
| `playerContinues` | 0x8C1D13F0 | Each player's continues, up to 255. A player who joins starts with it as the score, up to 9 |
| `continueNext`, `playersContinuing`, `continueTimer` | 0x8C1D13F2, 0x8C1D1411, 0x8C1D140C | Each player's 1 once a join counts as a continue, the players at the continue, and its countdown |
| `gameOver` | 0x8C1D1413 | 1 once the game over runs |
| `frameCount` | 0x8C15C524 | The frames since `gameInit` |
| `chapterScores` | 0x8C1D145C | Three blocks of 6 entries of 8 bytes, for player 1, player 2 and both together (`chapterScoresTotal`): the game's score in entry 0 and each chapter's part in entries 1 to 5, each with the chapter's longest chain, which goes into the entry's `unknown_8` |
