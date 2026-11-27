/* SPDX-License-Identifier: MIT
 * Per-build target profiles for /usr/bin/MPC (Gen 1 armhf and Gen 2 aarch64).
 *
 * Adding a new MPC firmware Build ID (e.g. Gen 2 3.10.1.49) is a pure data
 * change: append a CbProfile entry in src/profiles.c. At runtime, the addin
 * refuses to arm unless /proc/self/exe, architecture, GNU Build ID, and all
 * three hook prologues match an entry in the table.
 */
#ifndef MPC_CLIP_BRIDGE_PROFILES_H
#define MPC_CLIP_BRIDGE_PROFILES_H

#include <stddef.h>
#include <stdint.h>

#define MPC_EXE_PATH "/usr/bin/MPC"

typedef enum {
    CB_ARCH_ARM32   = 1,
    CB_ARCH_AARCH64 = 2
} CbArch;

enum {
    HOOK_LANES_CTOR = 0,
    HOOK_LANES_DTOR = 1,
    HOOK_UI_DRAIN   = 2,
    HOOK_COUNT      = 3,
    CB_MAX_HOOK_LEN = 16
};

typedef struct {
    const char *name;
    const char *fw_version;
    CbArch      arch;
    uint8_t     build_id[20];

    /* Hook anchor RVAs and expected non-PC-relative prologues
     * (8 bytes on ARM32, 16 bytes on AArch64). */
    uint8_t     hook_len;
    uint32_t    hook_rva[HOOK_COUNT];
    uint8_t     hook_expected[HOOK_COUNT][CB_MAX_HOOK_LEN];

    /* C++ vtable pointer RVAs (resolved against PIE load bias) */
    uint32_t    rva_lanes_vptr;
    uint32_t    rva_clip_vptr;
    uint32_t    rva_track_vptr;
    uint32_t    rva_sequence_vptr;
    uint32_t    rva_project_vptr;
    uint32_t    rva_event_owner_vptr;
    uint32_t    rva_per_thread_queues_vptr;
    uint32_t    rva_thread_queues_vptr;

    uint32_t    rva_drumprogram_vptr;
    uint32_t    rva_audioprogram_vptr;
    uint32_t    rva_midiprogram_vptr;
    uint32_t    rva_pluginprogram_vptr;
    uint32_t    rva_cvprogram_vptr;
    uint32_t    rva_keygroupprogram_vptr;

    /* Struct field offsets */
    uint32_t    off_root_project;
    uint32_t    off_root_lanes;

    uint32_t    off_lanes_columns;
    uint32_t    off_lanes_rows;
    uint32_t    off_lanes_root;
    uint32_t    off_lanes_command_factory;
    uint32_t    off_lanes_command_manager;
    uint32_t    off_lanes_project;
    uint32_t    off_lanes_flag;
    uint32_t    off_lanes_track_vec;
    uint32_t    off_lanes_clip_pool;

    uint32_t    off_factory_queues;
    uint32_t    off_factory_project;
    uint32_t    off_queues_ui_subobj;

    uint32_t    off_project_cur_track;
    uint32_t    off_project_cur_sequence;
    uint32_t    off_project_sel_track;
    uint32_t    off_project_sel_row;
    uint32_t    off_project_sel_is_seq;

    uint32_t    off_clip_event_owner;
    uint32_t    off_event_owner_tree;
    uint32_t    off_event_owner_sentinel;
    uint32_t    off_event_owner_root;
    uint32_t    off_event_owner_left;
    uint32_t    off_event_owner_right;
    uint32_t    off_event_owner_count;
    uint32_t    off_rb_node_payload;

    uint32_t    off_track_name;
    uint32_t    off_track_loop_mode;
    uint32_t    off_track_loop_len;
    uint32_t    off_track_loop_en;
    uint32_t    off_track_program;
    uint32_t    off_pluginprog_name;
    uint32_t    off_sequence_length;

    uint32_t    off_per_thread_depth;

    /* Native function RVAs */
    uint32_t    rva_clip_track;
    uint32_t    rva_clip_lookup;
    uint32_t    rva_clip_state;
    uint32_t    rva_clip_create;
    uint32_t    rva_clip_extent;
    uint32_t    rva_clip_set_length;
    uint32_t    rva_track_set_length;
    uint32_t    rva_shared_release;

    uint32_t    rva_step_lower_bound;
    uint32_t    rva_step_note_ctor;
    uint32_t    rva_step_set_insert;
    uint32_t    rva_step_set_release;
    uint32_t    rva_step_modified_release;
    uint32_t    rva_step_command_factory;
    uint32_t    rva_string_default;
    uint32_t    rva_string_destroy;
} CbProfile;

/* Look up a profile matching both CPU architecture and exact 20-byte Build ID.
 * Returns NULL if no profile matches. */
const CbProfile *cb_profile_find(CbArch arch, const uint8_t build_id[20]);

/* Enumerate registered profiles (useful for tests/diagnostics). */
size_t cb_profile_count(void);
const CbProfile *cb_profile_at(size_t index);

/* Verify that all HOOK_COUNT prologues in prof match the bytes at
 * image_base + prof->hook_rva[i]. If image_size != 0, bounds-checks each hook
 * against [0, image_size). Returns 1 only if every hook prologue matches. */
int cb_profile_verify_prologues(const CbProfile *prof,
                                const uint8_t *image_base,
                                size_t image_size);

#endif
