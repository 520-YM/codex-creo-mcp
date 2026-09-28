# Windows 安装教程

## 1. 前置条件

- Windows 10/11 x64。
- PTC Creo Parametric，建议与源码验证版本一致。
- 与 Creo 匹配且具有合法许可的 Pro/TOOLKIT SDK。
- Visual Studio 2019/2022 Build Tools，安装“使用 C++ 的桌面开发”。
- Node.js 20 或更高版本。
- Codex Desktop。

WJT276 不是本项目依赖，也不会随本项目安装或修改。

## 2. 克隆代码

```powershell
git clone <REPOSITORY_URL>
cd codex-creo-mcp
```

## 3. 设置编译环境

将路径换成同事电脑上的实际路径：

```powershell
$env:CREO_COMMON_FILES = 'C:\Program Files\PTC\Creo 10.0.0.0\Common Files'
$env:VSDEVCMD = 'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
```

## 4. 编译原生桥接

```powershell
.\build_all.cmd
```

编译结果位于 `dist\bin`。仓库不提供 PTC 库、头文件或已编译二进制。

## 5. 安装 MCP 文件

```powershell
.\install.ps1
```

默认安装目录：

```text
%USERPROFILE%\.codex\mcp\creo_safe
```

## 6. 内部连接加载方式

项目不使用专用 Creo 启动器。`install.ps1` 会生成独立的 `CreoSafeResident.dat`，其内容仅注册 v14 驻留 DLL，不包含 WJT276。

默认安装命令不修改 `config.pro`。审查 DAT 后，可由用户手动注册，或者显式授权安装脚本进行最小修改：

```powershell
.\install.ps1 -SkipBuild -RegisterConfigProPath 'C:\你的Creo配置目录\config.pro'
```

只有显式提供该参数时，脚本才会：

1. 读取指定的 `config.pro`。
2. 创建带时间戳的同目录备份。
3. 仅追加一条指向独立 `CreoSafeResident.dat` 的 `protkdat` 行。
4. 不修改已有选项、不替换现有插件注册、不修改 WJT276。

随后按原来的方式正常启动 Creo，无需任何专用启动器。

## 7. 配置 Codex MCP

打开 `config\codex-mcp-config.example.toml`，替换以下占位符：

- `<NODE_EXE>`：Node.js 可执行文件。
- `<INSTALL_ROOT>`：MCP 安装目录。
- `<CREO_COMMON_FILES>`：Creo Common Files。
- `<CREO_LOADPOINT>`：Creo 安装根目录。

审查后把该配置块加入 `%USERPROFILE%\.codex\config.toml`。

注意：配置模板中不存在 `CREO_PROJECT_ROOT`。所有工具都读取当前 Creo 工作目录。

## 8. 首次测试

1. 重启 Codex。
2. 正常启动 Creo，并选择工作目录。
3. 执行任意第一条业务命令；系统先自动完成只读会话、顶层装配和骨架基本信息握手。
4. 确认返回的 `working_directory` 与 Creo 中选择的目录一致。
5. 再用测试模型执行写入工具。

## 9. 迁移完整性检查

运行 `npm run check`，确认67项工具、当前工作目录策略、内部会话绑定和14步壁挂流程测试通过。再检查返回协议为 `creo-safe-internal-v14`。

## 10. 卸载

1. 从 Codex 配置中移除或禁用 `mcp_servers.creo_safe`。
2. 删除安装目录。

卸载操作不应删除任何 Creo 项目模型。
