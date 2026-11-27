/* SPDX-License-Identifier: MIT
 * Minimal /run/mpc-ext.sock line protocol (v2) for native multi-track clip R/W.
 */
#ifndef MPC_CLIP_BRIDGE_PROTO_H
#define MPC_CLIP_BRIDGE_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define MPC_EXT_SOCK_PATH "/run/mpc-ext.sock"
#define MPC_EXT_PROTO_VER 2u

#define CB_MAX_BATCH_NOTES 512u
#define CB_MAX_READ_NOTES  2048u
#define CB_MAX_LIST_CLIPS  512u
#define CB_MAX_TRACKS      128u

typedef enum {
    CB_CMD_NONE = 0,
    CB_CMD_HELLO,
    CB_CMD_TRACKS,
    CB_CMD_LIST,
    CB_CMD_READ,
    CB_CMD_WRITE_BATCH,
    CB_CMD_REPLACE,
    CB_CMD_DELETE,
    CB_CMD_CLEAR,
    CB_CMD_SET_LEN
} CbCmdKind;

typedef struct {
    uint32_t col;
    char     type[16];
    char     name[64];
    char     plugin[64];
} CbTrackInfo;

typedef struct {
    int64_t  tick;
    int64_t  length;
    uint8_t  note;
    uint8_t  vel;   /* 1..127 */
    uint8_t  prob;  /* 0..100 */
    uint8_t  pad;
} CbNote;

typedef struct {
    uint32_t col;
    uint32_t row;
    uint32_t state;
    uint32_t note_count;
    int64_t  extent;
    uint8_t  selected;
} CbClipInfo;

typedef struct {
    CbCmdKind kind;
    uint32_t  col;
    uint32_t  row;
    uint32_t  if_epoch;    /* 0 = unchecked, otherwise must match live project epoch */
    int32_t   note_filter; /* -1 = any note, else 0..127 */
    int64_t   tick_start;
    int64_t   tick_end;
    uint32_t  batch_count;
    CbNote    batch[CB_MAX_BATCH_NOTES];
} CbRequest;

typedef struct {
    int        ok;
    char       err[128];
    /* HELLO */
    int        ready;
    uint32_t   epoch;
    uint32_t   rev;
    uint32_t   cols;
    uint32_t   rows;
    int32_t    sel_col;
    int32_t    sel_row;
    int        seq_mode;
    /* TRACKS */
    uint32_t    track_count;
    CbTrackInfo tracks[CB_MAX_TRACKS];
    /* LIST */
    uint32_t   clip_count;
    CbClipInfo clips[CB_MAX_LIST_CLIPS];
    /* READ */
    uint32_t   note_count;
    CbNote     notes[CB_MAX_READ_NOTES];
    /* WRITE / REPLACE / DELETE / CLEAR / SET_LEN */
    uint32_t   affected;
    uint32_t   removed;
    int64_t    length_ticks;
} CbResponse;

/* Parse a command header line. For WRITE_BATCH, returns expected note lines
 * in *expect_lines (> 0); caller then feeds each line to cb_parse_note_line().
 * Returns 1 on valid header, 0 on syntax error (writes err).
 */
int cb_parse_header(const char *line, CbRequest *req, uint32_t *expect_lines,
                    char *err, size_t err_cap);

/* Parse one "<note> <tick> <vel> <len> [prob]" line into *out. */
int cb_parse_note_line(const char *line, CbNote *out, char *err, size_t err_cap);

/* Format a CbResponse into a text buffer. Returns bytes written (excl. NUL). */
size_t cb_format_response(const CbRequest *req, const CbResponse *resp,
                          const char *fw_ver, char *buf, size_t cap);

#endif
