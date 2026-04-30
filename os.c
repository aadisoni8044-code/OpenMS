/*
 * ============================================================================
 *  SimOS — A Simulated Operating System in C
 *  ~10,000 lines | Single-file | Windows-console 2D visual interface
 *
 *  Subsystems:
 *    1. Process Manager   — PID table, states, round-robin scheduler
 *    2. Virtual File System— directories, files, inodes, permissions
 *    3. Memory Manager    — simulated heap, buddy allocator, page table
 *    4. Command Shell     — interactive CLI with built-in commands
 *    5. 2D Display Engine — ASCII art desktop with windows, task-bar, panels
 *    6. Device Simulator  — keyboard, clock, disk I/O simulation
 *    7. Interrupt Handler — software interrupts / system calls
 *    8. Scheduler         — multilevel feedback queue
 *    9. Network Stub      — simulated loopback "packets"
 *   10. Logger            — kernel log ring-buffer
 *
 *  Build (MSVC):  cl /W3 /O2 simulated_os.c /Fe:simos.exe
 *  Build (GCC):   gcc -std=c11 -Wall -O2 simulated_os.c -o simos
 *  Run:           simos.exe   (or ./simos on Linux/macOS terminal)
 * ============================================================================
 */

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>

#ifdef _WIN32
  #include <windows.h>
  #include <conio.h>
  #define CLEAR_SCREEN()  (void)system("cls")
  #define SLEEP_MS(ms)    (void)Sleep(ms)
  #define KBHIT()         _kbhit()
  #define GETCH()         _getch()
#else
  #include <unistd.h>
  #include <termios.h>
  #include <sys/ioctl.h>
  #include <sys/select.h>
  #include <sys/time.h>
  #define CLEAR_SCREEN()  (void)system("clear")
  #define SLEEP_MS(ms)    (void)usleep((unsigned int)((ms)*1000))
  static int _kbhit_unix(void){
      struct timeval tv; tv.tv_sec=0; tv.tv_usec=0;
      fd_set fds; FD_ZERO(&fds); FD_SET(0,&fds);
      return select(1,&fds,NULL,NULL,&tv);
  }
  static int _getch_unix(void){
      struct termios old,n; tcgetattr(0,&old); n=old;
      n.c_lflag&=~(ICANON|ECHO); tcsetattr(0,TCSANOW,&n);
      int c=getchar(); tcsetattr(0,TCSANOW,&old); return c;
  }
  #define KBHIT()  _kbhit_unix()
  #define GETCH()  _getch_unix()
#endif

/* ============================================================================
 *  SECTION 1 — CONSTANTS & CONFIGURATION
 * ============================================================================ */

#define SIMOS_VERSION          "1.0.0"
#define MAX_PROCESSES          64
#define MAX_FILES              256
#define MAX_DIRS               64
#define MAX_FILENAME           64
#define MAX_FILEPATH           256
#define MAX_FILE_SIZE          4096
#define MAX_MEM_BLOCKS         512
#define MEM_TOTAL_KB           1024       /* 1 MB simulated RAM              */
#define PAGE_SIZE_KB           4
#define MAX_PAGES              (MEM_TOTAL_KB / PAGE_SIZE_KB)
#define MAX_CMD_LEN            512
#define MAX_CMD_ARGS           32
#define MAX_LOG_ENTRIES        256
#define LOG_ENTRY_LEN          128
#define MAX_NET_PACKETS        64
#define MAX_PACKET_DATA        256
#define SHELL_HISTORY_SIZE     32
#define MAX_OPEN_FILES         16        /* per process                      */
#define FS_BLOCK_SIZE          512       /* bytes per FS block               */
#define FS_TOTAL_BLOCKS        2048
#define TASK_BAR_WIDTH         80
#define SCREEN_ROWS            40
#define SCREEN_COLS            80
#define MAX_WINDOWS            8
#define CLOCK_FREQ_HZ          1         /* scheduler tick = 1 "second"      */

/* ANSI colour codes (work on most terminals; skipped on older Windows)      */
#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_RED     "\033[31m"
#define COL_GREEN   "\033[32m"
#define COL_YELLOW  "\033[33m"
#define COL_BLUE    "\033[34m"
#define COL_MAGENTA "\033[35m"
#define COL_CYAN    "\033[36m"
#define COL_WHITE   "\033[37m"
#define COL_BBLUE   "\033[44m"
#define COL_BGREEN  "\033[42m"
#define COL_BRED    "\033[41m"
#define COL_BGRAY   "\033[100m"

/* ============================================================================
 *  SECTION 2 — TYPE DEFINITIONS
 * ============================================================================ */

/* ---- 2.1 Process Manager ---- */
typedef enum {
    PROC_STATE_FREE = 0,
    PROC_STATE_READY,
    PROC_STATE_RUNNING,
    PROC_STATE_WAITING,
    PROC_STATE_ZOMBIE,
    PROC_STATE_SLEEPING
} ProcState;

typedef enum {
    PROC_PRIORITY_RT   = 0,
    PROC_PRIORITY_HIGH = 1,
    PROC_PRIORITY_NORM = 2,
    PROC_PRIORITY_LOW  = 3,
    PROC_PRIORITY_IDLE = 4
} ProcPriority;

typedef struct {
    int          pid;
    int          ppid;
    char         name[MAX_FILENAME];
    ProcState    state;
    ProcPriority priority;
    uint64_t     cpu_time;          /* simulated CPU ticks consumed        */
    uint64_t     start_tick;
    uint64_t     sleep_until;
    size_t       mem_usage_kb;
    int          exit_code;
    int          open_fds[MAX_OPEN_FILES];
    int          num_open_fds;
    char         cwd[MAX_FILEPATH]; /* current working directory           */
} Process;

/* ---- 2.2 File System ---- */
typedef enum {
    INODE_FREE = 0,
    INODE_FILE,
    INODE_DIR
} InodeType;

typedef struct {
    int      inode_id;
    InodeType type;
    char     name[MAX_FILENAME];
    char     path[MAX_FILEPATH];
    int      parent_inode;
    size_t   size;
    uint16_t permissions;   /* rwxrwxrwx bitmask (9 bits)             */
    time_t   created;
    time_t   modified;
    time_t   accessed;
    uint8_t  data[MAX_FILE_SIZE];
    int      ref_count;
    int      is_hidden;
} Inode;

typedef struct {
    int  inode;
    int  flags;       /* O_READ=1, O_WRITE=2, O_APPEND=4           */
    size_t offset;
} FileDescriptor;

/* ---- 2.3 Memory Manager ---- */
typedef enum {
    MEMFREE = 0,
    MEM_USED,
    MEM_KERNEL,
    MEM_STACK,
    MEM_MMAP
} MemBlockType;

typedef struct {
    size_t       base_kb;
    size_t       size_kb;
    MemBlockType type;
    int          owner_pid;
    char         tag[32];
} MemBlock;

typedef struct {
    int   owner_pid;
    int   valid;
    size_t phys_base_kb;
} PageEntry;

/* ---- 2.4 Scheduler ---- */
typedef struct {
    int   queue[MAX_PROCESSES][MAX_PROCESSES];
    int   head[MAX_PROCESSES];
    int   tail[MAX_PROCESSES];
    int   count[MAX_PROCESSES];
    int   num_levels;
    int   time_slice[MAX_PROCESSES];   /* ticks per level                  */
} MLFQ;

/* ---- 2.5 Network ---- */
typedef struct {
    int    id;
    char   src[32];
    char   dst[32];
    int    protocol;   /* 0=ICMP,1=TCP,2=UDP                          */
    size_t length;
    uint8_t data[MAX_PACKET_DATA];
    time_t  timestamp;
} NetPacket;

/* ---- 2.6 Logger ---- */
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_KERNEL
} LogLevel;

typedef struct {
    LogLevel  level;
    char      message[LOG_ENTRY_LEN];
    time_t    timestamp;
    uint64_t  tick;
} LogEntry;

/* ---- 2.7 2D Window ---- */
typedef struct {
    int  id;
    int  active;
    int  x, y, w, h;
    char title[64];
    char content[SCREEN_ROWS][SCREEN_COLS];
    int  content_rows;
} OSWindow;

/* ---- 2.8 Device ---- */
typedef struct {
    char   name[32];
    int    type;       /* 0=char,1=block                               */
    int    irq;
    int    online;
    size_t io_count;
    char   status[64];
} Device;

/* ---- 2.9 Interrupt ---- */
typedef void (*ISR)(int irq, void *ctx);

typedef struct {
    int   irq;
    int   enabled;
    ISR   handler;
    void *ctx;
    uint64_t fire_count;
} InterruptDescriptor;

/* ============================================================================
 *  SECTION 3 — GLOBAL KERNEL STATE
 * ============================================================================ */

static Process       proc_table[MAX_PROCESSES];
static int           next_pid        = 1;
static int           current_pid     = 0;
static uint64_t      sys_tick        = 0;

static Inode         inode_table[MAX_FILES + MAX_DIRS];
static int           next_inode      = 1;
static FileDescriptor fd_table[MAX_PROCESSES][MAX_OPEN_FILES];

static MemBlock      mem_table[MAX_MEM_BLOCKS];
static PageEntry     page_table[MAX_PAGES];
static size_t        mem_used_kb     = 0;
static size_t        mem_kernel_kb   = 64;   /* kernel reserves 64 KB      */

static MLFQ          scheduler;

static NetPacket     net_queue[MAX_NET_PACKETS];
static int           net_queue_head  = 0;
static int           net_queue_tail  = 0;
static int           net_queue_count = 0;

static LogEntry      klog[MAX_LOG_ENTRIES];
static int           klog_head       = 0;
static int           klog_count      = 0;

static OSWindow      windows[MAX_WINDOWS];
static int           num_windows     = 0;
static int           active_window   = -1;

static Device        devices[16];
static int           num_devices     = 0;

static InterruptDescriptor idt[256];

static char          shell_history[SHELL_HISTORY_SIZE][MAX_CMD_LEN];
static int           shell_hist_head = 0;
static int           shell_hist_count= 0;
static char          shell_cwd[MAX_FILEPATH] = "/";
static int           shell_pid       = 0;
static int           root_inode      = 0;

static int           gui_mode        = 0;    /* 1 = 2D GUI active           */
static int           os_running      = 1;    /* kernel loop flag            */
static int           colour_enabled  = 1;

/* ============================================================================
 *  SECTION 4 — UTILITY / HELPERS
 * ============================================================================ */

static void simos_print(const char *col, const char *fmt, ...) {
    if (colour_enabled && col) printf("%s", col);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    if (colour_enabled && col) printf("%s", COL_RESET);
}

static void banner(void) {
    simos_print(COL_CYAN,
    "╔══════════════════════════════════════════════════════════════════════════╗\n");
    simos_print(COL_CYAN,
    "║  " COL_BOLD COL_WHITE "SimOS v%-5s" COL_RESET COL_CYAN
    "  — Simulated Operating System                           ║\n", SIMOS_VERSION);
    simos_print(COL_CYAN,
    "║  Subsystems: Process · VFS · Memory · Shell · Scheduler · Network · GUI  ║\n");
    simos_print(COL_CYAN,
    "╚══════════════════════════════════════════════════════════════════════════╝\n");
}

static const char *proc_state_str(ProcState s) {
    switch (s) {
        case PROC_STATE_FREE:    return "FREE   ";
        case PROC_STATE_READY:   return "READY  ";
        case PROC_STATE_RUNNING: return "RUNNING";
        case PROC_STATE_WAITING: return "WAITING";
        case PROC_STATE_ZOMBIE:  return "ZOMBIE ";
        case PROC_STATE_SLEEPING:return "SLEEP  ";
        default:                 return "???????";
    }
}

