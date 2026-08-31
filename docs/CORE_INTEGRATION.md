# Core Integration Handoff — AI Stream Interface

**For:** whoever is working the CMaNGOS **core** repository (`mangos-classic`,
`mangos-tbc` or `mangos-wotlk`).
**From:** the side working the **playerbots module** (`Rychek4/playerbots`).
**Subject:** a change landed in the module that adds an external-AI control
interface. This document is everything the core side needs to marry to it.

---

## 1. The short version

**The core needs no code changes.** Not one line, not one header, not one
CMake edit.

The change is confined to three files inside the module, adds no new files, and
touches no core API that the module was not already using. What the core side
has to do:

1. Point `src/modules/Bots` at the commit below.
2. Rebuild with `BUILD_PLAYERBOTS` on, as you already do.
3. Set one config value (`AiPlayerbot.CommandServerPort`) — and read §7 before
   you do, because the security posture of that port has changed.

If the build is green and a `claim` over the port answers `ok`, you are done.

---

## 2. Coordinates

| | |
|---|---|
| Module repo | `https://github.com/Rychek4/playerbots` |
| Branch | `claude/read-write-systems-docs-xqxqma` |
| The code commit | `358ae253e28ea3f161970a9dc9f131b58cf6f122` |
| Branched from | `3a34318` — *"Role: Fixed players/bots no longer being seen as tanks when in tanking aura."* |

```
358ae25  AI stream: Implement the read/write interface for external AI control
fc5d994  Docs: Add complete read/write systems reference
0bfd1a7  Add AI Stream Interface design & reference doc
```

Take the **branch tip**, not the SHA above — anything after `358ae25` is
documentation only, including this file. `358ae25` is named so you can identify
the one commit that changes behaviour.

Diff against `master`, as of `358ae25`:

```
docs/AI_STREAM_INTERFACE.md      | 319 ++++++    (new, design record)
docs/READ_WRITE_SYSTEMS.md       | 699 ++++++    (new, full reference)
playerbot/PlayerbotAI.cpp        | 488 ++++--
playerbot/PlayerbotAI.h          |  32 ++-
playerbot/RandomPlayerbotMgr.cpp |   2 +-
```

Two of the five files are documentation. **No new source files**, which matters
for §4.

---

## 3. What actually changed, in one paragraph

An external process can now open the module's existing TCP command port, claim a
single bot, poll a JSON snapshot of that bot's strategic situation, poll a JSON
event array, and push high-level intents back. Intents are routed into the
existing whispered-command pipeline, so no new game logic exists — the whole
command vocabulary comes along for free. All tactical work (combat, targeting,
pathfinding) stays in the engine, untouched.

Five verbs on the existing protocol: `claim`, `release`, `snapshot`, `events`,
`intent <command>`.

The full contract is [`READ_WRITE_SYSTEMS.md`](READ_WRITE_SYSTEMS.md); the design
rationale is [`AI_STREAM_INTERFACE.md`](AI_STREAM_INTERFACE.md). You should not
need either to integrate — this document is meant to be sufficient on its own.

---

## 4. Build

Nothing to do.

* The module's `CMakeLists.txt` builds its sources with `file(GLOB ...)` per
  directory. Since the change added **no new files**, the source list is
  unchanged and `CMakeLists.txt` was not edited.
* No new third-party dependency. The change uses `<atomic>`, `<mutex>`,
  `<queue>`, `<vector>`, `<sstream>` and `<iomanip>` — the last four were already
  in use in the same translation unit, and `<atomic>` is now included explicitly
  by `playerbot/PlayerbotAI.h`.
* No new link dependency. The TCP server it uses is `PlayerbotCommandServer`,
  which already existed and already links boost::asio.
* **C++17 is required, and already was.** The change uses `std::scoped_lock`,
  which the module was already using (`PlayerbotAI.cpp:1142`, the chat-reply
  drain). If your core sets the standard below 17 for the module, it was already
  broken before this change.

---

## 5. The core API contract

This is the section to check if you are worried about breakage. Every core-side
symbol the new code touches was **already in use elsewhere in the module**,
meaning it is already proven to compile on whichever expansion you build.

