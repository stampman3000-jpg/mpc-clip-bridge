/* SPDX-License-Identifier: MIT
 * mpc-clip-ctl: CLI test client for /run/mpc-ext.sock (v1).
 *
 * Usage:
 *   mpc-clip-ctl hello
 *   mpc-clip-ctl list
 *   mpc-clip-ctl read <col> <row>
 *   mpc-clip-ctl write <col> <row> <note> <tick> <vel> <len> [prob]
 *   mpc-clip-ctl delete <col> <row> <note|-1> <tick_start> <tick_end>
 *   mpc-clip-ctl clear <col> <row>
 *   mpc-clip-ctl demo-2track [row]   # write drums to col 0 + bass to col 1, then read both
 */
#include "proto.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_sock(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect /run/mpc-ext.sock");
        close(fd);
        return -1;
    }
    return fd;
}

static int send_cmd(int fd, const char *payload, int multiline_resp) {
    size_t len = strlen(payload);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, payload + sent, len - sent);
        if (n <= 0) { perror("write"); return 1; }
        sent += (size_t)n;
    }

    char line[512];
    for (;;) {
        size_t i = 0;
        while (i + 1 < sizeof(line)) {
            char c = 0;
            ssize_t r = read(fd, &c, 1);
            if (r <= 0) return (i == 0) ? 1 : 0;
            line[i++] = c;
            if (c == '\n') break;
        }
        line[i] = '\0';
        fputs(line, stdout);
        if (strncmp(line, "ERR ", 4) == 0) return 1;
        if (!multiline_resp) return 0;
        if (strncmp(line, "END", 3) == 0) return 0;
    }
}

static int run_demo_2track(int fd, unsigned row) {
    /* Track 0 (Drums): 16-step beat (kick 1/5/9/13, snare 5/13, hats every step) */
    static const uint8_t kicks[16]  = { 127,0,0,0, 110,0,0,0, 122,0,0,0, 106,0,0,0 };
    static const uint8_t snares[16] = { 0,0,0,0, 116,0,0,0, 0,0,0,0, 127,0,0,0 };
    static const uint8_t hats[16]   = { 104,52,86,48, 100,56,84,62, 104,52,86,48, 100,56,92,70 };

    char buf[4096];
    int pos = snprintf(buf, sizeof(buf), "WRITE_BATCH 0 %u 22\n", row);
    for (int s = 0; s < 16; s++) {
        int tick = s * 240;
        if (kicks[s])  pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "36 %d %u 240 100\n", tick, kicks[s]);
        if (snares[s]) pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "38 %d %u 240 100\n", tick, snares[s]);
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "42 %d %u 240 100\n", tick, hats[s]);
    }
    printf("--- Writing 22-note drum pattern to col=0 row=%u ---\n", row);
    if (send_cmd(fd, buf, 0) != 0) return 1;

    /* Track 1 (Bass/Synth): 8-note C-minor tracker bassline */
    static const struct { int step; int note; int vel; int len; } bass[8] = {
        {  0, 36, 120, 480 }, {  3, 36,  92, 240 },
        {  4, 39, 108, 240 }, {  6, 43, 115, 480 },
        {  8, 36, 122, 480 }, { 11, 41,  98, 240 },
        { 12, 43, 118, 480 }, { 14, 46, 105, 480 }
    };
    pos = snprintf(buf, sizeof(buf), "WRITE_BATCH 1 %u 8\n", row);
    for (int i = 0; i < 8; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%d %d %d %d 100\n",
                        bass[i].note, bass[i].step * 240, bass[i].vel, bass[i].len);
    }
    printf("--- Writing 8-note bassline to col=1 row=%u ---\n", row);
    if (send_cmd(fd, buf, 0) != 0) return 1;

    printf("--- Reading back col=0 row=%u ---\n", row);
    snprintf(buf, sizeof(buf), "READ 0 %u\n", row);
    if (send_cmd(fd, buf, 1) != 0) return 1;

    printf("--- Reading back col=1 row=%u ---\n", row);
    snprintf(buf, sizeof(buf), "READ 1 %u\n", row);
    return send_cmd(fd, buf, 1);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: mpc-clip-ctl <hello|tracks|list|read|write|delete|clear|set-len|demo-2track> [args...]\n");
        return 2;
    }
    const char *sock = getenv("MPC_EXT_SOCK");
    if (!sock || !sock[0]) sock = MPC_EXT_SOCK_PATH;

    int fd = connect_sock(sock);
    if (fd < 0) return 1;

    char cmd[512];
    int rc = 0;
    if (strcmp(argv[1], "hello") == 0) {
        rc = send_cmd(fd, "HELLO\n", 0);
    } else if (strcmp(argv[1], "tracks") == 0) {
        rc = send_cmd(fd, "TRACKS\n", 1);
    } else if (strcmp(argv[1], "list") == 0) {
        rc = send_cmd(fd, "LIST\n", 1);
    } else if (strcmp(argv[1], "read") == 0 && argc == 4) {
        snprintf(cmd, sizeof(cmd), "READ %s %s\n", argv[2], argv[3]);
        rc = send_cmd(fd, cmd, 1);
    } else if (strcmp(argv[1], "write") == 0 && (argc == 8 || argc == 9)) {
        snprintf(cmd, sizeof(cmd), "WRITE %s %s %s %s %s %s %s\n",
                 argv[2], argv[3], argv[4], argv[5], argv[6], argv[7],
                 argc == 9 ? argv[8] : "100");
        rc = send_cmd(fd, cmd, 0);
    } else if (strcmp(argv[1], "delete") == 0 && argc == 7) {
        snprintf(cmd, sizeof(cmd), "DELETE %s %s %s %s %s\n",
                 argv[2], argv[3], argv[4], argv[5], argv[6]);
        rc = send_cmd(fd, cmd, 0);
    } else if (strcmp(argv[1], "clear") == 0 && argc == 4) {
        snprintf(cmd, sizeof(cmd), "CLEAR %s %s\n", argv[2], argv[3]);
        rc = send_cmd(fd, cmd, 0);
    } else if (strcmp(argv[1], "set-len") == 0 && argc == 5) {
        snprintf(cmd, sizeof(cmd), "SET_LEN %s %s %s\n", argv[2], argv[3], argv[4]);
        rc = send_cmd(fd, cmd, 0);
    } else if (strcmp(argv[1], "demo-2track") == 0) {
        unsigned row = (argc >= 3) ? (unsigned)strtoul(argv[2], NULL, 10) : 0u;
        rc = run_demo_2track(fd, row);
    } else {
        fprintf(stderr, "Bad arguments for '%s'\n", argv[1]);
        rc = 2;
    }

    close(fd);
    return rc;
}
