# Porting `mpc-clip-bridge` to New MPC Firmware Builds

`mpc-clip-bridge` uses a data-driven profile table in `src/profiles.c` (`CB_PROFILES[]`).
At runtime, `mpc_clip_bridge.so` refuses to arm unless:

1. `/proc/self/exe` is `/usr/bin/MPC`,
2. CPU architecture (`CB_ARCH_ARM32` or `CB_ARCH_AARCH64`) and the 20-byte `NT_GNU_BUILD_ID` match an entry in `CB_PROFILES[]`, and
3. All three hook prologue byte signatures match verbatim at `load_bias + hook_rva[i]`.

Adding support for another build (for example, a Gen 2 `3.10.1.49` image) only requires extracting its coordinates and appending a `CbProfile` entry to `src/profiles.c`.

> **Repo hygiene:** Keep all downloaded `.img` files, decompressed rootfs images, and extracted `/usr/bin/MPC` binaries **outside git** (for example in `~/mpc-fw/`). Never commit firmware images, extracted binaries, dumps, or byte blobs beyond the short hook prologue signatures in `src/profiles.c`.

---

## 1. Extracting `/usr/bin/MPC` from an Official Update Image

### Gen 2 (`AZ0x` container)

Stock `mpcimg2` rejects `AZ0x` Gen 2 images, but the container embeds standard XZ streams (including the main ext4 rootfs):

1. Download the official update `.img` to `~/mpc-fw/` (outside the repo).
2. Scan for XZ stream magic (`fd 37 7a 58 5a 00`):
   ```sh
   python3 -c '
   data = open("MPC-3.10.1-Gen2-update.img", "rb").read()
   magic = b"\xfd7zXZ\x00"
   pos = 0
   while True:
       i = data.find(magic, pos)
       if i < 0: break
       print(hex(i))
       pos = i + 1
   '
   ```
   On Gen 2 `3.10.1.12`, three XZ streams appear; the largest one (`0x258cc38`, decompressing to ~667 MB) is the ext4 rootfs.
3. Decompress the rootfs stream (`tail -c +$((offset + 1)) update.img | xz -dc > gen2-rootfs.ext4`).
4. Extract `/usr/bin/MPC` with `debugfs` (available inside Linux / Docker `e2fsprogs`):
   ```sh
   debugfs -R "dump /usr/bin/MPC MPC-gen2" gen2-rootfs.ext4
   ```
5. Record the ELF metadata:
   ```sh
   readelf -h MPC-gen2 | grep -E 'Class:|Machine:|Type:'
   readelf -n MPC-gen2          # 20-byte GNU Build ID
   readelf -lW MPC-gen2         # verify PT_LOAD segment 0 has p_offset=0, p_vaddr=0, R E
   ```
   Also check the target rootfs glibc version (`strings /lib/libc.so.6 | grep 'GNU C Library'`) so the Docker cross-toolchain produces symbols `<=` target glibc (Gen 1: `<= 2.31`, Gen 2: `<= 2.39`).

---

## 2. Resolving the 14 C++ Vtables via RTTI

`/usr/bin/MPC` is stripped of most local function names, but C++ RTTI type-name strings (`_ZTS...`) and `.rela.dyn` relocations are intact. Every vtable can be found deterministically:

1. Search `.rodata` for the exact null-terminated mangled RTTI name string (`_ZTS`):
   - `12MpcClipLanes`
   - `4Clip`
   - `5Track`
   - `8Sequence`
   - `7Project`
   - `N12SafeLifetime6HolderI23UnsafeSequenceEventListEE`
   - `15PerThreadQueues`
   - `12ThreadQueues`
   - `11DrumProgram`, `12AudioProgram`, `11MidiProgram`, `13PluginProgram`, `9CVProgram`, `15KeygroupProgram`
2. Scan `.rela.dyn` (`R_AARCH64_RELATIVE` = type `1027`, or `R_ARM_RELATIVE` = type `23`) for the relocation whose addend equals the `_ZTS` address. That relocation sits at `type_info + 8` (64-bit) or `type_info + 4` (32-bit), giving `type_info`.
3. Scan `.rela.dyn` again for relocations whose addend equals `type_info`. The primary vtable slot (`vptr`) is at `reloc_offset + 8` (64-bit) or `reloc_offset + 4` (32-bit) where the preceding `offset_to_top` word is `0`.

---

## 3. Locating the 3 Hook Anchors

For each hook, pick a sequence of **8 bytes** (ARM32) or **16 bytes** (AArch64) containing **zero PC-relative instructions** (`ADRP`, `ADR`, `LDR literal`, `B`, `BL`, `CBZ`, `TBZ`), because those bytes are copied verbatim into the out-of-line trampoline stub.

