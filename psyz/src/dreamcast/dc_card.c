// PS1 memory cards on Dreamcast VMUs. Card 1 (bu00) is the VMU in
// controller port A, card 2 (bu10) the one in port B. The cards are a small
// KOS filesystem at /pscard/0 and /pscard/1, so the libapi file calls in
// plat_unix.c and the game's stdio work on them unchanged: each card is a
// list of files in RAM, read from its VMU on first use, and a file is written
// back to the VMU when it is closed after a write or renamed to its final
// name. Names starting with '.' (the game's temporary save files) stay in RAM.
//
// A PS1 save becomes one VMU file named PE1_<hash of the PS1 name>: a VMS
// header (the save's own icon and title, so the Dreamcast's file manager
// shows them) followed by "PSXC", the 20-byte PS1 name and the PS1 data.
#include <psyz/log.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dc/maple.h>
#include <dc/vmu_pkg.h>
#include <dc/vmufs.h>
#include <kos/fs.h>
#include <kos/nmmgr.h>

// psyz.h is left out: it renames open, close, read and write, which are
// also the names of the VFS handler's members
void Psyz_AdjustPathCB(int (*callback)(char* dst, const char* src, int maxlen));

#define CARD_PORTS 2
#define CARD_FILES 24 // 15 saves, plus temporary files during a save
#define PS1_NAME 20
#define VMU_PREFIX "PE1_"
#define MAGIC "PSXC"
#define PAYLOAD_HEADER (4 + PS1_NAME)

typedef struct CardFile {
    char name[PS1_NAME + 1];
    uint8_t* data;
    size_t size, capacity;
    int open_count;
    bool removed; // unlinked while open: freed on the last close
} CardFile;

typedef struct {
    maple_device_t* dev;
    bool loaded;
    CardFile* files[CARD_FILES];
} Card;

typedef struct {
    bool dir;
    int card;
    // file
    CardFile* file;
    size_t pos;
    bool writable, written;
    // directory
    int next;
    dirent_t dirent;
} Handle;

static Card cards[CARD_PORTS];

static maple_device_t* FindVmu(int port) {
    for (int unit = 1; unit <= 2; unit++) {
        maple_device_t* dev = maple_enum_dev(port, unit);
        if (dev && (dev->info.functions & MAPLE_FUNC_MEMCARD)) {
            return dev;
        }
    }
    return NULL;
}

static void FreeFile(CardFile* f) {
    if (f->open_count) {
        f->removed = true;
        return;
    }
    free(f->data);
    free(f);
}

static void Forget(Card* c) {
    for (int i = 0; i < CARD_FILES; i++) {
        if (c->files[i]) {
            FreeFile(c->files[i]);
            c->files[i] = NULL;
        }
    }
    c->loaded = false;
}

