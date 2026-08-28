# On-device reading library

`b/z/catalog.kkbook` and `b/z/manifests/*.kkread` form the compact SPIFFS
edition of the bundled *Zero Lamp* novel. The abbreviated `b/z` path is
intentional: ESP32 SPIFFS limits full path length.

Regenerate the source package with:

```powershell
./scripts/convert_markdown_book_to_kkread.ps1 `
  -InputPath D:/game_demo/零灯_长篇小说.md `
  -OutputDirectory ./books/zero-lamp `
  -BookTitle 零灯
```

Only `catalog.kkbook` and the `manifests` directory belong in the device
filesystem. The exact Markdown source copy and JSON integrity catalogue remain
in `books/zero-lamp` and are not written to the watch.
