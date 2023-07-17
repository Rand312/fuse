/*
    FUSE: Filesystem in Userspace
    Copyright (C) 2001-2004  Miklos Szeredi <miklos@szeredi.hu>

    This program can be distributed under the terms of the GNU LGPL.
    See the file COPYING.LIB
*/

#include <config.h>
#include "fuse_i.h"
#include "fuse_kernel.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <sys/param.h>

#define FUSE_VERSION_FILE_OLD "/proc/fs/fuse/version"
#define FUSE_VERSION_FILE_NEW "/sys/fs/fuse/version"
#define FUSE_DEV_OLD "/proc/fs/fuse/dev"

#define FUSE_MAX_PATH 4096
#define PARAM(inarg) (((char *)(inarg)) + sizeof(*inarg))

#define ENTRY_REVALIDATE_TIME 1 /* sec */
#define ATTR_REVALIDATE_TIME 1 /* sec */

static struct fuse_context *(*fuse_getcontext)(void) = NULL;

static const char *opname(enum fuse_opcode opcode)
{
    switch (opcode) { 
    case FUSE_LOOKUP:		return "LOOKUP";
    case FUSE_FORGET:		return "FORGET";
    case FUSE_GETATTR:		return "GETATTR";
    case FUSE_SETATTR:		return "SETATTR";
    case FUSE_READLINK:		return "READLINK";
    case FUSE_SYMLINK:		return "SYMLINK";
    case FUSE_GETDIR:		return "GETDIR";
    case FUSE_MKNOD:		return "MKNOD";
    case FUSE_MKDIR:		return "MKDIR";
    case FUSE_UNLINK:		return "UNLINK";
    case FUSE_RMDIR:		return "RMDIR";
    case FUSE_RENAME:		return "RENAME";
    case FUSE_LINK:		return "LINK";
    case FUSE_OPEN:		return "OPEN";
    case FUSE_READ:		return "READ";
    case FUSE_WRITE:		return "WRITE";
    case FUSE_STATFS:		return "STATFS";
    case FUSE_FLUSH:		return "FLUSH";
    case FUSE_RELEASE:		return "RELEASE";
    case FUSE_FSYNC:		return "FSYNC";
    case FUSE_SETXATTR:		return "SETXATTR";
    case FUSE_GETXATTR:		return "GETXATTR";
    case FUSE_LISTXATTR:	return "LISTXATTR";
    case FUSE_REMOVEXATTR:	return "REMOVEXATTR";
    default: 			return "???";
    }
}


static inline void dec_avail(struct fuse *f)
{
    pthread_mutex_lock(&f->lock);
    f->numavail --;
    pthread_mutex_unlock(&f->lock);
}

static struct node *__get_node(struct fuse *f, fino_t ino)
{
    size_t hash = ino % f->ino_table_size;
    struct node *node;

    for (node = f->ino_table[hash]; node != NULL; node = node->ino_next)
        if (node->ino == ino)
            return node;
    
    return NULL;
}

static struct node *get_node(struct fuse *f, fino_t ino)
{
    struct node *node = __get_node(f, ino);
    if (node != NULL)
        return node;
    
    fprintf(stderr, "fuse internal error: inode %lu not found\n", ino);
    abort();
}

static void hash_ino(struct fuse *f, struct node *node)
{
    size_t hash = node->ino % f->ino_table_size;
    node->ino_next = f->ino_table[hash];
    f->ino_table[hash] = node;    
}

static void unhash_ino(struct fuse *f, struct node *node)
{
    size_t hash = node->ino % f->ino_table_size;
    struct node **nodep = &f->ino_table[hash];

    for (; *nodep != NULL; nodep = &(*nodep)->ino_next) 
        if (*nodep == node) {
            *nodep = node->ino_next;
            return;
        }
}

static fino_t next_ino(struct fuse *f)
{
    do {
        f->ctr++;
        if (!f->ctr)
            f->generation ++;
    } while (f->ctr == 0 || __get_node(f, f->ctr) != NULL);
    return f->ctr;
}

static void free_node(struct node *node)
{
    free(node->name);
    free(node);
}

static unsigned int name_hash(struct fuse *f, fino_t parent, const char *name)
{
    unsigned int hash = *name;

    if (hash)
        for (name += 1; *name != '\0'; name++)
            hash = (hash << 5) - hash + *name;

    return (hash + parent) % f->name_table_size;
}

static struct node *__lookup_node(struct fuse *f, fino_t parent,
                                const char *name)
{
    size_t hash = name_hash(f, parent, name);
    struct node *node;

    for (node = f->name_table[hash]; node != NULL; node = node->name_next)
        if (node->parent == parent && strcmp(node->name, name) == 0)
            return node;

    return NULL;
}

static struct node *lookup_node(struct fuse *f, fino_t parent,
                                const char *name)
{
    struct node *node;

    pthread_mutex_lock(&f->lock);
    node = __lookup_node(f, parent, name);
    pthread_mutex_unlock(&f->lock);
    if (node != NULL)
        return node;
    
    fprintf(stderr, "fuse internal error: node %lu/%s not found\n", parent,
            name);
    abort();
}

static int hash_name(struct fuse *f, struct node *node, fino_t parent,
                     const char *name)
{
    size_t hash = name_hash(f, parent, name);
    node->parent = parent;
    node->name = strdup(name);
    if (node->name == NULL)
        return -1;

    node->name_next = f->name_table[hash];
    f->name_table[hash] = node;
    return 0;
}

static void unhash_name(struct fuse *f, struct node *node)
{
    if (node->name != NULL) {
        size_t hash = name_hash(f, node->parent, node->name);
        struct node **nodep = &f->name_table[hash];
        
        for (; *nodep != NULL; nodep = &(*nodep)->name_next)
            if (*nodep == node) {
                *nodep = node->name_next;
                node->name_next = NULL;
                free(node->name);
                node->name = NULL;
                node->parent = 0;
                return;
            }
        fprintf(stderr, "fuse internal error: unable to unhash node: %lu\n",
                node->ino);
        abort();
    }
}

static struct node *find_node(struct fuse *f, fino_t parent, char *name,
                              struct fuse_attr *attr, int version)
{
    struct node *node;
    int mode = attr->mode & S_IFMT;
    int rdev = 0;
    
    if (S_ISCHR(mode) || S_ISBLK(mode))
        rdev = attr->rdev;

    pthread_mutex_lock(&f->lock);
    node = __lookup_node(f, parent, name);
    if (node != NULL) {
        if (node->mode == mode && node->rdev == rdev)
            goto out;
        
        unhash_name(f, node);
    }

    node = (struct node *) calloc(1, sizeof(struct node));
    if (node == NULL)
        goto out_err;