static void VmuName(const char* name, char out[13]) {
    uint32_t h = 2166136261u;
    for (const char* p = name; *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    snprintf(out, 13, VMU_PREFIX "%08lX", (unsigned long)h);
}

static void Load(Card* c) {
    vmu_dir_t* dir = NULL;
    int count = 0;
    c->loaded = true;
    if (vmufs_readdir(c->dev, &dir, &count) < 0) {
        return;
    }
    int n = 0;
    for (int i = 0; i < count && n < CARD_FILES; i++) {
        char vmu_name[13];
        memcpy(vmu_name, dir[i].filename, 12);
        vmu_name[12] = '\0';
        if (!dir[i].filetype || strncmp(vmu_name, VMU_PREFIX, 4)) {
            continue;
        }
        void* raw = NULL;
        int raw_size = 0;
        vmu_pkg_t pkg;
        if (vmufs_read(c->dev, vmu_name, &raw, &raw_size) < 0) {
            continue;
        }
        if (vmu_pkg_parse(raw, (size_t)raw_size, &pkg) == 0 &&
            pkg.data_len >= PAYLOAD_HEADER && !memcmp(pkg.data, MAGIC, 4)) {
            CardFile* f = calloc(1, sizeof(*f));
            size_t size = (size_t)pkg.data_len - PAYLOAD_HEADER;
            if (f && (f->data = malloc(size ? size : 1))) {
                memcpy(f->name, pkg.data + 4, PS1_NAME);
                memcpy(f->data, pkg.data + PAYLOAD_HEADER, size);
                f->size = f->capacity = size;
                c->files[n++] = f;
            } else {
                free(f);
            }
        }
        free(raw);
    }
    free(dir);
    INFOF("memory card %c: %d saves on the VMU", 'A' + (int)(c - cards), n);
}

// the card of port, read from its VMU when first used; NULL without a VMU
static Card* GetCard(int port) {
    if (port < 0 || port >= CARD_PORTS) {
        return NULL;
    }
    Card* c = &cards[port];
    maple_device_t* dev = FindVmu(port);
    if (dev != c->dev) {
        Forget(c);
        c->dev = dev;
    }
    if (!dev) {
        return NULL;
    }
    if (!c->loaded) {
        Load(c);
    }
    return c;
}

int DcCard_Present(int port) { return FindVmu(port) != NULL; }

// --- the VMU file of a save ---

// PS1 4bpp 16x16 icon (CLUT at 0x60, pixels at 0x80) as a 32x32 VMU icon
static bool MakeIcon(const CardFile* f, vmu_pkg_t* pkg, uint8_t* icon) {
    const uint8_t* d = f->data;
    if (f->size < 0x100 || d[0] != 'S' || d[1] != 'C') {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        unsigned c = d[0x60 + i * 2] | (d[0x61 + i * 2] << 8);
        unsigned r = (c & 31) >> 1, g = ((c >> 5) & 31) >> 1,
                 b = ((c >> 10) & 31) >> 1;
        pkg->icon_pal[i] = (uint16_t)((c ? 0xF000 : 0) | r << 8 | g << 4 | b);
    }
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x += 2) {
            uint8_t px = d[0x80 + (y / 2) * 8 + x / 4];
            uint8_t v = (x / 2) & 1 ? px >> 4 : px & 15;
            icon[y * 16 + x / 2] = (uint8_t)(v << 4 | v);
        }
    }
    pkg->icon_cnt = 1;
    pkg->icon_data = icon;
    return true;
}

// the save's Shift-JIS title (full-width letters and digits) in ASCII
static void MakeTitle(const CardFile* f, char* out, size_t max) {
    size_t n = 0;
    if (f->size >= 0x44 && f->data[0] == 'S' && f->data[1] == 'C') {
        const uint8_t* s = f->data + 4;
        const uint8_t* end = f->data + 0x44;
        while (s < end && *s && n + 1 < max) {
            if (*s < 0x80) {
                out[n++] = (char)*s++;
                continue;
            }
            if (s + 1 >= end) {
                break;
            }
            unsigned code = s[0] << 8 | s[1];
            s += 2;
            char c = 0;
            if (code >= 0x824F && code <= 0x8258) {
                c = (char)('0' + code - 0x824F);
            } else if (code >= 0x8260 && code <= 0x8279) {
                c = (char)('A' + code - 0x8260);
            } else if (code >= 0x8281 && code <= 0x829A) {
                c = (char)('a' + code - 0x8281);
            } else {
                switch (code) {
                case 0x8140: c = ' '; break;
                case 0x8146: c = ':'; break;
                case 0x815E: c = '/'; break;
                case 0x817C: c = '-'; break;
                case 0x8144: c = '.'; break;
                }
            }
            if (c) {
                out[n++] = c;
            }
        }
    }
    if (!n) {
        snprintf(out, max, "%s", f->name);
        return;
    }
    out[n] = '\0';
}

