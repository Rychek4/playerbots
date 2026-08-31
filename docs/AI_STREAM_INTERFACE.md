# AI Stream Interface — Design & Reference

**Status:** Implemented. This document is kept as the **decision record** — why
the design is shaped this way, the control-model argument, the build order. For
what is actually in the tree, including the details that only settled during
implementation, read [`READ_WRITE_SYSTEMS.md`](READ_WRITE_SYSTEMS.md); where the
two disagree, that one is right.

**Scope:** The C++ (CMaNGOS / playerbot) side of a system that lets an external
AI process "inhabit" a single playerbot character — receiving the strategic
data it needs and issuing high-level intents. The Python/AI side is built
separately and consumes the contract defined here.

---

## 1. Goal & guiding principle

Let an AI drive a bot at the **intent / strategic layer only**. All tactical
work — combat rotations, target selection, pathfinding, movement — stays in the
existing hardcoded C++ engine.

The AI does **not** need to know *how* to fight or *how* to walk to a city. It
needs to know *that* it is under attack, and to be able to say "go to
Stormwind" and let the engine route it there.

This single decision is what keeps the C++ footprint tiny: we are not building
game logic, we are exposing data the engine already computes and routing
commands the engine already understands.

### Control model: **Nudge (engine-led)**

When a bot is "claimed" by the AI, it keeps running its normal autonomous
strategies. AI intents are **high-level overrides layered on top** — "go here",
"train", "flee", "accept invite". If the AI sends nothing, the bot behaves
normally and never looks idle. (A future "Director" mode that suppresses
autonomous decisions is a possible additive upgrade — see §9.)

---

## 2. Transport

**One TCP port, request/response, newline-delimited** — reusing the existing
`PlayerbotCommandServer` unchanged.

- Server: `playerbot/PlayerbotCommandServer.cpp` (boost::asio, thread per
  connection), port from `PlayerbotAIConfig::commandServerPort`.
- Wire format: Python sends one line `"<command>,<guid>"`, C++ replies with one
  line. This is exactly the existing protocol
  (`RandomPlayerbotMgr::HandleRemoteCommand` → `PlayerbotAI::HandleRemoteCommand`).
- **No push channel. Python polls.** An LLM reasons in seconds; sub-second event
  delivery buys nothing, and polling removes an entire async subsystem. Events
  are buffered in C++ and drained by a poll.

One connection can address any bot via the `,<guid>` suffix; use one connection
per inhabited bot or one shared connection — no C++ difference.

> **Responses must be single-line JSON** (no embedded newlines), because the
> protocol is newline-delimited. `PlayerbotLLMInterface::SanitizeForJson` helps.

---

## 3. The contract

All messages go over the single command port as `"<verb> <args>,<guid>"`.

### 3.1 Control & read verbs (request → response)

| Verb | Sends | Returns | Meaning |
|---|---|---|---|
| `claim` | `claim,<guid>` | `ok` | Mark bot AI-controlled; enable snapshot + event buffering for this bot only. |
| `release` | `release,<guid>` | `ok` | Stop buffering; bot continues autonomously. |
| `snapshot` | `snapshot,<guid>` | JSON object | The strategic situation report (cached, tick-built). See §3.3. |
| `events` | `events,<guid>` | JSON array | All events since the last poll, then clears the buffer. See §3.4. |

### 3.2 Intent verbs (request → enqueued → `ok`)

Intents are **stripped of the `intent ` prefix and fed into the existing
whispered-command pipeline** (`ExternalEventHelper::ParseChatCommand`). That
means the entire existing command vocabulary is available for free. Examples:

| Intent | Backed by existing action |
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

> New game logic required for intents: **none.** The intent handler is a router.

### 3.3 Snapshot schema (`snapshot,<guid>`)

Single-line JSON object. Fields (extend as needed — version the schema):

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

HP/mana are reported as **bands** (`critical`/`low`/`medium`/`high`), not raw
numbers — the AI makes strategic, not tactical, decisions.

### 3.4 Event types (`events,<guid>`)

Array of objects, each with a `type`. The buffer is cleared on read.

| `type` | Extra fields | Source |
|---|---|---|
| `combat` | — | `ChangeEngine(BOT_STATE_COMBAT)` |
| `non-combat` | — | `ChangeEngine(BOT_STATE_NON_COMBAT)` |
| `dead` | — | `ChangeEngine(BOT_STATE_DEAD)` |
| `hp_critical` | `pct` | health-threshold check on tick |
| `whisper` | `from`, `text` | whisper handler in `HandleCommand` |
| `group_invite` | `from` | party-invite packet handler |
| `arrived` | `destination` | travel-complete |
| `level_up` | `level` | level-up hook |

