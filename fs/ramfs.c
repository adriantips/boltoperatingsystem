#include <stdint.h>
#include <stddef.h>
#include "fs.h"
#include "kheap.h"
#include "string.h"
#include "pit.h"

static fs_node *root_;
static fs_node *cwd_;

/* --------------------------------------------------------------------------
 *  node allocation
 * --------------------------------------------------------------------------*/
static fs_node *node_new(const char *name, int is_dir, fs_node *parent) {
    fs_node *n = (fs_node *)kmalloc(sizeof(fs_node));
    if (!n) return 0;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, FS_NAME_MAX);
    n->is_dir = is_dir;
    n->parent = parent;
    n->mtime  = pit_ticks();
    if (parent) {                               /* append to sibling list */
        if (!parent->child) parent->child = n;
        else {
            fs_node *c = parent->child;
            while (c->next) c = c->next;
            c->next = n;
        }
    }
    return n;
}

void fs_init(void) {
    root_ = (fs_node *)kmalloc(sizeof(fs_node));
    memset(root_, 0, sizeof(*root_));
    strncpy(root_->name, "/", FS_NAME_MAX);
    root_->is_dir = 1;
    cwd_ = root_;

    /* seed a small tree so the file commands have something to show */
    fs_create("/etc", 1);
    fs_create("/home", 1);
    fs_create("/tmp", 1);
    fs_node *f;
    f = fs_create("/etc/motd", 0);
    if (f) fs_write(f, "Welcome to BoltOS.\nType 'help' for commands.\n", 45);
    f = fs_create("/etc/version", 0);
    if (f) fs_write(f, "BoltOS 0.2 (64-bit long mode)\n", 30);
    f = fs_create("/home/readme.txt", 0);
    if (f) fs_write(f,
        "BoltOS in-RAM filesystem.\n"
        "Files live in the kernel heap and reset on reboot.\n"
        "Create one with:  write notes.txt hello world\n", 122);
}

fs_node *fs_root(void) { return root_; }
fs_node *fs_cwd(void)  { return cwd_; }
void     fs_set_cwd(fs_node *d) { if (d && d->is_dir) cwd_ = d; }

int fs_chdir(const char *path) {
    fs_node *n = fs_lookup(path && *path ? path : "/");
    if (!n || !n->is_dir) return -1;
    cwd_ = n;
    return 0;
}

/* --------------------------------------------------------------------------
 *  lookup / path resolution
 * --------------------------------------------------------------------------*/