static int Persist(Card* c, const CardFile* f) {
    size_t size = PAYLOAD_HEADER + f->size;
    uint8_t* payload = malloc(size);
    if (!payload) {
        return -1;
    }
    memcpy(payload, MAGIC, 4);
    memset(payload + 4, 0, PS1_NAME);
    memcpy(payload + 4, f->name, strnlen(f->name, PS1_NAME));
    memcpy(payload + PAYLOAD_HEADER, f->data, f->size);

    vmu_pkg_t pkg;
    static uint8_t icon[512];
    memset(&pkg, 0, sizeof(pkg));
    strcpy(pkg.desc_short, "Parasite Eve");
    MakeTitle(f, pkg.desc_long, 32);
    strcpy(pkg.app_id, "PARASITE EVE DC");
    if (!MakeIcon(f, &pkg, icon)) {
        pkg.icon_cnt = 0;
    }
    pkg.eyecatch_type = VMUPKG_EC_NONE;
    pkg.data_len = (int)size;
    pkg.data = payload;
    uint8_t* out = NULL;
    int out_size = 0;
    int rv = vmu_pkg_build(&pkg, &out, &out_size);
    free(payload);
    if (rv < 0) {
        return -1;
    }
    char name[13];
    VmuName(f->name, name);
    rv = vmufs_write(c->dev, name, out, out_size, VMUFS_OVERWRITE);
    free(out);
    if (rv < 0) {
        WARNF("memory card %c: no room on the VMU for %s (%d bytes)",
              'A' + (int)(c - cards), f->name, out_size);
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static void Unpersist(Card* c, const CardFile* f) {
    char name[13];
    VmuName(f->name, name);
    vmufs_delete(c->dev, name);
}

static inline bool Kept(const char* name) { return name[0] != '.'; }

// --- the filesystem ---

// "/<port>[/<name>]": the port, and the name (NULL for the card itself)
static int ParsePath(const char* fn, const char** name) {
    while (*fn == '/') {
        fn++;
    }
    if ((fn[0] != '0' && fn[0] != '1') || (fn[1] && fn[1] != '/')) {
        return -1;
    }
    int port = fn[0] - '0';
    fn++;
    while (*fn == '/') {
        fn++;
    }
    *name = *fn ? fn : NULL;
    return port;
}

static int FindFile(const Card* c, const char* name) {
    for (int i = 0; i < CARD_FILES; i++) {
        if (c->files[i] && !strncmp(c->files[i]->name, name, PS1_NAME)) {
            return i;
        }
    }
    return -1;
}

static void* CardOpen(vfs_handler_t* vfs, const char* fn, int mode) {
    (void)vfs;
    const char* name;
    int port = ParsePath(fn, &name);
    Card* c = port < 0 ? NULL : GetCard(port);
    if (!c) {
        errno = ENOENT;
        return NULL;
    }
    Handle* h = calloc(1, sizeof(*h));
    if (!h) {
        errno = ENOMEM;
        return NULL;
    }
    h->card = port;
    if (!name) {
        if (!(mode & O_DIR)) {
            free(h);
            errno = EISDIR;
            return NULL;
        }
        h->dir = true;
        return h;
    }
    int access = mode & O_MODE_MASK;
    int i = FindFile(c, name);
    if (i >= 0 && (mode & O_CREAT) && (mode & O_EXCL)) {
        free(h);
        errno = EEXIST;
        return NULL;
    }
    if (i < 0) {
        if (!(mode & O_CREAT)) {
            free(h);
            errno = ENOENT;
            return NULL;
        }
        for (i = 0; i < CARD_FILES && c->files[i]; i++) {
        }
        CardFile* f = i < CARD_FILES ? calloc(1, sizeof(*f)) : NULL;
        if (!f) {
            free(h);
            errno = ENOSPC;
            return NULL;
        }
        strncpy(f->name, name, PS1_NAME);
        c->files[i] = f;
        h->written = Kept(f->name); // a new file is saved even if empty
    }
    h->file = c->files[i];
    h->writable = access == O_WRONLY || access == O_RDWR;
    if (h->writable && (mode & O_TRUNC)) {
        h->file->size = 0;
        h->written = true;
    }
    if (mode & O_APPEND) {
        h->pos = h->file->size;
    }
    h->file->open_count++;
    return h;
}

static int CardClose(void* hnd) {
    Handle* h = hnd;
    int rv = 0;
    if (!h->dir) {
        CardFile* f = h->file;
        f->open_count--;
        if (f->removed) {
            if (!f->open_count) {
                FreeFile(f);
            }
        } else if (h->written && Kept(f->name)) {
            Card* c = GetCard(h->card);
            rv = c ? Persist(c, f) : -1;
        }
    }
    free(h);
    return rv;
}

static ssize_t CardRead(void* hnd, void* buf, size_t n) {
    Handle* h = hnd;
    if (h->dir) {
        errno = EISDIR;
        return -1;
    }
    size_t size = h->file->size;
    if (h->pos >= size) {
        return 0;
    }
    if (n > size - h->pos) {
        n = size - h->pos;
    }
    memcpy(buf, h->file->data + h->pos, n);
    h->pos += n;
    return (ssize_t)n;
}

static ssize_t CardWrite(void* hnd, const void* buf, size_t n) {
    Handle* h = hnd;
    if (h->dir || !h->writable) {
        errno = EBADF;
        return -1;
    }
    CardFile* f = h->file;
    size_t end = h->pos + n;
    if (end > f->capacity) {
        size_t cap = f->capacity ? f->capacity : 8192;
        while (cap < end) {
            cap *= 2;
        }
        uint8_t* data = realloc(f->data, cap);
        if (!data) {
            errno = ENOSPC;
            return -1;
        }
        f->data = data;
        f->capacity = cap;
    }
    if (h->pos > f->size) {
        memset(f->data + f->size, 0, h->pos - f->size);
    }
    memcpy(f->data + h->pos, buf, n);
    h->pos = end;
    if (end > f->size) {
        f->size = end;
    }
    h->written = true;
    return (ssize_t)n;
}

static off_t CardSeek(void* hnd, off_t offset, int whence) {
    Handle* h = hnd;
    if (h->dir) {
        errno = EISDIR;
        return -1;
    }
    off_t base = whence == SEEK_CUR   ? (off_t)h->pos
                 : whence == SEEK_END ? (off_t)h->file->size
                                      : 0;
    if (base + offset < 0) {
        errno = EINVAL;
        return -1;
    }
    h->pos = (size_t)(base + offset);
    return (off_t)h->pos;
}

static off_t CardTell(void* hnd) {
    Handle* h = hnd;
    return h->dir ? -1 : (off_t)h->pos;
}

static size_t CardTotal(void* hnd) {
    Handle* h = hnd;
    return h->dir ? 0 : h->file->size;
}

static const dirent_t* CardReaddir(void* hnd) {
    Handle* h = hnd;
    Card* c = &cards[h->card];
    if (!h->dir) {
        errno = ENOTDIR;
        return NULL;
    }
    while (h->next < CARD_FILES) {
        CardFile* f = c->files[h->next++];
        if (f) {
            memset(&h->dirent, 0, sizeof(h->dirent));
            strncpy(h->dirent.name, f->name, sizeof(h->dirent.name) - 1);
            h->dirent.size = (int)f->size;
            return &h->dirent;
        }
    }
    return NULL;
}

static int CardRewinddir(void* hnd) {
    ((Handle*)hnd)->next = 0;
    return 0;
}

static int CardUnlink(vfs_handler_t* vfs, const char* fn) {
    (void)vfs;
    const char* name;
    int port = ParsePath(fn, &name);
    Card* c = port < 0 ? NULL : GetCard(port);
    int i = c && name ? FindFile(c, name) : -1;
    if (i < 0) {
        errno = ENOENT;
        return -1;
    }
    CardFile* f = c->files[i];
    if (Kept(f->name)) {
        Unpersist(c, f);
    }
    c->files[i] = NULL;
    FreeFile(f);
    return 0;
}

static int CardRename(vfs_handler_t* vfs, const char* fn1, const char* fn2) {
    (void)vfs;
    const char *from, *to;
    int port = ParsePath(fn1, &from);
    Card* c = port < 0 ? NULL : GetCard(port);
    int i = c && from ? FindFile(c, from) : -1;
    if (i < 0 || ParsePath(fn2, &to) != port || !to) {
        errno = i < 0 ? ENOENT : EXDEV;
        return -1;
    }
    CardFile* f = c->files[i];
    char old_name[PS1_NAME + 1];
    memcpy(old_name, f->name, sizeof(old_name));
    int j = FindFile(c, to);
    if (j == i) {
        return 0;
    }
    memset(f->name, 0, sizeof(f->name));
    strncpy(f->name, to, PS1_NAME);
    if (Kept(f->name) && Persist(c, f) < 0) {
        memcpy(f->name, old_name, sizeof(old_name));
        return -1;
    }
    if (j >= 0) { // replaced: its VMU file was just overwritten
        CardFile* old = c->files[j];
        c->files[j] = NULL;
        FreeFile(old);
    }
    if (Kept(old_name)) {
        CardFile tmp;
        memcpy(tmp.name, old_name, sizeof(old_name));
        Unpersist(c, &tmp);
    }
    return 0;
}

static void FillStat(struct stat* st, bool dir, size_t size) {
    memset(st, 0, sizeof(*st));
    st->st_mode = dir ? (S_IFDIR | 0777) : (S_IFREG | 0666);
    st->st_size = dir ? 0 : (off_t)size;
    st->st_nlink = 1;
    st->st_blksize = 512;
}

static int CardStat(vfs_handler_t* vfs, const char* path, struct stat* st,
                    int flag) {
    (void)vfs;
    (void)flag;
    const char* p = path;
    while (*p == '/') {
        p++;
    }
    if (!*p) {
        FillStat(st, true, 0);
        return 0;
    }
    const char* name;
    int port = ParsePath(path, &name);
    Card* c = port < 0 ? NULL : GetCard(port);
    if (!c) {
        errno = ENOENT;
        return -1;
    }
    if (!name) {
        FillStat(st, true, 0);
        return 0;
    }
    int i = FindFile(c, name);
    if (i < 0) {
        errno = ENOENT;
        return -1;
    }
    FillStat(st, false, c->files[i]->size);
    return 0;
}

static int CardFstat(void* hnd, struct stat* st) {
    Handle* h = hnd;
    FillStat(st, h->dir, h->dir ? 0 : h->file->size);
    return 0;
}

static vfs_handler_t card_vfs = {
    .nmmgr = {"/pscard", 0, 0x00010000, 0, NMMGR_TYPE_VFS, NMMGR_LIST_INIT},
    .open = CardOpen,
    .close = CardClose,
    .read = CardRead,
    .write = CardWrite,
    .seek = CardSeek,
    .tell = CardTell,
    .total = CardTotal,
    .readdir = CardReaddir,
    .rename = CardRename,
    .unlink = CardUnlink,
    .stat = CardStat,
    .rewinddir = CardRewinddir,
    .fstat = CardFstat,
};

// "bu00:NAME" to "/pscard/0/NAME", "bu10:*" to "/pscard/1/"
static int CardPath(char* dst, const char* src, int maxlen) {
    if (strlen(src) < 5 || src[0] != 'b' || src[1] != 'u' || src[4] != ':' ||
        (src[2] != '0' && src[2] != '1')) {
        return -1;
    }
    const char* name = src + 5;
    if (*name == '*') {
        name = "";
    }
    snprintf(dst, (size_t)maxlen, "/pscard/%c/%s", src[2], name);
    return 0;
}

void DcCard_Init(void) {
    static bool done;
    if (done) {
        return;
    }
    done = true;
    if (nmmgr_handler_add(&card_vfs.nmmgr) < 0) {
        ERRORF("memory cards: no filesystem");
        return;
    }
    Psyz_AdjustPathCB(CardPath);
}
