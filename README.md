# Codex × Creo MCP

这是一个面向 Windows 和 Creo Parametric 的 Codex MCP 集成项目。它通过 Node.js MCP 服务和进程内 Pro/TOOLKIT DLL，让 Codex 能够读取与修改当前 Creo 会话。DLL 随 Creo 正常启动加载，每个 Creo 进程使用独立命名管道；全部模型命令均在 Creo 进程内部执行。

当前版本提供 67 项正式 MCP 工具，覆盖模型读取、参数与尺寸、常用零件特征、装配、骨架、钣金、质量、壁挂改型、STEP 导出和版本管理。

## 核心规则

- 每次命令先从当前 Creo 会话读取用户已经选择的工作目录。
- 所有文件操作只允许作用于该工作目录的直接子文件。
- 工具不接受 `project_name`、固定项目根目录或调用方提供的目录路径。
- 不上传或附带 Creo、Pro/TOOLKIT、WJT276、公司模型、配置文件或其他第三方二进制。
- 写入工具在修改前核对模型、特征、尺寸和旧值，失败时尽量回滚。

## 架构

```mermaid
flowchart LR
    C[Codex Desktop] --> M[Node.js MCP Server]
    M --> P[Per-PID Named Pipe]
    P --> D[In-process Pro/TOOLKIT DLL]
    D --> R[Creo Parametric]
```

详细说明见 [架构文档](docs/ARCHITECTURE.md)。

## 安装概要

1. 安装 Creo Parametric、匹配版本的 Pro/TOOLKIT 和 Visual Studio Build Tools C++。
2. 安装 Node.js 20 或更高版本。
3. 设置 `CREO_COMMON_FILES` 和 `VSDEVCMD`，运行 `build_all.cmd`。
4. 运行 `install.ps1`，将 MCP 服务和编译产物安装到用户目录。
5. 根据模板配置 Codex MCP，并审查安装脚本生成的独立 `CreoSafeResident.dat`。
6. 用户明确授权后，在目标电脑的 Creo 启动配置中独立注册该 DAT；不修改 WJT276。
7. 正常启动 Creo、选择工作目录，再进行只读首条命令握手。

完整步骤见 [Windows 安装教程](docs/INSTALL_WINDOWS.md)。

## 目录

- `mcp/`：MCP 服务和安全清理脚本。
- `native/`：Pro/TOOLKIT C 源码。
- `scripts/`：常驻桥接快捷命令脚本。
- `config/`：脱敏配置模板。
- `docs/`：架构、安装、工具和故障排除文档。
- `tests/`：目录策略和静态安全检查。

## 重要警告

内部 DLL 按具体 Creo PID 建立独立管道，并随该 Creo 进程退出。项目不需要专用 Creo 启动器。DLL 不使用高频 Toolkit 定时轮询，而是只在收到命令时通过 Creo 主线程消息执行，以降低对原生撤销栈的干扰。实际 `config.pro` 只在安装者显式传入 `-RegisterConfigProPath` 时备份并追加独立 DAT 注册行；WJT276 始终不修改。详见 [撤销与常驻桥接](docs/UNDO_AND_RESIDENT.md)。

## 新电脑迁移

仓库保存全部可公开迁移的源码、规则、测试、构建与安装脚本。目标电脑仍需自行安装合法的 Creo、匹配的 Pro/TOOLKIT SDK、Visual Studio C++ Build Tools、Node.js 和 Codex Desktop。公司 CAD 模型、WJT276、真实配置、PTC 二进制和私密 Excel/Word 原件不会进入公开仓库。

完整迁移清单见 [新电脑迁移手册](docs/PORTABLE_MIGRATION.md)。设计流程见 [公司规则库](standards/README.md)。

## 第三方软件

PTC Creo、Pro/TOOLKIT、Visual Studio Build Tools、Codex 和 WJT276 均不包含在本仓库中。请自行取得合法安装包、许可和 SDK。
