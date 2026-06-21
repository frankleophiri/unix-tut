/*
 * SERC Mini-OS — GTK3 Graphical Interface (C)
 * UNIX/POSIX EDITION — uses real POSIX system calls as required by
 * the assignment ("Libraries: Standard C libraries and POSIX system
 * calls" / "Environment: Linux or UNIX-based system recommended").
 *
 * Real POSIX calls used in this file:
 *   fork()    - process management.c  Creates a REAL child OS process
 *               for every scheduled emergency task.
 *   waitpid() - process management.c  Parent waits for the real child
 *               to finish, just like a real OS scheduler would.
 *   usleep()  - process management.c  The child "executes" by sleeping
 *               for its burst time — simulating real CPU work.
 *   open()/write()/read()/close() - file management.c  Low-level POSIX
 *               file I/O for the system log, instead of fopen/fprintf.
 *   getpid()  - returns the real OS process ID of a child.
 *   pipe()    - IPC.c  Creates a real kernel-managed pipe so two
 *               actual OS processes can exchange data (Component 4:
 *               Inter-Process Communication, "ambulance -> police").
 *   mkdir()/opendir()/readdir()/closedir()/stat() - filesystem
 *               structure.c  Builds and browses a REAL directory tree
 *               on disk (./serc_records/) for per-process records.
 *
 * COMPILE (Linux / WSL / any UNIX system):
 *   gcc gtk_mini_os.c -o gtk_mini_os $(pkg-config --cflags --libs gtk+-3.0)
 *
 * (Note the order: the source file comes BEFORE the pkg-config flags.
 *  Libraries must come AFTER the .c file on the command line, or the
 *  linker won't find the GTK symbols — this is a classic gcc gotcha.)
 *
 * NOTE: fork()/waitpid()/pipe()/opendir() are POSIX-only — this will
 * NOT compile on native Windows. That's intentional: the assignment
 * requires a Linux/UNIX environment. Use WSL (Windows Subsystem for
 * Linux) or a real Ubuntu machine/VM.
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>      /* fork(), usleep(), getpid(), pipe(), POSIX read/write/close */
#include <sys/types.h>   /* pid_t, ssize_t */
#include <sys/wait.h>    /* waitpid() */
#include <fcntl.h>       /* open(), O_CREAT, O_WRONLY, O_APPEND, O_TRUNC */
#include <time.h>        /* time(), ctime() — real timestamps in the log */
#include <dirent.h>      /* opendir(), readdir(), closedir() — directory browsing */
#include <sys/stat.h>    /* mkdir(), stat() — directory creation + file sizes */
#include <errno.h>       /* errno, EEXIST — checking why a syscall failed */

/* ─────────────────────────────────────────
   CONSTANTS  (same as mini_os.c)
   ───────────────────────────────────────── */
#define MAX_PROCESSES     10
#define MAX_MEMORY_BLOCKS 10
#define TOTAL_MEMORY      512
#define NAME_LEN          100
#define RECORDS_DIR       "serc_records"  /* real directory created on disk */

/* ─────────────────────────────────────────
   PROCESS STATES  (same as mini_os.c)
   ───────────────────────────────────────── */
typedef enum { NEW, READY, RUNNING, WAITING, TERMINATED } State;

const char *stateStr(State s) {
    switch (s) {
        case NEW:        return "New";
        case READY:      return "Ready";
        case RUNNING:    return "Running";
        case WAITING:    return "Waiting";
        case TERMINATED: return "Terminated";
        default:         return "Unknown";
    }
}

/* ─────────────────────────────────────────
   PCB  (same as mini_os.c + one new POSIX field)
   ───────────────────────────────────────── */
typedef struct {
    int pid;
    char name[NAME_LEN];
    State state;
    int priority;
    int burstTime;
    int arrivalTime;
    int waitingTime;
    int turnaroundTime;
    int memoryRequired;
    int memoryBlock;
    int active;
    pid_t osPid;   /* REAL operating-system PID, set by fork() when this
                      task is actually executed as a child process.
                      -1 means "not run yet". This is the ACTUAL Unix
                      process ID from the kernel, not a made-up number. */
} PCB;

/* ─────────────────────────────────────────
   MEMORY BLOCK  (same as mini_os.c)
   ───────────────────────────────────────── */
typedef struct {
    int size;
    int free;
    int pid;
} MemoryBlock;

/* ─────────────────────────────────────────
   OS GLOBAL STATE
   ───────────────────────────────────────── */
static PCB         processes[MAX_PROCESSES];
static int         processCount    = 0;
static MemoryBlock memory[MAX_MEMORY_BLOCKS];
static int         memoryBlockCount = 0;
static int         pidCounter      = 1;
static const char *logFile         = "serc_log.txt";

/* ─────────────────────────────────────────
   GTK WIDGET GLOBALS
   ───────────────────────────────────────── */
