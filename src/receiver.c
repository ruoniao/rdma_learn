#include "rdma_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <unistd.h>
#include <arpa/inet.h>
#include <endian.h>

// 组合输出路径
// 说明：接收端只保存文件名，不接收路径，避免覆盖系统路径
static int build_out_path(const char *dir, const char *name, char *out_path, size_t cap) {
    if (snprintf(out_path, cap, "%s/%s", dir, name) >= (int)cap) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <listen_ip> <port> <output_dir>\n", argv[0]);
        return 1;
    }
    const char *listen_ip = argv[1];                                // 监听 IP
    const char *port = argv[2];                                     // 监听端口
    const char *out_dir = argv[3];                                  // 输出目录

    // 1) 解析监听地址并创建监听 CM ID
    // 说明：listen 端必须先 bind + listen，等待对端 connect
    struct rdma_addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = RAI_PASSIVE;                                   // 被动监听
    hints.ai_port_space = RDMA_PS_TCP;

    struct rdma_addrinfo *res = NULL;
    if (rdma_getaddrinfo((char *)listen_ip, (char *)port, &hints, &res) != 0) {
        perror("rdma_getaddrinfo");
        return 1;
    }

    struct rdma_event_channel *ec = rdma_create_event_channel();
    if (!ec) {
        perror("rdma_create_event_channel");
        rdma_freeaddrinfo(res);
        return 1;
    }

    struct rdma_cm_id *listen_id = NULL;
    if (rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP) != 0) {
        perror("rdma_create_id");
        rdma_freeaddrinfo(res);
        rdma_destroy_event_channel(ec);
        return 1;
    }

    if (rdma_bind_addr(listen_id, res->ai_src_addr) != 0) {
        perror("rdma_bind_addr");
        rdma_freeaddrinfo(res);
        rdma_destroy_id(listen_id);
        rdma_destroy_event_channel(ec);
        return 1;
    }
    rdma_freeaddrinfo(res);

    if (rdma_listen(listen_id, 1) != 0) {
        perror("rdma_listen");
        rdma_destroy_id(listen_id);
        rdma_destroy_event_channel(ec);
        return 1;
    }
    printf("[receiver] listening on %s:%s\n", listen_ip, port);

    // 2) 等待连接请求（CM 事件）
    struct rdma_cm_id *id = NULL;
    if (rdma_wait_event(ec, RDMA_CM_EVENT_CONNECT_REQUEST, &id) != 0) {
        fprintf(stderr, "CONNECT_REQUEST failed\n");
        rdma_destroy_id(listen_id);
        rdma_destroy_event_channel(ec);
        return 1;
    }

    // 3) 创建 QP/CQ/PD
    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_comp_channel *comp_chan = NULL;
    if (rdma_build_qp(id, &pd, &cq, &comp_chan) != 0) {
        fprintf(stderr, "rdma_build_qp failed\n");
        rdma_destroy_id(id);
        rdma_destroy_id(listen_id);
        rdma_destroy_event_channel(ec);
        return 1;
    }

    // 4) 预投递 HELLO 接收
    // 说明：控制面走 Send/Recv，必须先 post_recv
    rdma_ctrl_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    struct ibv_mr *hello_mr = NULL;
    if (rdma_register_mr(pd, &hello, sizeof(hello), IBV_ACCESS_LOCAL_WRITE, &hello_mr) != 0) {
        fprintf(stderr, "register HELLO MR failed\n");
        return 1;
    }
    if (rdma_post_recv(id, &hello, sizeof(hello), hello_mr, 1) != 0) {
        fprintf(stderr, "post recv HELLO failed\n");
        return 1;
    }

    // 5) 接受连接
    struct rdma_conn_param conn_param;
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 1;
    conn_param.responder_resources = 1;
    conn_param.rnr_retry_count = 7;
    if (rdma_accept(id, &conn_param) != 0) {
        perror("rdma_accept");
        return 1;
    }
    if (rdma_wait_event(ec, RDMA_CM_EVENT_ESTABLISHED, NULL) != 0) {
        fprintf(stderr, "ESTABLISHED failed\n");
        return 1;
    }

    // 6) 等待 HELLO 到达
    if (rdma_poll_cq(cq, IBV_WC_RECV, NULL) != 0) {
        fprintf(stderr, "HELLO recv completion failed\n");
        return 1;
    }

    if (ntohl(hello.type) != RDMA_CTRL_HELLO) {
        fprintf(stderr, "invalid HELLO type\n");
        return 1;
    }

    uint32_t name_len = ntohl(hello.name_len);
    uint64_t file_size = be64toh(hello.file_size);
    if (name_len == 0 || name_len >= RDMA_MAX_NAME) {
        fprintf(stderr, "invalid file name length\n");
        return 1;
    }
    hello.name[RDMA_MAX_NAME - 1] = '\0';
    printf("[receiver] incoming file: %s (%llu bytes)\n",
           hello.name, (unsigned long long)file_size);

    // 7) 为文件数据分配 MR
    // 关键点：必须给远端写权限（IBV_ACCESS_REMOTE_WRITE）
    uint8_t *file_buf = (uint8_t *)malloc((size_t)file_size);
    if (!file_buf) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    struct ibv_mr *file_mr = NULL;
    if (rdma_register_mr(pd, file_buf, (size_t)file_size,
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE, &file_mr) != 0) {
        fprintf(stderr, "register file MR failed\n");
        free(file_buf);
        return 1;
    }

    // 8) 发送 MR 信息给发送端
    rdma_ctrl_mr_t mr_info;
    memset(&mr_info, 0, sizeof(mr_info));
    mr_info.type = htonl(RDMA_CTRL_MR);
    mr_info.addr = htobe64((uint64_t)(uintptr_t)file_buf);
    mr_info.rkey = htonl(file_mr->rkey);
    mr_info.length = htobe64((uint64_t)file_size);

    struct ibv_mr *mr_info_mr = NULL;
    if (rdma_register_mr(pd, &mr_info, sizeof(mr_info), IBV_ACCESS_LOCAL_WRITE, &mr_info_mr) != 0) {
        fprintf(stderr, "register MR_INFO MR failed\n");
        free(file_buf);
        return 1;
    }
    if (rdma_post_send(id, &mr_info, sizeof(mr_info), mr_info_mr, 2) != 0) {
        fprintf(stderr, "post send MR_INFO failed\n");
        free(file_buf);
        return 1;
    }
    if (rdma_poll_cq(cq, IBV_WC_SEND, NULL) != 0) {
        fprintf(stderr, "MR_INFO send completion failed\n");
        free(file_buf);
        return 1;
    }

    // 9) 预投递 FIN 接收
    // 说明：写入完成后，发送端会发送 FIN 通知
    rdma_ctrl_simple_t fin;
    memset(&fin, 0, sizeof(fin));
    struct ibv_mr *fin_mr = NULL;
    if (rdma_register_mr(pd, &fin, sizeof(fin), IBV_ACCESS_LOCAL_WRITE, &fin_mr) != 0) {
        fprintf(stderr, "register FIN MR failed\n");
        free(file_buf);
        return 1;
    }
    if (rdma_post_recv(id, &fin, sizeof(fin), fin_mr, 3) != 0) {
        fprintf(stderr, "post recv FIN failed\n");
        free(file_buf);
        return 1;
    }

    // 10) 等待 FIN 到达
    if (rdma_poll_cq(cq, IBV_WC_RECV, NULL) != 0) {
        fprintf(stderr, "FIN recv completion failed\n");
        free(file_buf);
        return 1;
    }
    if (ntohl(fin.type) != RDMA_CTRL_FIN) {
        fprintf(stderr, "invalid FIN type\n");
        free(file_buf);
        return 1;
    }

    // 11) 落盘保存
    char out_path[1024];
    if (build_out_path(out_dir, hello.name, out_path, sizeof(out_path)) != 0) {
        fprintf(stderr, "output path too long\n");
        free(file_buf);
        return 1;
    }
    FILE *fp = fopen(out_path, "wb");
    if (!fp) {
        perror("fopen");
        free(file_buf);
        return 1;
    }
    size_t wn = fwrite(file_buf, 1, (size_t)file_size, fp);
    fclose(fp);
    if (wn != (size_t)file_size) {
        fprintf(stderr, "fwrite failed\n");
        free(file_buf);
        return 1;
    }
    printf("[receiver] saved to %s\n", out_path);

    // 12) 发送 ACK
    rdma_ctrl_simple_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.type = htonl(RDMA_CTRL_ACK);
    struct ibv_mr *ack_mr = NULL;
    if (rdma_register_mr(pd, &ack, sizeof(ack), IBV_ACCESS_LOCAL_WRITE, &ack_mr) != 0) {
        fprintf(stderr, "register ACK MR failed\n");
        free(file_buf);
        return 1;
    }
    if (rdma_post_send(id, &ack, sizeof(ack), ack_mr, 4) != 0) {
        fprintf(stderr, "post send ACK failed\n");
        free(file_buf);
        return 1;
    }
    if (rdma_poll_cq(cq, IBV_WC_SEND, NULL) != 0) {
        fprintf(stderr, "ACK send completion failed\n");
        free(file_buf);
        return 1;
    }

    // 13) 断开连接并清理资源
    rdma_disconnect(id);
    rdma_destroy_qp(id);
    rdma_destroy_id(id);
    rdma_destroy_id(listen_id);
    rdma_destroy_event_channel(ec);
    free(file_buf);

    printf("[receiver] done\n");
    return 0;
}