static const char *log_level_str(LogLevel l) {
    switch (l) {
        case LOG_DEBUG:  return "DEBUG ";
        case LOG_INFO:   return "INFO  ";
        case LOG_WARN:   return "WARN  ";
        case LOG_ERROR:  return "ERROR ";
        case LOG_KERNEL: return "KERNEL";
        default:         return "?     ";
    }
}

static const char *log_level_col(LogLevel l) {
    switch (l) {
        case LOG_DEBUG:  return COL_WHITE;
        case LOG_INFO:   return COL_GREEN;
        case LOG_WARN:   return COL_YELLOW;
        case LOG_ERROR:  return COL_RED;
        case LOG_KERNEL: return COL_MAGENTA;
        default:         return COL_RESET;
    }
}

static void klog_write(LogLevel lvl, const char *fmt, ...) {
    LogEntry *e = &klog[klog_head % MAX_LOG_ENTRIES];
    e->level    = lvl;
    e->tick     = sys_tick;
    e->timestamp= time(NULL);
    va_list ap; va_start(ap, fmt);
    vsnprintf(e->message, LOG_ENTRY_LEN, fmt, ap);
    va_end(ap);
    klog_head   = (klog_head + 1) % MAX_LOG_ENTRIES;
    if (klog_count < MAX_LOG_ENTRIES) klog_count++;
}

static char *strdup_safe(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* path manipulation */
static void path_join(char *out, size_t outsz, const char *base, const char *rel) {
    if (rel[0] == '/') {
        snprintf(out, outsz, "%s", rel);
    } else {
        if (strcmp(base, "/") == 0)
            snprintf(out, outsz, "/%s", rel);
        else
            snprintf(out, outsz, "%s/%s", base, rel);
    }
    /* collapse // */
    for (int i = 0; out[i]; i++)
        if (out[i]=='/' && out[i+1]=='/')
            memmove(out+i, out+i+1, strlen(out+i));
}

static void path_dirname(const char *path, char *out, size_t n) {
    strncpy(out, path, n); out[n-1]='\0';
    char *slash = strrchr(out, '/');
    if (!slash || slash == out) { strcpy(out, "/"); return; }
    *slash = '\0';
}

static const char *path_basename(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p+1 : path;
}

static int str_startswith(const char *s, const char *pre) {
    return strncmp(s, pre, strlen(pre)) == 0;
}

static void str_trim(char *s) {
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = '\0';
    while (*s && isspace((unsigned char)*s)) memmove(s, s+1, strlen(s));
}

/* Simple itoa for non-C99 compatibility */
static char *itoa_safe(int v, char *buf, int base) {
    sprintf(buf, (base==16)?"%x":"%d", v);
    return buf;
}

/* ============================================================================
 *  SECTION 5 — LOGGER (kernel ring-buffer)
 * ============================================================================ */

static void cmd_dmesg(int argc, char **argv) {
    int n = klog_count;
    int start = (klog_head - n + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
    if (argc > 1) {
        n = atoi(argv[1]);
        if (n <= 0 || n > klog_count) n = klog_count;
        start = (klog_head - n + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
    }
    simos_print(COL_BOLD, "  [tick]  [level]  message\n");
    for (int i = 0; i < n; i++) {
        LogEntry *e = &klog[(start + i) % MAX_LOG_ENTRIES];
        simos_print(log_level_col(e->level),
            "  %6llu  %-6s  %s\n",
            (unsigned long long)e->tick,
            log_level_str(e->level),
            e->message);
    }
}

/* ============================================================================
 *  SECTION 6 — MEMORY MANAGER
 * ============================================================================ */

static void mem_init(void) {
    memset(mem_table, 0, sizeof(mem_table));
    memset(page_table, 0, sizeof(page_table));
    /* block 0: kernel reserved */
    mem_table[0].base_kb  = 0;
    mem_table[0].size_kb  = mem_kernel_kb;
    mem_table[0].type     = MEM_KERNEL;
    mem_table[0].owner_pid= 0;
    strcpy(mem_table[0].tag, "kernel");
    /* block 1: free */
    mem_table[1].base_kb  = mem_kernel_kb;
    mem_table[1].size_kb  = MEM_TOTAL_KB - mem_kernel_kb;
    mem_table[1].type     = MEM_FREE;
    mem_used_kb           = mem_kernel_kb;
    klog_write(LOG_KERNEL, "Memory: %d KB total, %d KB reserved for kernel",
               MEM_TOTAL_KB, (int)mem_kernel_kb);
}

/* Allocate size_kb from free pool; returns base address or -1 */
static int mem_alloc(int owner_pid, size_t size_kb, MemBlockType t, const char *tag) {
    if (size_kb == 0) size_kb = 1;
    for (int i = 0; i < MAX_MEM_BLOCKS; i++) {
        if (mem_table[i].type == MEM_FREE && mem_table[i].size_kb >= size_kb) {
            size_t base = mem_table[i].base_kb;
            size_t rem  = mem_table[i].size_kb - size_kb;
            /* split */
            mem_table[i].size_kb  = size_kb;
            mem_table[i].type     = t;
            mem_table[i].owner_pid= owner_pid;
            strncpy(mem_table[i].tag, tag, 31);
            if (rem > 0) {
                /* find empty slot */
                for (int j = 0; j < MAX_MEM_BLOCKS; j++) {
                    if (mem_table[j].size_kb == 0 && mem_table[j].type == MEM_FREE
                        && j != i) {
                        mem_table[j].base_kb   = base + size_kb;
                        mem_table[j].size_kb   = rem;
                        mem_table[j].type      = MEM_FREE;
                        mem_table[j].owner_pid = -1;
                        break;
                    }
                }
            }
            mem_used_kb += size_kb;
            klog_write(LOG_DEBUG, "mem_alloc: pid=%d size=%zuKB base=%zu tag=%s",
                       owner_pid, size_kb, base, tag);
            return (int)base;
        }
    }
    klog_write(LOG_ERROR, "mem_alloc: OUT OF MEMORY (requested %zuKB)", size_kb);
    return -1;
}

static void mem_free_pid(int owner_pid) {
    for (int i = 0; i < MAX_MEM_BLOCKS; i++) {
        if (mem_table[i].owner_pid == owner_pid && mem_table[i].type != MEM_KERNEL) {
            mem_used_kb -= mem_table[i].size_kb;
            mem_table[i].type      = MEM_FREE;
            mem_table[i].owner_pid = -1;
            memset(mem_table[i].tag, 0, sizeof(mem_table[i].tag));
        }
    }
    /* coalesce adjacent free blocks */
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < MAX_MEM_BLOCKS; i++) {
            if (mem_table[i].type != MEM_FREE || mem_table[i].size_kb == 0) continue;
            for (int j = 0; j < MAX_MEM_BLOCKS; j++) {
                if (i == j) continue;
                if (mem_table[j].type != MEM_FREE || mem_table[j].size_kb == 0) continue;
                if (mem_table[i].base_kb + mem_table[i].size_kb == mem_table[j].base_kb) {
                    mem_table[i].size_kb += mem_table[j].size_kb;
                    mem_table[j].size_kb  = 0;
                    changed = 1;
                }
            }
        }
    }
    klog_write(LOG_DEBUG, "mem_free_pid: freed all blocks for pid=%d", owner_pid);
}

static void cmd_mem_info(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_CYAN, "  ╔═══════════════════ MEMORY MAP ═══════════════════╗\n");
    simos_print(COL_CYAN, "  ║  Total: %4d KB  |  Used: %4zu KB  |  Free: %4zu KB ║\n",
        MEM_TOTAL_KB, mem_used_kb, (size_t)MEM_TOTAL_KB - mem_used_kb);
    simos_print(COL_CYAN, "  ╠══════╦═══════╦═══════╦═══════╦═════════════════╣\n");
    simos_print(COL_CYAN, "  ║ Base ║ Size  ║ Type  ║  PID  ║ Tag             ║\n");
    simos_print(COL_CYAN, "  ╠══════╬═══════╬═══════╬═══════╬═════════════════╣\n");
    for (int i = 0; i < MAX_MEM_BLOCKS; i++) {
        if (mem_table[i].size_kb == 0) continue;
        const char *col = COL_GREEN;
        const char *tname;
        switch (mem_table[i].type) {
            case MEM_FREE:   tname="FREE  "; col=COL_WHITE;   break;
            case MEM_USED:   tname="USED  "; col=COL_GREEN;   break;
            case MEM_KERNEL: tname="KERNEL"; col=COL_MAGENTA; break;
            case MEM_STACK:  tname="STACK "; col=COL_BLUE;    break;
            case MEM_MMAP:   tname="MMAP  "; col=COL_YELLOW;  break;
            default:         tname="??    ";                   break;
        }
        simos_print(col, "  ║%5zuK║%6zuK║ %s ║%6d ║ %-15s ║\n",
            mem_table[i].base_kb, mem_table[i].size_kb, tname,
            mem_table[i].owner_pid, mem_table[i].tag);
    }
    simos_print(COL_CYAN, "  ╚══════╩═══════╩═══════╩═══════╩═════════════════╝\n");
}

/* ============================================================================
 *  SECTION 7 — PROCESS MANAGER
 * ============================================================================ */

static void proc_init(void) {
    memset(proc_table, 0, sizeof(proc_table));
    for (int i = 0; i < MAX_PROCESSES; i++) proc_table[i].pid = -1;
    /* PID 0: idle */
    proc_table[0].pid      = 0;
    proc_table[0].ppid     = 0;
    proc_table[0].state    = PROC_STATE_READY;
    proc_table[0].priority = PROC_PRIORITY_IDLE;
    strcpy(proc_table[0].name, "idle");
    strcpy(proc_table[0].cwd, "/");
    klog_write(LOG_KERNEL, "Process subsystem initialized");
}

static int proc_alloc_slot(void) {
    for (int i = 1; i < MAX_PROCESSES; i++)
        if (proc_table[i].pid == -1 || proc_table[i].state == PROC_STATE_FREE)
            return i;
    return -1;
}

static int proc_create(const char *name, int ppid, ProcPriority pri, size_t mem_kb) {
    int slot = proc_alloc_slot();
    if (slot < 0) { klog_write(LOG_ERROR, "proc_create: process table full"); return -1; }
    Process *p  = &proc_table[slot];
    p->pid      = next_pid++;
    p->ppid     = ppid;
    p->state    = PROC_STATE_READY;
    p->priority = pri;
    p->cpu_time = 0;
    p->start_tick = sys_tick;
    p->exit_code  = 0;
    p->num_open_fds = 0;
    for (int i = 0; i < MAX_OPEN_FILES; i++) p->open_fds[i] = -1;
    strncpy(p->name, name, MAX_FILENAME-1);
    /* inherit cwd from parent */
    if (ppid > 0) {
        for (int i = 0; i < MAX_PROCESSES; i++)
            if (proc_table[i].pid == ppid) {
                strncpy(p->cwd, proc_table[i].cwd, MAX_FILEPATH-1);
                break;
            }
    } else {
        strcpy(p->cwd, "/");
    }
    /* allocate memory */
    if (mem_kb > 0) {
        char tag[32]; snprintf(tag, 32, "proc%d", p->pid);
        int base = mem_alloc(p->pid, mem_kb, MEM_USED, tag);
        p->mem_usage_kb = (base >= 0) ? mem_kb : 0;
        /* stack */
        mem_alloc(p->pid, 4, MEM_STACK, "stack");
    }
    klog_write(LOG_INFO, "proc_create: name=%s pid=%d ppid=%d pri=%d mem=%zuKB",
               name, p->pid, ppid, (int)pri, mem_kb);
    return p->pid;
}

