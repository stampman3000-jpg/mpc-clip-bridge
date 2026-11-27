/* SPDX-License-Identifier: MIT
 * Line-protocol parser and formatter for /run/mpc-ext.sock (v1).
 */
#include "proto.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap > 0) snprintf(err, cap, "%s", msg);
}

static uint32_t parse_if_epoch(const char *line) {
    const char *p = strstr(line, "if_epoch=");
    if (!p) return 0;
    unsigned long v = strtoul(p + 9, NULL, 10);
    return (uint32_t)v;
}

int cb_parse_note_line(const char *line, CbNote *out, char *err, size_t err_cap) {
    int note = -1, vel = -1, prob = 100;
    long long tick = -1, len = -1;
    int n = sscanf(line, " %d %lld %d %lld %d", &note, &tick, &vel, &len, &prob);
    if (n < 4) {
        set_err(err, err_cap, "bad note line (want: note tick vel len [prob])");
        return 0;
    }
    if (note < 0 || note > 127 || tick < 0 || vel < 1 || vel > 127 || len < 1 || prob < 0 || prob > 100) {
        set_err(err, err_cap, "note field out of range");
        return 0;
    }
    out->note = (uint8_t)note;
    out->tick = (int64_t)tick;
    out->vel  = (uint8_t)vel;
    out->length = (int64_t)len;
    out->prob = (uint8_t)prob;
    out->pad  = 0;
    return 1;
}

int cb_parse_header(const char *line, CbRequest *req, uint32_t *expect_lines,
                    char *err, size_t err_cap) {
    memset(req, 0, sizeof(*req));
    *expect_lines = 0;

    char verb[32] = {0};
    if (sscanf(line, " %31s", verb) != 1) {
        set_err(err, err_cap, "empty command");
        return 0;
    }

    if (strcmp(verb, "HELLO") == 0) {
        req->kind = CB_CMD_HELLO;
        return 1;
    }
    if (strcmp(verb, "TRACKS") == 0) {
        req->kind = CB_CMD_TRACKS;
        return 1;
    }
    if (strcmp(verb, "LIST") == 0) {
        req->kind = CB_CMD_LIST;
        return 1;
    }
    if (strcmp(verb, "READ") == 0) {
        unsigned col = 0, row = 0;
        if (sscanf(line, " %*s %u %u", &col, &row) != 2 || col >= 128 || row >= 128) {
            set_err(err, err_cap, "usage: READ <col> <row>");
            return 0;
        }
        req->kind = CB_CMD_READ;
        req->col = col;
        req->row = row;
        return 1;
    }
    if (strcmp(verb, "WRITE") == 0) {
        unsigned col = 0, row = 0;
        int note = -1, vel = -1, prob = 100;
        long long tick = -1, len = -1;
        int n = sscanf(line, " %*s %u %u %d %lld %d %lld %d",
                       &col, &row, &note, &tick, &vel, &len, &prob);
        if (n < 6 || col >= 128 || row >= 128 || note < 0 || note > 127 ||
            tick < 0 || vel < 1 || vel > 127 || len < 1 || prob < 0 || prob > 100) {
            set_err(err, err_cap, "usage: WRITE <col> <row> <note> <tick> <vel> <len> [prob]");
            return 0;
        }
        req->kind = CB_CMD_WRITE_BATCH;
        req->col = col;
        req->row = row;
        req->if_epoch = parse_if_epoch(line);
        req->batch_count = 1;
        req->batch[0] = (CbNote){
            .tick = (int64_t)tick, .length = (int64_t)len,
            .note = (uint8_t)note, .vel = (uint8_t)vel, .prob = (uint8_t)prob
        };
        return 1;
    }
    if (strcmp(verb, "WRITE_BATCH") == 0) {
        unsigned col = 0, row = 0, count = 0;
        if (sscanf(line, " %*s %u %u %u", &col, &row, &count) != 3 ||
            col >= 128 || row >= 128 || count == 0 || count > CB_MAX_BATCH_NOTES) {
            set_err(err, err_cap, "usage: WRITE_BATCH <col> <row> <1..512> [if_epoch=<u32>]");
            return 0;
        }
        req->kind = CB_CMD_WRITE_BATCH;
        req->col = col;
        req->row = row;
        req->if_epoch = parse_if_epoch(line);
        req->batch_count = 0;
        *expect_lines = count;
        return 1;
    }
    if (strcmp(verb, "REPLACE") == 0) {
        unsigned col = 0, row = 0, count = 0;
        int note = -1;
        long long t0 = -1, t1 = -1;
        if (sscanf(line, " %*s %u %u %d %lld %lld %u", &col, &row, &note, &t0, &t1, &count) != 6 ||
            col >= 128 || row >= 128 || note < -1 || note > 127 ||
            t0 < 0 || t1 < t0 || count > CB_MAX_BATCH_NOTES) {
            set_err(err, err_cap, "usage: REPLACE <col> <row> <del_note|-1> <t0> <t1> <add_count> [if_epoch=<u32>]");
            return 0;
        }
        req->kind = CB_CMD_REPLACE;
        req->col = col;
        req->row = row;
        req->if_epoch = parse_if_epoch(line);
        req->note_filter = note;
        req->tick_start = (int64_t)t0;
        req->tick_end = (int64_t)t1;
        req->batch_count = 0;
        *expect_lines = count;
        return 1;
    }
    if (strcmp(verb, "DELETE") == 0) {
        unsigned col = 0, row = 0;
        int note = -1;
        long long t0 = -1, t1 = -1;
        if (sscanf(line, " %*s %u %u %d %lld %lld", &col, &row, &note, &t0, &t1) != 5 ||
            col >= 128 || row >= 128 || note < -1 || note > 127 || t0 < 0 || t1 <= t0) {
            set_err(err, err_cap, "usage: DELETE <col> <row> <note|-1> <tick_start> <tick_end> [if_epoch=<u32>]");
            return 0;
        }
        req->kind = CB_CMD_DELETE;
        req->col = col;
        req->row = row;
        req->if_epoch = parse_if_epoch(line);
        req->note_filter = note;
        req->tick_start = (int64_t)t0;
        req->tick_end = (int64_t)t1;
        return 1;
    }
    if (strcmp(verb, "CLEAR") == 0) {
        unsigned col = 0, row = 0;
        if (sscanf(line, " %*s %u %u", &col, &row) != 2 || col >= 128 || row >= 128) {
            set_err(err, err_cap, "usage: CLEAR <col> <row> [if_epoch=<u32>]");
            return 0;
        }
        req->kind = CB_CMD_CLEAR;
        req->col = col;
        req->row = row;
        req->if_epoch = parse_if_epoch(line);
        return 1;
    }
    if (strcmp(verb, "SET_LEN") == 0) {
        unsigned col = 0, row = 0;
        long long len_ticks = -1;
        if (sscanf(line, " %*s %u %u %lld", &col, &row, &len_ticks) != 3 ||
            col >= 128 || row >= 128 || len_ticks < 120 || len_ticks > 960LL * 4LL * 256LL) {
            set_err(err, err_cap, "usage: SET_LEN <col> <row> <len_ticks> [if_epoch=<u32>]");
            return 0;
        }
        req->kind = CB_CMD_SET_LEN;
        req->col = col;
        req->row = row;
        req->tick_end = (int64_t)len_ticks;
        req->if_epoch = parse_if_epoch(line);
        return 1;
    }

    set_err(err, err_cap, "unknown command");
    return 0;
}

