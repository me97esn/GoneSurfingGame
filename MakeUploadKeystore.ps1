# ==========================================================
#  MakeUploadKeystore.ps1
#  Create the Android UPLOAD key this project signs release builds with, and
#  wire it into the config PackageAndroidRelease.bat reads.
#
#  Run it once, interactively. It asks for a password and never stores one in
#  a file you can commit:
#    - the keystore lands in GoneSurfing\Build\Android\, which .gitignore
#      already excludes in full (only res\ is tracked there);
#    - the passwords land in GoneSurfing\Config\Android\AndroidEngine.ini,
#      which is gitignored by a rule added alongside this script.
#
#  Upload key vs app signing key: with Play App Signing (the default for new
#  apps) Google holds the key that actually signs what users install. This one
#  only proves uploads are from you. Losing it is recoverable - Play support can
#  reset it - but back it up anyway, because a reset costs days.
#
#  Exit codes: 0 created, 2 setup error, 3 user cancelled.
# ==========================================================

param(
    [string]$Alias = "gonesurfing-upload",
    [string]$FileName = "gonesurfing-upload.jks",
    # Goes in the certificate. Play shows none of it to users; it only has to be
    # stable, because a re-created key with different details is a different key.
    [string]$CommonName = "Emil Stenberg",
    [string]$Organization = "Gone Surfing",
    [string]$Country = "SE",
    # 10000 days ~ 27 years. Play requires a certificate valid past 2033.
    [int]$ValidityDays = 10000
)

$ErrorActionPreference = 'Stop'

$ProjectDir  = Join-Path $PSScriptRoot "GoneSurfing"
$KeystoreDir = Join-Path $ProjectDir "Build\Android"
$KeystorePath = Join-Path $KeystoreDir $FileName
$ConfigDir   = Join-Path $ProjectDir "Config\Android"
$ConfigPath  = Join-Path $ConfigDir "AndroidEngine.ini"

# --- find keytool ------------------------------------------------------------
# Android Studio's bundled JBR is what this machine builds with; JAVA_HOME is the
# fallback for a machine that has a JDK but not Studio.
$KeytoolCandidates = @(
    "C:\Program Files\Android\Android Studio\jbr\bin\keytool.exe",
    "$env:LOCALAPPDATA\Programs\Android Studio\jbr\bin\keytool.exe"
)
if ($env:JAVA_HOME) { $KeytoolCandidates += (Join-Path $env:JAVA_HOME "bin\keytool.exe") }

$Keytool = $null
foreach ($c in $KeytoolCandidates) {
    if (Test-Path $c) { $Keytool = $c; break }
}
if (-not $Keytool) {
    $cmd = Get-Command keytool -ErrorAction SilentlyContinue
    if ($cmd) { $Keytool = $cmd.Source }
}
if (-not $Keytool) {
    Write-Host "ERROR: keytool.exe not found. Looked in Android Studio's JBR, JAVA_HOME and PATH."
    exit 2
}

Write-Host "=== Upload keystore ==="
Write-Host "  - keytool:  $Keytool"
Write-Host "  - keystore: $KeystorePath"
Write-Host "  - alias:    $Alias"
Write-Host ""

if (Test-Path $KeystorePath) {
    Write-Host "A keystore already exists at that path."
    Write-Host "Overwriting it makes every build signed with the old one un-updatable on Play."
    $answer = Read-Host "Type REPLACE to overwrite, anything else to cancel"
    if ($answer -ne "REPLACE") { Write-Host "Cancelled."; exit 3 }
    Remove-Item $KeystorePath -Force
}

# --- password ----------------------------------------------------------------
# One password for both the store and the key. Play accepts that, and two
# passwords for one key is a way to get locked out, not a security measure.
$pw1 = Read-Host "Keystore password (min 6 chars, put it in your password manager now)" -AsSecureString
$pw2 = Read-Host "Repeat it" -AsSecureString

$b1 = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($pw1)
$b2 = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($pw2)
try {
    $plain1 = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($b1)
    $plain2 = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($b2)
} finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b1)
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b2)
}

if ($plain1 -ne $plain2) { Write-Host "ERROR: the two passwords differ."; exit 3 }
if ($plain1.Length -lt 6) { Write-Host "ERROR: keytool requires at least 6 characters."; exit 3 }

# --- create ------------------------------------------------------------------
if (-not (Test-Path $KeystoreDir)) { New-Item -ItemType Directory -Force -Path $KeystoreDir | Out-Null }

$dname = "CN=$CommonName, O=$Organization, C=$Country"
& $Keytool -genkeypair -v `
    -keystore $KeystorePath `
    -alias $Alias `
    -keyalg RSA -keysize 2048 `
    -validity $ValidityDays `
    -storepass $plain1 -keypass $plain1 `
    -dname $dname

if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: keytool failed with $LASTEXITCODE"; exit 2 }
if (-not (Test-Path $KeystorePath)) { Write-Host "ERROR: keytool reported success but no file exists."; exit 2 }

# --- wire it into the Android platform config --------------------------------
# UE resolves KeyStore relative to <Project>\Build\Android, so the bare filename
# is correct here. These live in the Android platform config rather than
# DefaultEngine.ini purely so the passwords stay out of git.
if (-not (Test-Path $ConfigDir)) { New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null }

$ini = @"
; Android release signing. NOT IN GIT - see .gitignore. Recreate with
; MakeUploadKeystore.ps1 if this file is missing on a new machine.
;
; KeyStore is resolved relative to <Project>\Build\Android, so it is a bare
; filename. PackageAndroidRelease.bat looks for a KeyStore= line in this file
; and in DefaultEngine.ini, and passes -distribution when it finds one.
[/Script/AndroidRuntimeSettings.AndroidRuntimeSettings]
KeyStore=$FileName
KeyAlias=$Alias
KeyStorePassword=$plain1
KeyPassword=$plain1
"@

Set-Content -Path $ConfigPath -Value $ini -Encoding utf8

Write-Host ""
Write-Host "=== Done ==="
Write-Host "  keystore: $KeystorePath"
Write-Host "  config:   $ConfigPath"
Write-Host ""
Write-Host "BACK UP the keystore file and the password, somewhere that is not this machine."
Write-Host "Neither is in git, by design - a fresh clone cannot build a signed release without them."
Write-Host ""
Write-Host "Next: run PackageAndroidRelease.bat. It should now print"
Write-Host "  'Keystore configured: packaging with -distribution.'"
Write-Host "and the .aab it produces is the one you upload to Play."
exit 0
