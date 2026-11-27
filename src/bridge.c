/* SPDX-License-Identifier: MIT
 * mpc_clip_bridge.so: minimal LD_PRELOAD addin for MPC OS 3.10.1.x
 * (Gen 1 armhf and Gen 2 aarch64, selected by per-build profile table).
 *
 * Exposes native multi-track clip read/write over /run/mpc-ext.sock without
 * moving MPC's UI selection, executing all mutations on MPC's own UI thread
 * through MPC's own Command Factory (preserving Undo, Grid View, and .xpj saves).
 *
 * Credits:
 * - ARM32 trampoline gate and native Command Factory ABI adapted from
 *   zsoltf/mpc-3-ce (MIT).
 * - open-mpc addin lifecycle & coexistence rules from
 *   open-mpc/mpc-vst-plugins (docs/ADDINS.md).
 */
#define _GNU_SOURCE

#include "profiles.h"
#include "proto.h"

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#if defined(__aarch64__)
#define CB_CURRENT_ARCH CB_ARCH_AARCH64
#elif defined(__arm__)
#define CB_CURRENT_ARCH CB_ARCH_ARM32
#else
#error "Unsupported target architecture (expected armhf or aarch64)"
#endif

/* --------------------------------------------------------------------------
 * Trampoline gate context (must match src/gate.S or src/gate_aarch64.S)
 * -------------------------------------------------------------------------- */
#if defined(__aarch64__)
typedef struct {
    uint64_t r[31];   /* x0..x30 */
    uint64_t _pad0;
    uint64_t nzcv;
    uint64_t fpcr;
    uint64_t fpsr;
    uint64_t _pad1;
    uint64_t q[64];   /* q0..q31 (512 bytes) */
} GateCtx;

_Static_assert(offsetof(GateCtx, r) == 0, "GateCtx x0 offset mismatch");
_Static_assert(sizeof(GateCtx) == 800, "GateCtx AArch64 size mismatch");
#else
typedef struct {
    uint64_t d[32];
    uint32_t apsr;
    uint32_t fpscr;
    uint32_t r[13];
    uint32_t lr;
} GateCtx;

_Static_assert(offsetof(GateCtx, r) == 264, "GateCtx r0 offset mismatch");
#endif

extern const unsigned char cb_gate_begin[];
extern const unsigned char cb_gate_end[];
extern const unsigned char cb_gate_handler[];
extern const unsigned char cb_gate_id[];

/* --------------------------------------------------------------------------
 * Native MPC ABI types (32-bit armhf and 64-bit aarch64)
 * -------------------------------------------------------------------------- */
typedef struct { uintptr_t object, control; } NativeShared;
#if defined(__aarch64__)
/* On AArch64, C++ non-trivial std::shared_ptr (16 bytes) returns via x8 (sret),
 * whereas a plain 16-byte C struct returns in x0:x1. Padding to 24 bytes forces
 * AAPCS64 indirect-result passing in x8 while the callee writes only the first
 * 16 bytes (object, control). */
typedef struct { uintptr_t object, control, _sret_pad; } NativeSharedSret;
_Static_assert(sizeof(NativeSharedSret) == 24, "NativeSharedSret x8 sret ABI");
#endif

typedef struct { uintptr_t words[6]; } NativeEventSet;
typedef struct { uint64_t words[7]; } NativeEvent;

_Static_assert(sizeof(NativeEventSet) == 6 * sizeof(uintptr_t), "NativeEventSet ABI");
_Static_assert(sizeof(NativeEvent) == 56, "NativeEvent ABI");

typedef uintptr_t (*RbIncFn)(uintptr_t);

/* --------------------------------------------------------------------------
 * Global runtime state
 * -------------------------------------------------------------------------- */
static const CbProfile *g_prof = NULL;
static uintptr_t        g_bias = 0;
static RbIncFn          g_rb_inc = NULL;
static void            *g_stub_map = NULL;
static size_t           g_stub_len = 0;

enum {
    REQ_IDLE    = 0,
    REQ_PENDING = 1,
    REQ_RUNNING = 2,
    REQ_DONE    = 3
};

static _Atomic uintptr_t g_root = 0;
static _Atomic int       g_running = 0;
static _Atomic int       g_req_state = REQ_IDLE;

/* UI-thread-only monotonic epoch (bumps on project change) & change rev */
static uintptr_t g_last_project = 0;
static uint32_t  g_epoch = 0;
static uint32_t  g_rev = 1;
static uint64_t  g_last_fp = 0;

static CbRequest  g_req;
static CbResponse g_resp;

static pthread_mutex_t g_client_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_server_thread;
static int             g_server_started = 0;
static int             g_listen_fd = -1;

/* --------------------------------------------------------------------------
 * Memory / native call helpers
 * -------------------------------------------------------------------------- */
static inline int ptr_ok(uintptr_t p) {
    return p >= (uintptr_t)0x10000u && (p & (sizeof(uintptr_t) - 1u)) == 0u;
}

static inline uintptr_t rd_ptr(uintptr_t addr) {
    return *(const uintptr_t *)addr;
}

static inline uint32_t rd32(uintptr_t addr) {
    return *(const uint32_t *)addr;
}

static inline int has_vptr(uintptr_t obj, uint32_t rva) {
    return ptr_ok(obj) && rd_ptr(obj) == g_bias + (uintptr_t)rva;
}

#define NATIVE_FN(rva, type) ((type)(g_bias + (uintptr_t)(rva)))

static void event_set_init(NativeEventSet *s) {
    memset(s, 0, sizeof(*s));
    s->words[3] = s->words[4] = (uintptr_t)(s->words + 1);
}

static void event_set_release(NativeEventSet *s) {
    if (s->words[2]) {
        NATIVE_FN(g_prof->rva_step_set_release, void (*)(uintptr_t))(s->words[2]);
    }
}

static void shared_release(NativeShared *sp) {
    if (sp->control) {
        NATIVE_FN(g_prof->rva_shared_release, void (*)(uintptr_t))(sp->control);
    }
    sp->object = 0;
    sp->control = 0;
}

static NativeShared clip_lookup(uintptr_t lanes, uint32_t col, uint32_t row) {
#if defined(__aarch64__)
    NativeSharedSret res = NATIVE_FN(
        g_prof->rva_clip_lookup,
        NativeSharedSret (*)(uintptr_t, uintptr_t, uintptr_t))(
        lanes, (uintptr_t)col, (uintptr_t)row);
    return (NativeShared){ res.object, res.control };
#else
    NativeShared out = { 0, 0 };
    NATIVE_FN(g_prof->rva_clip_lookup,
              void (*)(NativeShared *, uintptr_t, unsigned, unsigned))(
        &out, lanes, col, row);
    return out;
#endif
}

