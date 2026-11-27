/* SPDX-License-Identifier: MIT
 * Per-build profile table and pure verification helpers.
 *
 * Native clip/event ABI derived from zsoltf/mpc-3-ce (MIT) and verified
 * against Gen 1 (armhf) and Gen 2 (aarch64) MPC OS 3.10.1.12 binaries.
 */
#include "profiles.h"

#include <string.h>

static const CbProfile CB_PROFILES[] = {
    /* ----------------------------------------------------------------------
     * Gen 1 MPC OS 3.10.1.12 (/usr/bin/MPC, ELF32 ARM hard-float PIE)
     * Verified live on Gen 1 MPC One.
     * ---------------------------------------------------------------------- */
    {
        .name       = "gen1-3.10.1.12",
        .fw_version = "3.10.1.12",
        .arch       = CB_ARCH_ARM32,
        .build_id   = {
            0x85, 0x1d, 0x05, 0x0e, 0xb0, 0x3c, 0xe0, 0xb8, 0x66, 0x3a,
            0xaa, 0x57, 0x2b, 0x25, 0x99, 0x81, 0x3c, 0xbf, 0x53, 0xd4
        },

        .hook_len = 8u,
        .hook_rva = {
            0x01030d20u, /* MpcClipLanes::MpcClipLanes entry (r0 = lanes, r1 = root) */
            0x01032f70u, /* MpcClipLanes::~MpcClipLanes +4   (r0 = lanes) */
            0x02d6d0c4u  /* PerThreadQueues::dispatch entry  (r0 = queues + 4) */
        },
        .hook_expected = {
            /* strd r4,r5,[sp,#-36]!; mov r5,r2 */
            { 0xf4, 0x42, 0x6d, 0xe1, 0x02, 0x50, 0xa0, 0xe1 },
            /* strd r4,r5,[sp,#-24]!; strd r6,r7,[sp,#8] */
            { 0xf8, 0x41, 0x6d, 0xe1, 0xf8, 0x60, 0xcd, 0xe1 },
            /* strd r4,r5,[sp,#-16]!; mov r5,r0 */
            { 0xf0, 0x41, 0x6d, 0xe1, 0x00, 0x50, 0xa0, 0xe1 }
        },

        .rva_lanes_vptr             = 0x06dec620u,
        .rva_clip_vptr              = 0x06e9b394u,
        .rva_track_vptr             = 0x06ea28c8u,
        .rva_sequence_vptr          = 0x06ea1f68u,
        .rva_project_vptr           = 0x06ea1468u,
        .rva_event_owner_vptr       = 0x06df0c18u,
        .rva_per_thread_queues_vptr = 0x06e08ed8u,
        .rva_thread_queues_vptr     = 0x06e08eecu,

        .rva_drumprogram_vptr       = 0x06e9e648u,
        .rva_audioprogram_vptr      = 0x06e9d8e4u,
        .rva_midiprogram_vptr       = 0x06e9fd70u,
        .rva_pluginprogram_vptr     = 0x06ea0488u,
        .rva_cvprogram_vptr         = 0x06e9e45cu,
        .rva_keygroupprogram_vptr   = 0x06e9eea0u,

        .off_root_project           = 0x26cu,
        .off_root_lanes             = 0x78cu,

        .off_lanes_columns          = 0x034u,
        .off_lanes_rows             = 0x048u,
        .off_lanes_root             = 0x178u,
        .off_lanes_command_factory  = 0x17cu,
        .off_lanes_command_manager  = 0x180u,
        .off_lanes_project          = 0x184u,
        .off_lanes_flag             = 0x1b8u,
        .off_lanes_track_vec        = 0x1bcu,
        .off_lanes_clip_pool        = 0x1c0u,

        .off_factory_queues         = 0x004u,
        .off_factory_project        = 0x014u,
        .off_queues_ui_subobj       = 0x004u,

        .off_project_cur_track      = 0x0a0u,
        .off_project_cur_sequence   = 0x0e4u,
        .off_project_sel_track      = 0x1b4u,
        .off_project_sel_row        = 0x1b8u,
        .off_project_sel_is_seq     = 0x1bcu,

        .off_clip_event_owner       = 0x3b8u,
        .off_event_owner_tree       = 0x030u,
        .off_event_owner_sentinel   = 0x034u,
        .off_event_owner_root       = 0x038u,
        .off_event_owner_left       = 0x03cu,
        .off_event_owner_right      = 0x040u,
        .off_event_owner_count      = 0x044u,
        .off_rb_node_payload        = 0x010u,

        .off_track_name             = 0x468u,
        .off_track_loop_mode        = 0x508u,
        .off_track_loop_len         = 0x510u,
        .off_track_loop_en          = 0x518u,
        .off_track_program          = 0x648u,
        .off_pluginprog_name        = 0x1c50u,
        .off_sequence_length        = 0x1a8u,

        .off_per_thread_depth       = 0x0f0u,

        .rva_clip_track             = 0x010255bcu,
        .rva_clip_lookup            = 0x01025ef0u,
        .rva_clip_state             = 0x01026e10u,
        .rva_clip_create            = 0x0102c3ecu,
        .rva_clip_extent            = 0x026faac4u,
        .rva_clip_set_length        = 0x01acec54u,
        .rva_track_set_length       = 0x01ad55bcu,
        .rva_shared_release         = 0x00cc5c34u,

        .rva_step_lower_bound       = 0x0270bd64u,
        .rva_step_note_ctor         = 0x02724d84u,
        .rva_step_set_insert        = 0x0117e6dcu,
        .rva_step_set_release       = 0x0206df74u,
        .rva_step_modified_release  = 0x0206dfdcu,
        .rva_step_command_factory   = 0x01ac7a64u,
        .rva_string_default         = 0x00954a18u,
        .rva_string_destroy         = 0x00940bc0u
    },

    /* ----------------------------------------------------------------------
     * Gen 2 MPC OS 3.10.1.12 (/usr/bin/MPC, ELF64 AArch64 PIE)
     * Shared by MPC One G2, XL, Live III, Key 37 G2 (untested on hardware).
     * ---------------------------------------------------------------------- */
    {
        .name       = "gen2-3.10.1.12",
        .fw_version = "3.10.1.12",
        .arch       = CB_ARCH_AARCH64,
        .build_id   = {
            0x8b, 0x28, 0x17, 0x9d, 0x89, 0x27, 0x7a, 0x5f, 0x73, 0xc3,
            0x0b, 0xe9, 0x07, 0x40, 0x76, 0x6e, 0x61, 0x66, 0xb8, 0x37
        },

        .hook_len = 16u,
        .hook_rva = {
            0x01664488u, /* MpcClipLanes::MpcClipLanes entry (x0 = lanes, x1 = root) */
            0x01666ed0u, /* MpcClipLanes::~MpcClipLanes +16  (x0 = lanes) */
            0x030b2360u  /* PerThreadQueues::dispatch entry  (x0 = queues + 8) */
        },
        .hook_expected = {
            /* paciasp; stp x29,x30,[sp,#-96]!; mov x29,sp; stp x19,x20,[sp,#16] */
            { 0x3f, 0x23, 0x03, 0xd5, 0xfd, 0x7b, 0xba, 0xa9,
              0xfd, 0x03, 0x00, 0x91, 0xf3, 0x53, 0x01, 0xa9 },
            /* mov x29,sp; add x9,x1,#0x298; stp x19,x20,[sp,#16]; mov x20,x0 */
            { 0xfd, 0x03, 0x00, 0x91, 0x29, 0x60, 0x0a, 0x91,
              0xf3, 0x53, 0x01, 0xa9, 0xf4, 0x03, 0x00, 0xaa },
            /* paciasp; stp x29,x30,[sp,#-48]!; mov x29,sp; stp x19,x20,[sp,#16] */
            { 0x3f, 0x23, 0x03, 0xd5, 0xfd, 0x7b, 0xbd, 0xa9,
              0xfd, 0x03, 0x00, 0x91, 0xf3, 0x53, 0x01, 0xa9 }
        },

        .rva_lanes_vptr             = 0x07b5d298u,
        .rva_clip_vptr              = 0x07cb4a78u,
        .rva_track_vptr             = 0x07cc2dc8u,
        .rva_sequence_vptr          = 0x07cc1c60u,
        .rva_project_vptr           = 0x07cc08a0u,
        .rva_event_owner_vptr       = 0x07b65e80u,
        .rva_per_thread_queues_vptr = 0x07b95250u,
        .rva_thread_queues_vptr     = 0x07b952b0u,

        .rva_drumprogram_vptr       = 0x07cbade8u,
        .rva_audioprogram_vptr      = 0x07cb91b8u,
        .rva_midiprogram_vptr       = 0x07cbdc60u,
        .rva_pluginprogram_vptr     = 0x07cbe9f0u,
        .rva_cvprogram_vptr         = 0x07cba850u,
        .rva_keygroupprogram_vptr   = 0x07cbbda8u,

        .off_root_project           = 0x4b8u,
        .off_root_lanes             = 0xde8u,

        .off_lanes_columns          = 0x068u,
        .off_lanes_rows             = 0x090u,
        .off_lanes_root             = 0x2e8u,
        .off_lanes_command_factory  = 0x2f0u,
        .off_lanes_command_manager  = 0x2f8u,
        .off_lanes_project          = 0x300u,
        .off_lanes_flag             = 0x368u,
        .off_lanes_track_vec        = 0x370u,
        .off_lanes_clip_pool        = 0x378u,

        .off_factory_queues         = 0x008u,
        .off_factory_project        = 0x028u,
        .off_queues_ui_subobj       = 0x008u,

        .off_project_cur_track      = 0x140u,
        .off_project_cur_sequence   = 0x1c8u,
        .off_project_sel_track      = 0x368u,
        .off_project_sel_row        = 0x370u,
        .off_project_sel_is_seq     = 0x378u,

        .off_clip_event_owner       = 0x708u,
        .off_event_owner_tree       = 0x060u,
        .off_event_owner_sentinel   = 0x068u,
        .off_event_owner_root       = 0x070u,
        .off_event_owner_left       = 0x078u,
        .off_event_owner_right      = 0x080u,
        .off_event_owner_count      = 0x088u,
        .off_rb_node_payload        = 0x020u,

        .off_track_name             = 0x870u,
        .off_track_loop_mode        = 0x9a0u,
        .off_track_loop_len         = 0x9a8u,
        .off_track_loop_en          = 0x9b0u,
        .off_track_program          = 0xc08u,
        .off_pluginprog_name        = 0x2fd8u,
        .off_sequence_length        = 0x348u,

        .off_per_thread_depth       = 0x1c8u,

        .rva_clip_track             = 0x01659de0u,
        .rva_clip_lookup            = 0x0165a6a0u,
        .rva_clip_state             = 0x0165b480u,
        .rva_clip_create            = 0x01660cd0u,
        .rva_clip_extent            = 0x02aecc44u,
        .rva_clip_set_length        = 0x01fd1950u,
        .rva_track_set_length       = 0x01fd7ec0u,
        .rva_shared_release         = 0x0135c6c0u,

        .rva_step_lower_bound       = 0x02afbde0u,
        .rva_step_note_ctor         = 0x02b11660u,
        .rva_step_set_insert        = 0x01790880u,
        .rva_step_set_release       = 0x01b959a8u,
        .rva_step_modified_release  = 0x017270e0u,
        .rva_step_command_factory   = 0x01fcdb40u,
        .rva_string_default         = 0x01057cecu,
        .rva_string_destroy         = 0x01045a80u
    }
};

