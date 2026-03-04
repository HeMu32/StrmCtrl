# ADR-001: SignalingChannel 自定义前缀回调扩展

## 状态

已采纳

## 背景

`SignalingChannel` 目前只支持内置前缀：`MSG:`、`SDP:`、`CFG:VIDEO`、`READY`。
随着协议演进，需要在不修改核心分发逻辑的前提下，允许业务层注册新的消息前缀与处理回调。

## 决策

在 `SignalingChannel` 中新增可扩展路由机制：

- 提供 `registerPrefixCallback(prefix, cb)` 接口注册自定义前缀回调。
- 提供 `unregisterPrefixCallback(prefix)` 和 `clearPrefixCallbacks()` 管理接口。
- 内置前缀仍由原有分支优先处理，且被定义为保留前缀，不允许外部覆盖。
- 对自定义前缀采用“最长前缀优先”匹配，避免 `CFG:` 与 `CFG:VIDEO:` 之类重叠前缀冲突。
- 回调签名为 `(payload, sender_id)`，其中 `payload` 为去掉前缀后的剩余内容。

## 备选方案

1. 完全改为统一注册表（包含内置前缀）
   - 优点：实现统一。
   - 缺点：容易误覆盖 `READY` 等关键控制帧，影响握手可靠性。

2. 仅支持单一未知消息回调
   - 优点：实现简单。
   - 缺点：调用方仍需自行二次解析，扩展成本高。

## 影响

- 兼容性：保持向后兼容，现有主从握手不变。
- 安全性/可靠性：保留前缀不可覆盖，降低协议关键路径被误改风险。
- 可维护性：新协议扩展无需改动 `dispatchRawMessage` 主体分支。

## 后续

- 若未来协议复杂度继续增长，可考虑引入显式消息类型字段（例如 JSON 包装）替代字符串前缀协议。