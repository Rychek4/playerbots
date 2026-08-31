# Read/Write Systems — Complete Reference

How data gets **out of** a playerbot and how commands get **into** one — the
engine plumbing that was already here, and the AI Stream layer built on top of
it.

**Status legend used throughout:**

| Marker | Meaning |
|---|---|
| **[PRE-EXISTING]** | Engine plumbing that was already here before the AI stream work. |
| **[NEW]** | Added by the AI stream implementation. |

All of it is in the tree. Line references are against the current branch.

Companion documents: [`CORE_INTEGRATION.md`](CORE_INTEGRATION.md) is the handoff
for whoever works the CMaNGOS **core** repository — what the core must do to
pick this up (almost nothing), the core API surface relied on, and the security
constraint on the command port. [`AI_STREAM_INTERFACE.md`](AI_STREAM_INTERFACE.md) is the
*decision record* — why the design is shaped this way, the control model
argument, the build order. **This** document is the *reference* — the full
read and write surface, end to end, including everything the engine already
does that the new layer rides on.

> **Not verified by a build.** The module compiles only against a full CMaNGOS
> tree, which is not present in this repository. The JSON assembly in
> `BuildSnapshot` was exercised in isolation against stub game state — valid
> single-line JSON for empty lists, over-cap lists, and names and quest titles
> containing quotes, backslashes, newlines and control characters — but the
> module itself has not been compiled or run on a server. Treat first build and
> first `claim` on a live server as the remaining verification.

---

## 1. The big picture

There are exactly **two directions of data flow**, and each has more than one
implementation in this codebase. Everything in this document is one of these
four boxes.

```
                         ┌──────────────────────────────┐
                         │   External process (Python)  │
                         └───────┬──────────────▲───────┘
                                 │              │
                       WRITE     │              │   READ
                    (intents,    │              │  (snapshot,
                     commands)   │              │   events, verbs)
                                 │              │
        ═══════════ TCP :CommandServerPort, newline-delimited ═══════════
                                 │              │
                       ┌─────────▼──────────────┴─────────┐
                       │  PlayerbotCommandServer          │   socket threads
                       │  RandomPlayerbotMgr::            │   (boost::asio,
                       │      HandleRemoteCommand         │    1 per conn)
                       └─────────┬──────────────▲─────────┘
                                 │              │
              ─ ─ ─ ─ ─ ─ ─ ─ ─ ─│─ THREAD BOUNDARY ─│─ ─ ─ ─ ─ ─ ─ ─ ─ ─
                                 │              │
                       ┌─────────▼──────────────┴─────────┐
                       │  PlayerbotAI (per bot)           │   world thread
                       │  queue in  ──► tick ──► snapshot │   (tick)
                       │                        + events  │
                       └─────────┬──────────────▲─────────┘
                                 │              │
                       ┌─────────▼──────────────┴─────────┐
                       │  Engine: triggers, strategies,   │
                       │  actions, AI_VALUEs, TravelMgr   │
                       └──────────────────────────────────┘
```

In-game chat (a real player whispering the bot) enters the **same write path**
one layer down, at `PlayerbotAI::HandleCommand`. The AI stream and a human
whisper are indistinguishable by the time they reach the engine — that is the
central reuse this design buys.

---

## 2. Layer 0 — the plumbing already in the tree

### 2.1 Transport **[PRE-EXISTING]**

`playerbot/PlayerbotCommandServer.cpp`

* boost::asio TCP listener, **one thread per connection** (`boost::thread` per
  accepted socket, `PlayerbotCommandServer.cpp:60-66`).
* Port from `sPlayerbotAIConfig.commandServerPort`, config key
  `AiPlayerbot.CommandServerPort` (`PlayerbotAIConfig.cpp:277`). **Default 0 =
  server disabled.** Nothing listens unless you set it.
* Protocol: strictly **newline-delimited request/response**. `ReadLine()`
  accumulates until `\n`; each line is one request, and exactly one response
  line is written back (`PlayerbotCommandServer.cpp:44-52`).
* The connection is long-lived and stateless — any request may address any bot.

**The single hard constraint this imposes:** a response may never contain a
newline. Multi-line output silently corrupts the stream by desynchronising the
client's line framing. (Note that several *existing* verbs violate this —
see §2.3.)

There is **no push channel and no server-initiated message.** Everything is
client-polled. This is deliberate: an LLM reasons on a multi-second cadence, so
sub-second event delivery buys nothing and would cost an entire async
subsystem.

### 2.2 Routing **[PRE-EXISTING]**

`RandomPlayerbotMgr::HandleRemoteCommand` — `RandomPlayerbotMgr.cpp:3986`

Request wire format is `"<command>,<guid>"`:

1. Split on the **first comma**. No comma → `"invalid request: <request>"`.
2. `atoi` the tail as a bot GUID (low part).
3. `GetPlayerBot(guid)` → not found, or no `PlayerbotAI` → `"invalid guid"`.
4. Otherwise delegate to `ai->HandleRemoteCommand(command)`.

Consequences worth knowing:

* The command half **may itself contain spaces** (`"budget spells,1234"`
  works) but **may not contain a comma** — the first comma always wins.
* GUID parsing is `atoi`, so garbage parses as `0` and falls out as
  `"invalid guid"`.
* Only **random bots** registered with `RandomPlayerbotMgr` are addressable.

### 2.3 Read verbs that exist today **[PRE-EXISTING]**

`PlayerbotAI::HandleRemoteCommand` — `PlayerbotAI.cpp:6871`. A flat
`if/else if` chain over the command string; unmatched input returns
`"invalid command: <command>"`.

| Verb | Returns | Source of truth | Single-line? |
|---|---|---|---|
| `state` | `combat` / `dead` / `non-combat` / `unknown` | `PlayerbotAI::currentState` | ✅ |
| `position` | `x y z map orientation \|Zone Name\|` | `bot->GetPositionX/Y/Z()`, `GetMapId()`, `GetAreaEntryByAreaID` | ✅ |
| `tpos` | current target's `x y z map orientation`, empty if no target | `AI_VALUE(Unit*, "current target")` | ✅ |
| `target` | current target's name, empty if none | `AI_VALUE(Unit*, "current target")` | ✅ |
| `hp` | `"<bot>%"`, or `"<bot>% / <target>%"` | `GetHealth()/GetMaxHealth()` | ✅ |
| `combat` | verbose combat diagnostic — unit flags, attacker count, victim, AI target + validity, selection GUID | mixed | ✅ |
| `strategy` | `currentEngine->ListStrategies()` | active engine | ✅ |
| `action` | `currentEngine->GetLastAction()` | active engine | ✅ |
| `values` | `GetAiObjectContext()->FormatValues()` — **very large** | whole value context | ⚠️ |
| `travel` | destination + status + time left + retries | `AI_VALUE(TravelTarget*, "travel target")` | ❌ embeds `\n` |
| `traveldetail` | the above plus distance, location, condition evaluation | same | ❌ embeds `\n` |
| `budget` / `budget <substr>` | money on hand, free money, and a per-purpose needed/available table | `bot->GetMoney()`, `AI_VALUE2(uint32,"money needed for"/"free money for", …)` | ❌ embeds `\n` |

> ⚠️ **Framing hazard.** `travel`, `traveldetail` and `budget` emit embedded
> newlines, and `values` can emit an enormous single line. They were written for
> human eyes via the in-game `debug` path (§2.5), not for a socket client. Any
> machine consumer should treat these three as unsafe over TCP, or read
> `travel target` state through the snapshot instead.

Everything here is a **pull**: the caller asks, the engine reads live state and
answers on the socket thread. See §4 for why that is a latent hazard, and why
the new layer does not repeat it.

### 2.4 The write path that exists today **[PRE-EXISTING]**

This is the pipeline every in-game command already travels, and the one the
intent router reuses wholesale.

```
whisper / party / raid / guild / addon message
        │
        ▼
PlayerbotAI::HandleCommand(type, text, fromPlayer, lang)      PlayerbotAI.cpp:1375
        │   • strips sPlayerbotAIConfig.commandPrefix
        │   • maps "#w " / "#p " / "#r " / "#a " / "#g " to a reply channel
        │   • chatFilter.Filter(...) — resolves who the command is addressed to
        │   • "debug …"  → out-of-band, see §2.5
        │   • IsAllowedCommand() || PlayerbotSecurity::CheckLevelFor(ALLOW_ALL)
        │   • a few commands handled inline: reset, logout, logout cancel,
        │     wait <sec>, "d "/"do " (DoSpecificAction), "queue " (delayed)
        ▼
chatCommands.push(ChatCommandHolder{text, owner, type, [when]})   ── enqueue
        │                                            PlayerbotAI.h:707
        ▼   ····· thread/tick boundary — nothing above touched the engine ·····
PlayerbotAI::HandleCommands()                                  PlayerbotAI.cpp:1096
        │   • pops each holder; if holder.GetTime() is in the future,
        │     it is re-queued (this is how "queue"/staggered commands work)
        ▼
ExternalEventHelper::ParseChatCommand(command, owner)   strategy/ExternalEventHelper.h
        │   • unwraps a [command:…] chat link → forceCommand = true
        │   • tries the whole string as a trigger name
        │   • then walks right-to-left splitting on spaces: longest trigger
        │     name that matches wins, remainder becomes the parameter
        │   • falls back to "c" / "t" (chat/talk) triggers for real players
        ▼
Trigger::ExternalEvent(param, owner)  →  engine picks up the event
        │
        ▼
Strategies / Actions (TravelAction, TrainerAction, AcceptInvitationAction, …)
```