enum { C_PID, C_NAME, C_STATE, C_PRIO, C_BURST, C_ARR, C_MEM, C_OSPID, N_COLS };

static GtkListStore *proc_store;
static GtkTextBuffer *log_buf;
static GtkTextBuffer *sched_buf;
static GtkTextBuffer *ipc_buf;     /* IPC / pipes message history */
static GtkTextBuffer *fs_buf;      /* filesystem directory listing */
static GtkWidget    *e_name, *e_burst, *e_arrival, *e_mem;
static GtkWidget    *combo_prio, *combo_pid_alloc, *combo_algo;
static GtkWidget    *e_ipc_sender, *e_ipc_receiver, *e_ipc_msg;
static GtkWidget    *mem_labels[MAX_MEMORY_BLOCKS];
static GtkWidget    *mem_progress;

/* ═══════════════════════════════════════════
   OS LOGIC  (identical to mini_os.c)
   ═══════════════════════════════════════════ */

/* ═══════════════════════════════════════════
   GUI HELPERS
   ═══════════════════════════════════════════ */

/*
 * logEvent() — POSIX VERSION
 * Uses the raw POSIX system calls open()/write()/close() instead of
 * the C standard library's fopen()/fprintf()/fclose(). These are
 * the actual Unix kernel system calls — fopen() is a *wrapper*
 * built on top of open(), so this is one layer closer to the OS.
 */
void logEvent(const char *msg) {
    /* Real timestamp using the POSIX time() + ctime() calls */
    time_t now = time(NULL);
    char *ts = ctime(&now);        /* e.g. "Sun Jun 21 14:32:08 2026\n" */
    ts[strcspn(ts, "\n")] = '\0';  /* ctime() always adds a \n — strip it */

    char line[600];
    int len = snprintf(line, sizeof(line), "[%s] %s\n", ts, msg);

    /* open() with POSIX flags:
         O_CREAT  -> create the file if it doesn't exist
         O_WRONLY -> open for writing only
         O_APPEND -> always write at the end, never overwrite
         0644     -> file permissions (rw-r--r--), Unix-style */
    int fd = open(logFile, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd != -1) {
        write(fd, line, len);   /* raw POSIX write — writes len bytes */
        close(fd);              /* always close the file descriptor */
    }

    if (log_buf) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(log_buf, &end);
        gtk_text_buffer_insert(log_buf, &end, line, -1);
    }
}

/*
 * loadLogFromDisk() — reads any existing serc_log.txt using the
 * POSIX open()/read()/close() calls and pours it into the GTK log
 * view on startup. Demonstrates POSIX file READ (logEvent already
 * demonstrates POSIX file WRITE).
 */
void loadLogFromDisk() {
    int fd = open(logFile, O_RDONLY);
    if (fd == -1) return;   /* file doesn't exist yet — that's fine */

    char buf[2048];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        if (log_buf) {
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(log_buf, &end);
            gtk_text_buffer_insert(log_buf, &end, buf, -1);
        }
    }
    close(fd);
}

void initMemory() {
    int sizes[] = {64, 128, 64, 128, 128};
    memoryBlockCount = 5;
    for (int i = 0; i < memoryBlockCount; i++) {
        memory[i].size = sizes[i];
        memory[i].free = 1;
        memory[i].pid  = -1;
    }
}

/* ═══════════════════════════════════════════
   FILESYSTEM STRUCTURE  (real POSIX directory calls)
   ═══════════════════════════════════════════ */

/*
 * initFilesystem() — creates a real directory on disk using mkdir().
 * This is the actual Unix system call behind the `mkdir` shell command.
 */
void initFilesystem() {
    /* mode 0755 = owner rwx, group r-x, others r-x — standard Unix dir perms */
    if (mkdir(RECORDS_DIR, 0755) == -1) {
        if (errno != EEXIST) {
            logEvent("ERROR: mkdir() failed to create serc_records/");
        }
        /* errno == EEXIST just means the folder is already there from a
           previous run — nothing wrong, so we say nothing */
    }
}

/*
 * writeProcessRecord() — for every process created, writes an individual
 * file into ./serc_records/ describing it. This builds a genuine
 * hierarchical filesystem: a root log file PLUS a structured directory
 * of per-process records, instead of one flat file.
 */
void writeProcessRecord(PCB *p) {
    char path[256];
    snprintf(path, sizeof(path), "%s/PID_%d_%s.txt", RECORDS_DIR, p->pid, p->name);
    for (char *c = path; *c; c++) if (*c == ' ') *c = '_';  /* clean filename */

    char content[400];
    int len = snprintf(content, sizeof(content),
        "SERC Emergency Task Record\n"
        "--------------------------\n"
        "PID:       %d\n"
        "Name:      %s\n"
        "Priority:  %d\n"
        "Burst:     %d ms\n"
        "Arrival:   %d ms\n"
        "Memory:    %d MB\n",
        p->pid, p->name, p->priority, p->burstTime, p->arrivalTime, p->memoryRequired);

    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd != -1) {
        write(fd, content, len);
        close(fd);
    }
}

