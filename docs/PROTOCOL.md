# Protocol for `/run/mpc-ext.sock`

This describes the line protocol implemented by `src/proto.c`, `src/proto.h`, and the socket server in `src/bridge.c`. TrackBoy is the first client plugin; `mpc-clip-ctl` (`src/cli.c`) is a test client for the same socket.

The advertised protocol version is **2**. `HELLO` always replies with `proto=2`. There is no separate v1 dialect in the current source: a one-line `WRITE` command is accepted as a single-note form of `WRITE_BATCH`. (File comments in `proto.h`, `proto.c`, and `cli.c` still say "v1" in places; that is stale relative to `MPC_EXT_PROTO_VER`, which is `2`.)

## Transport

- Unix domain stream socket at `/run/mpc-ext.sock` (`0660` after bind). The server path is fixed; `mpc-clip-ctl` can override the client path with the `MPC_EXT_SOCK` environment variable.
- Newline-delimited text (`\n`). Bare `\r` is ignored.
- Up to **8** concurrent clients. Further accept()s are closed immediately. `poll()` is used so clients are not starved.
- Each command is one header line, optionally followed by a fixed number of note lines (`WRITE_BATCH`, `REPLACE`). The server does not reply until the expected note lines have arrived (or the header itself is a complete command).
- Empty lines are ignored.
- Command lines are buffered up to **511** characters. Extra characters before the newline are dropped.
- Mutating commands (and reads) are dispatched to the MPC UI thread. The server waits up to **2000 ms**; if the UI thread does not take the request, the reply is `ERR ui_thread_timeout`. If no live project root has been captured yet, `HELLO` still returns immediately with `ready=0`, and every other command returns `ERR mpc_not_ready`.
- **Do not** do synchronous socket I/O from the MPC UI or audio callback; that can deadlock the UI thread that serves these requests. Use a background thread.

## Timing and addressing

- Timing is MPC's native **960 PPQ**. `240` ticks is one 16th-note step; `3840` ticks is one 4/4 bar.
- `col` and `row` are 0-based. The parser rejects values `>= 128`. The live grid may be smaller; a command whose `col`/`row` is outside the current grid returns `ERR clip_out_of_range`.
- `row=0` is the current **Main / Sequence** clip. `row>=1` is a **Clip Matrix** slot.
- Optional `if_epoch=<u32>` may appear anywhere on a mutating header line. `0` (or omitted) means no check. If it is non-zero and does not equal the live project epoch, the command fails with `ERR epoch_mismatch`.

## Note lines

Used as the body of `WRITE_BATCH` and `REPLACE`, and as the fields of one-line `WRITE`:

```
<note> <tick> <vel> <len> [prob]
```

| Field | Range | Notes |
|---|---|---|
| `note` | 0..127 | MIDI note number |
| `tick` | `>= 0` | Start time in ticks |
| `vel` | 1..127 | Velocity |
| `len` | `>= 1` | Duration in ticks |
| `prob` | 0..100 | Optional; defaults to **100** |

## Commands

### `HELLO`

```
HELLO
```

Success:

```
OK proto=2 fw=<fw> caps=native_clips_rw,tracks,epoch,set_len epoch=<u32> rev=<u32> ready=<0|1> cols=<N> rows=<M> sel_col=<c> sel_row=<r> seq_mode=<0|1>
```

- `fw` is the profile's firmware string (for the shipped profiles, `3.10.1.12`), or `unknown` if no profile is bound.
- `caps` is currently the fixed string `native_clips_rw,tracks,epoch,set_len`.
- `ready` is `1` only when the live clip-lanes context could be resolved on the UI thread. If the addin is loaded but no project root is available yet, `HELLO` still succeeds with `ready=0`, `sel_col=-1`, `sel_row=-1`, and `epoch`/`rev`/`cols`/`rows` at `0`.
- `epoch` increments when the live `Project` pointer changes (and wraps from `0` to `1`, never staying at `0` after a bump). Use it with `if_epoch=` to avoid writing into a newly loaded project.
- `rev` is a cheap change counter. On `HELLO` when `ready=1`, it is updated from an O(clips) fingerprint of RB-tree header words, clip lengths, selected slot, and track identity (the first 16 columns and 9 rows only). Clients can poll `HELLO` and skip `TRACKS` / `LIST` / `READ` while `(epoch, rev)` is unchanged.
- `sel_col` / `sel_row` are the selected slot, or `-1` if unknown. `seq_mode` is `1` when the project's selected-is-sequence flag is set, else `0`. In sequence mode, `sel_row` is reported as `0`.