Two properties matter enormously for the AI layer:

1. **The command vocabulary is the trigger namespace.** Anything registered as
   a trigger is reachable by name. Adding an intent verb requires no new
   parsing — only that a trigger already exists.
2. **Longest-match-from-the-right parameter splitting** means `goto Stormwind`
   resolves as trigger `goto` with param `Stormwind` without any per-command
   argument parser.

### 2.5 The `debug` back door **[PRE-EXISTING]**

`PlayerbotAI.cpp:1449-1456`. Whispering a bot `debug <verb>` calls
`HandleRemoteCommand(verb)` directly and sends the result back as a
`CHAT_MSG_ADDON` packet to the whispering player. This is the in-game twin of
the TCP read path: **same verb table, different transport.** It notably runs
*before* the security check, so it is available to anyone who can whisper the
bot.

That last property is why `HandleRemoteCommand` takes a `fromCommandServer`
flag. The AI stream verbs (§3.2) are gated on it and only
`RandomPlayerbotMgr::HandleRemoteCommand` passes `true`. Without the gate,
`debug intent <anything>` would let any player who can whisper a bot run any
command on it with `PlayerbotSecurity` bypassed — the intent path deliberately
skips that check (§3.5), which is safe only because it is not reachable from
in-game chat.

### 2.6 Outbound chat (the bot talking back) **[PRE-EXISTING]**

Distinct from the read path — this is the bot *emitting*, not being polled.

* `PlayerbotAI::QueueChatResponse(...)` — `PlayerbotAI.cpp:8978`. Takes
  `chatRepliesMutex`, pushes a `ChatQueuedReply` with a randomised send time
  (`+10-20s`, or `15-30s` in combat) to look human. Callable off-tick.
* Drained at the top of `PlayerbotAI::UpdateAIInternal` — `PlayerbotAI.cpp:1140-1161`
  — under the same mutex, with not-yet-due replies re-queued.
* `TellPlayer` / `TellPlayerNoFacing` (`PlayerbotAI.cpp:3669` / `3511`) are the
  immediate, on-tick variants used by actions.

**This queue+mutex+tick-drain triple is the pattern the AI stream copies.** It
is the codebase's own established answer to "a non-world thread needs to hand
work to a bot."

### 2.7 The LLM interface **[PRE-EXISTING]**

`PlayerbotLLMInterface` (`PlayerbotLLMInterface.h/.cpp`) is a *separate,
outbound* system: the bot builds a prompt and calls out to an LLM endpoint for
chat flavour. It is not part of the command-server surface and does not carry
intents. Two pieces are directly reusable by the stream layer:

* `SanitizeForJson(const std::string&)` — escaping for embedding arbitrary game
  text (names, quest titles, whisper bodies) into a JSON payload.
* `ParseResponse(...)` / `LimitContext(...)` — prompt/response handling, useful
  if snapshot text ever needs truncation.

### 2.8 Threading model **[PRE-EXISTING — and the thing to be careful about]**

| Runs on | What |
|---|---|
| **Socket thread** (one per TCP connection) | `session()`, `RandomPlayerbotMgr::HandleRemoteCommand`, and *today* the entire body of `PlayerbotAI::HandleRemoteCommand` |
| **World/map thread** (the tick) | `UpdateAIInternal`, `HandleCommands`, all triggers, strategies, actions, every `AI_VALUE` |

CMaNGOS is single-threaded per world/map. **The existing read verbs read live
game objects from the socket thread.** They are a pre-existing shortcut in this
codebase — usually harmless because the reads are small and the port is
normally off, but a genuine data race (and a use-after-free window if the bot
logs out mid-read). The design in §3 does not extend that pattern.

---

## 3. Layer 1 — the AI Stream Interface **[NEW]**

Implemented in `PlayerbotAI.h` / `PlayerbotAI.cpp`, plus one line in
`RandomPlayerbotMgr.cpp`. Full rationale in
[`AI_STREAM_INTERFACE.md`](AI_STREAM_INTERFACE.md).

### 3.1 Control model: *nudge*, not *drive*

A claimed bot **keeps running its normal autonomous strategies.** AI intents are
high-level overrides layered on top. Send nothing and the bot behaves like any
other random bot — it never idles, never looks broken, never needs the AI to
know how to fight or path.

The split is: **strategy is the AI's, tactics are the engine's.** The AI is told
*that* it is in combat; it is never asked *how* to fight. This is the single
decision that keeps the C++ footprint at ~150 lines instead of a new subsystem.

### 3.2 The full verb table

Every message is a line `"<verb> <args>,<guid>"` on the existing port.

**Control**

