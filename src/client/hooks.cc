#include <syscall.h>
#include <libsyscall_intercept_hook_point.h>
#include <sys/stat.h>
#include <string>
#include <dlfcn.h>
#include <sys/types.h>

#include "client/hooks.h"
#include "client/metaclient.h"
#include "util/parser.h"
#include "common/common.h"
#include "util/fs_log.h"
#include "util/memcached_tool.h"

extern "C" {
#include <dirent.h> // used for file types in the getdents{,64}() functions
#include <linux/kernel.h> // used for definition of alignment macros
#include <linux/const.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
}

/*
 * linux_dirent is used in getdents() but is privately defined in the linux kernel: fs/readdir.c.
 */
struct linux_dirent {
    unsigned long d_ino;
    unsigned long d_off;
    unsigned short d_reclen;
    char d_name[1];
};

/*
 * linux_dirent64 is used in getdents64() and defined in the linux kernel: include/linux/dirent.h.
 * However, it is not part of the kernel-headers and cannot be imported.
 */
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[1]; // originally `char d_name[0]` in kernel, but ISO C++ forbids zero-size array 'd_name'
};

/*
 * Macro used within getdents{,64} functions.
 * __ALIGN_KERNEL defined in linux/kernel.h
 */
#define ALIGN(x, a)                     __ALIGN_KERNEL((x), (a))


namespace metafs {

struct client_config *c_cfg;

struct client_context *c_ctx;

// parse client_json
struct client_config *client_parse_config(const char *fn) {
    p_assert(fn, "no config file");
    struct conf_parser cparser[] = {
        {"client_threads", offsetof(struct client_config, num_client_threads), cJSON_Number, "1"},
        {"local_ip", offsetof(struct client_config, local_ip), cJSON_String, "localhost"},
        {"num_servers", offsetof(struct client_config, num_servers), cJSON_Number, "1"},
        {"server_list", offsetof(struct client_config, server_list), cJSON_Array, NULL},
        {"server_fg_threads", offsetof(struct client_config, server_fg_threads), cJSON_Number, "1"},
        {"server_bg_threads", offsetof(struct client_config, server_bg_threads), cJSON_Number, "0"},
        {"mount_dir", offsetof(struct client_config, mountdir), cJSON_String, "/tmp/metafs"},
        {"memcached_ip", offsetof(struct client_config, memcached_ip), cJSON_String, "localhost"},
        {"memcached_port", offsetof(struct client_config, memcached_port), cJSON_Number, "0"},
        {NULL, 0, 0, NULL},
    };

