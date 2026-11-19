<#
PenguinScreen2 - Windows VR session launcher.

Starts the VR build (pcsx2-qt-vr.exe, beside this script) with VR ARMED
EXPLICITLY for this one launch (--vr). Nothing else arms VR: no environment
variable, no setting. The flat pcsx2-qt.exe is never touched by this script.

SteamVR is the only supported OpenXR runtime on Windows. The emulator speaks
OpenXR, so the loader binds whatever runtime is active: XR_RUNTIME_JSON when
that variable is set, otherwise HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime
(64-bit view). This script reads the same thing the loader will, names the
runtime it finds, and WARNS - it does not refuse - when it is not SteamVR, so
an unsupported runtime is a visible choice rather than a silent surprise. To
make SteamVR the active runtime: SteamVR -> Settings -> Developer -> "Set
SteamVR as OpenXR runtime".

    launch-vr-session.ps1 [game path] [emulator arguments...]
    launch-vr-session.ps1 -CheckOnly            report the runtime, launch nothing
    launch-vr-session.ps1 -Probe <manifest>     classify a manifest path (test hook)
    launch-vr-session.ps1 -Exe <path>           launch this file instead (test hook)
    launch-vr-session.ps1 -NoPause              never wait for Enter at the end

Every argument that is not one of the named options above goes to the
emulator unchanged, so a game path (spaces included) or a dragged-and-dropped
file works. Double-click launch-vr-session.bat if the execution policy blocks
.ps1 files.
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Probe,
    [switch]$CheckOnly,
    [switch]$NoPause,
    [string]$Exe,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$GameArgs
)

$ErrorActionPreference = 'Stop'
$VrExe = 'pcsx2-qt-vr.exe'
$RegSubKey = 'SOFTWARE\Khronos\OpenXR\1'

function Get-OpenXrRuntimeName {
    # The manifest's own runtime.name when the file is readable; otherwise the
    # best guess the path allows. Known vendors are named so the warning says
    # what was found, not just "not SteamVR".
    param([string]$Path)
    if ($Path -and (Test-Path -LiteralPath $Path)) {
        try {
            $manifest = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
            if ($manifest.runtime -and $manifest.runtime.name) { return [string]$manifest.runtime.name }
        } catch { }
    }
    $lower = $Path.ToLowerInvariant()
    if ($lower -match 'steamvr|steamxr')        { return 'SteamVR' }
    if ($lower -match 'oculus')                 { return 'Meta (Oculus) OpenXR runtime' }
    if ($lower -match 'mixedreality')           { return 'Windows Mixed Reality' }
    if ($lower -match 'virtualdesktop')         { return 'Virtual Desktop XR' }
    if ($lower -match 'varjo')                  { return 'Varjo OpenXR' }
    if ($lower -match 'pico')                   { return 'PICO OpenXR' }
    if ($lower -match 'monado')                 { return 'Monado' }
    return (Split-Path -Leaf $Path)
}

function Test-OpenXrRuntime {
    # Kind: steamvr | other | none. The decision the warning hangs on.
    param([string]$Path)
    if (-not $Path) {
        return [pscustomobject]@{ Kind = 'none'; Name = ''; Path = '' }
    }
    $name = Get-OpenXrRuntimeName -Path $Path
    $isSteamVr = ($name -match '(?i)steamvr') -or ($Path -match '(?i)steamvr|steamxr')
    $kind = if ($isSteamVr) { 'steamvr' } else { 'other' }
    return [pscustomobject]@{ Kind = $kind; Name = $name; Path = $Path }
}

