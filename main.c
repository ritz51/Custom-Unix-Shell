#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/time.h>
#include <termios.h>
#include <readline/readline.h>
#include <readline/history.h>

#define MAX_INPUT 4096
#define MAX_ARGS 256
#define MAX_PIPES 64
#define MAX_HISTORY 100
#define MAX_BOOKMARKS 50

typedef struct {
    char **argv;
    char *input_file;
    char *output_file;
    int append;
    int background;
} Command;

typedef struct {
    char name[100];
    char path[1024];
} Bookmark;

/* Globals */
static char *shell_history[MAX_HISTORY];
static int history_count = 0;

Bookmark bookmarks[MAX_BOOKMARKS];
int bookmark_count = 0;

/* Prototype */
int parse_pipeline(char **tokens, Command *cmds, int max_cmds);

/* ---------------- HISTORY ---------------- */

void history_add(const char *line) {
    if (history_count == MAX_HISTORY) {
        free(shell_history[0]);
        memmove(shell_history, shell_history + 1, (MAX_HISTORY - 1) * sizeof(char *));
        history_count--;
    }
    shell_history[history_count++] = strdup(line);
}

void builtin_history(void) {
    for (int i = 0; i < history_count; i++) {
        printf("%3d  %s\n", i + 1, shell_history[i]);
    }
}

/* ---------------- BOOKMARK ---------------- */

void handle_bookmark(char **argv) {
    if (!argv[1]) {
        printf("Usage: bookmark add/go/list\n");
        return;
    }

    if (strcmp(argv[1], "add") == 0) {
        if (!argv[2] || !argv[3]) {
            printf("Usage: bookmark add <name> <path>\n");
            return;
        }

        strcpy(bookmarks[bookmark_count].name, argv[2]);
        strcpy(bookmarks[bookmark_count].path, argv[3]);
        bookmark_count++;

        printf("Bookmark added\n");
    }
    else if (strcmp(argv[1], "go") == 0) {
        if (!argv[2]) return;

        for (int i = 0; i < bookmark_count; i++) {
            if (strcmp(bookmarks[i].name, argv[2]) == 0) {
                chdir(bookmarks[i].path);
                return;
            }
        }
        printf("Bookmark not found\n");
    }
    else if (strcmp(argv[1], "list") == 0) {
        for (int i = 0; i < bookmark_count; i++) {
            printf("%s -> %s\n", bookmarks[i].name, bookmarks[i].path);
        }
    }
}

/* ---------------- RECENT FILES ---------------- */

void recent_files() {
    DIR *dir = opendir(".");
    struct dirent *entry;

    if (!dir) return;

    typedef struct {
        char name[256];
        time_t mod_time;
    } FileInfo;

    FileInfo files[1024];
    int count = 0;

    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        if (stat(entry->d_name, &st) == 0) {
            strcpy(files[count].name, entry->d_name);
            files[count].mod_time = st.st_mtime;
            count++;
        }
    }

    closedir(dir);

    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (files[i].mod_time < files[j].mod_time) {
                FileInfo t = files[i];
                files[i] = files[j];
                files[j] = t;
            }
        }
    }

    for (int i = 0; i < count && i < 10; i++) {
        printf("%s\n", files[i].name);
    }
}

/* ---------------- SIGNAL ---------------- */

void sigint_handler(int sig) {
    (void)sig;
    write(STDOUT_FILENO, "\n", 1);
    rl_on_new_line();
    rl_replace_line("", 0);
    rl_redisplay();
}

/* ---------------- TOKENIZER ---------------- */

int tokenise(char *line, char **tokens, int max_tokens) {
    int count = 0;
    char *p = line;

    while (*p && count < max_tokens - 1) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        char buf[MAX_INPUT];
        int len = 0;

        while (*p && !isspace((unsigned char)*p)) {
            if (*p == '|' || *p == '<' || *p == '>' || *p == '&') {
                if (len > 0) break;

                if (*p == '>' && *(p + 1) == '>') {
                    buf[len++] = '>';
                    buf[len++] = '>';
                    p += 2;
                } else {
                    buf[len++] = *p++;
                }
                break;
            } else {
                buf[len++] = *p++;
            }
        }

        if (len > 0) {
            buf[len] = '\0';
            tokens[count++] = strdup(buf);
        }
    }

    tokens[count] = NULL;
    return count;
}