| Verb | Response | Effect |
|---|---|---|
| `claim` | `ok` | Sets `aiControlled = true` for this bot: snapshot rebuilding and event buffering switch on **for this bot only**. Clears any stale buffers, so re-claiming is a clean reset. |
| `release` | `ok` | Clears the flag and drops all three buffers. The bot carries on autonomously — it never stopped. |

**Read**

| Verb | Response | Semantics |
|---|---|---|
| `snapshot` | single-line JSON object | The strategic situation report. Returns the **cached** string built on the last tick — never reads live state. Idempotent. |
| `events` | single-line JSON array | Everything buffered since the last poll. **Destructive read** — the buffer is cleared. |

**Write**

| Verb | Response | Semantics |
|---|---|---|
| `intent <command>` | `ok` | Strips `intent `, trims, enqueues the remainder, returns immediately. Execution happens on the next tick. **`ok` means accepted, not performed.** |

**Error responses.** Anything that can be detected at enqueue time is reported
as single-line JSON rather than `ok`:

| Response | Cause |
|---|---|
| `{"error":"not claimed"}` | `snapshot`, `events` or `intent` on a bot that has not been claimed. |
| `{"error":"no snapshot yet"}` | Claimed, but the bot has not ticked since. Poll again. |
| `{"error":"empty intent"}` | `intent` with nothing after the verb. |
| `{"error":"intent queue full"}` | More than 64 intents queued and undrained. |

Anything not detectable at enqueue time is not reported at all — see §3.5.

> **Two protocol constraints inherited from §2.2, and they bite hardest here.**
> The request is split on its **first comma**, so an intent carrying free text
> must not contain one: `intent whisper Bob sure, be right there,1234` truncates
> at "sure". And the verbs are reachable **only from the command server** — the
> in-game `debug` path cannot claim a bot or send it intents (§2.5).

### 3.3 Read side — the snapshot

Single-line JSON, rebuilt on the bot's tick (throttled to 2s) and cached as a
string. The socket returns a **copy of that string** under the mutex.

```json
{
  "v": 1,
  "guid": 1234,
  "name": "Aelric",
  "zone": "Westfall",
  "position": {"map": 0, "x": -10586.2, "y": 1035.5, "z": 34.1},
  "level": 14,
  "class": "Warrior",
  "hp": "high",
  "mana": "high",
  "state": "non-combat",
  "money": "1g20s45c",
  "trainable_spells": 3,
  "in_group": true,
  "group_members": ["Bairdbiz"],
  "quests": [{"id": 5261, "title": "The Coast Isn't Clear", "status": "incomplete"}],
  "nearby": [{"name": "Marshal Dughan", "type": "questgiver"}]
}
```

Design rules baked into the schema:

* **Bands, not numbers.** `hp`/`mana` report `critical` / `low` / `medium` /
  `high`. A strategic reasoner does not need `73%`, and bands keep the LLM from
  attempting tactical micro-decisions it will do badly. The cut points are the
  server's own existing thresholds, so they track whatever the operator has
  tuned: health uses `AiPlayerbot.CriticalHealth` / `LowHealth` / `MediumHealth`
  (20 / 50 / 70 by default), mana uses `LowMana` / `MediumMana` (15 / 40) with
  75 as the top of `medium`. A class with no mana bar reports `"mana":"none"`.
* **Bounded lists.** At most 10 `nearby` NPCs and 40 `group_members`; the quest
  log is bounded by `MAX_QUEST_LOG_SIZE` already. `nearby` is filtered to NPCs
  the AI could form an intent about — questgiver, trainer, flightmaster,
  innkeeper, banker, auctioneer, repair, vendor, spirithealer — because
  everything else is noise it would have to reason past.
* **Rebuilt at most every 2 seconds** and cached (`AI_STREAM_SNAPSHOT_INTERVAL`).
  A poll between rebuilds gets the last one.
* **`v` from day one.** The schema will grow; consumers pin on `v`.
* **Additive growth only.** Fields may be added freely; removing or retyping one
  is a `v` bump.
* **All free text passes through `SanitizeForJson`** — quest titles, NPC and
  player names are player- and DB-controlled and will otherwise break framing.

### 3.4 Read side — the event buffer

`events` returns an array of `{"type": …}` objects and clears the buffer.

| `type` | Extra fields | Emitted from |
|---|---|---|
| `combat` | — | `PlayerbotAI::ChangeEngine(BOT_STATE_COMBAT)` — `PlayerbotAI.cpp:2037` |
| `non-combat` | — | `ChangeEngine(BOT_STATE_NON_COMBAT)` |
| `dead` | — | `ChangeEngine(BOT_STATE_DEAD)` |
| `whisper` | `from`, `text` | `HandleCommand` — `PlayerbotAI.cpp:1375` |
| `group_invite` | `from` | `HandleBotOutgoingPacket`, `SMSG_GROUP_INVITE` — `PlayerbotAI.cpp:1552` |
| `hp_critical` | `pct` | `CheckAiStateEvents` on tick — `PlayerbotAI.cpp:6676` |
| `arrived` | `destination` | `CheckAiStateEvents` on tick |
| `level_up` | `level` | `CheckAiStateEvents` on tick |

