# NR Nexus 项目索引

NR Nexus 是面向 RenoDX/ReShade 的独立、非官方 DLSS 神经渲染插件。本仓库仅含插件源码与项目工具，不含完整 RenoDX 宿主或旧研究工作区。按任务选择下列入口，无需全文读取。

## 当前边界

源码基线为 Lecram V8.5 RC10 导入提交 `9bb6c0fec57ecb28c77938f3feb8155433396d93`，独立项目初始化提交为 `db747cc0843691479be28494812e417a88e87c6f`。当前里程碑、构建阻塞及集成缺口统一见 [NOW](docs/context/NOW.md)。

[version.h](version.h) 是独立版本的唯一来源，与 Lecram 及 NVIDIA 运行时版本分开管理。旧项目的后续 NR+FG 实现尚未集成，其验收不适用于本源码快照。

## 文档入口

| 事项 | 权威文档 |
| --- | --- |
| 修改范围、语言维护、安全边界、最低验收 | [AGENTS](AGENTS.md) |
| 项目用途、公开能力、使用前提（中英双语） | [README](README.md) |
| 当前源码、里程碑、阻塞及验证状态 | [NOW](docs/context/NOW.md) |
| 既有技术决定及适用范围 | [DECISIONS](docs/context/DECISIONS.md) |
| 构建、定向测试、游戏自动化及证据边界 | [RUNBOOK](docs/context/RUNBOOK.md) |
| 已完成的源码迁移、后续集成与目录切换条件 | [MIGRATION](docs/MIGRATION.md) |
| 独立版本、标签及发布流程 | [VERSIONING](docs/VERSIONING.md) |
| 复用来源、原始版权与许可（保留原文） | [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES.md)、[LICENSE](LICENSE) |

## 目录职责

| 位置 | 用途 |
| --- | --- |
| 根目录 C++ 文件、`ui/`、`shaders/` | 插件源码，在兼容宿主中对应 `src/addons/dlss5` |
| `version.h`、`cmake/`、`tools/` | 独立版本定义及构建集成 |
| `docs/context/` | 当前状态、技术约束及验证方法 |
| `.local/context/README.md`（若存在） | 被忽略的本地来源索引，定位旧设计、宿主规则、测试和证据 |
| `tmp/`、`build/`、`artifacts/`、`dist/` | 被忽略的临时、构建、证据及打包产物 |

旧项目继续维护其未集成源码、详细设计和运行证据；本地索引只保存路由，不复制其动态进度，也不表示外部工具已迁入本仓库。路径映射由本页维护，当前风险归 NOW，迁移历史归 MIGRATION，避免另建重复索引。

最后核验：2026-10-24，范围为仓库状态、文档职责及语言规则。