static uintptr_t clip_track(uintptr_t lanes, uint32_t col) {
    return NATIVE_FN(g_prof->rva_clip_track, uintptr_t (*)(uintptr_t, uintptr_t))(
        lanes, (uintptr_t)col);
}

static uint32_t clip_state(uintptr_t lanes, uint32_t col, uint32_t row) {
    return NATIVE_FN(g_prof->rva_clip_state, uint32_t (*)(uintptr_t, uintptr_t, uintptr_t))(
        lanes, (uintptr_t)col, (uintptr_t)row);
}

static void clip_create(uintptr_t lanes, uint32_t col, uint32_t row) {
    NATIVE_FN(g_prof->rva_clip_create, void (*)(uintptr_t, uintptr_t, uintptr_t))(
        lanes, (uintptr_t)col, (uintptr_t)row);
}

static int64_t clip_extent(uintptr_t clip_obj) {
    return NATIVE_FN(g_prof->rva_clip_extent, int64_t (*)(uintptr_t))(clip_obj);
}

static NativeEvent make_note_event(const CbNote *n) {
    float vel = (float)n->vel / 127.0f;
#if defined(__aarch64__)
    return NATIVE_FN(g_prof->rva_step_note_ctor,
                     NativeEvent (*)(unsigned, int64_t, float, int64_t, unsigned, unsigned))(
        (unsigned)n->note, n->tick, vel, n->length, (unsigned)n->prob, 0u);
#else
    NativeEvent ev;
    NATIVE_FN(g_prof->rva_step_note_ctor,
              void (*)(NativeEvent *, unsigned, int64_t, float, int64_t, unsigned, unsigned))(
        &ev, (unsigned)n->note, n->tick, vel, n->length, (unsigned)n->prob, 0u);
    return ev;
#endif
}

static void submit_event_sets(uintptr_t factory, uintptr_t owner,
                              const NativeEventSet *removed,
                              const NativeEventSet *added) {
    NativeEventSet modified;
    uintptr_t strings[2];

    event_set_init(&modified);
    NATIVE_FN(g_prof->rva_string_default, void (*)(uintptr_t *))(&strings[0]);
    NATIVE_FN(g_prof->rva_string_default, void (*)(uintptr_t *))(&strings[1]);

#if defined(__aarch64__)
    NativeSharedSret res = NATIVE_FN(
        g_prof->rva_step_command_factory,
        NativeSharedSret (*)(uintptr_t, uintptr_t,
                             const NativeEventSet *, const NativeEventSet *,
                             const NativeEventSet *,
                             uintptr_t *, uintptr_t *, unsigned, uint32_t))(
        factory, owner, removed, added, &modified,
        &strings[0], &strings[1], 0u, 0u);
    if (res.control) {
        NATIVE_FN(g_prof->rva_shared_release, void (*)(uintptr_t))(res.control);
    }
#else
    uintptr_t result[2] = { 0, 0 };
    NATIVE_FN(g_prof->rva_step_command_factory,
              void (*)(uintptr_t *, uintptr_t, uintptr_t,
                       const NativeEventSet *, const NativeEventSet *,
                       const NativeEventSet *,
                       uintptr_t *, uintptr_t *, unsigned, uint32_t))(
        result, factory, owner, removed, added, &modified,
        &strings[0], &strings[1], 0u, 0u);
    if (result[1]) {
        NATIVE_FN(g_prof->rva_shared_release, void (*)(uintptr_t))(result[1]);
    }
#endif

    NATIVE_FN(g_prof->rva_string_destroy, void (*)(uintptr_t *))(&strings[1]);
    NATIVE_FN(g_prof->rva_string_destroy, void (*)(uintptr_t *))(&strings[0]);
    if (modified.words[2]) {
        NATIVE_FN(g_prof->rva_step_modified_release, void (*)(uintptr_t))(modified.words[2]);
    }
}

/* --------------------------------------------------------------------------
 * UI-thread request execution (runs strictly inside MPC's UI PerThreadQueues)
 * -------------------------------------------------------------------------- */
static void resp_fail(CbResponse *resp, const char *msg) {
    resp->ok = 0;
    snprintf(resp->err, sizeof(resp->err), "%s", msg);
}

static int resolve_context(uintptr_t ui_per_thread_q, uintptr_t *out_lanes,
                           uintptr_t *out_project, uintptr_t *out_factory,
                           uint32_t *out_cols, uint32_t *out_rows) {
    uintptr_t root = atomic_load_explicit(&g_root, memory_order_acquire);
    if (!ptr_ok(root)) return 0;

    uintptr_t lanes = rd_ptr(root + g_prof->off_root_lanes);
    if (!has_vptr(lanes, g_prof->rva_lanes_vptr)) return 0;
    if (rd_ptr(lanes + g_prof->off_lanes_root) != root) return 0;
    if (rd32(lanes + g_prof->off_lanes_flag) != 0) return 0;

    uintptr_t factory = rd_ptr(lanes + g_prof->off_lanes_command_factory);
    uintptr_t project = rd_ptr(root + g_prof->off_root_project);
    if (!ptr_ok(factory) || !has_vptr(project, g_prof->rva_project_vptr)) return 0;
    if (rd_ptr(lanes + g_prof->off_lanes_project) != project) return 0;
    if (rd_ptr(factory + g_prof->off_factory_project) != project) return 0;

    uintptr_t queues = rd_ptr(factory + g_prof->off_factory_queues);
    if (!has_vptr(queues, g_prof->rva_thread_queues_vptr)) return 0;
    uintptr_t ui_q = queues + (uintptr_t)g_prof->off_queues_ui_subobj;
    if (!has_vptr(ui_q, g_prof->rva_per_thread_queues_vptr)) return 0;

    /* Must be executing on MPC's UI thread at reentrancy depth 0 */
    if (ui_per_thread_q != ui_q) return 0;
    if (rd32(ui_per_thread_q + g_prof->off_per_thread_depth) != 0) return 0;

    uintptr_t cols = rd_ptr(lanes + g_prof->off_lanes_columns);
    uintptr_t rows = rd_ptr(lanes + g_prof->off_lanes_rows);
    if (cols > 128u || rows > 128u) return 0;

    *out_lanes = lanes;
    *out_project = project;
    *out_factory = factory;
    *out_cols = (uint32_t)cols;
    *out_rows = (uint32_t)rows;
    return 1;
}

