# Deploy BlueSpin Pi to a Raspberry Pi 3B using credentials from pi/deploy/secrets.env
$ErrorActionPreference = "Stop"

$DeployDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$PiDir = Split-Path -Parent $DeployDir
$SecretsFile = Join-Path $DeployDir "secrets.env"

if (-not (Test-Path $SecretsFile)) {
    throw "Missing $SecretsFile. Copy secrets.env.example to secrets.env and fill in PI_HOST, PI_USER, and auth."
}

$EnvMap = @{}
Get-Content $SecretsFile | ForEach-Object {
    $line = $_.Trim()
    if ($line -and -not $line.StartsWith("#") -and $line.Contains("=")) {
        $k, $v = $line.Split("=", 2)
        $EnvMap[$k.Trim()] = $v.Trim()
    }
}

$PiHost = $EnvMap["PI_HOST"]
$PiPort = if ($EnvMap["PI_PORT"]) { $EnvMap["PI_PORT"] } else { "22" }
$PiUser = if ($EnvMap["PI_USER"]) { $EnvMap["PI_USER"] } else { "pi" }
$SshKey = $EnvMap["PI_SSH_KEY"]
$PiPassword = $EnvMap["PI_PASSWORD"]

if (-not $PiHost -or $PiHost -eq "192.168.1.xxx") {
    throw "Set PI_HOST in $SecretsFile before deploying."
}

$Target = "${PiUser}@${PiHost}"

function Invoke-Remote {
    param([string]$Cmd)
    if ($SshKey) {
        & ssh -p $PiPort -i $SshKey -o StrictHostKeyChecking=accept-new $Target $Cmd
    } elseif ($PiPassword) {
        & plink -batch -ssh -P $PiPort -pw $PiPassword $Target $Cmd
    } else {
        & ssh -p $PiPort -o StrictHostKeyChecking=accept-new $Target $Cmd
    }
    if ($LASTEXITCODE -ne 0) { throw "Remote command failed ($LASTEXITCODE): $Cmd" }
}

function Copy-ToRemote {
    param([string]$LocalPath, [string]$RemotePath)
    if ($SshKey) {
        & scp -P $PiPort -i $SshKey -o StrictHostKeyChecking=accept-new -r $LocalPath "${Target}:${RemotePath}"
    } elseif ($PiPassword) {
        & pscp -batch -P $PiPort -pw $PiPassword -r $LocalPath "${Target}:${RemotePath}"
    } else {
        & scp -P $PiPort -o StrictHostKeyChecking=accept-new -r $LocalPath "${Target}:${RemotePath}"
    }
    if ($LASTEXITCODE -ne 0) { throw "SCP failed ($LASTEXITCODE)" }
}

# Build temporary bluespin.env from BLUESPIN_* keys in secrets.env
$TmpEnv = Join-Path $env:TEMP "bluespin.env"
$EnvLines = @()
foreach ($k in $EnvMap.Keys) {
    if ($k.StartsWith("BLUESPIN_")) {
        $EnvLines += "$k=$($EnvMap[$k])"
    }
}
$EnvLines -join "`n" | Set-Content -Path $TmpEnv -NoNewline -Encoding Ascii

$RemoteBase = "bluespin-pi"
Write-Host "Uploading pi/ to ${Target}:~/${RemoteBase} ..."
Invoke-Remote "rm -rf $RemoteBase && mkdir -p $RemoteBase"
Copy-ToRemote "$PiDir\bluespin_pi" "$RemoteBase"
Copy-ToRemote "$PiDir\scripts" "$RemoteBase"
Copy-ToRemote "$PiDir\systemd" "$RemoteBase"
Copy-ToRemote "$PiDir\pyproject.toml" "$RemoteBase/pyproject.toml"
Copy-ToRemote "$PiDir\README.md" "$RemoteBase/README.md"
Copy-ToRemote $TmpEnv "$RemoteBase/bluespin.env"
Remove-Item -Force -ErrorAction SilentlyContinue $TmpEnv

Write-Host "Running install.sh on ${Target} ..."
$SudoPrefix = if ($PiPassword) { "echo '$PiPassword' | sudo -S" } else { "sudo" }
Invoke-Remote "chmod +x $RemoteBase/scripts/install.sh && $SudoPrefix mkdir -p /etc/bluespin && $SudoPrefix mv $RemoteBase/bluespin.env /etc/bluespin/bluespin.env && $SudoPrefix bash $RemoteBase/scripts/install.sh"