### `TRACKS`

```
TRACKS
```

Success: zero or more `TRACK` lines, then `END`:

```
TRACK col=<c> type=<drum|plugin|keygroup|midi|cv|audio|other> name="<track>" plugin="<vst>"
...
END count=<k>
```

- Types come from the track program's vtable: `drum`, `plugin`, `keygroup`, `midi`, `cv`, `audio`, or `other`.
- `plugin="..."` is filled only for `type=plugin`. Otherwise it is empty.
- Names are copied from JUCE strings; bytes below 0x20, `DEL`, and `"` are replaced with a space. Name and plugin fields are capped at 63 characters. At most **128** tracks are returned.

### `LIST`

```
LIST
```

Success:

```
CLIP col=<c> row=<r> state=<s> notes=<n> len=<ticks> sel=<0|1>
...
END count=<k>
```

- Only slots that currently hold a clip with a valid event owner are listed (empty Clip Matrix slots are omitted). At most **512** clips.
- `notes` is the count of note events (event type byte `3`) in that clip, examined up to 8192 tree nodes.
- `len` for `row>0` is the clip extent; for `row=0` it is the Main/Sequence length, or the track's independent loop length when that loop is enabled.
- `sel` is `1` when this slot is the selected column and row from `HELLO`.
- `state` is the raw `uint32` returned by MPC's clip-state function. This repo does not document the meaning of individual values.

### `READ <col> <row>`

```
READ <col> <row>
```

Success:

```
NOTE tick=<t> note=<0..127> vel=<1..127> len=<ticks> prob=<0..100>
...
END count=<k>
```

- Returns note events only (type byte `3`), in tree order, starting from tick 0, up to **2048** notes (further notes are skipped). Tree walks stop after 8192 nodes.
- Velocity is converted from MPC's 0..1 float and clamped to 1..127.
- If the slot has no clip: `ERR no_clip_at_slot`. `READ` does not auto-create clips.

### `WRITE <col> <row> <note> <tick> <vel> <len> [prob] [if_epoch=<u32>]`

One-line shorthand for a single-note `WRITE_BATCH`. Parsed as `batch_count=1` with the same field ranges as a note line. Default `prob` is 100.

Success: `OK written=1` (or `OK written=<N>` if reported from the shared write path; for this command `N` is 1 when the note is accepted).

This is **additive**. It does not clear existing notes in the slot.

### `WRITE_BATCH <col> <row> <N> [if_epoch=<u32>]`

```
WRITE_BATCH <col> <row> <N> [if_epoch=<u32>]
<note> <tick> <vel> <len> [prob]
...  (exactly N lines)
```

- `N` must be in **1..512**.
- After all `N` note lines are read, they are inserted as one native MPC command (one Undo step) via `ModifySequenceEventListCommand`.
- Success: `OK written=<N'>` where `N'` is the number of notes actually inserted.
- Also additive: existing notes are left in place.
- For `row>=1`, if the Clip Matrix slot is empty, an empty clip is created first (`CLIP_CREATE`) and then the notes are written. `row=0` is not auto-created; a missing Main clip returns `ERR no_clip_at_slot`.

### `REPLACE <col> <row> <del_note|-1> <t0> <t1> <add_N> [if_epoch=<u32>]`

```
REPLACE <col> <row> <del_note|-1> <t0> <t1> <add_N> [if_epoch=<u32>]
<note> <tick> <vel> <len> [prob]
...  (exactly add_N lines; add_N may be 0)
```

- `del_note` is `-1` (any note) or `0..127`.
- The parser requires `t0 >= 0` and `t1 >= t0`. `add_N` is `0..512`.
- Notes whose tick is in the half-open range `[t0, t1)` and whose pitch matches `del_note` (or any pitch if `-1`) are removed. If `t1 == t0`, the runtime delete pass is skipped (`tick_end > tick_start` is required to walk the tree).
- The `add_N` notes are then inserted. Removal and insertion are submitted together as one native command (one Undo step).
- Success: `OK removed=<R> added=<A>`.
- Auto-creates an empty `row>=1` slot when `add_N > 0` and the slot has no clip. If `add_N` is 0 and there is no clip, the result is `ERR no_clip_at_slot`.

