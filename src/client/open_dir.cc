#include "client/open_dir.h"
#include <stdexcept>
#include <cstring>

namespace metafs {

DirEntry::DirEntry(const std::string& name, const FileType type, metafs_inode_t inode) :
        name_(name), type_(type), inode_(inode) {
}

const std::string& DirEntry::name() {
    return name_;
}

FileType DirEntry::type() {
    return type_;
}

OpenDir::OpenDir(const std::string& path, metafs_inode_t pinode, const std::string& fname, metafs_inode_t inode) :
        OpenFile(path, 0, pinode, fname, inode, FileType::directory) {
}

void OpenDir::add(const std::string& name, const FileType& type, metafs_inode_t inode) {
    entries.push_back(DirEntry(name, type, inode));
}

const DirEntry& OpenDir::getdent(unsigned int pos) {
    return entries.at(pos);
}

size_t OpenDir::size() {
    return entries.size();
}

void OpenDir::clearEntries() {
    entries.clear();
}

} //end namespace metafs