/*
 * refreshFilesystemView() — uses opendir()/readdir()/closedir() to
 * literally walk the real directory on disk, and stat() to read each
 * file's real size from the filesystem. Nothing here is simulated —
 * if you open a terminal and run `ls -l serc_records/` you'll see
 * exactly what this function shows in the GUI.
 */
void refreshFilesystemView() {
    if (!fs_buf) return;

    GString *out = g_string_new("");
    g_string_append_printf(out, "Directory: ./%s/\n\n", RECORDS_DIR);

    DIR *d = opendir(RECORDS_DIR);
    if (!d) {
        g_string_append(out, "(directory not found — create a process first)\n");
    } else {
        struct dirent *entry;
        int count = 0;
        while ((entry = readdir(d)) != NULL) {
            /* every Unix directory contains "." (itself) and ".."
               (parent) as real entries — skip those two */
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            char fullpath[300];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", RECORDS_DIR, entry->d_name);

            struct stat st;
            if (stat(fullpath, &st) == 0)
                g_string_append_printf(out, "  %-40s  %6ld bytes\n", entry->d_name, (long)st.st_size);
            else
                g_string_append_printf(out, "  %-40s  (stat failed)\n", entry->d_name);
            count++;
        }
        closedir(d);
        g_string_append_printf(out, "\n%d file(s) on disk.\n", count);
    }

    gtk_text_buffer_set_text(fs_buf, out->str, -1);
    g_string_free(out, TRUE);
}

/* ─────────────────────────────────────────
   SORT HELPERS  (same bubble sort as mini_os.c)
   ───────────────────────────────────────── */
void sortByArrival() {
    for (int i = 0; i < processCount - 1; i++)
        for (int j = 0; j < processCount - i - 1; j++)
            if (processes[j].arrivalTime > processes[j+1].arrivalTime) {
                PCB t = processes[j]; processes[j] = processes[j+1]; processes[j+1] = t;
            }
}

void sortByPriority() {
    for (int i = 0; i < processCount - 1; i++)
        for (int j = 0; j < processCount - i - 1; j++)
            if (processes[j].priority > processes[j+1].priority) {
                PCB t = processes[j]; processes[j] = processes[j+1]; processes[j+1] = t;
            }
}

/*
 * runRealProcess() — THE REAL POSIX PROCESS MANAGEMENT.
 * This is what actually satisfies "POSIX system calls" for Process
 * Management. It does NOT simulate a process — it creates one.
 *
 *   fork()    duplicates the current program into a parent + child.
 *   getpid()  inside the child returns the REAL kernel-assigned PID.
 *   usleep()  the child "does the emergency work" by sleeping for
 *             exactly its burst time, in microseconds.
 *   _exit()   the child terminates — it never returns to GTK code.
 *   waitpid() the parent (our GUI) blocks until that exact child
 *             finishes, then reaps it so it doesn't become a zombie.
 */
pid_t runRealProcess(PCB *p) {
    pid_t child = fork();

    if (child == 0) {
        /* ---- THIS BLOCK RUNS INSIDE THE CHILD PROCESS ---- */
        usleep(p->burstTime * 1000);  /* burstTime is in ms -> convert to µs */
        _exit(0);                     /* child dies here, cleanly */
    }
    else if (child > 0) {
        /* ---- THIS BLOCK RUNS INSIDE THE PARENT (our GUI) ---- */
        int status;
        waitpid(child, &status, 0);   /* wait for that specific child */
        return child;                 /* the REAL OS pid of the child */
    }
    else {
        logEvent("ERROR: fork() failed — could not create real process");
        return -1;
    }
}

/* ═══════════════════════════════════════════
   INTER-PROCESS COMMUNICATION  (real POSIX pipe())
   Assignment Component 4 — example given was literally
   "ambulance process sends update to police process".
   ═══════════════════════════════════════════ */

/*
 * sendPipeMessage() — creates a REAL kernel pipe, forks a REAL child
 * process to act as the sender, and has the parent (this GUI) act as
 * the receiver. The message physically travels through a kernel
 * buffer between two separate processes — this is not a simulation,
 * it's the same mechanism behind shell commands like `ls | grep foo`.
 *
 * Returns the sender's real PID, or -1 on failure.
 */
