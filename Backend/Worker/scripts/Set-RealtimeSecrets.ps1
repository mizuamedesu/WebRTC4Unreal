[CmdletBinding()]
param(
    [string]$EnvFile = (Join-Path $PSScriptRoot '..\..\..\.env'),
    [switch]$Deploy
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-DotEnvValues {
    param([Parameter(Mandatory)][string]$Path)

    $result = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -notmatch '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)\s*$') {
            continue
        }

        $name = $Matches[1]
        $value = $Matches[2].Trim()
        if ($value.Length -ge 2) {
            $isDoubleQuoted = $value.StartsWith('"') -and $value.EndsWith('"')
            $isSingleQuoted = $value.StartsWith("'") -and $value.EndsWith("'")
            if ($isDoubleQuoted -or $isSingleQuoted) {
                $value = $value.Substring(1, $value.Length - 2)
            }
        }

        $result[$name] = $value
    }

    return $result
}

$resolvedEnvFile = (Resolve-Path -LiteralPath $EnvFile).Path
$values = Read-DotEnvValues -Path $resolvedEnvFile

function Set-CanonicalValueFromAliases {
    param(
        [Parameter(Mandatory)][hashtable]$Values,
        [Parameter(Mandatory)][string]$CanonicalName,
        [Parameter(Mandatory)][string[]]$Aliases
    )

    foreach ($candidate in @($CanonicalName) + $Aliases) {
        if ($Values.ContainsKey($candidate) -and
            -not [string]::IsNullOrWhiteSpace([string]$Values[$candidate])) {
            $Values[$CanonicalName] = [string]$Values[$candidate]
            return
        }
    }
}

Set-CanonicalValueFromAliases -Values $values -CanonicalName 'TURN_KEY_ID' `
    -Aliases @('Turn_Token_ID')
Set-CanonicalValueFromAliases -Values $values -CanonicalName 'TURN_KEY_API_TOKEN' `
    -Aliases @('Token_Secret', 'Toekn_Secret')

if (-not $values.ContainsKey('CLIENT_ACCESS_KEY') -or
    [string]::IsNullOrWhiteSpace([string]$values['CLIENT_ACCESS_KEY'])) {
    $projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
    $gameConfigPath = Join-Path $projectRoot 'Config\DefaultGame.ini'
    if (Test-Path -LiteralPath $gameConfigPath -PathType Leaf) {
        $gameConfig = [IO.File]::ReadAllText($gameConfigPath)
        $accessKeyMatch = [regex]::Match($gameConfig, '(?m)^ProviderAccessKey=(\S+)\s*$')
        if ($accessKeyMatch.Success) {
            $values['CLIENT_ACCESS_KEY'] = $accessKeyMatch.Groups[1].Value
        }
    }
}

$requiredNames = @(
    'CLIENT_ACCESS_KEY',
    'TURN_KEY_ID',
    'TURN_KEY_API_TOKEN'
)

foreach ($name in $requiredNames) {
    if (-not $values.ContainsKey($name) -or [string]::IsNullOrWhiteSpace([string]$values[$name])) {
        throw "Missing or empty $name in $resolvedEnvFile"
    }
}

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$tempFile = [IO.Path]::GetFullPath(
    (Join-Path $tempRoot ("webrtc4unreal-worker-secrets-{0}.json" -f [guid]::NewGuid().ToString('N')))
)

if (-not $tempFile.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to create a secret file outside the system temporary directory.'
}

$workerDirectory = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
try {
    $secretPayload = [ordered]@{
        TURN_KEY_ID = [string]$values['TURN_KEY_ID']
        TURN_KEY_API_TOKEN = [string]$values['TURN_KEY_API_TOKEN']
    }
    foreach ($optionalName in @('CALLS_APP_ID', 'CALLS_APP_SECRET', 'CLIENT_ACCESS_KEY')) {
        if ($values.ContainsKey($optionalName) -and
            -not [string]::IsNullOrWhiteSpace([string]$values[$optionalName])) {
            $secretPayload[$optionalName] = [string]$values[$optionalName]
        }
    }
    $secretPayload = $secretPayload | ConvertTo-Json -Compress

    [IO.File]::WriteAllText($tempFile, $secretPayload, [Text.UTF8Encoding]::new($false))

    Push-Location -LiteralPath $workerDirectory
    try {
        if ($Deploy) {
            & npx.cmd wrangler deploy --secrets-file $tempFile
        }
        else {
            & npx.cmd wrangler secret bulk $tempFile
        }
        if ($LASTEXITCODE -ne 0) {
            $operation = if ($Deploy) { 'deploy --secrets-file' } else { 'secret bulk' }
            throw "wrangler $operation failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        Pop-Location
    }
}
finally {
    $secretPayload = $null
    $values.Clear()
    if ([IO.File]::Exists($tempFile)) {
        [IO.File]::Delete($tempFile)
    }
}

Write-Output $(if ($Deploy) {
    'Direct P2P Worker deployed with bootstrap and TURN secrets.'
} else {
    'Direct P2P bootstrap and TURN Worker secrets updated.'
})
Write-Output 'Temporary secret file removed.'
