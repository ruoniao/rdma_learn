#include "rdma_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <libgen.h>

#include <unistd.h>
#include <arpa/inet.h>
#include <endian.h>

// 读取文件到内存
// 目的：一次性读入文件内容，后续做 RDMA Write
// 注意：真实场景可做零拷贝或分块读，这里为了教学简单化
static int read_file(const char *path, uint8_t **out_buf, size_t *out_len, char *out_name, size_t name_cap) {
    FILE *fp = fopen(path, "rb");                         // 以二进制方式打开文件
    if (!fp) {
        perror("fopen");                                  // 打印错误
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {                      // 移动到文件末尾
        perror("fseek");
        fclose(fp);
        return -1;
    }
    long fsize = ftell(fp);                                 // 获取文件大小
    if (fsize < 0) {
        perror("ftell");
        fclose(fp);
        return -1;
    }
    rewind(fp);                                             // 回到文件开头

    uint8_t *buf = (uint8_t *)malloc((size_t)fsize);        // 分配缓冲区
    if (!buf) {
        fprintf(stderr, "malloc failed\n");
        fclose(fp);
        return -1;
    }
    size_t n = fread(buf, 1, (size_t)fsize, fp);            // 读取文件
    fclose(fp);
    if (n != (size_t)fsize) {
        fprintf(stderr, "fread failed\n");
        free(buf);
        return -1;
    }

    // 只传文件名，不传路径（避免路径注入 & 便于接收端落盘）
    char *path_copy = strdup(path);                         // 复制路径以获取文件名
    if (!path_copy) {
        free(buf);
        return -1;
    }
    char *name = basename(path_copy);                       // 获取文件名
    if (strlen(name) >= name_cap) {
        fprintf(stderr, "file name too long\n");
        free(path_copy);
        free(buf);
        return -1;
    }
    strncpy(out_name, name, name_cap);
    out_name[name_cap - 1] = '\0';                         // 保险截断
    free(path_copy);

    *out_buf = buf;
    *out_len = (size_t)fsize;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <receiver_ip> <port> <file_path>\n", argv[0]);
        return 1;
    }
    const char *server_ip = argv[1];                        // 接收端 IP
    const char *port = argv[2];                             // 接收端端口
    const char *file_path = argv[3];                        // 待发送文件路径

    uint8_t *file_buf = NULL;                               // 文件内容缓冲区
    size_t file_len = 0;                                    // 文件长度
    char file_name[RDMA_MAX_NAME];                          // 文件名
    if (read_file(file_path, &file_buf, &file_len, file_name, sizeof(file_name)) != 0) {
        return 1;                                           // 读文件失败
    }

    // 1) 地址解析：把 ip+port 解析为 RDMA CM 地址
    struct rdma_addrinfo hints;                             // 地址解析提示
    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;                      // TCP 语义的 RDMA CM

    struct rdma_addrinfo *res = NULL;                       // 解析结果
    if (rdma_getaddrinfo((char *)server_ip, (char *)port, &hints, &res) != 0) {
        perror("rdma_getaddrinfo");
        free(file_buf);
        return 1;
    }

    // 2) 创建事件通道 + CM ID
    struct rdma_event_channel *ec = rdma_create_event_channel();
    if (!ec) {
        perror("rdma_create_event_channel");
        rdma_freeaddrinfo(res);
        free(file_buf);
        return 1;
    }

    struct rdma_cm_id *id = NULL;
    if (rdma_create_id(ec, &id, NULL, RDMA_PS_TCP) != 0) {
        perror("rdma_create_id");
        rdma_freeaddrinfo(res);
        rdma_destroy_event_channel(ec);
        free(file_buf);
        return 1;
    }

    // 3) 解析地址与路由（RDMA CM 必需步骤）
    if (rdma_resolve_addr(id, NULL, res->ai_dst_addr, 2000) != 0) {
        perror("rdma_resolve_addr");
        rdma_freeaddrinfo(res);
        rdma_destroy_id(id);
        rdma_destroy_event_channel(ec);
        free(file_buf);
        return 1;
    }
    if (rdma_wait_event(ec, RDMA_CM_EVENT_ADDR_RESOLVED, NULL) != 0) {
        fprintf(stderr, "ADDR_RESOLVED failed\n");
        rdma_freeaddrinfo(res);
        rdma_destroy_id(id);
        rdma_destroy_event_channel(ec);
        free(file_buf);
        return 1;
    }
    if (rdma_resolve_route(id, 2000) != 0) {
        perror("rdma_resolve_route");
        rdma_freeaddrinfo(res);
        rdma_destroy_id(id);
        rdma_destroy_event_channel(ec);
        free(file_buf);
        return 1;
    }
    if (rdma_wait_event(ec, RDMA_CM_EVENT_ROUTE_RESOLVED, NULL) != 0) {
        fprintf(stderr, "ROUTE_RESOLVED failed\n");
        rdma_freeaddrinfo(res);
        rdma_destroy_id(id);
        rdma_destroy_event_channel(ec);
        free(file_buf);
        return 1;
    }
    rdma_freeaddrinfo(res);

    // 4) 创建 QP/CQ/PD（通信与完成机制）
    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_comp_channel *comp_chan = NULL;
    if (rdma_build_qp(id, &pd, &cq, &comp_chan) != 0) {
        fprintf(stderr, "rdma_build_qp failed\n");
        rdma_destroy_id(id);
        rdma_destroy_event_channel(ec);
        free(file_buf);
        return 1;
    }

    // 5) 注册 MR
    // - file_mr：文件内容，作为 RDMA Write 的本地源
    // - ctrl_mr：HELLO / MR_INFO / ACK 等控制消息
    struct ibv_mr *file_mr = NULL;
    if (rdma_register_mr(pd, file_buf, file_len, IBV_ACCESS_LOCAL_WRITE, &file_mr) != 0) {
        fprintf(stderr, "register file MR failed\n");
        rdma_destroy_id(id);
        rdma_destroy_event_channel(ec);
        free(file_buf);
        return 1;
    }

    rdma_ctrl_hello_t hello;                                 // HELLO 消息
    memset(&hello, 0, sizeof(hello));
    hello.type = htonl(RDMA_CTRL_HELLO);
    hello.name_len = htonl((uint32_t)strlen(file_name));
    hello.file_size = htobe64((uint64_t)file_len);
    strncpy(hello.name, file_name, RDMA_MAX_NAME - 1);

    rdma_ctrl_mr_t mr_info;                                  // 接收端 MR 信息（接收用）
    memset(&mr_info, 0, sizeof(mr_info));

    rdma_ctrl_simple_t ack;                                  // ACK（接收用）
    memset(&ack, 0, sizeof(ack));

    struct ibv_mr *hello_mr = NULL;
    struct ibv_mr *mr_info_mr = NULL;
    struct ibv_mr *ack_mr = NULL;

    if (rdma_register_mr(pd, &hello, sizeof(hello), IBV_ACCESS_LOCAL_WRITE, &hello_mr) != 0 ||
        rdma_register_mr(pd, &mr_info, sizeof(mr_info), IBV_ACCESS_LOCAL_WRITE, &mr_info_mr) != 0 ||
        rdma_register_mr(pd, &ack, sizeof(ack), IBV_ACCESS_LOCAL_WRITE, &ack_mr) != 0) {
        fprintf(stderr, "register ctrl MR failed\n");
        rdma_destroy_id(id);
        rdma_destroy_event_channel(ec);
        free(file_buf);
        return 1;
    }

    // 6) 预投递接收 MR_INFO
    // 关键点：Send/Recv 必须先 post_recv，否则对端 send 可能失败
    if (rdma_post_recv(id, &mr_info, sizeof(mr_info), mr_info_mr, 1) != 0) {
        fprintf(stderr, "post recv MR_INFO failed\n");
        return 1;
    }

    // 7) 建立连接
    struct rdma_conn_param conn_param;
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 1;
    conn_param.responder_resources = 1;
    conn_param.retry_count = 7;
    if (rdma_connect(id, &conn_param) != 0) {
        perror("rdma_connect");
        return 1;
    }
    if (rdma_wait_event(ec, RDMA_CM_EVENT_ESTABLISHED, NULL) != 0) {
        fprintf(stderr, "ESTABLISHED failed\n");
        return 1;
    }

    // 8) 发送 HELLO（让接收端准备 MR）
    if (rdma_post_send(id, &hello, sizeof(hello), hello_mr, 2) != 0) {
        fprintf(stderr, "post send HELLO failed\n");
        return 1;
    }
    if (rdma_poll_cq(cq, IBV_WC_SEND, NULL) != 0) {
        fprintf(stderr, "HELLO send completion failed\n");
        return 1;
    }

    // 9) 接收 MR_INFO（拿到远端 addr/rkey）
    if (rdma_poll_cq(cq, IBV_WC_RECV, NULL) != 0) {
        fprintf(stderr, "MR_INFO recv completion failed\n");
        return 1;
    }

    if (ntohl(mr_info.type) != RDMA_CTRL_MR) {
        fprintf(stderr, "invalid MR_INFO type\n");
        return 1;
    }
    uint64_t remote_addr = be64toh(mr_info.addr);
    uint32_t remote_rkey = ntohl(mr_info.rkey);
    uint64_t remote_len = be64toh(mr_info.length);
    if (file_len > remote_len) {
        fprintf(stderr, "remote MR too small\n");
        return 1;
    }

    // 10) 分块 RDMA Write
    // 关键点：RDMA Write 是单边操作，接收端不会触发 recv
    // 完成事件仍会在发送端 CQ 中出现，必须等待完成
    uint64_t offset = 0;
    while (offset < (uint64_t)file_len) {
        uint32_t chunk = RDMA_CHUNK;
        if (offset + chunk > (uint64_t)file_len) {
            chunk = (uint32_t)((uint64_t)file_len - offset);
        }
        if (rdma_post_write(id, file_buf + offset, chunk, file_mr,
                            remote_addr + offset, remote_rkey, 3) != 0) {
            fprintf(stderr, "post RDMA write failed\n");
            return 1;
        }
        if (rdma_poll_cq(cq, IBV_WC_RDMA_WRITE, NULL) != 0) {
            fprintf(stderr, "RDMA write completion failed\n");
            return 1;
        }
        offset += chunk;
    }

    // 11) 发送 FIN，并等待 ACK
    // 目的：让接收端知道“数据写完了，可以落盘”
    if (rdma_post_recv(id, &ack, sizeof(ack), ack_mr, 4) != 0) {
        fprintf(stderr, "post recv ACK failed\n");
        return 1;
    }

    rdma_ctrl_simple_t fin;
    memset(&fin, 0, sizeof(fin));
    fin.type = htonl(RDMA_CTRL_FIN);
    struct ibv_mr *fin_mr = NULL;
    if (rdma_register_mr(pd, &fin, sizeof(fin), IBV_ACCESS_LOCAL_WRITE, &fin_mr) != 0) {
        fprintf(stderr, "register FIN MR failed\n");
        return 1;
    }
    if (rdma_post_send(id, &fin, sizeof(fin), fin_mr, 5) != 0) {
        fprintf(stderr, "post send FIN failed\n");
        return 1;
    }
    if (rdma_poll_cq(cq, IBV_WC_SEND, NULL) != 0) {
        fprintf(stderr, "FIN send completion failed\n");
        return 1;
    }

    if (rdma_poll_cq(cq, IBV_WC_RECV, NULL) != 0) {
        fprintf(stderr, "ACK recv completion failed\n");
        return 1;
    }
    if (ntohl(ack.type) != RDMA_CTRL_ACK) {
        fprintf(stderr, "invalid ACK type\n");
        return 1;
    }

    // 12) 资源释放
    rdma_disconnect(id);
    rdma_destroy_qp(id);
    rdma_destroy_id(id);
    rdma_destroy_event_channel(ec);
    free(file_buf);

    printf("[sender] done\n");
    return 0;
}