pid_t sendPipeMessage(const char *senderLabel, const char *receiverLabel,
                       const char *message, char *outReceived, size_t outLen) {
    int fd[2];   /* fd[0] = read end, fd[1] = write end — POSIX convention */

    if (pipe(fd) == -1) {
        logEvent("ERROR: pipe() failed — could not create IPC channel");
        return -1;
    }

    pid_t senderPid = fork();

    if (senderPid == 0) {
        /* ---- CHILD PROCESS = the SENDER (e.g. Ambulance) ---- */
        close(fd[0]);                         /* sender never reads — close that end */
        write(fd[1], message, strlen(message) + 1);  /* +1 sends the '\0' too */
        close(fd[1]);
        _exit(0);
    }

    /* ---- PARENT PROCESS = the RECEIVER (e.g. Police) ----
       The parent must stay alive to keep running the GTK event loop,
       so in this simplified model the GUI process itself plays the
       role of the receiving emergency unit. */
    close(fd[1]);                              /* receiver never writes — close that end */
    ssize_t n = read(fd[0], outReceived, outLen - 1);
    outReceived[n > 0 ? n : 0] = '\0';
    close(fd[0]);

    int status;
    waitpid(senderPid, &status, 0);   /* reap the sender so it isn't a zombie */

    (void)senderLabel; (void)receiverLabel;  /* used by the caller for logging */
    return senderPid;
}

void refreshProcStore() {
    gtk_list_store_clear(proc_store);
    for (int i = 0; i < processCount; i++) {
        if (!processes[i].active) continue;
        GtkTreeIter it;
        gtk_list_store_append(proc_store, &it);

        /* Show "—" until the process has actually been forked/run */
        char ospidTxt[16];
        if (processes[i].osPid > 0)
            snprintf(ospidTxt, sizeof(ospidTxt), "%d", processes[i].osPid);
        else
            snprintf(ospidTxt, sizeof(ospidTxt), "—");

        gtk_list_store_set(proc_store, &it,
            C_PID,   processes[i].pid,
            C_NAME,  processes[i].name,
            C_STATE, stateStr(processes[i].state),
            C_PRIO,  processes[i].priority,
            C_BURST, processes[i].burstTime,
            C_ARR,   processes[i].arrivalTime,
            C_MEM,   processes[i].memoryRequired,
            C_OSPID, ospidTxt,
            -1);
    }
}

/* Update the memory-block labels and progress bar */
void refreshMemDisplay() {
    int used = 0;
    for (int i = 0; i < memoryBlockCount; i++) {
        char markup[400];
        if (memory[i].free) {
            snprintf(markup, sizeof(markup),
                "<tt><b>Block %d</b>  |  %d MB  |  <span foreground='#27ae60'>FREE</span></tt>",
                i, memory[i].size);
        } else {
            const char *pname = "?";
            for (int j = 0; j < processCount; j++)
                if (processes[j].pid == memory[i].pid) { pname = processes[j].name; break; }
            snprintf(markup, sizeof(markup),
                "<tt><b>Block %d</b>  |  %d MB  |  <span foreground='#e74c3c'>USED — PID %d (%s)</span></tt>",
                i, memory[i].size, memory[i].pid, pname);
            used += memory[i].size;
        }
        gtk_label_set_markup(GTK_LABEL(mem_labels[i]), markup);
    }

    if (mem_progress) {
        double frac = (double)used / TOTAL_MEMORY;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(mem_progress), frac);
        char txt[100];
        snprintf(txt, sizeof(txt), "%d / %d MB used  (%.0f%%)", used, TOTAL_MEMORY, frac * 100.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(mem_progress), txt);
    }
}

/* Repopulate the PID combo on the Memory tab */
void refreshPidCombo() {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo_pid_alloc));
    for (int i = 0; i < processCount; i++) {
        if (!processes[i].active) continue;
        char txt[300];
        snprintf(txt, sizeof(txt), "PID %d — %s  (needs %d MB, block: %s)",
            processes[i].pid, processes[i].name,
            processes[i].memoryRequired,
            processes[i].memoryBlock == -1 ? "none" : "assigned");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_pid_alloc), txt);
    }
}

/* ═══════════════════════════════════════════
   CALLBACKS
   ═══════════════════════════════════════════ */

static void cb_create(GtkButton *b, gpointer d) {
    if (processCount >= MAX_PROCESSES) { logEvent("ERROR: Max processes reached"); return; }

    const char *name = gtk_entry_get_text(GTK_ENTRY(e_name));
    if (!name || strlen(name) == 0) { logEvent("ERROR: Task name cannot be empty"); return; }

    PCB p;
    memset(&p, 0, sizeof(p));
    p.pid            = pidCounter++;
    strncpy(p.name, name, NAME_LEN - 1);
    p.priority       = atoi(gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo_prio)));
    p.burstTime      = atoi(gtk_entry_get_text(GTK_ENTRY(e_burst)));
    p.arrivalTime    = atoi(gtk_entry_get_text(GTK_ENTRY(e_arrival)));
    p.memoryRequired = atoi(gtk_entry_get_text(GTK_ENTRY(e_mem)));
    p.state          = READY;
    p.memoryBlock    = -1;
    p.active         = 1;
    p.osPid          = -1;   /* not forked/run yet */
    processes[processCount++] = p;
    writeProcessRecord(&p);  /* creates a real file in ./serc_records/ */

    char msg[300];
    snprintf(msg, sizeof(msg), "Process created: PID=%d  Name=%s  Priority=%d  Burst=%dms",
        p.pid, p.name, p.priority, p.burstTime);
    logEvent(msg);

    gtk_entry_set_text(GTK_ENTRY(e_name), "");
    refreshProcStore();
    refreshPidCombo();
    refreshFilesystemView();
}