static void find_selected_slot(uintptr_t lanes, uintptr_t project, uint32_t cols,
                               int32_t *sel_col, int32_t *sel_row, int *seq_mode) {
    *sel_col = -1;
    *sel_row = -1;
    *seq_mode = 0;

    uintptr_t sel_track = rd_ptr(project + g_prof->off_project_sel_track);
    if (!ptr_ok(sel_track)) sel_track = rd_ptr(project + g_prof->off_project_cur_track);
    uint8_t is_seq = *(const uint8_t *)(project + g_prof->off_project_sel_is_seq);
    *seq_mode = is_seq ? 1 : 0;
    *sel_row = is_seq ? 0 : (int32_t)rd_ptr(project + g_prof->off_project_sel_row);

    for (uint32_t c = 0; c < cols; c++) {
        if (clip_track(lanes, c) == sel_track) {
            *sel_col = (int32_t)c;
            break;
        }
    }
}

static uint32_t count_note_events(uintptr_t owner) {
    if (!g_rb_inc) return 0;
    uintptr_t sentinel = owner + g_prof->off_event_owner_sentinel;
    uintptr_t node = NATIVE_FN(g_prof->rva_step_lower_bound, uintptr_t (*)(uintptr_t, int64_t))(
        owner + g_prof->off_event_owner_tree, 0);
    uint32_t count = 0;
    for (uint32_t examined = 0; node != sentinel && examined < 8192u; examined++) {
        if (!ptr_ok(node)) break;
        uintptr_t evp = node + (uintptr_t)g_prof->off_rb_node_payload;
        if (*(const uint8_t *)(evp + 0x08u) == 3u) count++;
        node = g_rb_inc(node);
    }
    return count;
}

static void read_juce_str(uintptr_t str_addr, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!ptr_ok(str_addr)) return;
    uintptr_t p = rd_ptr(str_addr);
    if (p < (uintptr_t)0x10000u) return;
    const char *s = (const char *)p;
    size_t i = 0;
    while (i + 1 < cap) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == 0) break;
        if (ch < 0x20 || ch == 0x7f || ch == '"') ch = ' ';
        out[i++] = (char)ch;
    }
    out[i] = '\0';
}

static const char *track_type_name(uintptr_t trk, uintptr_t *out_prog) {
    uintptr_t prog = rd_ptr(trk + g_prof->off_track_program);
    if (out_prog) *out_prog = prog;
    if (!ptr_ok(prog)) return "other";
    uintptr_t v = rd_ptr(prog);
    if (v == g_bias + (uintptr_t)g_prof->rva_drumprogram_vptr)     return "drum";
    if (v == g_bias + (uintptr_t)g_prof->rva_pluginprogram_vptr)   return "plugin";
    if (v == g_bias + (uintptr_t)g_prof->rva_keygroupprogram_vptr) return "keygroup";
    if (v == g_bias + (uintptr_t)g_prof->rva_midiprogram_vptr)     return "midi";
    if (v == g_bias + (uintptr_t)g_prof->rva_cvprogram_vptr)       return "cv";
    if (v == g_bias + (uintptr_t)g_prof->rva_audioprogram_vptr)    return "audio";
    return "other";
}

static int64_t row0_clip_extent(uintptr_t project, uintptr_t trk) {
    int64_t ext = 0;
    uintptr_t seq = rd_ptr(project + g_prof->off_project_cur_sequence);
    if (has_vptr(seq, g_prof->rva_sequence_vptr)) {
        memcpy(&ext, (const void *)(seq + g_prof->off_sequence_length), 8);
    }
    if (has_vptr(trk, g_prof->rva_track_vptr) &&
        rd32(trk + g_prof->off_track_loop_mode) == 1u &&
        *(const uint8_t *)(trk + g_prof->off_track_loop_en) != 0) {
        int64_t tlen = 0;
        memcpy(&tlen, (const void *)(trk + g_prof->off_track_loop_len), 8);
        if (tlen > 0) ext = tlen;
    }
    if (ext < 0 || ext > (int64_t)(960LL * 4LL * 256LL)) ext = 0;
    return ext;
}

static inline uint64_t fnv_u64(uint64_t h, uint64_t v) {
    h ^= v;
    return h * UINT64_C(1099511628211);
}

/* Cheap O(clips) fingerprint reading only RB-tree header pointers + node count
 * (4 words per clip, zero tree traversal) to detect UI/pad/touchscreen edits. */
static void update_epoch_and_rev(uintptr_t lanes, uintptr_t project,
                                 uint32_t cols, uint32_t rows,
                                 int32_t sel_col, int32_t sel_row) {
    if (project != g_last_project) {
        g_last_project = project;
        g_epoch++;
        if (g_epoch == 0) g_epoch = 1;
        g_rev++;
    }
    uint64_t h = UINT64_C(14695981039346656037);
    h = fnv_u64(h, (uint64_t)project);
    h = fnv_u64(h, (uint64_t)rd_ptr(project + g_prof->off_project_cur_sequence));
    h = fnv_u64(h, (uint64_t)cols);
    h = fnv_u64(h, (uint64_t)rows);
    h = fnv_u64(h, (uint64_t)(uint32_t)sel_col);
    h = fnv_u64(h, (uint64_t)(uint32_t)sel_row);
    uint32_t max_c = cols < 16u ? cols : 16u;
    uint32_t max_r = rows < 9u ? rows : 9u;
    for (uint32_t c = 0; c < max_c; c++) {
        uintptr_t trk = clip_track(lanes, c);
        h = fnv_u64(h, (uint64_t)trk);
        if (!has_vptr(trk, g_prof->rva_track_vptr)) continue;
        h = fnv_u64(h, (uint64_t)rd_ptr(trk + g_prof->off_track_program));
        h = fnv_u64(h, (uint64_t)rd_ptr(trk + g_prof->off_track_name));
        for (uint32_t r = 0; r < max_r; r++) {
            NativeShared clip = clip_lookup(lanes, c, r);
            if (has_vptr(clip.object, g_prof->rva_clip_vptr)) {
                uintptr_t owner = clip.object + g_prof->off_clip_event_owner;
                h = fnv_u64(h, (uint64_t)clip.object);
                if (has_vptr(owner, g_prof->rva_event_owner_vptr)) {
                    h = fnv_u64(h, (uint64_t)rd_ptr(owner + g_prof->off_event_owner_root));
                    h = fnv_u64(h, (uint64_t)rd_ptr(owner + g_prof->off_event_owner_left));
                    h = fnv_u64(h, (uint64_t)rd_ptr(owner + g_prof->off_event_owner_right));
                    h = fnv_u64(h, (uint64_t)rd_ptr(owner + g_prof->off_event_owner_count));
                }
                int64_t ext = (r > 0) ? clip_extent(clip.object) : row0_clip_extent(project, trk);
                h = fnv_u64(h, (uint64_t)ext);
            }
            shared_release(&clip);
        }
    }
    if (h != g_last_fp) {
        g_last_fp = h;
        g_rev++;
    }
}

