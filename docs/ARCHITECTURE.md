# 架构说明

## 数据流

1. Codex 通过标准输入输出启动 `mcp/server.cjs`。
2. MCP 服务验证参数、工作目录、模型名、特征名和预期旧值。
3. 新 Creo 会话的首条命令先读取 PID、启动时间、专属管道、当前工作目录、当前模型、顶层装配和骨架基本信息。
4. 原生桥接程序通过 Pro/TOOLKIT 调用 Creo。
5. 修改后重新生成、读回验证并保存；失败时尽量回滚。
6. 旧版本清理只针对本次目标模型族，并移入 Windows 回收站。

## 组件

- `mcp/server.cjs`：67项工具的注册、参数验证、目录边界和进程调度。
- `native/*.c`：Pro/TOOLKIT 原生实现。
- `creo_internal_resident_loader_v14.exe`：仅供诊断或明确允许的动态载入场景使用。
- `creo_safe_resident_internal_v14.dll`：默认的 Creo 进程内统一命令执行器。
- `scripts/*.ps1`：经过模型身份保护的快捷业务命令。

## 工作目录边界

工具不保存项目根目录，也不接收目录参数。每次 MCP 调用开始时读取 Creo 当前工作目录并缓存到该次调用结束。模型文件必须为该目录的直接子文件，路径分隔符和目录逃逸会被拒绝。

## 会话绑定

- DLL 随正常 Creo 启动通过独立 DAT 注册加载。
- MCP 枚举当前 Creo PID，并用 PID、启动时间和独立命名管道选择精确会话。
- DLL 创建 PID 专用命名管道；PING 必须返回相同 Creo PID 和 `in_process_dll` 标识。
- MCP 在每条命令前复用并核对该内部管道；身份不一致时安全停止。
- DLL 与 Creo 位于同一进程，因此 Creo 退出时连接自然结束。

## 连接方式

- **默认**：随正常 Creo 启动加载的进程内 DLL，全部模型命令在 Creo 进程内部执行。
- **启动方式**：不使用专用启动器；用户按原方式启动 Creo。
- **触发方式**：DLL 使用 Windows 主线程消息按需执行命令，不使用 100ms Toolkit 定时轮询。
- **配置边界**：安装脚本默认不修改 `config.pro`；仅在用户显式提供 `-RegisterConfigProPath` 时备份并追加独立 DAT 注册行；不修改或替换 WJT276。