    node->mode = mode;
    node->rdev = rdev;
    node->open_count = 0;
    node->is_hidden = 0;
    node->ino = next_ino(f);
    node->generation = f->generation;
    if (hash_name(f, node, parent, name) == -1) {
        free(node);
        node = NULL;
        goto out_err;
    }
    hash_ino(f, node);

 out:
    attr->_user_ino = node->ino;
    node->version = version;
 out_err:
    pthread_mutex_unlock(&f->lock);
    return node;
}

static int path_lookup(struct fuse *f, const char *path, fino_t *inop)
{
    fino_t ino;
    int err;
    char *s;
    char *name;
    char *tmp = strdup(path);
    if (!tmp)
        return -ENOMEM;

    pthread_mutex_lock(&f->lock);
    ino = FUSE_ROOT_INO;
    err = 0;
    for  (s = tmp; (name = strsep(&s, "/")) != NULL; ) {
        if (name[0]) {
            struct node *node = __lookup_node(f, ino, name);
            if (node == NULL) {
                err = -ENOENT;
                break;
            }
            ino = node->ino;
        }
    }
    pthread_mutex_unlock(&f->lock);
    free(tmp);
    if (!err)
        *inop = ino;
    
    return err;
}

static char *add_name(char *buf, char *s, const char *name)
{
    size_t len = strlen(name);
    s -= len;
    if (s <= buf) {
        fprintf(stderr, "fuse: path too long: ...%s\n", s + len);
        return NULL;
    }
    strncpy(s, name, len);
    s--;
    *s = '/';

    return s;
}

static char *get_path_name(struct fuse *f, fino_t ino, const char *name)
{
    char buf[FUSE_MAX_PATH];
    char *s = buf + FUSE_MAX_PATH - 1;
    struct node *node;
    
    *s = '\0';

    if (name != NULL) {
        s = add_name(buf, s, name);
        if (s == NULL)
            return NULL;
    }

    pthread_mutex_lock(&f->lock);
    for (node = get_node(f, ino); node->ino != FUSE_ROOT_INO;
        node = get_node(f, node->parent)) {
        if (node->name == NULL) {
            s = NULL;
            break;
        }
        
        s = add_name(buf, s, node->name);
        if (s == NULL)
            break;
    }
    pthread_mutex_unlock(&f->lock);

    if (s == NULL) 
        return NULL;
    else if (*s == '\0')
        return strdup("/");
    else
        return strdup(s);
}

static char *get_path(struct fuse *f, fino_t ino)
{
    return get_path_name(f, ino, NULL);
}

static void destroy_node(struct fuse *f, fino_t ino, int version)
{
    struct node *node;

    pthread_mutex_lock(&f->lock);
    node = __get_node(f, ino);
    if (node && node->version == version && ino != FUSE_ROOT_INO) {
        unhash_name(f, node);
        unhash_ino(f, node);
        free_node(node);
    }
    pthread_mutex_unlock(&f->lock);

}

static void remove_node(struct fuse *f, fino_t dir, const char *name)
{
    struct node *node;

    pthread_mutex_lock(&f->lock);
    node = __lookup_node(f, dir, name);
    if (node == NULL) {
        fprintf(stderr, "fuse internal error: unable to remove node %lu/%s\n",
                dir, name);
        abort();
    }
    unhash_name(f, node);
    pthread_mutex_unlock(&f->lock);
}

static int rename_node(struct fuse *f, fino_t olddir, const char *oldname,
                        fino_t newdir, const char *newname, int hide)
{
    struct node *node;
    struct node *newnode;
    int err = 0;
    
    pthread_mutex_lock(&f->lock);
    node  = __lookup_node(f, olddir, oldname);
    newnode  = __lookup_node(f, newdir, newname);
    if (node == NULL) {
        fprintf(stderr, "fuse internal error: unable to rename node %lu/%s\n",
                olddir, oldname);
        abort();
    }

    if (newnode != NULL) {
        if (hide) {
            fprintf(stderr, "fuse: hidden file got created during hiding\n");
            err = -EBUSY;
            goto out;
        }
        unhash_name(f, newnode);
    }
        
    unhash_name(f, node);
    if (hash_name(f, node, newdir, newname) == -1) {
        err = -ENOMEM;
        goto out;
    }
        
    if (hide)
        node->is_hidden = 1;

 out:
    pthread_mutex_unlock(&f->lock);
    return err;
}

static void convert_stat(struct stat *stbuf, struct fuse_attr *attr)
{
    attr->mode      = stbuf->st_mode;
    attr->nlink     = stbuf->st_nlink;
    attr->uid       = stbuf->st_uid;
    attr->gid       = stbuf->st_gid;
    attr->rdev      = stbuf->st_rdev;
    attr->size      = stbuf->st_size;
    attr->blocks    = stbuf->st_blocks;
    attr->atime     = stbuf->st_atime;
    attr->mtime     = stbuf->st_mtime;
    attr->ctime     = stbuf->st_ctime;
#ifdef HAVE_STRUCT_STAT_ST_ATIM
    attr->atimensec = stbuf->st_atim.tv_nsec;
    attr->mtimensec = stbuf->st_mtim.tv_nsec;
    attr->ctimensec = stbuf->st_ctim.tv_nsec;
#endif
}

static int fill_dir(struct fuse_dirhandle *dh, char *name, int type)
{
    struct fuse_dirent dirent;
    size_t reclen;
    size_t res;

    dirent.ino = (unsigned long) -1;
    dirent.namelen = strlen(name);
    strncpy(dirent.name, name, sizeof(dirent.name));
    dirent.type = type;
    reclen = FUSE_DIRENT_SIZE(&dirent);
    res = fwrite(&dirent, reclen, 1, dh->fp);
    if (res == 0) {
        perror("fuse: writing directory file");
        return -EIO;
    }
    return 0;
}

//write(f->fd, outbuf, outsize)
//核心就是想 fuse->fd 写
static int send_reply_raw(struct fuse *f, char *outbuf, size_t outsize,
                          int locked)
{
    int res;

    if ((f->flags & FUSE_DEBUG)) {
        struct fuse_out_header *out = (struct fuse_out_header *) outbuf;
        printf("   unique: %i, error: %i (%s), outsize: %i\n", out->unique,
               out->error, strerror(-out->error), outsize);
        fflush(stdout);
    }

    /* This needs to be done before the reply, otherwise the scheduler
    could play tricks with us, and only let the counter be increased
    long after the operation is done */
    if (!locked)
        pthread_mutex_lock(&f->lock);
    f->numavail ++;
    if (!locked)
        pthread_mutex_unlock(&f->lock);

    res = write(f->fd, outbuf, outsize);
    if (res == -1) {
        /* ENOENT means the operation was interrupted */
        if (!f->exited && errno != ENOENT)
            perror("fuse: writing device");
        return -errno;
    }
    return 0;
}