static void execute_on_ui_thread(uintptr_t ui_per_thread_q) {
    const CbRequest *req = &g_req;
    CbResponse *resp = &g_resp;
    memset(resp, 0, sizeof(*resp));

    uintptr_t lanes = 0, project = 0, factory = 0;
    uint32_t cols = 0, rows = 0;
    int ctx_ok = resolve_context(ui_per_thread_q, &lanes, &project, &factory, &cols, &rows);

    if (req->kind == CB_CMD_HELLO) {
        resp->ok = 1;
        resp->ready = ctx_ok;
        resp->cols = cols;
        resp->rows = rows;
        resp->sel_col = -1;
        resp->sel_row = -1;
        if (ctx_ok) {
            find_selected_slot(lanes, project, cols, &resp->sel_col, &resp->sel_row, &resp->seq_mode);
            update_epoch_and_rev(lanes, project, cols, rows, resp->sel_col, resp->sel_row);
            resp->epoch = g_epoch;
            resp->rev = g_rev;
        }
        return;
    }

    if (!ctx_ok) {
        resp_fail(resp, "mpc_not_ready");
        return;
    }
    if (!g_rb_inc) {
        resp_fail(resp, "missing_rb_increment");
        return;
    }

    int32_t sel_col = -1, sel_row = -1;
    int seq_mode = 0;
    find_selected_slot(lanes, project, cols, &sel_col, &sel_row, &seq_mode);
    if (project != g_last_project) {
        g_last_project = project;
        g_epoch++;
        if (g_epoch == 0) g_epoch = 1;
        g_rev++;
    }

    if (req->kind == CB_CMD_TRACKS) {
        for (uint32_t c = 0; c < cols && resp->track_count < CB_MAX_TRACKS; c++) {
            uintptr_t trk = clip_track(lanes, c);
            if (!has_vptr(trk, g_prof->rva_track_vptr)) continue;
            CbTrackInfo *ti = &resp->tracks[resp->track_count++];
            memset(ti, 0, sizeof(*ti));
            ti->col = c;
            uintptr_t prog = 0;
            const char *tname = track_type_name(trk, &prog);
            snprintf(ti->type, sizeof(ti->type), "%s", tname);
            read_juce_str(trk + g_prof->off_track_name, ti->name, sizeof(ti->name));
            if (strcmp(tname, "plugin") == 0 && ptr_ok(prog)) {
                read_juce_str(prog + g_prof->off_pluginprog_name, ti->plugin, sizeof(ti->plugin));
            }
        }
        resp->ok = 1;
        return;
    }

    if (req->kind == CB_CMD_LIST) {
        for (uint32_t c = 0; c < cols && resp->clip_count < CB_MAX_LIST_CLIPS; c++) {
            uintptr_t trk = clip_track(lanes, c);
            if (!has_vptr(trk, g_prof->rva_track_vptr)) continue;
            for (uint32_t r = 0; r < rows && resp->clip_count < CB_MAX_LIST_CLIPS; r++) {
                NativeShared clip = clip_lookup(lanes, c, r);
                if (!has_vptr(clip.object, g_prof->rva_clip_vptr)) {
                    shared_release(&clip);
                    continue;
                }
                uintptr_t owner = clip.object + g_prof->off_clip_event_owner;
                if (has_vptr(owner, g_prof->rva_event_owner_vptr)) {
                    CbClipInfo *ci = &resp->clips[resp->clip_count++];
                    ci->col = c;
                    ci->row = r;
                    ci->state = clip_state(lanes, c, r);
                    ci->note_count = count_note_events(owner);
                    ci->extent = (r > 0) ? clip_extent(clip.object) : row0_clip_extent(project, trk);
                    ci->selected = ((int32_t)c == sel_col && (int32_t)r == sel_row) ? 1u : 0u;
                }
                shared_release(&clip);
            }
        }
        resp->ok = 1;
        return;
    }

    /* Epoch guard on mutating commands */
    int is_mut = (req->kind == CB_CMD_WRITE_BATCH || req->kind == CB_CMD_REPLACE ||
                  req->kind == CB_CMD_DELETE || req->kind == CB_CMD_CLEAR ||
                  req->kind == CB_CMD_SET_LEN);
    if (is_mut && req->if_epoch != 0 && req->if_epoch != g_epoch) {
        resp_fail(resp, "epoch_mismatch");
        return;
    }

    /* All remaining commands target a specific (col, row) clip */
    if (req->col >= cols || req->row >= rows) {
        resp_fail(resp, "clip_out_of_range");
        return;
    }

    if (req->kind == CB_CMD_SET_LEN) {
        uintptr_t trk = clip_track(lanes, req->col);
        if (!has_vptr(trk, g_prof->rva_track_vptr)) {
            resp_fail(resp, "invalid_track");
            return;
        }
        if (req->row == 0u) {
            static const uint8_t zero_disp_mode[0x60] = { 0 };
            NATIVE_FN(g_prof->rva_track_set_length,
                      void (*)(uintptr_t, uintptr_t, int64_t, const void *, unsigned))(
                factory, trk, req->tick_end, zero_disp_mode, 0u);
            g_rev++;
            resp->length_ticks = row0_clip_extent(project, trk);
            resp->ok = 1;
            return;
        }
        uintptr_t seq = rd_ptr(project + g_prof->off_project_cur_sequence);
        if (!has_vptr(seq, g_prof->rva_sequence_vptr)) {
            resp_fail(resp, "invalid_sequence");
            return;
        }
        NativeShared clip_slot = clip_lookup(lanes, req->col, req->row);
        if (!has_vptr(clip_slot.object, g_prof->rva_clip_vptr)) {
            shared_release(&clip_slot);
            clip_create(lanes, req->col, req->row);
            clip_slot = clip_lookup(lanes, req->col, req->row);
        }
        if (!has_vptr(clip_slot.object, g_prof->rva_clip_vptr)) {
            shared_release(&clip_slot);
            resp_fail(resp, "no_clip_at_slot");
            return;
        }
        shared_release(&clip_slot);
        NATIVE_FN(g_prof->rva_clip_set_length,
                  void (*)(uintptr_t, int64_t, unsigned, uintptr_t, uintptr_t, unsigned))(
            factory, req->tick_end, 1u, seq, trk + g_prof->off_track_name, req->row);
        g_rev++;
        clip_slot = clip_lookup(lanes, req->col, req->row);
        resp->length_ticks = has_vptr(clip_slot.object, g_prof->rva_clip_vptr)
            ? clip_extent(clip_slot.object) : req->tick_end;
        shared_release(&clip_slot);
        resp->ok = 1;
        return;
    }

    NativeShared clip = clip_lookup(lanes, req->col, req->row);
    /* Auto-create an empty Clip Matrix slot (row >= 1) when adding notes */
    if (!has_vptr(clip.object, g_prof->rva_clip_vptr) && req->row >= 1u &&
        (req->kind == CB_CMD_WRITE_BATCH || (req->kind == CB_CMD_REPLACE && req->batch_count > 0))) {
        shared_release(&clip);
        clip_create(lanes, req->col, req->row);
        g_rev++;
        clip = clip_lookup(lanes, req->col, req->row);
    }
    if (!has_vptr(clip.object, g_prof->rva_clip_vptr)) {
        shared_release(&clip);
        resp_fail(resp, "no_clip_at_slot");
        return;
    }
    uintptr_t owner = clip.object + g_prof->off_clip_event_owner;
    if (!has_vptr(owner, g_prof->rva_event_owner_vptr)) {
        shared_release(&clip);
        resp_fail(resp, "invalid_event_owner");
        return;
    }

    if (req->kind == CB_CMD_READ) {
        uintptr_t sentinel = owner + g_prof->off_event_owner_sentinel;
        uintptr_t node = NATIVE_FN(g_prof->rva_step_lower_bound, uintptr_t (*)(uintptr_t, int64_t))(
            owner + g_prof->off_event_owner_tree, 0);
        for (uint32_t examined = 0; node != sentinel && examined < 8192u; examined++) {
            if (!ptr_ok(node)) break;
            uintptr_t evp = node + (uintptr_t)g_prof->off_rb_node_payload;
            if (*(const uint8_t *)(evp + 0x08u) == 3u) {
                if (resp->note_count < CB_MAX_READ_NOTES) {
                    int64_t tick = 0, len = 0;
                    float vel_f = 0.0f;
                    memcpy(&tick,  (const void *)(evp + 0x00u), 8);
                    memcpy(&vel_f, (const void *)(evp + 0x10u), 4);
                    memcpy(&len,   (const void *)(evp + 0x28u), 8);
                    uint8_t note = *(const uint8_t *)(evp + 0x30u);
                    uint8_t prob = *(const uint8_t *)(evp + 0x32u);
                    int v = (int)(vel_f * 127.0f + 0.5f);
                    if (v < 1) v = 1;
                    if (v > 127) v = 127;
                    resp->notes[resp->note_count++] = (CbNote){
                        .tick = tick, .length = len,
                        .note = note, .vel = (uint8_t)v, .prob = prob
                    };
                }
            }
            node = g_rb_inc(node);
        }
        shared_release(&clip);
        resp->ok = 1;
        return;
    }

    if (req->kind == CB_CMD_WRITE_BATCH) {
        NativeEventSet removed, added;
        event_set_init(&removed);
        event_set_init(&added);

        for (uint32_t i = 0; i < req->batch_count; i++) {
            NativeEvent ev = make_note_event(&req->batch[i]);
            NATIVE_FN(g_prof->rva_step_set_insert, uintptr_t (*)(NativeEventSet *, const NativeEvent *))(
                &added, &ev);
        }

        if (added.words[5] > 0) {
            submit_event_sets(factory, owner, &removed, &added);
            g_rev++;
        }
        resp->affected = (uint32_t)added.words[5];
        event_set_release(&added);
        event_set_release(&removed);
        shared_release(&clip);
        resp->ok = 1;
        return;
    }

    if (req->kind == CB_CMD_REPLACE) {
        NativeEventSet removed, added;
        event_set_init(&removed);
        event_set_init(&added);

        if (req->tick_end > req->tick_start) {
            uintptr_t sentinel = owner + g_prof->off_event_owner_sentinel;
            uintptr_t node = NATIVE_FN(g_prof->rva_step_lower_bound, uintptr_t (*)(uintptr_t, int64_t))(
                owner + g_prof->off_event_owner_tree, req->tick_start);
            for (uint32_t examined = 0; node != sentinel && examined < 8192u; examined++) {
                if (!ptr_ok(node)) break;
                uintptr_t evp = node + (uintptr_t)g_prof->off_rb_node_payload;
                int64_t tick = 0;
                memcpy(&tick, (const void *)(evp + 0x00u), 8);
                if (tick >= req->tick_end) break;
                if (*(const uint8_t *)(evp + 0x08u) == 3u) {
                    uint8_t note = *(const uint8_t *)(evp + 0x30u);
                    if (req->note_filter < 0 || (int32_t)note == req->note_filter) {
                        NativeEvent ev;
                        memcpy(&ev, (const void *)evp, sizeof(ev));
                        NATIVE_FN(g_prof->rva_step_set_insert,
                                  uintptr_t (*)(NativeEventSet *, const NativeEvent *))(
                            &removed, &ev);
                    }
                }
                node = g_rb_inc(node);
            }
        }

        for (uint32_t i = 0; i < req->batch_count; i++) {
            NativeEvent ev = make_note_event(&req->batch[i]);
            NATIVE_FN(g_prof->rva_step_set_insert, uintptr_t (*)(NativeEventSet *, const NativeEvent *))(
                &added, &ev);
        }

        if (removed.words[5] > 0 || added.words[5] > 0) {
            submit_event_sets(factory, owner, &removed, &added);
            g_rev++;
        }
        resp->removed = (uint32_t)removed.words[5];
        resp->affected = (uint32_t)added.words[5];
        event_set_release(&added);
        event_set_release(&removed);
        shared_release(&clip);
        resp->ok = 1;
        return;
    }

    if (req->kind == CB_CMD_DELETE || req->kind == CB_CMD_CLEAR) {
        int64_t t0 = (req->kind == CB_CMD_CLEAR) ? 0 : req->tick_start;
        int64_t t1 = (req->kind == CB_CMD_CLEAR) ? INT64_MAX : req->tick_end;
        int32_t nf = (req->kind == CB_CMD_CLEAR) ? -1 : req->note_filter;

        NativeEventSet removed, added;
        event_set_init(&removed);
        event_set_init(&added);

        uintptr_t sentinel = owner + g_prof->off_event_owner_sentinel;
        uintptr_t node = NATIVE_FN(g_prof->rva_step_lower_bound, uintptr_t (*)(uintptr_t, int64_t))(
            owner + g_prof->off_event_owner_tree, t0);
        for (uint32_t examined = 0; node != sentinel && examined < 8192u; examined++) {
            if (!ptr_ok(node)) break;
            uintptr_t evp = node + (uintptr_t)g_prof->off_rb_node_payload;
            int64_t tick = 0;
            memcpy(&tick, (const void *)(evp + 0x00u), 8);
            if (tick >= t1) break;
            if (*(const uint8_t *)(evp + 0x08u) == 3u) {
                uint8_t note = *(const uint8_t *)(evp + 0x30u);
                if (nf < 0 || (int32_t)note == nf) {
                    NativeEvent ev;
                    memcpy(&ev, (const void *)evp, sizeof(ev));
                    NATIVE_FN(g_prof->rva_step_set_insert,
                              uintptr_t (*)(NativeEventSet *, const NativeEvent *))(
                        &removed, &ev);
                }
            }
            node = g_rb_inc(node);
        }

        if (removed.words[5] > 0) {
            submit_event_sets(factory, owner, &removed, &added);
            g_rev++;
        }
        resp->affected = (uint32_t)removed.words[5];
        event_set_release(&added);
        event_set_release(&removed);
        shared_release(&clip);
        resp->ok = 1;
        return;
    }

    shared_release(&clip);
    resp_fail(resp, "unsupported_cmd");
}

