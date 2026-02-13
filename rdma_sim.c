#include "rdma_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

// 等待并校验指定类型的 RDMA CM 事件
// 关键点：
// - rdma_get_cm_event 会阻塞，直到事件到达
// - 每个事件必须调用 rdma_ack_cm_event 进行确认
// - 如果事件类型不匹配，直接报错返回
int rdma_wait_event(struct rdma_event_channel *ec, enum rdma_cm_event_type expect, struct rdma_cm_id **out_id) {
    struct rdma_cm_event *event = NULL;                 // 事件指针
    if (rdma_get_cm_event(ec, &event) != 0) {           // 阻塞等待事件
        return -1;                                      // 获取事件失败
    }
    int ok = (event->event == expect);                  // 判断事件类型是否匹配
    if (ok && out_id) {                                 // 需要返回 cm_id
        *out_id = event->id;                            // 返回事件里的 id
    }
    rdma_ack_cm_event(event);                           // 事件必须确认
    return ok ? 0 : -1;                                 // 返回结果
}

// 创建 PD/CQ/QP（RC）
// 这个函数完成“QP 能工作”的最小资源准备：
// 1) 创建 PD 作为资源归属
// 2) 创建 CQ 用于完成通知
// 3) 创建 QP（RC 类型）
int rdma_build_qp(struct rdma_cm_id *id, struct ibv_pd **pd, struct ibv_cq **cq, struct ibv_comp_channel **comp_chan) {
    *pd = ibv_alloc_pd(id->verbs);                      // 分配 PD
    if (!*pd) {
        return -1;
    }

    *comp_chan = ibv_create_comp_channel(id->verbs);    // 创建完成通道（此 demo 不强依赖）
    if (!*comp_chan) {
        return -1;
    }

    *cq = ibv_create_cq(id->verbs, 16, NULL, *comp_chan, 0); // CQ 深度 16
    if (!*cq) {
        return -1;
    }

    if (ibv_req_notify_cq(*cq, 0) != 0) {               // 使能 CQ 通知（可选）
        return -1;
    }

    struct ibv_qp_init_attr qp_attr;                    // QP 初始化参数
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = *cq;                              // 发送 CQ
    qp_attr.recv_cq = *cq;                              // 接收 CQ
    qp_attr.qp_type = IBV_QPT_RC;                       // 可靠连接（RC）
    qp_attr.cap.max_send_wr = 16;                       // 发送 WR 深度
    qp_attr.cap.max_recv_wr = 16;                       // 接收 WR 深度
    qp_attr.cap.max_send_sge = 1;                       // 发送 SGE 数量
    qp_attr.cap.max_recv_sge = 1;                       // 接收 SGE 数量

    if (rdma_create_qp(id, *pd, &qp_attr) != 0) {        // 创建 QP
        return -1;
    }
    return 0;
}

// 注册 MR
// - buf：待注册的内存
// - len：长度
// - access：访问权限
// 注册成功后 out_mr->lkey/rkey 可用于本地/远端访问
int rdma_register_mr(struct ibv_pd *pd, void *buf, size_t len, int access, struct ibv_mr **out_mr) {
    *out_mr = ibv_reg_mr(pd, buf, len, access);
    if (!*out_mr) {
        return -1;
    }
    return 0;
}

// Post Recv
// 发送端/接收端要接收控制消息时，必须提前 post_recv
// 这是 RDMA Send/Recv 的关键规则：先挂接收，再发数据
int rdma_post_recv(struct rdma_cm_id *id, void *buf, size_t len, struct ibv_mr *mr, uint64_t wr_id) {
    struct ibv_sge sge;                                  // SGE 描述
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)buf;                           // 缓冲区地址
    sge.length = (uint32_t)len;                          // 缓冲区长度
    sge.lkey = mr->lkey;                                 // 本地 key

    struct ibv_recv_wr wr;                               // 接收 WR
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_id;                                    // WR 标识
    wr.sg_list = &sge;                                   // SGE 列表
    wr.num_sge = 1;                                      // SGE 数量

    struct ibv_recv_wr *bad = NULL;                      // 错误 WR 指针
    if (ibv_post_recv(id->qp, &wr, &bad) != 0) {
        return -1;
    }
    return 0;
}

// Post Send
// 用于发送控制消息（HELLO/MR/FIN/ACK）
// 这些消息都走双边 Send/Recv，确保对端已准备接收
int rdma_post_send(struct rdma_cm_id *id, void *buf, size_t len, struct ibv_mr *mr, uint64_t wr_id) {
    struct ibv_sge sge;                                  // SGE 描述
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)buf;
    sge.length = (uint32_t)len;
    sge.lkey = mr->lkey;

    struct ibv_send_wr wr;                               // 发送 WR
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;                             // Send 操作
    wr.send_flags = IBV_SEND_SIGNALED;                   // 请求完成事件

    struct ibv_send_wr *bad = NULL;
    if (ibv_post_send(id->qp, &wr, &bad) != 0) {
        return -1;
    }
    return 0;
}

// Post RDMA Write
// 数据面：单边写入对端 MR
// 注意：remote_addr/rkey 来自对端 MR 信息
int rdma_post_write(struct rdma_cm_id *id, void *buf, size_t len, struct ibv_mr *mr,
                    uint64_t remote_addr, uint32_t rkey, uint64_t wr_id) {
    struct ibv_sge sge;                                  // SGE 描述
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)buf;                           // 本地缓冲区地址
    sge.length = (uint32_t)len;                          // 写入长度
    sge.lkey = mr->lkey;                                 // 本地 key

    struct ibv_send_wr wr;                               // 发送 WR
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;                       // RDMA Write 操作
    wr.send_flags = IBV_SEND_SIGNALED;                   // 请求完成事件
    wr.wr.rdma.remote_addr = remote_addr;                // 对端地址
    wr.wr.rdma.rkey = rkey;                              // 对端 rkey

    struct ibv_send_wr *bad = NULL;
    if (ibv_post_send(id->qp, &wr, &bad) != 0) {
        return -1;
    }
    return 0;
}

// 轮询 CQ 等待完成事件
// 说明：此 demo 使用主动轮询（polling）方式，逻辑简单但会消耗 CPU
// 在真实系统中可用 comp_channel + eventfd 机制减少 CPU 占用
int rdma_poll_cq(struct ibv_cq *cq, enum ibv_wc_opcode expect, uint64_t *out_wr_id) {
    struct ibv_wc wc;                                    // 完成队列条目
    while (1) {
        int n = ibv_poll_cq(cq, 1, &wc);                  // 取 1 个完成
        if (n < 0) {
            return -1;                                   // 轮询出错
        }
        if (n == 0) {
            usleep(1000);                                // 小睡，避免空转
            continue;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            return -1;                                   // 完成状态异常
        }

        // 如果调用方不关心类型，直接返回
        if (expect != IBV_WC_SEND && expect != IBV_WC_RECV && expect != IBV_WC_RDMA_WRITE) {
            if (out_wr_id) {
                *out_wr_id = wc.wr_id;
            }
            return 0;
        }

        // 匹配期望类型
        if (wc.opcode == expect) {
            if (out_wr_id) {
                *out_wr_id = wc.wr_id;
            }
            return 0;
        }
        // 如果不是期望类型，继续轮询（可能有其他完成事件）
    }
}
