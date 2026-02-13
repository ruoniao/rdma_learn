# 真实 RDMA（rdma-core + Soft‑RoCE）文件传输 Demo

## 原理讲解（面向小白但有整体架构）
先给你一张“脑中结构图”的文字版：

**RDMA 系统分三层：**
1. **控制面（Control Plane）**：负责建立连接、交换元信息。典型是 `rdma_cm`（librdmacm）。
2. **数据面（Data Plane）**：真正的数据收发与 RDMA 操作。典型是 `ibverbs`（libibverbs）。
3. **硬件/驱动层**：RNIC 或 Soft‑RoCE（RXE）提供 RDMA 能力。

这套 demo 做的事情是：
- 控制面用 **Send/Recv** 交换 HELLO / MR / FIN / ACK。
- 数据面用 **RDMA Write** 把文件内容写到接收端内存。

### 关键概念快速建立
- **PD（Protection Domain）**：资源容器。所有 QP/CQ/MR 都要属于某个 PD。
- **CQ（Completion Queue）**：完成队列。Send/Recv/RDMA Write 完成后会有 WC 事件。
- **QP（Queue Pair）**：通信端点。RC 类型用于可靠连接。
- **MR（Memory Region）**：注册内存。只有注册过的内存才能被 RDMA 读写。
- **rkey**：远端访问“钥匙”。对端用 rkey 才能访问你的 MR。
- **Send/Recv**：双边操作，必须先 post_recv。
- **RDMA Write**：单边操作，只要有对端 MR 的 addr+rkey 即可写。

### 这个 demo 的完整流程（架构视角）
1. **CM 建连**
   - 发送端解析地址、路由、建立连接。
   - 接收端监听并接受连接。
2. **MR 交换（控制面）**
   - 发送端发 HELLO（文件名 + 文件大小）。
   - 接收端分配缓冲区、注册 MR，回传 MR 信息（addr/rkey/length）。
3. **RDMA Write（数据面）**
   - 发送端分块写入接收端 MR。
4. **完成通知**
   - 发送端发 FIN，接收端落盘并回 ACK。

## 环境安装
两台节点都执行：

```bash
./install_deps.sh all
```

## 启用 Soft‑RoCE
两台节点都执行（网卡名替换为你的实际网卡，如 `ens33`）：

```bash
./setup_rxe.sh ens33
```

确认 rxe 设备：

```bash
rdma link show
```

## 代码结构
- `rdma_sim.h`：RDMA 控制消息定义 + verbs 封装
- `rdma_sim.c`：RDMA CM 事件处理 + verbs 操作封装
- `sender.c`：发送端（读文件、注册 MR、RDMA Write、FIN）
- `receiver.c`：接收端（监听、注册 MR、落盘、ACK）

## 构建

```bash
./build.sh
```

## 运行
顺序必须是 **先接收端**，再发送端：

1. node2 启动接收端
```bash
./run_receiver.sh 192.168.153.131 18500 /app/source/rdma-recv
```

2. node1 发送文件
```bash
./run_sender.sh 192.168.153.131 18500 /app/source/rdma-learn/test.txt
```

## 测试
1. node1 创建测试文件：
```bash
echo "hello rdma real" > /app/source/rdma-learn/test.txt
```

2. node2 启动接收端
3. node1 发送文件
4. node2 验证：
```bash
cat /app/source/rdma-recv/test.txt
```

## 排错
1. **看不到 RDMA 设备**
   - 确认 `setup_rxe.sh` 已执行
   - `rdma link show` 是否有 `rxe0`

2. **连接失败**
   - 两台机器 IP 是否互通（`ping`）
   - 端口是否被占用

3. **写入失败**
   - 检查接收端 MR 大小是否足够
   - 检查是否启用了 `IBV_ACCESS_REMOTE_WRITE`

4. **权限问题**
   - 输出目录是否有写权限

## 下一步学习建议
- 把控制面与数据面拆成独立模块，理解“协议设计 + 数据路径”的分层思维。
- 尝试把 RDMA Write 改为 RDMA Read 或双向 Send/Recv。
- 引入多 QP、多 CQ，观察延迟与吞吐变化。
