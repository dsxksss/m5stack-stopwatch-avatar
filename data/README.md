# HTTPS root certificate bundle

`x509_crt_bundle` is an ESP32 compact trust bundle generated from curl's
Mozilla CA extract. It is certificate trust data, not a firmware image and not
a credential.

- Source: <https://curl.se/ca/cacert.pem>
- Mozilla snapshot: 2026-08-13 03:12:01 GMT
- Source SHA-256: `F66DFF1BDF8F96060B8177976F8B7D9254BC89BC4DB933D769F7384D28480BC9`
- Generated bundle SHA-256: `49E7E1CA53F48330B1B507872F1447EB5F333632B6802282EC51AAAB5640787C`
- Roots: 121

Regenerate from an updated PEM bundle with PowerShell 7:

```powershell
./scripts/generate_ca_bundle.ps1 -PemPath ./cacert.pem
```

## On-device reading library

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
