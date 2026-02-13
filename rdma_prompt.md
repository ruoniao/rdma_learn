### 需求背景
我有两台 VMware 虚拟机节点：
- node1: 192.168.153.130（VS Code 远程 SSH 连接到此节点开发）
- node2: 192.168.153.131
两台节点已实现 SSH 免密互通，均只有一个网卡，且未安装任何 RDMA 相关库。

- VS Code 在 Windows 上运行，Terminal 也是 Windows；如需执行远端命令，需要 ssh。

### 核心目标
帮我实现一个 **基于真实 RDMA 用户态库的文件传输 demo**（无 RDMA 硬件，使用 Soft‑RoCE 软件 RDMA 设备）。
使用 `rdma-core`（`librdmacm` + `libibverbs`）完成 node1 -> node2 的文件传输。

### 详细要求
#### 1. 前置准备（APT 安装）
- 列出两台节点需要安装的依赖（如 gcc、make、rdma-core、libibverbs-dev、librdmacm-dev 等）
- 提供一键安装脚本（区分单节点/双节点执行）

#### 2. RDMA 核心原理讲解（真实库）
- 用通俗语言解释 RDMA 核心概念（MR、PD、CQ、QP、CM、Send/Recv、RDMA Write）
- 说明 Soft‑RoCE 的作用及其与真实 RDMA 硬件的关系
- 要能让小白看懂，但要有整体架构视角（控制面/数据面/硬件层）

#### 3. 代码实现（真实 RDMA）
- 开发语言：C
- 功能：node1 作为发送端，将指定文件通过 RDMA Write 写入 node2 的内存，node2 落盘
- 代码结构：
  - `rdma_sim.h`：真实 RDMA 相关结构与函数声明（非 TCP 模拟）
  - `rdma_sim.c`：真实 RDMA 连接与数据传输逻辑（基于 rdma-core）
  - `sender.c`：发送端（读文件、注册 MR、RDMA Write、发送完成通知）
  - `receiver.c`：接收端（CM 监听、注册 MR、接收完成通知、落盘文件）
- 每一行核心代码加**详细中文注释**，针对重点要有解释
- 处理基础异常（文件不存在、连接失败、内存分配失败、RDMA 失败等）
- 流程：CM 建连 → MR 注册 → RDMA Write → FIN/ACK → 释放资源

#### 4. 构建脚本（build.sh）
- 一键编译生成 sender/receiver，可重复运行
- 处理清理旧文件

#### 5. 运行脚本（run_sender.sh/run_receiver.sh）
- run_receiver.sh：node2 启动接收端（监听 IP/端口/输出目录）
- run_sender.sh：node1 启动发送端（目标 IP/端口/文件路径）
- 日志输出便于调试

#### 6. Soft‑RoCE 启用脚本
- 提供脚本在两台节点启用 rxe（Soft‑RoCE）并验证设备可用

#### 7. 运行说明
- 清晰说明执行步骤：先 node2 启动接收端 → 再 node1 发送
- 给出测试示例（传输 test.txt 并验证）
- 常见问题排查（设备未就绪、端口占用、权限不足、网络不通）

### 额外要求
- 所有脚本/代码适配 Ubuntu/Debian（apt）
- 代码/脚本避免硬编码 IP/端口，通过参数指定
- 最终输出内容按“原理 → 环境安装 → 代码 → 构建 → 运行 → 测试 → 排错”顺序组织
- 不要反复问确认，一次性给出完整实现