/* ---------------- PARSER ---------------- */

int parse_pipeline(char **tokens, Command *cmds, int max_cmds) {
    int cmd_idx = 0, arg_idx = 0;
    static char *argv_buf[MAX_PIPES][MAX_ARGS];

    memset(cmds, 0, sizeof(Command) * max_cmds);
    cmds[0].argv = argv_buf[0];

    for (int i = 0; tokens[i]; i++) {
        if (strcmp(tokens[i], "|") == 0) {
            argv_buf[cmd_idx][arg_idx] = NULL;
            cmd_idx++;
            arg_idx = 0;
            cmds[cmd_idx].argv = argv_buf[cmd_idx];
        }
        else if (strcmp(tokens[i], "<") == 0) {
            cmds[cmd_idx].input_file = strdup(tokens[++i]);
        }
        else if (strcmp(tokens[i], ">") == 0) {
            cmds[cmd_idx].output_file = strdup(tokens[++i]);
            cmds[cmd_idx].append = 0;
        }
        else if (strcmp(tokens[i], ">>") == 0) {
            cmds[cmd_idx].output_file = strdup(tokens[++i]);
            cmds[cmd_idx].append = 1;
        }
        else if (strcmp(tokens[i], "&") == 0) {
            cmds[cmd_idx].background = 1;
        }
        else {
            argv_buf[cmd_idx][arg_idx++] = tokens[i];
        }
    }

    argv_buf[cmd_idx][arg_idx] = NULL;
    return cmd_idx + 1;
}
void search_recursive(const char *dir_path, const char *target) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                // recurse into subdirectory
                search_recursive(path, target);
            } else {
                if (strcmp(entry->d_name, target) == 0) {
                    printf("%s\n", path);
                }
            }
        }
    }

    closedir(dir);
}

/* ---------------- BUILTINS ---------------- */

int run_builtin(Command *cmd) {
    char **argv = cmd->argv;

    if (!argv[0]) return 1;

    if (strcmp(argv[0], "exit") == 0) exit(0);

    if (strcmp(argv[0], "cd") == 0) {
        chdir(argv[1] ? argv[1] : getenv("HOME"));
        return 1;
    }

    if (strcmp(argv[0], "pwd") == 0) {
        char cwd[4096];
        getcwd(cwd, sizeof(cwd));
        printf("%s\n", cwd);
        return 1;
    }

    if (strcmp(argv[0], "history") == 0) {
        builtin_history();
        return 1;
    }

    if (strcmp(argv[0], "bookmark") == 0) {
        handle_bookmark(argv);
        return 1;
    }

    if (strcmp(argv[0], "recent") == 0) {
        recent_files();
        return 1;
    }

    /* -------- ls -ascen -------- */
    if (strcmp(argv[0], "ls") == 0 && argv[1] && strcmp(argv[1], "-ascen") == 0) {
        DIR *dir = opendir(".");
        struct dirent *entry;
        char *files[1024];
        int count = 0;

        while ((entry = readdir(dir)) != NULL) {
            files[count++] = strdup(entry->d_name);
        }
        closedir(dir);

        for (int i = 0; i < count - 1; i++) {
            for (int j = i + 1; j < count; j++) {
                if (strcmp(files[i], files[j]) > 0) {
                    char *t = files[i];
                    files[i] = files[j];
                    files[j] = t;
                }
            }
        }

        for (int i = 0; i < count; i++) {
            printf("%s\n", files[i]);
            free(files[i]);
        }

        return 1;
    }

    /* -------- SEARCH (FIXED) -------- */
    if (strcmp(argv[0], "search") == 0) {
        if (!argv[1]) {
            printf("Usage: search <filename>\n");
            return 1;
        }

        char cwd[4096];
        getcwd(cwd, sizeof(cwd));

        search_recursive(cwd, argv[1]);
        return 1;
    }
    void tree_recursive(const char *dir_path, int depth) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        for (int i = 0; i < depth; i++)
            printf("│   ");

        printf("├── %s\n", entry->d_name);

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            tree_recursive(path, depth + 1);
        }
    }

    closedir(dir);
}

    /* -------- PROFILE -------- */
    if (strcmp(argv[0], "profile") == 0) {
        if (!argv[1]) {
            printf("Usage: profile <command>\n");
            return 1;
        }

        struct timeval start, end;
        gettimeofday(&start, NULL);

        pid_t pid = fork();

        if (pid == 0) {
            execvp(argv[1], &argv[1]);
            perror("execvp");
            exit(1);
        } else {
            int status;
            waitpid(pid, &status, 0);

            gettimeofday(&end, NULL);

            double time_taken =
                (end.tv_sec - start.tv_sec) +
                (end.tv_usec - start.tv_usec) / 1e6;

            printf("\n===== Profile =====\n");
            printf("Command : %s\n", argv[1]);
            printf("PID     : %d\n", pid);
            printf("Time    : %.6f sec\n", time_taken);
            printf("Exit    : %d\n", WEXITSTATUS(status));
            printf("===================\n\n");
        }
        return 1;
    }

    /* -------- WORKSPACE -------- */
    if (strcmp(argv[0], "workspace") == 0) {
        char cwd[4096];
        getcwd(cwd, sizeof(cwd));

        printf("\n===== Workspace =====\n");
        printf("Current directory : %s\n", cwd);
        printf("History size      : %d\n", history_count);
        printf("Bookmarks count   : %d\n", bookmark_count);
        printf("=====================\n\n");

        return 1;
    }
    if (strcmp(argv[0], "tree") == 0) {
        char cwd[4096];
        getcwd(cwd, sizeof(cwd));

        printf(".\n");
        tree_recursive(cwd, 0);

        return 1;
    }

    return 0;
}