### `DELETE <col> <row> <note|-1> <tick_start> <tick_end> [if_epoch=<u32>]`

```
DELETE <col> <row> <note|-1> <tick_start> <tick_end> [if_epoch=<u32>]
```

- `note` is `-1` (any) or `0..127`. The parser requires `tick_start >= 0` and `tick_end > tick_start` (equal ticks are rejected).
- Deletes matching note events with tick in the half-open range `[tick_start, tick_end)`.
- Success: `OK deleted=<N>`.

### `CLEAR <col> <row> [if_epoch=<u32>]`

```
CLEAR <col> <row> [if_epoch=<u32>]
```

Deletes every note event in the clip (internally a delete from tick `0` to `INT64_MAX` with note filter `-1`).

Success: `OK deleted=<N>`.

### `SET_LEN <col> <row> <len_ticks> [if_epoch=<u32>]`

```
SET_LEN <col> <row> <len_ticks> [if_epoch=<u32>]
```

- `len_ticks` must be in **120 .. 983040** inclusive (`983040` is `960 * 4 * 256`). Values below 120 are rejected by the parser (the unit tests cover this).
- `row=0` sets the track's independent Main/Sequence loop length (`SetAndEnableIndependentTrackLengthCommand`). The reported `len` is the length actually in effect afterwards (independent loop length if enabled, otherwise the sequence length).
- `row>=1` sets the Clip Matrix clip length (`SetClipLengthCommand`). If the slot is empty, an empty clip is created first.
- Success: `OK len=<ticks>`.

## Errors

Every failure is a single line:

```
ERR <message>
```

Parser / framing messages (from `src/proto.c`, or a bad note line in a batch):

- `empty command`
- `unknown command`
- `bad note line (want: note tick vel len [prob])`
- `note field out of range`
- `usage: READ <col> <row>`
- `usage: WRITE <col> <row> <note> <tick> <vel> <len> [prob]`
- `usage: WRITE_BATCH <col> <row> <1..512> [if_epoch=<u32>]`
- `usage: REPLACE <col> <row> <del_note|-1> <t0> <t1> <add_count> [if_epoch=<u32>]`
- `usage: DELETE <col> <row> <note|-1> <tick_start> <tick_end> [if_epoch=<u32>]`
- `usage: CLEAR <col> <row> [if_epoch=<u32>]`
- `usage: SET_LEN <col> <row> <len_ticks> [if_epoch=<u32>]`

Runtime messages (from `src/bridge.c`):

- `mpc_not_ready` — no live project/lanes context (or `HELLO` would report `ready=0`)
- `missing_rb_increment` — `dlsym` of `_ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base` failed
- `epoch_mismatch` — `if_epoch=` did not match the live epoch
- `clip_out_of_range` — `col`/`row` outside the current grid
- `invalid_track` — `SET_LEN` target column has no valid track
- `invalid_sequence` — `SET_LEN` on a Clip Matrix row could not resolve the current sequence
- `no_clip_at_slot` — no clip at that slot (and auto-create did not apply or failed)
- `invalid_event_owner` — clip exists but its event-list owner is not valid
- `unsupported_cmd` — reached the UI handler with an unexpected command kind
- `ui_thread_timeout` — the UI thread did not accept the request within 2 seconds

## Limits (from `src/proto.h`)

| Limit | Value |
|---|---|
| Protocol version | 2 |
| Max batch / replace notes | 512 |
| Max notes in a `READ` response | 2048 |
| Max clips in a `LIST` response | 512 |
| Max tracks in a `TRACKS` response | 128 |
| Max clients | 8 |
| Command line length | 511 characters |

## `mpc-clip-ctl`

The CLI talks to `/run/mpc-ext.sock` (or `MPC_EXT_SOCK`) and supports: `hello`, `tracks`, `list`, `read`, `write`, `delete`, `clear`, `set-len`, and `demo-2track` (writes a 22-note drum pattern to column 0 and an 8-note bass line to column 1, then reads both back). It does not expose `REPLACE` or `WRITE_BATCH` as subcommands, though `demo-2track` sends `WRITE_BATCH` directly.