Example poll response:

```json
[{"type":"whisper","from":"Bairdbiz","text":"want to group up?"},
 {"type":"combat"}]
```

---

## 4. C++ implementation plan

**Files touched: `PlayerbotAI.h` and `PlayerbotAI.cpp` only.**
No new files, no `CMakeLists.txt` changes, no new sockets, no new threads.

### 4.1 `PlayerbotAI.h` — new members & declarations

Mirror the existing `chatReplies` + `chatRepliesMutex` pattern (~line 695):

```cpp
std::mutex aiStreamMutex;
std::queue<std::string>  aiInboundIntents;   // Python -> bot
std::vector<std::string> aiOutboundEvents;   // bot -> Python (JSON lines)
std::string aiSnapshotCache;                 // rebuilt on tick
bool aiControlled = false;
// decls: PushAiEvent(...), BuildSnapshot(), DrainAiIntents()
```

### 4.2 `PlayerbotAI.cpp` — touch-points

> As built, this came to ~490 lines rather than ~150, and touched one extra
> line in `RandomPlayerbotMgr.cpp`. The estimate was low mostly on
> `BuildSnapshot` (~130 lines with the group, quest and nearby-NPC lists) and on
> four things the design had not accounted for: the `fromCommandServer` gate
> that keeps the verbs off the in-game `debug` path, JSON error responses,
> bounded buffers, and the tick-owned edge-detection state.


| # | Location | Change | ~LOC |
|---|---|---|---|
| 1 | `HandleRemoteCommand` (~6477) | Add `claim` / `release` / `snapshot` / `events` / `intent …` branches. snapshot & events return mutex-guarded string copies; intents push to `aiInboundIntents`. | ~50 |
| 2 | `ChangeEngine(BotState)` (~2009) | `if (aiControlled) PushAiEvent("combat"/"non-combat"/"dead")` at the single state-transition choke point. | ~5 |
| 3 | `UpdateAIInternal` / `HandleCommands` (~1095–1128) | Under the mutex, drain `aiInboundIntents` and run each through `ParseChatCommand`; then (throttled) rebuild `aiSnapshotCache` if `aiControlled`. **This is the only place game state is read — and it runs on the bot's tick.** | ~25 |
| 4 | whisper path (`HandleCommand`, CHAT_MSG_WHISPER, ~1440) + party-invite handler + hp-threshold check | `PushAiEvent(...)` one-liners. | ~10 |
| 5 | new method `BuildSnapshot()` | Read the already-computed `AI_VALUE`s into a single-line JSON string. | ~60 |

**Total: ~150 lines across 2 files.**

### 4.3 Threading & safety model

CMaNGOS is single-threaded per world/map. The command server runs on **separate
threads**, so nothing on the socket side may touch a live game object.

- **Snapshot** is built **on the bot's tick** and cached. The socket only ever
  returns a **copied string** under `aiStreamMutex`. Python never touches a game
  object.
- **Events** are appended on the tick and read as string copies under the mutex.
- **Intents** are enqueued from the socket thread under the mutex and
  **executed on the tick** via the existing command pipeline.

This is strictly safer than the existing remote commands (which read game state
directly from the worker thread — a known shortcut in this codebase that we do
not repeat).

- **Scale:** all snapshot/event work is gated on `aiControlled`. The other
  few-hundred autonomous random bots cost nothing.

### 4.4 Intent permission context

The engine's command path expects an "owner" with permissions
(`PlayerbotSecurity`). Injected AI intents are routed as the bot's master/self
and bypass the security-level check, since the AI is trusted server-side. This
is the one nuance in the intent handler.

---

## 5. Data-source reference

Everything the snapshot reports is already computed by the engine. Key mappings:

| Snapshot / decision need | C++ source | Notes |
|---|---|---|
| **"Do I have a skill to train?"** | `AI_VALUE(std::vector<TrainerSpell const*>, "trainable spells")` | `TrainerValues.cpp:111`. Built from the **global** `GAI_VALUE("trainable spell map")`, filtered by class/level/prereqs via `bot->GetTrainerSpellState()`. **Works anywhere — no trainer nearby required.** |
| Where trainers are | `AvailableTrainersValue` | `TrainerValues.cpp:169` |
| Zone / area | `bot->GetAreaId()` + `GetAreaEntryByAreaID` | as in existing `position` command |
| Position | `bot->GetPositionX/Y/Z()`, `GetMapId()` | existing `position` command |
| HP / mana | `bot->GetHealth()`, `GetMaxHealth()`, power | existing `hp` command; report as bands |
| Combat state | `PlayerbotAI::currentState` (`BotState`) | existing `state` command |
| Money | `bot->GetMoney()`, `AI_VALUE2(uint32,"free money for",...)` | existing `budget` command |
| Current target | `AI_VALUE(Unit*, "current target")` | existing `target` command |
| Group | `bot->GetGroup()` | |
| Quest log | quest-log AI values | |
| Nearby units of interest | nearest-unit AI values | |
| Travel target / status | `AI_VALUE(TravelTarget*, "travel target")` | existing `travel` command |