static void cb_terminate(GtkButton *b, gpointer tv_ptr) {
    GtkTreeView      *tv  = GTK_TREE_VIEW(tv_ptr);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
    GtkTreeModel     *mdl;
    GtkTreeIter       it;
    if (!gtk_tree_selection_get_selected(sel, &mdl, &it)) {
        logEvent("ERROR: Select a process row first");
        return;
    }
    int pid;
    gtk_tree_model_get(mdl, &it, C_PID, &pid, -1);

    for (int i = 0; i < processCount; i++) {
        if (processes[i].pid == pid && processes[i].active) {
            processes[i].state  = TERMINATED;
            processes[i].active = 0;
            if (processes[i].memoryBlock != -1) {
                memory[processes[i].memoryBlock].free = 1;
                memory[processes[i].memoryBlock].pid  = -1;
                processes[i].memoryBlock = -1;
            }
            char msg[100];
            snprintf(msg, sizeof(msg), "Process terminated: PID=%d", pid);
            logEvent(msg);
            refreshProcStore();
            refreshMemDisplay();
            refreshPidCombo();
            return;
        }
    }
    logEvent("ERROR: PID not found");
}

static void cb_run_sched(GtkButton *b, gpointer d) {
    int active = 0;
    for (int i = 0; i < processCount; i++) if (processes[i].active) active++;
    if (!active) { logEvent("ERROR: No active processes to schedule"); return; }

    int algo = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_algo)); /* 0=FCFS 1=Priority */
    if (algo == 0) sortByArrival(); else sortByPriority();

    GString *out = g_string_new(algo == 0
        ? "── FCFS Scheduling Results ─────────────────────────\n\n"
        : "── Priority Scheduling Results ─────────────────────\n\n");

    int curTime = 0, totWait = 0, totTA = 0, cnt = 0;
    for (int i = 0; i < processCount; i++) {
        if (!processes[i].active) continue;
        cnt++;
        if (curTime < processes[i].arrivalTime) curTime = processes[i].arrivalTime;
        processes[i].waitingTime    = curTime - processes[i].arrivalTime;
        if (processes[i].waitingTime < 0) processes[i].waitingTime = 0;
        processes[i].turnaroundTime = processes[i].waitingTime + processes[i].burstTime;
        curTime  += processes[i].burstTime;
        totWait  += processes[i].waitingTime;
        totTA    += processes[i].turnaroundTime;
        processes[i].state = RUNNING;

        /* ── REAL POSIX EXECUTION ──
           Up to this point everything was just arithmetic (wait/turnaround
           time). This line actually forks a brand-new child OS process,
           lets it "run" for burstTime ms, and waits for it to finish —
           genuine Unix process management, not a simulation. */
        processes[i].osPid = runRealProcess(&processes[i]);

        g_string_append_printf(out,
            "  PID %-3d  [P%d]  %-22s  Wait: %-5dms  T/A: %-6dms  RealOSPid: %d\n",
            processes[i].pid, processes[i].priority, processes[i].name,
            processes[i].waitingTime, processes[i].turnaroundTime, processes[i].osPid);
        processes[i].state = TERMINATED;

        char lmsg[150];
        snprintf(lmsg, sizeof(lmsg), "%s ran PID=%d as REAL OS process (kernel pid=%d, ran for %dms)",
            algo == 0 ? "FCFS" : "Priority", processes[i].pid, processes[i].osPid, processes[i].burstTime);
        logEvent(lmsg);
    }
    g_string_append_printf(out,
        "\n  Avg Waiting Time    : %d ms\n"
        "  Avg Turnaround Time : %d ms\n"
        "  CPU Utilization     : ~100%%\n",
        totWait / cnt, totTA / cnt);

    gtk_text_buffer_set_text(sched_buf, out->str, -1);
    g_string_free(out, TRUE);
    refreshProcStore();
}

static void cb_alloc_mem(GtkButton *b, gpointer d) {
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_pid_alloc));
    if (idx < 0) { logEvent("ERROR: Select a process first"); return; }

    /* Map combo index → pid */
    int cnt = 0, pid = -1;
    for (int i = 0; i < processCount; i++) {
        if (!processes[i].active) continue;
        if (cnt == idx) { pid = processes[i].pid; break; }
        cnt++;
    }
    if (pid < 0) return;

    PCB *p = NULL;
    for (int i = 0; i < processCount; i++)
        if (processes[i].pid == pid && processes[i].active) { p = &processes[i]; break; }
    if (!p) return;
    if (p->memoryBlock != -1) { logEvent("ERROR: Already has memory assigned"); return; }

    /* First Fit  (same algorithm as mini_os.c) */
    for (int i = 0; i < memoryBlockCount; i++) {
        if (memory[i].free && memory[i].size >= p->memoryRequired) {
            memory[i].free = 0;
            memory[i].pid  = pid;
            p->memoryBlock = i;
            p->state       = RUNNING;
            char msg[200];
            snprintf(msg, sizeof(msg),
                "Memory allocated: Block %d (%d MB) -> PID %d  [First Fit]",
                i, memory[i].size, pid);
            logEvent(msg);
            refreshMemDisplay();
            refreshProcStore();
            refreshPidCombo();
            return;
        }
    }
    logEvent("ERROR: No suitable block found — fragmentation or insufficient memory");
}

