# Hopa Reapth Control Plane Code

## 计划

### 1  **周期性全路径探测**
   - 每隔 300ms 发送四路的全路径 `perbe` 探测包

> **!** 探测报文方向 : cq ------> yy

### 2  **检测算法**
   - 搜集单向时延 (使用时间戳差值替代)
   - 计算梯度等数值
   - 检测换路需求

### 3  **信令交互**
   - `repath` : 换路信号
   - `repath_ack` : 确认信号
   - `retran_repath` : `repath` 丢失重传信号
   - 通过 `ACK` 实现丢失恢复机制