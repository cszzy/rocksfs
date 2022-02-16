#pragma once

#include <string>
#include <vector>

#include "client/open_file_map.h"
#include "common/fs.h"

namespace metafs {

class DirEntry {
private:
    std::string name_;
    FileType type_;
    metafs_inode_t inode_;
public:
    DirEntry(const std::string& name, FileType type, metafs_inode_t inode);

    const std::string& name();

    FileType type();
};

class OpenDir : public OpenFile {
private:
    std::vector<DirEntry> entries;

public:
    explicit OpenDir(const std::string& path, metafs_inode_t pinode, const std::string& fname, metafs_inode_t inode);

    void add(const std::string& name, const FileType& type, metafs_inode_t inode);

    const DirEntry& getdent(unsigned int pos);

    void clearEntries();

    size_t size();
};

} // end namespace metafs