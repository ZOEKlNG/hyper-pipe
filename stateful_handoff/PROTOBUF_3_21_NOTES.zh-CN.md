# Protobuf 3.21.12：本轮实测发现的两个隐藏成本

这份说明约束于本实验实际使用的3.21.12生成器/头文件/库，不应套用到所有Protobuf版本。

## 1. proto3隐式消息字段与显式optional的Clear行为不同

```proto
Module tls = 3;          // 第一版基线：ImplicitEvent
optional Module tls = 3; // 最终版本：ParallelEvent
```

本版生成器对前者使用指针是否为空表达presence（字段存在性）。父消息Clear或clear_tls会delete子对象再置空。显式optional使用has bits（存在性位图），Clear调用子对象的Clear后清位图，保留分配对象及字符串容量。

因此“普通并列子消息天然就能保留全部热容量”不成立。可直接对照[生成代码](events.pb.cc)中ImplicitEvent::Clear、ParallelEvent::Clear；[schema](events.proto)在同一二进制保留了两种类型。

oneof活动成员切换默认仍会清除/销毁旧成员；同一类型持续输出时，清子消息而不清父消息是不同的可复用路径。混合TLS＋HTTP要求两者同时存在，不能由oneof表达。

## 2. bytes的指针长度setter会先创建临时string

实际3.21.12头文件中：

```cpp
using ConstStringParam = const std::string&;
inline void ArenaStringPtr::SetBytes(const void* p, size_t n, Arena* arena) {
  Set(ConstStringParam{static_cast<const char*>(p), n}, arena);
}
```

所以`set_text0(ptr,len)`在这一配置中先创建临时std::string，再向PB拥有的string复制。长度超过SSO容量时，即使PB目标容量足够，临时对象仍需要一次分配。短字符串临时对象可能不分配，但仍多了内容复制。

最终编码器使用**官方生成类的公开mutable接口**：

```cpp
message.mutable_text0()->assign(ptr, len);
```

这仍是拥有式复制；不是借用输入、不是手写wire编码或改变标准PB格式。预热目标容量后避免临时string及其分配。每次字段更新后仍执行ByteSizeLong，再SerializeWithCachedSizesToArray。

## 分开测两项改变

P0隐式并列、P1 oneof、P2显式optional × `set_ptr_len` / `mutable_assign` 两种API，在相同消息、同一二进制中交叉实测。64B字段、每条交替TLS/HTTP/DNS、父消息Clear的分配审计中：

| 并列消息类型 | bytes API | C++ new/event |
|---|---|---:|
| 隐式 | set_ptr_len | 14 |
| 隐式 | mutable_assign | 10 |
| 显式optional | set_ptr_len | 4 |
| 显式optional | mutable_assign | 0 |

这是编码对象已预热后的计数，不包含输入生成或整条会话生命周期。审计独立于性能程序；它统计operator new而不是全部C malloc。[原始计数](results/proto_alloc.csv)

## 对先前报告的修正

此前core_handoff与json_compare计时本身仍是实际运行结果，但所用3.21.12 bytes setter有上述临时string成本，不能当作官方库可达到的最小编码开销。

此前core_handoff的载荷复制账本漏计了这个中间层：在该具体API下，A为4、A_scratch为5、B为4、B_direct为3、D为3层。改用mutable assign才回到A3、A_scratch4、B3、B_direct2、D2的账本。这里只计业务字节存储层，不含NIC、内核、标量、长度前缀与队列元数据。

第一版stateful隐式子消息＋旧setter的完整142配置结果和冻结源码保留为[基线](baseline_implicit/README.md)，最终主表重测显式optional＋mutable assign。不同轮次的吞吐不能直接作严格单变量因果比较；上面的同进程schema/API交叉测试用来分离这两个成本。
