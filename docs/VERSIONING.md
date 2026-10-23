# 独立版本管理

[version.h](../version.h) 是 SemVer 字符串的唯一来源，不带前导 `v`，初始版本为 `0.1.0-dev.1`。
界面／日志显示 `v` 加该字符串；CMake 集成将同一字符串写入 Windows PE FileVersion。
PE 数字元组使用 `major,minor,patch,0`，预发布顺序由完整字符串表达，不由四段整数表达。

加载器文件名保留为 `renodx-dlss5.addon64`。通过验收的包可采用 `tools/check-version.ps1` 输出的
`ArchiveStem`，例如 `NR-Nexus-v0.1.0-dev.1-<short-sha>.zip`，包内仍使用原加载器文件名。
命名示例不表示已经打包或发布。

日常提交由 Git 历史记录，不递增版本；完成具体验证里程碑后再更新版本头，例如 `0.1.0-dev.2`。
`0.1.0-rc.1` 须完成内部验收，`0.1.0` 须通过稳定验收并获得所有者明确发布确认。
补丁／次／主版本按 SemVer 递增，上述序列只是示例。`0.x` 阶段公开兼容接口仍属实验性，变更须记录。
Lecram V8.5 RC10 和 NVIDIA 运行时／DLL 版本单独作为来源信息维护。

## 里程碑与发布流程

1. 审查并提交里程碑、更新后的版本头及验收结论。使用 `tools/build.ps1` 构建，执行受影响测试及适用的游戏／视觉验收；记录输入、提交、工具／依赖标识、产物哈希及未验证范围。未变化部分复用有效证据。
2. 运行 `tools/check-version.ps1` 和 `git diff --check`；检查公开文件树及新增可达历史，排除密钥、私有路径、专有资源和构建残留，核对许可义务。机器相关日志与原始证据不进入公开仓库。
3. 已提交里程碑通过上述门槛后，新建带注释的 `v<version>` 标签，并执行 `tools/check-version.ps1 -ExpectedTag <tag> -VerifyTag`。标签不可覆盖。
4. 开发标签通过门槛后可自动生成 GitHub **Pre-release**；RC 发布需内部验收，稳定 GitHub Release 还需所有者明确确认。仅上传合格插件二进制及必要声明，不上传厂商运行时 DLL 或模型。源码推送不自动触发发布或版本递增。

当前未安装无条件的标签到 Release 自动化。代码与验收缺口统一见 [NOW](context/NOW.md)。
文档整理不递增版本，也不构成标签或 Release 的验收。