    struct client_config *conf = (struct client_config *)safe_alloc(sizeof(struct client_config), true);
    parse_from_file(conf, fn, cparser);
    conf->mountdir_len = strlen(conf->mountdir);
    return conf;
}

// parse config and init client
void init_client_ctx() {
    const char *fn = getenv("METAFS_CLIENT_CONFIG");
    // init config from client_json
    c_cfg = client_parse_config(fn);

    // init client context 
    c_ctx = new client_context();
#ifdef USE_CACHE
    c_ctx->dentry_cache = new DentryCache<DirentryValue>(10000);
    assert(c_ctx->dentry_cache != NULL);
#else 
    c_ctx->dentry_cache = nullptr;
#endif

    //init client id
	{
		struct mc_ctx *mi;
		mi = mt_create_ctx(c_cfg->memcached_ip, c_cfg->memcached_port);
		c_ctx->id = mt_incr(mi, "CLIENT_ID");
		printf("client id %d\n", c_ctx->id);
		mt_destory_ctx(mi);
	}

    c_ctx->local_uri = (char*)safe_alloc(24, true);
    std::string local_uri = c_cfg->local_ip;
    local_uri += ":" + to_string(kUDPPort_Client + c_ctx->id);
    strcpy(c_ctx->local_uri, local_uri.c_str());
    printf("client uri: %s\n", c_ctx->local_uri);

    c_ctx->total_servers = c_cfg->num_servers * c_cfg->server_fg_threads;
    
    // inti erpc
    c_ctx->nexus_ = new erpc::Nexus(local_uri, 0, c_cfg->server_bg_threads);
    
    METAFS_CLIENT->Init();

    p_assert(METAFS_CLIENT->ReadRegionmap() == 0, "read region map fail");
    
    printf("preload ok\n");
}

// /**
//  * @param dirfd parent directory fd
//  * @param raw_path the path passed by user
//  * @param real_path file's path in metafs
//  * @return 1: error, 0: success 
//  */
// int resolve_fd_to_path(int dirfd, const char *raw_path, string &real_path) {
//     const char *mt_dir = c_cfg->mountdir;
//     int mt_dir_len = c_cfg->mountdir_len;

//     assert(mt_dir != nullptr);
//     assert(raw_path != nullptr);

//     if(raw_path[0] == '/') {
//         // 绝对路径,忽略fd
//         // cpath need has consistent prefix with mount_dir
//         if(strncmp(mt_dir, raw_path, mt_dir_len) || raw_path[mt_dir_len] == '\0') {
//             return 1;
//         }
//         const char *fn = raw_path + mt_dir_len;
//         while(*fn == '/') {
//             ++fn;
//         }
        // --fn
//         real_path = fn;
//     } else {
//         // 相对路径,当前只支持fd == AT_FDCWD的情况
//         if(dirfd != AT_FDCWD) {
//             return 1;
//         }
//         //不能是绝对路径，也不能有.和..
//         assert(raw_path[0] != '/' && strncmp(raw_path, "..", 2) && strncmp(raw_path, "./", 2));
        
//         real_path = mt_dir;
//         real_path += '/';
//         real_path += raw_path;
//     }

//     return 0;
// }


// res记录标准规定的错误值
// return 1/0, 1代表hook失败, 0代表hook成功

// TODO:每次opendir都会打开fd，然后使用fd getattr，所以需要在fd中存储stat,不然rpc次数太多了

// res记录fd
int hook_openat(int dirfd, const char* cpath, int flags, mode_t mode, long *res) {
    // if(flags & O_PATH || flags & O_APPEND || flags & O_EXCL) {
    //     *res = -ENOTSUP;
    //      return 1; 
    // }
    if(dirfd != AT_FDCWD) {
        return 1;
    }

    if(cpath[0] != '/') {
        return 1;
    }

    const char* mt_dir = c_cfg->mountdir;
    int mt_dir_len = c_cfg->mountdir_len;
    // cpath need has consistent prefix with mount_dir
    if(strncmp(mt_dir, cpath, mt_dir_len) || cpath[mt_dir_len] == '\0') {
        return 1;
    }

    const char *fn = cpath + mt_dir_len;
    while(*fn == '/') {
        ++fn;
    }
    --fn;

    string fname;
    metafs_inode_t pinode;
    string realpath(fn);

    rocksdb::Status s = METAFS_CLIENT->ResolvePath(realpath, pinode, fname);
    // assert(s.ok());
    if(!s.ok()) {
        *res = -ENOENT;
        return 0;
    }

    metafs_inode_t inode; 
    metafs_stat_t stat;

    if(flags & O_CREAT) {
        FS_LOG("create");
        // create a file or directory
        int ret;
        
        if(flags & O_DIRECTORY || S_ISDIR(mode)) {
            ret = METAFS_CLIENT->Mkdir(pinode, fname, mode | S_IFDIR, inode, stat);
            if(ret != 0) {
                *res = -EEXIST;
                return 0;
            }
            // 目录不创建fd
        } else {
            ret = METAFS_CLIENT->Mknod(pinode, fname, mode | S_IFREG, inode, stat);
            if(ret != 0) {
                *res = -EEXIST;
                return 0;
            }
            *res = METAFS_CLIENT->file_map()->add(make_shared<OpenFile>(realpath, flags, pinode, fname, inode));
        }
        return 0;
    } else {
        FS_LOG("open");
        bool exists = true;
        int ret = METAFS_CLIENT->Open(pinode, fname, flags & O_DIRECTORY ? mode | S_IFDIR : mode | S_IFREG, inode, stat);
        if(ret == -ENOENT) {
            exists = false;
        }

        if(exists == true) {
            // file exists
            if(S_ISDIR(stat.mode)) {
                // 读目录下的所有文件
                auto open_dir = make_shared<OpenDir>(realpath, pinode, fname, inode);
                assert(open_dir);
                ret = METAFS_CLIENT->Readdir(inode, open_dir);
                if(ret) {
                    FS_LOG("get dir entry failed");
                    *res = -EBADF;
                    return -1;
                }
                
                *res = METAFS_CLIENT->file_map()->add(open_dir);
                FS_LOG("opendir success, fd: %d", *res);

            } else if (S_ISREG(stat.mode)) {
                *res = METAFS_CLIENT->file_map()->add(make_shared<OpenFile>(realpath, flags, pinode, fname, inode));
            } else {
                *res = -ENOTSUP;
                return 1;
            }
        } else {
            *res = -ENOENT;
            return 0;
        }
    }
    return 0;    
}

// TODO: 读写文件如果需要sync则需要更新元数据
int hook_fsync(int fd, long *res) {
    if(fd < START_FD) {
        return 1;
    }

    *res = 0;
    return 0;
}


int hook_close(int fd, long *res) {
    if(fd < START_FD) {
        return 1;
    }

    if(METAFS_CLIENT->file_map()->exist(fd)) {
        METAFS_CLIENT->file_map()->remove(fd); 
    }

    *res = 0;
    return 0;
}

int hook_mkdirat(int dirfd, const char *path, mode_t mode, long *res) {
    if(dirfd != AT_FDCWD) {
        return 1;
    }
    long fd;
    int was_hooked = hook_openat(dirfd, path, O_CREAT | O_DIRECTORY, mode, &fd);
    if (was_hooked) {
        return 1;
    }
	// hook_close((int)fd, res);
    *res = 0;
	return 0;
}

int hook_statfs(const char *path, struct statfs *sf, long *res) {
    const char* mt_dir = c_cfg->mountdir;
    int mt_dir_len = c_cfg->mountdir_len;
    if(strncmp(mt_dir, path, mt_dir_len - 1)) {
        return 1;
    }

    // TODO: RPC get fs info
    sf->f_type = METAFS_MAGIC;
    sf->f_bsize = METAFS_BLK_SIZE;
    sf->f_blocks = 40960; // TODO: need to modify
    sf->f_bfree = 40960;
    sf->f_bavail = sf->f_bfree;
    sf->f_files = 0;
    sf->f_ffree = (unsigned long)-1;
    sf->f_fsid = {0, 0};
    sf->f_namelen = METAFS_MAX_FNAME_LEN;
    sf->f_frsize = 0;
	sf->f_flags = ST_NOSUID | ST_NODEV;

    *res = 0;
    return 0;
}


// 删除目录和文件都使用这个函数
// 服务器判断是文件还是目录，决定是否删除
int hook_unlinkat(int dirfd, const char *cpath, int flags, long *res) {
    // cpath need has consistent prefix with mount_dir

    if(dirfd != AT_FDCWD) {
        return 1;
    }

    if(cpath[0] != '/') {
        return 1;
    }

    const char* mt_dir = c_cfg->mountdir;
    int mt_dir_len = c_cfg->mountdir_len;
    // cpath need has consistent prefix with mount_dir
    if(strncmp(mt_dir, cpath, mt_dir_len) || cpath[mt_dir_len] == '\0') {
        return 1;
    }

    const char *fn = cpath + mt_dir_len;
    while(*fn == '/') {
        ++fn;
    }
    --fn;

    string fname;
    metafs_inode_t pinode;
    string realpath(fn);

    rocksdb::Status s = METAFS_CLIENT->ResolvePath(realpath, pinode, fname);
    assert(s.ok());

    if(flags & AT_REMOVEDIR) {
        // remove a dir
        FS_LOG("remove a dir");
        *res = METAFS_CLIENT->Rmdir(pinode, fname);
    } else {
        *res = METAFS_CLIENT->Unlink(pinode, fname);
    }
    
    return 0;
}

int hook_stat(const char *cpath, struct stat *st, long *res) {
    const char* mt_dir = c_cfg->mountdir;
    int mt_dir_len = c_cfg->mountdir_len;
    if(strncmp(mt_dir, cpath, mt_dir_len) || cpath[mt_dir_len] == '\0') {
        return 1;
    }

    const char *fn = cpath + mt_dir_len;
    while(*fn == '/') {
        ++fn;
    }
    --fn;

    metafs_inode_t pinode;
    string fname;

    rocksdb::Status s = METAFS_CLIENT->ResolvePath(string(fn), pinode, fname);
    if(!s.ok()) {
        return -ENOENT;
    }

    metafs_inode_t inode;
    metafs_stat_t stat;
    *res = METAFS_CLIENT->Getstat(pinode, fname, inode, stat);

    if(*res == 0) {
        st->st_dev = 0;
        st->st_ino = inode;
        st->st_mode = stat.mode;
        st->st_nlink = 1;
        st->st_uid = st->st_gid = 0;
        st->st_rdev = 0;
        st->st_atime = stat.atime;
        st->st_ctime = stat.ctime;
        st->st_mtime = stat.mtime;
    }

    return 0;
}


int hook_fstat(int fd, struct stat *st, long *res) {
    if(fd < START_FD) {
        return 1;
    }

    if(METAFS_CLIENT->file_map()->exist(fd)) {
        auto ffd = METAFS_CLIENT->file_map()->get(fd);

        metafs_inode_t pinode = ffd->pinode();
        string fname = ffd->fname();

        metafs_inode_t inode;
        metafs_stat_t stat;

        *res = METAFS_CLIENT->Getstat(pinode, fname, inode, stat);
        if(*res == 0) {
            st->st_dev = 0;
            st->st_ino = inode;
            st->st_mode = stat.mode;
            st->st_nlink = 1;
            st->st_uid = st->st_gid = 0;
            st->st_rdev = 0;
            st->st_atime = stat.atime;
            st->st_ctime = stat.ctime;
            st->st_mtime = stat.mtime;
        }
    } else {
        *res = -EINVAL;
    }

    return 0;
}

int hook_access(const char *path, int mask, long *res) {
    struct stat st;
    long stat_res;
    int was_hooked = hook_stat(path, &st, &stat_res);
    if(was_hooked) {
        return 1;
    }
    if(stat_res == 0) {
        *res = 0;
    } else {
        *res = -ENOENT;
    }
    return 0;
}

int hook_read(int fd, void *buf, size_t len, long *res) {
    if(fd < START_FD) {
        return 1;
    }
    // TODO
    FS_LOG("hook SYS_read, fd: %d", fd);
    // *res = -ENOTSUP;
    *res = 0;
    return 0;
}

int hook_write(int fd, const char *buf, size_t len, long *res) {
    if(fd < START_FD) {
        return 1;
    }
    FS_LOG("hook SYS_write, fd: %d", fd);
    // TODO
    // *res = -ENOTSUP;
    *res = 0;
    return 0;
}

int hook_lseek(int fd, long off, int flag, long *res) {
    if(fd < START_FD) {
        return 1;
    }

    auto fs_fd = METAFS_CLIENT->file_map()->get(fd);

    if(flag = SEEK_SET) {
        if(off < 0) {
            *res = -EINVAL;
            return 1;
        }
        fs_fd->pos(off);
    } else if (flag == SEEK_CUR) {
        fs_fd->pos(fs_fd->pos() + off);
    } else {
        *res = -EINVAL;
        return 1;
    }
    return 0;
}

int hook_getdents(int fd, struct linux_dirent *dirp, int count, long *res) {
    if(fd < START_FD) {
        return 1;
    }

    if(METAFS_CLIENT->file_map()->exist(fd)) {
        auto open_dir = METAFS_CLIENT->file_map()->get_dir(fd);
        if(open_dir == nullptr) {
            //Cast did not succeeded: open_file is a regular file
            *res = -EBADF;
            return 1;
        }

        // get directory position of which entries to return
        auto pos = open_dir->pos();
        if (pos >= open_dir->size()) {
            *res = 0;
            return 0;
        }

        unsigned int written = 0;
        struct linux_dirent* current_dirp = nullptr;
        while (pos < open_dir->size()) {
            // get dentry fir current position
            auto de = open_dir->getdent(pos);
            /*
            * Calculate the total dentry size within the kernel struct `linux_dirent` depending on the file name size.
            * The size is then aligned to the size of `long` boundary.
            * This line was originally defined in the linux kernel: fs/readdir.c in function filldir():
            * int reclen = ALIGN(offsetof(struct linux_dirent, d_name) + namlen + 2, sizeof(long));
            * However, since d_name is null-terminated and de.name().size() does not include space
            * for the null-terminator, we add 1. Thus, + 3 in total.
            */
            auto total_size = ALIGN(offsetof(
                                            struct linux_dirent, d_name) + de.name().size() + 3, sizeof(long));
            if (total_size > (count - written)) {
                //no enough space left on user buffer to insert next dirent
                break;
            }
            current_dirp = reinterpret_cast<struct linux_dirent*>(reinterpret_cast<char*>(dirp) + written);
            current_dirp->d_ino = std::hash<std::string>()(
                    open_dir->path() + "/" + de.name());

            current_dirp->d_reclen = total_size;

            *(reinterpret_cast<char*>(current_dirp) + total_size - 1) =
                    ((de.type() == FileType::regular) ? DT_REG : DT_DIR);

            FS_LOG("name: %d, pos : %s", pos, de.name().c_str());
            std::strcpy(&(current_dirp->d_name[0]), de.name().c_str());
            ++pos;
            current_dirp->d_off = pos;
            written += total_size;
        }

        if (written == 0) {
            *res = -EINVAL;
            return 1;
        }
        // set directory position for next getdents() call
        open_dir->pos(pos);
        *res =  written;
    }
    return 0;
}

int hook_getdents64(int fd, struct linux_dirent64 *dirp, int count, long *res) {
    if(fd < START_FD) {
        return 1;
    }

    if(METAFS_CLIENT->file_map()->exist(fd)) {
        auto open_dir = METAFS_CLIENT->file_map()->get_dir(fd);
        if (open_dir == nullptr) {
            //Cast did not succeeded: open_file is a regular file
            *res = -EBADF;
            return 1;
        }
        FS_LOG("getdents64: pos: %d, total_ents: %d", open_dir->pos(), open_dir->size());
        auto pos = open_dir->pos();
        if (pos >= open_dir->size()) {
            *res = 0;
            return 0;
        }
        unsigned int written = 0;
        struct linux_dirent64* current_dirp = nullptr;
        while (pos < open_dir->size()) {
            auto de = open_dir->getdent(pos);
            /*
            * Calculate the total dentry size within the kernel struct `linux_dirent` depending on the file name size.
            * The size is then aligned to the size of `long` boundary.
            *
            * This line was originally defined in the linux kernel: fs/readdir.c in function filldir64():
            * int reclen = ALIGN(offsetof(struct linux_dirent64, d_name) + namlen + 1, sizeof(u64));
            * We keep + 1 because:
            * Since d_name is null-terminated and de.name().size() does not include space
            * for the null-terminator, we add 1. Since d_name in our `struct linux_dirent64` definition
            * is not a zero-size array (as opposed to the kernel version), we subtract 1. Thus, it stays + 1.
            */
            auto total_size = ALIGN(offsetof(
                                            struct linux_dirent64, d_name) + de.name().size() + 1, sizeof(uint64_t));
            if (total_size > (count - written)) {
                //no enough space left on user buffer to insert next dirent
                break;
            }
            current_dirp = reinterpret_cast<struct linux_dirent64*>(reinterpret_cast<char*>(dirp) + written);
            current_dirp->d_ino = std::hash<std::string>()(
                    open_dir->path() + "/" + de.name());

            current_dirp->d_reclen = total_size;
            current_dirp->d_type = ((de.type() == FileType::regular) ? DT_REG : DT_DIR);

            FS_LOG("name: %d, pos : %s", pos, de.name().c_str());
            std::strcpy(&(current_dirp->d_name[0]), de.name().c_str());
            ++pos;
            current_dirp->d_off = pos;
            written += total_size;
        }

        if (written == 0) {
            *res = -EINVAL;
            return 1;
        }
        open_dir->pos(pos);
        *res = written;
    }
    return 0;
}

// 返回1表示
// mkdir时调用的似乎是SYS_create
// opendir内部还会调用SYS_close
int hook(long syscall_number,
                long a0, long a1,
                long a2, long a3,
                long a4, long a5,
                long *res) {
    switch(syscall_number) {
        case SYS_open:
            FS_LOG("SYS_open");
            return hook_openat(AT_FDCWD, (char *)a0, (int)a1, (mode_t)a2, res);
        case SYS_creat:
            FS_LOG("SYS_create");
            return hook_openat(AT_FDCWD, (char *)a0, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)a1, res);
        case SYS_openat:
            FS_LOG("SYS_openat");
            return hook_openat((int)a0, (char *)a1, (int)a2, (mode_t)a3, res);
        case SYS_close:
            FS_LOG("SYS_close, fd: %d", (int)a0);
            return hook_close((int)a0, res);
        case SYS_write:
            // FS_LOG("SYS_write, fd: %d", (int)a0);
            return hook_write((int)a0, (char *)a1, (size_t)a2, res);
        case SYS_read:
            // FS_LOG("SYS_read, fd: %d", (int)a0);
            return hook_read((int)a0, (void *)a1, (size_t)a2, res);
        case SYS_lseek:
            FS_LOG("SYS_lseek");
            return hook_lseek((int)a0, a1, (int)a2, res);
        case SYS_fsync:
            FS_LOG("SYS_fsync");
            return hook_fsync((int)a0, res);
        case SYS_stat:
            FS_LOG("SYS_stat");
            return hook_stat((const char *)a0, (struct stat *)a1, res);
        case SYS_fstat:
            FS_LOG("SYS_fstat");
            return hook_fstat((int)a0, (struct stat*)a1, res);
        case SYS_mkdirat:
            FS_LOG("SYS_mkdirat");
            return hook_mkdirat((int)a0, (const char *)a1, (mode_t)a2, res);
        case SYS_mkdir:
            FS_LOG("SYS_mkdir");
            return hook_mkdirat(AT_FDCWD, (const char *)a0, (mode_t)a1, res);
        case SYS_statfs:
            FS_LOG("SYS_statfs");
            return hook_statfs((const char *)a0, (struct statfs *)a1, res);
        case SYS_access:
            FS_LOG("SYS_access");
            return hook_access((const char *)a0, (int)a1, res);
        case SYS_unlink:
            FS_LOG("SYS_unlink");
            return hook_unlinkat(AT_FDCWD, (const char *)a0, 0, res);
        case SYS_rmdir:
            FS_LOG("SYS_rmdir");
            return hook_unlinkat(AT_FDCWD, (const char *)a0, AT_REMOVEDIR, res);
        case SYS_getdents:
            FS_LOG("SYS_getdents");
            return hook_getdents((int)a0, (linux_dirent *)a1, (int)a2, res);
        case SYS_getdents64:
            FS_LOG("SYS_getdents64");
            return hook_getdents64((int)a0, (linux_dirent64 *)a1, (int)a3, res);
        // case SYS_rename:
        //     return hook_renameat(AT_FDCWD, (const char))
        // case SYS_renameat:

        default:
            // FS_LOG("SYS_unhook: %d", syscall_number);
            assert(syscall_number != SYS_fork && syscall_number != SYS_vfork);
            return 1;
    }
    assert(false);
    return 0;
}