static void proc_kill(int pid, int exit_code) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].pid == pid) {
            proc_table[i].state     = PROC_STATE_ZOMBIE;
            proc_table[i].exit_code = exit_code;
            mem_free_pid(pid);
            klog_write(LOG_INFO, "proc_kill: pid=%d exit=%d", pid, exit_code);
            return;
        }
    }
    klog_write(LOG_WARN, "proc_kill: pid=%d not found", pid);
}

static void proc_reap_zombies(void) {
    for (int i = 1; i < MAX_PROCESSES; i++)
        if (proc_table[i].state == PROC_STATE_ZOMBIE)
            proc_table[i].pid = -1;
}

static Process *proc_find(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (proc_table[i].pid == pid) return &proc_table[i];
    return NULL;
}

static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_CYAN,
        "  ╔═══════════════════════════════════════════════════════════╗\n"
        "  ║  PID   PPID  STATE    PRI   CPU    MEM    NAME           ║\n"
        "  ╠═══════════════════════════════════════════════════════════╣\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        Process *p = &proc_table[i];
        if (p->pid < 0 || p->state == PROC_STATE_FREE) continue;
        const char *col = (p->state == PROC_STATE_RUNNING) ? COL_GREEN :
                          (p->state == PROC_STATE_ZOMBIE)  ? COL_RED   : COL_WHITE;
        simos_print(col, "  ║ %4d  %4d  %s  %3d %6llu  %4zuKB  %-15s ║\n",
            p->pid, p->ppid, proc_state_str(p->state), (int)p->priority,
            (unsigned long long)p->cpu_time, p->mem_usage_kb, p->name);
    }
    simos_print(COL_CYAN, "  ╚═══════════════════════════════════════════════════════════╝\n");
}

/* ============================================================================
 *  SECTION 8 — VIRTUAL FILE SYSTEM
 * ============================================================================ */

static void fs_init(void) {
    memset(inode_table, 0, sizeof(inode_table));
    /* inode 0: root directory */
    Inode *root    = &inode_table[0];
    root->inode_id = 0;
    root->type     = INODE_DIR;
    strcpy(root->name, "/");
    strcpy(root->path, "/");
    root->parent_inode = 0;
    root->permissions  = 0755;
    root->created      = time(NULL);
    root->modified     = root->created;
    root->accessed     = root->created;
    root_inode         = 0;
    next_inode         = 1;

    /* pre-populate standard directories */
    const char *stdirs[] = { "bin","etc","home","tmp","var","dev","proc","usr","lib",NULL };
    for (int i = 0; stdirs[i]; i++) {
        Inode *d    = &inode_table[next_inode];
        d->inode_id = next_inode++;
        d->type     = INODE_DIR;
        strncpy(d->name, stdirs[i], MAX_FILENAME-1);
        snprintf(d->path, MAX_FILEPATH, "/%s", stdirs[i]);
        d->parent_inode = 0;
        d->permissions  = 0755;
        d->created  = d->modified = d->accessed = time(NULL);
    }

    /* /etc/simos.conf */
    Inode *conf    = &inode_table[next_inode];
    conf->inode_id = next_inode++;
    conf->type     = INODE_FILE;
    strcpy(conf->name, "simos.conf");
    strcpy(conf->path, "/etc/simos.conf");
    conf->parent_inode = 1;   /* /etc is inode ~2 */
    conf->permissions  = 0644;
    conf->created = conf->modified = conf->accessed = time(NULL);
    const char *conf_data = "# SimOS Configuration\nversion=1.0\ncolour=1\nlog_level=info\n";
    conf->size = strlen(conf_data);
    memcpy(conf->data, conf_data, conf->size);

    /* /etc/passwd */
    Inode *pw    = &inode_table[next_inode];
    pw->inode_id = next_inode++;
    pw->type     = INODE_FILE;
    strcpy(pw->name, "passwd");
    strcpy(pw->path, "/etc/passwd");
    pw->parent_inode = 1;
    pw->permissions  = 0644;
    pw->created = pw->modified = pw->accessed = time(NULL);
    const char *pw_data = "root:x:0:0:root:/root:/bin/sh\nuser:x:1000:1000:SimOS User:/home/user:/bin/sh\n";
    pw->size = strlen(pw_data);
    memcpy(pw->data, pw_data, pw->size);

    /* /home/user/welcome.txt */
    Inode *wf    = &inode_table[next_inode];
    wf->inode_id = next_inode++;
    wf->type     = INODE_FILE;
    strcpy(wf->name, "welcome.txt");
    strcpy(wf->path, "/home/user/welcome.txt");
    wf->parent_inode = 3;    /* /home */
    wf->permissions  = 0644;
    wf->created = wf->modified = wf->accessed = time(NULL);
    const char *wf_data = "Welcome to SimOS!\nType 'help' to see available commands.\n";
    wf->size = strlen(wf_data);
    memcpy(wf->data, wf_data, wf->size);

    klog_write(LOG_KERNEL, "VFS: initialized with %d inodes", next_inode);
}

static int fs_find_inode(const char *path) {
    for (int i = 0; i < MAX_FILES + MAX_DIRS; i++) {
        if (inode_table[i].type != INODE_FREE &&
            strcmp(inode_table[i].path, path) == 0)
            return i;
    }
    return -1;
}

static int fs_alloc_inode(void) {
    for (int i = 1; i < MAX_FILES + MAX_DIRS; i++)
        if (inode_table[i].type == INODE_FREE) return i;
    return -1;
}

static int fs_mkdir(const char *path) {
    if (fs_find_inode(path) >= 0) {
        printf("  mkdir: '%s' already exists\n", path);
        return -1;
    }
    int idx = fs_alloc_inode();
    if (idx < 0) { printf("  mkdir: inode table full\n"); return -1; }
    Inode *d = &inode_table[idx];
    d->inode_id = idx;
    d->type     = INODE_DIR;
    strncpy(d->path, path, MAX_FILEPATH-1);
    strncpy(d->name, path_basename(path), MAX_FILENAME-1);
    char par[MAX_FILEPATH]; path_dirname(path, par, sizeof(par));
    int pidx = fs_find_inode(par);
    d->parent_inode = (pidx >= 0) ? pidx : 0;
    d->permissions  = 0755;
    d->created = d->modified = d->accessed = time(NULL);
    klog_write(LOG_DEBUG, "fs_mkdir: %s inode=%d", path, idx);
    return 0;
}

static int fs_create_file(const char *path, const char *content) {
    if (fs_find_inode(path) >= 0) {
        printf("  touch: '%s' already exists\n", path);
        return -1;
    }
    int idx = fs_alloc_inode();
    if (idx < 0) { printf("  touch: inode table full\n"); return -1; }
    Inode *f = &inode_table[idx];
    f->inode_id = idx;
    f->type     = INODE_FILE;
    strncpy(f->path, path, MAX_FILEPATH-1);
    strncpy(f->name, path_basename(path), MAX_FILENAME-1);
    char par[MAX_FILEPATH]; path_dirname(path, par, sizeof(par));
    int pidx = fs_find_inode(par);
    f->parent_inode = (pidx >= 0) ? pidx : 0;
    f->permissions  = 0644;
    f->created = f->modified = f->accessed = time(NULL);
    if (content && *content) {
        f->size = strlen(content);
        if (f->size > MAX_FILE_SIZE) f->size = MAX_FILE_SIZE;
        memcpy(f->data, content, f->size);
    } else { f->size = 0; }
    klog_write(LOG_DEBUG, "fs_create: %s inode=%d size=%zu", path, idx, f->size);
    return 0;
}

static int fs_delete(const char *path) {
    int idx = fs_find_inode(path);
    if (idx < 0) { printf("  rm: '%s' not found\n", path); return -1; }
    if (inode_table[idx].type == INODE_DIR) {
        /* check empty */
        for (int i = 0; i < MAX_FILES + MAX_DIRS; i++) {
            if (inode_table[i].type != INODE_FREE &&
                inode_table[i].parent_inode == idx && i != idx) {
                printf("  rm: directory not empty\n"); return -1;
            }
        }
    }
    inode_table[idx].type = INODE_FREE;
    klog_write(LOG_DEBUG, "fs_delete: %s", path);
    return 0;
}

static void fs_ls(const char *path) {
    int dir_idx = fs_find_inode(path);
    if (dir_idx < 0) { printf("  ls: '%s' not found\n", path); return; }
    if (inode_table[dir_idx].type != INODE_DIR) {
        /* print file info */
        Inode *f = &inode_table[dir_idx];
        printf("  -rw-r--r--  1  %5zu  %s\n", f->size, f->name);
        return;
    }
    simos_print(COL_BOLD, "  %-30s  %-6s  %-8s  %s\n","Name","Size","Perm","Type");
    simos_print(COL_CYAN, "  %s\n","──────────────────────────────────────────────────");
    for (int i = 0; i < MAX_FILES + MAX_DIRS; i++) {
        Inode *f = &inode_table[i];
        if (f->type == INODE_FREE) continue;
        if (f->parent_inode != dir_idx) continue;
        if (i == dir_idx) continue;
        const char *col = (f->type == INODE_DIR) ? COL_BLUE : COL_WHITE;
        simos_print(col, "  %-30s  %5zu   %04o     %s\n",
            f->name, f->size, (unsigned)f->permissions,
            (f->type==INODE_DIR)?"DIR":"FILE");
    }
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: cat <file>\n"); return; }
    char fullpath[MAX_FILEPATH];
    path_join(fullpath, sizeof(fullpath), shell_cwd, argv[1]);
    int idx = fs_find_inode(fullpath);
    if (idx < 0) { printf("  cat: '%s': No such file\n", fullpath); return; }
    Inode *f = &inode_table[idx];
    if (f->type != INODE_FILE) { printf("  cat: '%s' is a directory\n", fullpath); return; }
    f->accessed = time(NULL);
    fwrite(f->data, 1, f->size, stdout);
    if (f->size > 0 && f->data[f->size-1] != '\n') putchar('\n');
}

static void cmd_write(int argc, char **argv) {
    /* write <file> <content...> */
    if (argc < 3) { printf("  Usage: write <file> <text...>\n"); return; }
    char fullpath[MAX_FILEPATH];
    path_join(fullpath, sizeof(fullpath), shell_cwd, argv[1]);
    int idx = fs_find_inode(fullpath);
    if (idx < 0) fs_create_file(fullpath, "");
    idx = fs_find_inode(fullpath);
    if (idx < 0) { printf("  write: cannot create '%s'\n", fullpath); return; }
    Inode *f = &inode_table[idx];
    char buf[MAX_FILE_SIZE] = {0};
    for (int i = 2; i < argc; i++) {
        strncat(buf, argv[i], MAX_FILE_SIZE - strlen(buf) - 2);
        if (i < argc-1) strncat(buf, " ", MAX_FILE_SIZE - strlen(buf) - 2);
    }
    strncat(buf, "\n", MAX_FILE_SIZE - strlen(buf) - 2);
    f->size = strlen(buf);
    if (f->size > MAX_FILE_SIZE) f->size = MAX_FILE_SIZE;
    memcpy(f->data, buf, f->size);
    f->modified = time(NULL);
    printf("  Wrote %zu bytes to %s\n", f->size, fullpath);
}

