# C++ → Java 点对点 UDP 组件原型

这是能在 Linux 两台机器运行的 **socket backend PoC**，用于验证协议、路由和有界队列语义。它没有实现 DPDK/AF_XDP 后端、Flink Connector、持久化、可靠交付、控制中心共识或动态迁移 fence。不要将其当成已完成的生产通信系统或“端到端零拷贝”实现。

## 编译与功能验证

需要 C++20 编译器、Linux `sendmmsg`、Java 21。标准 JDK 包含 `javac`；当前实验环境只有 JRE，因此使用官方 ECJ 编译器 jar。

```bash
# 有 JDK21 的环境
./build.sh

# 仅 JRE21 的环境：将 ECJ 下载到任意位置，然后指定
ECJ_JAR=/path/to/ecj.jar ./build.sh
python3 smoke.py
```

`build.sh` 运行 C++ 协议校验，并由 Java 读取 C++ 生成的二进制 fixture，检查正常字段以及错误的版本、总长度/尾长、头长和 magic。`smoke.py` 发送100条有效记录和3条畸形记录，检查 receiver 的统计守恒；这是功能测试，不是吞吐结论。它在 localhost 使用端口19091。

## 两台机器运行

接收机器（例 `192.0.2.20`，请换成实际地址）：

```bash
java -cp build UdpReceiver --host 0.0.0.0 --port 19090 --queue 4096 --seconds 60
```

发送机器：

```bash
build/producer --host 192.0.2.20 --port 19090 --count 1000000 --payload 256 \
  --batch 32 --flush-us 10 --rate 100000 --producer 1 --session 12345
```

只使用 IPv4 literal，UDP 目的端口需网络可达。`--rate 0` 表示饱和发送；默认 `--rate 0`。非零rate使用绝对时间表尽力pacing，线程被调度暂停后可能追赶形成短时突发，不能当成精确流量整形器。`--count` 是记录数，`--payload` 是可变尾部字节数，不含72字节头。当前 Datagram payload 上限1472字节（头72+尾最多1400），对应无IPv4 options且MTU1500的常见路径；隧道或更低PMTU环境必须缩小此上限。代码没有实现PMTU发现。

`--batch 32` 是一次 `sendmmsg` 提交至多32个独立 UDP 数据报，不是把32条记录拼成一个超大UDP包。待发送批次同时受 `--flush-us` 限制，低速输入也会定时flush。deadline 为尽力调度，Linux线程唤醒不保证准确到10µs；不能将参数当成实测时延保证。批次数组和路由/序号状态只在启动时分配。

每条日志的 event id 是 `i % 65536`；生产者本地计算 `mix64(id) & (bucket_count - 1)`。默认4096个虚拟桶，每桶有独立序列号，同一会话内从0递增。多个生产者必须使用不同producer id；“同id到同目标”不产生跨生产者的统一序号或全局顺序。

## 静态路由表

默认所有虚拟桶发往 `--host/--port`。可提供只覆盖部分桶的CSV，其余桶使用默认目的地址：

```csv
bucket,host,port,epoch
0,192.0.2.20,19090,1
1,192.0.2.21,19090,1
```

```bash
build/producer --routes routes.csv --buckets 4096 --host 192.0.2.20 --port 19090
```

CSV只在启动加载；生产者热路径不访问远程中心，也没有broker数据转发。同一id在同一路由快照下到相同目标。重启后改变CSV不能自动保证迁移期间只由一个消费者处理，更不能保证多生产者的全局顺序。epoch当前仅写入线格式，接收端没有实现租约验证或epoch fence，也没有验证producer/session/partition对应的owner。携带sequence字段不等于已经完成gap判断或迁移对账。

## 线格式

全部整数使用 little endian，编码/读取用显式offset，C++不强转packed结构体。版本为1、schema为1。

