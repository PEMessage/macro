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
#include <readline/readline.h>
#include <readline/history.h>

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
    int fd_stdin = dup(STDIN_FILENO);
    char *input;

    if (fd_in > 0) {
        printf("macro: reading commands from \"macro.ini\"\n\n");
        
    } else {
        fd_in = 0 ;
    }
    

    while (1) {

        if ( fd_in > 0 ) {
            dup2(fd_in, STDIN_FILENO);
        }

        // Use readline to get user input
        input = readline("> "); // Prompt for input

        if ( input == NULL  && fd_in > 0) { // Handle EOF (Ctrl+D)
            dup2(fd_stdin, STDIN_FILENO);
            close(fd_in);
            fd_in = 0;
            continue ;
        } else if (input == NULL ) { // Handle EOF (Ctrl+D)
            printf("macro: EOF, exiting...\n");
            break;
        }
        // printf("len: %d\n", strlen(input));
        // printf("Input :'%s'\n", input);


        // Check if the program is still running
        if (pid == 0) {
            printf("macro: %s is no longer running, exiting...\n", program);
            free(input); // Free the input buffer
            break;
        }

        size_t raw_input_len = strlen(input);

        if ( raw_input_len == 0 ) {
            HIST_ENTRY **hist_list = history_list();
            int history_count = history_length;

            if (history_count > 0) {
                // if input is just <enter> free here
                // Get the latest entry
                HIST_ENTRY *latest_entry = hist_list[history_count - 1];
                free(input);
                input = strdup(latest_entry->line);
            } else {
                // if input is just <enter> free here
                free(input);
                continue;
            }
        }


        // If input is not empty, add it to history
        if (*input && raw_input_len != 0 && fd_in == 0 ) {
            add_history(input);
        }

        // Process the input
        if (!strcmp("exit", input)) {
            free(input); // Free the input buffer
            break; // Exit the loop
        }

        // Handle commands and aliases
        char *cmd = strtok(input, " ");
        char *rest = strtok(NULL, "");

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
                free(input); // Free the input buffer
                break;
            }
        } else if (!strcmp("unalias", cmd)) {
            if (verbose) {
                printf("macro: calling alias_del rest=\"%s\"\n", rest);
            }
            alias_del(rest);
        } else {
            // Prepare the output buffer
            snprintf(outbuf, sizeof(outbuf), "%s %s\n", cmd, rest);
            int len = strlen(outbuf);
            if (verbose) {
                printf("macro: sending (%d bytes): %s", len, outbuf);
            }

            int sent = write(fd_out, outbuf, len);
            if (sent != len) {
                fprintf(stderr, "macro: incomplete send, exiting...\n");
                free(input); // Free the input buffer
                break;
            }
        }

        free(input); // Free the input buffer after processing
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
