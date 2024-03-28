# ovs-dpdk源码笔记（v2.17.2）

## 1. vswitchd 守护进程主函数

```c
// vswitchd/ovs-vswitchd.c
int
main(int argc, char *argv[])
{
    /* ...... */

    daemonize_start(true); // 启动守护进程

    /* ...... */

    while (!exiting) { // 循环直到守护进程结束
        /* ...... */

        bridge_run(); // 实时监控网桥状态变化，dpdk_init()函数在其中

        /* ...... */
    }
    
    /* ...... */

    return 0;
}
```

## 2. dpdk netdev 注册

### 2.1 注册 dpdk 类型的三种 netdev

```c
// main() -> bridge_run() -> dpdk_init() -> dpdk_init__() -> netdev_dpdk_register()

void
netdev_dpdk_register(void)
{
    netdev_register_provider(&dpdk_class);
    netdev_register_provider(&dpdk_vhost_class);
    netdev_register_provider(&dpdk_vhost_client_class);
}
```

### 2.2 定义 struct netdev_class

```c
// lib/netdev.c
/* Initializes and registers a new netdev provider.  After successful
 * registration, new netdevs of that type can be opened using netdev_open(). */
int
netdev_register_provider(const struct netdev_class *new_class)
    OVS_EXCLUDED(netdev_class_mutex, netdev_mutex)
{
    /* ...... */

        error = new_class->init ? new_class->init() : 0; // 

    /* ...... */

    return error;
}


// lib/netdev-provider.h 中提供 struct netdev_class 定义，列出了netdev的基本操作，后续实例时给定具体回调函数
struct netdev_class {

    const char *type; //  system, tap, gre 等类型

    bool is_pmd; // true表示为netdev应该由PMD线程轮询
    
/* ## ------------------- ## */
/* ## Top-Level Functions ## */
/* ## ------------------- ## */

    int (*init)(void); // netdev注册时回调此函数进行初始化，不需要初始化则置null

    void (*run)(const struct netdev_class *netdev_class); // 指定netdev的周期性的任务
    
    void (*wait)(const struct netdev_class *netdev_class); // 配合run()

/* ## ---------------- ## */
/* ## netdev Functions ## */
/* ## ---------------- ## */
    
/* 
*    netdev和netdev_rxq的生命周期
*            "alloc"          "construct"        "destruct"       "dealloc"
*            ------------   ----------------  ---------------  --------------
* netdev      ->alloc        ->construct        ->destruct        ->dealloc
* netdev_rxq  ->rxq_alloc    ->rxq_construct    ->rxq_destruct    ->rxq_dealloc
*/
    struct netdev *(*alloc)(void);
    int (*construct)(struct netdev *);
    void (*destruct)(struct netdev *);
    void (*dealloc)(struct netdev *);

    /* ...... */

    /* netdev的发包函数，在派生后-实例化时绑定具体回调函数 */
    int (*send)(struct netdev *netdev, int qid, struct dp_packet_batch *batch, bool concurrent_txq);
    
    /* netdev的收包函数，在派生后-实例化时绑定具体回调函数 */
    int (*rxq_recv)(struct netdev_rxq *rx, struct dp_packet_batch *batch, int *qfill);
    
    /* ...... */

    /* 其他函数多为set和get */
}
```

### 2.3 dpdk 类型的  netdev_class 实例化

```c
// lib/netdev-dpdk.c
#define NETDEV_DPDK_CLASS_COMMON                            \
    .is_pmd = true, // dpdk的pmd轮询
    /* ...... */

#define NETDEV_DPDK_CLASS_BASE                          \
    NETDEV_DPDK_CLASS_COMMON,                           \
    .init = netdev_dpdk_class_init, // 初始化函数
    /* ...... */
    .rxq_recv = netdev_dpdk_rxq_recv // netdev收包函数，使用rte_eth_rx_burst收包

static const struct netdev_class dpdk_class = {
    .type = "dpdk", // dpdk类型，目前使用的就是这种
    NETDEV_DPDK_CLASS_BASE,
    /* ...... */
    .send = netdev_dpdk_eth_send,  // netdev发包函数，不确定是否在此发包，将包直接放到硬件上？
};

static const struct netdev_class dpdk_vhost_class = {
    .type = "dpdkvhostuser",
    NETDEV_DPDK_CLASS_COMMON,
    /* ...... */
    .send = netdev_dpdk_vhost_send,
    /* ...... */
    .rxq_recv = netdev_dpdk_vhost_rxq_recv, // 从VM接收报文
    /* ...... */
};

static const struct netdev_class dpdk_vhost_client_class = {
    .type = "dpdkvhostuserclient",
    NETDEV_DPDK_CLASS_COMMON,
    /* ...... */
    .send = netdev_dpdk_vhost_send,
    /* ...... */
    .rxq_recv = netdev_dpdk_vhost_rxq_recv, // 从VM接收报文
    /* ...... */
};
```