int metafs_hook(long syscall_number,
                long a0, long a1,
                long a2, long a3,
                long a4, long a5,
                long *res) {
    thread_local static int reentrance_flag = false;
	int oerrno, was_hooked;
	if (reentrance_flag)
	{
		// FS_LOG("internal sys call %ld", syscall_number);
		return 1;
	}
	reentrance_flag = true;
	oerrno = errno;
	was_hooked = hook(syscall_number, a0, a1, a2, a3, a4, a5, res);
	errno = oerrno;
	reentrance_flag = false;
    // printf("hook ok\n");
	return was_hooked;
}

// rewrite syscall function, faster than hook.=_=实际是人为减少了一些Syscall
#if HOOK_REWRITE
bool init_rewrite_flag = false;

int rmdir(const char *path)
{
	static int (*real_rmdir)(const char *path) = NULL;
	if (unlikely(real_rmdir == NULL))
	{
		real_rmdir = (typeof(real_rmdir))dlsym(RTLD_NEXT, "rmdir");
	}
	long res;
	if (likely(init_rewrite_flag) &&
			0 == hook_unlink(path, &res))
		return res;
	return real_rmdir(path);
}

int unlink(const char *path)
{
	static int (*real_unlink)(const char *path) = NULL;
	if (unlikely(real_unlink == NULL))
	{
		real_unlink = (typeof(real_unlink))dlsym(RTLD_NEXT, "unlink");
	}
	long res;
	if (likely(init_rewrite_flag) &&
			0 == hook_unlink(path, &res))
		return res;
	return real_unlink(path);
}

