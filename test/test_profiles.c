/* SPDX-License-Identifier: MIT
 * Host unit tests for per-build profile lookup and hook prologue verification.
 */
#include "../src/profiles.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t GEN1_BUILD_ID[20] = {
    0x85, 0x1d, 0x05, 0x0e, 0xb0, 0x3c, 0xe0, 0xb8, 0x66, 0x3a,
    0xaa, 0x57, 0x2b, 0x25, 0x99, 0x81, 0x3c, 0xbf, 0x53, 0xd4
};

static const uint8_t GEN2_BUILD_ID[20] = {
    0x8b, 0x28, 0x17, 0x9d, 0x89, 0x27, 0x7a, 0x5f, 0x73, 0xc3,
    0x0b, 0xe9, 0x07, 0x40, 0x76, 0x6e, 0x61, 0x66, 0xb8, 0x37
};

static const uint8_t UNKNOWN_BUILD_ID[20] = {
    0xde, 0xad, 0xbe, 0xef, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};

static void test_profile_lookup(void) {
    assert(cb_profile_count() >= 2);
    assert(cb_profile_at(cb_profile_count()) == NULL);

    /* Gen 1 exact match */
    const CbProfile *g1 = cb_profile_find(CB_ARCH_ARM32, GEN1_BUILD_ID);
    assert(g1 != NULL);
    assert(strcmp(g1->name, "gen1-3.10.1.12") == 0);
    assert(strcmp(g1->fw_version, "3.10.1.12") == 0);
    assert(g1->arch == CB_ARCH_ARM32);
    assert(g1->hook_len == 8u);
    assert(g1->hook_rva[HOOK_LANES_CTOR] == 0x01030d20u);
    assert(g1->hook_rva[HOOK_LANES_DTOR] == 0x01032f70u);
    assert(g1->hook_rva[HOOK_UI_DRAIN]   == 0x02d6d0c4u);

    /* Gen 2 exact match */
    const CbProfile *g2 = cb_profile_find(CB_ARCH_AARCH64, GEN2_BUILD_ID);
    assert(g2 != NULL);
    assert(strcmp(g2->name, "gen2-3.10.1.12") == 0);
    assert(strcmp(g2->fw_version, "3.10.1.12") == 0);
    assert(g2->arch == CB_ARCH_AARCH64);
    assert(g2->hook_len == 16u);
    assert(g2->hook_rva[HOOK_LANES_CTOR] == 0x01664488u);
    assert(g2->hook_rva[HOOK_LANES_DTOR] == 0x01666ed0u);
    assert(g2->hook_rva[HOOK_UI_DRAIN]   == 0x030b2360u);

    /* Architecture mismatch must fail even with a known Build ID */
    assert(cb_profile_find(CB_ARCH_AARCH64, GEN1_BUILD_ID) == NULL);
    assert(cb_profile_find(CB_ARCH_ARM32, GEN2_BUILD_ID) == NULL);

    /* Unknown Build ID (e.g. unported 3.10.1.49) must fail */
    assert(cb_profile_find(CB_ARCH_ARM32, UNKNOWN_BUILD_ID) == NULL);
    assert(cb_profile_find(CB_ARCH_AARCH64, UNKNOWN_BUILD_ID) == NULL);

    /* Single-bit Build ID flip must fail */
    uint8_t flipped[20];
    memcpy(flipped, GEN2_BUILD_ID, 20);
    flipped[19] ^= 0x01u;
    assert(cb_profile_find(CB_ARCH_AARCH64, flipped) == NULL);
    assert(cb_profile_find(CB_ARCH_ARM32, NULL) == NULL);
}

static void test_prologue_verification_for_profile(const CbProfile *p) {
    assert(p != NULL);
    /* Build a synthetic image big enough to hold the highest hook RVA */
    size_t max_end = 0;
    for (int i = 0; i < HOOK_COUNT; i++) {
        size_t end = (size_t)p->hook_rva[i] + p->hook_len;
        if (end > max_end) max_end = end;
    }

    uint8_t *img = calloc(1, max_end + 64);
    assert(img != NULL);

    /* Zeroed image must be refused */
    assert(cb_profile_verify_prologues(p, img, max_end + 64) == 0);
    assert(cb_profile_verify_prologues(p, img, 0) == 0);

    /* Populate all 3 expected prologues -> must pass */
    for (int i = 0; i < HOOK_COUNT; i++) {
        memcpy(img + p->hook_rva[i], p->hook_expected[i], p->hook_len);
    }
    assert(cb_profile_verify_prologues(p, img, max_end + 64) == 1);
    assert(cb_profile_verify_prologues(p, img, 0) == 1);

    /* Truncated image (1 byte short of highest hook end) must be refused */
    assert(cb_profile_verify_prologues(p, img, max_end - 1) == 0);

    /* Corrupting any single byte in any of the 3 prologues must be refused */
    for (int i = 0; i < HOOK_COUNT; i++) {
        size_t first = p->hook_rva[i];
        size_t last  = (size_t)p->hook_rva[i] + p->hook_len - 1;

        img[first] ^= 0xffu;
        assert(cb_profile_verify_prologues(p, img, max_end + 64) == 0);
        img[first] ^= 0xffu;
        assert(cb_profile_verify_prologues(p, img, max_end + 64) == 1);

        img[last] ^= 0x01u;
        assert(cb_profile_verify_prologues(p, img, max_end + 64) == 0);
        img[last] ^= 0x01u;
        assert(cb_profile_verify_prologues(p, img, max_end + 64) == 1);
    }

    /* Null checks */
    assert(cb_profile_verify_prologues(NULL, img, max_end) == 0);
    assert(cb_profile_verify_prologues(p, NULL, max_end) == 0);

    free(img);
}

static void test_all_profiles_sanity(void) {
    for (size_t i = 0; i < cb_profile_count(); i++) {
        const CbProfile *p = cb_profile_at(i);
        assert(p != NULL);
        assert(p->name && p->name[0] != '\0');
        assert(p->fw_version && p->fw_version[0] != '\0');
        assert((p->arch == CB_ARCH_ARM32 && p->hook_len == 8u) ||
               (p->arch == CB_ARCH_AARCH64 && p->hook_len == 16u));
        for (int h = 0; h < HOOK_COUNT; h++) {
            assert(p->hook_rva[h] != 0);
            assert((p->hook_rva[h] & 3u) == 0);
        }
        /* Non-zero critical vtables & native functions */
        assert(p->rva_lanes_vptr != 0 && p->rva_clip_vptr != 0 && p->rva_project_vptr != 0);
        assert(p->rva_clip_lookup != 0 && p->rva_step_command_factory != 0);
        assert(p->off_queues_ui_subobj == (p->arch == CB_ARCH_ARM32 ? 4u : 8u));
        assert(p->off_rb_node_payload == (p->arch == CB_ARCH_ARM32 ? 0x10u : 0x20u));

        test_prologue_verification_for_profile(p);
    }
}

int main(void) {
    test_profile_lookup();
    test_all_profiles_sanity();
    printf("test_profiles: ALL PASSED (%zu profiles verified)\n", cb_profile_count());
    return 0;
}
