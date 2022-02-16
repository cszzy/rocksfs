## XXX

```
inode(64bits)格式：最高位0标识为文件，为1标识为目录。每个服务器的每个线程占用16 bits保证inode分配不重叠：

|--1bit whether is file or directory--|--7bit not use now--|--8bits server_id--|--8bits server_thread_id--|--40 bits 用来分配inode--|
```

> 客户端使用Cache缓存文件inode,即key= pinode+fname, value = inode

> Note: eRPC中在enque_request后，必须`rpc_->run_event_loop_once()`,rpc结果才能被处理

> 在readdir,服务器可以在一个req_handler内多次enqeue_response吗?

> git失败：export https_proxy=192.168.0.1:7890

> 通过`resize_msg_buffer`resize rpc buffer大小

<!-- ```c++
typedef std::function<void (ReqHandle *req_handle, void *context)> erpc_req_func_t;
``` -->

>用来发送多个RPC的？
  ```c++
  /// A preallocated msgbuf for single-packet responses
  MsgBuffer pre_resp_msgbuf_;

  /// A non-preallocated msgbuf for possibly multi-packet responses
  MsgBuffer dyn_resp_msgbuf_;
  ```
---
## eRPC的`app/masstree`代码分析
> masstree: 基数树和B+树的优化

> masstree分为CLIENT端和SERVER端
### SERVER
1. 创建masstree，启动多线程向内部插入键值对.
2. 创建nexus(每个进程创建一个)
    - 注意eRPC规定服务器的端口必须在[31850,31850+32),`numa_node`表示进程绑定的numa节点,`num_bg_threads`表示后台处理RPC请求的线程数量(可以为0).
    ```c++
    erpc::Nexus nexus(erpc::get_uri_for_process(FLAGS_process_id),
    FLAGS_numa_node, FLAGS_num_server_bg_threads);
    ```
    - masstree中的请求分为点查询和范围查询。因此在服务器分别为他们注册处理函数.注意处理函数注册时分为`kForeground`和`kBackground`两种类型,表示请求处理函数的两种类型.前台模式的处理函数运行在调用事件循环的线程中,后台模式的处理函数运行在eRPC产生的后台线程中.默认注册为前台类型.
    masstree中,点查询注册为前台类型.对于范围查询,如果`num_bg_threads`不为0,则注册为后台类型,否则注册为前台类型.
    在masstree中,前台线程即为处理客户端请求的线程,后台线程即为nexus中启动的eRPC线程.
    ```c++
    nexus.register_req_func(kAppPointReqType, point_req_handler,erpc::ReqFuncType::kForeground);
    ```
3. 创建`num_server_fg_threads`个线程，每个线程创建一个eRPC对象.对于每个线程:
    - 每个线程用于处理客户请求,绑定到同一个`numa_node`.
    - 每个线程运行`server_thread_func`函数.该函数首先创建`AppContext`,这是每个线程专属的，保存一些该线程的信息和统计信息.然后使用取模的方法计算线程应该绑定到的`numa_node`的`phy_port`.最后使用`nexus`,`Appcontext`,`basic_sm_handler`,`thread_id`,`phy_port`创建`RPC`对象,进入事件循环.注意`Appcontext`会作为之前注册的请求处理函数以及`basic_sm_handler`函数的参数.`thread_id`作为`rpc_id`使用,同一进程的多个线程创建的`RPC`对象必须有唯一的`rpc_id`,在与远端进行连接时使用`URI+rpc_id`.`sm_handler`作为session管理的回调,在session被成功创建和销毁时被调用(多个客户端的RPC对象可以和同一个服务端RPC对象创建session，同样一个客户端RPC对象也可以和多个客户端RPC对象建立session).每个RPC对象使用网卡的一个port,`phy_port`是活跃端口的索引号.
    ```c++
    erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(&c),
    static_cast<uint8_t>(thread_id),
    basic_sm_handler, phy_port);
    ```
