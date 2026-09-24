<#
.SYNOPSIS
    Collects OBS Studio and Cloud Closed Captions plugin diagnostics into one zip.

.DESCRIPTION
    Gathers the information needed to diagnose Windows plugin problems: OBS
    installs and versions, every copy of the plugin DLL with hashes, a load test
    against the installed OBS runtime, recent OBS logs and crash dumps, and the
    plugin's saved settings with the API key redacted.

    Requires no administrator rights and changes nothing on the machine.

.PARAMETER OutputDir
    Directory to build the report in. Defaults to a folder on the Desktop.

.PARAMETER LogCount
    Number of recent OBS log files to include. Defaults to 3.

.PARAMETER SkipLoadTest
    Skip loading obs.dll and the plugin DLL into this PowerShell process.

.PARAMETER IncludeFullSceneCollections
    Copy whole scene collection JSON files instead of only the plugin's settings
    object. These contain source names, file paths, and other scene data.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File windows_diagnostics.ps1
#>
[CmdletBinding()]
param(
    [string]$OutputDir = (Join-Path ([Environment]::GetFolderPath('Desktop')) ('obs-captions-diagnostics-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))),
    [int]$LogCount = 3,
    [switch]$SkipLoadTest,
    [switch]$IncludeFullSceneCollections
)

$ErrorActionPreference = 'Continue'
$PluginDllName = 'obs_google_caption_plugin.dll'
$SettingsKey = 'cloud_closed_caption_rat'

$AppData = $env:APPDATA
if ([string]::IsNullOrWhiteSpace($AppData)) {
    $AppData = [Environment]::GetFolderPath('ApplicationData')
}
if ([string]::IsNullOrWhiteSpace($AppData)) {
    $AppData = Join-Path ([Environment]::GetFolderPath('UserProfile')) 'AppData\Roaming'
}

# ---------------------------------------------------------------- report file

New-Item -Path $OutputDir -ItemType Directory -Force | Out-Null
$Report = Join-Path $OutputDir 'report.txt'
if (Test-Path -LiteralPath $Report) {
    Remove-Item -LiteralPath $Report -Force
}

function Write-Line([string]$Text = '') {
    Add-Content -Path $Report -Value $Text -Encoding UTF8
    Write-Host $Text
}

function Write-Section([string]$Title) {
    Write-Line
    Write-Line ('=' * 72)
    Write-Line $Title
    Write-Line ('=' * 72)
}

function Format-ObsVersion([uint32]$Packed) {
    # OBS packs versions as (major << 24) | (minor << 16) | patch.
    $major = ($Packed -shr 24) -band 0xFF
    $minor = ($Packed -shr 16) -band 0xFF
    $patch = $Packed -band 0xFFFF
    return ('{0}.{1}.{2} (0x{3:x8})' -f $major, $minor, $patch, $Packed)
}

function Write-FileFacts([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path)) {
        Write-Line ("{0}: NOT FOUND ({1})" -f $Label, $Path)
        return
    }
    $item = Get-Item -LiteralPath $Path
    Write-Line ("{0}: {1}" -f $Label, $item.FullName)
    Write-Line ("    size      : {0} bytes" -f $item.Length)
    Write-Line ("    modified  : {0}" -f $item.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))
    $info = $item.VersionInfo
    if ($info -and $info.FileVersion) {
        Write-Line ("    version   : {0} / product {1}" -f $info.FileVersion, $info.ProductVersion)
    }
    try {
        $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
        Write-Line ("    sha256    : {0}" -f $hash)
    } catch {
        Write-Line ("    sha256    : failed ({0})" -f $_.Exception.Message)
    }
}

# ------------------------------------------------------------------- machine

