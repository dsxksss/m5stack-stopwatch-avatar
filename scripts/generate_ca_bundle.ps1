param(
    [Parameter(Mandatory = $true)]
    [string]$PemPath,

    [string]$OutputPath = "data/x509_crt_bundle"
)

$ErrorActionPreference = "Stop"
$OutputEncoding = [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = [Text.UTF8Encoding]::new($false)

$pemFile = (Resolve-Path -LiteralPath $PemPath).Path
$pemText = [IO.File]::ReadAllText($pemFile, [Text.Encoding]::ASCII)
$matches = [regex]::Matches(
    $pemText,
    '-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----'
)
if ($matches.Count -eq 0) {
    throw "No PEM certificates found in $pemFile"
}
if ($matches.Count -gt 200) {
    throw "Certificate count $($matches.Count) exceeds the firmware limit of 200"
}

$entries = foreach ($match in $matches) {
    $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::CreateFromPem(
        $match.Value
    )
    try {
        $publicKey = $null
        $rsa = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPublicKey(
            $certificate
        )
        if ($null -ne $rsa) {
            try { $publicKey = $rsa.ExportSubjectPublicKeyInfo() } finally { $rsa.Dispose() }
        }
        if ($null -eq $publicKey) {
            $ecdsa = [Security.Cryptography.X509Certificates.ECDsaCertificateExtensions]::GetECDsaPublicKey(
                $certificate
            )
            if ($null -ne $ecdsa) {
                try { $publicKey = $ecdsa.ExportSubjectPublicKeyInfo() } finally { $ecdsa.Dispose() }
            }
        }
        if ($null -eq $publicKey) {
            $dsa = [Security.Cryptography.X509Certificates.DSACertificateExtensions]::GetDSAPublicKey(
                $certificate
            )
            if ($null -ne $dsa) {
                try { $publicKey = $dsa.ExportSubjectPublicKeyInfo() } finally { $dsa.Dispose() }
            }
        }
        if ($null -eq $publicKey) {
            throw "Unsupported public-key algorithm for $($certificate.Subject)"
        }

        [pscustomobject]@{
            Subject = [byte[]]$certificate.SubjectName.RawData
            PublicKey = [byte[]]$publicKey
            SortKey = [Convert]::ToHexString($certificate.SubjectName.RawData)
        }
    } finally {
        $certificate.Dispose()
    }
}

$entries = @($entries | Sort-Object -Property SortKey)
$stream = [IO.MemoryStream]::new()
try {
    $stream.WriteByte(($entries.Count -shr 8) -band 0xFF)
    $stream.WriteByte($entries.Count -band 0xFF)
    foreach ($entry in $entries) {
        foreach ($length in @($entry.Subject.Length, $entry.PublicKey.Length)) {
            if ($length -gt 0xFFFF) { throw "Certificate field is too large" }
            $stream.WriteByte(($length -shr 8) -band 0xFF)
            $stream.WriteByte($length -band 0xFF)
        }
        $stream.Write($entry.Subject, 0, $entry.Subject.Length)
        $stream.Write($entry.PublicKey, 0, $entry.PublicKey.Length)
    }

    $target = [IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputPath))
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
    [IO.File]::WriteAllBytes($target, $stream.ToArray())
    Write-Host "Generated $target with $($entries.Count) roots ($($stream.Length) bytes)"
} finally {
    $stream.Dispose()
}