static int __send_reply(struct fuse *f, struct fuse_in_header *in, int error,
                        void *arg, size_t argsize, int locked)
{
    int res;
    char *outbuf;
    size_t outsize;
    struct fuse_out_header *out;

    if (error <= -1000 || error > 0) {
        fprintf(stderr, "fuse: bad error value: %i\n",  error);
        error = -ERANGE;
    }

    if (error)
        argsize = 0;

    outsize = sizeof(struct fuse_out_header) + argsize;
    outbuf = (char *) malloc(outsize);
    if (outbuf == NULL) {
        fprintf(stderr, "fuse: failed to allocate reply buffer\n");
        res = -ENOMEM;
    } else {
        out = (struct fuse_out_header *) outbuf;
        memset(out, 0, sizeof(struct fuse_out_header));
        out->unique = in->unique;
        out->error = error;
        if (argsize != 0)
            memcpy(outbuf + sizeof(struct fuse_out_header), arg, argsize);
        
        res = send_reply_raw(f, outbuf, outsize, locked);
        free(outbuf);
    }

    return res;
}

static int send_reply(struct fuse *f, struct fuse_in_header *in, int error,
                        void *arg, size_t argsize)
{
    return __send_reply(f, in, error, arg, argsize, 0);
}

static int is_open(struct fuse *f, fino_t dir, const char *name)
{
    struct node *node;
    int isopen = 0;
    pthread_mutex_lock(&f->lock);
    node = __lookup_node(f, dir, name);
    if (node && node->open_count > 0)
        isopen = 1;
    pthread_mutex_unlock(&f->lock);
    return isopen;
}

static char *hidden_name(struct fuse *f, fino_t dir, const char *oldname,
                        char *newname, size_t bufsize)
{
    struct stat buf;
    struct node *node;
    struct node *newnode;
    char *newpath;
    int res;
    int failctr = 10;

    if (!f->op.getattr)
        return NULL;

    do {
        node = lookup_node(f, dir, oldname);
        pthread_mutex_lock(&f->lock);
        do {
            f->hidectr ++;
            snprintf(newname, bufsize, ".fuse_hidden%08x%08x",
                     (unsigned int) node->ino, f->hidectr);
            newnode = __lookup_node(f, dir, newname);
        } while(newnode);
        pthread_mutex_unlock(&f->lock);
        
        newpath = get_path_name(f, dir, newname);
        if (!newpath)
            break;
        
        res = f->op.getattr(newpath, &buf);
        if (res != 0)
            break;
        free(newpath);
        newpath = NULL;
    } while(--failctr);

    return newpath;
}

static int hide_node(struct fuse *f, const char *oldpath, fino_t dir,
                     const char *oldname)
{
    char newname[64];
    char *newpath;
    int err = -EBUSY;

    if (f->op.rename && f->op.unlink) {
        newpath = hidden_name(f, dir, oldname, newname, sizeof(newname));
        if (newpath) {
            int res = f->op.rename(oldpath, newpath);
            if (res == 0)
                err = rename_node(f, dir, oldname, dir, newname, 1);
            free(newpath);
        }
    }
    return err;
}

static int lookup_path(struct fuse *f, fino_t ino, int version,  char *name,
                       const char *path, struct fuse_entry_out *arg)
{
    int res;
    struct stat buf;

    res = f->op.getattr(path, &buf);
    if (res == 0) {
        struct node *node;

        memset(arg, 0, sizeof(struct fuse_entry_out));
        convert_stat(&buf, &arg->attr);
        node = find_node(f, ino, name, &arg->attr, version);
        if (node == NULL)
            res = -ENOMEM;
        else {
            arg->ino = node->ino;
            arg->generation = node->generation;
            arg->entry_valid = ENTRY_REVALIDATE_TIME;
            arg->entry_valid_nsec = 0;
            arg->attr_valid = ATTR_REVALIDATE_TIME;
            arg->attr_valid_nsec = 0;
            if (f->flags & FUSE_DEBUG) {
                printf("   INO: %li\n", arg->ino);
                fflush(stdout);
            }
        }
    }
    return res;
}

static void do_lookup(struct fuse *f, struct fuse_in_header *in, char *name)
{
    int res;
    int res2;
    char *path;
    struct fuse_entry_out arg;

    res = -ENOENT;
    path = get_path_name(f, in->ino, name);
    if (path != NULL) {
        if (f->flags & FUSE_DEBUG) {
            printf("LOOKUP %s\n", path);
            fflush(stdout);
        }
        res = -ENOSYS;
        if (f->op.getattr)
            res = lookup_path(f, in->ino, in->unique, name, path, &arg);
        free(path);
    }
    res2 = send_reply(f, in, res, &arg, sizeof(arg));
    if (res == 0 && res2 == -ENOENT)
        destroy_node(f, arg.ino, in->unique);
}

static void do_forget(struct fuse *f, struct fuse_in_header *in,
                      struct fuse_forget_in *arg)
{
    if (f->flags & FUSE_DEBUG) {
        printf("FORGET %li/%i\n", in->ino, arg->version);
        fflush(stdout);
    }
    destroy_node(f, in->ino, arg->version);
}

static void do_getattr(struct fuse *f, struct fuse_in_header *in)
{
    int res;
    char *path;
    struct stat buf;
    struct fuse_attr_out arg;

    res = -ENOENT;
    path = get_path(f, in->ino);
    if (path != NULL) {
        res = -ENOSYS;
        if (f->op.getattr)
            res = f->op.getattr(path, &buf);
        free(path);
    }

    if (res == 0) {
        memset(&arg, 0, sizeof(struct fuse_attr_out));
        arg.attr_valid = ATTR_REVALIDATE_TIME;
        arg.attr_valid_nsec = 0;
        arg.attr._user_ino = in->ino;
        convert_stat(&buf, &arg.attr);
    }

    send_reply(f, in, res, &arg, sizeof(arg));
}

static int do_chmod(struct fuse *f, const char *path, struct fuse_attr *attr)
{
    int res;

    res = -ENOSYS;
    if (f->op.chmod)
        res = f->op.chmod(path, attr->mode);

    return res;
}        

static int do_chown(struct fuse *f, const char *path, struct fuse_attr *attr,
                    int valid)
{
    int res;
    uid_t uid = (valid & FATTR_UID) ? attr->uid : (uid_t) -1;
    gid_t gid = (valid & FATTR_GID) ? attr->gid : (gid_t) -1;
    
    res = -ENOSYS;
    if (f->op.chown)
        res = f->op.chown(path, uid, gid);

    return res;
}

