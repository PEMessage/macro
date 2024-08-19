/*
 * Copyright (c) 2022, Geoff Mottram
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <pwd.h>

//
// Function declarations
//
static int alias_add(char *space);
static void alias_del(char *space);
static char *alias_find(char *cmd);
static int ini_file(void);
static int init(int argc, char **argv);
static void proc_exit(int);
static int send(int fd, char *buf, int len);
static void usage(void);

//
// Constants
//
#define BUF_SIZE  1024
#define MAX_ALIASES  1024

//
// Static data
//
static const char *version1 = "macro v1 (04/22/22 - Earth Day)";
static const char *version2 = "Copyright (c) 2022 Geoff Mottram";
static char inbuf[BUF_SIZE + 32];
static char outbuf[BUF_SIZE + 32];
static char *program;                   // program being run
static int verbose;                     // verbose mode
static int repeat_off;                  // turn off ENTER key repeats last command
static pid_t pid;                       // process ID of child

struct alias {
    char *name;
    char *expand;
};

static struct alias aliases[MAX_ALIASES];       // array of aliases
static int acount;                      // number of aliases

//
// Functions
//
// List aliases or define a new alias
static int alias_add(char *rest) {
    if (*rest == 0) {
        if (acount) {
            struct alias *a = aliases;
            printf("macro aliases:\n");
            for (int i = 0; i < acount; i++, a++) {
                printf("\t%s=%s\n", a->name, a->expand);
            }
        } else {
            printf("macro: no aliases defined\n");
        }
    } else if (acount < MAX_ALIASES) {
        char *eq = strchr(rest, '=');
        if (eq) {
            char *end = eq;                     // last character, exclusive
            while ((end != rest) && (end[-1] == ' ')) {
                end--;
            }
            if (end > rest) {
                *end = 0;                       // null terminate alias name
                int nlen = end - rest;          // length of alias name
                eq++;                           // skip =
                while (*eq == ' ') {
                    eq++;
                }
                if (*eq) {
                    int rlen = strlen(eq);
                    int blen = nlen + rlen + 2; // need two strings
                    char *buf = malloc(blen);
                    if (buf) {
                        if (verbose) {
                            printf("macro: add alias \"%s\" to mean \"%s\"\n", rest, eq);
                        }
                        struct alias *a = aliases + acount;
                        acount++;
                        a->name = buf;
                        memcpy(buf, rest, nlen + 1);        // copy null
                        a->expand = memcpy(buf + nlen + 1, eq, rlen + 1);       // copy null
                    } else {
                        printf("macro: out of memory defining alias\n");
                        return -1;
                    }
                } else {
                    printf("macro: no string found after = in alias definition\nusage: alias NAME=EXPAND\n");
                }
                char *start = eq + 1;
            } else {
                printf("macro: no string found before = in alias definition\nusage: alias NAME=EXPAND\n");
            }
        } else {
            printf("macro: no = found in alias definition\nusage: alias NAME=EXPAND\n");
        }
    } else {
        printf("macro: alias table is full (max number=%d)\n", MAX_ALIASES);
    }
    return 0;
}

static void alias_del(char *rest) {
    if (acount) {
        if (*rest) {
            char *space = strchr(rest, ' ');
            if (space) {
                *space = 0;                             // null terminate
            }
        }
        if (*rest) {
            int ocount = acount;
            struct alias *a = aliases;
            for (int i = 0; i < acount; i++, a++) {
                if (!strcmp(a->name, rest)) {
                    printf("macro: removing alias \"%s\"\n", rest);
                    free(a->name);
                    int after = acount - i - 1; 
                    if (after) {
                        memcpy(&aliases[i], &aliases[i+1], after * sizeof(struct alias));
                        a = &aliases[acount - 1];       // old last entry
                    }
                    a->name = a->expand = NULL;         // clear old last entry
                    acount--;
                    break;
                }
            }
            if (acount == ocount) {
                printf("macro: no such alias \"%s\"\n", rest);
            }
        } else {
            printf("macro: usage: unalias NAME\n");
        }
    } else {
        printf("macro: alias table is empty\n");
    }
}

static char *alias_find(char *cmd) {
    struct alias *a = aliases;
    for (int i = 0; i < acount; i++, a++) {
        if (!strcmp(a->name, cmd)) {
            return a->expand;
        }
    }
    return NULL;
}

// Check for an ini file and open it, if so.
static int ini_file() {
    char *home_path = getpwuid(getuid())->pw_dir;

    #define INI_PATH_MAXLENGTH 1024
    #define INI_PATH_NR 3

    char ini_path[INI_PATH_NR][INI_PATH_MAXLENGTH];
    snprintf(ini_path[0], sizeof(ini_path[0]), "./macro.ini");
    snprintf(ini_path[1], sizeof(ini_path[1]), "%s/.macro.ini", home_path);
    snprintf(ini_path[2], sizeof(ini_path[2]), "%s/.config/macro/macro.ini", home_path);

    int fd = 0;

    for (size_t i = 0; i < sizeof(ini_path) / sizeof(ini_path[0]); i++) {
        fd = open(ini_path[i], O_RDONLY);
        if (fd < 0) {
            if (errno == ENOENT) {
                continue ;
            } else {
                // char *err = strerror(errno);                                         
                // fprintf(stderr, "macro: problem reading \"macro.ini\": %s\n", err);  
                // exit(1);
                printf("Not using ini: '%s'", ini_path[i]);
            }
        } else {
            printf("using ini: '%s'", ini_path[i]);
            return fd;
        }
    }
    fprintf(stderr, "macro: problem reading ini");  
    exit(1);
}

// Parse command line arguments
static int init(int argc, char **argv) {
    int     fd[2];

    printf("\n%s\n%s\n\n", version1, version2);
    int prog = 1;
    while (prog < argc) {
        char *arg = argv[prog];
        if (*arg != '-') {
            break;
        } else {
            arg++;
            prog++;
            while (1) {
                char ch = *arg++;
                if (ch == 0) {
                    break;
                } else if (ch == 'r') {
                    repeat_off = 1;
                } else if (ch == 'v') {
                    verbose = 1;
                } else {
                    fprintf(stderr, "macro: unkown option: %c\n\n", ch);
                    usage();
                }
            }
        }
    }
    if (argc == prog) {
        usage();
    }
    program = argv[prog];
    if (verbose) {
        printf("macro: program=\"%s\"\n", program);
    }
    signal(SIGCHLD, proc_exit);
    siginterrupt(SIGCHLD, 1);       // allow signal to interrupt read(), below
    pipe(fd);
    pid = fork();
    if (pid == -1) {
        char *err = strerror(errno);
        fprintf(stderr, "macro: cannot fork: %s\n", err);
        exit(1);
    }
    if (pid == 0) {                 // child
        dup2(fd[0], 0);             // standard input comes from pipe
        close(fd[0]);               // close duplicated file descriptor
        close(fd[1]);               // child closes output side of pipe
        if (execvp(program, &argv[prog])) {
            char *err = strerror(errno);
            fprintf(stderr, "macro: cannot exec: %s\n", program, err);
        } else {
            // should be unreachable
            fprintf(stderr, "macro: child(%s) running after execvp() call\n", program);
        }
        exit(1);
    } else {
        close(fd[0]);               // parent closes input side of pipe
    }
    return fd[1];                   // parent uses output side of pipe
}

int main(int argc, char **argv) {
    int fd_in = ini_file();
    int fd_out = init(argc, argv);
    int max = BUF_SIZE;             // leave room for a null
    char *in = inbuf;
    outbuf[0] = 0;                  // for repeat command
    int flush = 0;
    if (fd_in) {
        printf("macro: reading commands from \"macro.ini\"\n\n");
    }
    while (1) {
        int count = read(fd_in, in, 1);
        if (pid == 0) {
            printf("macro: %s is no longer running, exiting...\n", program);
            break;
        }
        if (count == -1) {
            if (errno == EINTR) {
                if (verbose) {
                    fprintf(stderr, "macro: EINTR\n");
                }
            } else {
                char *err = strerror(errno);
                fprintf(stderr, "macro: error reading from stdin: %s\n", err);
                break;
            }
        } else if (count == 0) {
            if (fd_in != 0) {                   // reading from ini file?
                close(fd_in);                   // close ini file
                fd_in = 0;                      // switch to stdin
                outbuf[0] = 0;                  // reset repeat mechanism
            } else {
                printf("macro: EOF, exiting...\n");
                break;
            }
        } else if (*in == '\n') {
            if (!flush) {
                *in = 0;                        // null terminate
                //
                // If the input line is blank and there was a last line
                // in outbuf and the repeat option is not turned off,
                // then resend the last command from outbuf.
                //
                if (!*inbuf) {
                    int len;
                    if (!*outbuf || repeat_off) {
                        outbuf[0] = '\n';
                        outbuf[1] = 0;
                        len = 1;
                        if (verbose) {
                            printf("macro: sending newline (1 byte)");
                        }
                    } else {
                        len = strlen(outbuf);
                        if (verbose) {
                            printf("macro: resending last command (%d bytes): %s", len, outbuf);
                        }
                    }
                    int sent = write(fd_out, outbuf, len);
                    if (sent != len) {
                        fprintf(stderr, "macro: incomplete send, exiting...\n");
                        break;
                    }
                } else {
                    // if processing the "macro.ini" file and the
                    // line starts with a "#", ignore the line.
                    if (fd_in && (*inbuf == '#')) {
                        if (verbose) {
                            printf("macro: skipping \"macro.ini\" comment: \"%s\"\n", inbuf);
                        }
                        max = BUF_SIZE;
                        in = inbuf;
                        continue;
                    }
                    int rlen = 0;                   // length of remainder of line
                    char *rest = strchr(inbuf, ' ');
                    if (rest) {
                        *rest++ = 0;                // null terminate command
                        while (*rest == ' ') {
                            rest++;
                        }
                        rlen = in - rest;
                    } else {
                        rest = in;                  // points to null character
                        rlen = 0;
                    }
                    char *cmd = inbuf;
                    if (verbose) {
                        printf("macro: cmd=\"%s\"\n", cmd);
                    }
                    char *expand = alias_find(cmd);
                    if (expand) {
                        if (verbose) {
                            printf("macro: alias_find expanded cmd=\"%s\" into \"%s\"\n", cmd, expand);
                        }
                        cmd = expand;
                    }
                    if (!strcmp("alias", cmd)) {
                        if (verbose) {
                            printf("macro: calling alias_add rest=\"%s\"\n", rest);
                        }
                        if (alias_add(rest)) {
                            break;
                        }
                    } else if (!strcmp("unalias", cmd)) {
                        if (verbose) {
                            printf("macro: calling alias_del rest=\"%s\"\n", rest);
                        }
                        alias_del(rest);
                    } else {
                        int clen = strlen(cmd);
                        if ((clen + rlen + 3) > BUF_SIZE) {        // 3=space plus newline plus null
                            printf("macro: line too long, skipping...\n");
                            max = BUF_SIZE;
                            in = inbuf;
                            continue;
                        }
                        memcpy(outbuf, cmd, clen);
                        char *out = outbuf + clen;
                        if (rlen) {
                            *out++ = ' ';
                            memcpy(out, rest, rlen);
                            out += rlen;
                        }
                        *out++ = '\n';
                        *out = 0;
                        int len =  out - outbuf;
                        if (verbose) {
                            printf("macro: sending (%d bytes): %s", len, outbuf);
                        }
                        int sent = write(fd_out, outbuf, len);
                        if (sent != len) {
                            fprintf(stderr, "macro: incomplete send, exiting...\n");
                            break;
                        }
                    }
                }
                // reset
                max = BUF_SIZE;
                in = inbuf;
            } else {
                flush = 0;
            }
        } else if (!flush) {
            if (*in < ' ') {                // control codes to spaces
                *in = ' ';
            }
            if ((*in != ' ') || (in != inbuf)) {    // ignore leading spaces
                if (--max) {
                    in++;
                } else {
                    fprintf(stderr, "macro: line too long, resetting input\n");
                    flush = 1;
                    max = BUF_SIZE;
                    in = inbuf;
                }
            }
        }
    }
    return 0;
}


// Called when child process exits
static void proc_exit(int sig) {
    pid = 0;
}

static void usage(void) {
    printf("Usage: macro [-rv] PROGRAM ARGS...\n");
    printf("Where: -r Turn off ENTER key repeats last command\n");
    printf("       -v Verbose output (debugging aid)\n\n");
    printf("A macro processing front-end to PROGRAM\n");
    printf("Reads initialization script from \"macro.ini\"\n\n");
    exit(1);
}