### METAFS_CLIENT
1. 创建Nexus(每个进程一个),后台线程数量为`num_server_bg_threads`(可以为0).
    ```c++
    erpc::Nexus nexus(erpc::get_uri_for_process(FLAGS_process_id), FLAGS_numa_node, FLAGS_num_server_bg_threads);
    ```
2. 创建`num_client_threads`个客户端线程,所有线程绑定到同一个numa_node.每个线程创建一个eRPC对象，向server发起请求和接收响应.对于每个线程：
    - 首先创建`AppContext`.取模计算`phy_port`.之后创建RPC对象.注意`basic_sm_handler`用于在创建和销毁session时被调用(用来修改`AppContext`内的记录的连接数等).
    ```c++
    erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(&c), static_cast<uint8_t>(thread_id), basic_sm_handler, phy_port);
    ```
    - 计算出客户端线程`client_gid`(gid = processid * num_client_threads + thread_id),即该线程在所有客户端线程中的唯一id.
    - 利用`gid`计算出要连接的服务器线程的id-`server_tid`(server_tid = client_gid % num_server_fg_threads). 
    - 和远程创建session:在masstree中每个客户端线程只创建一个session(在分布式文件系统中：有多少个服务器进程一个客户端进程就应该至少创建多少个与对应服务器连接session).同时规定process=0的进程作为服务器进程,所以使用`get_uri_for_process(0)`获得服务器URI.之后使用`URI+thread_id`创建连接.
    ```c++
    c.session_num_vec_[0] =
      rpc.create_session(erpc::get_uri_for_process(0), server_tid);
    ```
    - 之后轮询`c.num_sm_resps_ == 1`判断是否创建好session.
    - 然后`alloc_req_resp_msg_buffers(&c)`分配MSGbuffer.调用`send_req`不断发送请求和接收响应.
### OTHERS
> 服务器在req_handler函数中，通常会`resize_msg_buffer`(resize的大小必须小于alloc_msg_buffer时申请的大小),客户端也可以这样做(但是eRPC/app样例中中客户端执行这个函数较少).这么做的用途是什么?是可以用来减少RPC message的大小吗?

### TODO
> TODO:删除一个文件或目录,如果是目录是不是应该递归删除捏?

### SYScall trace
  ```
  opendir(char*): SYS_openat（如果不存在则只进行到这步）,否则继续SYS_fstat。如果stat.mode是文件则SYS_close文件fd。
  stat(char*, stat*): SYS_stat
  rmdir(char*): SYS_rmdir
  mkdir(char*, mode_t): 创建目录 SYS_mkdir
  creat(char*, mode_t): 创建文件 SYS_create
  open(char*, O_DIRECTORY): O_DIRECTORY标志打开目录 SYS_openat, （应该先向服务器open，然后RPC_readdir）
  readdir(DIR*): 内部可能调用多次SYS_getdents64（读大目录会发生什么？不清楚）
  closedir(DIR*): SYS_close
  close(fd): SYS_close
  ```

### filebench测试遇到的问题

  ```
  9.063: Waiting for pid 8269 thread filereaderthread-1
  10.063: Waiting for pid 8269 thread filereaderthread-1
  11.064: Running...
  11.064: Unexpected Process termination Code 3, Errno 0 around line 77
  12.065: Run took 1 seconds...
  12.065: Run took 1 seconds...
  ```

原因是系统打开了ASLR。
ASLR(Address Space Layout Randomization)在2005年被引入到Linux的内核 kernel 2.6.12 中，当然早在2004年就以patch的形式被引入。随着内存地址的随机化，使得响应的应用变得随机。这意味着同一应用多次执行所使用内存空间完全不同，也意味着简单的缓冲区溢出攻击无法达到目的。

使用root权限以及如下命令关闭ASLR。

  ```shell
  echo 0 > /proc/sys/kernel/randomize_va_space
  ```

eRPC设置大页 echo 5120 > /proc/sys/vm/nr_hugepages