static int do_truncate(struct fuse *f, const char *path,
                       struct fuse_attr *attr)
{
    int res;

    res = -ENOSYS;
    if (f->op.truncate)
        res = f->op.truncate(path, attr->size);

    return res;
}

static int do_utime(struct fuse *f, const char *path, struct fuse_attr *attr)
{
    int res;
    struct utimbuf buf;
    buf.actime = attr->atime;
    buf.modtime = attr->mtime;
    res = -ENOSYS;
    if (f->op.utime)
        res = f->op.utime(path, &buf);

    return res;
}

static void do_setattr(struct fuse *f, struct fuse_in_header *in,
                       struct fuse_setattr_in *arg)
{
    int res;
    char *path;
    int valid = arg->valid;
    struct fuse_attr *attr = &arg->attr;
    struct fuse_attr_out outarg;

    res = -ENOENT;
    path = get_path(f, in->ino);
    if (path != NULL) {
        res = -ENOSYS;
        if (f->op.getattr) {
            res = 0;
            if (!res && (valid & FATTR_MODE))
                res = do_chmod(f, path, attr);
            if (!res && (valid & (FATTR_UID | FATTR_GID)))
                res = do_chown(f, path, attr, valid);
            if (!res && (valid & FATTR_SIZE))
                res = do_truncate(f, path, attr);
            if (!res && (valid & (FATTR_ATIME | FATTR_MTIME)) == 
               (FATTR_ATIME | FATTR_MTIME))
                res = do_utime(f, path, attr);
            if (!res) {
                struct stat buf;
                res = f->op.getattr(path, &buf);
                if (!res) {
                    memset(&outarg, 0, sizeof(struct fuse_attr_out));
                    outarg.attr_valid = ATTR_REVALIDATE_TIME;
                    outarg.attr_valid_nsec = 0;
                    outarg.attr._user_ino = in->ino;
                    convert_stat(&buf, &outarg.attr);
                }
            }
        }
        free(path);
    }
    send_reply(f, in, res, &outarg, sizeof(outarg));
}

static void do_readlink(struct fuse *f, struct fuse_in_header *in)
{
    int res;
    char link[PATH_MAX + 1];
    char *path;

    res = -ENOENT;
    path = get_path(f, in->ino);
    if (path != NULL) {
        res = -ENOSYS;
        if (f->op.readlink)
            res = f->op.readlink(path, link, sizeof(link));
        free(path);
    }
    link[PATH_MAX] = '\0';
    send_reply(f, in, res, link, res == 0 ? strlen(link) : 0);
}

static void do_getdir(struct fuse *f, struct fuse_in_header *in)
{
    int res;
    struct fuse_getdir_out arg;
    struct fuse_dirhandle dh;
    char *path;

    dh.fuse = f;
    dh.fp = tmpfile();
    dh.dir = in->ino;

    res = -EIO;
    if (dh.fp == NULL)
        perror("fuse: failed to create temporary file");
    else {
        res = -ENOENT;
        path = get_path(f, in->ino);
        if (path != NULL) {
            res = -ENOSYS;
            if (f->op.getdir)
                res = f->op.getdir(path, &dh, (fuse_dirfil_t) fill_dir);
            free(path);
        }
        fflush(dh.fp);
    }
    memset(&arg, 0, sizeof(struct fuse_getdir_out));
    if (res == 0)
        arg.fd = fileno(dh.fp);
    send_reply(f, in, res, &arg, sizeof(arg));
    if (dh.fp != NULL)
        fclose(dh.fp);
}

static void do_mknod(struct fuse *f, struct fuse_in_header *in,
                     struct fuse_mknod_in *inarg)
{
    int res;
    int res2;
    char *path;
    char *name = PARAM(inarg);
    struct fuse_entry_out outarg;

    res = -ENOENT;
    path = get_path_name(f, in->ino, name);
    if (path != NULL) {
        if (f->flags & FUSE_DEBUG) {
            printf("MKNOD %s\n", path);
            fflush(stdout);
        }
        res = -ENOSYS;
        if (f->op.mknod && f->op.getattr) {
            res = f->op.mknod(path, inarg->mode, inarg->rdev);
            if (res == 0)
                res = lookup_path(f, in->ino, in->unique, name, path, &outarg);
        }
        free(path);
    }
    res2 = send_reply(f, in, res, &outarg, sizeof(outarg));
    if (res == 0 && res2 == -ENOENT)
        destroy_node(f, outarg.ino, in->unique);
}

static void do_mkdir(struct fuse *f, struct fuse_in_header *in,
                     struct fuse_mkdir_in *inarg)
{
    int res;
    int res2;
    char *path;
    char *name = PARAM(inarg);
    struct fuse_entry_out outarg;

    res = -ENOENT;
    path = get_path_name(f, in->ino, name);
    if (path != NULL) {
        if (f->flags & FUSE_DEBUG) {
            printf("MKDIR %s\n", path);
            fflush(stdout);
        }
        res = -ENOSYS;
        if (f->op.mkdir && f->op.getattr) {
            res = f->op.mkdir(path, inarg->mode);
            if (res == 0)
                res = lookup_path(f, in->ino, in->unique, name, path, &outarg);
        }
        free(path);
    }
    res2 = send_reply(f, in, res, &outarg, sizeof(outarg));
    if (res == 0 && res2 == -ENOENT)
        destroy_node(f, outarg.ino, in->unique);
}

static void do_unlink(struct fuse *f, struct fuse_in_header *in, char *name)
{
    int res;
    char *path;

    res = -ENOENT;
    path = get_path_name(f, in->ino, name);
    if (path != NULL) {
        if (f->flags & FUSE_DEBUG) {
            printf("UNLINK %s\n", path);
            fflush(stdout);
        }
        res = -ENOSYS;
        if (f->op.unlink) {
            if (!(f->flags & FUSE_HARD_REMOVE) && is_open(f, in->ino, name))
                res = hide_node(f, path, in->ino, name);
            else {
                res = f->op.unlink(path);
                if (res == 0)
                    remove_node(f, in->ino, name);
            }
        }
        free(path);
    }
    send_reply(f, in, res, NULL, 0);
}

static void do_rmdir(struct fuse *f, struct fuse_in_header *in, char *name)
{
    int res;
    char *path;

    res = -ENOENT;
    path = get_path_name(f, in->ino, name);
    if (path != NULL) {
        if (f->flags & FUSE_DEBUG) {
            printf("RMDIR %s\n", path);
            fflush(stdout);
        }
        res = -ENOSYS;
        if (f->op.rmdir) {
            res = f->op.rmdir(path);
            if (res == 0)
                remove_node(f, in->ino, name);
        }
        free(path);
    }
    send_reply(f, in, res, NULL, 0);
}

