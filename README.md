# Codex 会话删除工具

Windows 10 / 11 x64 桌面工具，使用 C++17、Unicode 和 MFC。无需安装，直接运行 [Release 程序](release/CodexSessionCleaner.exe)。

## 使用

1. 双击 `release\CodexSessionCleaner.exe`。程序自动递归扫描 `%USERPROFILE%\.codex\sessions`。
2. 在左侧选择主会话文件。列表仅显示 `session_meta.source` 为 `cli` / `vscode` 的交互主会话，隐藏子 Agent 和非交互会话；显示相对路径，按修改时间从新到旧排列。底部显示完整路径，可以复制。
3. 右侧显示前 5 条用户及助手文字消息。预览隐藏 `<INSTRUCTIONS>...</INSTRUCTIONS>` 和 `<environment_context>...</environment_context>` 区块（含标签），过滤后仅剩空白的消息跳过；原始文件不变。可选中文字并按 `Ctrl+C` 复制，`Ctrl+A` 全选；长消息自动换行，可滚动查看。
4. 核对完整路径后点击“删除选中会话…”。弹窗说明会永久删除该主会话及其所有层级的子 Agent 会话文件，默认选择“取消”；选择“确定”后执行删除。
5. 点击“刷新”重新扫描。状态栏仅统计主会话数量，并显示读取结果、损坏行数量和实际删除文件数量。

删除前重新扫描整个目录，根据 `source.subagent.thread_spawn.parent_thread_id` 查找全部后代，先删除深层子文件，最后删除主文件。普通 `fork` 生成的独立主会话不会被关联删除。成功后移除列表项并清空预览；失败时保留列表项，报告已删除的文件数量及失败路径，可再次删除完成剩余清理。文件被其他程序占用时可能需要关闭该程序后重试。

**删除无法撤销，不经过回收站。** 本工具只删除所选主会话及其子 Agent 的 `.jsonl` 文件，不修改 Codex 索引、`history.jsonl` 或数据库，也不保证清除其他界面中缓存的历史条目。

`resume --all` 的 `--all` 表示取消工作目录筛选。本工具采用其交互主会话筛选规则；数据库中的归档状态、缓存等仍可能导致两边数量不同。无法识别元数据的文件不会显示，并会报告错误；删除时若元数据无法读取或 ID 重复，则停止操作，避免遗漏或错误关联。主文件被外部删除后，本次列表中保存的会话 ID 仍可用于清理其后代。

归属判断依赖现存的父子关系：中间层文件已经丢失时，其后代可能无法关联到主会话。删除过程不是原子操作；如果 Codex 仍在创建子会话，扫描后新增的文件可能留存，建议结束相关任务后再删除。

目录不存在、目录为空或文件无法读取时会显示提示。扫描跳过目录链接、文件链接等重解析点；删除前重新检查目录范围、扩展名、实际路径和链接属性。扫描和预览在后台进行，切换选择不会被旧预览覆盖。

解析仅处理 `response_item` 内的 `message`，角色仅限 `user` / `assistant`，合并 `input_text`、`output_text`、`text` 片段。配置、工具调用、事件消息副本和无文字记录被忽略。空行忽略，损坏行跳过并计数。找到第 5 条文字消息后停止读取，因此损坏行数量仅统计已读取部分。

## 源码与编译

直接用 VS2022 打开 `CodexSessionCleaner.sln`，选择 `Debug | x64` 或 `Release | x64` 后生成解决方案。

依赖本机 VS2022 v143 C++ 工具链、v143 x86/x64 MFC 组件，以及 Windows SDK `10.0.22621.0`。本次交付使用 VS2022 Professional 17.13.6、MSVC 14.43.34808；MFC 已通过 `E:\Software\VS2022\vs2022\vs_setup.exe` 补装。

MFC 和 C/C++ 运行库均静态链接；Release 程序无需另外安装 Visual C++ 运行库。固定版本 `nlohmann/json v3.11.3` 单头文件及 MIT 许可证已放在 `third_party\nlohmann`，编译无需下载依赖。

在 PowerShell 中执行：

```powershell
.\tools\build.ps1 -Configuration Debug
.\tools\build.ps1 -Configuration Release
.\tools\test-ui.ps1 -Configuration Release
```

`build.ps1` 编译解决方案并运行核心测试。输出位于 `bin\x64\Debug` 和 `bin\x64\Release`。界面测试需要当前登录的 Windows 桌面会话。

## 测试与文件结构

核心测试和界面测试均创建唯一临时目录，清理前核对路径。删除测试不会操作真实 Codex 会话。

可以手动指定测试目录：

```powershell
.\release\CodexSessionCleaner.exe --sessions-root 'D:\临时会话'
```

指定目录后，该目录成为扫描与删除范围。正常使用不需要此参数。

- `src\Main.cpp`：MFC 界面、后台扫描与预览、结果版本检查。
- `src\SessionStore.cpp`：扫描、UTF-8 消息提取、文件句柄删除与路径核对。
- `tests\SessionStoreTests.cpp`：核心测试。
- `tools\test-ui.ps1`：仅针对测试进程的界面自动化。
- `docs\验证记录.md`：实际编译和验证结果。
- `release`：可直接运行的 Release 程序及中文说明、第三方许可证。
