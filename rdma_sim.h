#ifndef RDMA_SIM_H
#define RDMA_SIM_H

#include <stdint.h>
#include <stddef.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

// 控制消息类型（控制面走 Send/Recv；数据面走 RDMA Write）
// 设计目的：把“连接握手 + MR 交换 + 完成通知”与“数据写入”分离，贴近真实 RDMA 编程模型
// - HELLO：发送端告诉接收端文件名/大小（让接收端准备 MR）
// - MR：接收端回传 MR 的 addr/rkey/length（发送端据此做 RDMA Write）
// - FIN：发送端通知“写入结束”
// - ACK：接收端通知“落盘完成”
typedef enum {
    RDMA_CTRL_HELLO = 1,
    RDMA_CTRL_MR    = 2,
    RDMA_CTRL_FIN   = 3,
    RDMA_CTRL_ACK   = 4
} rdma_ctrl_type_t;

// 文件名最大长度（只传文件名，不传路径）
#define RDMA_MAX_NAME 256

// 单次 RDMA Write 的最大块大小
// 目的：避免单次 WR 过大导致资源不足，模拟真实系统中需要分块的情况
#define RDMA_CHUNK (64 * 1024)

// HELLO 控制消息：发送端 -> 接收端
// 说明：接收端收到后根据 file_size 分配接收缓冲区，并注册 MR
// 注意：所有字段在网络中传输时都按网络字节序
// - type：消息类型
// - name_len：文件名长度
// - file_size：文件大小
// - name：文件名（固定数组，实际长度用 name_len）
typedef struct {
    uint32_t type;
    uint32_t name_len;
    uint64_t file_size;
    char name[RDMA_MAX_NAME];
} rdma_ctrl_hello_t;

// MR 信息控制消息：接收端 -> 发送端
// 说明：发送端据此执行 RDMA Write
// - addr：接收端 MR 的虚拟地址（对 RDMA 可见）
// - rkey：接收端 MR 的 rkey（远端访问凭证）
// - length：接收端 MR 长度
// 注意：addr/rkey/length 全部按网络字节序
// 注意：addr 为接收端虚拟地址，需要配合 rkey 才能访问
// 在真实系统里这就是“单边写”的关键条件
typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t addr;
    uint32_t rkey;
    uint64_t length;
} rdma_ctrl_mr_t;

// FIN/ACK 控制消息（仅表示状态，没有额外负载）
typedef struct {
    uint32_t type;
    uint32_t reserved;
} rdma_ctrl_simple_t;

// 等待指定类型的 RDMA CM 事件
// 作用：简化事件处理，避免调用方重复写“get + check + ack”逻辑
// 成功返回 0，失败返回 -1
int rdma_wait_event(struct rdma_event_channel *ec, enum rdma_cm_event_type expect, struct rdma_cm_id **out_id);

// 创建 PD/CQ/QP（RC）
// 说明：
// - PD：保护域，用于资源隔离
// - CQ：完成队列，发送/接收/写完成都在这里回报
// - QP：RC 可靠连接队列对，用于 RDMA 操作
int rdma_build_qp(struct rdma_cm_id *id, struct ibv_pd **pd, struct ibv_cq **cq, struct ibv_comp_channel **comp_chan);

// 注册 MR
// access 用于指定访问权限，例如：
// - IBV_ACCESS_LOCAL_WRITE
// - IBV_ACCESS_REMOTE_WRITE
// 注册成功后 out_mr 内含 lkey/rkey 等关键信息
int rdma_register_mr(struct ibv_pd *pd, void *buf, size_t len, int access, struct ibv_mr **out_mr);

// Post Recv：投递接收缓冲区
// RDMA 的 Send/Recv 是“对称操作”，必须先 post_recv 才能接收对端 send
int rdma_post_recv(struct rdma_cm_id *id, void *buf, size_t len, struct ibv_mr *mr, uint64_t wr_id);

// Post Send：发送控制消息（HELLO/MR/FIN/ACK）
int rdma_post_send(struct rdma_cm_id *id, void *buf, size_t len, struct ibv_mr *mr, uint64_t wr_id);

// Post RDMA Write：单边写入对端 MR
// 注意：remote_addr/rkey 来自对端 MR 信息
int rdma_post_write(struct rdma_cm_id *id, void *buf, size_t len, struct ibv_mr *mr,
                    uint64_t remote_addr, uint32_t rkey, uint64_t wr_id);

// 轮询 CQ 等待完成事件
// expect 可指定 IBV_WC_SEND / IBV_WC_RECV / IBV_WC_RDMA_WRITE
// 返回 0 表示等到期望完成；返回 -1 表示失败
int rdma_poll_cq(struct ibv_cq *cq, enum ibv_wc_opcode expect, uint64_t *out_wr_id);

#endif // RDMA_SIM_H
