pkill -9 mdtest

rm -rf /mnt/pmem01/metafs*
rm -rf /tmp/metafs*

export MC_IP=localhost
export MC_PORT=11211

echo memcached ${MC_IP} ${MC_PORT}
#clear
nc -C ${MC_IP} ${MC_PORT}<< AAA
flush_all
set CLIENT_ID 0 0 1
0
set SERVER_ID 0 0 1
0
quit
AAA

echo "clear done"