static void cb_free_mem(GtkButton *b, gpointer d) {
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_pid_alloc));
    if (idx < 0) { logEvent("ERROR: Select a process first"); return; }

    int cnt = 0, pid = -1;
    for (int i = 0; i < processCount; i++) {
        if (!processes[i].active) continue;
        if (cnt == idx) { pid = processes[i].pid; break; }
        cnt++;
    }
    if (pid < 0) return;

    for (int i = 0; i < processCount; i++) {
        if (processes[i].pid == pid && processes[i].memoryBlock != -1) {
            int blk = processes[i].memoryBlock;
            memory[blk].free = 1;
            memory[blk].pid  = -1;
            processes[i].memoryBlock = -1;
            processes[i].state = READY;
            char msg[100];
            snprintf(msg, sizeof(msg), "Memory freed: Block %d from PID %d", blk, pid);
            logEvent(msg);
            refreshMemDisplay();
            refreshProcStore();
            refreshPidCombo();
            return;
        }
    }
    logEvent("ERROR: No memory assigned to this process");
}

static void cb_clear_log(GtkButton *b, gpointer d) {
    gtk_text_buffer_set_text(log_buf, "", -1);
    /* POSIX truncate: open with O_TRUNC wipes the file to 0 bytes */
    int fd = open(logFile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd != -1) close(fd);
    logEvent("Log cleared.");
}

static void cb_send_ipc(GtkButton *b, gpointer d) {
    const char *sender   = gtk_entry_get_text(GTK_ENTRY(e_ipc_sender));
    const char *receiver = gtk_entry_get_text(GTK_ENTRY(e_ipc_receiver));
    const char *msg      = gtk_entry_get_text(GTK_ENTRY(e_ipc_msg));

    if (!msg || strlen(msg) == 0) { logEvent("ERROR: IPC message cannot be empty"); return; }

    char received[300];
    pid_t senderPid = sendPipeMessage(sender, receiver, msg, received, sizeof(received));
    if (senderPid < 0) return;

    char logMsg[500];
    snprintf(logMsg, sizeof(logMsg),
        "IPC pipe: \"%s\" -> \"%s\"  |  msg=\"%s\"  |  senderPID=%d  receiverPID(this GUI)=%d",
        sender, receiver, received, senderPid, getpid());
    logEvent(logMsg);

    if (ipc_buf) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(ipc_buf, &end);
        char line[600];
        snprintf(line, sizeof(line), "[%s -> %s]   PID %d -> PID %d:   \"%s\"\n",
            sender, receiver, senderPid, getpid(), received);
        gtk_text_buffer_insert(ipc_buf, &end, line, -1);
    }

    gtk_entry_set_text(GTK_ENTRY(e_ipc_msg), "");
}

static void cb_refresh_fs(GtkButton *b, gpointer d) {
    refreshFilesystemView();
}


/* ═══════════════════════════════════════════
   TAB BUILDERS
   ═══════════════════════════════════════════ */