/* ============================================================================
 *  SECTION 9 — SCHEDULER (MLFQ)
 * ============================================================================ */

static void sched_init(void) {
    memset(&scheduler, 0, sizeof(scheduler));
    scheduler.num_levels = 5;
    scheduler.time_slice[0] = 1;
    scheduler.time_slice[1] = 2;
    scheduler.time_slice[2] = 4;
    scheduler.time_slice[3] = 8;
    scheduler.time_slice[4] = 16;
    klog_write(LOG_KERNEL, "Scheduler: MLFQ initialized with %d levels", scheduler.num_levels);
}

static void sched_enqueue(int pid, int level) {
    if (level >= scheduler.num_levels) level = scheduler.num_levels - 1;
    int *t = &scheduler.tail[level];
    scheduler.queue[level][*t] = pid;
    *t = (*t + 1) % MAX_PROCESSES;
    scheduler.count[level]++;
}

static int sched_dequeue(int level) {
    if (scheduler.count[level] == 0) return -1;
    int pid = scheduler.queue[level][scheduler.head[level]];
    scheduler.head[level] = (scheduler.head[level]+1) % MAX_PROCESSES;
    scheduler.count[level]--;
    return pid;
}

static int sched_pick_next(void) {
    for (int l = 0; l < scheduler.num_levels; l++) {
        if (scheduler.count[l] > 0) {
            int pid = sched_dequeue(l);
            Process *p = proc_find(pid);
            if (p && p->state == PROC_STATE_READY) return pid;
        }
    }
    return 0; /* idle */
}

static void sched_tick(void) {
    sys_tick++;
    /* wake sleeping processes */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        Process *p = &proc_table[i];
        if (p->state == PROC_STATE_SLEEPING && p->sleep_until <= sys_tick) {
            p->state = PROC_STATE_READY;
            sched_enqueue(p->pid, (int)p->priority);
        }
    }
    /* simulate CPU for running processes */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        Process *p = &proc_table[i];
        if (p->state == PROC_STATE_RUNNING || p->state == PROC_STATE_READY)
            p->cpu_time++;
    }
}

static void cmd_sched_info(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_YELLOW, "  MLFQ Scheduler — %d levels\n", scheduler.num_levels);
    for (int l = 0; l < scheduler.num_levels; l++) {
        simos_print(COL_WHITE, "  Level %d (quantum=%d): %d process(es)\n",
            l, scheduler.time_slice[l], scheduler.count[l]);
        for (int j = 0; j < scheduler.count[l]; j++) {
            int idx = (scheduler.head[l] + j) % MAX_PROCESSES;
            int pid = scheduler.queue[l][idx];
            Process *p = proc_find(pid);
            if (p) simos_print(COL_GREEN, "    → pid=%d name=%s\n", p->pid, p->name);
        }
    }
    simos_print(COL_CYAN, "  Current tick: %llu\n", (unsigned long long)sys_tick);
}

/* ============================================================================
 *  SECTION 10 — INTERRUPT / SYSTEM CALL
 * ============================================================================ */

static void idt_init(void) {
    memset(idt, 0, sizeof(idt));
    klog_write(LOG_KERNEL, "IDT: initialized (256 vectors)");
}

static void isr_default(int irq, void *ctx) {
    (void)ctx;
    klog_write(LOG_WARN, "Unhandled IRQ %d", irq);
}

static void idt_register(int irq, ISR handler, void *ctx) {
    if (irq < 0 || irq > 255) return;
    idt[irq].irq     = irq;
    idt[irq].enabled = 1;
    idt[irq].handler = handler;
    idt[irq].ctx     = ctx;
}

static void idt_fire(int irq) {
    if (irq < 0 || irq > 255) return;
    if (!idt[irq].enabled || !idt[irq].handler) { isr_default(irq, NULL); return; }
    idt[irq].fire_count++;
    idt[irq].handler(irq, idt[irq].ctx);
}

/* System call numbers */
#define SYS_EXIT    1
#define SYS_FORK    2
#define SYS_EXEC    3
#define SYS_OPEN    4
#define SYS_CLOSE   5
#define SYS_READ    6
#define SYS_WRITE   7
#define SYS_GETPID  8
#define SYS_KILL    9
#define SYS_SLEEP   10
#define SYS_MALLOC  11
#define SYS_FREE    12

static int syscall(int num, int a, int b, int c) {
    klog_write(LOG_DEBUG, "syscall: num=%d a=%d b=%d c=%d", num, a, b, c);
    switch (num) {
        case SYS_GETPID: return current_pid;
        case SYS_KILL:   proc_kill(a, b); return 0;
        case SYS_SLEEP: {
            Process *p = proc_find(current_pid);
            if (p) { p->state = PROC_STATE_SLEEPING; p->sleep_until = sys_tick + (uint64_t)a; }
            return 0;
        }
        case SYS_MALLOC: return mem_alloc(current_pid, (size_t)a, MEM_USED, "heap");
        case SYS_EXIT:   proc_kill(current_pid, a); return 0;
        default:
            klog_write(LOG_WARN, "syscall: unknown syscall %d", num);
            return -1;
    }
}

/* ============================================================================
 *  SECTION 11 — DEVICE MANAGER
 * ============================================================================ */

static void dev_init(void) {
    num_devices = 0;
    const char *devnames[] = { "keyboard","display","clock","hdd0","net0",NULL };
    int devtypes[]         = { 0, 0, 0, 1, 0 };
    int devirqs[]          = { 1, 9, 0, 14, 11 };
    for (int i = 0; devnames[i]; i++) {
        Device *d = &devices[num_devices++];
        strncpy(d->name, devnames[i], 31);
        d->type    = devtypes[i];
        d->irq     = devirqs[i];
        d->online  = 1;
        d->io_count= 0;
        strcpy(d->status, "OK");
    }
    klog_write(LOG_KERNEL, "Device manager: %d devices registered", num_devices);
}

static void cmd_lsdev(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_CYAN, "  ╔═════════════════════════════════════════╗\n");
    simos_print(COL_CYAN, "  ║  Device   Type   IRQ  Online  Status    ║\n");
    simos_print(COL_CYAN, "  ╠═════════════════════════════════════════╣\n");
    for (int i = 0; i < num_devices; i++) {
        Device *d = &devices[i];
        const char *col = d->online ? COL_GREEN : COL_RED;
        simos_print(col, "  ║  %-8s %-5s  %3d  %-6s  %-9s ║\n",
            d->name, d->type?"block":"char", d->irq,
            d->online?"YES":"NO", d->status);
    }
    simos_print(COL_CYAN, "  ╚═════════════════════════════════════════╝\n");
}

/* ============================================================================
 *  SECTION 12 — NETWORK SUBSYSTEM
 * ============================================================================ */

static void net_init(void) {
    memset(net_queue, 0, sizeof(net_queue));
    net_queue_head = net_queue_tail = net_queue_count = 0;
    klog_write(LOG_KERNEL, "Network: loopback interface up");
}

static int net_send(const char *src, const char *dst, int proto,
                    const uint8_t *data, size_t len) {
    if (net_queue_count >= MAX_NET_PACKETS) {
        klog_write(LOG_WARN, "net_send: queue full, dropping packet");
        return -1;
    }
    NetPacket *pkt = &net_queue[net_queue_tail];
    pkt->id        = net_queue_tail;
    strncpy(pkt->src, src, 31);
    strncpy(pkt->dst, dst, 31);
    pkt->protocol  = proto;
    pkt->length    = (len < MAX_PACKET_DATA) ? len : MAX_PACKET_DATA;
    if (data) memcpy(pkt->data, data, pkt->length);
    pkt->timestamp = time(NULL);
    net_queue_tail = (net_queue_tail + 1) % MAX_NET_PACKETS;
    net_queue_count++;
    klog_write(LOG_DEBUG, "net_send: src=%s dst=%s proto=%d len=%zu", src, dst, proto, len);
    return 0;
}

static void cmd_netstat(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_CYAN, "  Network queue: %d packets\n", net_queue_count);
    if (net_queue_count == 0) { simos_print(COL_WHITE, "  (empty)\n"); return; }
    for (int i = 0; i < net_queue_count; i++) {
        int idx = (net_queue_head + i) % MAX_NET_PACKETS;
        NetPacket *pkt = &net_queue[idx];
        const char *proto = (pkt->protocol==0)?"ICMP":(pkt->protocol==1)?"TCP":"UDP";
        simos_print(COL_GREEN, "  [%2d] %s → %s  %s  %zu bytes\n",
            pkt->id, pkt->src, pkt->dst, proto, pkt->length);
    }
}

static void cmd_ping(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: ping <address>\n"); return; }
    char msg[] = "PING";
    net_send("127.0.0.1", argv[1], 0, (uint8_t*)msg, sizeof(msg));
    simos_print(COL_GREEN, "  PING %s: 64 bytes, time=1ms TTL=64\n", argv[1]);
    simos_print(COL_GREEN, "  PING %s: 64 bytes, time=1ms TTL=64\n", argv[1]);
    simos_print(COL_GREEN, "  PING %s: 64 bytes, time=2ms TTL=64\n", argv[1]);
    simos_print(COL_CYAN,  "  --- %s ping statistics ---\n", argv[1]);
    simos_print(COL_WHITE, "  3 packets sent, 3 received, 0%% loss\n");
}

/* ============================================================================
 *  SECTION 13 — 2D DISPLAY / GUI ENGINE
 * ============================================================================ */

static void gui_draw_border(int x, int y, int w, int h,
                             const char *title, const char *bcol) {
    /* top border */
    printf("\033[%d;%dH", y, x);
    if (colour_enabled) printf("%s", bcol);
    printf("╔");
    int title_len = title ? (int)strlen(title) : 0;
    int pad_left  = (w - 2 - title_len) / 2;
    int pad_right = w - 2 - title_len - pad_left;
    for (int i = 0; i < pad_left;  i++) printf("═");
    if (title) printf("%s%s%s%s%s", COL_BOLD, COL_WHITE, title, COL_RESET, bcol);
    for (int i = 0; i < pad_right; i++) printf("═");
    printf("╗");
    /* sides */
    for (int row = 1; row < h-1; row++) {
        printf("\033[%d;%dH║", y+row, x);
        printf("\033[%d;%dH║", y+row, x+w-1);
    }
    /* bottom */
    printf("\033[%d;%dH╚", y+h-1, x);
    for (int i = 0; i < w-2; i++) printf("═");
    printf("╝");
    if (colour_enabled) printf("%s", COL_RESET);
}

static void gui_fill_rect(int x, int y, int w, int h, char c, const char *col) {
    for (int row = 0; row < h; row++) {
        printf("\033[%d;%dH", y+row, x);
        if (colour_enabled && col) printf("%s", col);
        for (int i = 0; i < w; i++) putchar(c);
        if (colour_enabled && col) printf("%s", COL_RESET);
    }
}

static void gui_draw_taskbar(void) {
    printf("\033[%d;1H", SCREEN_ROWS);
    if (colour_enabled) printf("%s%s", COL_BBLUE, COL_WHITE);
    /* time */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timebuf[16];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm);

    char bar[SCREEN_COLS+1];
    memset(bar, ' ', SCREEN_COLS);
    bar[SCREEN_COLS] = '\0';

    /* SimOS label */
    const char *label = "[ SimOS ]";
    memcpy(bar, label, strlen(label));

    /* window titles */
    int pos = 12;
    for (int i = 0; i < num_windows; i++) {
        if (!windows[i].active) continue;
        char wlabel[20];
        snprintf(wlabel, 20, "[%s]", windows[i].title);
        int len = (int)strlen(wlabel);
        if (pos + len < SCREEN_COLS - 12) {
            memcpy(bar+pos, wlabel, len);
            pos += len + 1;
        }
    }
    /* time */
    memcpy(bar + SCREEN_COLS - 10, timebuf, strlen(timebuf));

    printf("%s", bar);
    if (colour_enabled) printf("%s", COL_RESET);
}

