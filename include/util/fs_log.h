#pragma once

#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <syscall.h>
#include <libsyscall_intercept_hook_point.h>

namespace metafs {

// use for open log
// #define KVFSDEBUG

#ifndef KVFSDEBUG
#define KVFS_LOG(...)
#else 
class KVFSLogger {
public:
    static KVFSLogger * GetInstance() { 
        if(instance_ == nullptr)
        {
            instance_ = new KVFSLogger();
        }
        return instance_;
    }

    void Log(const char *file_name,int line ,const char * data ,  ...) {
        va_list ap;
        va_start(ap,data);
        //std::cout << data << std::endl;
        fprintf(fp,"[%s : %d]- ",file_name ,line);
        vfprintf(fp,data,ap);
        int len = strlen(data);
        if(data[len-1] != '\n')
            fprintf(fp,"\n");
        fflush(fp);
    }

protected:
    KVFSLogger() :
        logfile_name("fs_log.log")
    {
        fp = fopen(logfile_name.c_str(),"w");
        assert(fp);

    };
    std::string logfile_name;
    FILE * fp;

    static KVFSLogger * instance_;
};

#define KVFS_LOG(...) KVFSLogger::GetInstance()->Log(__FILE__,__LINE__,__VA_ARGS__)

#endif // NDEBUG

#if 0
#define FS_LOG(fmt, ...) (                                               \
		{                                                                 \
			char _l[1024];                                                  \
			int _n;                                                         \
			_n = snprintf(_l, sizeof(_l), "LOG: " fmt "\n", ##__VA_ARGS__); \
			syscall_no_intercept(SYS_write, 2, _l, _n);                     \
		})
#else
#define FS_LOG(fmt, ...) ((void)(0))
#endif

} // end namespace metafs

