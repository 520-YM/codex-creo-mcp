# 公司设计标准规则库

本目录集中管理户外横屏内贴玻璃壁挂产品的设计知识、Creo 自动改型规则和验证标准。

## 使用原则

- `rules/` 供工程师阅读和评审。
- `data/` 供 Codex 与 Creo 自动化程序直接读取。
- `source-private/` 保存原始 Word、Excel 等内部资料，默认不纳入 Git。
- 原始资料与整理后的规则冲突时，停止自动修改并请求人工确认。
- 自动化只能报告经过 API 读回验证的真实结果。

## 目录

- [安全与授权边界](rules/01_安全与授权边界.md)
- [产品尺寸与模型规则](rules/02_产品尺寸与模型规则.md)
- [气弹簧设计与选型](rules/03_气弹簧设计与选型.md)
- [过滤棉选型](rules/04_过滤棉选型.md)
- [风扇与铰链规则](rules/05_风扇与铰链规则.md)
- [壁挂自动改型流程](rules/06_壁挂自动改型流程.md)
- [验证回滚与结果报告](rules/07_验证回滚与结果报告.md)
- [当前已知问题与研究重点](rules/08_当前已知问题与研究重点.md)
- [气弹簧完整型号库](rules/09_气弹簧完整型号库.md)
- [公司通用建模与项目规则](rules/10_公司通用建模与项目规则.md)
- [Creo 会话首条命令流程](rules/11_会话首条命令流程.md)
- [Creo 常用操作流程](rules/12_Creo常用操作流程.md)
- [质量读取与力学计算范围](rules/13_质量读取与力学计算范围.md)
- [资料来源清单](sources.md)

## 结构化数据

- `data/wall_mount_sizes.json`
- `data/screen_models.json`
- `data/gas_spring_design_rules.json`
- `data/filter_cotton_rules.json`
- `data/automation_policy.json`
- `data/workflow_rules.json`

## 更新流程

1. 更新原始 Excel 或 Word。
2. 对比变化项，不直接覆盖已确认规则。
3. 同步 Markdown 和 JSON。
4. 校验单位、方向、允许偏差和模型特征名称。
5. 提交 Git 版本并记录确认人和日期。

## 规则优先级

当次明确指令 > 已批准的项目专用规则 > 本规则库中的公司通用规则 > Creo 默认行为。
