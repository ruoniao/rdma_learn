
# 真实 RDMA 版本执行记录（rdma-core + Soft‑RoCE）

日期：2026-02-13

## 脚本修正
- 修复脚本 BOM/CRLF 导致的 `#!/usr/bin/env: No such file or directory` 与 `^M` 问题。
- 统一为 LF，重新同步到 node1/node2。

## node1（192.168.153.130）
1. 安装依赖（rdma-core + verbs + rdmacm）
- 已安装成功。

2. 启用 Soft‑RoCE（网卡 ens33）
```
./setup_rxe.sh ens33
```
- `rdma link show` 可看到 `rxe0` 设备。
- `ibv_devices/ibv_devinfo` 未找到（可能 PATH 或工具缺失），但 rxe0 已创建。

3. 构建
```
./build.sh
```

4. 发送
```
echo "hello rdma real" > /app/source/rdma-learn/test.txt
./run_sender.sh 192.168.153.131 18500 /app/source/rdma-learn/test.txt
```
输出：
```
[run_sender] ip=192.168.153.131 port=18500 file=/app/source/rdma-learn/test.txt
[sender] done
```

## node2（192.168.153.131）
1. 安装依赖（rdma-core + verbs + rdmacm）
- 已安装成功。

2. 启用 Soft‑RoCE（网卡 ens33）
```
./setup_rxe.sh ens33
```
- `rdma link show` 可看到 `rxe0` 设备。
- `ibv_devices/ibv_devinfo` 未找到（可能 PATH 或工具缺失），但 rxe0 已创建。

3. 构建
```
./build.sh
```

4. 接收端日志（/tmp/rdma_receiver.log）
```
[run_receiver] ip=192.168.153.131 port=18500 out_dir=/app/source/rdma-recv
[receiver] listening on 192.168.153.131:18500
[receiver] incoming file: test.txt (16 bytes)
[receiver] saved to /app/source/rdma-recv/test.txt
[receiver] done
```

## 验证
```
cat /app/source/rdma-recv/test.txt
```
输出：
```
hello rdma real
```
