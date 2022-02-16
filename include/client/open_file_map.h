#pragma once

#include <map>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <atomic>

#include "common/fs.h"

namespace metafs {

/* Forward declaration */
class OpenDir;

enum class OpenFile_flags {
    append = 0,
    creat,
    trunc,
    rdonly,
    wronly,
    rdwr,
    cloexec,
    flag_count // this is purely used as a size variable of this enum class
};

enum class FileType {
    regular,
    directory
};

class OpenFile {
protected:
    FileType type_;
    std::string path_;
    metafs_inode_t inode_; // 文件自身的inode
    std::array<bool, static_cast<int>(OpenFile_flags::flag_count)> flags_ = {{false}};
    unsigned long pos_;
    std::mutex pos_mutex_;
    std::mutex flag_mutex_;

    metafs_inode_t pinode_; // parent inode
    std::string fname_; // file_name
    // metafs_inode_t inode; // itself inode

public:
    // multiple threads may want to update the file position if fd has been duplicated by dup()

    OpenFile(const std::string& path, int flags, metafs_inode_t pinode, 
                const std::string &fname, metafs_inode_t inode, FileType type = FileType::regular);

    ~OpenFile() = default;

    // getter/setter
    std::string path() const;
    void path(const std::string& path_);

    unsigned long pos();
    void pos(unsigned long pos_);

    // get pinode
    metafs_inode_t pinode();

    // get fname
    std::string &fname();

    bool get_flag(OpenFile_flags flag);
    void set_flag(OpenFile_flags flag, bool value);

    FileType type() const;
};


class OpenFileMap {

private:
    std::unordered_map<int, std::shared_ptr<OpenFile>> files_;
    std::recursive_mutex files_mutex_;

    int safe_generate_fd_idx_();

    /*
     * TODO: Setting our file descriptor index to a specific value is dangerous because we might clash with the kernel.
     * E.g., if we would passthrough and not intercept and the kernel assigns a file descriptor but we will later use
     * the same fd value, we will intercept calls that were supposed to be going to the kernel. This works the other way around too.
     * To mitigate this issue, we set the initial fd number to a high value. We "hope" that we do not clash but this is no permanent solution.
     * Note: This solution will probably work well already for many cases because kernel fd values are reused, unlike to ours.
     * The only case where we will clash with the kernel is, if one process has more than 100000 files open at the same time.
     */
    int fd_idx;
    std::mutex fd_idx_mutex;
    std::atomic<bool> fd_validation_needed;

public:
    OpenFileMap();

    std::shared_ptr<OpenFile> get(int fd);

    std::shared_ptr<OpenDir> get_dir(int dirfd);

    bool exist(int fd);

    int add(std::shared_ptr<OpenFile>);

    bool remove(int fd);

    int dup(int oldfd);

    int dup2(int oldfd, int newfd);

    int generate_fd_idx();

    int get_fd_idx();
};

} // end namespace metafs