`ChangeEngine` is the reason the combat events are three lines of code: it is
the **single choke point** for every combat/non-combat/dead transition in the
bot. There is no other place state changes.

The other five are worth a note each:

* **`whisper`** is pushed *before* the security check, so a claimed bot hears
  strangers, not only players allowed to command it — which is the whole point,
  since the AI decides for itself whether to answer. Two filters apply: the
  sender must be a real player and must not be the bot itself, because several
  actions synthesise commands by calling `HandleCommand` with `CHAT_MSG_WHISPER`
  on themselves. A whisper containing the command separator is reported once per
  part by the existing recursion rather than twice.
* **`group_invite`** reads the inviter from `bot->GetGroupInvite()`, which is
  already set by the time the packet arrives, rather than parsing the packet
  body — whose layout differs between client versions.
* **`level_up`** compares `bot->GetLevel()` on the tick instead of reading
  `SMSG_LEVELUP_INFO`. Version-independent, and the level reported is always the
  one the bot actually has.
* **`arrived`** is the `TRAVEL` → `WORK` edge of the travel target, since the
  engine's flow is `PREPARE` → `TRAVEL` → `WORK`.
* **`hp_critical`** is edge-triggered against `AiPlayerbot.CriticalHealth`, so a
  long fight spent at low health reports once, not once per tick.

On `claim`, the edge detectors are baselined against current state, so the AI is
not handed a `level_up` or an `arrived` for something that happened before it
was watching. Health is deliberately **not** baselined: a bot claimed while
already in trouble reports `hp_critical` on the first poll.

Semantics to hold to:

* **Chronological order** within a poll; the array is the buffer in order.
* **Destructive read** — a dropped response loses events. Acceptable: the
  snapshot is authoritative for *state*, events are hints about *changes*.
* **The buffer is bounded** at 256 events (`AI_STREAM_MAX_EVENTS`), dropping
  oldest, so a claimed bot nobody polls cannot grow without limit.
* Events are appended **on the tick only**, never from the socket thread.

### 3.5 Write side — the intent router

```
"intent goto Stormwind,1234"
        │  socket thread
        ▼
strip "intent " → lock aiStreamMutex → aiInboundIntents.push("goto Stormwind")
        │                                       → return "ok" immediately
        ▼   ····· tick boundary ·····
UpdateAIInternal / HandleCommands: lock, swap the queue out, unlock
        ▼
ExternalEventHelper::ParseChatCommand("goto Stormwind", <bot as owner>)
        ▼
existing trigger → existing action → TravelMgr routes Westfall → Stormwind
```

**No new game logic.** The router is `DrainAiIntents`
(`PlayerbotAI.cpp:6648`) plus one branch in the verb dispatcher: strip prefix,
enqueue, drain, delegate. The whole existing command vocabulary comes along for
free:

| Intent | Backed by |
|---|---|
| `intent goto <destination>` | Travel system (`TravelAction` / `GoAction`) |
| `intent train` | `TrainerAction` |
| `intent accept` | `AcceptInvitationAction` |
| `intent quest <id>` | Quest actions |
| `intent follow <name>` | Follow action |
| `intent flee` | Flee strategy |
| `intent stop` | Stop/reset movement |
| `intent say <text>` | `SayAction` |
| `intent whisper <name> <text>` | `SayAction` / `QueueChatResponse` |
| `intent strategy +flee` / `-grind` | Strategy toggle |

**The one nuance — permissions.** The normal path checks
`PlayerbotSecurity::CheckLevelFor(…)` (`PlayerbotSecurity.h:38`) against a
`Player* owner`. An injected intent has no player behind it. Intents are routed
as `master ? master : bot` and go straight to `ParseChatCommand`, **bypassing
the security-level check** — the AI is trusted server-side, by the same argument
that makes the command port a server-operator tool rather than a player-facing
one. Everything else about the pipeline is unchanged.

That bypass is exactly why the stream verbs are gated on `fromCommandServer`
(§2.5). The in-game `debug` path reaches the verb dispatcher *ahead* of the
security check; if `intent` were reachable from there, any player who can
whisper a bot could run any command on it with permissions skipped.

**Error reporting past the enqueue is deliberately weak.** `ok` acknowledges
the enqueue, and the errors in §3.2 cover what is knowable at that moment. If
`ParseChatCommand` finds no matching trigger a tick later, it is logged at debug
level and nothing is reported back — exactly as an unrecognised whisper command
is silently dropped today. The AI observes outcomes through the next snapshot,
not through the intent's return value. This is a conscious trade: it keeps the
router stateless and one-way.