/* --------------------------------------------------------------------------
 * Hook dispatcher called from gate.S / gate_aarch64.S
 * -------------------------------------------------------------------------- */
static void bridge_hook(GateCtx *ctx, uint32_t hook_id) {
    if (hook_id == HOOK_LANES_CTOR) {
        /* r1 / x1 is the owning root object (holds primary flag=0 lanes & live project) */
        uintptr_t arg1 = (uintptr_t)ctx->r[1];
        if (ptr_ok(arg1)) {
            atomic_store_explicit(&g_root, arg1, memory_order_release);
        }
        return;
    }
    if (hook_id == HOOK_LANES_DTOR) {
        uintptr_t arg0 = (uintptr_t)ctx->r[0];
        uintptr_t root = atomic_load_explicit(&g_root, memory_order_acquire);
        if (ptr_ok(root) && rd_ptr(root + g_prof->off_root_lanes) == arg0) {
            atomic_store_explicit(&g_root, 0, memory_order_release);
        }
        return;
    }
    if (hook_id == HOOK_UI_DRAIN) {
        /* Ultra-cheap fast path when no request is pending (single atomic load) */
        if (atomic_load_explicit(&g_req_state, memory_order_acquire) != REQ_PENDING) return;

        /* Strictly verify this is MPC's UI PerThreadQueues at depth 0 —
         * never run on audio/worker queues, even while MPC is still booting. */
        uintptr_t arg0 = (uintptr_t)ctx->r[0];
        uintptr_t root = atomic_load_explicit(&g_root, memory_order_acquire);
        if (!ptr_ok(root)) return;
        uintptr_t lanes = rd_ptr(root + g_prof->off_root_lanes);
        if (!has_vptr(lanes, g_prof->rva_lanes_vptr)) return;
        uintptr_t factory = rd_ptr(lanes + g_prof->off_lanes_command_factory);
        if (!ptr_ok(factory)) return;
        uintptr_t queues = rd_ptr(factory + g_prof->off_factory_queues);
        if (!has_vptr(queues, g_prof->rva_thread_queues_vptr)) return;
        if (arg0 != queues + (uintptr_t)g_prof->off_queues_ui_subobj) return;
        if (rd32(arg0 + g_prof->off_per_thread_depth) != 0) return;

        int expected = REQ_PENDING;
        if (!atomic_compare_exchange_strong_explicit(
                &g_req_state, &expected, REQ_RUNNING,
                memory_order_acq_rel, memory_order_relaxed)) {
            return;
        }

        execute_on_ui_thread(arg0);
        atomic_store_explicit(&g_req_state, REQ_DONE, memory_order_release);
    }
}

