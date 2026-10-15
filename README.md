# vrs-lib 1.3.2 (SWEGCA VRS-MRP)

**English** | [한국어](README.ko.md)

**VRS-MRP** stands for **V**erification-**R**einforced **S**ynapse, **M**emory **R**ecovery **P**lugin.

vrs-lib is a Windows plugin for **Claude Code** and the **ChatGPT desktop app**. It leaves context compaction alone. After the context has been compacted, it gives you back the **exact pre-compaction originals** (your messages and the answers), byte for byte.

- The engine is libvrs lib7 (frozen build), ported natively to Windows.
- One executable (`bin/vrs_ccd.exe`) is the daemon, the MCP server for both apps, and the hooks. **No Python and no installer needed.**
- **One PC, one experience (new in 1.2.0):** both apps share one daemon and one store on your PC. Claude Code transcripts and ChatGPT app threads live side by side in the same store without clashing.
- **Search across both apps (new in 1.3.0):** `vrs_search` finds the turns of every Claude Code session and every ChatGPT app thread on this PC that answer a question, wherever you ask from. The strongest remembered turns come back verbatim with their original positions.
- **One experience, recalled by itself (new in 1.3.1):** every message you send is also a cue. Before the model answers, the memories it calls up from the one store of both apps (other Claude Code sessions and ChatGPT app threads) are added to that turn, verbatim, each with its app and session. No tool call is needed; the turns of the session you are in are left out. Among memories of equal strength, the one reached by the rarest word of your message (a name, an identifier, a number) comes first (1.3.2).
- `source/` holds the source that `bin/vrs_ccd.exe` was built from.

## Install

### Claude Code

Inside Claude Code:

```
/plugin install vrs-lib --marketplace raspie10032/SWEGCA-VRS-MRP
```

Or from a terminal:

```bash
claude plugin install vrs-lib --marketplace raspie10032/SWEGCA-VRS-MRP
```

- The repository is public, so you don't need to sign in to GitHub. Git is all you need.
- It takes effect in **sessions you open after installing**.
- In the desktop app, you can manage it under **+ → Plugins**.

### ChatGPT desktop app

Use the codex that ships with the app to add the marketplace `raspie10032/SWEGCA-VRS-MRP`, then install `vrs-lib`:

```bash
codex plugin marketplace add raspie10032/SWEGCA-VRS-MRP
```

```bash
codex plugin add vrs-lib@vrs-lib
```

- The post-compaction hint is a hook, and the app runs it only after you **trust** it. When a new version changes the hook text, you'll be asked to trust it again.
- Restart the app to pick up the plugin.

## Tools