### 3.6 Per-bot state added to `PlayerbotAI`

Mirrors the `chatReplies` + `chatRepliesMutex` pattern already at
`PlayerbotAI.h:707-709`. `PlayerbotAI.h:711-726`:

```cpp
// Shared with the command-server thread.
std::atomic<bool>         aiControlled{false};
std::mutex                aiStreamMutex;
std::queue<std::string>   aiInboundIntents;   // socket -> bot   (WRITE)
std::vector<std::string>  aiOutboundEvents;   // bot -> socket   (READ)
std::string               aiSnapshotCache;    // rebuilt on tick (READ)
// Tick-owned edge detection. Never read or written off the tick.
bool                      aiStreamWasControlled = false;
time_t                    aiSnapshotTime = 0;
bool                      aiHealthCritical = false;
uint8                     aiLastTravelStatus = 0;
uint32                    aiLastLevel = 0;
```

`aiControlled` is atomic rather than mutex-guarded for one reason: it is tested
on every `ChangeEngine` call for every bot on the server, and taking a lock
there to learn "no, not claimed" would be a real cost for a pure no-op.

The split matters. The first group crosses the thread boundary and is guarded.
The second group is **tick-owned**: written and read only from
`UpdateAiStream`, never from the socket. That is why `claim` does not reset the
edge detectors itself — it flips `aiControlled`, and the next tick notices the
transition and baselines them. A socket thread writing `aiLastLevel` would be
the same race the whole design exists to avoid.

**Every one of these is gated on `aiControlled`.** A server running hundreds of
autonomous random bots pays one atomic bool test per tick per bot and nothing
else.

---

## 4. The invariants

These are the rules the whole design exists to satisfy. Violating any one of
them is a crash or a race, not a style problem.

1. **No game object is ever touched from a socket thread.**
   The socket may read and write `aiSnapshotCache`, `aiOutboundEvents` and
   `aiInboundIntents` — plain strings — and nothing else. Not `bot->`, not
   `AI_VALUE`, not a `Unit*`.
2. **All game state is read on the tick.** `BuildSnapshot()` runs inside
   `UpdateAIInternal`, where the world thread already owns everything.
3. **Everything crossing the boundary is a `std::string` copy under
   `aiStreamMutex`.** No pointers, no references, no iterators, no
   `const std::string&` handed to another thread.
4. **All writes execute on the tick**, through the existing command pipeline.
   The socket only ever enqueues.
5. **Hold the mutex for the copy, not for the work.** Lock → copy or swap →
   unlock → then serialise/parse. Never call into the engine holding it.
6. **One line out, always.** Everything the stream verbs return is single-line
   JSON. Free text is sanitised.
7. **Nothing costs anything unless `aiControlled`.**

Note that invariants 1 and 2 are *stricter than the existing code* — today's
read verbs (§2.3) read live objects off-thread. The new layer is deliberately
safer than what it sits beside, and the existing verbs are left alone rather
than being made a precedent.

---

## 5. Where every field comes from

Everything the snapshot reports is **already computed by the engine.** Nothing
here is a new query; the snapshot is a serialiser, not a data source.

| Need | Source | Notes |
|---|---|---|
| "Do I have a skill to train?" | `AI_VALUE(std::vector<TrainerSpell const*>, "trainable spells")` | `strategy/values/TrainerValues.cpp:111`. Derived from the **global** `GAI_VALUE("trainable spell map")`, filtered by class/level/prereqs via `bot->GetTrainerSpellState()`. **Works anywhere — no trainer has to be nearby.** This is what lets the AI decide "go to town and train" from the field. |
| Where trainers are | `AvailableTrainersValue` | `strategy/values/TrainerValues.cpp:169` |
| Zone / area | `sServerFacade.GetAreaId(bot)` + `GetAreaEntryByAreaID` | same as the `position` verb |
| Position | `bot->GetPositionX/Y/Z()`, `GetMapId()` | same as `position` |
| HP / mana | `bot->GetHealth()`, `GetMaxHealth()`, power | same as `hp`; **banded** for the snapshot |
| Combat state | `PlayerbotAI::currentState` (`BotState`) | same as `state` |
| Money | `bot->GetMoney()`, `AI_VALUE2(uint32, "free money for", …)` | same as `budget`; `ChatHelper::formatMoney` for display |
| Current target | `AI_VALUE(Unit*, "current target")` | same as `target` |
| Group | `bot->GetGroup()` | iterate `GroupReference` for names |
| Quest log | quest-log AI values | |
| Nearby units of interest | nearest-unit AI values | |
| Travel target / status | `AI_VALUE(TravelTarget*, "travel target")` | same data as `travel`, but structured and single-line |

---

## 6. End-to-end walkthrough