### Hook 0 — `HOOK_LANES_CTOR` (`MpcClipLanes::MpcClipLanes`)
- Search `.text` for `ADRP+ADD` (AArch64) or literal-pool (ARM32) references to `RVA_LANES_VPTR`.
- The large constructor (~1–2 KB) that initializes `MpcClipLanes` fields and writes `RVA_LANES_VPTR` to `[this]` is `MpcClipLanes::MpcClipLanes`.
- Verify from its caller in `Root` that at function entry `r0`/`x0` is `lanes`, `r1`/`x1` is `root`, and the caller stores the primary (`flag=0`) `lanes` pointer into `[root + OFF_ROOT_LANES]` (and live `Project*` lives at `[root + OFF_ROOT_PROJECT]`).
- Gen 2 `3.10.1.12`: **`0x01664488`** (entry: `paciasp; stp x29, x30, [sp, #-96]!; mov x29, sp; stp x19, x20, [sp, #16]`).

### Hook 1 — `HOOK_LANES_DTOR` (`MpcClipLanes::~MpcClipLanes`)
- Read vtable slot 0 (complete object destructor `D1`) of `RVA_LANES_VPTR`.
- If the entry contains `ADRP` instructions (as on AArch64 Gen 2, where `+0x08` and `+0x0c` load vtable pages), advance the hook anchor past them to the first 16-byte window of non-PC-relative instructions where `x0` still holds `this` (`lanes`).
- Gen 2 `3.10.1.12`: entry is `0x01666ec0`; hook anchor is **`0x01666ed0`** (`+0x10`: `mov x29, sp; add x9, x1, #0x298; stp x19, x20, [sp, #16]; mov x20, x0`). Putting the hook at `+0x10` also leaves `0x01666ec0` untouched for indirect virtual calls.

### Hook 2 — `HOOK_UI_DRAIN` (`PerThreadQueues::dispatch`)
- Find RTTI string `23JuceOutboundQueueRunnerI15PerThreadQueuesE` (or search for `JuceOutboundQueueRunner` + `PerThreadQueues`) -> resolve its vtable.
- Vtable slot 2 is JUCE's `AsyncUpdater::handleAsyncUpdate()` override, which loads the `PerThreadQueues*` member and tail-branches (`b`) directly into `PerThreadQueues::dispatch`.
- Verify the target function checks/increments a 32-bit reentrancy depth counter (`OFF_PER_THREAD_DEPTH`, `+0x1c8` on Gen 2, `+0x0f0` on Gen 1) on `this` (`queues + OFF_QUEUES_UI_SUBOBJ`).
- Gen 2 `3.10.1.12`: `JuceOutboundQueueRunner<PerThreadQueues>` vtable slot 2 (`0x01b13120`) tail-branches to **`0x030b2360`** (entry: `paciasp; stp x29, x30, [sp, #-48]!; mov x29, sp; stp x19, x20, [sp, #16]`).

---

## 4. Locating Native Function RVAs & Struct Offsets

Once the vtables and constructors are located:

- **`MpcClipLanes` vtable slots:**
  - Slot 4 = `RVA_CLIP_STATE` (`uint32_t(lanes, col, row)`) — bounds-checks `lanes->columns` and `lanes->rows`.
  - Slot 6 = `RVA_CLIP_LOOKUP` (`std::shared_ptr<Clip>(lanes, col, row)`) — returns via `x8` sret on AArch64.
  - Slot 10 = `RVA_CLIP_CREATE` (`void(lanes, col, row)`) — auto-creates an empty Clip Matrix clip.
  - Slot 20 = `RVA_CLIP_TRACK` (`Track*(lanes, col)`) — indexes `lanes->track_vec`.
- **`Root` & `MpcClipLanes` fields:** Read directly from `MpcClipLanes::MpcClipLanes` and its caller in `Root` (`OFF_ROOT_PROJECT`, `OFF_ROOT_LANES`, `OFF_LANES_*`). Verify `OFF_LANES_FLAG == 0` identifies the primary lanes.
- **`CommandFactory` & `ThreadQueues`:**
  - Follow `lanes + OFF_LANES_COMMAND_FACTORY` into `CommandFactory` methods.
  - `RVA_CLIP_SET_LENGTH` and `RVA_TRACK_SET_LENGTH` are called from clip/track length UI actions and allocate command objects pushed to `CommandManager` (`[factory + 0x60]` on Gen 2).
  - `RVA_STEP_COMMAND_FACTORY` (`ModifySequenceEventListCommand` factory) takes `(factory, owner, &removed, &added, &modified, &str0, &str1, 0, 0)` and returns `std::shared_ptr` via `x8` sret on AArch64.
  - `OFF_FACTORY_QUEUES` (`0x08` on Gen 2) points to `ThreadQueues`; `~ThreadQueues` shows `OFF_QUEUES_UI_SUBOBJ` (`queues + 8` on Gen 2, `queues + 4` on Gen 1).