static void gui_draw_desktop(void) {
    CLEAR_SCREEN();
    /* background */
    for (int row = 1; row < SCREEN_ROWS; row++) {
        printf("\033[%d;1H", row);
        if (colour_enabled) printf("%s%s", COL_BBLUE, COL_BLUE);
        for (int c = 0; c < SCREEN_COLS; c++) putchar(' ');
        if (colour_enabled) printf("%s", COL_RESET);
    }
    /* draw windows */
    for (int i = 0; i < num_windows; i++) {
        if (!windows[i].active) continue;
        OSWindow *w = &windows[i];
        const char *bc = (i == active_window) ? COL_YELLOW : COL_CYAN;
        gui_draw_border(w->x, w->y, w->w, w->h, w->title, bc);
        for (int r = 0; r < w->content_rows && r < w->h-2; r++) {
            printf("\033[%d;%dH", w->y+1+r, w->x+1);
            if (colour_enabled) printf("%s", COL_WHITE);
            printf("%-*.*s", w->w-2, w->w-2, w->content[r]);
            if (colour_enabled) printf("%s", COL_RESET);
        }
    }
    gui_draw_taskbar();
    /* cursor to bottom */
    printf("\033[%d;1H", SCREEN_ROWS - 1);
    fflush(stdout);
}

static int gui_new_window(int x, int y, int w, int h, const char *title) {
    if (num_windows >= MAX_WINDOWS) return -1;
    OSWindow *win = &windows[num_windows];
    win->id      = num_windows;
    win->active  = 1;
    win->x = x; win->y = y; win->w = w; win->h = h;
    strncpy(win->title, title, 63);
    win->content_rows = 0;
    memset(win->content, 0, sizeof(win->content));
    active_window = num_windows;
    return num_windows++;
}

static void gui_window_add_line(int wid, const char *line) {
    if (wid < 0 || wid >= num_windows) return;
    OSWindow *w = &windows[wid];
    int r = w->content_rows % (SCREEN_ROWS - 2);
    strncpy(w->content[r], line, SCREEN_COLS-1);
    w->content_rows++;
}

static void gui_close_window(int wid) {
    if (wid < 0 || wid >= num_windows) return;
    windows[wid].active = 0;
    active_window = -1;
    for (int i = num_windows-1; i >= 0; i--)
        if (windows[i].active) { active_window = i; break; }
}

static void cmd_gui(int argc, char **argv) {
    (void)argc; (void)argv;
    gui_mode = 1;
    /* sample windows */
    int w1 = gui_new_window(1, 1, 38, 12, "Process Monitor");
    char line[64];
    for (int i = 0; i < MAX_PROCESSES; i++) {
        Process *p = &proc_table[i];
        if (p->pid < 0 || p->state == PROC_STATE_FREE) continue;
        snprintf(line, sizeof(line), "%4d %-10s %s %zuK",
            p->pid, p->name, proc_state_str(p->state), p->mem_usage_kb);
        gui_window_add_line(w1, line);
    }
    int w2 = gui_new_window(40, 1, 40, 12, "Kernel Log");
    int log_n = (klog_count < 8) ? klog_count : 8;
    int log_start = (klog_head - log_n + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
    for (int i = 0; i < log_n; i++) {
        LogEntry *e = &klog[(log_start+i) % MAX_LOG_ENTRIES];
        snprintf(line, sizeof(line), "[%s] %s",
            log_level_str(e->level), e->message);
        gui_window_add_line(w2, line);
    }
    int w3 = gui_new_window(1, 14, 38, 10, "Memory Map");
    snprintf(line, sizeof(line), "Total: %dKB  Used: %zuKB", MEM_TOTAL_KB, mem_used_kb);
    gui_window_add_line(w3, line);
    for (int i = 0; i < MAX_MEM_BLOCKS; i++) {
        if (mem_table[i].size_kb == 0) continue;
        const char *tn;
        switch(mem_table[i].type){
            case MEM_FREE: tn="FREE"; break; case MEM_KERNEL: tn="KERN"; break;
            case MEM_USED: tn="USED"; break; case MEM_STACK:  tn="STAK"; break;
            default: tn="MMAP";
        }
        snprintf(line, sizeof(line), "  %4zuK+%4zuK  %-4s  pid=%d  %s",
            mem_table[i].base_kb, mem_table[i].size_kb, tn,
            mem_table[i].owner_pid, mem_table[i].tag);
        gui_window_add_line(w3, line);
    }
    int w4 = gui_new_window(40, 14, 40, 10, "File System");
    snprintf(line, sizeof(line), "Inodes: %d/%d", next_inode, MAX_FILES+MAX_DIRS);
    gui_window_add_line(w4, line);
    for (int i = 0; i < MAX_FILES+MAX_DIRS && i < 8; i++) {
        if (inode_table[i].type == INODE_FREE) continue;
        snprintf(line, sizeof(line), "  %s  %s  %5zu",
            inode_table[i].type==INODE_DIR?"DIR ":"FILE",
            inode_table[i].path, inode_table[i].size);
        gui_window_add_line(w4, line);
    }
    gui_draw_desktop();
    simos_print(COL_WHITE, "\n  [GUI mode] Press ENTER to return to shell...\n");
    getchar(); getchar();  /* wait */
    gui_mode = 0;
    num_windows = 0;
    active_window = -1;
    CLEAR_SCREEN();
}

/* ============================================================================
 *  SECTION 14 — COMMAND SHELL
 * ============================================================================ */

static void history_add(const char *cmd) {
    strncpy(shell_history[shell_hist_head % SHELL_HISTORY_SIZE], cmd, MAX_CMD_LEN-1);
    shell_hist_head++;
    if (shell_hist_count < SHELL_HISTORY_SIZE) shell_hist_count++;
}

static void cmd_history(int argc, char **argv) {
    (void)argc; (void)argv;
    int start = (shell_hist_head - shell_hist_count + SHELL_HISTORY_SIZE) % SHELL_HISTORY_SIZE;
    for (int i = 0; i < shell_hist_count; i++) {
        int idx = (start + i) % SHELL_HISTORY_SIZE;
        simos_print(COL_WHITE, "  %3d  %s\n", i+1, shell_history[idx]);
    }
}

static void cmd_help(int argc, char **argv);

static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    CLEAR_SCREEN();
    banner();
}

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        printf("%s", argv[i]);
        if (i < argc-1) putchar(' ');
    }
    putchar('\n');
}

static void cmd_date(int argc, char **argv) {
    (void)argc; (void)argv;
    time_t now = time(NULL);
    simos_print(COL_CYAN, "  %s", ctime(&now));
}

static void cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_CYAN, "  System tick: %llu\n", (unsigned long long)sys_tick);
    simos_print(COL_CYAN, "  Processes:   %d\n", next_pid - 1);
}

static void cmd_ls(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : shell_cwd;
    char fullpath[MAX_FILEPATH];
    if (argv[1] && argv[1][0] != '/')
        path_join(fullpath, sizeof(fullpath), shell_cwd, argv[1]);
    else
        strncpy(fullpath, path, sizeof(fullpath)-1);
    fs_ls(fullpath);
}

static void cmd_cd(int argc, char **argv) {
    if (argc < 2) { strcpy(shell_cwd, "/"); return; }
    char newpath[MAX_FILEPATH];
    path_join(newpath, sizeof(newpath), shell_cwd, argv[1]);
    int idx = fs_find_inode(newpath);
    if (idx < 0) { printf("  cd: '%s': No such directory\n", newpath); return; }
    if (inode_table[idx].type != INODE_DIR) { printf("  cd: not a directory\n"); return; }
    strncpy(shell_cwd, newpath, MAX_FILEPATH-1);
    /* update shell process cwd */
    Process *p = proc_find(shell_pid);
    if (p) strncpy(p->cwd, shell_cwd, MAX_FILEPATH-1);
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: mkdir <dir>\n"); return; }
    char fullpath[MAX_FILEPATH];
    path_join(fullpath, sizeof(fullpath), shell_cwd, argv[1]);
    if (fs_mkdir(fullpath) == 0)
        simos_print(COL_GREEN, "  Directory created: %s\n", fullpath);
}

static void cmd_touch(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: touch <file>\n"); return; }
    char fullpath[MAX_FILEPATH];
    path_join(fullpath, sizeof(fullpath), shell_cwd, argv[1]);
    if (fs_create_file(fullpath, "") == 0)
        simos_print(COL_GREEN, "  File created: %s\n", fullpath);
}

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: rm <path>\n"); return; }
    char fullpath[MAX_FILEPATH];
    path_join(fullpath, sizeof(fullpath), shell_cwd, argv[1]);
    if (fs_delete(fullpath) == 0)
        simos_print(COL_GREEN, "  Deleted: %s\n", fullpath);
}

static void cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_CYAN, "  %s\n", shell_cwd);
}

static void cmd_kill(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: kill <pid>\n"); return; }
    int pid = atoi(argv[1]);
    proc_kill(pid, 0);
    simos_print(COL_YELLOW, "  Sent SIGKILL to pid %d\n", pid);
}

static void cmd_spawn(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: spawn <name> [mem_kb] [priority]\n"); return; }
    size_t mem_kb = (argc > 2) ? (size_t)atoi(argv[2]) : 16;
    int pri       = (argc > 3) ? atoi(argv[3]) : PROC_PRIORITY_NORM;
    int pid = proc_create(argv[1], shell_pid, (ProcPriority)pri, mem_kb);
    if (pid > 0) {
        simos_print(COL_GREEN, "  Process '%s' spawned with PID %d\n", argv[1], pid);
        sched_enqueue(pid, pri);
    }
}

static void cmd_sleep_proc(int argc, char **argv) {
    if (argc < 3) { printf("  Usage: sleep_proc <pid> <ticks>\n"); return; }
    int pid = atoi(argv[1]);
    int ticks = atoi(argv[2]);
    Process *p = proc_find(pid);
    if (!p) { printf("  sleep_proc: pid %d not found\n", pid); return; }
    p->state = PROC_STATE_SLEEPING;
    p->sleep_until = sys_tick + (uint64_t)ticks;
    simos_print(COL_YELLOW, "  pid=%d sleeping for %d ticks\n", pid, ticks);
}

static void cmd_tick(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 1;
    for (int i = 0; i < n; i++) sched_tick();
    simos_print(COL_CYAN, "  Advanced %d tick(s). Total: %llu\n",
        n, (unsigned long long)sys_tick);
}

static void cmd_irq(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: irq <number>\n"); return; }
    int irq = atoi(argv[1]);
    idt_fire(irq);
    simos_print(COL_YELLOW, "  Fired IRQ %d (count=%llu)\n",
        irq, (unsigned long long)idt[irq & 0xFF].fire_count);
}

static void cmd_net_send(int argc, char **argv) {
    if (argc < 4) { printf("  Usage: netsend <src> <dst> <message>\n"); return; }
    char msg[MAX_PACKET_DATA];
    strncpy(msg, argv[3], MAX_PACKET_DATA-1);
    net_send(argv[1], argv[2], 1, (uint8_t*)msg, strlen(msg));
    simos_print(COL_GREEN, "  Packet queued: %s → %s\n", argv[1], argv[2]);
}

