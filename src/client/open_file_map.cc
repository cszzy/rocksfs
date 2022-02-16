#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <glog/logging.h>

#include "client/open_dir.h"
#include "client/open_file_map.h"

#include <fcntl.h>
#include <type_traits>

template<typename E>
constexpr typename std::underlying_type<E>::type to_underlying(E e) {
    return static_cast<typename std::underlying_type<E>::type>(e);
}

using namespace std;
namespace metafs {

OpenFile::OpenFile(const std::string& path, int flags, metafs_inode_t pinode, 
                const std::string &fname, metafs_inode_t inode, FileType type) :
        pinode_(pinode), fname_(fname), inode_(inode), type_(type), path_(path) {
    // set flags to OpenFile
    if (flags & O_CREAT)
        flags_[to_underlying(OpenFile_flags::creat)] = true;
    if (flags & O_APPEND)
        flags_[to_underlying(OpenFile_flags::append)] = true;
    if (flags & O_TRUNC)
        flags_[to_underlying(OpenFile_flags::trunc)] = true;
    if (flags & O_RDONLY)
        flags_[to_underlying(OpenFile_flags::rdonly)] = true;
    if (flags & O_WRONLY)
        flags_[to_underlying(OpenFile_flags::wronly)] = true;
    if (flags & O_RDWR)
        flags_[to_underlying(OpenFile_flags::rdwr)] = true;

    pos_ = 0; // If O_APPEND flag is used, it will be used before each write.
}

OpenFileMap::OpenFileMap() :
        fd_idx(START_FD),
        fd_validation_needed(false) {}

std::string OpenFile::path() const {
    return path_;
}

void OpenFile::path(const string& path) {
    OpenFile::path_ = path;
}

unsigned long OpenFile::pos() {
    lock_guard<mutex> lock(pos_mutex_);
    return pos_;
}

void OpenFile::pos(unsigned long pos) {
    lock_guard<mutex> lock(pos_mutex_);
    OpenFile::pos_ = pos;
}

bool OpenFile::get_flag(OpenFile_flags flag) {
    lock_guard<mutex> lock(pos_mutex_);
    return flags_[to_underlying(flag)];
}

void OpenFile::set_flag(OpenFile_flags flag, bool value) {
    lock_guard<mutex> lock(flag_mutex_);
    flags_[to_underlying(flag)] = value;
}

FileType OpenFile::type() const {
    return type_;
}

metafs_inode_t OpenFile::pinode(){
    return pinode_;
}

std::string& OpenFile::fname() {
    return fname_;
}

// OpenFileMap starts here

shared_ptr<OpenFile> OpenFileMap::get(int fd) {
    lock_guard<recursive_mutex> lock(files_mutex_);
    auto f = files_.find(fd);
    if (f == files_.end()) {
        return nullptr;
    } else {
        return f->second;
    }
}

shared_ptr<OpenDir> OpenFileMap::get_dir(int dirfd) {
    auto f = get(dirfd);
    if (f == nullptr || f->type() != FileType::directory) {
        return nullptr;
    }
    return static_pointer_cast<OpenDir>(f);
}

bool OpenFileMap::exist(const int fd) {
    lock_guard<recursive_mutex> lock(files_mutex_);
    auto f = files_.find(fd);
    return !(f == files_.end());
}

int OpenFileMap::safe_generate_fd_idx_() {
    auto fd = generate_fd_idx();
    /*
     * Check if fd is still in use and generate another if yes
     * Note that this can only happen once the all fd indices within the int has been used to the int::max
     * Once this limit is exceeded, we set fd_idx back to 3 and begin anew. Only then, if a file was open for
     * a long time will we have to generate another index.
     *
     * This situation can only occur when all fd indices have been given away once and we start again,
     * in which case the fd_validation_needed flag is set. fd_validation is set to false, if
     */
    if (fd_validation_needed) {
        while (exist(fd)) {
            fd = generate_fd_idx();
        }
    }
    return fd;
}

int OpenFileMap::add(std::shared_ptr<OpenFile> open_file) {
    auto fd = safe_generate_fd_idx_();
    lock_guard<recursive_mutex> lock(files_mutex_);
    files_.insert(make_pair(fd, open_file));
    return fd;
}

bool OpenFileMap::remove(const int fd) {
    lock_guard<recursive_mutex> lock(files_mutex_);
    auto f = files_.find(fd);
    if (f == files_.end()) {
        return false;
    }
    files_.erase(fd);
    if (fd_validation_needed && files_.empty()) {
        fd_validation_needed = false;
        LOG(INFO) << "fd_validation flag reset";
    }
    return true;
}

int OpenFileMap::dup(const int oldfd) {
    lock_guard<recursive_mutex> lock(files_mutex_);
    auto open_file = get(oldfd);
    if (open_file == nullptr) {
        errno = EBADF;
        return -1;
    }
    auto newfd = safe_generate_fd_idx_();
    files_.insert(make_pair(newfd, open_file));
    return newfd;
}

int OpenFileMap::dup2(const int oldfd, const int newfd) {
    lock_guard<recursive_mutex> lock(files_mutex_);
    auto open_file = get(oldfd);
    if (open_file == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (oldfd == newfd)
        return newfd;
    // remove newfd if exists in filemap silently
    if (exist(newfd)) {
        remove(newfd);
    }
    // to prevent duplicate fd idx in the future. First three fd are reservered by os streams that we do not overwrite
    if (get_fd_idx() < newfd && newfd != 0 && newfd != 1 && newfd != 2)
        fd_validation_needed = true;
    files_.insert(make_pair(newfd, open_file));
    return newfd;
}

/**
 * Generate new file descriptor index to be used as an fd within one process
 * @return fd_idx
 */
int OpenFileMap::generate_fd_idx() {
    // We need a mutex here for thread safety
    std::lock_guard<std::mutex> inode_lock(fd_idx_mutex);
    if (fd_idx == std::numeric_limits<int>::max()) {
        LOG(INFO) << " Warning: File descriptor index exceeded ints max value. Setting it back to " << START_FD;
        /*
         * Setting fd_idx back to 3 could have the effect that fd are given twice for different path.
         * This must not happen. Instead a flag is set which tells can tell the OpenFileMap that it should check
         * if this fd is really safe to use.
         */
        fd_idx = START_FD;
        fd_validation_needed = true;
    }
    return fd_idx++;
}

int OpenFileMap::get_fd_idx() {
    std::lock_guard<std::mutex> inode_lock(fd_idx_mutex);
    return fd_idx;
}


} // end namespace metafs