static void do_symlink(struct fuse *f, struct fuse_in_header *in, char *name,
                       char *link)
{
    int res;
    int res2;
    char *path;
    struct fuse_entry_out outarg;

    res = -ENOENT;
    path = get_path_name(f, in->ino, name);
    if (path != NULL) {
        if (f->flags & FUSE_DEBUG) {
            printf("SYMLINK %s\n", path);
            fflush(stdout);
        }
        res = -ENOSYS;
        if (f->op.symlink && f->op.getattr) {
            res = f->op.symlink(link, path);
            if (res == 0)
                res = lookup_path(f, in->ino, in->unique, name, path, &outarg);
        }
        free(path);
    }
    res2 = send_reply(f, in, res, &outarg, sizeof(outarg));
    if (res == 0 && res2 == -ENOENT)
        destroy_node(f, outarg.ino, in->unique);

}

static void do_rename(struct fuse *f, struct fuse_in_header *in,
                      struct fuse_rename_in *inarg)
{
    int res;
    fino_t olddir = in->ino;
    fino_t newdir = inarg->newdir;
    char *oldname = PARAM(inarg);
    char *newname = oldname + strlen(oldname) + 1;
    char *oldpath;
    char *newpath;

    res = -ENOENT;
    oldpath = get_path_name(f, olddir, oldname);
    if (oldpath != NULL) {
        newpath = get_path_name(f, newdir, newname);
        if (newpath != NULL) {
            if (f->flags & FUSE_DEBUG) {
                printf("RENAME %s -> %s\n", oldpath, newpath);
                fflush(stdout);
            }
            res = -ENOSYS;
            if (f->op.rename) {
                res = 0;
                if (!(f->flags & FUSE_HARD_REMOVE) && 
                    is_open(f, newdir, newname))
                    res = hide_node(f, newpath, newdir, newname);
                if (res == 0) {
                    res = f->op.rename(oldpath, newpath);
                    if (res == 0)
                        res = rename_node(f, olddir, oldname, newdir, newname, 0);
                }
            }
            free(newpath);
        }
        free(oldpath);
    }
    send_reply(f, in, res, NULL, 0);   
}

static void do_link(struct fuse *f, struct fuse_in_header *in,
                    struct fuse_link_in *arg)
{
    int res;
    int res2;
    char *oldpath;
    char *newpath;
    char *name = PARAM(arg);
    struct fuse_entry_out outarg;

    res = -ENOENT;
    oldpath = get_path(f, in->ino);
    if (oldpath != NULL) {
        newpath =  get_path_name(f, arg->newdir, name);
        if (newpath != NULL) {
            if (f->flags & FUSE_DEBUG) {
                printf("LINK %s\n", newpath);
                fflush(stdout);
            }
            res = -ENOSYS;
            if (f->op.link && f->op.getattr) {
                res = f->op.link(oldpath, newpath);
                if (res == 0)
                    res = lookup_path(f, arg->newdir, in->unique, name,
                                      newpath, &outarg);
            }
            free(newpath);
        }
        free(oldpath);
    }
    res2 = send_reply(f, in, res, &outarg, sizeof(outarg));
    if (res == 0 && res2 == -ENOENT)
        destroy_node(f, outarg.ino, in->unique);
}

static void do_open(struct fuse *f, struct fuse_in_header *in,
                    struct fuse_open_in *arg)
{
    int res;
    char *path;
    struct fuse_open_out outarg;

    res = -ENOENT;
    path = get_path(f, in->ino);
    if (path != NULL) {
        res = -ENOSYS;
        if (f->op.open)
            res = f->op.open(path, arg->flags);
    }
    if (res == 0) {
        int res2;

        /* If the request is interrupted the lock must be held until
           the cancellation is finished.  Otherwise there could be
           races with rename/unlink, against which the kernel can't
           protect */
        pthread_mutex_lock(&f->lock);
        f->fh_ctr ++;
        outarg.fh = f->fh_ctr;
        if (f->flags & FUSE_DEBUG) {
            printf("OPEN[%lu] flags: 0x%x\n", outarg.fh, arg->flags);
            fflush(stdout);
        }

        res2 = __send_reply(f, in, res, &outarg, sizeof(outarg), 1);
        if(res2 == -ENOENT) {
            /* The open syscall was interrupted, so it must be cancelled */
            if(f->op.release)
                f->op.release(path, arg->flags);
        } else
            get_node(f, in->ino)->open_count ++;
        pthread_mutex_unlock(&f->lock);

    } else
        send_reply(f, in, res, NULL, 0);

    if (path)
        free(path);
}

static void do_flush(struct fuse *f, struct fuse_in_header *in,
                     struct fuse_flush_in *arg)
{
    char *path;
    int res;

    res = -ENOENT;
    path = get_path(f, in->ino);
    if (path != NULL) {
        if (f->flags & FUSE_DEBUG) {
            printf("FLUSH[%lu]\n", arg->fh);
            fflush(stdout);
        }
        res = -ENOSYS;
        if (f->op.flush)
            res = f->op.flush(path);
        free(path);
    }
    send_reply(f, in, res, NULL, 0);
}

static void do_release(struct fuse *f, struct fuse_in_header *in,
                       struct fuse_release_in *arg)
{
    struct node *node;
    char *path;

    pthread_mutex_lock(&f->lock);
    node = get_node(f, in->ino);
    --node->open_count;
    pthread_mutex_unlock(&f->lock);

    path = get_path(f, in->ino);
    if (path != NULL) {
        if (f->flags & FUSE_DEBUG) {
            printf("RELEASE[%lu]\n", arg->fh);
            fflush(stdout);
        }
        if (f->op.release)
            f->op.release(path, arg->flags);

        if(node->is_hidden && node->open_count == 0)
            /* can now clean up this hidden file */
            f->op.unlink(path);
        
        free(path);
    }
    send_reply(f, in, 0, NULL, 0);
}

