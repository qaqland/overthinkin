> [!WARNING]
> 纯粹的 LLM 制品

在测试 keyi 中发现 sed 有较高概率使文件在编辑前后 mtime 不变（包括纳秒精度）

测试 demo 分别由 C 和 Bash 组成，前者几乎稳定触发，后者稳定不触发。

