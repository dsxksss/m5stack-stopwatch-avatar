[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [string]$BookTitle = '',

    [ValidateRange(200, 1800)]
    [int]$MaxBlockGlyphs = 1500,

    [ValidateRange(1, 96)]
    [int]$MaxBlockLines = 80,

    [ValidateRange(512, 8192)]
    [int]$MaxBlockBytes = 7000,

    [ValidateRange(1, 12)]
    [int]$MaxBlocksPerManifest = 10
)

$ErrorActionPreference = 'Stop'
$OutputEncoding = [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = [Text.UTF8Encoding]::new($false)
$Utf8NoBom = [Text.UTF8Encoding]::new($false)

function Get-TextElementCount {
    param([AllowEmptyString()][string]$Text)
    if ([string]::IsNullOrEmpty($Text)) { return 0 }
    return [Globalization.StringInfo]::ParseCombiningCharacters($Text).Count
}

function Test-BlockFits {
    param([AllowEmptyString()][string]$Text)
    $lineCount = if ([string]::IsNullOrEmpty($Text)) { 0 } else { ($Text -split "`n").Count }
    return (
        (Get-TextElementCount $Text) -le $MaxBlockGlyphs -and
        $lineCount -le $MaxBlockLines -and
        $Utf8NoBom.GetByteCount($Text) -le $MaxBlockBytes
    )
}

function Split-HardText {
    param([Parameter(Mandatory = $true)][string]$Text)
    $parts = [Collections.Generic.List[string]]::new()
    $indices = [Globalization.StringInfo]::ParseCombiningCharacters($Text)
    $builder = [Text.StringBuilder]::new()

    for ($i = 0; $i -lt $indices.Count; $i++) {
        $index = $indices[$i]
        $nextIndex = if ($i + 1 -lt $indices.Count) { $indices[$i + 1] } else { $Text.Length }
        $element = $Text.Substring($index, $nextIndex - $index)
        $candidate = $builder.ToString() + $element
        if ($builder.Length -gt 0 -and -not (Test-BlockFits $candidate)) {
            $parts.Add($builder.ToString())
            [void]$builder.Clear()
        }
        [void]$builder.Append($element)
    }

    if ($builder.Length -gt 0) { $parts.Add($builder.ToString()) }
    return $parts.ToArray()
}

function Split-IntoBlocks {
    param([Parameter(Mandatory = $true)][string]$Text)
    $blocks = [Collections.Generic.List[string]]::new()
    $current = ''
    $paragraphs = [regex]::Split($Text.Trim(), "`n\s*`n")

    foreach ($paragraph in $paragraphs) {
        $trimmed = $paragraph.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed)) { continue }
        $units = if (Test-BlockFits $trimmed) { @($trimmed) } else { @(Split-HardText $trimmed) }

        foreach ($unit in $units) {
            $candidate = if ([string]::IsNullOrEmpty($current)) { $unit } else { "$current`n`n$unit" }
            if (-not [string]::IsNullOrEmpty($current) -and -not (Test-BlockFits $candidate)) {
                $blocks.Add($current)
                $current = $unit
            }
            else {
                $current = $candidate
            }
        }
    }

    if (-not [string]::IsNullOrEmpty($current)) { $blocks.Add($current) }
    return $blocks.ToArray()
}

function Convert-MarkdownLinesToPlainText {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Lines
    )
    $plain = foreach ($line in $Lines) {
        $value = $line -replace '^\s*>\s?', ''
        $value = $value -replace '^\s*[-*_]{3,}\s*$', ''
        $value = $value -replace '`([^`]+)`', '$1'
        $value = $value -replace '\*\*([^*]+)\*\*', '$1'
        $value = $value -replace '(?<!\*)\*([^*]+)\*(?!\*)', '$1'
        $value.TrimEnd()
    }
    return (($plain -join "`n") -replace "`n{3,}", "`n`n").Trim()
}

$resolvedInput = (Resolve-Path -LiteralPath $InputPath).Path
$inputBytes = [IO.File]::ReadAllBytes($resolvedInput)
$inputText = $Utf8NoBom.GetString($inputBytes)
if ($inputText.Contains([char]0xFFFD)) {
    throw 'The input is not valid UTF-8 or already contains replacement characters.'
}

$allLines = $inputText.Replace("`r`n", "`n").Replace("`r", "`n") -split "`n"
if ([string]::IsNullOrWhiteSpace($BookTitle)) {
    $h1 = $allLines | Where-Object { $_ -match '^#\s+(.+)$' } | Select-Object -First 1
    $BookTitle = if ($h1 -match '^#\s+(.+)$') { $Matches[1].Trim() } else { [IO.Path]::GetFileNameWithoutExtension($resolvedInput) }
}

$sections = [Collections.Generic.List[object]]::new()
$currentTitle = ''
$currentVolume = ''
$currentLines = [Collections.Generic.List[string]]::new()

function Add-CurrentSection {
    if ([string]::IsNullOrWhiteSpace($script:currentTitle)) { return }
    $body = Convert-MarkdownLinesToPlainText $script:currentLines.ToArray()
    $displayTitle = if (
        -not [string]::IsNullOrWhiteSpace($script:currentVolume) -and
        $script:currentTitle -ne $script:currentVolume
    ) { "$($script:currentVolume) · $($script:currentTitle)" } else { $script:currentTitle }

    $content = if ([string]::IsNullOrWhiteSpace($body)) { $script:currentTitle } else { "$($script:currentTitle)`n`n$body" }
    $script:sections.Add([pscustomobject]@{ Title = $displayTitle; Content = $content })
    $script:currentLines.Clear()
}