size_t cb_profile_count(void) {
    return sizeof(CB_PROFILES) / sizeof(CB_PROFILES[0]);
}

const CbProfile *cb_profile_at(size_t index) {
    if (index >= cb_profile_count()) return NULL;
    return &CB_PROFILES[index];
}

const CbProfile *cb_profile_find(CbArch arch, const uint8_t build_id[20]) {
    if (!build_id) return NULL;
    for (size_t i = 0; i < cb_profile_count(); i++) {
        if (CB_PROFILES[i].arch == arch &&
            memcmp(CB_PROFILES[i].build_id, build_id, 20) == 0) {
            return &CB_PROFILES[i];
        }
    }
    return NULL;
}

int cb_profile_verify_prologues(const CbProfile *prof,
                                const uint8_t *image_base,
                                size_t image_size) {
    if (!prof || !image_base) return 0;
    if (prof->hook_len == 0 || prof->hook_len > CB_MAX_HOOK_LEN) return 0;

    for (int i = 0; i < HOOK_COUNT; i++) {
        size_t rva = (size_t)prof->hook_rva[i];
        if (image_size != 0) {
            if (rva > image_size || (size_t)prof->hook_len > image_size - rva) {
                return 0;
            }
        }
        if (memcmp(image_base + rva, prof->hook_expected[i], prof->hook_len) != 0) {
            return 0;
        }
    }
    return 1;
}