static void do_read(struct fuse *f, struct fuse_in_header *in,
                    struct fuse_read_in *arg)
{
    int res;
    char *path;
    char *outbuf = (char *) malloc(sizeof(struct fuse_out_header) + arg->size);
    if (outbuf == NULL)
        send_reply(f, in, -ENOMEM, NULL, 0);
    else {
        struct fuse_out_header *out = (struct fuse_out_header *) outbuf;
        char *buf = outbuf + sizeof(struct fuse_out_header);
        size_t size;
        size_t outsize;
        
        res = -ENOENT;
        path = get_path(f, in->ino);
        if (path != NULL) {
            if (f->flags & FUSE_DEBUG) {
                printf("READ[%lu] %u bytes from %llu\n", arg->fh, arg->size,
                       arg->offset);
                fflush(stdout);
            }
            
            res = -ENOSYS;
            if (f->op.read)
                res = f->op.read(path, buf, arg->size, arg->offset);
            free(path);
        }
        
        size = 0;
        if (res >= 0) {
            size = res;
            res = 0;
            if (f->flags & FUSE_DEBUG) {
                printf("   READ[%lu] %u bytes\n", arg->fh, size);
                fflush(stdout);
            }
        }
        memset(out, 0, sizeof(struct fuse_out_header));
        out->unique = in->unique;
        out->error = res;
        outsize = sizeof(struct fuse_out_header) + size;
        
        send_reply_raw(f, outbuf, outsize, 0);
        free(outbuf);
    }
}

static void do_write(struct fuse *f, struct fuse_in_header *in,
                     struct fuse_write_in *arg)
{
    int res;
    char *path;
    struct fuse_write_out outarg;

    res = -ENOENT;
    path = get_path(f, in->ino);
    if (path != NULL) {
        if (f->flags & FUSE_DEBUG) {
            printf("WRITE%s[%lu] %u bytes to %llu\n",
                   arg->writepage ? "PAGE" : "", arg->fh, arg->size,
                   arg->offset);
            fflush(stdout);
        }

        res = -ENOSYS;
        if (f->op.write)
            res = f->op.write(path, PARAM(arg), arg->size, arg->offset);
        free(path);
    }
    
    if (res >= 0) { 
        outarg.size = res;
        res = 0;
    }

    send_reply(f, in, res, &outarg, sizeof(outarg));
}

static int default_statfs(struct statfs *buf)
{
    buf->f_namelen = 255;
    buf->f_bsize = 512;
    return 0;
}

static void convert_statfs(struct statfs *statfs, struct fuse_kstatfs *kstatfs)
{
    kstatfs->bsize	= statfs->f_bsize;
    kstatfs->blocks	= statfs->f_blocks;
    kstatfs->bfree	= statfs->f_bfree;
    kstatfs->bavail	= statfs->f_bavail;
    kstatfs->files	= statfs->f_files;
    kstatfs->ffree	= statfs->f_ffree;
    kstatfs->namelen	= statfs->f_namelen;
}

static void do_statfs(struct fuse *f, struct fuse_in_header *in)
{
    int res;
    struct fuse_statfs_out arg;
    struct statfs buf;

    memset(&buf, 0, sizeof(struct statfs));
    if (f->op.statfs)
        res = f->op.statfs("/", &buf);
    else
        res = default_statfs(&buf);

    if (res == 0)
        convert_statfs(&buf, &arg.st);

    send_reply(f, in, res, &arg, sizeof(arg));
}

static void do_fsync(struct fuse *f, struct fuse_in_header *in,
                     struct fuse_fsync_in *inarg)
{
    int res;
    char *path;

    res = -ENOENT;
    path = get_path(f, in->ino);
    if (path != NULL) {
        if (f->flags & FUSE_DEBUG) {
            printf("FSYNC[%lu]\n", inarg->fh);
            fflush(stdout);
        }
        res = -ENOSYS;
        if (f->op.fsync)
            res = f->op.fsync(path, inarg->datasync);
        free(path);
    }
    send_reply(f, in, res, NULL, 0);
}

static void do_setxattr(struct fuse *f, struct fuse_in_header *in,
                        struct fuse_setxattr_in *arg)
{
    int res;
    char *path;
    char *name = PARAM(arg);
    unsigned char *value = name + strlen(name) + 1;

    res = -ENOENT;
    path = get_path(f, in->ino);
    if (path != NULL) {
        res = -ENOSYS;
        if (f->op.setxattr)
            res = f->op.setxattr(path, name, value, arg->size, arg->flags);
        free(path);
    }    
    send_reply(f, in, res, NULL, 0);
}

static int common_getxattr(struct fuse *f, struct fuse_in_header *in,
                           const char *name, char *value, size_t size)
{
    int res;
    char *path;

    res = -ENOENT;
    path = get_path(f, in->ino);
    if (path != NULL) {
        res = -ENOSYS;
        if (f->op.getxattr)
            res = f->op.getxattr(path, name, value, size);
        free(path);
    }    
    return res;
}

static void do_getxattr_read(struct fuse *f, struct fuse_in_header *in,
                             const char *name, size_t size)
{
    int res;
    char *outbuf = (char *) malloc(sizeof(struct fuse_out_header) + size);
    if (outbuf == NULL)
        send_reply(f, in, -ENOMEM, NULL, 0);
    else {
        struct fuse_out_header *out = (struct fuse_out_header *) outbuf;
        char *value = outbuf + sizeof(struct fuse_out_header);
        
        res = common_getxattr(f, in, name, value, size);
        size = 0;
        if (res > 0) {
            size = res;
            res = 0;
        }
        memset(out, 0, sizeof(struct fuse_out_header));
        out->unique = in->unique;
        out->error = res;
        
        send_reply_raw(f, outbuf, sizeof(struct fuse_out_header) + size, 0);
        free(outbuf);
    }
}

static void do_getxattr_size(struct fuse *f, struct fuse_in_header *in,
                             const char *name)
{
    int res;
    struct fuse_getxattr_out arg;

    res = common_getxattr(f, in, name, NULL, 0);
    if (res >= 0) {
        arg.size = res;
        res = 0;
    }
    send_reply(f, in, res, &arg, sizeof(arg));
}

static void do_getxattr(struct fuse *f, struct fuse_in_header *in,
                        struct fuse_getxattr_in *arg)
{
    char *name = PARAM(arg);
    
    if (arg->size)
        do_getxattr_read(f, in, name, arg->size);
    else
        do_getxattr_size(f, in, name);
}

static int common_listxattr(struct fuse *f, struct fuse_in_header *in,
                            char *list, size_t size)
{
    int res;
    char *path;

    res = -ENOENT;
    path = get_path(f, in->ino);
    if (path != NULL) {
        res = -ENOSYS;
        if (f->op.listxattr)
            res = f->op.listxattr(path, list, size);
        free(path);
    }    
    return res;
}

