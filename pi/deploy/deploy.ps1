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

if (-not $PiHost -or $PiHost -eq "192.168.1.xxx") {
    throw "Set PI_HOST in $SecretsFile before deploying."
}

$SshArgs = @("-p", $PiPort, "-o", "StrictHostKeyChecking=accept-new")
$ScpArgs = @("-P", $PiPort, "-o", "StrictHostKeyChecking=accept-new")
if ($SshKey) {
    $SshArgs += @("-i", $SshKey)
    $ScpArgs += @("-i", $SshKey)
}

$Target = "${PiUser}@${PiHost}"
Write-Host "Uploading pi/ to ${Target}:~/bluespin-pi ..."
& ssh @SshArgs $Target "rm -rf ~/bluespin-pi && mkdir -p ~/bluespin-pi"
& scp @ScpArgs -r "$PiDir/*" "${Target}:~/bluespin-pi/"

Write-Host "Running install.sh on ${Target} ..."
& ssh @SshArgs $Target "chmod +x ~/bluespin-pi/scripts/install.sh && sudo bash ~/bluespin-pi/scripts/install.sh"