| Tool | Arguments | What it does |
|---|---|---|
| `vrs_restore` | none (`start` and `budget` are optional) | Returns the exact messages and answers from before the last compaction of this session (in the app, this thread). If the result spans several pages, pass its `next` value as `start` to get the next one. |
| `vrs_read` | `path`, `start`, `end` | Reads an exact byte range of a transcript file, together with its SHA-256. It can read under the Claude Code transcript folder, the app's `sessions` folder, and any folders you list in `VRS_CC_READ_ROOTS` or `VRS_CX_READ_ROOTS` (separated by `;`). |
| `vrs_search` | `query` (`start` and `budget` are optional) | Searches every Claude Code session and app thread on this PC. The first rows replay the strongest remembered turns verbatim (the order is by the synapse strength of the query's words with each memory, then by how many words match, then by recency); the rest list every matching turn by position, readable with `vrs_read`. Rare words (names, identifiers, numbers) reach older turns; a word that appears in more than 64 turns is seen through its 64 most recent ones. Pass `next` as `start` for the next page. |

- In Claude Code the tools are named `mcp__plugin_vrs-lib_vrs-lib__vrs_restore`, `mcp__plugin_vrs-lib_vrs-lib__vrs_read` and `mcp__plugin_vrs-lib_vrs-lib__vrs_search`.
- `NOT_READY` means the index is still being prepared. It does not mean the originals are gone. Just call again with the same arguments a few seconds later.
  - The very first start after installing indexes all of your history, so this can last a few dozen seconds. Restarts after that take a few seconds.

## What the plugin does

- **MCP server:** Claude Code uses `.mcp.json` and the app uses `codex/mcp.json`. Both start `vrs_ccd.exe --mcp`, which starts the daemon if it isn't running yet.
- **Hooks:**
  - Claude Code (`hooks/hooks.json`):
    - On session start and on each prompt, `--hook writer` leaves a writer marker for the open session.
    - Right after a compaction, `--hook compact` adds a hint: the summary is not the original, so check with `vrs_restore`.
  - The app (`codex/hooks.json`) adds the same hint right after a compaction.
- **Daemon and data:**
  - There is one daemon per Windows user, on the pipe `\\.\pipe\vrs-lib-<user>`.
  - The data lives in `%LOCALAPPDATA%\vrs-lib`. It doesn't belong to either app, so it stays when you remove the plugin. To delete it, stop the daemon and remove the folder yourself.
  - You can change these with the environment variables `VRS_HOME` (data folder) and `VRS_ENDPOINT` (pipe).
  - The daemon only reads your original transcripts (`~/.claude/projects`, `~/.codex/sessions`); it never writes to them.

## Upgrading from 1.3.0 or 1.3.1

- Stop the running daemon with `Get-Process vrs_ccd | Stop-Process` and open a new session (restart the ChatGPT app as well); the 1.3.1 daemon starts on the same data.
- In the ChatGPT app, the new UserPromptSubmit hook has to be trusted once (the app asks, or review it under hooks).

## Upgrading from 1.2.0

- A daemon that's already running keeps using the 1.2.0 executable, which has no search. Stop it with `Get-Process vrs_ccd | Stop-Process` and open a new session; the 1.3.0 daemon starts and builds its search index (the first start can take a minute on a large history).

## Upgrading from 1.1.x

- On its first start, the 1.2.0 daemon moves your old data folders into `%LOCALAPPDATA%\vrs-lib` by **renaming** them. Nothing is copied.
  - The old Claude Code folder is `~/.claude/plugins/data/vrs-lib-vrs-lib`, and the old app folder is `~/.codex/vrs-lib`.
  - Every move is listed in `migrated.txt`.
  - Anything that can't be renamed (another drive, a locked folder) is simply indexed again.
- A daemon that's already running keeps using the old version's executable. While it's up, the app's tools answer `OLD_DAEMON_ON_PIPE`. Stop it with `Get-Process vrs_ccd | Stop-Process` and open a new session; the 1.2.0 daemon will start.
- Claude Code sessions that were already open pick up the 1.2.0 hooks once you reopen them. Restart the app as well.

## Repository layout

| Path | Contents |
|---|---|
| `.claude-plugin/plugin.json`, `.claude-plugin/marketplace.json` | Claude Code plugin and marketplace (both apps read these) |
| `.codex-plugin/plugin.json`, `codex/mcp.json`, `codex/hooks.json` | ChatGPT desktop app plugin |
| `.mcp.json`, `hooks/hooks.json` | Claude Code MCP server and hooks |
| `bin/vrs_ccd.exe` | The single executable: daemon, MCP server, hooks |
| `docs/VRS_MCP_libvrs_v1.md` | Design and verification notes (in Korean) |
| `source/` | The source it was built from |
| `README.md`, `README.ko.md` | This guide in English and Korean |
| `SHA256SUMS.txt`, `VERSION`, `THIRD_PARTY_NOTICES.md` | Checksums, version, third-party notices (mimalloc, MIT) |

## Verification summary

### 1.1.x (Claude Code)

| Check | Result |
|---|---|
| Same engine | Same results as the frozen Windows and Linux builds. |
| Exact originals | SHA and boundaries match for 250 items, including a path longer than 260 characters (436). |
| Live updates | 100/100 |
| Per-event cost | Doesn't grow with transcript length (ratio 1.078). |
| Restart | 1.2–1.5 s in the fixed test |
| Serial structure | 0 in the static check |
| Requests before ready | `NOT_READY` comes back right away (at most 339 ms). |
| Recovery after compaction (fork test KM6c′) | Both arms forked from the same compaction summary. Of 18 facts the summary lost, the arm without the tool got 0 in both pairs, and the arm with the tool got all 18 in both pairs (bar 17, no wrong answers, one tool call). |

### 1.3.2 (recall on every message, on fixed copies of real Claude Code and app histories)

| Check | Result |
|---|---|
| Exact originals and their source | 30 of 30 real questions: 129 recalled memories and 256 entries matched the originals in SHA and text, each with the right app and session. |
| Size | At most 9,554 UTF-16 units per message (Claude Code takes up to 10,000). |
| Speed | Recall adds 5.40 ms at the median (p95 8.17 ms). |
| No regression | `vrs_search` (30/30), `vrs_restore` and `vrs_read` (34/34) answers equal 1.3.0; the K5 dump equals the independent calculator (14,270 lines). |

### 1.3.0 (search across both apps, on fixed copies of real Claude Code and app histories)

| Check | Result |
|---|---|
| Same values as an independent calculator | The daemon's full dump (turns, cells, synapses, queries) equals a separately written calculator line for line (14,270 lines), with 1 and 12 workers under race injection and after 10 forced kills during the build. |
| Finds what you asked for | 19 of 20 real Claude Code questions and 10 of 10 cross-app questions had their answer on the first page. |
| Exact originals | 557 replayed entries matched the originals in SHA and text. |
| Live updates | 100/100 just-written lines found (median 10 ms). |
| Restart | First search 3.5–4.1 s after a restart (16,212 turns). |
| No regression | `vrs_restore` and `vrs_read` answers are byte-identical to 1.2.0 (34/34). |

### 1.2.0 (one PC, one experience, with real Claude Code and the real ChatGPT desktop app)

| Check | Result |
|---|---|
| One daemon | Each app's host called `vrs_restore` and got a proper result, with a single daemon serving both calls. |
| App recovery after compaction | On a real app thread, the boundary in the result matched the real compaction point. In 4 app threads with compactions, all 22 items matched in SHA and text. |
| No regression for Claude Code | SHA matched for all 4850 items in 62 transcripts with compactions. With a single root the results equal 1.1.1 (proof and tests). |
| Restart and latency | First answer 3.6 s after a restart. Adding the app's history kept the latency ratio at 0.98–1.02. |
| Migration | A copy of the old data (5716 files) was moved by renaming only (same file ids). Undo was exact, and restore answers were byte-identical to those before the move. |

## License

**MIT AND MPL-2.0.** MIT (`LICENSE`) covers the project. Only `source/native/libvrs/swegca_cpp/`, which was ported from SWEGCA-Architecture, is MPL-2.0 (`LICENSES/MPL-2.0.txt`). `NOTICE.md` describes the boundary, and `THIRD_PARTY_NOTICES.md` holds the third-party notice (mimalloc, MIT).
