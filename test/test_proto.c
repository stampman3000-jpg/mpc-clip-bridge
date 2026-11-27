/* SPDX-License-Identifier: MIT
 * Host unit tests for /run/mpc-ext.sock protocol parser and formatter.
 */
#include "../src/proto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_parse_commands(void) {
    CbRequest req;
    uint32_t expect = 99;
    char err[128] = {0};

    assert(cb_parse_header("HELLO", &req, &expect, err, sizeof(err)) == 1);
    assert(req.kind == CB_CMD_HELLO && expect == 0);

    assert(cb_parse_header("TRACKS", &req, &expect, err, sizeof(err)) == 1);
    assert(req.kind == CB_CMD_TRACKS && expect == 0);

    assert(cb_parse_header("LIST", &req, &expect, err, sizeof(err)) == 1);
    assert(req.kind == CB_CMD_LIST && expect == 0);

    assert(cb_parse_header("READ 2 1", &req, &expect, err, sizeof(err)) == 1);
    assert(req.kind == CB_CMD_READ && req.col == 2 && req.row == 1);

    assert(cb_parse_header("WRITE 1 0 36 480 110 240 100 if_epoch=7", &req, &expect, err, sizeof(err)) == 1);
    assert(req.kind == CB_CMD_WRITE_BATCH && req.col == 1 && req.row == 0 && req.if_epoch == 7 && req.batch_count == 1);
    assert(req.batch[0].note == 36 && req.batch[0].tick == 480 && req.batch[0].vel == 110 &&
           req.batch[0].length == 240 && req.batch[0].prob == 100);

    assert(cb_parse_header("WRITE_BATCH 0 1 22", &req, &expect, err, sizeof(err)) == 1);
    assert(req.kind == CB_CMD_WRITE_BATCH && req.col == 0 && req.row == 1 && expect == 22);

    CbNote note;
    assert(cb_parse_note_line("42 960 88 240 75", &note, err, sizeof(err)) == 1);
    assert(note.note == 42 && note.tick == 960 && note.vel == 88 && note.length == 240 && note.prob == 75);

    assert(cb_parse_header("REPLACE 1 0 36 960 961 1 if_epoch=2", &req, &expect, err, sizeof(err)) == 1);
    assert(req.kind == CB_CMD_REPLACE && req.col == 1 && req.row == 0 && req.note_filter == 36 &&
           req.tick_start == 960 && req.tick_end == 961 && req.if_epoch == 2 && expect == 1);

    assert(cb_parse_header("DELETE 1 0 -1 0 3840", &req, &expect, err, sizeof(err)) == 1);
    assert(req.kind == CB_CMD_DELETE && req.note_filter == -1 && req.tick_start == 0 && req.tick_end == 3840);

    assert(cb_parse_header("CLEAR 0 0", &req, &expect, err, sizeof(err)) == 1);
    assert(req.kind == CB_CMD_CLEAR && req.col == 0 && req.row == 0);

    assert(cb_parse_header("SET_LEN 2 0 1920 if_epoch=5", &req, &expect, err, sizeof(err)) == 1);
    assert(req.kind == CB_CMD_SET_LEN && req.col == 2 && req.row == 0 &&
           req.tick_end == 1920 && req.if_epoch == 5 && expect == 0);
    assert(cb_parse_header("SET_LEN 2 0 60", &req, &expect, err, sizeof(err)) == 0);

    /* Reject invalid inputs */
    assert(cb_parse_header("READ 999 0", &req, &expect, err, sizeof(err)) == 0);
    assert(cb_parse_header("WRITE 0 0 128 0 100 240", &req, &expect, err, sizeof(err)) == 0);
    assert(cb_parse_header("WRITE_BATCH 0 0 0", &req, &expect, err, sizeof(err)) == 0);
    assert(cb_parse_header("DELETE 0 0 36 480 480", &req, &expect, err, sizeof(err)) == 0);
    assert(cb_parse_header("NOPE", &req, &expect, err, sizeof(err)) == 0);
}

static void test_format_responses(void) {
    char buf[1024];
    CbRequest req = { .kind = CB_CMD_HELLO };
    CbResponse resp = { .ok = 1, .ready = 1, .epoch = 3, .rev = 12, .cols = 8, .rows = 4, .sel_col = 0, .sel_row = 1, .seq_mode = 0 };
    size_t n = cb_format_response(&req, &resp, "3.10.1.12", buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "OK proto=2 fw=3.10.1.12 caps=native_clips_rw,tracks,epoch,set_len epoch=3 rev=12 ready=1 cols=8 rows=4 sel_col=0 sel_row=1 seq_mode=0\n") != NULL);

    req.kind = CB_CMD_SET_LEN;
    memset(&resp, 0, sizeof(resp));
    resp.ok = 1;
    resp.length_ticks = 3840;
    n = cb_format_response(&req, &resp, "3.10.1.12", buf, sizeof(buf));
    assert(strcmp(buf, "OK len=3840\n") == 0);

    req.kind = CB_CMD_TRACKS;
    memset(&resp, 0, sizeof(resp));
    resp.ok = 1;
    resp.track_count = 1;
    resp.tracks[0].col = 0;
    snprintf(resp.tracks[0].type, sizeof(resp.tracks[0].type), "plugin");
    snprintf(resp.tracks[0].name, sizeof(resp.tracks[0].name), "Plugin 001");
    snprintf(resp.tracks[0].plugin, sizeof(resp.tracks[0].plugin), "TrackBoy");
    n = cb_format_response(&req, &resp, "3.10.1.12", buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "TRACK col=0 type=plugin name=\"Plugin 001\" plugin=\"TrackBoy\"\n") != NULL);
    assert(strstr(buf, "END count=1\n") != NULL);

    req.kind = CB_CMD_READ;
    memset(&resp, 0, sizeof(resp));
    resp.ok = 1;
    resp.note_count = 2;
    resp.notes[0] = (CbNote){ .tick = 0, .length = 240, .note = 36, .vel = 127, .prob = 100 };
    resp.notes[1] = (CbNote){ .tick = 240, .length = 240, .note = 42, .vel = 64, .prob = 90 };
    n = cb_format_response(&req, &resp, "3.10.1.12", buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "NOTE tick=0 note=36 vel=127 len=240 prob=100\n") != NULL);
    assert(strstr(buf, "NOTE tick=240 note=42 vel=64 len=240 prob=90\n") != NULL);
    assert(strstr(buf, "END count=2\n") != NULL);

    resp.ok = 0;
    snprintf(resp.err, sizeof(resp.err), "no_clip_at_slot");
    cb_format_response(&req, &resp, "3.10.1.12", buf, sizeof(buf));
    assert(strcmp(buf, "ERR no_clip_at_slot\n") == 0);
}

int main(void) {
    test_parse_commands();
    test_format_responses();
    printf("test_proto: ALL PASSED\n");
    return 0;
}