/* ---------------- EXECUTION ---------------- */

void exec_single(Command *cmd) {
    if (cmd->input_file) {
        int fd = open(cmd->input_file, O_RDONLY);
        if (fd < 0) { perror("input"); exit(1); }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }

    if (cmd->output_file) {
        int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->output_file, flags, 0644);
        if (fd < 0) { perror("output"); exit(1); }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }

    execvp(cmd->argv[0], cmd->argv);
    perror("execvp");
    exit(127);
}

void execute_pipeline(Command *cmds, int n) {
    int pipes[MAX_PIPES][2];
    pid_t pids[MAX_PIPES];

    for (int i = 0; i < n - 1; i++)
        pipe(pipes[i]);

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();

        if (pid == 0) {
            if (i > 0) dup2(pipes[i-1][0], 0);
            if (i < n - 1) dup2(pipes[i][1], 1);

            for (int j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            exec_single(&cmds[i]);
        }
        pids[i] = pid;
    }

    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    if (!cmds[0].background) {
        for (int i = 0; i < n; i++)
            waitpid(pids[i], NULL, 0);
    }
}

/* ---------------- LIVE FILE SUGGESTIONS ---------------- */

#define MAX_SUGGESTIONS 8

static char live_matches[MAX_SUGGESTIONS][256];
static char live_match_paths[MAX_SUGGESTIONS][4096];
static int live_match_count = 0;
static int live_selected = 0;
static int live_drawn_rows = 0;

static void collect_live_matches(const char *dir_path, const char *query) {
    if (live_match_count >= MAX_SUGGESTIONS)
        return;

    DIR *dir = opendir(dir_path);
    if (!dir)
        return;

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL &&
           live_match_count < MAX_SUGGESTIONS) {

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0)
            continue;

        /*
         * Search recursively through directories, but match ONLY
         * the filename. This prevents "ow" from matching random
         * characters in the directory path.
         */
        if (S_ISDIR(st.st_mode)) {
            collect_live_matches(path, query);
            continue;
        }

        char name_lower[256];
        char query_lower[256];

        snprintf(name_lower, sizeof(name_lower), "%s", entry->d_name);
        snprintf(query_lower, sizeof(query_lower), "%s", query);

        for (int i = 0; name_lower[i]; i++)
            name_lower[i] = (char)tolower((unsigned char)name_lower[i]);

        for (int i = 0; query_lower[i]; i++)
            query_lower[i] = (char)tolower((unsigned char)query_lower[i]);

        if (strstr(name_lower, query_lower) != NULL) {
            snprintf(live_match_paths[live_match_count],
                     sizeof(live_match_paths[live_match_count]),
                     "%s", path);

            snprintf(live_matches[live_match_count],
                     sizeof(live_matches[live_match_count]),
                     "%s", entry->d_name);

            live_match_count++;
        }
    }

    closedir(dir);
}