| Core symbol used by the new code | Already used at |
|---|---|
| `Player::GetPowerType`, `GetPower`, `GetMaxPower`, `POWER_MANA` | `PlayerbotAI.cpp:484` |
| `Player::IsAlive` | `PlayerbotAI.cpp:480` |
| `Player::GetHealth`, `GetMaxHealth` | `PlayerbotAI.cpp:4519` |
| `Player::GetMoney` | `PlayerbotAI.cpp:7186` |
| `Player::GetGroupInvite` | `strategy/actions/InviteToGroupAction.cpp` |
| `Group::GetLeaderName` | `strategy/actions/LeaveGroupAction.cpp` |
| `Group::GetFirstMember`, `GroupReference::next`/`getSource` | `PlayerbotAI.cpp:1489` |
| `Player::GetQuestSlotQuestId`, `GetQuestStatus`, `MAX_QUEST_LOG_SIZE` | `PlayerbotAI.cpp:2768` |
| `QUEST_STATUS_COMPLETE` | `PlayerbotFactory.cpp` |
| `ObjectMgr::GetQuestTemplate`, `Quest::GetTitle` | `PlayerbotAI.cpp:2804` |
| `GetAreaEntryByAreaID`, `AreaTableEntry::zone` / `area_name[0]` | `PlayerbotAI.cpp:2939-2948` |
| `Creature::GetCreatureInfo`, `CreatureInfo::NpcFlags` | `PlayerbotAI.cpp:794`, `GuidPosition.h` |
| `UNIT_NPC_FLAG_*` (questgiver, trainer, flightmaster, innkeeper, banker, auctioneer, repair, vendor, spirithealer) | `RandomPlayerbotMgr.cpp:2882-2886`, `DebugAction.cpp:3315-3328` |
| `SMSG_GROUP_INVITE` | `PlayerbotAI.cpp:188` |
| `Player::isRealPlayer` | `PlayerbotAI.cpp:1797` |
| `Player::GetObjectGuid().GetCounter()` | `PlayerbotAI.cpp:7041` |

**None of these are new requirements on the core.** Every `UNIT_NPC_FLAG_*`
value listed is used unguarded elsewhere in the module, so none of them needs an
expansion `#ifdef`.

The one signature that changed is module-internal:

```cpp
// playerbot/PlayerbotAI.h
std::string HandleRemoteCommand(std::string command, bool fromCommandServer = false);
```

The new parameter is defaulted, so every existing caller compiles unchanged. If
anything on the **core** side calls `PlayerbotAI::HandleRemoteCommand` — we do
not believe anything does — it will still compile, and will correctly *not* get
access to the AI stream verbs. See §7 for why that gate exists.

---

## 6. Configuration and runtime

`AiPlayerbot.CommandServerPort` **already exists** in all three shipped config
templates, commented out with a suggested value of 8888:

```
playerbot/aiplayerbot.conf.dist.in          :969
playerbot/aiplayerbot.conf.dist.in.tbc      :986
playerbot/aiplayerbot.conf.dist.in.wotlk    :981
```

No config schema change was needed and none was made. Uncommenting it is the
whole activation step.

**Three conditions must all hold for the port to listen** — this is pre-existing
behaviour and a common source of "I set the port and nothing is listening":

1. `AiPlayerbot.Enabled = 1`
2. `AiPlayerbot.RandomBotAutologin = 1`
3. `AiPlayerbot.CommandServerPort` non-zero

The server is started from inside the module (`RandomPlayerbotMgr.cpp:269`, in
the `RandomPlayerbotMgr` constructor), not from core startup code. There is
nothing for the core to call.

**Only random bots are addressable.** The request router resolves the GUID via
`RandomPlayerbotMgr::GetPlayerBot`, so alt-bots owned by a logged-in player
return `invalid guid`. Unchanged by this work, but it will be the first thing
that confuses anyone testing.

---

## 7. Security — the one thing to get right

The command port is **unauthenticated** and binds `tcp::v4()` on all interfaces.
That is pre-existing. What changed is the blast radius: the port previously
exposed read-only diagnostics, and now exposes **bot control**.

**Bind it to loopback or firewall it.** If the AI process runs on the same host,
nothing needs to reach that port from outside.

There is a second thing worth understanding, because it constrains any future
refactor on either side. Intents deliberately bypass `PlayerbotSecurity` — an
injected intent has no player behind it, and the port is a server-operator
surface. That is only safe because the verbs are unreachable from in-game chat:

* `PlayerbotAI::HandleCommand` handles a whispered `debug <verb>` by calling
  `HandleRemoteCommand` **before** the security check runs.
* So without a gate, `debug intent <anything>` would let any player who can
  whisper a bot run any command on it with permissions skipped.
* Hence `fromCommandServer`. Only `RandomPlayerbotMgr::HandleRemoteCommand`
  (`RandomPlayerbotMgr.cpp:4005`) passes `true`.

