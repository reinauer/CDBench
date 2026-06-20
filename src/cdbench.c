/*
 * CDBench - AmigaOS CD filesystem benchmark utility
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosasl.h>
#include <dos/dosextens.h>
#include <dos/exall.h>
#include <dos/filehandler.h>
#include <devices/timer.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/timer.h>

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_BUFSIZE 32768UL
#define DEFAULT_READSIZE 2048UL
#define DEFAULT_ITER 128UL
#define DEFAULT_PASSES 1UL
#define DISC_SECTOR_SIZE 2048UL
#define EXALL_BUFSIZE 32768UL
#define MAX_CACHE_PROBE (4UL * 1024UL * 1024UL)

struct Device *TimerBase;

typedef enum EntryKind {
    ENTRY_DATA,
    ENTRY_AUDIO,
    ENTRY_AUDIO_META,
    ENTRY_DIR
} EntryKind;

typedef struct CatalogEntry {
    char *path;
    char *name;
    EntryKind kind;
    uint64_t size;
    unsigned depth;
} CatalogEntry;

typedef struct Catalog {
    CatalogEntry *items;
    size_t count;
    size_t cap;
    uint64_t data_bytes;
    uint64_t audio_bytes;
    uint32_t data_count;
    uint32_t audio_count;
    uint32_t audio_meta_count;
    uint32_t dir_count;
    int used_exnext_fallback;
} Catalog;

typedef struct Identity {
    char dos_device[64];
    char handler[128];
    char exec_device[128];
    uint32_t exec_unit;
    uint32_t open_flags;
    uint32_t dostype;
    uint32_t sector_size;
    uint32_t buffers;
    uint32_t bufmemtype;
    uint32_t mask;
    uint32_t maxtransfer;
    char control[192];
    char source[64];
    int have_exec;
} Identity;

typedef struct Config {
    char *device;
    char *seq_path;
    char *rand_path;
    char *audio_path;
    char *dir_path;
    char *parent_path;
    uint32_t bufsize;
    uint32_t readsize;
    uint64_t maxbytes;
    uint32_t seconds;
    uint32_t iter;
    uint32_t passes;
    uint32_t seed;
    int csv;
    int verbose;
    int raw;
    int cache;
} Config;

typedef struct TimerState {
    struct MsgPort *port;
    struct TimeRequest *req;
    uint32_t freq;
} TimerState;

typedef struct LatStats {
    uint64_t avg;
    uint64_t median;
    uint64_t p95;
    uint64_t min;
    uint64_t max;
} LatStats;

typedef struct RunContext {
    Config cfg;
    TimerState timer;
    Identity ident;
    Catalog catalog;
    char title[128];
    char disc_type[16];
    char fingerprint[32];
    uint32_t current_pass;
    struct Summary *summary;
} RunContext;

typedef struct Summary {
    char discover_status[16];
    char discover_selector[32];
    uint64_t discover_bytes;
    uint64_t discover_ops;
    uint64_t discover_ops_s;
    uint64_t discover_elapsed_us;

    char seq_data_status[16];
    char seq_data_error[64];
    char seq_data_leaf[80];
    uint64_t seq_data_file_size;
    uint64_t seq_data_bytes;
    uint64_t seq_data_rate_kib_s;
    uint64_t seq_data_x_milli;

    char seq_audio_status[16];
    char seq_audio_error[64];
    char seq_audio_leaf[80];
    uint64_t seq_audio_file_size;
    uint64_t seq_audio_bytes;
    uint64_t seq_audio_rate_kib_s;
    uint64_t seq_audio_x_milli;

    LatStats rand_same;
    LatStats rand_stride;
    LatStats rand_seeded;
    int have_rand_same;
    int have_rand_stride;
    int have_rand_seeded;

    uint64_t meta_exall_ops_s;
    uint64_t meta_exnext_ops_s;
    uint64_t traverse_warm_ops_s;
    uint64_t meta_exall_elapsed_us;
    uint64_t meta_exnext_elapsed_us;
    uint64_t traverse_warm_elapsed_us;
    int have_meta_exall;
    int have_meta_exnext;
    int have_traverse_warm;

    LatStats meta_lock_shallow;
    LatStats meta_lock_deep;
    LatStats meta_openfromlock;
    LatStats meta_negative;
    LatStats parent;
    LatStats small_open;
    LatStats audio_open;
    int have_meta_lock_shallow;
    int have_meta_lock_deep;
    int have_meta_openfromlock;
    int have_meta_negative;
    int have_parent;
    int have_small_open;
    int have_audio_open;

    char mixed_status[16];
    char mixed_error[64];
    LatStats mixed;
    int have_mixed;

    uint32_t cache_rows;
    uint64_t cache_max_working_set;
    uint64_t cache_seq_warm_cold_milli;
    uint64_t cache_sector_warm_cold_milli;
    uint64_t cache_partial_warm_cold_milli;

    char raw_status[16];
    char raw_error[64];
    int raw_seen;
} Summary;

typedef struct Result {
    const char *test;
    const char *status;
    const char *error;
    const char *path;
    const char *selector;
    const char *access_shape;
    uint32_t pass;
    uint32_t seed;
    uint64_t file_size;
    uint64_t bytes;
    uint64_t ops;
    uint64_t ops_s;
    uint64_t read_size;
    uint64_t buffer_size;
    uint64_t working_set_bytes;
    uint64_t hot_set_bytes;
    uint64_t eviction_bytes;
    uint64_t elapsed_us;
    uint64_t rate_kib_s;
    uint64_t x_speed_milli;
    uint64_t raw_rate_kib_s;
    uint64_t efficiency_milli;
    uint64_t warm_cold_milli;
    LatStats lat;
} Result;

typedef struct TimedRead {
    uint64_t elapsed_us;
    uint64_t bytes_read;
} TimedRead;

static char *xstrdup(const char *s);
static int catalog_add(Catalog *cat, const char *path, const char *name,
                       EntryKind kind, uint64_t size, unsigned depth);
static void catalog_free(Catalog *cat);
static int discover_catalog(Catalog *cat, const char *root);
static void emit_result(RunContext *ctx, const Result *r);
static void emit_summary(const RunContext *ctx);
static void config_cleanup(Config *cfg);
static void timer_cleanup(TimerState *timer);

static char *xstrdup(const char *s)
{
    size_t n;
    char *p;

    if (!s)
        s = "";
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static int ascii_tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

static int str_casecmp(const char *a, const char *b)
{
    unsigned char ca, cb;

    while (*a || *b) {
        ca = (unsigned char)ascii_tolower((unsigned char)*a);
        cb = (unsigned char)ascii_tolower((unsigned char)*b);
        if (ca != cb)
            return (int)ca - (int)cb;
        if (*a)
            a++;
        if (*b)
            b++;
    }
    return 0;
}

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t slen = strlen(s);
    size_t tlen = strlen(suffix);

    if (tlen > slen)
        return 0;
    return str_casecmp(s + slen - tlen, suffix) == 0;
}

static int starts_with_ci(const char *s, const char *prefix)
{
    while (*prefix) {
        if (ascii_tolower((unsigned char)*s) !=
            ascii_tolower((unsigned char)*prefix))
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

static const char *leaf_name(const char *path)
{
    const char *p = path;
    const char *leaf = path;

    while (*p) {
        if (*p == ':' || *p == '/')
            leaf = p + 1;
        p++;
    }
    return leaf;
}

static char *join_path(const char *dir, const char *name)
{
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    int need_slash = 0;
    char *out;

    if (dlen != 0 && dir[dlen - 1] != ':' && dir[dlen - 1] != '/')
        need_slash = 1;
    out = (char *)malloc(dlen + (size_t)need_slash + nlen + 1);
    if (!out)
        return NULL;
    memcpy(out, dir, dlen);
    if (need_slash)
        out[dlen++] = '/';
    memcpy(out + dlen, name, nlen + 1);
    return out;
}

static int is_track_number_name(const char *leaf, const char *ext)
{
    if (!starts_with_ci(leaf, "Track"))
        return 0;
    if (!isdigit((unsigned char)leaf[5]) ||
        !isdigit((unsigned char)leaf[6]))
        return 0;
    return str_casecmp(leaf + 7, ext) == 0;
}

static int parent_is_cdda_dir(const char *path)
{
    const char *leaf = leaf_name(path);
    size_t prefix_len = (size_t)(leaf - path);
    const char *p;

    if (prefix_len < 5)
        return 0;
    p = leaf - 5;
    if ((p == path || p[-1] == ':' || p[-1] == '/') &&
        p[0] == 'C' && p[1] == 'D' && p[2] == 'D' && p[3] == 'A' &&
        p[4] == '/')
        return 1;
    if ((p == path || p[-1] == ':' || p[-1] == '/') &&
        (ascii_tolower((unsigned char)p[0]) == 'c') &&
        (ascii_tolower((unsigned char)p[1]) == 'd') &&
        (ascii_tolower((unsigned char)p[2]) == 'd') &&
        (ascii_tolower((unsigned char)p[3]) == 'a') &&
        p[4] == '/')
        return 1;
    return 0;
}

static int is_audio_meta_name(const char *leaf)
{
    return str_casecmp(leaf, "CDDB.txt") == 0 ||
           str_casecmp(leaf, "CD-TEXT.txt") == 0;
}

static EntryKind classify_file(const char *path, const char *name)
{
    if (is_audio_meta_name(name))
        return ENTRY_AUDIO_META;
    if (ends_with_ci(name, ".cdda") || ends_with_ci(name, ".aiff") ||
        ends_with_ci(name, ".aif"))
        return ENTRY_AUDIO;
    if (ends_with_ci(name, ".wav") &&
        (parent_is_cdda_dir(path) || is_track_number_name(name, ".wav")))
        return ENTRY_AUDIO;
    return ENTRY_DATA;
}

static void catalog_init(Catalog *cat)
{
    memset(cat, 0, sizeof(*cat));
}

static void catalog_clear(Catalog *cat)
{
    size_t i;

    for (i = 0; i < cat->count; i++) {
        free(cat->items[i].path);
        free(cat->items[i].name);
    }
    cat->count = 0;
    cat->data_bytes = 0;
    cat->audio_bytes = 0;
    cat->data_count = 0;
    cat->audio_count = 0;
    cat->audio_meta_count = 0;
    cat->dir_count = 0;
    cat->used_exnext_fallback = 0;
}

static void catalog_free(Catalog *cat)
{
    catalog_clear(cat);
    free(cat->items);
    memset(cat, 0, sizeof(*cat));
}

static int catalog_reserve(Catalog *cat, size_t extra)
{
    CatalogEntry *p;
    size_t new_cap;

    if (cat->count + extra <= cat->cap)
        return 1;
    new_cap = cat->cap ? cat->cap * 2 : 256;
    while (new_cap < cat->count + extra)
        new_cap *= 2;
    p = (CatalogEntry *)realloc(cat->items, new_cap * sizeof(cat->items[0]));
    if (!p)
        return 0;
    cat->items = p;
    cat->cap = new_cap;
    return 1;
}

static int catalog_add(Catalog *cat, const char *path, const char *name,
                       EntryKind kind, uint64_t size, unsigned depth)
{
    CatalogEntry *e;
    char *path_copy;
    char *name_copy;

    if (!catalog_reserve(cat, 1))
        return 0;
    path_copy = xstrdup(path);
    name_copy = xstrdup(name);
    if (!path_copy || !name_copy) {
        free(path_copy);
        free(name_copy);
        return 0;
    }
    e = &cat->items[cat->count++];
    e->path = path_copy;
    e->name = name_copy;
    e->kind = kind;
    e->size = size;
    e->depth = depth;
    if (kind == ENTRY_DATA) {
        cat->data_count++;
        cat->data_bytes += size;
    } else if (kind == ENTRY_AUDIO) {
        cat->audio_count++;
        cat->audio_bytes += size;
    } else if (kind == ENTRY_AUDIO_META) {
        cat->audio_meta_count++;
    } else if (kind == ENTRY_DIR) {
        cat->dir_count++;
    }
    return 1;
}

static uint64_t eclock_to_u64(const struct EClockVal *v)
{
    return ((uint64_t)v->ev_hi << 32) | (uint64_t)v->ev_lo;
}

static uint64_t timer_now(const TimerState *timer)
{
    struct EClockVal v;

    (void)timer;
    ReadEClock(&v);
    return eclock_to_u64(&v);
}

static uint64_t ticks_to_us(const TimerState *timer, uint64_t ticks)
{
    if (timer->freq == 0)
        return 0;
    return (ticks * 1000000ULL) / (uint64_t)timer->freq;
}

static int timer_init(TimerState *timer)
{
    memset(timer, 0, sizeof(*timer));
    timer->port = CreateMsgPort();
    if (!timer->port)
        return 0;
    timer->req = (struct TimeRequest *)CreateIORequest(timer->port,
                                                       sizeof(*timer->req));
    if (!timer->req) {
        timer_cleanup(timer);
        return 0;
    }
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK,
                   (struct IORequest *)timer->req, 0) != 0) {
        timer_cleanup(timer);
        return 0;
    }
    TimerBase = timer->req->tr_node.io_Device;
    {
        struct EClockVal v;
        timer->freq = ReadEClock(&v);
    }
    return timer->freq != 0;
}

static void timer_cleanup(TimerState *timer)
{
    if (timer->req) {
        if (timer->req->tr_node.io_Device)
            CloseDevice((struct IORequest *)timer->req);
        DeleteIORequest((struct IORequest *)timer->req);
    }
    if (timer->port)
        DeleteMsgPort(timer->port);
    memset(timer, 0, sizeof(*timer));
    TimerBase = NULL;
}

static int bstr_to_c(BSTR bstr, char *out, size_t outsz)
{
    UBYTE *b;
    size_t len;

    if (!out || outsz == 0)
        return 0;
    out[0] = '\0';
    if (!bstr)
        return 0;
    b = (UBYTE *)BADDR(bstr);
    if (!b)
        return 0;
    len = b[0];
    if (len >= outsz)
        len = outsz - 1;
    memcpy(out, b + 1, len);
    out[len] = '\0';
    return 1;
}

static void strip_device_colon(const char *device, char *out, size_t outsz)
{
    size_t i = 0;

    if (outsz == 0)
        return;
    while (device[i] && device[i] != ':' && device[i] != '/' &&
           i + 1 < outsz) {
        out[i] = device[i];
        i++;
    }
    out[i] = '\0';
}

static void identify_device(RunContext *ctx)
{
    struct DosList *list;
    struct DosList *node;
    char name[64];

    memset(&ctx->ident, 0, sizeof(ctx->ident));
    strip_device_colon(ctx->cfg.device, name, sizeof(name));
    if (name[0] == '\0')
        return;
    snprintf(ctx->ident.dos_device, sizeof(ctx->ident.dos_device), "%s", name);

    list = LockDosList(LDF_DEVICES | LDF_READ);
    if (!list)
        return;
    node = FindDosEntry(list, (CONST_STRPTR)name, LDF_DEVICES);
    if (node) {
        struct FileSysStartupMsg *fssm = NULL;
        struct DosEnvec *de = NULL;

        bstr_to_c(node->dol_misc.dol_handler.dol_Handler,
                  ctx->ident.handler, sizeof(ctx->ident.handler));
        if (node->dol_misc.dol_handler.dol_Startup)
            fssm = (struct FileSysStartupMsg *)
                BADDR(node->dol_misc.dol_handler.dol_Startup);
        if (fssm) {
            bstr_to_c(fssm->fssm_Device, ctx->ident.exec_device,
                      sizeof(ctx->ident.exec_device));
            ctx->ident.exec_unit = fssm->fssm_Unit;
            ctx->ident.open_flags = fssm->fssm_Flags;
            ctx->ident.have_exec = ctx->ident.exec_device[0] != '\0';
            if (fssm->fssm_Environ)
                de = (struct DosEnvec *)BADDR(fssm->fssm_Environ);
        }
        if (de) {
            ctx->ident.sector_size = de->de_SizeBlock << 2;
            ctx->ident.buffers = de->de_NumBuffers;
            ctx->ident.bufmemtype = de->de_BufMemType;
            ctx->ident.maxtransfer = de->de_MaxTransfer;
            ctx->ident.mask = de->de_Mask;
            ctx->ident.dostype = de->de_DosType;
            bstr_to_c((BSTR)de->de_Control, ctx->ident.control,
                      sizeof(ctx->ident.control));
        }
        snprintf(ctx->ident.source, sizeof(ctx->ident.source),
                 "doslist");
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
}

static void resolve_title(RunContext *ctx)
{
    BPTR lock;
    struct FileInfoBlock *fib;

    snprintf(ctx->title, sizeof(ctx->title), "%s", ctx->cfg.device);
    lock = Lock((CONST_STRPTR)ctx->cfg.device, SHARED_LOCK);
    if (!lock)
        return;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib) {
        if (Examine(lock, fib) && fib->fib_FileName[0] != '\0') {
            snprintf(ctx->title, sizeof(ctx->title), "%s",
                     (char *)fib->fib_FileName);
        }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
}

static void update_hash(uint32_t *hash, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i;

    for (i = 0; i < len; i++) {
        *hash ^= p[i];
        *hash *= 16777619UL;
    }
}

static int cmp_catalog_path(const void *a, const void *b)
{
    const CatalogEntry *ea = (const CatalogEntry *)a;
    const CatalogEntry *eb = (const CatalogEntry *)b;

    return strcmp(ea->path, eb->path);
}

static void build_fingerprint(RunContext *ctx)
{
    CatalogEntry *copy;
    size_t i;
    uint32_t h = 2166136261UL;

    update_hash(&h, ctx->title, strlen(ctx->title));
    copy = (CatalogEntry *)malloc(ctx->catalog.count * sizeof(copy[0]));
    if (!copy) {
        snprintf(ctx->fingerprint, sizeof(ctx->fingerprint), "%08lx",
                 (unsigned long)h);
        return;
    }
    memcpy(copy, ctx->catalog.items, ctx->catalog.count * sizeof(copy[0]));
    qsort(copy, ctx->catalog.count, sizeof(copy[0]), cmp_catalog_path);
    for (i = 0; i < ctx->catalog.count; i++) {
        char buf[64];

        update_hash(&h, copy[i].path, strlen(copy[i].path));
        snprintf(buf, sizeof(buf), ":%u:%lu:%lu",
                 (unsigned)copy[i].kind,
                 (unsigned long)(copy[i].size >> 32),
                 (unsigned long)(copy[i].size & 0xffffffffUL));
        update_hash(&h, buf, strlen(buf));
    }
    free(copy);
    snprintf(ctx->fingerprint, sizeof(ctx->fingerprint), "%08lx",
             (unsigned long)h);
}

static void classify_disc(RunContext *ctx)
{
    if (ctx->catalog.audio_count == 0)
        snprintf(ctx->disc_type, sizeof(ctx->disc_type), "data");
    else if (ctx->catalog.data_count == 0)
        snprintf(ctx->disc_type, sizeof(ctx->disc_type), "audio");
    else
        snprintf(ctx->disc_type, sizeof(ctx->disc_type), "mixed");
}

static int discover_dir_exnext(Catalog *cat, const char *path, unsigned depth)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    int ok = 1;

    lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (!lock)
        return 0;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) {
        UnLock(lock);
        return 0;
    }
    if (!Examine(lock, fib)) {
        FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        return 0;
    }
    while (ExNext(lock, fib)) {
        char *child = join_path(path, (char *)fib->fib_FileName);
        EntryKind kind;

        if (!child) {
            ok = 0;
            break;
        }
        if (fib->fib_DirEntryType > 0) {
            if (!catalog_add(cat, child, (char *)fib->fib_FileName,
                             ENTRY_DIR, 0, depth + 1))
                ok = 0;
            else if (!discover_dir_exnext(cat, child, depth + 1))
                ok = 0;
        } else {
            kind = classify_file(child, (char *)fib->fib_FileName);
            if (!catalog_add(cat, child, (char *)fib->fib_FileName, kind,
                             (uint64_t)(uint32_t)fib->fib_Size, depth + 1))
                ok = 0;
        }
        free(child);
        if (!ok)
            break;
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return ok;
}

static int discover_dir_exall(Catalog *cat, const char *path, unsigned depth)
{
    BPTR lock;
    APTR buf;
    struct ExAllControl *ctrl;
    int ok = 1;
    LONG more;

    lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (!lock)
        return 0;
    buf = AllocVec(EXALL_BUFSIZE, MEMF_ANY);
    ctrl = (struct ExAllControl *)AllocDosObject(DOS_EXALLCONTROL, NULL);
    if (!buf || !ctrl) {
        if (buf)
            FreeVec(buf);
        if (ctrl)
            FreeDosObject(DOS_EXALLCONTROL, ctrl);
        UnLock(lock);
        return 0;
    }

    do {
        struct ExAllData *ed;

        more = ExAll(lock, (struct ExAllData *)buf, EXALL_BUFSIZE,
                     ED_SIZE, ctrl);
        ed = (struct ExAllData *)buf;
        while (ctrl->eac_Entries && ed) {
            char *child = join_path(path, (char *)ed->ed_Name);
            EntryKind kind;

            if (!child) {
                ok = 0;
                break;
            }
            if (ed->ed_Type > 0) {
                if (!catalog_add(cat, child, (char *)ed->ed_Name,
                                 ENTRY_DIR, 0, depth + 1))
                    ok = 0;
                else if (!discover_dir_exall(cat, child, depth + 1))
                    ok = 0;
            } else {
                kind = classify_file(child, (char *)ed->ed_Name);
                if (!catalog_add(cat, child, (char *)ed->ed_Name, kind,
                                 (uint64_t)ed->ed_Size, depth + 1))
                    ok = 0;
            }
            free(child);
            if (!ok)
                break;
            ed = ed->ed_Next;
            ctrl->eac_Entries--;
        }
        if (!ok)
            break;
    } while (more);

    if (!ok || (IoErr() != ERROR_NO_MORE_ENTRIES && IoErr() != 0))
        ok = 0;
    FreeDosObject(DOS_EXALLCONTROL, ctrl);
    FreeVec(buf);
    UnLock(lock);
    return ok;
}

static int discover_catalog(Catalog *cat, const char *root)
{
    catalog_clear(cat);
    if (discover_dir_exall(cat, root, 0))
        return 1;
    catalog_clear(cat);
    cat->used_exnext_fallback = 1;
    return discover_dir_exnext(cat, root, 0);
}

static int discover_catalog_exnext(Catalog *cat, const char *root)
{
    catalog_clear(cat);
    cat->used_exnext_fallback = 1;
    return discover_dir_exnext(cat, root, 0);
}

static void csv_field(const char *s)
{
    const char *p;

    putchar('"');
    if (s) {
        for (p = s; *p; p++) {
            if (*p == '"')
                putchar('"');
            putchar(*p);
        }
    }
    putchar('"');
}

static uint64_t rate_kib_s(uint64_t bytes, uint64_t elapsed_us)
{
    if (elapsed_us == 0)
        return 0;
    return (bytes * 1000000ULL) / elapsed_us / 1024ULL;
}

static uint64_t x_speed_milli(uint64_t kib_s, uint64_t one_x_kib_s)
{
    if (one_x_kib_s == 0)
        return 0;
    return (kib_s * 1000ULL) / one_x_kib_s;
}

static void print_u64_dec(uint64_t v)
{
    char buf[24];
    size_t pos = sizeof(buf);

    buf[--pos] = '\0';
    do {
        buf[--pos] = (char)('0' + (v % 10ULL));
        v /= 10ULL;
    } while (v);
    fputs(&buf[pos], stdout);
}

static void csv_u64(uint64_t v)
{
    print_u64_dec(v);
    putchar(',');
}

static void print_milli(uint64_t v)
{
    print_u64_dec(v / 1000ULL);
    printf(".%03lu", (unsigned long)(v % 1000ULL));
}

static void copy_text(char *dst, size_t dstsz, const char *src)
{
    size_t len;

    if (!dst || dstsz == 0)
        return;
    if (!src)
        src = "";
    len = strlen(src);
    if (len >= dstsz)
        len = dstsz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void copy_leaf_text(char *dst, size_t dstsz, const char *path)
{
    copy_text(dst, dstsz, path ? leaf_name(path) : "");
}

static void summary_record(RunContext *ctx, const Result *r)
{
    Summary *s = ctx->summary;
    int ok;

    if (!s || !r->test)
        return;
    ok = r->status && strcmp(r->status, "ok") == 0;
    if (strcmp(r->test, "discover") == 0) {
        copy_text(s->discover_status, sizeof(s->discover_status),
                  r->status);
        copy_text(s->discover_selector, sizeof(s->discover_selector),
                  r->selector);
        s->discover_bytes = r->bytes;
        s->discover_ops = r->ops;
        s->discover_ops_s = r->ops_s;
        s->discover_elapsed_us = r->elapsed_us;
    } else if (strcmp(r->test, "seq_data") == 0) {
        copy_text(s->seq_data_status, sizeof(s->seq_data_status),
                  r->status);
        copy_text(s->seq_data_error, sizeof(s->seq_data_error), r->error);
        copy_leaf_text(s->seq_data_leaf, sizeof(s->seq_data_leaf), r->path);
        s->seq_data_file_size = r->file_size;
        s->seq_data_bytes = r->bytes;
        s->seq_data_rate_kib_s = r->rate_kib_s;
        s->seq_data_x_milli = r->x_speed_milli;
    } else if (strcmp(r->test, "seq_audio") == 0) {
        copy_text(s->seq_audio_status, sizeof(s->seq_audio_status),
                  r->status);
        copy_text(s->seq_audio_error, sizeof(s->seq_audio_error), r->error);
        copy_leaf_text(s->seq_audio_leaf, sizeof(s->seq_audio_leaf),
                       r->path);
        s->seq_audio_file_size = r->file_size;
        s->seq_audio_bytes = r->bytes;
        s->seq_audio_rate_kib_s = r->rate_kib_s;
        s->seq_audio_x_milli = r->x_speed_milli;
    } else if (strcmp(r->test, "rand_same") == 0 && ok) {
        s->rand_same = r->lat;
        s->have_rand_same = 1;
    } else if (strcmp(r->test, "rand_stride") == 0 && ok) {
        s->rand_stride = r->lat;
        s->have_rand_stride = 1;
    } else if (strcmp(r->test, "rand_seeded") == 0 && ok) {
        s->rand_seeded = r->lat;
        s->have_rand_seeded = 1;
    } else if (strcmp(r->test, "meta_exall") == 0 && ok) {
        s->meta_exall_ops_s = r->ops_s;
        s->meta_exall_elapsed_us = r->elapsed_us;
        s->have_meta_exall = 1;
    } else if (strcmp(r->test, "meta_exnext") == 0 && ok) {
        s->meta_exnext_ops_s = r->ops_s;
        s->meta_exnext_elapsed_us = r->elapsed_us;
        s->have_meta_exnext = 1;
    } else if (strcmp(r->test, "traverse_warm") == 0 && ok) {
        s->traverse_warm_ops_s = r->ops_s;
        s->traverse_warm_elapsed_us = r->elapsed_us;
        s->have_traverse_warm = 1;
    } else if (strcmp(r->test, "meta_lock") == 0 && ok) {
        if (r->access_shape && strcmp(r->access_shape, "deep") == 0) {
            s->meta_lock_deep = r->lat;
            s->have_meta_lock_deep = 1;
        } else {
            s->meta_lock_shallow = r->lat;
            s->have_meta_lock_shallow = 1;
        }
    } else if (strcmp(r->test, "meta_openfromlock") == 0 && ok) {
        s->meta_openfromlock = r->lat;
        s->have_meta_openfromlock = 1;
    } else if (strcmp(r->test, "meta_negative") == 0 && ok) {
        s->meta_negative = r->lat;
        s->have_meta_negative = 1;
    } else if (strcmp(r->test, "parent") == 0 && ok) {
        s->parent = r->lat;
        s->have_parent = 1;
    } else if (strcmp(r->test, "small_open") == 0 && ok) {
        s->small_open = r->lat;
        s->have_small_open = 1;
    } else if (strcmp(r->test, "audio_track_open") == 0 && ok) {
        s->audio_open = r->lat;
        s->have_audio_open = r->lat.avg != 0;
    } else if (strcmp(r->test, "mixed_switch") == 0) {
        copy_text(s->mixed_status, sizeof(s->mixed_status), r->status);
        copy_text(s->mixed_error, sizeof(s->mixed_error), r->error);
        s->mixed = r->lat;
        s->have_mixed = 1;
    } else if (starts_with_ci(r->test, "cache_")) {
        s->cache_rows++;
        if (r->working_set_bytes > s->cache_max_working_set)
            s->cache_max_working_set = r->working_set_bytes;
        if (strcmp(r->test, "cache_seq") == 0)
            s->cache_seq_warm_cold_milli = r->warm_cold_milli;
        else if (strcmp(r->test, "cache_sector") == 0)
            s->cache_sector_warm_cold_milli = r->warm_cold_milli;
        else if (strcmp(r->test, "cache_partial") == 0)
            s->cache_partial_warm_cold_milli = r->warm_cold_milli;
    } else if (strcmp(r->test, "raw_seq_data") == 0) {
        copy_text(s->raw_status, sizeof(s->raw_status), r->status);
        copy_text(s->raw_error, sizeof(s->raw_error), r->error);
        s->raw_seen = 1;
    }
}

static void print_seconds(uint64_t us)
{
    print_u64_dec(us / 1000000ULL);
    printf(".%03lu s", (unsigned long)((us % 1000000ULL) / 1000ULL));
}

static void print_bytes_human(uint64_t bytes)
{
    print_u64_dec(bytes);
    printf(" bytes");
    if (bytes >= 1024ULL * 1024ULL) {
        uint64_t mib = bytes / (1024ULL * 1024ULL);
        uint64_t frac = ((bytes % (1024ULL * 1024ULL)) * 100ULL) /
            (1024ULL * 1024ULL);

        printf(" (");
        print_u64_dec(mib);
        printf(".%02lu MiB)", (unsigned long)frac);
    }
}

static void print_rate_x(uint64_t kib_s, uint64_t x_milli)
{
    print_u64_dec(kib_s);
    printf(" KiB/s");
    if (x_milli) {
        printf(", ");
        print_milli(x_milli);
        printf("x");
    }
}

static void print_latency_summary(const char *label, const LatStats *lat)
{
    printf("  %s: avg ", label);
    print_u64_dec(lat->avg);
    printf(" us, p95 ");
    print_u64_dec(lat->p95);
    printf(" us\n");
}

static void print_read_summary(const char *label, const char *status,
                               const char *error, const char *leaf,
                               uint64_t file_size, uint64_t bytes,
                               uint64_t kib_s, uint64_t x_milli)
{
    printf("  %s: ", label);
    if (!status || status[0] == '\0') {
        printf("not run\n");
        return;
    }
    if (strcmp(status, "skipped") == 0 || strcmp(status, "error") == 0) {
        printf("%s", status);
        if (error && error[0])
            printf(" (%s)", error);
        printf("\n");
        return;
    }
    printf("%s", status);
    if (leaf && leaf[0])
        printf(" %s", leaf);
    printf(", ");
    print_bytes_human(bytes);
    if (file_size) {
        printf(" of ");
        print_bytes_human(file_size);
    }
    printf(", ");
    print_rate_x(kib_s, x_milli);
    printf("\n");
}

static void emit_summary(const RunContext *ctx)
{
    const Summary *s = ctx->summary;

    if (!s || ctx->cfg.csv)
        return;

    printf("\nSummary\n");
    printf("  Disc: %s, title=\"%s\", fingerprint=%s\n",
           ctx->disc_type, ctx->title, ctx->fingerprint);
    printf("  Visible content: %lu data files, %lu audio tracks, %lu dirs, ",
           (unsigned long)ctx->catalog.data_count,
           (unsigned long)ctx->catalog.audio_count,
           (unsigned long)ctx->catalog.dir_count);
    print_bytes_human(ctx->catalog.data_bytes + ctx->catalog.audio_bytes);
    printf("\n");
    if (ctx->ident.have_exec) {
        printf("  Mount: %s unit %lu via %s, sector %lu, buffers %lu",
               ctx->ident.exec_device,
               (unsigned long)ctx->ident.exec_unit,
               ctx->ident.dos_device,
               (unsigned long)ctx->ident.sector_size,
               (unsigned long)ctx->ident.buffers);
        if (ctx->ident.handler[0])
            printf(", handler=%s", ctx->ident.handler);
        if (ctx->ident.control[0])
            printf(", control=%s", ctx->ident.control);
        printf("\n");
    }

    if (s->discover_status[0]) {
        printf("  Discovery: %s, ", s->discover_status);
        print_u64_dec(s->discover_ops);
        printf(" entries in ");
        print_seconds(s->discover_elapsed_us);
        printf(", ");
        print_u64_dec(s->discover_ops_s);
        printf(" entries/s");
        if (s->discover_selector[0])
            printf(" using %s", s->discover_selector);
        printf("\n");
    }

    print_read_summary("Data read", s->seq_data_status,
                       s->seq_data_error, s->seq_data_leaf,
                       s->seq_data_file_size, s->seq_data_bytes,
                       s->seq_data_rate_kib_s, s->seq_data_x_milli);
    print_read_summary("Audio read", s->seq_audio_status,
                       s->seq_audio_error, s->seq_audio_leaf,
                       s->seq_audio_file_size, s->seq_audio_bytes,
                       s->seq_audio_rate_kib_s, s->seq_audio_x_milli);

    if (s->have_rand_seeded || s->have_rand_same ||
        s->have_rand_stride) {
        printf("  Random read latency:\n");
        if (s->have_rand_same)
            print_latency_summary("same offset", &s->rand_same);
        if (s->have_rand_stride)
            print_latency_summary("stride", &s->rand_stride);
        if (s->have_rand_seeded)
            print_latency_summary("seeded", &s->rand_seeded);
    }

    if (s->have_meta_exall || s->have_meta_exnext ||
        s->have_traverse_warm) {
        printf("  Metadata traversal:");
        if (s->have_meta_exall) {
            printf(" ExAll ");
            print_u64_dec(s->meta_exall_ops_s);
            printf(" entries/s");
        }
        if (s->have_meta_exnext) {
            printf(", ExNext ");
            print_u64_dec(s->meta_exnext_ops_s);
            printf(" entries/s");
        }
        if (s->have_traverse_warm) {
            printf(", warm ");
            print_u64_dec(s->traverse_warm_ops_s);
            printf(" entries/s");
        }
        printf("\n");
    }

    if (s->have_meta_lock_shallow || s->have_meta_lock_deep ||
        s->have_meta_openfromlock || s->have_meta_negative ||
        s->have_parent || s->have_small_open) {
        printf("  Metadata latency:\n");
        if (s->have_meta_lock_shallow)
            print_latency_summary("shallow lock", &s->meta_lock_shallow);
        if (s->have_meta_lock_deep)
            print_latency_summary("deep lock", &s->meta_lock_deep);
        if (s->have_meta_openfromlock)
            print_latency_summary("open from lock",
                                  &s->meta_openfromlock);
        if (s->have_meta_negative)
            print_latency_summary("negative lookup",
                                  &s->meta_negative);
        if (s->have_parent)
            print_latency_summary("parent lookup", &s->parent);
        if (s->have_small_open)
            print_latency_summary("small open/read", &s->small_open);
    }

    if (s->have_audio_open)
        print_latency_summary("Audio track open/read", &s->audio_open);
    if (s->have_mixed) {
        printf("  Mixed switch: %s", s->mixed_status);
        if (s->mixed_error[0])
            printf(" (%s)", s->mixed_error);
        if (s->mixed.avg) {
            printf(", avg ");
            print_u64_dec(s->mixed.avg);
            printf(" us, p95 ");
            print_u64_dec(s->mixed.p95);
            printf(" us");
        }
        printf("\n");
    }
    if (s->cache_rows) {
        printf("  Cache probe: %lu rows, largest working set ",
               (unsigned long)s->cache_rows);
        print_bytes_human(s->cache_max_working_set);
        if (s->cache_seq_warm_cold_milli) {
            printf(", stream warm/cold ");
            print_milli(s->cache_seq_warm_cold_milli);
        }
        if (s->cache_sector_warm_cold_milli) {
            printf(", sector ");
            print_milli(s->cache_sector_warm_cold_milli);
        }
        if (s->cache_partial_warm_cold_milli) {
            printf(", partial ");
            print_milli(s->cache_partial_warm_cold_milli);
        }
        printf("\n");
    }
    if (s->raw_seen) {
        printf("  Raw baseline: %s", s->raw_status);
        if (s->raw_error[0])
            printf(" (%s)", s->raw_error);
        printf("\n");
    }
    if (!ctx->cfg.verbose)
        printf("  Use VERBOSE for full per-test rows and paths.\n");
}

static void emit_result(RunContext *ctx, const Result *r)
{
    uint32_t result_pass = r->pass ? r->pass : ctx->current_pass;

    summary_record(ctx, r);

    if (ctx->cfg.csv) {
        csv_field(ctx->title); putchar(',');
        csv_field(ctx->cfg.device); putchar(',');
        csv_field(ctx->ident.dos_device); putchar(',');
        csv_field(ctx->ident.handler); putchar(',');
        csv_field(ctx->ident.exec_device); putchar(',');
        printf("%lu,", (unsigned long)ctx->ident.exec_unit);
        printf("%08lx,", (unsigned long)ctx->ident.dostype);
        printf("%lu,", (unsigned long)ctx->ident.sector_size);
        printf("%lu,", (unsigned long)ctx->ident.buffers);
        csv_field(ctx->ident.control); putchar(',');
        csv_field(ctx->disc_type); putchar(',');
        csv_field(ctx->fingerprint); putchar(',');
        csv_field(r->test); putchar(',');
        csv_field(r->status); putchar(',');
        csv_field(r->error); putchar(',');
        csv_field(r->path); putchar(',');
        csv_field(r->selector); putchar(',');
        csv_u64(result_pass);
        csv_u64(r->seed);
        csv_field(r->access_shape); putchar(',');
        csv_u64(r->file_size);
        csv_u64(r->bytes);
        csv_u64(r->ops);
        csv_u64(r->ops_s);
        csv_u64(r->read_size);
        csv_u64(r->buffer_size);
        csv_u64(r->working_set_bytes);
        csv_u64(r->hot_set_bytes);
        csv_u64(r->eviction_bytes);
        csv_u64(r->elapsed_us);
        csv_u64(r->rate_kib_s);
        csv_u64(r->x_speed_milli);
        csv_u64(r->raw_rate_kib_s);
        csv_u64(r->efficiency_milli);
        csv_u64(r->warm_cold_milli);
        csv_u64(r->lat.avg);
        csv_u64(r->lat.median);
        csv_u64(r->lat.p95);
        csv_u64(r->lat.min);
        print_u64_dec(r->lat.max);
        putchar('\n');
        return;
    }
    if (!ctx->cfg.verbose)
        return;

    printf("%-18s %-8s", r->test, r->status);
    if (r->path && r->path[0])
        printf(" path=%s", r->path);
    if (r->bytes) {
        printf(" bytes=");
        print_u64_dec(r->bytes);
    }
    if (r->ops) {
        printf(" ops=");
        print_u64_dec(r->ops);
    }
    if (r->ops_s) {
        printf(" ops/s=");
        print_u64_dec(r->ops_s);
    }
    if (r->file_size) {
        printf(" file_size=");
        print_u64_dec(r->file_size);
    }
    if (r->elapsed_us) {
        printf(" elapsed_us=");
        print_u64_dec(r->elapsed_us);
    }
    if (r->rate_kib_s) {
        printf(" rate=");
        print_u64_dec(r->rate_kib_s);
        printf(" KiB/s");
    }
    if (r->x_speed_milli) {
        printf(" x=");
        print_milli(r->x_speed_milli);
    }
    if (r->lat.avg) {
        printf(" avg_us=");
        print_u64_dec(r->lat.avg);
        printf(" p95_us=");
        print_u64_dec(r->lat.p95);
    }
    if (r->warm_cold_milli) {
        printf(" warm/cold=");
        print_milli(r->warm_cold_milli);
    }
    if (r->error && r->error[0])
        printf(" (%s)", r->error);
    putchar('\n');
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t aa = *(const uint64_t *)a;
    uint64_t bb = *(const uint64_t *)b;

    if (aa < bb)
        return -1;
    if (aa > bb)
        return 1;
    return 0;
}

static void calc_lat_stats(uint64_t *lat, size_t n, LatStats *out)
{
    size_t i;
    uint64_t sum = 0;

    memset(out, 0, sizeof(*out));
    if (n == 0)
        return;
    qsort(lat, n, sizeof(lat[0]), cmp_u64);
    for (i = 0; i < n; i++)
        sum += lat[i];
    out->avg = sum / (uint64_t)n;
    out->median = lat[n / 2];
    out->p95 = lat[(n * 95) / 100 >= n ? n - 1 : (n * 95) / 100];
    out->min = lat[0];
    out->max = lat[n - 1];
}

static const CatalogEntry *find_largest(const Catalog *cat, EntryKind kind)
{
    const CatalogEntry *best = NULL;
    size_t i;

    for (i = 0; i < cat->count; i++) {
        const CatalogEntry *e = &cat->items[i];

        if (e->kind != kind)
            continue;
        if (!best || e->size > best->size)
            best = e;
    }
    return best;
}

static const CatalogEntry *find_by_path(const Catalog *cat, const char *path,
                                        EntryKind kind)
{
    size_t i;

    if (!path)
        return NULL;
    for (i = 0; i < cat->count; i++) {
        const CatalogEntry *e = &cat->items[i];

        if (e->kind != kind)
            continue;
        if (str_casecmp(e->path, path) == 0)
            return e;
    }
    return NULL;
}

static const CatalogEntry *find_by_suffix(const Catalog *cat,
                                          EntryKind kind,
                                          const char *suffix)
{
    size_t i;

    for (i = 0; i < cat->count; i++) {
        const CatalogEntry *e = &cat->items[i];

        if (e->kind == kind && ends_with_ci(e->path, suffix))
            return e;
    }
    return NULL;
}

static uint64_t examine_file_size(const char *path)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    uint64_t size = 0;

    if (!path)
        return 0;
    lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (!lock)
        return 0;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib) {
        if (Examine(lock, fib) && fib->fib_DirEntryType <= 0)
            size = (uint64_t)(uint32_t)fib->fib_Size;
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    return size;
}

static uint64_t selected_file_size(const Catalog *cat, const char *override,
                                   const CatalogEntry *entry,
                                   EntryKind kind)
{
    const CatalogEntry *explicit_entry;

    if (!override || !override[0])
        return entry ? entry->size : 0;
    explicit_entry = find_by_path(cat, override, kind);
    if (explicit_entry)
        return explicit_entry->size;
    return examine_file_size(override);
}

static const CatalogEntry *select_seq_data_entry(const Catalog *cat,
                                                 const char **selector)
{
    const CatalogEntry *entry;

    entry = find_by_suffix(cat, ENTRY_DATA, "bench/seq.bin");
    if (entry) {
        if (selector)
            *selector = "fixture";
        return entry;
    }
    if (selector)
        *selector = "largest";
    return find_largest(cat, ENTRY_DATA);
}

static const CatalogEntry *select_rand_data_entry(const Catalog *cat,
                                                  const char **selector)
{
    const CatalogEntry *entry;

    entry = find_by_suffix(cat, ENTRY_DATA, "bench/rand.bin");
    if (entry) {
        if (selector)
            *selector = "fixture";
        return entry;
    }
    return select_seq_data_entry(cat, selector);
}

static const CatalogEntry *find_deepest_dir(const Catalog *cat)
{
    const CatalogEntry *best = NULL;
    size_t i;

    for (i = 0; i < cat->count; i++) {
        const CatalogEntry *e = &cat->items[i];

        if (e->kind != ENTRY_DIR)
            continue;
        if (!best || e->depth > best->depth)
            best = e;
    }
    return best;
}

static const CatalogEntry *find_dir_by_rank(const Catalog *cat, unsigned rank)
{
    const CatalogEntry *best = NULL;
    size_t i;
    unsigned target_depth = rank;

    for (i = 0; i < cat->count; i++) {
        const CatalogEntry *e = &cat->items[i];

        if (e->kind != ENTRY_DIR)
            continue;
        if (!best)
            best = e;
        if (rank == 0) {
            if (e->depth < best->depth)
                best = e;
        } else if (rank == 1) {
            if (!best || (e->depth >= target_depth && e->depth < best->depth))
                best = e;
        } else {
            if (e->depth > best->depth)
                best = e;
        }
    }
    return best;
}

static const char *selected_path(const char *override,
                                 const CatalogEntry *entry)
{
    if (override && override[0])
        return override;
    if (entry)
        return entry->path;
    return NULL;
}

static uint64_t seq_read_file(const RunContext *ctx, const char *path,
                              uint32_t bufsize, uint64_t maxbytes,
                              uint32_t seconds, Result *res)
{
    BPTR fh;
    void *buf;
    uint64_t start;
    uint64_t bytes = 0;
    uint64_t elapsed = 0;
    LONG got;
    const char *stop_reason = "full";

    memset(res, 0, sizeof(*res));
    res->path = path;
    res->buffer_size = bufsize;
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        res->status = "error";
        res->error = "open failed";
        return 0;
    }
    buf = malloc(bufsize);
    if (!buf) {
        Close(fh);
        res->status = "error";
        res->error = "out of memory";
        return 0;
    }
    start = timer_now(&ctx->timer);
    for (;;) {
        uint32_t want = bufsize;

        if (maxbytes && bytes >= maxbytes) {
            stop_reason = "maxbytes";
            break;
        }
        if (maxbytes && bytes + want > maxbytes)
            want = (uint32_t)(maxbytes - bytes);
        if (want == 0)
            break;
        got = Read(fh, buf, want);
        if (got < 0) {
            res->status = "error";
            res->error = "read failed";
            break;
        }
        if (got == 0)
            break;
        bytes += (uint32_t)got;
        elapsed = ticks_to_us(&ctx->timer, timer_now(&ctx->timer) - start);
        if (seconds && elapsed >= (uint64_t)seconds * 1000000ULL) {
            stop_reason = "seconds";
            break;
        }
    }
    elapsed = ticks_to_us(&ctx->timer, timer_now(&ctx->timer) - start);
    free(buf);
    Close(fh);
    if (!res->status)
        res->status = stop_reason;
    res->bytes = bytes;
    res->elapsed_us = elapsed;
    res->rate_kib_s = rate_kib_s(bytes, elapsed);
    return bytes;
}

static void run_seq_data(RunContext *ctx)
{
    const char *selector = NULL;
    const CatalogEntry *entry = select_seq_data_entry(&ctx->catalog,
                                                      &selector);
    const char *path = selected_path(ctx->cfg.seq_path, entry);
    uint64_t file_size;
    Result r;

    if (!path) {
        memset(&r, 0, sizeof(r));
        r.test = "seq_data";
        r.status = "skipped";
        r.error = "no non-audio data file";
        emit_result(ctx, &r);
        return;
    }
    file_size = selected_file_size(&ctx->catalog, ctx->cfg.seq_path, entry,
                                   ENTRY_DATA);
    seq_read_file(ctx, path, ctx->cfg.bufsize, ctx->cfg.maxbytes,
                  ctx->cfg.seconds, &r);
    r.test = "seq_data";
    r.selector = ctx->cfg.seq_path ? "override" : selector;
    r.file_size = file_size;
    r.x_speed_milli = x_speed_milli(r.rate_kib_s, 150);
    emit_result(ctx, &r);
}

static void run_seq_audio(RunContext *ctx)
{
    const CatalogEntry *entry = find_largest(&ctx->catalog, ENTRY_AUDIO);
    const char *path = selected_path(ctx->cfg.audio_path, entry);
    Result r;
    BPTR fh;

    if (!path) {
        memset(&r, 0, sizeof(r));
        r.test = "seq_audio";
        r.status = "skipped";
        r.error = "no exported audio-track file";
        emit_result(ctx, &r);
        return;
    }
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (fh) {
        LONG seek_rc = Seek(fh, 0, OFFSET_BEGINNING);

        Close(fh);
        if (seek_rc == -1) {
            memset(&r, 0, sizeof(r));
            r.test = "seq_audio";
            r.status = "skipped";
            r.path = path;
            r.error = "audio file does not support Seek";
            emit_result(ctx, &r);
            return;
        }
    }
    seq_read_file(ctx, path, ctx->cfg.bufsize, ctx->cfg.maxbytes,
                  ctx->cfg.seconds, &r);
    r.test = "seq_audio";
    r.selector = ctx->cfg.audio_path ? "override" : "largest";
    r.file_size = selected_file_size(&ctx->catalog, ctx->cfg.audio_path,
                                     entry, ENTRY_AUDIO);
    r.x_speed_milli = x_speed_milli(r.rate_kib_s, 176);
    emit_result(ctx, &r);
}

static uint32_t lcg_next(uint32_t *state)
{
    *state = *state * 1664525UL + 1013904223UL;
    return *state;
}

static void run_random_pattern(RunContext *ctx, const char *test,
                               const char *path, uint64_t file_size,
                               int pattern, const char *selector)
{
    BPTR fh;
    void *buf;
    uint64_t *lat;
    uint32_t i;
    uint32_t ops = ctx->cfg.iter;
    uint32_t seed = ctx->cfg.seed;
    Result r;

    memset(&r, 0, sizeof(r));
    r.test = test;
    r.path = path;
    r.selector = selector;
    r.read_size = ctx->cfg.readsize;
    r.seed = seed;
    r.file_size = file_size;
    if (!path || file_size < ctx->cfg.readsize || ctx->cfg.readsize == 0) {
        r.status = "skipped";
        r.error = "file too small for random read";
        emit_result(ctx, &r);
        return;
    }
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        r.status = "error";
        r.error = "open failed";
        emit_result(ctx, &r);
        return;
    }
    buf = malloc(ctx->cfg.readsize);
    lat = (uint64_t *)malloc((size_t)ops * sizeof(lat[0]));
    if (!buf || !lat) {
        free(buf);
        free(lat);
        Close(fh);
        r.status = "error";
        r.error = "out of memory";
        emit_result(ctx, &r);
        return;
    }

    for (i = 0; i < ops; i++) {
        uint64_t span = file_size - ctx->cfg.readsize;
        uint64_t off;
        uint64_t start;
        LONG got;

        if (pattern == 0) {
            off = 0;
        } else if (pattern == 1) {
            off = ((uint64_t)i * ctx->cfg.readsize) % (span + 1);
        } else if (pattern == 2) {
            if (i % 3 == 0)
                off = 0;
            else if (i % 3 == 1)
                off = span / 2;
            else
                off = span;
        } else if (pattern == 3) {
            off = ((uint64_t)i * 131072ULL) % (span + 1);
        } else {
            uint64_t blocks = (span / DISC_SECTOR_SIZE) + 1;

            off = ((uint64_t)(lcg_next(&seed) % (uint32_t)blocks)) *
                DISC_SECTOR_SIZE;
            if (off > span)
                off = span;
        }
        Seek(fh, (LONG)off, OFFSET_BEGINNING);
        start = timer_now(&ctx->timer);
        got = Read(fh, buf, ctx->cfg.readsize);
        lat[i] = ticks_to_us(&ctx->timer, timer_now(&ctx->timer) - start);
        if (got > 0)
            r.bytes += (uint32_t)got;
    }
    r.status = "ok";
    r.ops = ops;
    calc_lat_stats(lat, ops, &r.lat);
    emit_result(ctx, &r);
    free(lat);
    free(buf);
    Close(fh);
}

static void run_random_tests(RunContext *ctx)
{
    const char *selector = NULL;
    const CatalogEntry *entry = select_rand_data_entry(&ctx->catalog,
                                                       &selector);
    const char *path = selected_path(ctx->cfg.rand_path, entry);
    uint64_t size = selected_file_size(&ctx->catalog, ctx->cfg.rand_path,
                                       entry, ENTRY_DATA);

    if (!path) {
        Result r;

        memset(&r, 0, sizeof(r));
        r.test = "rand_seeded";
        r.status = "skipped";
        r.error = "no non-audio data file";
        emit_result(ctx, &r);
        return;
    }
    if (ctx->cfg.rand_path)
        selector = "override";
    run_random_pattern(ctx, "rand_same", path, size, 0, selector);
    run_random_pattern(ctx, "rand_adjacent", path, size, 1, selector);
    run_random_pattern(ctx, "rand_edges", path, size, 2, selector);
    run_random_pattern(ctx, "rand_stride", path, size, 3, selector);
    run_random_pattern(ctx, "rand_seeded", path, size, 4, selector);
}

static void timed_discovery_result(RunContext *ctx, uint64_t elapsed_us,
                                   int ok)
{
    Result r;

    memset(&r, 0, sizeof(r));
    r.test = "discover";
    r.status = ok ? "ok" : "error";
    r.error = ok ? "" : "discovery failed";
    r.path = ctx->cfg.device;
    r.selector = ctx->catalog.used_exnext_fallback ? "Examine/ExNext" :
        "ExAll";
    r.bytes = ctx->catalog.data_bytes + ctx->catalog.audio_bytes;
    r.ops = ctx->catalog.data_count + ctx->catalog.audio_count +
        ctx->catalog.audio_meta_count + ctx->catalog.dir_count;
    r.elapsed_us = elapsed_us;
    if (elapsed_us)
        r.ops_s = (r.ops * 1000000ULL) / elapsed_us;
    emit_result(ctx, &r);
}

static void run_warm_traversal(RunContext *ctx, const char *test,
                               const char *path)
{
    Catalog tmp;
    uint64_t start;
    uint64_t elapsed;
    Result r;
    int ok;

    catalog_init(&tmp);
    start = timer_now(&ctx->timer);
    ok = discover_catalog(&tmp, path);
    elapsed = ticks_to_us(&ctx->timer, timer_now(&ctx->timer) - start);
    memset(&r, 0, sizeof(r));
    r.test = test;
    r.status = ok ? "ok" : "error";
    r.error = ok ? "" : "traversal failed";
    r.path = path;
    r.selector = tmp.used_exnext_fallback ? "Examine/ExNext" : "ExAll";
    r.ops = tmp.count;
    r.bytes = tmp.data_bytes + tmp.audio_bytes;
    r.elapsed_us = elapsed;
    if (elapsed)
        r.ops_s = (r.ops * 1000000ULL) / elapsed;
    emit_result(ctx, &r);
    catalog_free(&tmp);
}

static void run_exnext_traversal(RunContext *ctx, const char *test,
                                 const char *path)
{
    Catalog tmp;
    uint64_t start;
    uint64_t elapsed;
    Result r;
    int ok;

    catalog_init(&tmp);
    start = timer_now(&ctx->timer);
    ok = discover_catalog_exnext(&tmp, path);
    elapsed = ticks_to_us(&ctx->timer, timer_now(&ctx->timer) - start);
    memset(&r, 0, sizeof(r));
    r.test = test;
    r.status = ok ? "ok" : "error";
    r.error = ok ? "" : "traversal failed";
    r.path = path;
    r.selector = "Examine/ExNext";
    r.ops = tmp.count;
    r.bytes = tmp.data_bytes + tmp.audio_bytes;
    r.elapsed_us = elapsed;
    if (elapsed)
        r.ops_s = (r.ops * 1000000ULL) / elapsed;
    emit_result(ctx, &r);
    catalog_free(&tmp);
}

static void run_latency_lock(RunContext *ctx, const char *test,
                             const char *path, const char *shape)
{
    uint64_t *lat;
    uint32_t i;
    Result r;

    memset(&r, 0, sizeof(r));
    r.test = test;
    r.path = path;
    r.access_shape = shape;
    lat = (uint64_t *)malloc((size_t)ctx->cfg.iter * sizeof(lat[0]));
    if (!path || !lat) {
        r.status = "skipped";
        r.error = !path ? "no path" : "out of memory";
        emit_result(ctx, &r);
        free(lat);
        return;
    }
    for (i = 0; i < ctx->cfg.iter; i++) {
        uint64_t start = timer_now(&ctx->timer);
        BPTR lock = Lock((CONST_STRPTR)path, SHARED_LOCK);

        lat[i] = ticks_to_us(&ctx->timer, timer_now(&ctx->timer) - start);
        if (lock)
            UnLock(lock);
    }
    r.status = "ok";
    r.ops = ctx->cfg.iter;
    calc_lat_stats(lat, ctx->cfg.iter, &r.lat);
    emit_result(ctx, &r);
    free(lat);
}

static void run_latency_openfromlock(RunContext *ctx, const char *path)
{
    uint64_t *lat;
    uint32_t i;
    Result r;

    memset(&r, 0, sizeof(r));
    r.test = "meta_openfromlock";
    r.path = path;
    lat = (uint64_t *)malloc((size_t)ctx->cfg.iter * sizeof(lat[0]));
    if (!path || !lat) {
        r.status = "skipped";
        r.error = !path ? "no file" : "out of memory";
        emit_result(ctx, &r);
        free(lat);
        return;
    }
    for (i = 0; i < ctx->cfg.iter; i++) {
        uint64_t start = timer_now(&ctx->timer);
        BPTR lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
        BPTR fh = 0;

        if (lock)
            fh = OpenFromLock(lock);
        lat[i] = ticks_to_us(&ctx->timer, timer_now(&ctx->timer) - start);
        if (fh)
            Close(fh);
        else if (lock)
            UnLock(lock);
    }
    r.status = "ok";
    r.ops = ctx->cfg.iter;
    calc_lat_stats(lat, ctx->cfg.iter, &r.lat);
    emit_result(ctx, &r);
    free(lat);
}

static void run_negative_lookup(RunContext *ctx)
{
    char *miss = join_path(ctx->cfg.device, "__cdbench_missing__.info");

    run_latency_lock(ctx, "meta_negative", miss, "miss");
    free(miss);
}

static void run_parent_test(RunContext *ctx, const char *path,
                            const char *shape)
{
    uint64_t *lat;
    uint32_t i;
    BPTR lock;
    Result r;

    memset(&r, 0, sizeof(r));
    r.test = "parent";
    r.path = path;
    r.access_shape = shape;
    if (!path) {
        r.status = "skipped";
        r.error = "no directory";
        emit_result(ctx, &r);
        return;
    }
    lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (!lock) {
        r.status = "error";
        r.error = "lock failed";
        emit_result(ctx, &r);
        return;
    }
    lat = (uint64_t *)malloc((size_t)ctx->cfg.iter * sizeof(lat[0]));
    if (!lat) {
        UnLock(lock);
        r.status = "error";
        r.error = "out of memory";
        emit_result(ctx, &r);
        return;
    }
    for (i = 0; i < ctx->cfg.iter; i++) {
        uint64_t start = timer_now(&ctx->timer);
        BPTR parent = ParentDir(lock);

        lat[i] = ticks_to_us(&ctx->timer, timer_now(&ctx->timer) - start);
        if (parent)
            UnLock(parent);
    }
    r.status = "ok";
    r.ops = ctx->cfg.iter;
    calc_lat_stats(lat, ctx->cfg.iter, &r.lat);
    emit_result(ctx, &r);
    free(lat);
    UnLock(lock);
}

static void run_metadata_tests(RunContext *ctx)
{
    const CatalogEntry *data = find_largest(&ctx->catalog, ENTRY_DATA);
    const CatalogEntry *shallow = find_dir_by_rank(&ctx->catalog, 0);
    const CatalogEntry *deep = find_deepest_dir(&ctx->catalog);
    const char *dir_path = ctx->cfg.dir_path ? ctx->cfg.dir_path :
        ctx->cfg.device;
    const char *parent_path = ctx->cfg.parent_path ? ctx->cfg.parent_path :
        (deep ? deep->path : ctx->cfg.device);

    run_warm_traversal(ctx, "meta_exall", dir_path);
    run_exnext_traversal(ctx, "meta_exnext", dir_path);
    run_warm_traversal(ctx, "traverse_warm", dir_path);
    run_latency_lock(ctx, "meta_lock",
                     shallow ? shallow->path : ctx->cfg.device, "shallow");
    run_latency_lock(ctx, "meta_lock",
                     deep ? deep->path : ctx->cfg.device, "deep");
    run_latency_openfromlock(ctx, data ? data->path : NULL);
    run_negative_lookup(ctx);
    run_parent_test(ctx, parent_path,
                    ctx->cfg.parent_path ? "override" : "deep");
}

static void run_small_open(RunContext *ctx)
{
    uint64_t *lat;
    void *buf;
    uint32_t done = 0;
    size_t i;
    Result r;

    memset(&r, 0, sizeof(r));
    r.test = "small_open";
    lat = (uint64_t *)malloc((size_t)ctx->cfg.iter * sizeof(lat[0]));
    buf = malloc(ctx->cfg.readsize);
    if (!lat || !buf) {
        r.status = "error";
        r.error = "out of memory";
        emit_result(ctx, &r);
        free(lat);
        free(buf);
        return;
    }
    for (i = 0; i < ctx->catalog.count && done < ctx->cfg.iter; i++) {
        const CatalogEntry *e = &ctx->catalog.items[i];
        uint64_t start;
        BPTR fh;
        LONG got = 0;

        if (e->kind != ENTRY_DATA)
            continue;
        start = timer_now(&ctx->timer);
        fh = Open((CONST_STRPTR)e->path, MODE_OLDFILE);
        if (fh) {
            got = Read(fh, buf, ctx->cfg.readsize);
            Close(fh);
        }
        lat[done] = ticks_to_us(&ctx->timer, timer_now(&ctx->timer) - start);
        if (got > 0)
            r.bytes += (uint32_t)got;
        done++;
    }
    if (done == 0) {
        r.status = "skipped";
        r.error = "no data files";
    } else {
        r.status = "ok";
        r.ops = done;
        calc_lat_stats(lat, done, &r.lat);
    }
    emit_result(ctx, &r);
    free(lat);
    free(buf);
}

static void run_audio_open(RunContext *ctx)
{
    uint64_t *lat;
    void *buf;
    uint32_t done = 0;
    size_t i;
    Result r;

    memset(&r, 0, sizeof(r));
    r.test = "audio_track_open";
    lat = (uint64_t *)malloc((size_t)ctx->cfg.iter * sizeof(lat[0]));
    buf = malloc(ctx->cfg.readsize);
    if (!lat || !buf) {
        r.status = "error";
        r.error = "out of memory";
        emit_result(ctx, &r);
        free(lat);
        free(buf);
        return;
    }
    for (i = 0; i < ctx->catalog.count && done < ctx->cfg.iter; i++) {
        const CatalogEntry *e = &ctx->catalog.items[i];
        uint64_t start;
        BPTR fh;
        LONG got = 0;

        if (e->kind != ENTRY_AUDIO)
            continue;
        start = timer_now(&ctx->timer);
        fh = Open((CONST_STRPTR)e->path, MODE_OLDFILE);
        if (fh) {
            got = Read(fh, buf, ctx->cfg.readsize);
            Close(fh);
        }
        lat[done] = ticks_to_us(&ctx->timer, timer_now(&ctx->timer) - start);
        if (got > 0)
            r.bytes += (uint32_t)got;
        done++;
    }
    if (done == 0) {
        r.status = "skipped";
        r.error = "no audio tracks";
    } else {
        r.status = "ok";
        r.ops = done;
        calc_lat_stats(lat, done, &r.lat);
    }
    emit_result(ctx, &r);
    free(lat);
    free(buf);
}

static void run_mixed_switch(RunContext *ctx)
{
    const CatalogEntry *data = find_largest(&ctx->catalog, ENTRY_DATA);
    const CatalogEntry *audio = find_largest(&ctx->catalog, ENTRY_AUDIO);
    BPTR dfh;
    BPTR afh;
    void *buf;
    uint64_t *lat;
    uint32_t i;
    Result r;

    memset(&r, 0, sizeof(r));
    r.test = "mixed_switch";
    if (!data || !audio) {
        r.status = "skipped";
        r.error = "requires data and audio file";
        emit_result(ctx, &r);
        return;
    }
    dfh = Open((CONST_STRPTR)data->path, MODE_OLDFILE);
    afh = Open((CONST_STRPTR)audio->path, MODE_OLDFILE);
    buf = malloc(ctx->cfg.readsize);
    lat = (uint64_t *)malloc((size_t)ctx->cfg.iter * sizeof(lat[0]));
    if (!dfh || !afh || !buf || !lat) {
        if (dfh)
            Close(dfh);
        if (afh)
            Close(afh);
        free(buf);
        free(lat);
        r.status = "error";
        r.error = "open or allocation failed";
        emit_result(ctx, &r);
        return;
    }
    for (i = 0; i < ctx->cfg.iter; i++) {
        BPTR fh = (i & 1) ? afh : dfh;
        uint64_t start;
        LONG got;

        Seek(fh, 0, OFFSET_BEGINNING);
        start = timer_now(&ctx->timer);
        got = Read(fh, buf, ctx->cfg.readsize);
        lat[i] = ticks_to_us(&ctx->timer, timer_now(&ctx->timer) - start);
        if (got > 0)
            r.bytes += (uint32_t)got;
    }
    r.status = "ok";
    r.path = data->path;
    r.ops = ctx->cfg.iter;
    r.read_size = ctx->cfg.readsize;
    calc_lat_stats(lat, ctx->cfg.iter, &r.lat);
    emit_result(ctx, &r);
    free(lat);
    free(buf);
    Close(dfh);
    Close(afh);
}

static TimedRead read_range_once(const RunContext *ctx, const char *path,
                                 uint64_t bytes, uint32_t chunk,
                                 const char *shape)
{
    BPTR fh;
    void *buf;
    uint64_t start;
    uint64_t done = 0;
    TimedRead tr;

    memset(&tr, 0, sizeof(tr));

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return tr;
    buf = malloc(chunk);
    if (!buf) {
        Close(fh);
        return tr;
    }
    start = timer_now(&ctx->timer);
    while (done < bytes) {
        uint32_t want = chunk;
        LONG got;

        if (done + want > bytes)
            want = (uint32_t)(bytes - done);
        if (shape && strcmp(shape, "partial") == 0) {
            Seek(fh, (LONG)done, OFFSET_BEGINNING);
            want = want > 512 ? 512 : want;
            done += DISC_SECTOR_SIZE;
        } else if (shape && strcmp(shape, "sector") == 0) {
            Seek(fh, (LONG)done, OFFSET_BEGINNING);
            want = DISC_SECTOR_SIZE;
            done += DISC_SECTOR_SIZE;
        } else {
            done += want;
        }
        got = Read(fh, buf, want);
        if (got <= 0)
            break;
        tr.bytes_read += (uint32_t)got;
    }
    free(buf);
    Close(fh);
    tr.elapsed_us = ticks_to_us(&ctx->timer, timer_now(&ctx->timer) - start);
    return tr;
}

static void emit_cache_pair(RunContext *ctx, const CatalogEntry *entry,
                            uint64_t working_set, const char *shape,
                            const char *test)
{
    TimedRead cold;
    TimedRead warm;
    Result r;
    uint32_t chunk = ctx->cfg.bufsize;

    if (strcmp(shape, "sector") == 0 || strcmp(shape, "partial") == 0)
        chunk = DISC_SECTOR_SIZE;
    cold = read_range_once(ctx, entry->path, working_set, chunk, shape);
    warm = read_range_once(ctx, entry->path, working_set, chunk, shape);
    memset(&r, 0, sizeof(r));
    r.test = test;
    r.status = "ok";
    r.path = entry->path;
    r.access_shape = shape;
    r.file_size = entry->size;
    r.working_set_bytes = working_set;
    r.bytes = warm.bytes_read;
    r.elapsed_us = warm.elapsed_us;
    r.rate_kib_s = rate_kib_s(warm.bytes_read, warm.elapsed_us);
    if (cold.elapsed_us)
        r.warm_cold_milli = (warm.elapsed_us * 1000ULL) / cold.elapsed_us;
    emit_result(ctx, &r);
}

static void run_cache_probe(RunContext *ctx)
{
    const CatalogEntry *entry = find_largest(&ctx->catalog, ENTRY_DATA);
    uint64_t ws;

    if (!ctx->cfg.cache)
        return;
    if (!entry || entry->size < 32768) {
        Result r;

        memset(&r, 0, sizeof(r));
        r.test = "cache_seq";
        r.status = "skipped";
        r.error = "no large data file";
        emit_result(ctx, &r);
        return;
    }
    for (ws = 32768; ws <= entry->size && ws <= MAX_CACHE_PROBE; ws <<= 1) {
        emit_cache_pair(ctx, entry, ws, "stream", "cache_seq");
        emit_cache_pair(ctx, entry, ws, "sector", "cache_sector");
        emit_cache_pair(ctx, entry, ws, "partial", "cache_partial");
        if (ws > (1ULL << 30))
            break;
    }
}

static void run_raw_stub(RunContext *ctx)
{
    Result r;

    if (!ctx->cfg.raw)
        return;
    memset(&r, 0, sizeof(r));
    r.test = "raw_seq_data";
    r.status = "skipped";
    r.error = "raw SCSI baseline LBA equivalence not implemented in v1";
    emit_result(ctx, &r);
    memset(&r, 0, sizeof(r));
    r.test = "seq_data_efficiency";
    r.status = "skipped";
    r.error = "raw denominator unavailable";
    emit_result(ctx, &r);
}

enum {
    ARG_DEVICE,
    ARG_SEQ,
    ARG_RAND,
    ARG_AUDIO,
    ARG_CDDA,
    ARG_DIR,
    ARG_PARENT,
    ARG_BUFSIZE,
    ARG_READSIZE,
    ARG_MAXBYTES,
    ARG_SECONDS,
    ARG_ITER,
    ARG_PASSES,
    ARG_SEED,
    ARG_RAW,
    ARG_CACHE,
    ARG_CSV,
    ARG_VERBOSE,
    ARG_COUNT
};

static int parse_args(Config *cfg)
{
    LONG args[ARG_COUNT];
    struct RDArgs *rdargs;
    int ok = 1;

    memset(cfg, 0, sizeof(*cfg));
    cfg->bufsize = DEFAULT_BUFSIZE;
    cfg->readsize = DEFAULT_READSIZE;
    cfg->iter = DEFAULT_ITER;
    cfg->passes = DEFAULT_PASSES;
    cfg->seed = 1;
    memset(args, 0, sizeof(args));
    rdargs = ReadArgs((CONST_STRPTR)
        "DEVICE/A,SEQ/K,RAND/K,AUDIO/K,CDDA/K,DIR/K,PARENT/K,"
        "BUFSIZE/N,READSIZE/N,MAXBYTES/N,SECONDS/N,ITER/N,"
        "PASSES/N,SEED/N,RAW/S,CACHE/S,CSV/S,VERBOSE/S",
        args, NULL);
    if (!rdargs)
        return 0;
    if (args[ARG_DEVICE]) {
        cfg->device = xstrdup((const char *)args[ARG_DEVICE]);
        ok = cfg->device != NULL;
    }
    if (ok && args[ARG_SEQ]) {
        cfg->seq_path = xstrdup((const char *)args[ARG_SEQ]);
        ok = cfg->seq_path != NULL;
    }
    if (ok && args[ARG_RAND]) {
        cfg->rand_path = xstrdup((const char *)args[ARG_RAND]);
        ok = cfg->rand_path != NULL;
    }
    if (ok && (args[ARG_AUDIO] || args[ARG_CDDA])) {
        cfg->audio_path = xstrdup((const char *)(args[ARG_AUDIO] ?
            args[ARG_AUDIO] : args[ARG_CDDA]));
        ok = cfg->audio_path != NULL;
    }
    if (ok && args[ARG_DIR]) {
        cfg->dir_path = xstrdup((const char *)args[ARG_DIR]);
        ok = cfg->dir_path != NULL;
    }
    if (ok && args[ARG_PARENT]) {
        cfg->parent_path = xstrdup((const char *)args[ARG_PARENT]);
        ok = cfg->parent_path != NULL;
    }
    if (args[ARG_BUFSIZE])
        cfg->bufsize = (uint32_t)*(LONG *)args[ARG_BUFSIZE];
    if (args[ARG_READSIZE])
        cfg->readsize = (uint32_t)*(LONG *)args[ARG_READSIZE];
    if (args[ARG_MAXBYTES])
        cfg->maxbytes = (uint64_t)*(LONG *)args[ARG_MAXBYTES];
    if (args[ARG_SECONDS])
        cfg->seconds = (uint32_t)*(LONG *)args[ARG_SECONDS];
    if (args[ARG_ITER])
        cfg->iter = (uint32_t)*(LONG *)args[ARG_ITER];
    if (args[ARG_PASSES])
        cfg->passes = (uint32_t)*(LONG *)args[ARG_PASSES];
    if (args[ARG_SEED])
        cfg->seed = (uint32_t)*(LONG *)args[ARG_SEED];
    cfg->raw = args[ARG_RAW] != 0;
    cfg->cache = args[ARG_CACHE] != 0;
    cfg->csv = args[ARG_CSV] != 0;
    cfg->verbose = args[ARG_VERBOSE] != 0;
    if (cfg->bufsize == 0)
        cfg->bufsize = DEFAULT_BUFSIZE;
    if (cfg->readsize == 0)
        cfg->readsize = DEFAULT_READSIZE;
    if (cfg->iter == 0)
        cfg->iter = DEFAULT_ITER;
    if (cfg->passes == 0)
        cfg->passes = DEFAULT_PASSES;
    FreeArgs(rdargs);
    if (!ok) {
        config_cleanup(cfg);
        return 0;
    }
    return 1;
}

static void config_cleanup(Config *cfg)
{
    free(cfg->device);
    free(cfg->seq_path);
    free(cfg->rand_path);
    free(cfg->audio_path);
    free(cfg->dir_path);
    free(cfg->parent_path);
    memset(cfg, 0, sizeof(*cfg));
}

static void print_csv_header(void)
{
    puts("title,device,dos_device,handler,exec_device,exec_unit,dostype,sector_size,"
         "buffers,control,disc_type,disc_fingerprint,test,status,error,path,"
         "selector,pass,seed,access_shape,file_size,bytes,ops,ops_s,read_size,"
         "buffer_size,working_set_bytes,hot_set_bytes,eviction_bytes,"
         "elapsed_us,rate_kib_s,x_speed,raw_rate_kib_s,efficiency_pct,"
         "warm_cold_ratio,avg_us,median_us,p95_us,min_us,max_us");
}

int main(void)
{
    RunContext ctx;
    Summary summary;
    uint64_t start;
    uint64_t elapsed;
    int ok;
    uint32_t pass;

    memset(&ctx, 0, sizeof(ctx));
    memset(&summary, 0, sizeof(summary));
    ctx.summary = &summary;
    if (!parse_args(&ctx.cfg)) {
        PrintFault(IoErr(), (CONST_STRPTR)"CDBench");
        return RETURN_ERROR;
    }
    if (!timer_init(&ctx.timer)) {
        fprintf(stderr, "CDBench: could not open timer.device UNIT_ECLOCK\n");
        config_cleanup(&ctx.cfg);
        return RETURN_ERROR;
    }
    catalog_init(&ctx.catalog);
    identify_device(&ctx);
    resolve_title(&ctx);

    if (ctx.cfg.csv)
        print_csv_header();
    else {
        printf("CDBench device=%s title=\"%s\" timer_freq=%lu\n",
               ctx.cfg.device, ctx.title, (unsigned long)ctx.timer.freq);
        if (ctx.ident.have_exec)
            printf("Backing device: %s unit %lu dos=%s buffers=%lu "
                   "sector=%lu handler=%s control=%s\n",
                   ctx.ident.exec_device,
                   (unsigned long)ctx.ident.exec_unit,
                   ctx.ident.dos_device,
                   (unsigned long)ctx.ident.buffers,
                   (unsigned long)ctx.ident.sector_size,
                   ctx.ident.handler,
                   ctx.ident.control);
        if (!ctx.cfg.verbose)
            printf("Running benchmarks...\n");
    }

    start = timer_now(&ctx.timer);
    ok = discover_catalog(&ctx.catalog, ctx.cfg.device);
    elapsed = ticks_to_us(&ctx.timer, timer_now(&ctx.timer) - start);
    classify_disc(&ctx);
    build_fingerprint(&ctx);
    timed_discovery_result(&ctx, elapsed, ok);
    if (!ok) {
        catalog_free(&ctx.catalog);
        timer_cleanup(&ctx.timer);
        config_cleanup(&ctx.cfg);
        return RETURN_ERROR;
    }
    if (!ctx.cfg.csv && ctx.cfg.verbose)
        printf("Disc type=%s fingerprint=%s files=%lu audio=%lu dirs=%lu\n",
               ctx.disc_type, ctx.fingerprint,
               (unsigned long)ctx.catalog.data_count,
               (unsigned long)ctx.catalog.audio_count,
               (unsigned long)ctx.catalog.dir_count);

    for (pass = 0; pass < ctx.cfg.passes; pass++) {
        ctx.current_pass = pass + 1;
        run_seq_data(&ctx);
        run_seq_audio(&ctx);
        run_audio_open(&ctx);
        run_random_tests(&ctx);
        run_metadata_tests(&ctx);
        run_small_open(&ctx);
        run_mixed_switch(&ctx);
        run_cache_probe(&ctx);
        run_raw_stub(&ctx);
    }

    emit_summary(&ctx);
    catalog_free(&ctx.catalog);
    timer_cleanup(&ctx.timer);
    config_cleanup(&ctx.cfg);
    return RETURN_OK;
}