static void find_live_matches(const char *query) {
    live_match_count = 0;
    live_selected = 0;

    if (!query || !*query)
        return;

    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd)))
        return;

    collect_live_matches(cwd, query);
}

/*
 * Remove the suggestion block previously drawn below the prompt.
 * We save the cursor position first, so the prompt/input is never
 * mixed with the suggestions.
 */
static void clear_live_suggestions(void) {
    if (live_drawn_rows == 0)
        return;

    printf("\0337");                 /* save cursor */
    printf("\033[1B");               /* one row below input */

    for (int i = 0; i < live_drawn_rows; i++) {
        printf("\r\033[2K");
        if (i < live_drawn_rows - 1)
            printf("\033[1B");
    }

    printf("\0338");                 /* restore cursor */
    live_drawn_rows = 0;
}

/*
 * Draw a compact Google-style suggestion list.
 *
 * Only the filename is shown, never the full path.
 */
static void draw_live_suggestions(void) {
    clear_live_suggestions();

    if (live_match_count == 0)
        return;

    printf("\0337");                 /* save cursor */
    printf("\033[1B");               /* move below input */

    for (int i = 0; i < live_match_count; i++) {
        printf("\r\033[2K");

        if (i == live_selected)
            printf("  > %s", live_matches[i]);
        else
            printf("    %s", live_matches[i]);

        if (i < live_match_count - 1)
            printf("\033[1B");
    }

    printf("\0338");                 /* return to input cursor */
    live_drawn_rows = live_match_count;
    fflush(stdout);
}

static void redraw_live_input(const char *prompt,
                              const char *line,
                              int cursor) {
    /*
     * Erase only the current input row and redraw it.
     * Suggestions are drawn separately below it.
     */
    printf("\r\033[2K");
    printf("%s", prompt);
    printf("%s", line);

    /*
     * Move cursor from end of line back to the logical cursor.
     */
    int end_pos = (int)strlen(line);
    int move_left = end_pos - cursor;

    if (move_left > 0)
        printf("\033[%dD", move_left);

    fflush(stdout);
    draw_live_suggestions();
}

static void insert_live_char(char *line, int *length, int *cursor, char c) {
    if (*length >= MAX_INPUT - 1)
        return;

    memmove(&line[*cursor + 1],
            &line[*cursor],
            (size_t)(*length - *cursor + 1));

    line[*cursor] = c;
    (*length)++;
    (*cursor)++;
}

/*
 * If the user has typed:
 *
 *     search rep
 *
 * this returns "rep".
 */
static const char *get_search_query(const char *line) {
    if (strncmp(line, "search ", 7) == 0)
        return line + 7;

    return NULL;
}


static void apply_live_selection(char *line, int *length, int *cursor) {
    if (live_match_count <= 0)
        return;

    const char *prefix = "search ";
    size_t prefix_len = strlen(prefix);
    size_t name_len = strlen(live_matches[live_selected]);

    if (prefix_len + name_len >= MAX_INPUT)
        return;

    memcpy(line, prefix, prefix_len);
    memcpy(line + prefix_len, live_matches[live_selected], name_len);
    line[prefix_len + name_len] = '\0';

    *length = (int)(prefix_len + name_len);
    *cursor = *length;
}

/*
 * Custom line editor.
 *
 * Live suggestions appear only for:
 *
 *     search <text>
 *
 * They update after every typed character.
 */