## 3. PMD创建流程

### 3.1 以 add port 为例

```c
// lib/dpif-netdev.c
static int
dpif_netdev_port_add(struct dpif *dpif, struct netdev *netdev,
                     odp_port_t *port_nop)
{

    /* ...... */

        error = do_add_port(dp, dpif_port, netdev_get_type(netdev), port_no);
    
    /* ...... */

    return error;
}
```

### 3.2 创建 PMD 线程并轮询

```c
// do_add_port -> reconfigure_datapath() -> reconfigure_pmd_threads() -> ovs_thread_create(ds_cstr(&name),pmd_thread_main, pmd)

static void *
pmd_thread_main(void *f_)
{

    /* ...... */

    for (;;) { // PMD轮询
        
        /* ...... */

                dp_netdev_process_rxq_port(pmd, poll_list[i].rxq,poll_list[i].port_no);
        }

        if (!rx_packets) {

            /* ...... */

            tx_packets = dp_netdev_pmd_flush_output_packets(pmd, false);
        }

        /* ...... */

    }

    /* ...... */

    return NULL;
}
```

#### 3.2.1 `dp_netdev_process_rxq_port` 函数

```c
static int
dp_netdev_process_rxq_port(struct dp_netdev_pmd_thread *pmd,
                           struct dp_netdev_rxq *rxq,
                           odp_port_t port_no)
{

    /* ...... */

    error = netdev_rxq_recv(rxq->rx, &batch, qlen_p); // 接收报文，通过rx->netdev->netdev_class->rxq_recv()调用注册时绑定的回调函数
    if (!error) {
        
        /* ...... */

        /* 处理接收到的报文(batch) */
            dp_netdev_input(pmd, &batch, port_no); // 处理报文，进行三级流表匹配

        /* ...... */

        dp_netdev_pmd_flush_output_packets(pmd, false); // 发送报文

        /* ...... */

    }

    /* ...... */

    return batch_cnt;
}
```

#### 3.2.2 `dp_netdev_input` 三级流表匹配 + 执行 `action`

```c
dp_netdev_input() -> dp_netdev_input__()
```

```c
static void
dp_netdev_input__(struct dp_netdev_pmd_thread *pmd,
                  struct dp_packet_batch *packets,
                  bool md_is_valid, odp_port_t port_no)
{

    /* ...... */

    dfc_processing(pmd, packets, keys, missed_keys, batches, &n_batches, // EMC 精确匹配

    /* ...... */

        fast_path_processing(pmd, packets, missed_keys, // dpcls + ofcls 二级datapath分类器 + 三级openflow分类器
    
    /* ...... */

    for (i = 0; i < n_batches; i++) {
        packet_batch_per_flow_execute(&batches[i], pmd); // 执行流表action
    }
}
```

#### 3.2.3 `packet_batch_per_flow_execute` 执行流表 `action`

```c
packet_batch_per_flow_execute() -> dp_netdev_execute_actions() -> odp_execute_actions() -> dp_execute_cb()

// lib/dpif-netdev.c
dp_execute_cb() // 函数主体，各种action的匹配执行
```

#### 3.2.4`dp_netdev_pmd_flush_output_packets` 发包过程

```c
dp_netdev_pmd_flush_output_packets() -> dp_netdev_pmd_flush_output_on_port() -> netdev_send() -> netdev->netdev_class->send()
```
















