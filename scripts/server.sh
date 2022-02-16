# ../bin/metafs_server --process_id 0 --server_fg_threads 2 --server_bg_threads 0 &
# ../bin/metafs_server --process_id 1 --server_fg_threads 5 --server_bg_threads 5 &
# arg1:server id, arg2:服务器前台线程数,arg3:每个线程分配的PMEM空间(GB)

# /home/zzy/metafs/bin/metafs_server 0 4 16

export METAFS_SERVER_CONFIG=/home/zzy/metafs/config/server.json
/home/zzy/metafs/bin/metafs_server