static char *readline_live(const char *prompt) {
    struct termios old_term, raw_term;

    if (tcgetattr(STDIN_FILENO, &old_term) != 0)
        return readline(prompt);

    raw_term = old_term;

    raw_term.c_lflag &= ~(ICANON | ECHO);
    raw_term.c_cc[VMIN] = 1;
    raw_term.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_term) != 0)
        return readline(prompt);

    char line[MAX_INPUT];
    int length = 0;
    int cursor = 0;

    line[0] = '\0';

    live_match_count = 0;
    live_selected = 0;
    live_drawn_rows = 0;

    printf("%s", prompt);
    fflush(stdout);

    while (1) {
        char c;

        if (read(STDIN_FILENO, &c, 1) != 1)
            break;

        /* Enter */
        if (c == '\r' || c == '\n') {
            clear_live_suggestions();

            printf("\r\033[2K%s%s\n", prompt, line);
            fflush(stdout);
            break;
        }

        /* Ctrl+C */
        if (c == 3) {
            clear_live_suggestions();

            printf("\r\033[2K%s^C\n", prompt);
            line[0] = '\0';
            length = 0;
            cursor = 0;
            break;
        }

        /* Backspace */
        if (c == 127 || c == 8) {
            if (cursor > 0) {
                memmove(&line[cursor - 1],
                        &line[cursor],
                        (size_t)(length - cursor + 1));

                cursor--;
                length--;
            }

            const char *query = get_search_query(line);

            if (query) {
                find_live_matches(query);
            } else {
                live_match_count = 0;
                live_selected = 0;
            }

            redraw_live_input(prompt, line, cursor);
            continue;
        }

        /* Escape sequence: arrows */
        if (c == 27) {
            char seq1, seq2;

            if (read(STDIN_FILENO, &seq1, 1) != 1)
                continue;

            if (seq1 != '[')
                continue;

            if (read(STDIN_FILENO, &seq2, 1) != 1)
                continue;

            if (seq2 == 'A') {                 /* Up */
                if (live_match_count > 0) {
                    live_selected--;

                    if (live_selected < 0)
                        live_selected = live_match_count - 1;
                }
            }
            else if (seq2 == 'B') {            /* Down */
                if (live_match_count > 0) {
                    live_selected++;

                    if (live_selected >= live_match_count)
                        live_selected = 0;
                }
            }
            else if (seq2 == 'C') {            /* Right */
                if (cursor < length)
                    cursor++;
            }
            else if (seq2 == 'D') {            /* Left */
                if (cursor > 0)
                    cursor--;
            }

            redraw_live_input(prompt, line, cursor);
            continue;
        }

        /* Printable character */
        if (isprint((unsigned char)c)) {
            insert_live_char(line, &length, &cursor, c);

            const char *query = get_search_query(line);

            if (query) {
                find_live_matches(query);
            } else {
                live_match_count = 0;
                live_selected = 0;
            }

            redraw_live_input(prompt, line, cursor);
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);

    return strdup(line);
}

/* ---------------- PROMPT ---------------- */

void build_prompt(char *buf, size_t size) {
    char cwd[4096];
    getcwd(cwd, sizeof(cwd));
    snprintf(buf, size, "TShell:%s$ ", cwd);
}

/* ---------------- MAIN ---------------- */

int main() {
    setenv("PATH", "/usr/bin:/bin", 1);

    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    rl_catch_signals = 0;

    char *tokens[MAX_ARGS];
    Command cmds[MAX_PIPES];

    printf("Welcome to TShell\n");

    while (1) {
        char prompt[512];
        build_prompt(prompt, sizeof(prompt));

        char *line = readline_live(prompt);
        if (!line) break;

        if (*line == '\0') { free(line); continue; }

        history_add(line);
        add_history(line);

        int ntok = tokenise(line, tokens, MAX_ARGS);
        int ncmd = parse_pipeline(tokens, cmds, MAX_PIPES);

        if (ncmd == 1 && !run_builtin(&cmds[0])) {
            execute_pipeline(cmds, ncmd);
        } else if (ncmd > 1) {
            execute_pipeline(cmds, ncmd);
        }

        for (int i = 0; i < ntok; i++) free(tokens[i]);
        free(line);
    }

    return 0;
}