---

## 6. Example end-to-end flow

Python has already sent `claim,1234`; it polls `events,1234` ~1×/sec and pulls a
`snapshot` when it decides to reason.

```
# 1. Player whispers the NPC
→ events,1234
← [{"type":"whisper","from":"Bairdbiz","text":"hey, want to group up and quest?"}]
→ intent whisper Bairdbiz Sure! Invite me and I'll head your way.,1234
← ok

# 2. Player invites to group
→ events,1234
← [{"type":"group_invite","from":"Bairdbiz"}]
→ intent accept,1234
← ok

# 3. AI decides what to do — pulls a snapshot
→ snapshot,1234
← {"v":1,"zone":"Westfall","level":14,"hp":"high","money":"1g20s",
   "trainable_spells":3,"in_group":true,"group_members":["Bairdbiz"]}
# LLM sees trainable_spells:3 -> decides to train before questing
→ intent goto Stormwind,1234
← ok
# C++ TravelMgr computes the entire Westfall -> Stormwind route.

# 4. En route, gnolls attack
→ events,1234
← [{"type":"combat"}]
# Nudge mode: AI does nothing; C++ combat strategy auto-fights.
# (If a later poll showed {"type":"hp_critical"} the AI could send intent flee.)

# 5. Arrival
→ events,1234
← [{"type":"arrived","destination":"Stormwind"}]
→ intent train,1234
← ok
# TrainerAction learns the eligible spells.
→ snapshot,1234
← {...,"trainable_spells":0}   # AI confirms done, moves on to questing
```

---

## 7. Key existing code seams (glossary)

| Symbol | File | Role |
|---|---|---|
| `PlayerbotCommandServer` | `PlayerbotCommandServer.cpp` | The TCP command server we reuse. |
| `RandomPlayerbotMgr::HandleRemoteCommand` | `RandomPlayerbotMgr.cpp:3986` | Routes `cmd,guid` to the right bot. |
| `PlayerbotAI::HandleRemoteCommand` | `PlayerbotAI.cpp:6477` | The per-bot request/response dispatcher we extend. |
| `PlayerbotAI::ChangeEngine` | `PlayerbotAI.cpp:2009` | Single combat/non-combat/dead choke point → combat events. |
| `PlayerbotAI::HandleCommands` | `PlayerbotAI.cpp:1095` | On-tick drain of queued commands; where we drain intents. |
| `ExternalEventHelper::ParseChatCommand` | `strategy/ExternalEventHelper.h` | Turns a command string into engine triggers/actions. Intent router target. |
| `PlayerbotAI::chatCommands` / `chatRepliesMutex` | `PlayerbotAI.h:694` | Existing on-tick queue + mutex pattern we mirror. |
| `TrainableSpellsValue` | `strategy/values/TrainerValues.cpp:111` | Global "what can I train" value. |
| `PlayerbotLLMInterface` | `PlayerbotLLMInterface.cpp` | Existing LLM socket + `SanitizeForJson`. |

---

## 8. Build order

1. **Read side first** (data-out you can test with a telnet client):
   `claim` / `release` / `snapshot` / `events` + the `ChangeEngine` combat event.
2. **Event hooks:** whisper, group_invite, arrived, hp_critical, level_up.
3. **Intent side:** the `intent …` router + on-tick drain + permission bypass.
4. **BuildSnapshot** field-by-field against §3.3 / §5.

---

## 9. Open items & future

- **Snapshot rebuild throttle:** pick an interval (e.g. 1–2s) so claimed bots
  don't rebuild JSON every tick.
- **Snapshot schema versioning:** `"v"` field is in place from day one.
- **Security:** the command port is unauthenticated (existing condition).
  Controlling a bot is limited blast radius; consider loopback-only or a token
  if exposed.
- **Director mode (future, additive):** on claim, suppress autonomous decision
  strategies so the bot *only* does what the AI says (combat/pathing still
  hardcoded). Reuses the same event buffer; needs a suppression strategy + a
  safe-default idle behavior.
- **Push upgrade (future, additive):** a second out-port that pushes events
  instead of polling. Drains the *same* event buffer, so nothing here is wasted.