int stat(const char *path, struct stat *buf)
{
	static int (*real_stat)(const char *path, struct stat *buf) = NULL;
	if (unlikely(real_stat == NULL))
	{
		real_stat = (typeof(real_stat))dlsym(RTLD_NEXT, "stat");
	}
	long res;
	if (likely(init_rewrite_flag) &&
			0 == hook_stat(path, buf, &res))
		return res;
	return real_stat(path, buf);
}

int fsync(int fd)
{
	static int (*real_sync)(int fd) = NULL;
	if (unlikely(real_sync == NULL))
	{
		real_sync = (typeof(real_sync))dlsym(RTLD_NEXT, "fsync");
	}
	long res;
	if (likely(init_rewrite_flag) &&
			0 == hook_fsync(fd, &res))
		return res;
	return real_sync(fd);
}

off_t lseek(int fd, off_t offset, int whence)
{
	static int (*real_seek)(int fd, off_t offset, int whence) = NULL;
	if (unlikely(real_seek == NULL))
	{
		real_seek = (typeof(real_seek))dlsym(RTLD_NEXT, "lseek");
	}
	long res;
	if (likely(init_rewrite_flag) &&
			0 == hook_lseek(fd, offset, whence, &res))
		return res;
	return real_seek(fd, offset, whence);
}