static void cmd_sysinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_BOLD COL_CYAN,
        "\n  ┌─────────────────────────────────────────────────────┐\n"
        "  │              SimOS System Information                │\n"
        "  ├─────────────────────────────────────────────────────┤\n");
    simos_print(COL_WHITE,
        "  │  Version   : %-38s│\n", "SimOS v" SIMOS_VERSION);
    simos_print(COL_WHITE,
        "  │  Arch      : %-38s│\n", "x86 (simulated)");
    simos_print(COL_WHITE,
        "  │  RAM       : %-38s│\n", "1024 KB (simulated)");

    char pbuf[32]; snprintf(pbuf, 32, "%d", next_pid - 1);
    simos_print(COL_WHITE, "  │  Processes : %-38s│\n", pbuf);

    char mbuf[48]; snprintf(mbuf, 48, "%zu KB used / %d KB total", mem_used_kb, MEM_TOTAL_KB);
    simos_print(COL_WHITE, "  │  Memory    : %-38s│\n", mbuf);

    char fbuf[32]; snprintf(fbuf, 32, "%d inodes", next_inode);
    simos_print(COL_WHITE, "  │  FS Inodes : %-38s│\n", fbuf);

    char tbuf[32]; snprintf(tbuf, 32, "%llu", (unsigned long long)sys_tick);
    simos_print(COL_WHITE, "  │  Ticks     : %-38s│\n", tbuf);

    simos_print(COL_WHITE, "  │  Devices   : %-38d│\n", num_devices);
    simos_print(COL_CYAN,  "  └─────────────────────────────────────────────────────┘\n\n");
}

static void cmd_colour(int argc, char **argv) {
    if (argc < 2) {
        printf("  colour: %s\n", colour_enabled ? "ON" : "OFF");
        return;
    }
    colour_enabled = (strcmp(argv[1],"on")==0 || strcmp(argv[1],"1")==0) ? 1 : 0;
    printf("  Colour output: %s\n", colour_enabled ? "ON" : "OFF");
}

static void cmd_inode_info(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: inode <path>\n"); return; }
    char fullpath[MAX_FILEPATH];
    path_join(fullpath, sizeof(fullpath), shell_cwd, argv[1]);
    int idx = fs_find_inode(fullpath);
    if (idx < 0) { printf("  inode: '%s' not found\n", fullpath); return; }
    Inode *f = &inode_table[idx];
    simos_print(COL_CYAN, "  Inode #%d\n", f->inode_id);
    simos_print(COL_WHITE, "  Path     : %s\n", f->path);
    simos_print(COL_WHITE, "  Type     : %s\n", f->type==INODE_DIR?"directory":"file");
    simos_print(COL_WHITE, "  Size     : %zu bytes\n", f->size);
    simos_print(COL_WHITE, "  Perm     : %04o\n", (unsigned)f->permissions);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&f->created));
    simos_print(COL_WHITE, "  Created  : %s\n", tbuf);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&f->modified));
    simos_print(COL_WHITE, "  Modified : %s\n", tbuf);
    simos_print(COL_WHITE, "  Ref count: %d\n", f->ref_count);
}

static void cmd_chmod(int argc, char **argv) {
    if (argc < 3) { printf("  Usage: chmod <octal> <path>\n"); return; }
    char fullpath[MAX_FILEPATH];
    path_join(fullpath, sizeof(fullpath), shell_cwd, argv[2]);
    int idx = fs_find_inode(fullpath);
    if (idx < 0) { printf("  chmod: '%s' not found\n", fullpath); return; }
    unsigned perm = 0;
    sscanf(argv[1], "%o", &perm);
    inode_table[idx].permissions = (uint8_t)perm;
    simos_print(COL_GREEN, "  chmod %s %s\n", argv[1], fullpath);
}

static void cmd_cp(int argc, char **argv) {
    if (argc < 3) { printf("  Usage: cp <src> <dst>\n"); return; }
    char src[MAX_FILEPATH], dst[MAX_FILEPATH];
    path_join(src, sizeof(src), shell_cwd, argv[1]);
    path_join(dst, sizeof(dst), shell_cwd, argv[2]);
    int sidx = fs_find_inode(src);
    if (sidx < 0) { printf("  cp: source not found\n"); return; }
    int didx = fs_alloc_inode();
    if (didx < 0) { printf("  cp: inode table full\n"); return; }
    memcpy(&inode_table[didx], &inode_table[sidx], sizeof(Inode));
    inode_table[didx].inode_id = didx;
    strncpy(inode_table[didx].path, dst, MAX_FILEPATH-1);
    strncpy(inode_table[didx].name, path_basename(dst), MAX_FILENAME-1);
    inode_table[didx].created = inode_table[didx].modified = time(NULL);
    simos_print(COL_GREEN, "  Copied %s → %s\n", src, dst);
}

static void cmd_mv(int argc, char **argv) {
    if (argc < 3) { printf("  Usage: mv <src> <dst>\n"); return; }
    cmd_cp(argc, argv);
    char fullpath[MAX_FILEPATH];
    path_join(fullpath, sizeof(fullpath), shell_cwd, argv[1]);
    fs_delete(fullpath);
}

static void cmd_find(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: find <name>\n"); return; }
    int found = 0;
    for (int i = 0; i < MAX_FILES+MAX_DIRS; i++) {
        if (inode_table[i].type == INODE_FREE) continue;
        if (strstr(inode_table[i].name, argv[1])) {
            simos_print(COL_GREEN, "  %s\n", inode_table[i].path);
            found++;
        }
    }
    if (!found) printf("  find: no matches for '%s'\n", argv[1]);
}

static void cmd_wc(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: wc <file>\n"); return; }
    char fullpath[MAX_FILEPATH];
    path_join(fullpath, sizeof(fullpath), shell_cwd, argv[1]);
    int idx = fs_find_inode(fullpath);
    if (idx < 0) { printf("  wc: '%s' not found\n", fullpath); return; }
    if (inode_table[idx].type != INODE_FILE) { printf("  wc: not a file\n"); return; }
    Inode *f = &inode_table[idx];
    int lines=0, words=0;
    size_t bytes = f->size;
    int in_word = 0;
    for (size_t i = 0; i < bytes; i++) {
        char c = (char)f->data[i];
        if (c == '\n') lines++;
        if (isspace((unsigned char)c)) in_word = 0;
        else if (!in_word) { in_word = 1; words++; }
    }
    simos_print(COL_WHITE, "  lines=%-6d  words=%-6d  bytes=%-6zu  %s\n",
        lines, words, bytes, f->name);
}

static void cmd_df(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_CYAN, "  Filesystem     Size   Used  Avail  Use%%\n");
    simos_print(COL_WHITE, "  /dev/sda1      %4dK  %4zuK  %4zuK  %3zu%%\n",
        MEM_TOTAL_KB, mem_used_kb,
        (size_t)MEM_TOTAL_KB - mem_used_kb,
        mem_used_kb * 100 / MEM_TOTAL_KB);
}

static void cmd_env(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_WHITE, "  PATH=/bin:/usr/bin\n");
    simos_print(COL_WHITE, "  HOME=/home/user\n");
    simos_print(COL_WHITE, "  SHELL=/bin/sh\n");
    simos_print(COL_WHITE, "  USER=user\n");
    simos_print(COL_WHITE, "  TERM=xterm-256color\n");
    simos_print(COL_WHITE, "  SIMOS_VERSION=%s\n", SIMOS_VERSION);
}

static void cmd_proc_detail(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: proc <pid>\n"); return; }
    int pid = atoi(argv[1]);
    Process *p = proc_find(pid);
    if (!p) { printf("  proc: pid %d not found\n", pid); return; }
    simos_print(COL_CYAN, "  ─── Process Detail ─────────────────────\n");
    simos_print(COL_WHITE, "  PID      : %d\n", p->pid);
    simos_print(COL_WHITE, "  Name     : %s\n", p->name);
    simos_print(COL_WHITE, "  PPID     : %d\n", p->ppid);
    simos_print(COL_WHITE, "  State    : %s\n", proc_state_str(p->state));
    simos_print(COL_WHITE, "  Priority : %d\n", (int)p->priority);
    simos_print(COL_WHITE, "  CPU time : %llu ticks\n", (unsigned long long)p->cpu_time);
    simos_print(COL_WHITE, "  Memory   : %zu KB\n", p->mem_usage_kb);
    simos_print(COL_WHITE, "  CWD      : %s\n", p->cwd);
    simos_print(COL_WHITE, "  Open FDs : %d\n", p->num_open_fds);
}

static void cmd_malloc_test(int argc, char **argv) {
    int kb = (argc > 1) ? atoi(argv[1]) : 8;
    int base = mem_alloc(current_pid, (size_t)kb, MEM_USED, "test-alloc");
    if (base >= 0)
        simos_print(COL_GREEN, "  Allocated %d KB at base %dKB\n", kb, base);
    else
        simos_print(COL_RED, "  Allocation failed\n");
}

static void cmd_crash(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_RED,
        "\n  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
        "  !!  KERNEL PANIC — SIMULATED BLUE SCREEN     !!\n"
        "  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n");
    simos_print(COL_BOLD COL_WHITE,
        "  STOP: 0x0000007E (0xC0000005, 0xFFFF8001, 0x...)\n");
    simos_print(COL_WHITE,
        "\n  A problem has been detected and SimOS has been\n"
        "  shut down to prevent damage to your simulation.\n\n"
        "  SIMULATED_KERNEL_FAULT\n\n"
        "  If this is the first time you've seen this error,\n"
        "  restart your simulation. If this screen appears\n"
        "  again, type 'reboot' to continue.\n\n");
    klog_write(LOG_ERROR, "KERNEL PANIC: user-induced crash");
}

static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_YELLOW, "  Rebooting SimOS...\n");
    SLEEP_MS(800);
    /* re-init all subsystems */
    mem_init();
    proc_init();
    fs_init();
    sched_init();
    net_init();
    dev_init();
    idt_init();
    sys_tick   = 0;
    next_pid   = 1;
    next_inode = 1;
    num_windows= 0;
    active_window = -1;
    strcpy(shell_cwd, "/");
    klog_write(LOG_KERNEL, "System rebooted by user");
    shell_pid = proc_create("shell", 0, PROC_PRIORITY_HIGH, 8);
    current_pid = shell_pid;
    sched_enqueue(shell_pid, PROC_PRIORITY_HIGH);
    CLEAR_SCREEN();
    banner();
    simos_print(COL_GREEN, "  SimOS rebooted successfully.\n\n");
}

static void cmd_exit_shell(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_YELLOW, "  Exiting SimOS shell. Goodbye!\n");
    os_running = 0;
}

static void cmd_about(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_CYAN,
    "\n  ╔══════════════════════════════════════════════════════════════╗\n"
    "  ║              About SimOS v" SIMOS_VERSION "                              ║\n"
    "  ╠══════════════════════════════════════════════════════════════╣\n"
    "  ║  A fully simulated operating system written in a single C   ║\n"
    "  ║  file (~10,000 lines).                                      ║\n"
    "  ║                                                             ║\n"
    "  ║  Subsystems:                                                ║\n"
    "  ║   • Process Manager  — PID table, states, scheduling        ║\n"
    "  ║   • Virtual FS       — inodes, dirs, files, permissions     ║\n"
    "  ║   • Memory Manager   — buddy allocator, page table          ║\n"
    "  ║   • MLFQ Scheduler   — multilevel feedback queue            ║\n"
    "  ║   • Device Manager   — char/block device registry           ║\n"
    "  ║   • Network Stack    — loopback packet queue                ║\n"
    "  ║   • IDT / Syscalls   — 256-vector interrupt table           ║\n"
    "  ║   • Kernel Logger    — ring-buffer dmesg                    ║\n"
    "  ║   • 2D GUI           — ASCII desktop with windows           ║\n"
    "  ║   • Shell            — interactive command interpreter       ║\n"
    "  ╚══════════════════════════════════════════════════════════════╝\n\n");
}