| Offset | 字节 | 字段 |
|---:|---:|---|
| 0 | 4 | magic，字节 `HPG1` |
| 4 | 2 | protocol version |
| 6 | 2 | header length，当前72 |
| 8 | 4 | total datagram length |
| 12 | 4 | schema id |
| 16 | 8 | producer id |
| 24 | 8 | boot/session id |
| 32 | 8 | per-bucket sequence |
| 40 | 8 | event/routing id |
| 48 | 8 | sender timestamp ns |
| 56 | 4 | virtual partition |
| 60 | 4 | route epoch |
| 64 | 4 | event type |
| 68 | 4 | payload length |
| header length | variable | payload bytes |

校验先检查报文最小/最大长度，再检查magic/version/schema/头长/总长与尾长关系，之后才访问字段。允许同版本头尾之间附加未知头字段，尾部从 `header_length` 开始。它不包含schema生成器和完整兼容性治理。

发送时间戳使用 `steady_clock`，用于本机调度观察；没有双机时钟同步及时间基准约定，不能直接据此计算跨机器单向时延。`session`默认源于系统时钟计数，生产系统应使用严格唯一的boot identifier；本例可用参数显式指定。

## 所有权、有界缓冲与计数

C++生产者在启动分配固定批次数量的buffer，原地构造字节。`sendmmsg`同步调用返回后本批用户缓冲可重用；普通UDP socket路径仍有内核复制。任何未被系统调用接受的数据报立即计入 `local_send_drop`，没有无限重试或上游阻塞。`kernel_accepted`只表示本机内核接受，**不表示网卡发出或远端收到**。JSON输出满足 `offered = kernel_accepted + local_send_drop`。

Java接收端有独立drain线程、`capacity + 2`个固定direct buffer、一个固定discard buffer及容量为 `capacity` 的 `ArrayBlockingQueue`。队列/pool满时仍继续收包并丢弃，采用drop-new策略。消费线程处理完才把slot归还pool；入队成功后drain线程不能再写该slot。`--worker-delay-us` 可故意减慢消费者用于功能验证。这里的ArrayBlockingQueue有锁，DatagramChannel与SocketAddress路径也不宣称零分配；选择它是为了给出清楚、可检查的最小实现。

接收JSON输出含 `received/invalid/queued/queue_drop/processed`，退出时等待队列drain并检查：

```text
received = invalid + queued + queue_drop
queued = processed
```

`pool_empty_observed`是子原因/尝试次数，不能与 `queue_drop` 再相加。内核socket溢出及网络丢失不在这些本地计数内；PoC尚无 `SO_RXQ_OVFL` 采集、乱序窗口、重号去重、尾部丢失对账或可靠high-watermark控制面。端到端丢失应由后续协议补充，不应简单拿本地send成功数当最终delivery数。

接收器主线程在 `--seconds` 后关闭socket，工作线程排空已接收队列。关闭边界不保证仍在网络或socket队列中的数据得到处理，因此容量测试应明确发送停止与接收drain间隔。

## 真实跨语言负载验证

```bash
python3 load_runner.py
```

它在loopback顺序运行3种负载，每种3轮：20k records/s、100k records/s、100k records/s且工作线程每条请求sleep50µs。每轮发送约2秒，接收器保持4秒以给socket与软件队列留排空时间。发送线程绑CPU1，Java进程可用CPU2/3，固定payload256B、每datagram328B；后者软件队列容量256，其余1024。输出保存至 `results/workloads.csv` 及每轮含完整命令/stdout的JSON。

这是验证跨语言协议和真实过载丢弃行为的实验，不是不同通信组件的最优吞吐排名。sleep50µs是请求值，调度器可以让实际等待更长，不能据此推导消费者真实20k/s能力。队列使用带锁实现，JVM启动、调度与UDP loopback也都在实验中。这里不计算未同步双机时延。`kernel_accepted - java_received` 是本轮未在Java观测到的数据，包含网络/socket队列/接收结束边界等因素，不应全部命名为软件queue丢失。