static void do_listxattr_read(struct fuse *f, struct fuse_in_header *in,
                              size_t size)
{
    int res;
    char *outbuf = (char *) malloc(sizeof(struct fuse_out_header) + size);
    if (outbuf == NULL)
        send_reply(f, in, -ENOMEM, NULL, 0);
    else {
        struct fuse_out_header *out = (struct fuse_out_header *) outbuf;
        char *list = outbuf + sizeof(struct fuse_out_header);
        
        res = common_listxattr(f, in, list, size);
        size = 0;
        if (res > 0) {
            size = res;
            res = 0;
        }
        memset(out, 0, sizeof(struct fuse_out_header));
        out->unique = in->unique;
        out->error = res;
        
        send_reply_raw(f, outbuf, sizeof(struct fuse_out_header) + size, 0);
        free(outbuf);
    }
}

static void do_listxattr_size(struct fuse *f, struct fuse_in_header *in)
{
    int res;
    struct fuse_getxattr_out arg;

    res = common_listxattr(f, in, NULL, 0);
    if (res >= 0) {
        arg.size = res;
        res = 0;
    }
    send_reply(f, in, res, &arg, sizeof(arg));
}

static void do_listxattr(struct fuse *f, struct fuse_in_header *in,
                         struct fuse_getxattr_in *arg)
{
    if (arg->size)
        do_listxattr_read(f, in, arg->size);
    else
        do_listxattr_size(f, in);
}

static void do_removexattr(struct fuse *f, struct fuse_in_header *in,
                           char *name)
{
    int res;
    char *path;

    res = -ENOENT;
    path = get_path(f, in->ino);
    if (path != NULL) {
        res = -ENOSYS;
        if (f->op.removexattr)
            res = f->op.removexattr(path, name);
        free(path);
    }    
    send_reply(f, in, res, NULL, 0);
}


static void free_cmd(struct fuse_cmd *cmd)
{
    free(cmd->buf);
    free(cmd);
}

void __fuse_process_cmd(struct fuse *f, struct fuse_cmd *cmd)
{
    struct fuse_in_header *in = (struct fuse_in_header *) cmd->buf;
    void *inarg = cmd->buf + sizeof(struct fuse_in_header);
    size_t argsize;
    struct fuse_context *ctx = fuse_get_context();

    dec_avail(f);

    if ((f->flags & FUSE_DEBUG)) {
        printf("unique: %i, opcode: %s (%i), ino: %li, insize: %i\n",
               in->unique, opname(in->opcode), in->opcode, in->ino,
               cmd->buflen);
        fflush(stdout);
    }

    ctx->fuse = f;
    ctx->uid = in->uid;
    ctx->gid = in->gid;
    ctx->pid = in->pid;
    
    argsize = cmd->buflen - sizeof(struct fuse_in_header);
        
    switch (in->opcode) {
    case FUSE_LOOKUP:
        do_lookup(f, in, (char *) inarg);
        break;

    case FUSE_GETATTR:
        do_getattr(f, in);
        break;

    case FUSE_SETATTR:
        do_setattr(f, in, (struct fuse_setattr_in *) inarg);
        break;

    case FUSE_READLINK:
        do_readlink(f, in);
        break;

    case FUSE_GETDIR:
        do_getdir(f, in);
        break;

    case FUSE_MKNOD:
        do_mknod(f, in, (struct fuse_mknod_in *) inarg);
        break;
            
    case FUSE_MKDIR:
        do_mkdir(f, in, (struct fuse_mkdir_in *) inarg);
        break;
            
    case FUSE_UNLINK:
        do_unlink(f, in, (char *) inarg);
        break;

    case FUSE_RMDIR:
        do_rmdir(f, in, (char *) inarg);
        break;

    case FUSE_SYMLINK:
        do_symlink(f, in, (char *) inarg, 
                   ((char *) inarg) + strlen((char *) inarg) + 1);
        break;

    case FUSE_RENAME:
        do_rename(f, in, (struct fuse_rename_in *) inarg);
        break;
            
    case FUSE_LINK:
        do_link(f, in, (struct fuse_link_in *) inarg);
        break;

    case FUSE_OPEN:
        do_open(f, in, (struct fuse_open_in *) inarg);
        break;

    case FUSE_FLUSH:
        do_flush(f, in, (struct fuse_flush_in *) inarg);
        break;

    case FUSE_RELEASE:
        do_release(f, in, (struct fuse_release_in *) inarg);
        break;

    case FUSE_READ:
        do_read(f, in, (struct fuse_read_in *) inarg);
        break;

    case FUSE_WRITE:
        do_write(f, in, (struct fuse_write_in *) inarg);
        break;

    case FUSE_STATFS:
        do_statfs(f, in);
        break;

    case FUSE_FSYNC:
        do_fsync(f, in, (struct fuse_fsync_in *) inarg);
        break;

    case FUSE_SETXATTR:
        do_setxattr(f, in, (struct fuse_setxattr_in *) inarg);
        break;

    case FUSE_GETXATTR:
        do_getxattr(f, in, (struct fuse_getxattr_in *) inarg);
        break;

    case FUSE_LISTXATTR:
        do_listxattr(f, in, (struct fuse_getxattr_in *) inarg);
        break;

    case FUSE_REMOVEXATTR:
        do_removexattr(f, in, (char *) inarg);
        break;

    default:
        send_reply(f, in, -ENOSYS, NULL, 0);
    }

    free_cmd(cmd);
}

int __fuse_exited(struct fuse* f)
{
    return f->exited;
}

struct fuse_cmd *__fuse_read_cmd(struct fuse *f)
{
    ssize_t res;
    struct fuse_cmd *cmd;
    struct fuse_in_header *in;
    void *inarg;

    cmd = (struct fuse_cmd *) malloc(sizeof(struct fuse_cmd));
    if (cmd == NULL) {
        fprintf(stderr, "fuse: failed to allocate cmd in read\n");
        return NULL;
    }
    cmd->buf = (char *) malloc(FUSE_MAX_IN);
    if (cmd->buf == NULL) {
        fprintf(stderr, "fuse: failed to allocate read buffer\n");
        free(cmd);
        return NULL;
    }
    in = (struct fuse_in_header *) cmd->buf;
    inarg = cmd->buf + sizeof(struct fuse_in_header);

    res = read(f->fd, cmd->buf, FUSE_MAX_IN);
    if (res == -1) {
        free_cmd(cmd);
        if (__fuse_exited(f) || errno == EINTR)
            return NULL;
        
        /* ENODEV means we got unmounted, so we silenty return failure */
        if (errno != ENODEV) {
            /* BAD... This will happen again */
            perror("fuse: reading device");
        }
        
        fuse_exit(f);
        return NULL;
    }
    if ((size_t) res < sizeof(struct fuse_in_header)) {
        free_cmd(cmd);
        /* Cannot happen */
        fprintf(stderr, "short read on fuse device\n");
        fuse_exit(f);
        return NULL;
    }
    cmd->buflen = res;
    