/* --------------------------------------------------------------------------
 * Socket server thread (background, low priority, signals blocked)
 * -------------------------------------------------------------------------- */
static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (n == 0) return 0;
        sent += (size_t)n;
    }
    return 1;
}

static int dispatch_to_ui(const CbRequest *req, CbResponse *out_resp) {
    /* Fast answer before a project is open so HELLO never stalls or hits non-UI queues */
    if (atomic_load_explicit(&g_root, memory_order_acquire) == 0) {
        memset(out_resp, 0, sizeof(*out_resp));
        if (req->kind == CB_CMD_HELLO) {
            out_resp->ok = 1;
            out_resp->ready = 0;
            out_resp->sel_col = -1;
            out_resp->sel_row = -1;
            return 1;
        }
        resp_fail(out_resp, "mpc_not_ready");
        return 0;
    }

    pthread_mutex_lock(&g_client_mu);
    g_req = *req;
    atomic_store_explicit(&g_req_state, REQ_PENDING, memory_order_release);

    int done = 0;
    for (int ms = 0; ms < 2000; ms++) {
        if (atomic_load_explicit(&g_req_state, memory_order_acquire) == REQ_DONE) {
            done = 1;
            break;
        }
        if (!atomic_load_explicit(&g_running, memory_order_acquire)) break;
        struct timespec ts = { 0, 1000000L }; /* 1 ms */
        nanosleep(&ts, NULL);
    }

    if (!done) {
        /* Try to cancel cleanly before the UI thread picks it up */
        int expected = REQ_PENDING;
        if (!atomic_compare_exchange_strong_explicit(
                &g_req_state, &expected, REQ_IDLE,
                memory_order_acq_rel, memory_order_acquire)) {
            /* UI thread already claimed it (REQ_RUNNING or REQ_DONE); wait for it to finish */
            while (atomic_load_explicit(&g_req_state, memory_order_acquire) != REQ_DONE) {
                struct timespec ts = { 0, 1000000L };
                nanosleep(&ts, NULL);
            }
            done = 1;
        }
    }

    if (done) {
        *out_resp = g_resp;
        atomic_store_explicit(&g_req_state, REQ_IDLE, memory_order_release);
    } else {
        memset(out_resp, 0, sizeof(*out_resp));
        resp_fail(out_resp, "ui_thread_timeout");
    }
    pthread_mutex_unlock(&g_client_mu);
    return done;
}

