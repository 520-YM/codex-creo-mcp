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

## 6. 创建 Creo Toolkit 注册文件

复制 `config\creotk.dat.example` 到安装目录并将 `<INSTALL_ROOT>` 替换为实际路径。

然后审查 `config\config.pro.example`。只有在获得配置文件所有者明确许可后，才把相应行加入实际 `config.pro`。不要覆盖完整配置文件。

如果电脑上有 WJT276，应保留其原注册段，将 CreoSafeResident 作为另一个独立注册段加入同一个 `.dat` 文件；不要修改或重新分发 WJT276 文件。

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
2. 启动 Creo 并选择工作目录。
3. 执行只读命令“读取当前模型”。
4. 确认返回的 `working_directory` 与 Creo 中选择的目录一致。
5. 再用测试模型执行写入工具。

## 9. 卸载

1. 从 Codex 配置中移除或禁用 `mcp_servers.creo_safe`。
2. 经配置文件所有者同意后，从 Creo 注册文件中移除 CreoSafeResident 段。
3. 删除安装目录。

卸载操作不应删除任何 Creo 项目模型。