**If you touch that call path, keep the flag.** Any new caller that passes
`true` reopens the hole.

---

## 8. Threading, for when you are reviewing this against core assumptions

CMaNGOS is single-threaded per world/map. The command server runs on separate
boost threads (one per connection).

The new code holds to a strict rule: **the socket thread never touches a game
object.** It copies `std::string`s in and out of buffers under a mutex, and
everything that reads or writes the world happens on the bot's tick, inside
`UpdateAIInternal`.

Worth knowing as context: the module's **pre-existing** remote verbs (`state`,
`position`, `hp`, `travel`, `budget`, …) *do* read live game objects from the
socket thread. That is an old shortcut in the module, not something this change
introduced, and this change does not extend it. If you are auditing thread
safety around the command port, that older code is where to look, not the new
code.

---

## 9. If it does not compile

Given §5, a build failure most likely means an expansion difference we could not
test for. Ordered by likelihood, all in `playerbot/PlayerbotAI.cpp`:

1. **`BuildSnapshot`** (line 6736) — the largest new function, and the one that
   touches the most core types. Quest log, group iteration, area lookup and
   creature flags all live here.
2. **`Group::GetLeaderName()` return type** (line 1564) — we assign it to a
   `std::string`, which works whether it returns `const char*` or `std::string`.
   If your core returns something else, that is the line.
3. **`AreaTableEntry::area_name[0]`** (in `BuildSnapshot`) — this mirrors the
   existing `position` verb exactly, so if it breaks, the `position` verb was
   already broken.
4. **`std::atomic<bool> aiControlled{false};`** in `PlayerbotAI.h:716` — needs
   `<atomic>`, which the header now includes at line 13.

**Please report a failure back rather than patching around it**, unless the fix
is obviously local. We would rather correct it at the source than have the two
trees diverge.

---

## 10. How to verify end to end

No Python needed. With the server up and at least one random bot logged in, get
a bot GUID (the low part — the module logs these, or use the `.bot` commands),
then over a raw TCP connection to the port, one request per line:

```
state,1234                      -> non-combat          (pre-existing verb, proves the port works)
claim,1234                      -> ok
snapshot,1234                   -> {"error":"no snapshot yet"}   (for up to one tick)
snapshot,1234                   -> {"v":1,"guid":1234,"name":"...","zone":"...",...}
events,1234                     -> []
events,1234                     -> [{"type":"combat"}]           (after pulling a mob)
intent goto Stormwind,1234      -> ok
release,1234                    -> ok
snapshot,1234                   -> {"error":"not claimed"}
```

Success criteria: `snapshot` returns one line of valid JSON, `events` returns one
line of valid JSON, and the bot keeps behaving autonomously the whole time — a
claimed bot is *nudged*, not driven, so it should never look idle or stuck.

Two protocol details that will bite during testing:

* The request is split on its **first comma**, so intent free text must not
  contain one. `intent say sure, on my way,1234` truncates at "sure".
* Responses are newline-delimited. The new verbs always return a single line.
  Three **pre-existing** verbs (`travel`, `traveldetail`, `budget`) embed
  newlines and will desync a line-based client — they were written for the
  in-game `debug` path. Not new, but do not use them to test the socket.

---

## 11. Keeping the two sides married

* **Nothing here requires lockstep.** The module change is self-contained, so a
  core update and a module update can land independently.
* **If the module's `master` moves**, we rebase this branch onto it rather than
  merging, so the three commits above stay identifiable.
* **If you need a change on our side** — a different verb, a field in the
  snapshot, a config knob for the 2-second snapshot interval — ask rather than
  patching the module in-place from the core tree. A local edit under
  `src/modules/Bots` will be lost the next time the pointer moves.
* **Open items on our side**, in case they affect your planning: this has never
  been through a compiler (the module builds only against a full core tree, which
  we do not have), the snapshot interval is a constant rather than config, and
  the comma limitation in §10 would need a protocol change in
  `RandomPlayerbotMgr::HandleRemoteCommand` to fix.

---

## 12. What we would like back

1. **Confirmation of which core repo and expansion you are on** — classic, tbc
   or wotlk. Everything above is expansion-agnostic as far as we can tell, but a
   green build on a named expansion is the first real verification this change
   has had.
2. **The first build result**, pass or fail, with the compiler error if it fails.
3. **Whether anything on the core side calls `PlayerbotAI::HandleRemoteCommand`.**
   We believe nothing does. If something does, we want to know before it becomes
   a security question.