enum { CB_MAX_CLIENTS = 8 };

typedef struct {
    int        fd;
    char       line[512];
    size_t     line_len;
    uint32_t   expect_lines;
    int        bad_batch;
    char       batch_err[128];
    CbRequest  req;
} ClientSlot;

static int process_client_line(ClientSlot *cl, const char *line, char *out_buf, size_t out_cap) {
    const char *fw = g_prof ? g_prof->fw_version : "unknown";
    if (cl->expect_lines > 0) {
        if (!cl->bad_batch) {
            if (!cb_parse_note_line(line, &cl->req.batch[cl->req.batch_count],
                                    cl->batch_err, sizeof(cl->batch_err))) {
                cl->bad_batch = 1;
            } else {
                cl->req.batch_count++;
            }
        }
        cl->expect_lines--;
        if (cl->expect_lines > 0) return 1;

        if (cl->bad_batch) {
            int n = snprintf(out_buf, out_cap, "ERR %s\n", cl->batch_err);
            return send_all(cl->fd, out_buf, (size_t)n);
        }
        CbResponse resp;
        dispatch_to_ui(&cl->req, &resp);
        size_t out_len = cb_format_response(&cl->req, &resp, fw, out_buf, out_cap);
        return send_all(cl->fd, out_buf, out_len);
    }

    if (line[0] == '\0') return 1;

    uint32_t expect = 0;
    char err[128] = {0};
    if (!cb_parse_header(line, &cl->req, &expect, err, sizeof(err))) {
        int n = snprintf(out_buf, out_cap, "ERR %s\n", err);
        return send_all(cl->fd, out_buf, (size_t)n);
    }
    if (expect > 0) {
        cl->expect_lines = expect;
        cl->bad_batch = 0;
        cl->batch_err[0] = '\0';
        return 1;
    }

    CbResponse resp;
    dispatch_to_ui(&cl->req, &resp);
    size_t out_len = cb_format_response(&cl->req, &resp, fw, out_buf, out_cap);
    return send_all(cl->fd, out_buf, out_len);
}

static int drain_client(ClientSlot *cl, char *out_buf, size_t out_cap) {
    char tmp[512];
    for (;;) {
        ssize_t r = recv(cl->fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            return 0;
        }
        if (r == 0) return 0;

        for (ssize_t i = 0; i < r; i++) {
            char c = tmp[i];
            if (c == '\r') continue;
            if (c == '\n') {
                cl->line[cl->line_len] = '\0';
                cl->line_len = 0;
                if (!process_client_line(cl, cl->line, out_buf, out_cap)) return 0;
            } else if (cl->line_len + 1 < sizeof(cl->line)) {
                cl->line[cl->line_len++] = c;
            }
        }
    }
}

static void *server_main(void *arg) {
    (void)arg;
    setpriority(PRIO_PROCESS, 0, 10);

    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);

    unlink(MPC_EXT_SOCK_PATH);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return NULL;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", MPC_EXT_SOCK_PATH);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }
    chmod(MPC_EXT_SOCK_PATH, 0660);
    if (listen(fd, 8) != 0) {
        close(fd);
        unlink(MPC_EXT_SOCK_PATH);
        return NULL;
    }
    g_listen_fd = fd;

    static char out_buf[131072];
    ClientSlot *clients = calloc(CB_MAX_CLIENTS, sizeof(*clients));
    if (!clients) {
        close(fd);
        g_listen_fd = -1;
        unlink(MPC_EXT_SOCK_PATH);
        return NULL;
    }
    for (int i = 0; i < CB_MAX_CLIENTS; i++) clients[i].fd = -1;

    while (atomic_load_explicit(&g_running, memory_order_acquire)) {
        struct pollfd pfds[1 + CB_MAX_CLIENTS];
        int slot_map[1 + CB_MAX_CLIENTS];
        nfds_t nfds = 0;

        pfds[nfds].fd = fd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        slot_map[nfds++] = -1;

        for (int i = 0; i < CB_MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0) {
                pfds[nfds].fd = clients[i].fd;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                slot_map[nfds++] = i;
            }
        }

        int pr = poll(pfds, nfds, 250);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        if (pfds[0].revents & POLLIN) {
            for (;;) {
                int cfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (cfd < 0) break;
                int placed = 0;
                for (int i = 0; i < CB_MAX_CLIENTS; i++) {
                    if (clients[i].fd < 0) {
                        memset(&clients[i], 0, sizeof(clients[i]));
                        clients[i].fd = cfd;
                        struct timeval tv = { 2, 0 };
                        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                        placed = 1;
                        break;
                    }
                }
                if (!placed) close(cfd);
            }
        }

        for (nfds_t k = 1; k < nfds; k++) {
            int idx = slot_map[k];
            if (pfds[k].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (!drain_client(&clients[idx], out_buf, sizeof(out_buf))) {
                    close(clients[idx].fd);
                    clients[idx].fd = -1;
                }
            }
        }
    }

    for (int i = 0; i < CB_MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0) close(clients[i].fd);
    }
    free(clients);
    close(fd);
    g_listen_fd = -1;
    unlink(MPC_EXT_SOCK_PATH);
    return NULL;
}

/* --------------------------------------------------------------------------
 * ELF identity check + transactional ARM32 / AArch64 inline hook installer
 * -------------------------------------------------------------------------- */
static int phdr_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    (void)data;
    if (info->dlpi_name && info->dlpi_name[0] != '\0') return 0;

    int has_load0 = 0;
    const CbProfile *matched = NULL;
    for (unsigned i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type == PT_LOAD && p->p_offset == 0 && p->p_vaddr == 0 &&
            (p->p_flags & (PF_R | PF_X)) == (PF_R | PF_X)) {
            has_load0 = 1;
        }
        if (p->p_type == PT_NOTE && p->p_memsz <= 4096u) {
            const uint8_t *s = (const uint8_t *)(info->dlpi_addr + p->p_vaddr);
            const uint8_t *end = s + p->p_memsz;
            while (s + sizeof(ElfW(Nhdr)) <= end) {
                ElfW(Nhdr) h;
                memcpy(&h, s, sizeof(h));
                s += sizeof(h);
                uint32_t n = (h.n_namesz + 3u) & ~3u;
                uint32_t d = (h.n_descsz + 3u) & ~3u;
                if (n > 4096u || d > 4096u || s + n + d > end) break;
                if (h.n_type == NT_GNU_BUILD_ID && h.n_namesz == 4 && h.n_descsz == 20 &&
                    memcmp(s, "GNU", 4) == 0) {
                    matched = cb_profile_find(CB_CURRENT_ARCH, s + n);
                }
                s += n + d;
            }
        }
    }
    if (has_load0 && matched) {
        g_bias = (uintptr_t)info->dlpi_addr;
        g_prof = matched;
    }
    return 0;
}