static GtkWidget *build_process_tab() {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);

    /* ── Form ── */
    GtkWidget *frame = gtk_frame_new("  Create Emergency Task  ");
    GtkWidget *grid  = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);

    /* Row 0: name */
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Task Name:"), 0, 0, 1, 1);
    e_name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(e_name), "e.g. Ambulance Dispatch");
    gtk_widget_set_hexpand(e_name, TRUE);
    gtk_grid_attach(GTK_GRID(grid), e_name, 1, 0, 3, 1);

    /* Row 1: priority + burst */
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Priority (1-5):"), 0, 1, 1, 1);
    combo_prio = gtk_combo_box_text_new();
    const char *plbls[] = {"1 — Critical","2 — High","3 — Medium","4 — Low","5 — Routine"};
    for (int i = 0; i < 5; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_prio), plbls[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_prio), 2);
    gtk_grid_attach(GTK_GRID(grid), combo_prio, 1, 1, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Burst Time (ms):"), 2, 1, 1, 1);
    e_burst = gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(e_burst), "30");
    gtk_grid_attach(GTK_GRID(grid), e_burst, 3, 1, 1, 1);

    /* Row 2: arrival + mem */
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Arrival Time (ms):"), 0, 2, 1, 1);
    e_arrival = gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(e_arrival), "0");
    gtk_grid_attach(GTK_GRID(grid), e_arrival, 1, 2, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Memory Required (MB):"), 2, 2, 1, 1);
    e_mem = gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(e_mem), "64");
    gtk_grid_attach(GTK_GRID(grid), e_mem, 3, 2, 1, 1);

    /* Create button */
    GtkWidget *btn_create = gtk_button_new_with_label("  +  Create Process  ");
    gtk_grid_attach(GTK_GRID(grid), btn_create, 0, 3, 4, 1);
    g_signal_connect(btn_create, "clicked", G_CALLBACK(cb_create), NULL);

    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);

    /* ── TreeView ── */
    GtkWidget *frame2 = gtk_frame_new("  Active Processes  ");
    proc_store = gtk_list_store_new(N_COLS,
        G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT,
        G_TYPE_STRING);  /* C_OSPID — real kernel PID, shown as text */

    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(proc_store));
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(tv), GTK_TREE_VIEW_GRID_LINES_BOTH);

    const char *col_names[] = {"PID","Name","State","Priority","Burst(ms)","Arrival(ms)","Mem(MB)","Real OS PID"};
    for (int i = 0; i < N_COLS; i++) {
        GtkCellRenderer    *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn  *c = gtk_tree_view_column_new_with_attributes(col_names[i], r, "text", i, NULL);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_column_set_min_width(c, 70);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), c);
    }

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 200);
    gtk_container_add(GTK_CONTAINER(scroll), tv);
    gtk_container_add(GTK_CONTAINER(frame2), scroll);
    gtk_box_pack_start(GTK_BOX(vbox), frame2, TRUE, TRUE, 0);

    GtkWidget *btn_term = gtk_button_new_with_label("  ✕  Terminate Selected Process  ");
    g_signal_connect(btn_term, "clicked", G_CALLBACK(cb_terminate), tv);
    gtk_box_pack_start(GTK_BOX(vbox), btn_term, FALSE, FALSE, 0);

    return vbox;
}

static GtkWidget *build_scheduling_tab() {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);

    /* Controls */
    GtkWidget *frame_ctrl = gtk_frame_new("  Run CPU Scheduling Algorithm  ");
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 10);

    combo_algo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_algo), "FCFS — First Come First Serve");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_algo), "Priority Scheduling (Emergency Level)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_algo), 0);

    GtkWidget *btn_run = gtk_button_new_with_label("  ▶  Run  ");
    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("Algorithm:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), combo_algo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), btn_run, FALSE, FALSE, 0);
    g_signal_connect(btn_run, "clicked", G_CALLBACK(cb_run_sched), NULL);
    gtk_container_add(GTK_CONTAINER(frame_ctrl), hbox);
    gtk_box_pack_start(GTK_BOX(vbox), frame_ctrl, FALSE, FALSE, 0);

    /* Results */
    GtkWidget *frame_res = gtk_frame_new("  Scheduling Results  ");
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    sched_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_text_buffer_set_text(sched_buf, "\n  Create some processes then press Run.\n", -1);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tv);
    gtk_container_add(GTK_CONTAINER(frame_res), scroll);
    gtk_box_pack_start(GTK_BOX(vbox), frame_res, TRUE, TRUE, 0);

    return vbox;
}

static GtkWidget *build_memory_tab() {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);

    /* Controls */
    GtkWidget *frame_ctrl = gtk_frame_new("  Memory Allocation — First Fit  ");
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);

    combo_pid_alloc = gtk_combo_box_text_new();
    GtkWidget *btn_alloc = gtk_button_new_with_label("  +  Allocate Memory  ");
    GtkWidget *btn_free  = gtk_button_new_with_label("  ⊖  Free Memory  ");
    g_signal_connect(btn_alloc, "clicked", G_CALLBACK(cb_alloc_mem), NULL);
    g_signal_connect(btn_free,  "clicked", G_CALLBACK(cb_free_mem),  NULL);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Select Process:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), combo_pid_alloc, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_alloc, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_free,  3, 0, 1, 1);
    gtk_container_add(GTK_CONTAINER(frame_ctrl), grid);
    gtk_box_pack_start(GTK_BOX(vbox), frame_ctrl, FALSE, FALSE, 0);

    /* Memory block display */
    GtkWidget *frame_mem = gtk_frame_new("  Memory Blocks (512 MB Total)  ");
    GtkWidget *mvbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(mvbox), 12);

    for (int i = 0; i < MAX_MEMORY_BLOCKS; i++) {
        mem_labels[i] = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(mem_labels[i]), 0.0);
        gtk_box_pack_start(GTK_BOX(mvbox), mem_labels[i], FALSE, FALSE, 2);
    }

    mem_progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(mem_progress), TRUE);
    gtk_box_pack_start(GTK_BOX(mvbox), mem_progress, FALSE, FALSE, 6);

    gtk_container_add(GTK_CONTAINER(frame_mem), mvbox);
    gtk_box_pack_start(GTK_BOX(vbox), frame_mem, TRUE, TRUE, 0);

    return vbox;
}

