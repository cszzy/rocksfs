# for hook
ulimit -c unlimited

# mytest
# export METACLIENT_PATH=/home/zzy/metafs/build/libmetafs_client.so
# export METAFS_CLIENT_CONFIG=/home/zzy/metafs/config/client.json
# export INTERCEPT_HOOK_CMDLINE_FILTER=test
# LD_PRELOAD=/home/zzy/metafs/build/libmetafs_client.so ./test

# mpi-mdtest
export MF_PATH=/home/zzy/metafs/build/libmetafs_client.so
export MF_CPATH=/home/zzy/metafs/config/client.json
mpiexec --allow-run-as-root \
    -N 1 \
    -x METAFS_CLIENT_CONFIG=${MF_CPATH} \
    -x INTERCEPT_HOOK_CMDLINE_FILTER=mdtest \
    -x LD_PRELOAD=${MF_PATH} \
        mdtest -n 250000 -C -u -z 1 -b 10 -d /tmp/metafs

# filebench test
# export METACLIENT_PATH=/home/zzy/metafs/build/libmetafs_client.so
# export METAFS_CLIENT_CONFIG=/home/zzy/metafs/config/client.json
# echo 0 > /proc/sys/kernel/randomize_va_space
# export INTERCEPT_HOOK_CMDLINE_FILTER=filebench
# LD_PRELOAD=/home/zzy/metafs/build/libmetafs_client.so filebench -f ../filebench/makedirs.f