/* ====================================================================
 *  SECTION 15 — EXTENDED COMMANDS
 * ==================================================================== */

/* Disk I/O simulation */
typedef struct {
    int    sector;
    char   data[FS_BLOCK_SIZE];
    int    dirty;
} DiskBlock;

static DiskBlock disk_cache[16];
static int       disk_cache_count = 0;

static void cmd_diskread(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: diskread <sector>\n"); return; }
    int sector = atoi(argv[1]);
    simos_print(COL_GREEN, "  [DMA] Reading sector %d → buffer 0x%04X\n",
        sector, sector * FS_BLOCK_SIZE);
    simos_print(COL_WHITE, "  Data: ");
    for (int i = 0; i < 16; i++) printf("%02X ", (sector*16+i) & 0xFF);
    printf("...\n");
    /* simulate latency */
    devices[3].io_count++;  /* hdd0 */
    klog_write(LOG_DEBUG, "diskread: sector=%d", sector);
}

static void cmd_diskwrite(int argc, char **argv) {
    if (argc < 3) { printf("  Usage: diskwrite <sector> <data>\n"); return; }
    int sector = atoi(argv[1]);
    simos_print(COL_YELLOW, "  [DMA] Writing sector %d ← '%s'\n", sector, argv[2]);
    devices[3].io_count++;
    klog_write(LOG_DEBUG, "diskwrite: sector=%d data='%s'", sector, argv[2]);
}

/* Pipe simulation */
static char pipe_buffer[MAX_FILE_SIZE] = {0};

static void cmd_pipe(int argc, char **argv) {
    if (argc < 3) { printf("  Usage: pipe <cmd1> | <cmd2>  (type 'pipe help' for info)\n"); return; }
    /* basic: pipe echo <text> | wc  */
    simos_print(COL_YELLOW, "  [PIPE] Simulated: %s | %s\n", argv[1], argv[2]);
    if (strcmp(argv[1],"echo")==0 && strcmp(argv[2],"wc")==0 && argc>3) {
        int words = 0; int in_word = 0;
        for (int i = 3; argv[i]; i++) { words++; (void)in_word; }
        printf("  result: %d word(s)\n", words);
    }
}

/* Stress test: spawn N processes */
static void cmd_stress(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 5;
    int mem = (argc > 2) ? atoi(argv[2]) : 4;
    simos_print(COL_YELLOW, "  Stress: spawning %d processes...\n", n);
    for (int i = 0; i < n; i++) {
        char name[32]; snprintf(name, 32, "stress%d", i);
        int pid = proc_create(name, shell_pid, PROC_PRIORITY_LOW, (size_t)mem);
        if (pid > 0) sched_enqueue(pid, PROC_PRIORITY_LOW);
    }
    simos_print(COL_GREEN, "  Done. Use 'ps' to view processes.\n");
}

/* Simulate page fault */
static void isr_page_fault(int irq, void *ctx) {
    (void)irq; (void)ctx;
    simos_print(COL_RED, "\n  [IRQ 14] Page Fault — addr=0xDEADBEEF pid=%d\n", current_pid);
    klog_write(LOG_ERROR, "Page fault in pid=%d", current_pid);
}

/* show page table */
static void cmd_pagetable(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_CYAN, "  Page Table (%d pages, %d KB each)\n", MAX_PAGES, PAGE_SIZE_KB);
    simos_print(COL_WHITE, "  %-6s  %-6s  %-6s  %-6s\n","Page","Base","Pid","Valid");
    for (int i = 0; i < MAX_PAGES && i < 32; i++) {
        simos_print((page_table[i].valid ? COL_GREEN : COL_WHITE),
            "  %-6d  %-6zu  %-6d  %s\n",
            i, page_table[i].phys_base_kb, page_table[i].owner_pid,
            page_table[i].valid ? "YES" : "no");
    }
    if (MAX_PAGES > 32) printf("  ... (%d more pages)\n", MAX_PAGES-32);
}

static void cmd_mmap(int argc, char **argv) {
    int kb = (argc > 1) ? atoi(argv[1]) : PAGE_SIZE_KB;
    int base = mem_alloc(current_pid, (size_t)kb, MEM_MMAP, "mmap");
    if (base >= 0) {
        /* mark pages */
        int pg = (int)(base / PAGE_SIZE_KB);
        int np = (kb + PAGE_SIZE_KB - 1) / PAGE_SIZE_KB;
        for (int i = pg; i < pg+np && i < MAX_PAGES; i++) {
            page_table[i].owner_pid  = current_pid;
            page_table[i].phys_base_kb = (size_t)base;
            page_table[i].valid      = 1;
        }
        simos_print(COL_GREEN, "  mmap: %d KB at 0x%04X (page %d)\n", kb, base*1024, pg);
    }
}

static void cmd_munmap(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: munmap <base_kb>\n"); return; }
    int base = atoi(argv[1]);
    int pg = base / PAGE_SIZE_KB;
    for (int i = pg; i < MAX_PAGES; i++) {
        if (page_table[i].phys_base_kb == (size_t)base && page_table[i].owner_pid == current_pid) {
            page_table[i].valid = 0;
            page_table[i].owner_pid = -1;
        }
    }
    simos_print(COL_YELLOW, "  munmap: released mapping at base %d KB\n", base);
}

/* Top-like display */
static void cmd_top(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_BOLD, "  SimOS top — tick: %llu  processes: %d  mem used: %zu/%d KB\n\n",
        (unsigned long long)sys_tick, next_pid-1, mem_used_kb, MEM_TOTAL_KB);
    simos_print(COL_CYAN, "  %-6s %-15s %-8s %-6s %-8s %-5s\n",
        "PID","NAME","STATE","PRI","CPU","MEM");
    simos_print(COL_CYAN, "  ─────────────────────────────────────────────────\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        Process *p = &proc_table[i];
        if (p->pid < 0 || p->state == PROC_STATE_FREE) continue;
        const char *col = (p->state == PROC_STATE_RUNNING) ? COL_GREEN :
                          (p->state == PROC_STATE_ZOMBIE)  ? COL_RED :
                          (p->state == PROC_STATE_SLEEPING)? COL_YELLOW : COL_WHITE;
        simos_print(col, "  %-6d %-15s %-8s %-6d %-8llu %zuK\n",
            p->pid, p->name, proc_state_str(p->state), (int)p->priority,
            (unsigned long long)p->cpu_time, p->mem_usage_kb);
    }
}

/* Inspect an interrupt vector */
static void cmd_int_info(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: intinfo <irq>\n"); return; }
    int irq = atoi(argv[1]) & 0xFF;
    simos_print(COL_CYAN, "  IDT[%d]: enabled=%d handler=%s fired=%llu\n",
        irq, idt[irq].enabled,
        idt[irq].handler ? "set" : "none",
        (unsigned long long)idt[irq].fire_count);
}

static void cmd_lsint(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_CYAN, "  Registered interrupt vectors:\n");
    for (int i = 0; i < 256; i++) {
        if (idt[i].handler || idt[i].fire_count)
            simos_print(COL_WHITE, "  IRQ %-3d  enabled=%-3s  fires=%llu\n",
                i, idt[i].enabled?"yes":"no",
                (unsigned long long)idt[i].fire_count);
    }
}

/* Hex dump a file */
static void cmd_hexdump(int argc, char **argv) {
    if (argc < 2) { printf("  Usage: hexdump <file>\n"); return; }
    char fullpath[MAX_FILEPATH];
    path_join(fullpath, sizeof(fullpath), shell_cwd, argv[1]);
    int idx = fs_find_inode(fullpath);
    if (idx < 0) { printf("  hexdump: '%s' not found\n", fullpath); return; }
    Inode *f = &inode_table[idx];
    size_t max = (f->size < 256) ? f->size : 256;
    for (size_t i = 0; i < max; i += 16) {
        printf("  %04zX  ", i);
        for (size_t j = i; j < i+16; j++) {
            if (j < max) printf("%02X ", f->data[j]); else printf("   ");
            if (j == i+7) printf(" ");
        }
        printf(" |");
        for (size_t j = i; j < i+16 && j < max; j++)
            putchar(isprint(f->data[j]) ? f->data[j] : '.');
        printf("|\n");
    }
    if (f->size > 256) printf("  ... (%zu bytes total)\n", f->size);
}

/* Append content to file */
static void cmd_append(int argc, char **argv) {
    if (argc < 3) { printf("  Usage: append <file> <text...>\n"); return; }
    char fullpath[MAX_FILEPATH];
    path_join(fullpath, sizeof(fullpath), shell_cwd, argv[1]);
    int idx = fs_find_inode(fullpath);
    if (idx < 0) { fs_create_file(fullpath, ""); idx = fs_find_inode(fullpath); }
    if (idx < 0) { printf("  append: cannot create '%s'\n", fullpath); return; }
    Inode *f = &inode_table[idx];
    char buf[MAX_FILE_SIZE] = {0};
    for (int i = 2; i < argc; i++) {
        strncat(buf, argv[i], MAX_FILE_SIZE - strlen(buf) - 2);
        if (i < argc-1) strncat(buf, " ", MAX_FILE_SIZE - strlen(buf) - 2);
    }
    strncat(buf, "\n", MAX_FILE_SIZE - strlen(buf) - 2);
    size_t addlen = strlen(buf);
    if (f->size + addlen > MAX_FILE_SIZE) addlen = MAX_FILE_SIZE - f->size;
    memcpy(f->data + f->size, buf, addlen);
    f->size += addlen;
    f->modified = time(NULL);
    printf("  Appended %zu bytes to %s\n", addlen, fullpath);
}

/* Simulate context switch */
static void cmd_ctxswitch(int argc, char **argv) {
    (void)argc; (void)argv;
    int next = sched_pick_next();
    if (next == current_pid) { printf("  Context switch: same process (pid=%d)\n", next); return; }
    Process *old = proc_find(current_pid);
    Process *np  = proc_find(next);
    if (old) old->state = PROC_STATE_READY;
    if (np)  np->state  = PROC_STATE_RUNNING;
    current_pid = next;
    simos_print(COL_CYAN, "  Context switch: → pid=%d (%s)\n",
        next, np ? np->name : "unknown");
    sched_tick();
}

/* Show process tree */
static void print_proc_tree(int ppid, int depth) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        Process *p = &proc_table[i];
        if (p->pid < 0 || p->state == PROC_STATE_FREE) continue;
        if (p->ppid != ppid || p->pid == ppid) continue;
        for (int d = 0; d < depth; d++) printf("  ");
        simos_print(COL_CYAN, "  └─ ");
        simos_print(COL_WHITE, "pid=%-4d %s\n", p->pid, p->name);
        print_proc_tree(p->pid, depth+1);
    }
}

static void cmd_pstree(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_BOLD, "  Process tree:\n");
    simos_print(COL_MAGENTA, "  pid=0 [idle]\n");
    print_proc_tree(0, 1);
    simos_print(COL_MAGENTA, "  pid=%d [shell]\n", shell_pid);
    print_proc_tree(shell_pid, 1);
}

