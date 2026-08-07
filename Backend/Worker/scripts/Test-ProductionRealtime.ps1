[CmdletBinding()]
param(
    [string]$WorkerUrl = 'https://webrtc4unreal-realtime.fasutotesuto.workers.dev',
    [string]$EnvFile = (Join-Path $PSScriptRoot '..\..\..\.env')
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
if (-not $values.ContainsKey('CLIENT_ACCESS_KEY') -or
    [string]::IsNullOrWhiteSpace([string]$values['CLIENT_ACCESS_KEY'])) {
    throw "Missing or empty CLIENT_ACCESS_KEY in $resolvedEnvFile"
}

$baseUrl = $WorkerUrl.TrimEnd('/')
$room = $null
$participantToken = $null
try {
    $roomResponse = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$baseUrl/v1/rooms" -Headers @{
        'X-P2P-Bootstrap-Key' = [string]$values['CLIENT_ACCESS_KEY']
        Accept = 'application/json'
    } -ContentType 'application/json' -Body '{"room_name":"Production credential test","max_participants":3}'

    $room = $roomResponse.Content | ConvertFrom-Json
    $participantToken = [string]$room.participant_token
    if ([string]::IsNullOrWhiteSpace([string]$room.room_id) -or
        [string]::IsNullOrWhiteSpace([string]$room.participant_id) -or
        [string]::IsNullOrWhiteSpace($participantToken)) {
        throw 'Worker room response is missing participant credentials.'
    }

    Write-Output ('ROOM_CREATE_STATUS=' + [int]$roomResponse.StatusCode)
    Write-Output ('ROOM_PROTOCOL=' + [string]$room.protocol)

    $sessionUrl = "$baseUrl/v1/rooms/$($room.room_id)/participants/$($room.participant_id)/session"
    $sessionResponse = Invoke-WebRequest -UseBasicParsing -Method Post -Uri $sessionUrl -Headers @{
        Authorization = 'Bearer ' + $participantToken
        Accept = 'application/json'
    } -ContentType 'application/json' -Body '{}'
    $session = $sessionResponse.Content | ConvertFrom-Json

    Write-Output ('REALTIME_SESSION_STATUS=' + [int]$sessionResponse.StatusCode)
    Write-Output ('REALTIME_SESSION_ID_PRESENT=' + (-not [string]::IsNullOrWhiteSpace([string]$session.sessionId)))
}
finally {
    if ($null -ne $room -and -not [string]::IsNullOrWhiteSpace($participantToken)) {
        $deleteUrl = "$baseUrl/v1/rooms/$($room.room_id)/participants/$($room.participant_id)"
        try {
            $deleteResponse = Invoke-WebRequest -UseBasicParsing -Method Delete -Uri $deleteUrl -Headers @{
                Authorization = 'Bearer ' + $participantToken
                Accept = 'application/json'
            }
            Write-Output ('PARTICIPANT_CLEANUP_STATUS=' + [int]$deleteResponse.StatusCode)
        }
        catch {
            Write-Warning 'Production test participant cleanup failed.'
        }
    }
    $participantToken = $null
    $values.Clear()
}