- **`Project` fields:** From `MpcClipLanes` methods reading `[lanes + OFF_LANES_PROJECT]` (`OFF_PROJECT_CUR_SEQUENCE`, `OFF_PROJECT_SEL_TRACK`, `OFF_PROJECT_SEL_ROW`, `OFF_PROJECT_SEL_IS_SEQ`) and `Project` methods pairing `cur_track` with `cur_sequence` (`OFF_PROJECT_CUR_TRACK`).
- **`Clip`, `Track`, `Sequence`, `PluginProgram` fields:**
  - `Clip` ctor shows `OFF_CLIP_EVENT_OWNER` (`SafeLifetime::Holder<UnsafeSequenceEventList>` embedded member) and `RVA_CLIP_EXTENT` (computes `max(clip_length, loop_start + loop_length)` when in loop mode).
  - `Track` ctor/methods show `OFF_TRACK_NAME`, `OFF_TRACK_LOOP_MODE`/`LEN`/`EN`, and `OFF_TRACK_PROGRAM`.
  - `PluginProgram` ctor calls `RVA_STRING_DEFAULT` seven consecutive times for the seven `juce::String` fields at the start of `juce::PluginDescription` (`OFF_PLUGINPROG_NAME`).
  - `Sequence` ctor initializes `OFF_SEQUENCE_LENGTH` (`bars * 3840`).
- **Event RB-tree helpers:**
  - `RVA_STEP_LOWER_BOUND` walks `[tree + 0x10]` (`root`) comparing `[node + 0x20]` (`tick`, confirming `OFF_RB_NODE_PAYLOAD = 0x20` on 64-bit vs `0x10` on 32-bit).
  - `RVA_STEP_NOTE_CTOR` initializes a 56-byte `NativeEvent` (`+0x00` tick, `+0x08` type=3, `+0x10` float vel, `+0x28` int64 len, `+0x30` note, `+0x32` prob).
  - `RVA_STEP_SET_INSERT`, `RVA_STEP_SET_RELEASE`, `RVA_STEP_MODIFIED_RELEASE`, `RVA_SHARED_RELEASE`, `RVA_STRING_DEFAULT`, and `RVA_STRING_DESTROY` are directly called by `ModifySequenceEventListCommand` and its callers.

---

## 5. AArch64 ABI & Hardening Notes

- **PAC / BTI:** Gen 2 `/usr/bin/MPC` uses ARMv8.3-A `paciasp`/`autiasp` and ARMv8.5-A `bti c` (HINT encodings, NOPs on older cores). `src/gate_aarch64.S` starts with `bti c` (`0xd503245f`, valid landing pad for `br x16`) and restores original `sp` and `x30` before running the copied 16-byte prologue so `paciasp` signs the untouched return address with the untouched stack pointer.
- **Indirect return (`x8` sret):** Under AAPCS64 + Itanium C++ ABI, `std::shared_ptr` (non-trivial destructor, 16 bytes) is returned via `x8`, whereas a plain 16-byte C struct returns in `x0:x1`. `src/bridge.c` uses a 24-byte `NativeSharedSret` wrapper on `__aarch64__` so the C compiler passes `&res` in `x8` while the C++ callee writes only the first 16 bytes (`object`, `control`). Verify with `objdump -d build/aarch64/mpc_clip_bridge.so` after any compiler/ABI change.

---

## 6. Registering & Testing a New Profile

1. Append a new `CbProfile` entry to `CB_PROFILES[]` in `src/profiles.c`.
2. Add lookup + prologue-mismatch coverage in `test/test_profiles.c`.
3. Run offline host tests and both cross-builds:
   ```sh
   cc -std=gnu11 -O2 -Wall -Wextra -Werror -o /tmp/test_proto test/test_proto.c src/proto.c && /tmp/test_proto
   cc -std=gnu11 -O2 -Wall -Wextra -Werror -o /tmp/test_profiles test/test_profiles.c src/profiles.c && /tmp/test_profiles
   ./build_armhf.sh
   ./build_aarch64.sh
   ```
4. Confirm 0 exported dynamic symbols (`nm -D --defined-only`) and max `GLIBC_*` version compatibility before testing on hardware.
