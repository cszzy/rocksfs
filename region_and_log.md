eRPC buffer 现在最多好像只能传输3824B，在region split时如何扩大进行bulk传输?

interface：

- region需要进行分裂时，向zk请求，分配的新的new_region(zk如何确定分裂点)

  ```c++
  void region_split(old_region, new_region &);
  ```

- 对于文件或目录操作

  - 如果ops对应region正在进行分裂，则除了执行操作还需要log更新操作，同时更新region信息

    ```c++
    bool check_op_and_log(op_key);
    ```

    region结构：

    ```c++
    struct RegionKey {
        uint64_t hi; // hi for pinode
        uint64_t low; // low for hash(fname)
    };
    
    struct ServerRegion {
        uint64_t region_id; 
        uint64_t kv_num; // amount of kvs in this region
        RegionKey start_key;
        RegionKey end_key;
    };
    ```

    log格式：

    ```c++
    key: region_id + log_id
    value: op(文件操作转换为PUT/DELETE KV操作)
    ```

  - 如果对应region没有进行分裂，则只需更新region信息，然后判断是否需要进行region split

    ```c++
    bool update_and_check_region(op);
    ```

- 需要分裂region时，向target server发送`create_region`RPC，target使用`read_region`RPC分多次从origin server读取新region并进行rebuild

  ```c++
  rpc_resp_t rpc_create_region(new_region_id);
  ```

  ```c++
  rpc_resp_t rpc_read_region(new_region_id, offset&);
  void rebuild_region();
  ```

  target server rebuild region后，发起`read_region_log`RPC从origin server读取该region的log并apply log

  ```c++
  rpc_resp_t rpc_read_regionlog(region_id, offset&);
  void replay_region_log();
  ```

  apply log almost complete时，通知origin server。 origin server将剩下的region log发送给target进行replay，对于客户端的属于该region的请求，origin server不再处理和进行log，全部拒绝，要求客户端等待一段时间后重试（可以让client更新region缓存，重定向请求到target）。等待target apply结束，更新zk的region信息。origin server进行kv和log的空间回收。

  ```c++
  rpc_resp_t rpc_almost_done();
  void update_region_info();
  ```

  

https://zhuanlan.zhihu.com/p/46372968

- ls/rmdir如何操作的（readdir只使用pinode）-- zookeeper服务器需要缓存pinode下的所有文件哈希值的范围

- zookeeper：当Client需要操作一个key，首先问zk这个key属于哪一个region，zk向client返回这个region的相关信息。Client需要缓存region信息，region后续可能还会发生变更，Client要知道并令缓存失效，去zk获取最新的region信息

  当region需要进行spllit时，需要向zk请求新的region id；系统初始化时，如何初始化region？

  region阈值：96M(tinykv)