ssize_t read(int fd, void *buf, size_t siz)
{
	static int (*real_read)(int fd, void *buf, size_t siz) = NULL;
	if (unlikely(real_read == NULL))
	{
		real_read = (typeof(real_read))dlsym(RTLD_NEXT, "read");
	}
	long res;
	if (likely(init_rewrite_flag) &&
			0 == hook_read(fd, (char *)buf, siz, &res))
		return res;
	return real_read(fd, buf, siz);
}

ssize_t write(int fd, const void *buf, size_t siz)
{
	static int (*real_write)(int fd, const void *buf, size_t siz) = NULL;
	if (unlikely(real_write == NULL))
	{
		real_write = (typeof(real_write))dlsym(RTLD_NEXT, "write");
	}
	long res;
	if (likely(init_rewrite_flag) &&
			0 == hook_write(fd, (const char *)buf, siz, &res))
		return res;
	return real_write(fd, buf, siz);
}

int close(int fd)
{
	static int (*real_close)(int fd) = NULL;
	if (unlikely(real_close == NULL))
	{
		real_close = (typeof(real_close))dlsym(RTLD_NEXT, "close");
	}
	long res;
	if (likely(init_rewrite_flag) &&
			0 == hook_close(fd, &res))
		return res;
	return real_close(fd);
}

