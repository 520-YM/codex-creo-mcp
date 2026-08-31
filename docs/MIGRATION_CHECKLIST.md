# 同事电脑迁移检查表

## 软件

- [ ] Creo 版本与构建源码所用 SDK 兼容。
- [ ] Pro/TOOLKIT SDK 和许可可用。
- [ ] Visual Studio Build Tools C++ 已安装。
- [ ] Node.js 20+ 已安装。
- [ ] Codex Desktop 已安装。

## 编译与安装

- [ ] 设置 `CREO_COMMON_FILES`。
- [ ] 设置或自动发现 `VSDEVCMD`。
- [ ] `build_all.cmd` 成功。
- [ ] `install.ps1` 成功。
- [ ] Codex MCP 配置中的路径已替换。
- [ ] Creo Toolkit 注册文件已审查并获得许可。

## 首次验证

- [ ] 常驻桥接只读握手成功。
- [ ] 工作目录来自当前 Creo 会话。
- [ ] 不存在 `project_name` 或固定项目目录。
- [ ] 打开模型工具不关闭其他 Creo 窗口。
- [ ] 测试模型上的尺寸修改可读回验证。
- [ ] Creo 关闭后桥接自动退出。
- [ ] 已决定使用“速度优先”或“撤销优先”模式。

## 禁止迁移的内容

- 公司模型、图纸、工作目录和测试结果。
- 用户真实 `config.pro/config.sup`。
- WJT276、Creo、Pro/TOOLKIT 或 Visual Studio 二进制。
- API Token、GitHub 凭据、Codex 本地凭据。
- 截图、录屏、trail 和 traceback 日志。