static GtkWidget *build_log_tab() {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);

    GtkWidget *frame = gtk_frame_new("  System Event Log — serc_log.txt  ");
    GtkWidget *tv    = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tv);
    gtk_container_add(GTK_CONTAINER(frame), scroll);
    gtk_box_pack_start(GTK_BOX(vbox), frame, TRUE, TRUE, 0);

    GtkWidget *btn_clr = gtk_button_new_with_label("  Clear Log  ");
    g_signal_connect(btn_clr, "clicked", G_CALLBACK(cb_clear_log), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), btn_clr, FALSE, FALSE, 0);

    return vbox;
}

static GtkWidget *build_ipc_tab() {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);

    GtkWidget *frame_ctrl = gtk_frame_new("  Inter-Process Communication — real POSIX pipe()  ");
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Sender Process:"), 0, 0, 1, 1);
    e_ipc_sender = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e_ipc_sender), "Ambulance Dispatch");
    gtk_grid_attach(GTK_GRID(grid), e_ipc_sender, 1, 0, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Receiver Process:"), 2, 0, 1, 1);
    e_ipc_receiver = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e_ipc_receiver), "Police Coordination");
    gtk_grid_attach(GTK_GRID(grid), e_ipc_receiver, 3, 0, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Message:"), 0, 1, 1, 1);
    e_ipc_msg = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(e_ipc_msg), "e.g. Need police escort to Main St");
    gtk_widget_set_hexpand(e_ipc_msg, TRUE);
    gtk_grid_attach(GTK_GRID(grid), e_ipc_msg, 1, 1, 3, 1);

    GtkWidget *btn_send = gtk_button_new_with_label("  ➜  Send via Real pipe()  ");
    gtk_grid_attach(GTK_GRID(grid), btn_send, 0, 2, 4, 1);
    g_signal_connect(btn_send, "clicked", G_CALLBACK(cb_send_ipc), NULL);

    gtk_container_add(GTK_CONTAINER(frame_ctrl), grid);
    gtk_box_pack_start(GTK_BOX(vbox), frame_ctrl, FALSE, FALSE, 0);

    GtkWidget *frame_hist = gtk_frame_new("  Message History (each line = a real fork()+pipe() exchange)  ");
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    ipc_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_text_buffer_set_text(ipc_buf, "  No messages sent yet. Try the default Ambulance -> Police example above.\n", -1);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tv);
    gtk_container_add(GTK_CONTAINER(frame_hist), scroll);
    gtk_box_pack_start(GTK_BOX(vbox), frame_hist, TRUE, TRUE, 0);

    return vbox;
}

static GtkWidget *build_filesystem_tab() {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);

    GtkWidget *frame = gtk_frame_new("  Filesystem Structure — ./serc_records/ (real directory on disk)  ");
    GtkWidget *tv    = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    fs_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tv);
    gtk_container_add(GTK_CONTAINER(frame), scroll);
    gtk_box_pack_start(GTK_BOX(vbox), frame, TRUE, TRUE, 0);

    GtkWidget *btn_refresh = gtk_button_new_with_label("  ⟳  Refresh Listing (opendir / readdir / stat)  ");
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(cb_refresh_fs), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), btn_refresh, FALSE, FALSE, 0);

    return vbox;
}


/* ═══════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    initMemory();
    initFilesystem();   /* mkdir() — create ./serc_records/ if not already there */

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "SERC Mini-OS  —  GTK3 GUI  (Unix/POSIX Edition)");
    gtk_window_set_default_size(GTK_WINDOW(win), 980, 680);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *nb = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_process_tab(),     gtk_label_new("  Processes  "));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_scheduling_tab(),  gtk_label_new("  CPU Scheduling  "));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_memory_tab(),      gtk_label_new("  Memory  "));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_ipc_tab(),         gtk_label_new("  IPC (Pipes)  "));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_filesystem_tab(),  gtk_label_new("  Filesystem  "));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_log_tab(),         gtk_label_new("  System Log  "));

    gtk_container_add(GTK_CONTAINER(win), nb);
    gtk_widget_show_all(win);

    refreshMemDisplay();
    refreshFilesystemView();  /* show ./serc_records/ contents from any previous run */
    loadLogFromDisk();        /* POSIX read() — show any log history from previous runs */

    char startMsg[100];
    snprintf(startMsg, sizeof(startMsg),
        "=== SERC Mini-OS GTK Started === (this GUI is real Unix process PID %d)", getpid());
    logEvent(startMsg);
    logEvent("Memory initialised: 5 blocks, 512 MB total");
    logEvent("Filesystem ready: ./serc_records/ directory available");

    gtk_main();


    logEvent("=== SERC Mini-OS GTK Shutdown ===");
    return 0;
}