Write-Section 'Machine'
Write-Line ("collected     : {0}" -f (Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz'))
Write-Line ("script        : {0}" -f $MyInvocation.MyCommand.Path)
Write-Line ("powershell    : {0} (64-bit process: {1})" -f $PSVersionTable.PSVersion, [Environment]::Is64BitProcess)
try {
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    Write-Line ("os            : {0} build {1} ({2})" -f $os.Caption, $os.BuildNumber, $os.OSArchitecture)
} catch {
    Write-Line ("os            : {0}" -f [Environment]::OSVersion.VersionString)
}
try {
    $cpu = Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1
    Write-Line ("cpu           : {0}" -f $cpu.Name.Trim())
} catch { }

# -------------------------------------------------------------- OBS installs

Write-Section 'OBS Studio installations'

$installDirs = New-Object System.Collections.Generic.List[string]
function Add-InstallDir([string]$Dir) {
    if ([string]::IsNullOrWhiteSpace($Dir)) { return }
    $trimmed = $Dir.Trim('"').TrimEnd('\')
    if ((Test-Path -LiteralPath $trimmed) -and ($installDirs -notcontains $trimmed)) {
        $installDirs.Add($trimmed)
    }
}

foreach ($key in @('HKLM:\SOFTWARE\OBS Studio', 'HKLM:\SOFTWARE\WOW6432Node\OBS Studio')) {
    try {
        $item = Get-Item -LiteralPath $key -ErrorAction Stop
        Add-InstallDir ([string]$item.GetValue(''))
    } catch { }
}
foreach ($uninstall in @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*')) {
    try {
        Get-ItemProperty -Path $uninstall -ErrorAction SilentlyContinue |
            Where-Object { $_.DisplayName -like '*OBS Studio*' } |
            ForEach-Object {
                Write-Line ("registry      : {0} {1} -> {2}" -f $_.DisplayName, $_.DisplayVersion, $_.InstallLocation)
                Add-InstallDir ([string]$_.InstallLocation)
            }
    } catch { }
}
foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
    if (-not [string]::IsNullOrWhiteSpace($root)) {
        Add-InstallDir (Join-Path $root 'obs-studio')
    }
}

if ($installDirs.Count -eq 0) {
    Write-Line 'No OBS Studio installation found in the registry or the default folders.'
    Write-Line 'If OBS is installed somewhere else, note the path when you send this report.'
}

$obsBinDirs = New-Object System.Collections.Generic.List[string]
foreach ($dir in $installDirs) {
    Write-Line
    Write-Line ("install dir   : {0}" -f $dir)
    $exe = Join-Path $dir 'bin\64bit\obs64.exe'
    Write-FileFacts $exe '  obs64.exe'
    if (Test-Path -LiteralPath $exe) {
        $obsBinDirs.Add((Join-Path $dir 'bin\64bit'))
    }
    Write-FileFacts (Join-Path $dir 'bin\64bit\libcurl.dll') '  libcurl.dll'
}

# --------------------------------------------------------------- plugin DLLs

Write-Section 'Cloud Closed Captions plugin files'

$pluginPaths = New-Object System.Collections.Generic.List[string]
foreach ($dir in $installDirs) {
    $candidate = Join-Path $dir ('obs-plugins\64bit\' + $PluginDllName)
    if ((Test-Path -LiteralPath $candidate) -and ($pluginPaths -notcontains $candidate)) {
        $pluginPaths.Add($candidate)
    }
}
$userPluginRoot = Join-Path $AppData 'obs-studio\plugins'
if (Test-Path -LiteralPath $userPluginRoot) {
    Get-ChildItem -LiteralPath $userPluginRoot -Recurse -Filter $PluginDllName -ErrorAction SilentlyContinue |
        ForEach-Object {
            if ($pluginPaths -notcontains $_.FullName) { $pluginPaths.Add($_.FullName) }
        }
}

if ($pluginPaths.Count -eq 0) {
    Write-Line ("{0} was not found in any OBS plugin folder." -f $PluginDllName)
    Write-Line 'The plugin is not installed, or it was copied to the wrong directory.'
    Write-Line ('Expected: <OBS install>\obs-plugins\64bit\' + $PluginDllName)
} else {
    if ($pluginPaths.Count -gt 1) {
        Write-Line ("WARNING: {0} copies of the plugin found. OBS may load a stale one." -f $pluginPaths.Count)
    }
    $index = 0
    foreach ($path in $pluginPaths) {
        $index++
        Write-Line
        Write-FileFacts $path ("copy #{0}" -f $index)
    }
}

# Anything else the installer or a manual copy may have left behind.
Write-Line
foreach ($dir in $installDirs) {
    $uninstaller = Join-Path $dir 'obs-plugins\64bit\uninstall_cloud_captions.exe'
    if (Test-Path -LiteralPath $uninstaller) {
        Write-Line ("NSIS uninstaller present: {0}" -f $uninstaller)
    }
}

# ----------------------------------------------------------------- load test

Write-Section 'DLL load test'

if ($SkipLoadTest) {
    Write-Line 'Skipped (-SkipLoadTest).'
} elseif (-not [Environment]::Is64BitProcess) {
    Write-Line 'Skipped: run this script from 64-bit PowerShell to test the x64 plugin.'
} elseif ($pluginPaths.Count -eq 0 -or $obsBinDirs.Count -eq 0) {
    Write-Line 'Skipped: need both an OBS installation and a plugin DLL.'
} else {
    $source = @'
using System;
using System.Runtime.InteropServices;

public static class ObsLoadProbe {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr LoadLibraryExW(string fileName, IntPtr file, uint flags);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool SetDllDirectoryW(string path);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    public static extern IntPtr GetProcAddress(IntPtr module, string name);

    private delegate uint VersionFunc();

    public static IntPtr Load(string path, out int error) {
        // 0x8 = LOAD_WITH_ALTERED_SEARCH_PATH: also search the DLL's own folder.
        IntPtr handle = LoadLibraryExW(path, IntPtr.Zero, 0x8);
        error = handle == IntPtr.Zero ? Marshal.GetLastWin32Error() : 0;
        return handle;
    }

    public static uint Version(IntPtr module, string name) {
        IntPtr proc = GetProcAddress(module, name);
        if (proc == IntPtr.Zero) {
            return 0;
        }
        VersionFunc func = (VersionFunc) Marshal.GetDelegateForFunctionPointer(proc, typeof(VersionFunc));
        return func();
    }
}
'@
    try {
        if (-not ([System.Management.Automation.PSTypeName]'ObsLoadProbe').Type) {
            Add-Type -TypeDefinition $source -Language CSharp
        }

        $obsBin = $obsBinDirs[0]
        Write-Line ("runtime       : {0}" -f $obsBin)
        [void][ObsLoadProbe]::SetDllDirectoryW($obsBin)

        $obsError = 0
        $obsHandle = [ObsLoadProbe]::Load((Join-Path $obsBin 'obs.dll'), [ref]$obsError)
        if ($obsHandle -eq [IntPtr]::Zero) {
            Write-Line ("obs.dll       : FAILED to load, error {0} ({1})" -f $obsError, (New-Object System.ComponentModel.Win32Exception($obsError)).Message)
        } else {
            $obsVersion = [ObsLoadProbe]::Version($obsHandle, 'obs_get_version')
            Write-Line ("obs.dll       : loaded, obs_get_version = {0}" -f (Format-ObsVersion $obsVersion))

            foreach ($path in $pluginPaths) {
                $pluginError = 0
                $pluginHandle = [ObsLoadProbe]::Load($path, [ref]$pluginError)
                if ($pluginHandle -eq [IntPtr]::Zero) {
                    $message = (New-Object System.ComponentModel.Win32Exception($pluginError)).Message
                    Write-Line ("plugin        : FAILED to load {0}" -f $path)
                    Write-Line ("    error     : {0} ({1})" -f $pluginError, $message)
                    if ($pluginError -eq 126) {
                        Write-Line '    meaning   : a DLL the plugin imports is missing from this OBS runtime.'
                    } elseif ($pluginError -eq 193) {
                        Write-Line '    meaning   : wrong architecture (a 32-bit DLL in a 64-bit OBS).'
                    }
                } else {
                    $moduleVersion = [ObsLoadProbe]::Version($pluginHandle, 'obs_module_ver')
                    Write-Line ("plugin        : loaded {0}" -f $path)
                    Write-Line ("    obs_module_ver = {0}" -f (Format-ObsVersion $moduleVersion))
                    if ($moduleVersion -ne $obsVersion) {
                        Write-Line '    MISMATCH  : built against a different OBS version; OBS will refuse to load it.'
                        Write-Line '                Install the plugin package matching Help > About.'
                    } else {
                        Write-Line '    match     : plugin API version equals this OBS runtime.'
                    }
                }
            }
        }
        [void][ObsLoadProbe]::SetDllDirectoryW($null)
    } catch {
        Write-Line ("Load test failed to run: {0}" -f $_.Exception.Message)
    }
}

# ----------------------------------------------------------------- OBS logs

Write-Section 'OBS logs'

$logSource = Join-Path $AppData 'obs-studio\logs'
$logDest = Join-Path $OutputDir 'logs'
$newestLog = $null
if (Test-Path -LiteralPath $logSource) {
    New-Item -Path $logDest -ItemType Directory -Force | Out-Null
    $logs = @(Get-ChildItem -LiteralPath $logSource -Filter '*.txt' |
        Sort-Object LastWriteTime -Descending | Select-Object -First $LogCount)
    foreach ($log in $logs) {
        Copy-Item -LiteralPath $log.FullName -Destination $logDest -Force
        Write-Line ("copied        : {0} ({1} bytes, {2})" -f $log.Name, $log.Length, $log.LastWriteTime)
    }
    $newestLog = $logs | Select-Object -First 1
    if ($logs.Count -eq 0) { Write-Line 'No log files in the OBS logs folder.' }
} else {
    Write-Line ("OBS log folder not found: {0}" -f $logSource)
}

$crashSource = Join-Path $AppData 'obs-studio\crashes'
if (Test-Path -LiteralPath $crashSource) {
    $crashes = @(Get-ChildItem -LiteralPath $crashSource -Filter '*.txt' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 2)
    if ($crashes.Count -gt 0) {
        $crashDest = Join-Path $OutputDir 'crashes'
        New-Item -Path $crashDest -ItemType Directory -Force | Out-Null
        foreach ($crash in $crashes) {
            Copy-Item -LiteralPath $crash.FullName -Destination $crashDest -Force
            Write-Line ("crash log     : {0} ({1})" -f $crash.Name, $crash.LastWriteTime)
        }
    } else {
        Write-Line 'No crash logs (good).'
    }
} else {
    Write-Line 'No crashes folder (good).'
}

# --------------------------------------------------- plugin lines in the log

Write-Section 'Plugin lines from the newest log'

if ($newestLog) {
    $pattern = 'captions_plugin|Deepgram|obs_google_caption_plugin|Cloud Closed Captions|Failed to load|LoadLibrary|Module .* not loaded'
    $pluginLines = @(Select-String -LiteralPath $newestLog.FullName -Pattern $pattern)
    $extract = Join-Path $OutputDir 'plugin_log_extract.txt'
    if ($pluginLines.Count -gt 0) {
        $pluginLines | ForEach-Object { $_.Line } | Set-Content -LiteralPath $extract -Encoding UTF8
        Write-Line ("{0} matching lines written to plugin_log_extract.txt" -f $pluginLines.Count)
        Write-Line 'First and last few:'
        $pluginLines | Select-Object -First 15 | ForEach-Object { Write-Line ('  ' + $_.Line) }
        if ($pluginLines.Count -gt 25) { Write-Line '  ...' }
        if ($pluginLines.Count -gt 15) {
            $pluginLines | Select-Object -Last 10 | ForEach-Object { Write-Line ('  ' + $_.Line) }
        }
    } else {
        Write-Line 'No plugin lines at all in the newest log.'
        Write-Line 'OBS never loaded the module, or this log is from a run without the plugin installed.'
    }
} else {
    Write-Line 'No log to scan.'
}

# ------------------------------------------------------------------ settings

Write-Section 'Plugin settings (API key redacted)'

function Protect-Settings($Settings) {
    if ($null -eq $Settings) { return $null }
    foreach ($name in @('custom_api_key')) {
        $property = $Settings.PSObject.Properties[$name]
        if ($property) {
            $length = 0
            if ($property.Value) { $length = ([string]$property.Value).Length }
            $property.Value = ("<redacted, {0} characters>" -f $length)
        }
    }
    return $Settings
}

$settingsDest = Join-Path $OutputDir 'settings'
New-Item -Path $settingsDest -ItemType Directory -Force | Out-Null

$sceneDir = Join-Path $AppData 'obs-studio\basic\scenes'
if (Test-Path -LiteralPath $sceneDir) {
    foreach ($scene in (Get-ChildItem -LiteralPath $sceneDir -Filter '*.json')) {
        try {
            $data = Get-Content -LiteralPath $scene.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
            $pluginSettings = $data.PSObject.Properties[$SettingsKey]
            if (-not $pluginSettings) {
                Write-Line ("{0}: no plugin settings saved in this scene collection" -f $scene.Name)
                continue
            }
            if ($IncludeFullSceneCollections) {
                $pluginSettings.Value = Protect-Settings $pluginSettings.Value
                $out = Join-Path $settingsDest ('scene_' + $scene.Name)
                $data | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $out -Encoding UTF8
            } else {
                $out = Join-Path $settingsDest ('captions_' + $scene.Name)
                (Protect-Settings $pluginSettings.Value) | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $out -Encoding UTF8
            }
            Write-Line ("{0}: plugin settings exported" -f $scene.Name)
            $provider = $pluginSettings.Value.speech_api_provider
            $enabled = $pluginSettings.Value.enabled
            Write-Line ("    enabled = {0}, speech_api_provider = {1} (0 Google, 1 Deepgram, 2 Local WebSocket)" -f $enabled, $provider)
            if ($pluginSettings.Value.deepgram_model) {
                Write-Line ("    deepgram_model = {0}" -f $pluginSettings.Value.deepgram_model)
            }
            if ($pluginSettings.Value.local_ws_server_url) {
                Write-Line ("    local_ws_server_url = {0}" -f $pluginSettings.Value.local_ws_server_url)
            }
        } catch {
            Write-Line ("{0}: could not parse ({1})" -f $scene.Name, $_.Exception.Message)
        }
    }
} else {
    Write-Line ("Scene collection folder not found: {0}" -f $sceneDir)
}

$configFile = Join-Path $AppData 'obs-studio\plugin_config\obs_google_caption_plugin\cloud_closed_captions_settings.json'
if (Test-Path -LiteralPath $configFile) {
    try {
        $data = Get-Content -LiteralPath $configFile -Raw -Encoding UTF8 | ConvertFrom-Json
        (Protect-Settings $data) | ConvertTo-Json -Depth 20 |
            Set-Content -LiteralPath (Join-Path $settingsDest 'plugin_config_settings.json') -Encoding UTF8
        Write-Line 'plugin_config settings file exported.'
    } catch {
        Write-Line ("plugin_config settings file could not be parsed: {0}" -f $_.Exception.Message)
    }
}

# -------------------------------------------------------------------- audio

Write-Section 'Audio devices'
try {
    Get-CimInstance Win32_SoundDevice -ErrorAction Stop |
        ForEach-Object { Write-Line ("  {0} [{1}]" -f $_.Name, $_.Status) }
} catch {
    Write-Line 'Could not enumerate audio devices.'
}

# ----------------------------------------------------------------- packaging

Write-Section 'Done'

$zipPath = Join-Path (Split-Path -Parent $OutputDir) ((Split-Path -Leaf $OutputDir) + '.zip')
try {
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Compress-Archive -Path (Join-Path $OutputDir '*') -DestinationPath $zipPath -Force
    Write-Host ''
    Write-Host ("Report folder: {0}" -f $OutputDir)
    Write-Host ("Zip to send  : {0}" -f $zipPath)
} catch {
    Write-Host ''
    Write-Host ("Could not create the zip ({0}). Send the folder instead: {1}" -f $_.Exception.Message, $OutputDir)
}

Write-Host ''
Write-Host 'The API key is redacted, but OBS logs contain machine and device names.'
Write-Host 'Open report.txt and skim it before sharing.'