static size_t appendf(char *buf, size_t cap, size_t pos, const char *fmt, ...) {
    if (pos >= cap) return pos;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n <= 0) return pos;
    if ((size_t)n >= cap - pos) return cap - 1;
    return pos + (size_t)n;
}

size_t cb_format_response(const CbRequest *req, const CbResponse *resp,
                          const char *fw_ver, char *buf, size_t cap) {
    if (!resp->ok) {
        return appendf(buf, cap, 0, "ERR %s\n", resp->err[0] ? resp->err : "failed");
    }
    size_t pos = 0;
    switch (req->kind) {
        case CB_CMD_HELLO:
            return appendf(buf, cap, 0,
                "OK proto=%u fw=%s caps=native_clips_rw,tracks,epoch,set_len epoch=%u rev=%u ready=%d cols=%u rows=%u sel_col=%d sel_row=%d seq_mode=%d\n",
                MPC_EXT_PROTO_VER, fw_ver ? fw_ver : "unknown",
                resp->epoch, resp->rev,
                resp->ready, resp->cols, resp->rows,
                resp->sel_col, resp->sel_row, resp->seq_mode);
        case CB_CMD_TRACKS:
            for (uint32_t i = 0; i < resp->track_count; i++) {
                const CbTrackInfo *t = &resp->tracks[i];
                pos = appendf(buf, cap, pos,
                    "TRACK col=%u type=%s name=\"%s\" plugin=\"%s\"\n",
                    t->col, t->type, t->name, t->plugin);
            }
            return appendf(buf, cap, pos, "END count=%u\n", resp->track_count);
        case CB_CMD_LIST:
            for (uint32_t i = 0; i < resp->clip_count; i++) {
                const CbClipInfo *c = &resp->clips[i];
                pos = appendf(buf, cap, pos,
                    "CLIP col=%u row=%u state=%u notes=%u len=%" PRId64 " sel=%u\n",
                    c->col, c->row, c->state, c->note_count, c->extent, (unsigned)c->selected);
            }
            return appendf(buf, cap, pos, "END count=%u\n", resp->clip_count);
        case CB_CMD_READ:
            for (uint32_t i = 0; i < resp->note_count; i++) {
                const CbNote *n = &resp->notes[i];
                pos = appendf(buf, cap, pos,
                    "NOTE tick=%" PRId64 " note=%u vel=%u len=%" PRId64 " prob=%u\n",
                    n->tick, (unsigned)n->note, (unsigned)n->vel, n->length, (unsigned)n->prob);
            }
            return appendf(buf, cap, pos, "END count=%u\n", resp->note_count);
        case CB_CMD_WRITE_BATCH:
            return appendf(buf, cap, 0, "OK written=%u\n", resp->affected);
        case CB_CMD_REPLACE:
            return appendf(buf, cap, 0, "OK removed=%u added=%u\n", resp->removed, resp->affected);
        case CB_CMD_DELETE:
        case CB_CMD_CLEAR:
            return appendf(buf, cap, 0, "OK deleted=%u\n", resp->affected);
        case CB_CMD_SET_LEN:
            return appendf(buf, cap, 0, "OK len=%" PRId64 "\n", resp->length_ticks);
        default:
            return appendf(buf, cap, 0, "ERR unknown\n");
    }
}