foreach ($line in $allLines) {
    if ($line -match '^(#{2,4})\s+(.+?)\s*$') {
        Add-CurrentSection
        $level = $Matches[1].Length
        $heading = $Matches[2].Trim()
        if ($level -eq 2) { $currentVolume = $heading }
        $currentTitle = $heading
        continue
    }
    if ($line -match '^#\s+') { continue }
    if (-not [string]::IsNullOrWhiteSpace($currentTitle)) { $currentLines.Add($line) }
}
Add-CurrentSection

if ($sections.Count -eq 0) { throw 'No level 2-4 Markdown sections were found.' }

$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$sourceDirectory = Join-Path $outputRoot 'source'
$manifestDirectory = Join-Path $outputRoot 'manifests'
[IO.Directory]::CreateDirectory($sourceDirectory) | Out-Null
[IO.Directory]::CreateDirectory($manifestDirectory) | Out-Null

$sourceCopy = Join-Path $sourceDirectory 'book.md'
[IO.File]::WriteAllBytes($sourceCopy, $inputBytes)
$sourceHash = (Get-FileHash -LiteralPath $sourceCopy -Algorithm SHA256).Hash

$catalogEntries = [Collections.Generic.List[object]]::new()
$manifestIndex = 0
$sectionIndex = 0

foreach ($section in $sections) {
    $sectionIndex++
    $blocks = @(Split-IntoBlocks $section.Content)
    if ($blocks.Count -eq 0) { continue }
    $partCount = [Math]::Ceiling($blocks.Count / [double]$MaxBlocksPerManifest)

    for ($part = 0; $part -lt $partCount; $part++) {
        $manifestIndex++
        $offset = $part * $MaxBlocksPerManifest
        $take = [Math]::Min($MaxBlocksPerManifest, $blocks.Count - $offset)
        $partBlocks = @($blocks[$offset..($offset + $take - 1)])
        $title = if ($partCount -gt 1) { "$($section.Title)（$($part + 1)/$partCount）" } else { $section.Title }
        $lines = [Collections.Generic.List[string]]::new()
        $lines.Add('KKREAD/1')
        $lines.Add("TITLE: $title")
        foreach ($block in $partBlocks) {
            $lines.Add('::TEXT')
            foreach ($contentLine in ($block -split "`n")) { $lines.Add($contentLine) }
            $lines.Add('::END')
        }
        $manifestText = ($lines -join "`n") + "`n"
        $manifestBytes = $Utf8NoBom.GetBytes($manifestText)
        if ($manifestBytes.Length -gt 24576) {
            throw "Generated manifest exceeds 24 KiB: $title"
        }

        $fileName = '{0:D3}.kkread' -f $manifestIndex
        $manifestPath = Join-Path $manifestDirectory $fileName
        [IO.File]::WriteAllBytes($manifestPath, $manifestBytes)
        $catalogEntries.Add([ordered]@{
            index = $manifestIndex
            section = $sectionIndex
            part = $part + 1
            parts = [int]$partCount
            title = $title
            file = "manifests/$fileName"
            blocks = $partBlocks.Count
            bytes = $manifestBytes.Length
            sha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
        })
    }
}

$catalog = [ordered]@{
    format = 'KKREAD-BOOK/1'
    title = $BookTitle
    generated_utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    source = [ordered]@{
        file = 'source/book.md'
        bytes = $inputBytes.Length
        sha256 = $sourceHash
    }
    limits = [ordered]@{
        max_block_glyphs = $MaxBlockGlyphs
        max_block_lines = $MaxBlockLines
        max_block_bytes = $MaxBlockBytes
        max_blocks_per_manifest = $MaxBlocksPerManifest
        max_manifest_bytes = 24576
    }
    manifests = $catalogEntries
}

$catalogText = $catalog | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText((Join-Path $outputRoot 'catalog.json'), $catalogText + "`n", $Utf8NoBom)

$deviceCatalogLines = [Collections.Generic.List[string]]::new()
$deviceCatalogLines.Add('KKBOOK/1')
$deviceCatalogLines.Add("TITLE: $BookTitle")
$deviceCatalogLines.Add("SOURCE_SHA256: $sourceHash")
foreach ($entry in $catalogEntries) {
    if ($entry.title.Contains('|')) {
        throw "Chapter title contains the reserved catalog delimiter: $($entry.title)"
    }
    $deviceCatalogLines.Add("::CHAPTER $($entry.file)|$($entry.title)")
}
[IO.File]::WriteAllText(
    (Join-Path $outputRoot 'catalog.kkbook'),
    ($deviceCatalogLines -join "`n") + "`n",
    $Utf8NoBom
)

$readme = @"
# $BookTitle · KKREAD 书包

此目录由 scripts/convert_markdown_book_to_kkread.ps1 生成。

- source/book.md：原始 Markdown 的逐字节副本；
- catalog.json：完整章节元数据、文件大小和 SHA-256；
- catalog.kkbook：设备端使用的轻量章节目录；
- manifests/*.kkread：可由 KK 0.11.0 读取的 KKREAD/1 章节清单。

原稿 SHA-256：$sourceHash

这些文件目前只是本地书包。要让设备从公网读取，需要先放到公开或受控的 HTTPS
静态托管服务，再把某个 .kkread 文件的绝对 HTTPS 地址保存到 KK 的 /read 页面。
公开托管前请先确认作品的公开范围与版权授权。
"@
[IO.File]::WriteAllText((Join-Path $outputRoot 'README.md'), $readme.TrimStart() + "`n", $Utf8NoBom)

[pscustomobject]@{
    Title = $BookTitle
    SourceBytes = $inputBytes.Length
    SourceSha256 = $sourceHash
    Sections = $sections.Count
    Manifests = $catalogEntries.Count
    OutputDirectory = $outputRoot
} | Format-List