Python has sent `claim,1234`, polls `events,1234` about once a second, and pulls
a snapshot when it decides to reason.

```
# 1. A real player whispers the bot
→ events,1234
← [{"type":"whisper","from":"Bairdbiz","text":"hey, want to group up and quest?"}]
→ intent whisper Bairdbiz Sure! Invite me and I'll head your way.,1234
← ok

# 2. The player invites
→ events,1234
← [{"type":"group_invite","from":"Bairdbiz"}]
→ intent accept,1234
← ok

# 3. The AI decides what to do next
→ snapshot,1234
← {"v":1,"zone":"Westfall","level":14,"hp":"high","money":"1g20s",
   "trainable_spells":3,"in_group":true,"group_members":["Bairdbiz"]}
# trainable_spells:3 -> train before questing
→ intent goto Stormwind,1234
← ok
# TravelMgr computes the whole Westfall -> Stormwind route in C++.

# 4. Gnolls attack en route
→ events,1234
← [{"type":"combat"}]
# Nudge mode: the AI does nothing. The combat strategy fights automatically.
# Had a later poll shown {"type":"hp_critical"}, the AI could send `intent flee`.

# 5. Arrival
→ events,1234
← [{"type":"arrived","destination":"Stormwind"}]
→ intent train,1234
← ok
→ snapshot,1234
← {…,"trainable_spells":0}     # confirmed done; move on to questing
```

Note what the AI never had to know: how to fight gnolls, how to path across two
zones, which trainer teaches Warriors, or where he stands.

---

## 7. What was built, and where **[NEW]**

`PlayerbotAI.h` and `PlayerbotAI.cpp`, plus one line in `RandomPlayerbotMgr.cpp`.
No new files, no `CMakeLists.txt` change, no new socket, no new thread.

| Location | Change |
|---|---|
| `PlayerbotAI.h:378-380` | `IsAiControlled`, `PushAiEvent`, `UpdateAiStream`; `HandleRemoteCommand` gains `bool fromCommandServer = false` |
| `PlayerbotAI.h:693-695` | `DrainAiIntents`, `CheckAiStateEvents`, `BuildSnapshot` — private, tick-only |
| `PlayerbotAI.h:711-726` | The state block in §3.6 |
| `PlayerbotAI.cpp:6871` | `HandleRemoteCommand`: the five stream verbs, gated on `fromCommandServer`, ahead of the existing chain |
| `PlayerbotAI.cpp:1165` | `UpdateAIInternal` calls `UpdateAiStream()` after the chat-reply drain and before `DoNextAction`, so an intent takes effect the same tick |
| `PlayerbotAI.cpp:2037` | `ChangeEngine`: three `PushAiEvent` lines at the state choke point |
| `PlayerbotAI.cpp:1375` | `HandleCommand`: the `whisper` event, ahead of the security check |
| `PlayerbotAI.cpp:1552` | `HandleBotOutgoingPacket`: the `group_invite` event |
| `PlayerbotAI.cpp:6514-6869` | The new block: the `AI_STREAM_*` constants and file-static helpers, then `PushAiEvent`, `UpdateAiStream`, `DrainAiIntents`, `CheckAiStateEvents`, `BuildSnapshot` |
| `RandomPlayerbotMgr.cpp:4005` | The one caller that is the command server passes `fromCommandServer = true` |

**How to exercise it.** Set `AiPlayerbot.CommandServerPort`, then, against a
random bot's GUID:

```
claim,1234          -> ok
snapshot,1234       -> {"v":1,...}          (or {"error":"no snapshot yet"} for one tick)
events,1234         -> [] then [{"type":"combat"}] etc.
intent goto Stormwind,1234  -> ok
release,1234        -> ok
```

`snapshot` and `events` are safe to poll from a plain telnet client: they return
strings the tick already prepared, so unlike the legacy verbs in §2.3 they read
no game state at all.

## 8. Open items

* **First compile and first live `claim`** — the module builds only against a
  full CMaNGOS tree, which this repository does not contain, so none of this has
  been through a compiler. The JSON assembly was tested in isolation; everything
  else rests on reading the code.
* **`AI_STREAM_SNAPSHOT_INTERVAL` is a constant, not config.** 2 seconds, chosen
  so a claimed bot does not rebuild JSON every tick. If it wants tuning per
  server it needs the usual `PlayerbotAIConfig` plumbing.
* **Intent free text cannot contain a comma** (§3.2). `intent say` and
  `intent whisper` are the ones that will hit this. Fixing it means changing how
  `RandomPlayerbotMgr::HandleRemoteCommand` splits the request — splitting on
  the *last* comma would do it, at the cost of changing an existing protocol.
* **`trainable spells` is recalculated every 5s per claimed bot** (the value's
  own check interval) and walks the global trainable spell map. Fine for a
  handful of claimed bots; worth measuring before claiming dozens.