int create(const char *path, mode_t mode)
{
	static int (*real_create)(const char *path, mode_t mode) = NULL;
	if (unlikely(real_create == NULL))
	{
		real_create = (typeof(real_create))dlsym(RTLD_NEXT, "create");
	}
	long res;
	if (likely(init_rewrite_flag) &&
			0 == hook_openat(AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC, mode | S_IFREG, &res))
		return res;
	return real_create(path, mode);
}

int openat(int fd, const char *path, int oflag, ...)
{
	static int (*real_openat)(int fd, const char *path, int oflag, ...) = NULL;
	if (unlikely(real_openat == NULL))
	{
		real_openat = (typeof(real_openat))dlsym(RTLD_NEXT, "openat");
	}
	mode_t mode = 0;
	int was_hooked;
	long res;
	if (oflag & O_CREAT)
	{
		va_list argptr;
		va_start(argptr, oflag);
		mode = va_arg(argptr, mode_t);
		va_end(argptr);
	}
	if (likely(init_rewrite_flag) &&
			0 == hook_openat(fd, path, oflag, mode | S_IFREG, &res))
		return res;
	if (oflag & O_CREAT)
		return real_openat(fd, path, oflag, mode);
	else
		return real_openat(fd, path, oflag);
}

int open(const char *path, int oflag, ...)
{
	static int (*real_open)(const char *path, int oflag, ...) = NULL;
	if (unlikely(real_open == NULL))
	{
		real_open = (typeof(real_open))dlsym(RTLD_NEXT, "open");
	}
	mode_t mode = 0;
	long res;
	if (oflag & O_CREAT)
	{
		va_list argptr;
		va_start(argptr, oflag);
		mode = va_arg(argptr, mode_t);
		va_end(argptr);
	}
	if (likely(init_rewrite_flag) && 0 == hook_openat(AT_FDCWD, path, oflag, mode | S_IFREG, &res))
		return res;

	if (oflag & O_CREAT)
		return real_open(path, oflag, mode);
	else
		return real_open(path, oflag);
}

#endif
} // end namespace metafs