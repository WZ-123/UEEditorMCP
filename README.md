# UEEditorMCP

> **基于 [lilklon/UEBlueprintMCP](https://github.com/lilklon/UEBlueprintMCP) 改进与扩展。**  
> 原项目采用 **MIT 协议**，本插件在其基础上重构为面向 Unreal Editor 的固定 MCP 工具接口、动作注册表、持久连接和多领域编辑器自动化能力，同样遵循 MIT 协议发布。  
> 感谢 [@lilklon](https://github.com/lilklon) 提供的架构基础。

---

一个面向 **AI 辅助 Unreal Editor 开发** 的 MCP 插件。  
核心目标不是暴露越来越多零散工具，而是提供一个**固定工具接口 + 可持续扩展动作注册表（Action Registry）** 的架构，让 GitHub Copilot、Cursor 及其他兼容 MCP 的客户端能够稳定调用 Unreal Editor 能力。

当前项目由三部分组成：

- **`ue-editor-mcp`**：统一主服务器，固定暴露 **7 个 MCP 工具**
- **`ue-editor-mcp-logs`**：日志 / 资产缩略图 / 资产 diff / 资产历史
- **`ue-editor-mcp-insights`**：面向 Unreal Insights `.utrace` 的离线分析

> **仅限编辑器** — C++ 模块类型为 `Editor`，不会进入 Shipping、Development Game 等运行时打包产物，不影响游戏运行时性能。

## 功能特性

- **7 个固定 MCP 工具** — 单一 `ue-editor-mcp` 服务器，工具接口长期稳定
- **Action Registry 架构** — Python 侧统一注册动作元数据，AI 通过搜索 → schema → 执行完成操作
- **蓝图自动化编辑** — 蓝图创建、编译、变量/函数管理、节点操作、图结构导出、补丁应用
- **图编辑增强** — 自动布局、自动注释、节点选区编排、折叠为函数/宏、跨图导入导出节点
- **材质系统支持** — 材质创建、表达式编辑、属性设置、编译诊断、自动布局、自动注释、实例创建、材质应用到 Actor / Component
- **UMG / MVVM 支持** — Widget Blueprint、控件树编辑、事件绑定、属性设置、MVVM ViewModel / Binding 管理
- **输入系统支持** — Legacy Input 与 Enhanced Input 资产 / Mapping Context 管理
- **编辑器能力扩展** — Actor、Viewport、Outliner、PIE、Asset 操作、Asset Editor 打开、Level Streaming 管理
- **DataTable 支持** — CSV 导入 / 导出
- **日志上下文能力** — 实时日志、Saved/Logs 离线读取、日志断言、编译错误辅助排查
- **源码控制相关能力** — 资产缩略图、资产历史、对比仓库版本、Blueprint revisions diff
- **离线 Trace 分析** — `ue-editor-mcp-insights` 支持 `.utrace -> SummarizeTrace -> CSV -> 卡顿归因`
- **Niagara 能力接入** — 已集成 Monolith Niagara port，对 Niagara 动作做统一注册与调度
- **持久 TCP 连接** — Python 服务器与编辑器插件之间保持长连接，减少重复握手开销
- **批量执行** — `ue_batch` 通过 C++ `batch_execute` 在**单次 TCP 往返**中执行最多 50 个动作
- **多客户端支持** — 默认监听 `127.0.0.1:55558`，支持最多 8 路并发连接
- **崩溃保护** — 动作执行链路包含 SEH + C++ 异常防护
- **自动化配置** — `setup_mcp.ps1` / `setup_mcp.bat` 自动创建 venv，并生成 VS Code / Cursor 的 MCP 配置

## 架构

```text
VS Code / Cursor / MCP Client（GitHub Copilot 等）
        │
        ├── ue-editor-mcp             （7 fixed tools, stdio）
        ├── ue-editor-mcp-logs        （logs / thumbnails / diff / history）
        └── ue-editor-mcp-insights    （utrace offline analysis）
                     │
                     ▼
        Python MCP Servers
        ├── server_unified.py
        ├── server_unreal_logs.py
        └── server_unreal_insights.py
                     │
                     │ TCP/JSON（端口 55558，长度前缀帧）
                     ▼
        C++ 插件（FMCPServer → 每连接一个 FMCPClientHandler）
                     │
                     │ 游戏线程分发
                     ▼
        FEditorAction / Niagara Action / Editor API
                     │
                     ▼
                Unreal Editor
```

主工作链路是：

1. MCP 客户端通过 **stdio** 启动 Python MCP server
2. Python 侧根据工具请求解析动作、构造参数
3. Python 通过持久 TCP 连接把命令发给 Unreal Editor 内的 C++ 插件
4. C++ 插件把编辑器修改调度到 **GameThread**
5. 返回结构化 JSON 结果给客户端

## MCP 服务器说明

### 1) `ue-editor-mcp`

主服务器，对外暴露固定的 7 个 MCP 工具：

| # | 工具 | 用途 |
|---|------|------|
| 1 | `ue_ping` | 测试与 Unreal Editor 的连接 |
| 2 | `ue_actions_search` | 按关键字 / 标签搜索动作 |
| 3 | `ue_actions_schema` | 查询指定动作的输入 schema / 示例 / 元数据 |
| 4 | `ue_actions_run` | 执行单个动作 |
| 5 | `ue_batch` | 批量执行多个动作（单次 TCP 往返） |
| 6 | `ue_resources_read` | 读取嵌入式资源文档 |
| 7 | `ue_logs_tail` | 查看 Python 命令日志 / 编辑器日志缓冲区 |

这个服务器是**主要入口**。  
AI 一般通过下面两种工作流来调用能力。

#### AI 工作流（快速路径 — 1 次往返）

```text
# 当动作 ID 和参数已经明确时，直接用 ue_batch：
ue_batch(actions=[
  {action_id: "blueprint.create", params: {name: "BP_Player", parent_class: "Character"}},
  {action_id: "variable.create", params: {blueprint_name: "BP_Player", variable_name: "Speed", variable_type: "Float"}},
  {action_id: "blueprint.compile", params: {blueprint_name: "BP_Player"}}
])
```

#### AI 工作流（发现路径 — 3 次往返）

```text
第 1 步：ue_actions_search(query="create blueprint")
  → 返回匹配动作 ID 列表

第 2 步：ue_actions_schema(action_id="blueprint.create")
  → 返回输入 schema、示例、描述

第 3 步：ue_actions_run(action_id="blueprint.create", params={...})
  → 执行动作并返回结果
```

---

### 2) `ue-editor-mcp-logs`

面向日志与资产上下文的轻量独立服务器，当前暴露 4 个工具：

- `unreal_logs_get`
- `unreal_asset_thumbnail_get`
- `unreal_asset_diff_get`
- `unreal_asset_history_get`

适合以下场景：

- 获取实时编辑器日志
- 在 UE 未连接时，从 `Saved/Logs` 做离线读取
- 获取资产缩略图（PNG base64 / image block）
- 查看资产源码控制历史
- 对比资产与仓库版本差异

---

### 3) `ue-editor-mcp-insights`

面向 `.utrace` 离线性能分析的独立服务器，当前暴露：

- `unreal_insights_summarize_trace`
- `unreal_insights_summarize_thread_scopes`
- `unreal_insights_analyze_trace`
- `unreal_insights_analyze_frame_window`
- `unreal_insights_explain_stutter`

适合以下场景：

- 调用 `SummarizeTrace` 导出 CSV 摘要
- 做线程级热点归因
- 分析某段 frame window
- 生成更适合直接回复用户的“卡顿原因解释”

## 当前能力覆盖

README 不再强依赖容易过时的“动作总数”统计，下面按能力域说明当前覆盖范围。

| 域 | 说明 | 典型动作 |
|----|------|----------|
| `blueprint.*` | 蓝图创建、编译、属性、父类、接口、组件、完整快照 | `blueprint.create`、`blueprint.compile`、`blueprint.describe_full` |
| `component.*` | 组件属性、静态网格、物理、组件事件绑定 | `component.set_property`、`component.bind_event` |
| `node.*` | 蓝图节点创建 | `node.add_event`、`node.add_function_call`、`node.add_branch` |
| `graph.*` | 图连接、描述、注释、补丁、选区、折叠、跨图导入导出 | `graph.describe`、`graph.apply_patch`、`graph.collapse_selection_to_function` |
| `layout.*` | 自动布局与批量布局注释 | `layout.auto_selected`、`layout.auto_blueprint` |
| `variable.*` | 变量创建、重命名、默认值、元数据 | `variable.create`、`variable.set_default` |
| `function.*` | 函数创建、调用、删除、重命名 | `function.create`、`function.rename` |
| `dispatcher.*` | 事件派发器与 delegate 创建 / 绑定 | `dispatcher.create`、`dispatcher.create_event` |
| `material.*` | 材质创建、表达式、编译、布局、注释、实例、应用 | `material.create`、`material.compile`、`material.apply_to_actor` |
| `widget.*` | UMG Widget Blueprint 编辑、控件树、属性与事件绑定 | `widget.create`、`widget.get_tree`、`widget.set_properties` |
| `widget.mvvm_*` | MVVM ViewModel / Binding 管理 | `widget.mvvm_add_viewmodel`、`widget.mvvm_get_bindings` |
| `input.*` | Legacy Input / Enhanced Input 管理 | `input.create_action`、`input.create_mapping_context` |
| `editor.*` | 编辑器、Actor、Asset、PIE、日志、Outliner、Level Streaming | `editor.spawn_actor`、`editor.start_pie`、`editor.get_outliner_tree` |
| `batch.*` | 批量执行 | `batch.execute` |

另外，C++ 侧还接入了 **Niagara Monolith port**，通过 `niagara_*` 类型命令统一调度。

## 编译诊断建议

### Blueprint 编译诊断

- `blueprint.compile` 返回状态与汇总计数
- 对某些复杂 UMG / MVVM 编译失败，完整编译器文本未必都包含在 compile 返回值中
- 更可靠的排查方法：

```text
1. blueprint.compile
2. editor.get_logs(count=200, category="LogBlueprint", min_verbosity="Error")
3. 如需更底层上下文，再看 LogOutputDevice / Ensure / Fatal
```

### Material 编译诊断

- `material.compile` 会等待材质编译完成
- 返回 `error_count`、`warning_count`、`errors[]`
- 每条错误尽可能关联 `expression_name` / `expression_class` / `node_name`

推荐排查流程：

```text
1. material.compile
2. ue_logs_tail(source="editor", category="LogMaterial", min_verbosity="Error")
```

## 自动布局命令

### 蓝图编辑器

插件会在 Blueprint Editor 中注册 **Auto Layout** 命令：

- 菜单：`编辑 -> Auto Layout`
- 默认快捷键：`Ctrl + Alt + L`

行为：

- 有选中节点：布局当前选区
- 无选中节点：布局当前焦点图

### 材质编辑器

插件会在 Material Editor 中注册 **Auto Layout** 菜单项：

- 菜单：`Edit -> Auto Layout`

行为：

- 对当前材质图执行自动布局
- 与 `material.auto_layout` 动作保持一致

---

## 安装与部署

下面是推荐部署方式，适用于 **VS Code / Cursor + Unreal Editor + MCP** 的常见工作流。

### 第 1 步：将插件放入项目

将本插件放到 Unreal 项目的：

```text
Plugins/UEEditorMCP/
```

确保项目结构类似：

```text
YourProject/
├─ Plugins/
│  └─ UEEditorMCP/
├─ Content/
├─ Config/
└─ YourProject.uproject
```

---

### 第 2 步：编译 C++ 插件

本插件是 **Editor 模块**，需要编译编辑器目标。

示例：

```powershell
Engine\Build\BatchFiles\Build.bat YourProjectEditor Win64 Development <项目根>\YourProject.uproject -waitmutex
```

例如：

```powershell
F:\UE_5.x\Engine\Build\BatchFiles\Build.bat MyGameEditor Win64 Development F:\Work\MyGame\MyGame.uproject -waitmutex
```

也可以直接使用 Visual Studio / Rider / VS Code 任务编译编辑器目标。

> 如果你的项目能正常编译编辑器插件，UEEditorMCP 会随编辑器一起加载。

---

### 第 3 步：配置 Python MCP 环境（推荐一键方式）

本项目推荐使用 **Unreal Engine 自带 Python** 自动创建虚拟环境。  
通常**不需要单独安装系统 Python**。

#### PowerShell（推荐）

在插件目录执行：

```powershell
cd Plugins/UEEditorMCP
.\setup_mcp.ps1
```

如果自动检测不到引擎路径，可以显式指定：

```powershell
.\setup_mcp.ps1 -EngineRoot "F:\UE_5.x"
```

#### 命令提示符

```cmd
cd Plugins\UEEditorMCP
setup_mcp.bat
```

#### 脚本会自动完成的事情

`setup_mcp.ps1` / `setup_mcp.bat` 会：

1. 自动定位 Unreal Engine 内置 Python
2. 在 `Plugins/UEEditorMCP/Python/.venv` 创建虚拟环境
3. 安装 `requirements.txt` 中依赖
4. 调用 `ensure_mcp_servers.ps1`
5. 自动生成：
   - `.vscode/mcp.json`
   - `.cursor/mcp.json`

也就是说，**现在推荐的部署方式是直接跑脚本**，不再建议优先手写 MCP 配置。

---

### 第 4 步：确认生成的 MCP 配置

脚本会在项目根目录生成两份配置：

#### VS Code

```text
.vscode/mcp.json
```

#### Cursor

```text
.cursor/mcp.json
```

其中会自动配置三个服务器：

- `ue-editor-mcp`
- `ue-editor-mcp-logs`
- `ue-editor-mcp-insights`

并自动填好：

- venv Python 路径
- `PYTHONPATH`
- `WORKSPACE_ROOT`（Insights server 用）

---

### 第 5 步：启动顺序

推荐启动顺序如下：

1. **打开 Unreal 项目**
   - 插件加载后，C++ TCP 服务器会在本地启动，默认监听 `127.0.0.1:55558`

2. **打开 VS Code 或 Cursor**
   - MCP 客户端会根据生成的配置自动启动 Python MCP servers

3. **在聊天窗口中调用**
   - GitHub Copilot / Cursor / 其他兼容 MCP 客户端 即可通过 MCP 工具访问 UE 编辑器能力

---

## 手动配置（可选）

如果你不想使用自动脚本，也可以手动配置。手写 MCP 配置时推荐使用绝对路径；不同客户端对相对路径的解析基准可能不同，自动脚本生成的配置最可靠。

### 1) 创建 Python venv

```bash
cd Plugins/UEEditorMCP/Python
python -m venv .venv
```

Windows 激活：

```bash
.venv\Scripts\activate
pip install -r requirements.txt
```

### 2) 手动写 `.vscode/mcp.json`

```jsonc
{
  "servers": {
    "ue-editor-mcp": {
      "command": "./Plugins/UEEditorMCP/Python/.venv/Scripts/python.exe",
      "args": ["-m", "ue_editor_mcp.server_unified"],
      "env": {
        "PYTHONPATH": "./Plugins/UEEditorMCP/Python"
      }
    },
    "ue-editor-mcp-logs": {
      "command": "./Plugins/UEEditorMCP/Python/.venv/Scripts/python.exe",
      "args": ["-m", "ue_editor_mcp.server_unreal_logs"],
      "env": {
        "PYTHONPATH": "./Plugins/UEEditorMCP/Python"
      }
    },
    "ue-editor-mcp-insights": {
      "command": "./Plugins/UEEditorMCP/Python/.venv/Scripts/python.exe",
      "args": ["-m", "ue_editor_mcp.server_unreal_insights"],
      "env": {
        "PYTHONPATH": "./Plugins/UEEditorMCP/Python",
        "WORKSPACE_ROOT": "."
      }
    }
  }
}
```

### 3) Cursor 配置格式

如果是 Cursor，需要使用 `.cursor/mcp.json`，字段名为 `mcpServers`，格式与 `ensure_mcp_servers.ps1` 自动生成的内容一致。

---

## 日志上下文工具（`ue-editor-mcp-logs`）

### `unreal_logs_get`

支持实时日志、Saved/Logs 追踪和离线回退。

主要参数：

- `mode`：`auto | live | saved`
- `tailLines`：默认 `200`
- `maxBytes`：默认 `65536`
- `cursor`：增量游标
- `workspaceRoot`：UE 不可达时用于离线读取
- `filter.minVerbosity`
- `filter.category`
- `filter.contains`

推荐用法：

1. 首次读取：
   ```text
   mode=auto, tailLines=200, maxBytes=65536
   ```
2. 保存返回的 `cursor`
3. 后续继续传 `cursor` 做增量读取

---

### `unreal_asset_thumbnail_get`

获取资产缩略图。

支持：

- `assetPath`
- `assetPaths`
- `assetIds`
- `ids`
- `size`

返回：

- `thumbnails[]`
- 兼容字段 `image_base64`

---

### `unreal_asset_diff_get`

对比资产与源码控制版本。

支持：

- Blueprint 节点级 diff
- 通用资产属性级 diff
- 指定 revision

---

### `unreal_asset_history_get`

列出资产源码控制历史修订，可与 `unreal_asset_diff_get` 配合使用。

---

## Unreal Insights 离线分析工具（`ue-editor-mcp-insights`）

### `unreal_insights_summarize_trace`

对 `.utrace` 执行 `SummarizeTrace`，导出：

- `Scopes.csv`
- `Bookmarks.csv`
- 以及其他摘要 CSV

可选：

- `projectPath`
- `editorCmdPath`
- `engineRoot`
- `timeoutSeconds`
- `includeThreadScopes`

---

### `unreal_insights_summarize_thread_scopes`

通过 `UnrealInsights.exe` 导出线程 timing events，聚合为 `ThreadScopes.csv`。

默认线程：

- `GameThread`
- `RenderThread 0`
- `RHIThread`

---

### `unreal_insights_analyze_trace`

读取已有 CSV，输出：

- 全局热点
- bookmark 聚类
- 按线程热点
- scope 线程归属
- 卡顿模式检测

---

### `unreal_insights_analyze_frame_window`

对指定帧段或时间窗做聚焦分析。

---

### `unreal_insights_explain_stutter`

输出更偏“结论型”的解释，适合直接作为用户回复。

推荐工作流：

```text
1. unreal_insights_summarize_trace
2. unreal_insights_summarize_thread_scopes（可选，但推荐）
3. unreal_insights_analyze_trace
4. unreal_insights_analyze_frame_window（需要帧段时）
5. unreal_insights_explain_stutter
```

---

## 新增动作

如果要扩展插件能力，通常分三步：

### 第 1 步：C++ 侧实现动作

例如新增一个 Action：

```cpp
// Source/UEEditorMCP/Public/Actions/MyActions.h
class FMyNewAction : public FEditorAction
{
public:
    FMyNewAction() : FEditorAction(TEXT("my_new_action")) {}
    virtual FString Validate(const TSharedPtr<FJsonObject>& Params) override;
    virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params) override;
};
```

并在 `MCPBridge.cpp` 注册：

```cpp
ActionHandlers.Add(TEXT("my_new_action"), MakeShared<FMyNewAction>());
```

### 第 2 步：Python 侧注册 ActionDef

在 `Python/ue_editor_mcp/registry/actions.py` 添加：

```python
ActionDef(
    id="domain.my_new_action",
    command="my_new_action",
    description="这个动作的用途说明",
    tags=("domain", "keyword1", "keyword2"),
    input_schema={
        "type": "object",
        "properties": {
            "param1": {"type": "string", "description": "..."},
        },
        "required": ["param1"],
    },
    examples=(
        {"param1": "value"},
    ),
)
```

### 第 3 步：重新编译并重启

```powershell
Engine\Build\BatchFiles\Build.bat YourProjectEditor Win64 Development <项目根>\YourProject.uproject -waitmutex
```

然后重启编辑器 / MCP 客户端即可。

> 一般情况下，不需要修改 `server_unified.py` 本身；只要 C++ 命令和 ActionDef 注册完成，`ue_actions_search / schema / run` 就能自动识别。

## 编辑器专属安全保障

本插件通过多层机制保证仅在编辑器环境生效：

| 层级 | 机制 | 效果 |
|------|------|------|
| `.uplugin` | `"Type": "Editor"` | 非编辑器目标不会加载该模块 |
| `.Build.cs` | 依赖 `UnrealEd`、`BlueprintGraph`、`UMGEditor`、`MaterialEditor` 等编辑器模块 | 游戏目标无法链接这些依赖 |
| `.uplugin` | `PlatformAllowList = Win64 / Mac / Linux` | 仅支持桌面编辑器平台 |

TCP 服务器、MCP 工具和所有编辑器操作逻辑都只存在于编辑器模块中。

## 技术细节

### C++ TCP 服务器（`FMCPServer`）

- 监听 `127.0.0.1:55558`
- 每个连接由独立 `FMCPClientHandler` 线程处理
- `ping` / `close` 等轻量命令可直接在线程侧处理
- 其他编辑器命令统一切到 `GameThread`
- C++ 连接空闲超时默认 `300s`；Python 命令超时默认 `120s`
- 启用 `SO_REUSEADDR`
- 默认最大客户端数：`8`

### Python 统一服务器（`server_unified.py`）

- 对外暴露 7 个固定工具
- 通过 Action Registry 做动作搜索与 schema 自省
- `ue_batch` 最大 50 条动作
- 维护 Python 侧命令日志 ring buffer
- 支持从返回结果中抽取 image block

### 日志服务器（`server_unreal_logs.py`）

- 支持 UE 实时环形日志读取
- 支持从 `Saved/Logs` 反向 tail
- 支持 cursor 增量读取
- 支持资产 diff / history / thumbnail

### Insights 服务器（`server_unreal_insights.py`）

- 读取 `.utrace`
- 调用 `UnrealEditor-Cmd.exe -run=SummarizeTrace`
- 调用 `UnrealInsights.exe` 导出线程 scope
- 基于 CSV 做热点聚合与模式归因

### 通信协议

Python 与 C++ 间通信采用长度前缀协议：

```text
[4 字节消息长度（大端序）] [UTF-8 JSON payload]
```

请求示例：

```json
{"type": "create_blueprint", "params": {"name": "BP_MyActor", "parent_class": "Actor"}}
```

响应示例：

```json
{"success": true, "blueprint_name": "BP_MyActor", "path": "/Game/Blueprints/BP_MyActor"}
```

## 关键文件

| 文件 | 用途 |
|------|------|
| `Python/ue_editor_mcp/server_unified.py` | 主 MCP 服务器，固定 7 工具 |
| `Python/ue_editor_mcp/server_unreal_logs.py` | 日志 / 缩略图 / diff / history 服务器 |
| `Python/ue_editor_mcp/server_unreal_insights.py` | `.utrace` 离线分析服务器 |
| `Python/ue_editor_mcp/registry/__init__.py` | ActionRegistry 实现 |
| `Python/ue_editor_mcp/registry/actions.py` | ActionDef 注册表 |
| `Python/ue_editor_mcp/connection.py` | 持久 TCP 连接封装 |
| `Python/ue_editor_mcp/resources/*.md` | 资源文档 |
| `Source/UEEditorMCP/Private/MCPServer.cpp` | TCP 服务器与客户端处理线程 |
| `Source/UEEditorMCP/Private/MCPBridge.cpp` | C++ 命令注册与分发中心 |
| `Source/UEEditorMCP/Private/Actions/*.cpp` | 编辑器动作实现 |
| `Source/UEEditorMCP/Private/Niagara/*.cpp` | Niagara port 能力实现 |
| `Source/UEEditorMCP/UEEditorMCP.Build.cs` | 模块依赖配置 |
| `setup_mcp.ps1` / `setup_mcp.bat` | 一键配置 Python MCP 环境 |
| `ensure_mcp_servers.ps1` | 自动生成 `.vscode/.cursor` MCP 配置 |

## 环境要求

- Unreal Engine 5.x 编辑器环境
- Visual Studio 2022 / 对应平台 C++ 构建工具
- VS Code、Cursor 或其他兼容 MCP 的客户端
- Windows 优先（脚本与自动配置目前主要围绕 Windows 工作流设计）

## 文档

- **[DEVPLAN.md](DEVPLAN.md)** — 开发计划与阶段性设计记录

---

## 许可证

MIT

本项目基于 [lilklon/UEBlueprintMCP](https://github.com/lilklon/UEBlueprintMCP)（MIT 许可证）改进与扩展。  
原始代码版权归 [@lilklon](https://github.com/lilklon) 所有。
