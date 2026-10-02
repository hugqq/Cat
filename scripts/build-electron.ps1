$ErrorActionPreference = 'Stop'

$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$temporaryRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$temporaryOutput = Join-Path $temporaryRoot ('cat-electron-build-' + [Guid]::NewGuid().ToString('N'))
$portableOutput = Join-Path $projectRoot 'desktop-release\portable'
$locationPushed = $false

try {
    Push-Location $projectRoot
    $locationPushed = $true

    & npx electron-builder --win portable --x64 "--config.directories.output=$temporaryOutput"
    if ($LASTEXITCODE -ne 0) {
        throw "Electron 打包失败，退出代码：$LASTEXITCODE"
    }

    $artifact = Get-ChildItem -LiteralPath $temporaryOutput -Filter '*.exe' -File |
        Where-Object { $_.DirectoryName -eq $temporaryOutput } |
        Select-Object -First 1
    if (-not $artifact) {
        throw '没有找到生成的便携版 exe。'
    }

    New-Item -ItemType Directory -Path $portableOutput -Force | Out-Null
    $destination = Join-Path $portableOutput $artifact.Name
    Copy-Item -LiteralPath $artifact.FullName -Destination $destination -Force
    Write-Host "便携版已生成：$destination"
}
finally {
    if ($locationPushed) { Pop-Location }
    $resolvedTemporaryOutput = [System.IO.Path]::GetFullPath($temporaryOutput)
    if (
        $resolvedTemporaryOutput.StartsWith($temporaryRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedTemporaryOutput)
    ) {
        Remove-Item -LiteralPath $resolvedTemporaryOutput -Recurse -Force -ErrorAction SilentlyContinue
    }
}
