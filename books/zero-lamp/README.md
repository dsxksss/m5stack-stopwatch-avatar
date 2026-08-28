# 零灯 · KKREAD 书包

此目录由 scripts/convert_markdown_book_to_kkread.ps1 生成。

- source/book.md：原始 Markdown 的逐字节副本；
- catalog.json：完整章节元数据、文件大小和 SHA-256；
- catalog.kkbook：设备端使用的轻量章节目录；
- manifests/*.kkread：可由 KK 0.11.0 读取的 KKREAD/1 章节清单。

原稿 SHA-256：BC669FB4382FEFC8904DA521C94B104EC97BDCCA54F51A2F49842E0D45B6A137

这些文件只用于生成设备内置书库。`catalog.kkbook` 与 `manifests` 目录会写入
SPIFFS；原始 Markdown 副本和完整性目录只保留在仓库中，不写入设备。