fs_node *fs_child(fs_node *dir, const char *name) {
    if (!dir || !dir->is_dir) return 0;
    for (fs_node *c = dir->child; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return 0;
}

fs_node *fs_lookup(const char *path) {
    if (!path || !*path) return cwd_;
    char buf[FS_PATH_MAX];
    strncpy(buf, path, sizeof(buf));

    fs_node *cur = (buf[0] == '/') ? root_ : cwd_;
    char *p = buf;
    while (*p == '/') p++;

    while (*p) {
        char *seg = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = 0;
        while (*p == '/') p++;

        if (seg[0] == 0 || strcmp(seg, ".") == 0) continue;
        if (strcmp(seg, "..") == 0) { if (cur->parent) cur = cur->parent; continue; }
        if (!cur->is_dir) return 0;
        cur = fs_child(cur, seg);
        if (!cur) return 0;
    }
    return cur;
}

/* Resolve everything up to the last component; return the parent dir and copy
 * the final name into leaf_out. */
fs_node *fs_parent_of(const char *path, char *leaf_out, uint32_t leaf_cap) {
    char buf[FS_PATH_MAX];
    strncpy(buf, path, sizeof(buf));
    /* strip a single trailing slash */
    uint32_t L = (uint32_t)strlen(buf);
    if (L > 1 && buf[L - 1] == '/') buf[L - 1] = 0;

    char *slash = strrchr(buf, '/');
    if (!slash) {                               /* bare name -> cwd */
        strncpy(leaf_out, buf, leaf_cap);
        return cwd_;
    }
    strncpy(leaf_out, slash + 1, leaf_cap);
    if (slash == buf) return root_;             /* "/leaf" */
    *slash = 0;
    return fs_lookup(buf);
}

fs_node *fs_create(const char *path, int is_dir) {
    char leaf[FS_NAME_MAX];
    fs_node *parent = fs_parent_of(path, leaf, sizeof(leaf));
    if (!parent || !parent->is_dir) return 0;
    if (leaf[0] == 0) return 0;
    if (fs_child(parent, leaf)) return 0;       /* already exists */
    return node_new(leaf, is_dir, parent);
}

/* --------------------------------------------------------------------------
 *  remove
 * --------------------------------------------------------------------------*/
static void node_free_tree(fs_node *n) {
    fs_node *c = n->child;
    while (c) { fs_node *nx = c->next; node_free_tree(c); c = nx; }
    if (n->data) kfree(n->data);
    kfree(n);
}

int fs_remove_node(fs_node *n) {
    if (!n || n == root_) return -1;
    fs_node *parent = n->parent;
    if (parent) {                               /* unlink from sibling list */
        if (parent->child == n) parent->child = n->next;
        else {
            fs_node *c = parent->child;
            while (c && c->next != n) c = c->next;
            if (c) c->next = n->next;
        }
    }
    if (cwd_ == n) cwd_ = parent ? parent : root_;
    node_free_tree(n);
    return 0;
}

int fs_remove(const char *path) {
    return fs_remove_node(fs_lookup(path));
}

/* --------------------------------------------------------------------------
 *  read/write
 * --------------------------------------------------------------------------*/
int fs_write(fs_node *n, const void *data, uint32_t len) {
    if (!n || n->is_dir) return -1;
    if (n->cap < len || !n->data) {
        uint8_t *nd = (uint8_t *)kmalloc(len ? len : 1);
        if (!nd) return -1;
        if (n->data) kfree(n->data);
        n->data = nd;
        n->cap  = len ? len : 1;
    }
    if (len) memcpy(n->data, data, len);
    n->size  = len;
    n->mtime = pit_ticks();
    return 0;
}

int fs_append(fs_node *n, const void *data, uint32_t len) {
    if (!n || n->is_dir) return -1;
    uint32_t need = n->size + len;
    if (n->cap < need) {
        uint8_t *nd = (uint8_t *)kmalloc(need ? need : 1);
        if (!nd) return -1;
        if (n->size && n->data) memcpy(nd, n->data, n->size);
        if (n->data) kfree(n->data);
        n->data = nd;
        n->cap  = need;
    }
    if (len) memcpy(n->data + n->size, data, len);
    n->size  = need;
    n->mtime = pit_ticks();
    return 0;
}

/* --------------------------------------------------------------------------
 *  move / rename
 * --------------------------------------------------------------------------*/
static int is_ancestor(fs_node *anc, fs_node *n) {
    for (fs_node *p = n; p; p = p->parent) if (p == anc) return 1;
    return 0;
}

int fs_move(fs_node *n, fs_node *newparent, const char *newname) {
    if (!n || n == root_ || !newparent || !newparent->is_dir) return -1;
    if (is_ancestor(n, newparent)) return -1;   /* no moving into own subtree */
    if (fs_child(newparent, newname)) return -1;

    fs_node *op = n->parent;                     /* unlink from old parent */
    if (op) {
        if (op->child == n) op->child = n->next;
        else {
            fs_node *c = op->child;
            while (c && c->next != n) c = c->next;
            if (c) c->next = n->next;
        }
    }
    n->next = 0;
    n->parent = newparent;
    strncpy(n->name, newname, FS_NAME_MAX);
    if (!newparent->child) newparent->child = n;
    else {
        fs_node *c = newparent->child;
        while (c->next) c = c->next;
        c->next = n;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 *  helpers
 * --------------------------------------------------------------------------*/
void fs_abspath(fs_node *n, char *out, uint32_t cap) {
    if (!n) { strncpy(out, "?", cap); return; }
    if (n == root_) { strncpy(out, "/", cap); return; }

    fs_node *stack[FS_MAX_DEPTH];
    int depth = 0;
    for (fs_node *p = n; p && p != root_ && depth < FS_MAX_DEPTH; p = p->parent)
        stack[depth++] = p;

    uint32_t pos = 0;
    for (int i = depth - 1; i >= 0; i--) {
        if (pos + 1 < cap) out[pos++] = '/';
        const char *s = stack[i]->name;
        while (*s && pos + 1 < cap) out[pos++] = *s++;
    }
    out[pos < cap ? pos : cap - 1] = 0;
}

static uint64_t bytes_rec(fs_node *n) {
    uint64_t t = n->is_dir ? 0 : n->size;
    for (fs_node *c = n->child; c; c = c->next) t += bytes_rec(c);
    return t;
}
uint64_t fs_total_bytes(void) { return root_ ? bytes_rec(root_) : 0; }

static int count_rec(fs_node *n) {
    int t = 1;
    for (fs_node *c = n->child; c; c = c->next) t += count_rec(c);
    return t;
}
int fs_count_nodes(void) { return root_ ? count_rec(root_) - 1 : 0; }
