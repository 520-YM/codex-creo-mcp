# 新电脑完整迁移手册

## 仓库能够迁移的内容

- 67项 MCP 工具的 Node.js 服务。
- Pro/TOOLKIT 原生桥接源码。
- v14 内部会话绑定、重新生成、质量和装配能力。
- 壁挂14步流程、知识库和安全边界。
- Windows 构建、安装、测试和回滚脚本。
- 脱敏配置模板。

## 不随公开仓库迁移的内容

- PTC Creo 和 Pro/TOOLKIT 二进制、许可及头文件。
- Visual Studio、Node.js、Codex Desktop。
- WJT276 插件及其注册文件。
- 公司 CAD 模型、真实 `config.pro`、客户资料和私密 Excel/Word 原件。
- GitHub、PTC 或其他账号凭据。

## 目标电脑步骤

1. 安装与源码兼容的 Creo 及合法 Pro/TOOLKIT SDK。
2. 安装 Visual Studio C++ Build Tools、Node.js 20+、Git 和 Codex Desktop。
3. 克隆仓库。
4. 设置目标电脑自己的 `CREO_COMMON_FILES` 和 `VSDEVCMD`。
5. 运行 `build_all.cmd`，生成 `dist/bin` 中的 v14 DLL、加载器和命令程序。
6. 运行 `install.ps1`。
7. 审查安装目录中的 `CreoSafeResident.dat`。
8. 经电脑所有者明确许可后，手动注册 DAT，或用 `-RegisterConfigProPath` 让脚本先备份再追加一行。
9. 把脱敏 MCP 配置模板中的占位符替换为目标电脑路径，加入 Codex 配置。
10. 运行 `npm run check`。
11. 正常启动 Creo，选择测试工作目录。
12. 执行第一条只读命令，确认 v14、PID、启动时间、工作目录、顶层装配和骨架信息。
13. 使用测试模型执行不保存重新生成验证。
14. 最后才允许在正式模型上执行写入和保存。

## 公司数据迁移

公开仓库只含脱敏后的规则和结构化知识。CAD 项目与私密原件应通过公司批准的内部存储迁移到目标电脑。Creo 工作目录可位于任意路径；MCP 永远读取当前 Creo 工作目录，不依赖 `E:\project`。

## 回滚

- 安装脚本修改配置时会创建带时间戳的备份。
- 保留前一版 DLL、DAT 和 MCP 服务文件。
- 新版本依次通过静态测试、只读握手、不保存测试和保存测试。
- 如果 v14 无法加载，恢复配置备份和上一个已验证运行时，不修改 WJT276。