    /* Forget is special, it can be done without messing with threads. */
    if (in->opcode == FUSE_FORGET) {
        do_forget(f, in, (struct fuse_forget_in *) inarg);
        free_cmd(cmd);
        return NULL;
    }

    return cmd;
}

int fuse_loop(struct fuse *f)
{
    if (f == NULL)
        return -1;

    while (1) {
        struct fuse_cmd *cmd;

        if (__fuse_exited(f))
            break;

        cmd = __fuse_read_cmd(f);
        if (cmd == NULL)
            continue;

        __fuse_process_cmd(f, cmd);
    }
    f->exited = 0;
    return 0;
}

int fuse_invalidate(struct fuse *f, const char *path)
{
    int res;
    int err;
    fino_t ino;
    struct fuse_user_header h;

    err = path_lookup(f, path, &ino);
    if (err) {
        if (err == -ENOENT)
            return 0;
        else
            return err;
    }

    memset(&h, 0, sizeof(struct fuse_user_header));
    h.opcode = FUSE_INVALIDATE;
    h.ino = ino;
    
    if ((f->flags & FUSE_DEBUG)) {
        printf("INVALIDATE ino: %li\n", ino);
        fflush(stdout);
    }

    res = write(f->fd, &h, sizeof(struct fuse_user_header));
    if (res == -1) {
        if (errno != ENOENT) {
            perror("fuse: writing device");
            return -errno;
        }
    }
    return 0;
}

void fuse_exit(struct fuse *f)
{
    f->exited = 1;
}

struct fuse_context *fuse_get_context()
{
    static struct fuse_context context;
    if (fuse_getcontext)
        return fuse_getcontext();
    else
        return &context;
}

void __fuse_set_getcontext_func(struct fuse_context *(*func)(void))
{
    fuse_getcontext = func;
}

static int check_version(struct fuse *f)
{
    int res;
    const char *version_file = FUSE_VERSION_FILE_NEW;
    FILE *vf = fopen(version_file, "r");
    if (vf == NULL) {
        version_file = FUSE_VERSION_FILE_OLD;
        vf = fopen(version_file, "r");
        if (vf == NULL) {
            struct stat tmp;
            if (stat(FUSE_DEV_OLD, &tmp) != -1) {
                fprintf(stderr, "fuse: kernel interface too old, need >= %i.%i\n",
                        FUSE_KERNEL_VERSION, FUSE_KERNEL_MINOR_VERSION);
                return -1;
            } else {
                fprintf(stderr, "fuse: warning: version of kernel interface unknown\n");
                return 0;
            }
        }
    }
    res = fscanf(vf, "%i.%i", &f->majorver, &f->minorver);
    fclose(vf);
    if (res != 2) {
        fprintf(stderr, "fuse: error reading %s\n", version_file);
        return -1;
    }
    if (f->majorver != FUSE_KERNEL_VERSION) {
        fprintf(stderr, "fuse: bad kernel interface major version: needs %i\n",
                FUSE_KERNEL_VERSION);
        return -1;
    }
    if (f->minorver < FUSE_KERNEL_MINOR_VERSION) {
        fprintf(stderr, "fuse: kernel interface too old: need >= %i.%i\n",
                FUSE_KERNEL_VERSION, FUSE_KERNEL_MINOR_VERSION);
        return -1;
    }    
    
    return 0;
}


int fuse_is_lib_option(const char *opt)
{
    if (strcmp(opt, "debug") == 0 ||
        strcmp(opt, "hard_remove") == 0)
        return 1;
    else
        return 0;
}

static int parse_lib_opts(struct fuse *f, const char *opts)
{
    if (opts) {
        char *xopts = strdup(opts);
        char *s = xopts;
        char *opt;

        if (xopts == NULL)
            return -1;
        
        while((opt = strsep(&s, ","))) {
            if (strcmp(opt, "debug") == 0)
                f->flags |= FUSE_DEBUG;
            else if (strcmp(opt, "hard_remove") == 0)
                f->flags |= FUSE_HARD_REMOVE;
            else 
                fprintf(stderr, "fuse: warning: unknown option `%s'\n", opt);
        }
        free(xopts);
    }
    return 0;
}

struct fuse *fuse_new(int fd, const char *opts, const struct fuse_operations *op)
{
    struct fuse *f;
    struct node *root;

    f = (struct fuse *) calloc(1, sizeof(struct fuse));
    if (f == NULL)
        goto out;

    if (check_version(f) == -1)
        goto out_free;

    if (parse_lib_opts(f, opts) == -1)
        goto out_free;

    f->fd = fd;
    f->ctr = 0;
    f->generation = 0;
    /* FIXME: Dynamic hash table */
    f->name_table_size = 14057;
    f->name_table = (struct node **)
        calloc(1, sizeof(struct node *) * f->name_table_size);
    if (f->name_table == NULL)
        goto out_free;

    f->ino_table_size = 14057;
    f->ino_table = (struct node **)
        calloc(1, sizeof(struct node *) * f->ino_table_size);
    if (f->ino_table == NULL)
        goto out_free_name_table;

    pthread_mutex_init(&f->lock, NULL);
    f->numworker = 0;
    f->numavail = 0;
    f->op = *op;
    f->exited = 0;

    root = (struct node *) calloc(1, sizeof(struct node));
    if (root == NULL)
        goto out_free_ino_table;

    root->mode = 0;
    root->rdev = 0;
    root->name = strdup("/");
    if (root->name == NULL)
        goto out_free_root;

    root->parent = 0;
    root->ino = FUSE_ROOT_INO;
    root->generation = 0;
    hash_ino(f, root);

    return f;

 out_free_root:
    free(root);
 out_free_ino_table:
    free(f->ino_table);
 out_free_name_table:
    free(f->name_table);
 out_free:
    free(f);
 out:
    fprintf(stderr, "fuse: failed to allocate fuse object\n");
    return NULL;
}

void fuse_destroy(struct fuse *f)
{
    size_t i;
    for (i = 0; i < f->ino_table_size; i++) {
        struct node *node;

        for (node = f->ino_table[i]; node != NULL; node = node->ino_next) {
            if (node->is_hidden) {
                char *path = get_path(f, node->ino);
                if (path)
                    f->op.unlink(path);
            }
        }
    }
    for (i = 0; i < f->ino_table_size; i++) {
        struct node *node;
        struct node *next;

        for (node = f->ino_table[i]; node != NULL; node = next) {
            next = node->ino_next;
            free_node(node);
        }
    }
    free(f->ino_table);
    free(f->name_table);
    pthread_mutex_destroy(&f->lock);
    free(f);
}