static int is_mpc_process(void) {
    char exe[128];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return 0;
    exe[n] = '\0';
    return strcmp(exe, MPC_EXE_PATH) == 0;
}

#if defined(__aarch64__)
enum { CB_JUMP_LEN = 16 };
static void emit_jump(uint8_t *dst, uintptr_t target) {
    uint32_t insn0 = 0x58000050u; /* ldr x16, #8 */
    uint32_t insn1 = 0xd61f0200u; /* br  x16     */
    uint64_t addr  = (uint64_t)target;
    memcpy(dst + 0, &insn0, 4);
    memcpy(dst + 4, &insn1, 4);
    memcpy(dst + 8, &addr,  8);
}
#else
enum { CB_JUMP_LEN = 8 };
static void emit_jump(uint8_t *dst, uintptr_t target) {
    uint32_t insn0 = 0xe51ff004u; /* ldr pc, [pc, #-4] */
    uint32_t addr  = (uint32_t)target;
    memcpy(dst + 0, &insn0, 4);
    memcpy(dst + 4, &addr,  4);
}
#endif

static int install_hooks(void) {
    if (!g_prof || g_prof->hook_len != CB_JUMP_LEN) return 0;

    /* 1. Verify all 3 anchor prologues before touching any code page */
    if (!cb_profile_verify_prologues(g_prof, (const uint8_t *)g_bias, 0)) {
        return 0;
    }

    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page_sz = (ps >= 4096) ? (uintptr_t)ps : 4096u;
    uintptr_t page_mask = ~(page_sz - 1u);

    const size_t stub_stride = 512u;
    size_t gate_len = (size_t)(cb_gate_end - cb_gate_begin);
    size_t handler_off = (size_t)(cb_gate_handler - cb_gate_begin);
    size_t id_off = (size_t)(cb_gate_id - cb_gate_begin);
    if (gate_len + g_prof->hook_len + CB_JUMP_LEN > stub_stride) return 0;

    g_stub_len = (HOOK_COUNT * stub_stride + page_sz - 1u) & page_mask;
    unsigned char *code = mmap(NULL, g_stub_len, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 0;

    uintptr_t handler_addr = (uintptr_t)&bridge_hook;
    for (int i = 0; i < HOOK_COUNT; i++) {
        unsigned char *dst = code + (size_t)i * stub_stride;
        memcpy(dst, cb_gate_begin, gate_len);
        memcpy(dst + handler_off, &handler_addr, sizeof(handler_addr));
        uint32_t hid = (uint32_t)i;
        memcpy(dst + id_off, &hid, sizeof(hid));

        uint8_t *tail = dst + gate_len;
        memcpy(tail, g_prof->hook_expected[i], g_prof->hook_len);
        tail += g_prof->hook_len;
        emit_jump(tail, g_bias + (uintptr_t)g_prof->hook_rva[i] + g_prof->hook_len);
    }

    __builtin___clear_cache((char *)code, (char *)code + g_stub_len);
    if (mprotect(code, g_stub_len, PROT_READ | PROT_EXEC) != 0) {
        munmap(code, g_stub_len);
        return 0;
    }

    /* Collect unique pages touched by the hook sites */
    uintptr_t pages[HOOK_COUNT * 2];
    int npages = 0;
    for (int i = 0; i < HOOK_COUNT; i++) {
        uintptr_t base = g_bias + (uintptr_t)g_prof->hook_rva[i];
        uintptr_t ends[2] = { base, base + g_prof->hook_len - 1u };
        for (int k = 0; k < 2; k++) {
            uintptr_t pg = ends[k] & page_mask;
            int dup = 0;
            for (int j = 0; j < npages; j++) if (pages[j] == pg) dup = 1;
            if (!dup) pages[npages++] = pg;
        }
    }

    for (int i = 0; i < npages; i++) {
        if (mprotect((void *)pages[i], page_sz, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            for (int j = 0; j < i; j++) mprotect((void *)pages[j], page_sz, PROT_READ | PROT_EXEC);
            munmap(code, g_stub_len);
            return 0;
        }
    }

    for (int i = 0; i < HOOK_COUNT; i++) {
        uintptr_t site = g_bias + (uintptr_t)g_prof->hook_rva[i];
        emit_jump((uint8_t *)site, (uintptr_t)(code + (size_t)i * stub_stride));
        __builtin___clear_cache((char *)site, (char *)(site + g_prof->hook_len));
    }

    for (int i = 0; i < npages; i++) {
        mprotect((void *)pages[i], page_sz, PROT_READ | PROT_EXEC);
    }

    g_stub_map = code;
    return 1;
}

__attribute__((constructor))
static void mpc_clip_bridge_start(void) {
    if (getenv("MPC_CLIP_BRIDGE_DISABLE")) return;
    if (access("/data/mpc-addins/clip-bridge/disabled", F_OK) == 0) return;
    if (!is_mpc_process()) return;

    dl_iterate_phdr(phdr_cb, NULL);
    if (!g_prof || g_bias == 0 || (g_bias & 4095u) != 0) return;

    g_rb_inc = (RbIncFn)dlsym(RTLD_DEFAULT, "_ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base");
    if (!g_rb_inc) return;

    if (!install_hooks()) return;

    atomic_store_explicit(&g_running, 1, memory_order_release);

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) == 0) {
        pthread_attr_setstacksize(&attr, 256u * 1024u);
        if (pthread_create(&g_server_thread, &attr, server_main, NULL) == 0) {
            g_server_started = 1;
        }
        pthread_attr_destroy(&attr);
    }
}

__attribute__((destructor))
static void mpc_clip_bridge_stop(void) {
    atomic_store_explicit(&g_running, 0, memory_order_release);
    if (g_listen_fd >= 0) shutdown(g_listen_fd, SHUT_RDWR);
    if (g_server_started) {
        pthread_join(g_server_thread, NULL);
        g_server_started = 0;
    }
}