function Read-ActiveRuntime {
    # What the OpenXR loader will bind, in the loader's own order: the
    # XR_RUNTIME_JSON override first, then the 64-bit registry view. The
    # explicit Registry64 view matters: a 32-bit PowerShell is otherwise
    # redirected to WOW6432Node, where no runtime registers itself.
    if ($env:XR_RUNTIME_JSON) {
        return [pscustomobject]@{ Path = [string]$env:XR_RUNTIME_JSON
                                  Source = 'XR_RUNTIME_JSON (environment; the loader obeys it before the registry)' }
    }
    $value = ''
    try {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
        $key = $base.OpenSubKey($RegSubKey)
        if ($key) {
            $raw = $key.GetValue('ActiveRuntime')
            if ($raw) { $value = [string]$raw }
            $key.Close()
        }
        $base.Close()
    } catch { }
    return [pscustomobject]@{ Path = $value; Source = "HKLM\$RegSubKey\ActiveRuntime (64-bit view)" }
}

function Format-CommandArg {
    # One argument, quoted the way the Windows command line expects when it
    # carries spaces or quotes; untouched otherwise.
    param([string]$Arg)
    if ($Arg -match '[\s"]') { return '"' + ($Arg -replace '"', '\"') + '"' }
    return $Arg
}

function Wait-ForEnter {
    param([string]$Why)
    if ($NoPause) { return }
    try { [void](Read-Host "$Why Press Enter to close") } catch { }
}

# Test hook: classify a manifest path and exit. Lets the decision be checked
# without a registry, a headset, or SteamVR installed.
if ($PSBoundParameters.ContainsKey('Probe')) {
    $r = Test-OpenXrRuntime -Path $Probe
    Write-Output ("KIND={0} NAME={1}" -f $r.Kind, $r.Name)
    exit 0
}

$exe = if ($Exe) { $Exe } else { Join-Path $PSScriptRoot $VrExe }
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host "!! $VrExe not found beside this script ($PSScriptRoot)."
    Write-Host "!! This launcher starts the VR build only; it does not run pcsx2-qt.exe."
    Wait-ForEnter -Why 'Nothing was launched.'
    exit 1
}

$active = Read-ActiveRuntime
$runtime = Test-OpenXrRuntime -Path $active.Path
switch ($runtime.Kind) {
    'steamvr' {
        Write-Host ">> Active OpenXR runtime: SteamVR ($($runtime.Path))"
        Write-Host ">>   read from $($active.Source)"
    }
    'other' {
        Write-Host "!! WARNING: the active OpenXR runtime is '$($runtime.Name)'"
        Write-Host "!!          ($($runtime.Path)), read from $($active.Source)."
        Write-Host "!! SteamVR is the only supported runtime for this build. Other runtimes may"
        Write-Host "!! start. To switch: SteamVR -> Settings ->"
        Write-Host "!! Developer -> 'Set SteamVR as OpenXR runtime'. Continuing anyway."
    }
    'none' {
        Write-Host "!! WARNING: no active OpenXR runtime is registered"
        Write-Host "!!          ($($active.Source) is missing)."
        Write-Host "!! Install SteamVR and set it as the OpenXR runtime (SteamVR -> Settings ->"
        Write-Host "!! Developer). Without a runtime the game runs flat. Continuing anyway."
    }
}

if ($CheckOnly) { exit 0 }

# --vr is the one and only thing that arms VR, per launch. Everything the
# user passed follows it, quoted per argument. The emulator is a GUI process,
# so a plain call would return at once with no exit code: wait for it and
# hand its exit code back to the .bat / the caller.
$extra = @()
if ($GameArgs) { $extra = @($GameArgs) }
$argLine = (@('--vr') + $extra | ForEach-Object { Format-CommandArg -Arg $_ }) -join ' '
Write-Host ">> Launching $(Split-Path -Leaf $exe) $argLine"
$proc = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory (Split-Path -Parent $exe) -Wait -PassThru
$rc = [int]$proc.ExitCode
if ($rc -ne 0) {
    Write-Host "!! $(Split-Path -Leaf $exe) exited with code $rc."
    Wait-ForEnter -Why 'The emulator reported an error.'
} elseif ($runtime.Kind -ne 'steamvr') {
    Wait-ForEnter -Why 'Read the runtime warning above.'
}
exit $rc