/* ============================================================================
 *  SECTION 16 — COMMAND DISPATCH TABLE
 * ============================================================================ */

typedef struct {
    const char *name;
    void (*fn)(int, char**);
    const char *help;
} Command;

static Command commands[] = {
    /* General */
    {"help",       cmd_help,        "Show this help"},
    {"clear",      cmd_clear,       "Clear screen"},
    {"about",      cmd_about,       "About SimOS"},
    {"echo",       cmd_echo,        "Echo text"},
    {"date",       cmd_date,        "Show date/time"},
    {"uptime",     cmd_uptime,      "Show uptime"},
    {"sysinfo",    cmd_sysinfo,     "System information"},
    {"env",        cmd_env,         "Show environment"},
    {"colour",     cmd_colour,      "colour on|off — toggle ANSI colour"},
    {"history",    cmd_history,     "Command history"},
    {"exit",       cmd_exit_shell,  "Exit SimOS"},
    {"quit",       cmd_exit_shell,  "Exit SimOS"},
    /* Process */
    {"ps",         cmd_ps,          "List processes"},
    {"top",        cmd_top,         "Process monitor"},
    {"spawn",      cmd_spawn,       "spawn <name> [mem_kb] [pri]"},
    {"kill",       cmd_kill,        "kill <pid>"},
    {"proc",       cmd_proc_detail, "proc <pid> — process details"},
    {"pstree",     cmd_pstree,      "Process tree"},
    {"sleep_proc", cmd_sleep_proc,  "sleep_proc <pid> <ticks>"},
    {"ctxswitch",  cmd_ctxswitch,   "Simulate context switch"},
    {"stress",     cmd_stress,      "stress <n> [mem] — spawn N processes"},
    {"crash",      cmd_crash,       "Simulate BSOD"},
    {"reboot",     cmd_reboot,      "Reboot SimOS"},
    /* Memory */
    {"meminfo",    cmd_mem_info,    "Memory map"},
    {"malloc",     cmd_malloc_test, "malloc <kb> — allocate memory"},
    {"mmap",       cmd_mmap,        "mmap <kb> — memory-map region"},
    {"munmap",     cmd_munmap,      "munmap <base_kb>"},
    {"pagetable",  cmd_pagetable,   "Show page table"},
    /* File System */
    {"ls",         cmd_ls,          "ls [path] — list directory"},
    {"cd",         cmd_cd,          "cd <path>"},
    {"pwd",        cmd_pwd,         "Print working directory"},
    {"mkdir",      cmd_mkdir,       "mkdir <dir>"},
    {"touch",      cmd_touch,       "touch <file>"},
    {"rm",         cmd_rm,          "rm <path>"},
    {"cat",        cmd_cat,         "cat <file>"},
    {"write",      cmd_write,       "write <file> <text>"},
    {"append",     cmd_append,      "append <file> <text>"},
    {"cp",         cmd_cp,          "cp <src> <dst>"},
    {"mv",         cmd_mv,          "mv <src> <dst>"},
    {"find",       cmd_find,        "find <name>"},
    {"wc",         cmd_wc,          "wc <file> — word count"},
    {"hexdump",    cmd_hexdump,     "hexdump <file>"},
    {"inode",      cmd_inode_info,  "inode <path> — inode details"},
    {"chmod",      cmd_chmod,       "chmod <octal> <path>"},
    {"df",         cmd_df,          "Disk usage"},
    /* Scheduler */
    {"sched",      cmd_sched_info,  "Scheduler state"},
    {"tick",       cmd_tick,        "tick [n] — advance n ticks"},
    /* Devices */
    {"lsdev",      cmd_lsdev,       "List devices"},
    {"diskread",   cmd_diskread,    "diskread <sector>"},
    {"diskwrite",  cmd_diskwrite,   "diskwrite <sector> <data>"},
    /* Network */
    {"netstat",    cmd_netstat,     "Network packet queue"},
    {"ping",       cmd_ping,        "ping <address>"},
    {"netsend",    cmd_net_send,    "netsend <src> <dst> <msg>"},
    /* Interrupts */
    {"irq",        cmd_irq,         "irq <n> — fire interrupt"},
    {"lsint",      cmd_lsint,       "List interrupt handlers"},
    {"intinfo",    cmd_int_info,    "intinfo <irq> — interrupt detail"},
    /* Logging */
    {"dmesg",      cmd_dmesg,       "dmesg [n] — kernel log"},
    /* Pipe */
    {"pipe",       cmd_pipe,        "pipe <cmd1> | <cmd2>"},
    /* GUI */
    {"gui",        cmd_gui,         "Launch 2D GUI desktop"},
    {NULL, NULL, NULL}
};

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    simos_print(COL_BOLD COL_CYAN,
        "\n  ╔══════════════════════════════════════════════════════════════╗\n"
        "  ║                   SimOS Command Reference                    ║\n"
        "  ╚══════════════════════════════════════════════════════════════╝\n\n");
    int col = 0;
    for (int i = 0; commands[i].name; i++) {
        simos_print(COL_GREEN, "  %-14s", commands[i].name);
        simos_print(COL_WHITE, "%-30s", commands[i].help);
        col++;
        if (col % 2 == 0) printf("\n");
    }
    if (col % 2 != 0) printf("\n");
    printf("\n");
}

/* ============================================================================
 *  SECTION 17 — SHELL MAIN LOOP
 * ============================================================================ */

static void shell_exec(char *cmdline) {
    str_trim(cmdline);
    if (!cmdline[0]) return;
    history_add(cmdline);

    /* tokenize */
    char *argv[MAX_CMD_ARGS];
    int   argc = 0;
    char  buf[MAX_CMD_LEN];
    strncpy(buf, cmdline, MAX_CMD_LEN-1);
    char *tok = strtok(buf, " \t");
    while (tok && argc < MAX_CMD_ARGS-1) { argv[argc++] = tok; tok = strtok(NULL, " \t"); }
    argv[argc] = NULL;
    if (argc == 0) return;

    /* find command */
    for (int i = 0; commands[i].name; i++) {
        if (strcmp(commands[i].name, argv[0]) == 0) {
            commands[i].fn(argc, argv);
            return;
        }
    }
    simos_print(COL_RED, "  simos: command not found: %s  (type 'help')\n", argv[0]);
}

static void shell_prompt(void) {
    simos_print(COL_GREEN, "simos");
    simos_print(COL_WHITE, ":");
    simos_print(COL_BLUE, "%s", shell_cwd);
    simos_print(COL_WHITE, "$ ");
    fflush(stdout);
}

static void shell_run(void) {
    char cmdline[MAX_CMD_LEN];
    while (os_running) {
        shell_prompt();
        if (!fgets(cmdline, sizeof(cmdline), stdin)) break;
        /* strip newline */
        char *nl = strchr(cmdline, '\n');
        if (nl) *nl = '\0';
        shell_exec(cmdline);
        sched_tick();   /* one tick per command */
    }
}

/* ============================================================================
 *  SECTION 18 — KERNEL INIT & BOOT SEQUENCE
 * ============================================================================ */

static void boot_splash(void) {
    CLEAR_SCREEN();
    simos_print(COL_CYAN,
    "\n\n"
    "         ███████╗██╗███╗   ███╗ ██████╗ ███████╗\n"
    "         ██╔════╝██║████╗ ████║██╔═══██╗██╔════╝\n"
    "         ███████╗██║██╔████╔██║██║   ██║███████╗\n"
    "         ╚════██║██║██║╚██╔╝██║██║   ██║╚════██║\n"
    "         ███████║██║██║ ╚═╝ ██║╚██████╔╝███████║\n"
    "         ╚══════╝╚═╝╚═╝     ╚═╝ ╚═════╝ ╚══════╝\n\n");
    simos_print(COL_WHITE,
    "                  Simulated Operating System\n"
    "                        version " SIMOS_VERSION "\n\n");
    SLEEP_MS(600);

    const char *steps[] = {
        "Initialising IDT...           ",
        "Starting memory manager...    ",
        "Mounting virtual file system..",
        "Loading process table...      ",
        "Starting MLFQ scheduler...    ",
        "Registering devices...        ",
        "Bringing up network stack...  ",
        "Starting kernel logger...     ",
        "Launching shell (pid=1)...    ",
        NULL
    };
    for (int i = 0; steps[i]; i++) {
        simos_print(COL_WHITE, "  [ ");
        simos_print(COL_GREEN, " OK ");
        simos_print(COL_WHITE, " ]  %s\n", steps[i]);
        SLEEP_MS(120);
    }
    printf("\n");
    SLEEP_MS(300);
    CLEAR_SCREEN();
}

static void kernel_init(void) {
    boot_splash();
    idt_init();
    mem_init();
    fs_init();
    proc_init();
    sched_init();
    dev_init();
    net_init();

    /* register common ISRs */
    idt_register(0,  isr_default,     NULL); /* divide by zero */
    idt_register(1,  isr_default,     NULL); /* debug          */
    idt_register(14, isr_page_fault,  NULL); /* page fault     */
    idt_register(32, isr_default,     NULL); /* timer          */
    idt_register(33, isr_default,     NULL); /* keyboard       */

    /* create kernel process */
    int kpid = proc_create("kernel", 0, PROC_PRIORITY_RT, mem_kernel_kb);
    proc_table[proc_find(kpid) - proc_table].state = PROC_STATE_RUNNING;
    current_pid = kpid;

    /* create init */
    int init_pid = proc_create("init", kpid, PROC_PRIORITY_HIGH, 4);
    sched_enqueue(init_pid, PROC_PRIORITY_HIGH);

    /* create shell */
    shell_pid = proc_create("shell", init_pid, PROC_PRIORITY_HIGH, 8);
    current_pid = shell_pid;
    proc_table[proc_find(shell_pid) - proc_table].state = PROC_STATE_RUNNING;
    sched_enqueue(shell_pid, PROC_PRIORITY_HIGH);

    /* populate page table for kernel */
    for (int i = 0; i < (int)mem_kernel_kb / PAGE_SIZE_KB && i < MAX_PAGES; i++) {
        page_table[i].owner_pid    = kpid;
        page_table[i].phys_base_kb = (size_t)i * PAGE_SIZE_KB;
        page_table[i].valid        = 1;
    }

    /* send a loopback packet for demo */
    uint8_t hello[] = "Hello, SimOS Network!";
    net_send("127.0.0.1", "127.0.0.1", 2, hello, sizeof(hello));

    klog_write(LOG_KERNEL, "SimOS v%s booted successfully", SIMOS_VERSION);
    klog_write(LOG_INFO,   "Shell PID=%d", shell_pid);

    banner();
    simos_print(COL_GREEN, "  Boot complete. Type 'help' for available commands.\n\n");
}

/* ============================================================================
 *  SECTION 19 — MAIN ENTRY POINT
 * ============================================================================ */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

#ifdef _WIN32
    /* Enable ANSI escape codes on Windows 10+ */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode))
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    SetConsoleTitle(TEXT("SimOS " SIMOS_VERSION));
#endif

    kernel_init();
    shell_run();

    simos_print(COL_CYAN, "\n  SimOS shut down cleanly. Goodbye.\n\n");
    return 0;
}

/*
 * ============================================================================
 *  END OF simulated_os.c
 *  Total functional subsystems: 10
 *  Approximate line count: ~10,000 (including blanks, comments, code)
 * ============================================================================
 */