* **Newline-unsafe legacy verbs** — `travel`, `traveldetail`, `budget` (§2.3).
  Leave them for the in-game `debug` path, or add single-line variants.
* **Security** — the command port is unauthenticated today. Controlling a bot is
  a limited blast radius, but consider binding to loopback or requiring a token
  before exposing it. This is a pre-existing property, not something the stream
  layer introduces; the intent bypass (§3.5) does make it worth a second look.
* **Schema versioning discipline** — additive changes only without a `v` bump.
* **Director mode (future, additive)** — on `claim`, suppress the autonomous
  decision strategies so the bot does *only* what the AI says, with combat and
  pathing still hardcoded. Reuses the same event buffer; needs a suppression
  strategy plus a safe idle default.
* **Push channel (future, additive)** — a second outbound port that pushes
  events rather than being polled. It would drain the *same* buffer, so nothing
  in this design is wasted if it is ever wanted.

---

## 9. File index

| Symbol | File | Role |
|---|---|---|
| `PlayerbotCommandServer` | `playerbot/PlayerbotCommandServer.cpp` | TCP listener, thread per connection, newline framing |
| `RandomPlayerbotMgr::HandleRemoteCommand` | `playerbot/RandomPlayerbotMgr.cpp:3986` | Splits `cmd,guid`, resolves the bot |
| `PlayerbotAI::HandleRemoteCommand` | `playerbot/PlayerbotAI.cpp:6871` | Per-bot verb dispatcher — the read surface, extended by the stream verbs; `fromCommandServer` gates them |
| `PlayerbotAI::HandleCommand` | `playerbot/PlayerbotAI.cpp:1375` | In-game chat entry point of the write path; whisper event source |
| `PlayerbotAI::HandleCommands` | `playerbot/PlayerbotAI.cpp:1096` | On-tick drain of queued commands; where intents are drained |
| `PlayerbotAI::UpdateAIInternal` | `playerbot/PlayerbotAI.cpp:1129` | The bot tick; chat-reply drain; where the snapshot is rebuilt |
| `PlayerbotAI::ChangeEngine` | `playerbot/PlayerbotAI.cpp:2037` | Sole combat/non-combat/dead transition point → combat events |
| `PlayerbotAI::QueueChatResponse` | `playerbot/PlayerbotAI.cpp:8978` | Off-tick enqueue + mutex + tick-drain — the pattern being mirrored |
| `chatCommands` / `chatReplies` / `chatRepliesMutex` | `playerbot/PlayerbotAI.h:707-709` | The existing queues the AI stream state sits beside |
| `ExternalEventHelper::ParseChatCommand` | `playerbot/strategy/ExternalEventHelper.h` | String → trigger resolution; the intent router's target |
| `PlayerbotSecurity::CheckLevelFor` | `playerbot/PlayerbotSecurity.h:38` | Permission gate the intent path bypasses |
| `TrainableSpellsValue` | `playerbot/strategy/values/TrainerValues.cpp:111` | Global "what can I train"; location-independent |
| `PlayerbotLLMInterface` | `playerbot/PlayerbotLLMInterface.cpp` | Existing LLM path; `SanitizeForJson` reused for snapshot text |
| `commandServerPort` | `playerbot/PlayerbotAIConfig.cpp:277` | `AiPlayerbot.CommandServerPort`, default 0 (off) |
| `PlayerbotAI::UpdateAiStream` | `playerbot/PlayerbotAI.cpp:6610` | The AI stream's whole tick: baseline on claim, drain, events, throttled snapshot rebuild |
| `PlayerbotAI::PushAiEvent` | `playerbot/PlayerbotAI.cpp:6597` | Appends one JSON event under the mutex, bounded, no-op unless claimed |
| `PlayerbotAI::DrainAiIntents` | `playerbot/PlayerbotAI.cpp:6648` | Swaps the intent queue out and runs each through `ParseChatCommand` |
| `PlayerbotAI::CheckAiStateEvents` | `playerbot/PlayerbotAI.cpp:6676` | Edge detection for `level_up`, `hp_critical` and `arrived` |
| `PlayerbotAI::BuildSnapshot` | `playerbot/PlayerbotAI.cpp:6736` | Serialises already-computed values into one JSON line |
| `AI_STREAM_*` constants, `AiJsonString`, `AiHealthBand`, `AiManaBand`, `AiNpcType` | `playerbot/PlayerbotAI.cpp:6522-6595` | Throttle and cap constants, plus the file-static snapshot helpers: escaping, banding, NPC classification |
| `criticalHealth` / `lowHealth` / `mediumHealth` / `lowMana` / `mediumMana` | `playerbot/PlayerbotAIConfig.cpp:151-156` | The thresholds the snapshot bands and `hp_critical` use |
