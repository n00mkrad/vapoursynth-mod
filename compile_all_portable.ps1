[CmdletBinding()]
param(
    [string]$BuildRoot = "build_portable",
    [string]$PythonVersion = "3.14.0",
    [switch]$RefreshLocks,
    [switch]$Clean,
    [switch]$BuildVspipeExeOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Script:RepoRoot = (Resolve-Path $PSScriptRoot).Path

function Write-Step {
    param([Parameter(Mandatory = $true)][string]$Message)
    Write-Host "[portable-build] $Message"
}

function Resolve-AbsolutePath {
    param([Parameter(Mandatory = $true)][string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $Script:RepoRoot $Path))
}

function Ensure-Directory {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        New-Item -Path $Path -ItemType Directory -Force | Out-Null
    }
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = $Script:RepoRoot
    )
    Push-Location $WorkingDirectory
    try {
        & $FilePath @Arguments
        $exitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    if ($exitCode -ne 0) {
        throw "Command failed with exit code ${exitCode}: $FilePath $($Arguments -join ' ')"
    }
}

function Invoke-CheckedCapture {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = $Script:RepoRoot
    )
    Push-Location $WorkingDirectory
    try {
        $output = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    if ($exitCode -ne 0) {
        $combined = ($output | Out-String).Trim()
        throw "Command failed with exit code ${exitCode}: $FilePath $($Arguments -join ' ')`n$combined"
    }
    return @($output)
}

function Invoke-RobocopyMirror {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [string[]]$ExcludeDirectories = @()
    )
    Ensure-Directory -Path $Destination
    $args = @($Source, $Destination, "/MIR", "/R:2", "/W:1", "/NFL", "/NDL", "/NP", "/NJH", "/NJS")
    if ($ExcludeDirectories.Count -gt 0) {
        $args += "/XD"
        $args += $ExcludeDirectories
    }
    & robocopy @args | Out-Null
    $robocopyExitCode = $LASTEXITCODE
    if ($robocopyExitCode -gt 7) {
        throw "Robocopy failed with exit code $robocopyExitCode while mirroring '$Source' to '$Destination'"
    }
}

function Invoke-RobocopyCopy {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    Ensure-Directory -Path $Destination
    $args = @($Source, $Destination, "/E", "/R:2", "/W:1", "/NFL", "/NDL", "/NP", "/NJH", "/NJS")
    & robocopy @args | Out-Null
    $robocopyExitCode = $LASTEXITCODE
    if ($robocopyExitCode -gt 7) {
        throw "Robocopy copy failed with exit code $robocopyExitCode while copying '$Source' to '$Destination'"
    }
}

function ConvertTo-HashtableRecursive {
    param([Parameter(Mandatory = $true)][object]$InputObject)
    if ($null -eq $InputObject) {
        return $null
    }
    if ($InputObject -is [System.Collections.IDictionary]) {
        $dictionary = @{}
        foreach ($key in $InputObject.Keys) {
            $dictionary[$key] = ConvertTo-HashtableRecursive -InputObject $InputObject[$key]
        }
        return $dictionary
    }
    if (($InputObject -is [System.Collections.IEnumerable]) -and -not ($InputObject -is [string])) {
        $items = @()
        foreach ($item in $InputObject) {
            $items += ,(ConvertTo-HashtableRecursive -InputObject $item)
        }
        return $items
    }
    if ($InputObject -is [pscustomobject]) {
        $dictionary = @{}
        foreach ($property in $InputObject.PSObject.Properties) {
            $dictionary[$property.Name] = ConvertTo-HashtableRecursive -InputObject $property.Value
        }
        return $dictionary
    }
    return $InputObject
}

function Get-VersionMacroValue {
    param([Parameter(Mandatory = $true)][string]$VersionFile)
    $line = (Get-Content -LiteralPath $VersionFile -TotalCount 1).Trim()
    if ($line -match "VS_CURRENT_RELEASE\s+([0-9]+)") {
        return $matches[1]
    }
    throw "Could not parse release version from '$VersionFile'"
}

function Get-VersionExtraValue {
    param([Parameter(Mandatory = $true)][string]$VersionExtraFile)
    if (-not (Test-Path -LiteralPath $VersionExtraFile -PathType Leaf)) {
        return ""
    }
    $line = (Get-Content -LiteralPath $VersionExtraFile -TotalCount 1).Trim()
    if ($line -match "VS_CURRENT_RELEASE_EXTRA\s+(.+)$") {
        $extra = $matches[1].Trim().Trim('"')
        return $extra
    }
    throw "Could not parse extra version from '$VersionExtraFile'"
}

function Import-VcVarsEnvironment {
    param([Parameter(Mandatory = $true)][string]$VcVarsAllPath)
    if (-not (Test-Path -LiteralPath $VcVarsAllPath -PathType Leaf)) {
        throw "Build tools environment script not found: $VcVarsAllPath"
    }
    Write-Step "Initializing Visual Studio BuildTools environment"
    $cmdLine = "call `"$VcVarsAllPath`" x64 >nul && set"
    $envLines = Invoke-CheckedCapture -FilePath "cmd.exe" -Arguments @("/d", "/s", "/c", $cmdLine)
    $imported = 0
    foreach ($line in $envLines) {
        if ($line -match "^([^=]+)=(.*)$") {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
            $imported++
        }
    }
    if ($imported -eq 0) {
        throw "Failed to import environment variables from vcvarsall.bat"
    }
}

function Get-PythonExecutablePath {
    param([Parameter(Mandatory = $true)][string]$PythonRoot)
    return Join-Path $PythonRoot "python.exe"
}

function Ensure-PortablePython {
    param(
        [Parameter(Mandatory = $true)][string]$PythonRoot,
        [Parameter(Mandatory = $true)][string]$PythonVersionValue,
        [Parameter(Mandatory = $true)][string]$WorkRoot
    )
    $pythonExe = Get-PythonExecutablePath -PythonRoot $PythonRoot
    $pyVerCompact = $PythonVersionValue.Substring(0, 4).Replace(".", "")
    $pythonImportLib = Join-Path $PythonRoot "libs\python$pyVerCompact.lib"
    if ((Test-Path -LiteralPath $pythonExe -PathType Leaf) -and (Test-Path -LiteralPath $pythonImportLib -PathType Leaf)) {
        Write-Step "Portable Python already exists at $PythonRoot"
        return
    }
    Write-Step "Bootstrapping NuGet Python $PythonVersionValue"
    if (Test-Path -LiteralPath $PythonRoot -PathType Container) {
        Remove-Item -LiteralPath $PythonRoot -Recurse -Force
    }
    Ensure-Directory -Path $PythonRoot
    $tmpDir = Join-Path $WorkRoot "tmp_python_bootstrap"
    Ensure-Directory -Path $tmpDir
    $nupkgPath = Join-Path $tmpDir "python.$PythonVersionValue.nupkg"
    $zipPath = Join-Path $tmpDir "python.$PythonVersionValue.zip"
    $expandedPath = Join-Path $tmpDir "python_nupkg_expanded"
    if (Test-Path -LiteralPath $expandedPath -PathType Container) {
        Remove-Item -LiteralPath $expandedPath -Recurse -Force
    }
    Invoke-WebRequest -Uri "https://www.nuget.org/api/v2/package/python/$PythonVersionValue" -OutFile $nupkgPath
    Copy-Item -LiteralPath $nupkgPath -Destination $zipPath -Force
    Expand-Archive -LiteralPath $zipPath -DestinationPath $expandedPath -Force
    $toolsPath = Join-Path $expandedPath "tools"
    if (-not (Test-Path -LiteralPath $toolsPath -PathType Container)) {
        throw "NuGet Python package extraction failed: missing tools directory in $expandedPath"
    }
    Invoke-RobocopyMirror -Source $toolsPath -Destination $PythonRoot
}

function Write-PinnedConstraintsFile {
    param([Parameter(Mandatory = $true)][string]$ConstraintsPath)
    @(
        "build==1.3.0"
        "Cython==3.1.3"
        "meson==1.9.1"
        "meson-python>=0.18.0"
        "wheel==0.45.1"
        "sphinx==8.2.3"
        "sphinx-intl==2.3.2"
        "sphinx-rtd-theme==3.0.2"
    ) | Set-Content -LiteralPath $ConstraintsPath -Encoding ASCII
}

function Ensure-PinnedPythonPackages {
    param(
        [Parameter(Mandatory = $true)][string]$PythonExe,
        [Parameter(Mandatory = $true)][string]$ConstraintsPath
    )
    Write-Step "Installing pinned Python build packages"
    Invoke-Checked -FilePath $PythonExe -Arguments @("-m", "pip", "install", "--upgrade", "pip", "setuptools")
    Invoke-Checked -FilePath $PythonExe -Arguments @("-m", "pip", "install", "--upgrade", "-r", $ConstraintsPath)
}

function Get-RemoteHeadInfo {
    param([Parameter(Mandatory = $true)][string]$Url)
    $lines = Invoke-CheckedCapture -FilePath "git" -Arguments @("ls-remote", "--symref", $Url, "HEAD")
    $headRef = $null
    $headSha = $null
    foreach ($line in $lines) {
        if ($line -match "^ref:\s+([^\s]+)\s+HEAD$") {
            $headRef = $matches[1]
            continue
        }
        if ($line -match "^([a-f0-9]{40})\s+HEAD$") {
            $headSha = $matches[1]
            continue
        }
    }
    if (-not $headRef -or -not $headSha) {
        throw "Failed to resolve remote HEAD for $Url"
    }
    return @{
        ref = $headRef
        sha = $headSha
    }
}

function Load-OrCreateDependencyLock {
    param(
        [Parameter(Mandatory = $true)][string]$LockPath,
        [Parameter(Mandatory = $true)][hashtable]$RepoMap,
        [Parameter(Mandatory = $true)][bool]$ForceRefresh
    )
    if ((-not $ForceRefresh) -and (Test-Path -LiteralPath $LockPath -PathType Leaf)) {
        Write-Step "Using existing dependency lock: $LockPath"
        $jsonObject = Get-Content -LiteralPath $LockPath -Raw | ConvertFrom-Json
        return (ConvertTo-HashtableRecursive -InputObject $jsonObject)
    }
    Write-Step "Creating dependency lock file"
    $repos = @{}
    foreach ($name in $RepoMap.Keys) {
        $head = Get-RemoteHeadInfo -Url $RepoMap[$name]
        $repos[$name] = @{
            url = $RepoMap[$name]
            ref = $head.ref
            sha = $head.sha
        }
    }
    $lock = @{
        created_utc = [DateTime]::UtcNow.ToString("o")
        repos = $repos
    }
    $lock | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $LockPath -Encoding UTF8
    return $lock
}

function Sync-GitDependency {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][hashtable]$RepoInfo,
        [Parameter(Mandatory = $true)][string]$DepsRoot
    )
    $repoPath = Join-Path $DepsRoot $Name
    $url = [string]$RepoInfo["url"]
    $sha = [string]$RepoInfo["sha"]
    if (-not (Test-Path -LiteralPath (Join-Path $repoPath ".git") -PathType Container)) {
        if (Test-Path -LiteralPath $repoPath) {
            Remove-Item -LiteralPath $repoPath -Recurse -Force
        }
        Write-Step "Cloning $Name"
        Invoke-Checked -FilePath "git" -Arguments @("clone", $url, $repoPath)
    }
    Write-Step "Checking out $Name at $sha"
    $gitArgsPrefix = @("-c", "safe.directory=*")
    Invoke-Checked -FilePath "git" -Arguments ($gitArgsPrefix + @("fetch", "--depth", "1", "origin", $sha)) -WorkingDirectory $repoPath
    Invoke-Checked -FilePath "git" -Arguments ($gitArgsPrefix + @("checkout", "--force", "--detach", $sha)) -WorkingDirectory $repoPath
    Invoke-Checked -FilePath "git" -Arguments ($gitArgsPrefix + @("submodule", "update", "--init", "--recursive")) -WorkingDirectory $repoPath
}

function Copy-LockedDependenciesToMirror {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDepsRoot,
        [Parameter(Mandatory = $true)][string]$MirrorRoot
    )
    $subprojectsRoot = Join-Path $MirrorRoot "subprojects"
    Ensure-Directory -Path $subprojectsRoot
    foreach ($name in @("zimg", "libp2p")) {
        $source = Join-Path $SourceDepsRoot $name
        $destination = Join-Path $subprojectsRoot $name
        if (Test-Path -LiteralPath $destination) {
            Remove-Item -LiteralPath $destination -Recurse -Force
        }
        Write-Step "Overlaying locked dependency $name into source mirror"
        Invoke-RobocopyMirror -Source $source -Destination $destination -ExcludeDirectories @(".git")
        $packagefilesSource = Join-Path $subprojectsRoot "packagefiles\$name"
        if (Test-Path -LiteralPath $packagefilesSource -PathType Container) {
            Write-Step "Applying packagefiles patch overlay for $name"
            Invoke-RobocopyCopy -Source $packagefilesSource -Destination $destination
        }
    }
}

function New-SourceMirror {
    param(
        [Parameter(Mandatory = $true)][string]$MirrorRoot,
        [Parameter(Mandatory = $true)][string]$BuildRootAbsolute
    )
    if (Test-Path -LiteralPath $MirrorRoot) {
        Remove-Item -LiteralPath $MirrorRoot -Recurse -Force
    }
    Write-Step "Creating source mirror in $MirrorRoot"
    Invoke-RobocopyMirror -Source $Script:RepoRoot -Destination $MirrorRoot -ExcludeDirectories @(".git", ".vs", ".vscode", "build_portable", "build", "dist")
    Ensure-Directory -Path (Join-Path $MirrorRoot "dist")
}

$buildRootAbsolute = Resolve-AbsolutePath -Path $BuildRoot
$toolsRoot = Join-Path $buildRootAbsolute "tools"
$pythonRoot = Join-Path $toolsRoot "python"
$locksRoot = Join-Path $buildRootAbsolute "locks"
$depsRoot = Join-Path $buildRootAbsolute "deps"
$workRoot = Join-Path $buildRootAbsolute "work"
$outRoot = Join-Path $buildRootAbsolute "out"
$compiledOutRoot = Join-Path $outRoot "Compiled"
$lockPath = Join-Path $locksRoot "deps.lock.json"
$constraintsPath = Join-Path $locksRoot "python-constraints.txt"

if ($Clean -and (Test-Path -LiteralPath $buildRootAbsolute -PathType Container)) {
    Write-Step "Cleaning build root $buildRootAbsolute"
    Remove-Item -LiteralPath $buildRootAbsolute -Recurse -Force
}

Ensure-Directory -Path $buildRootAbsolute
Ensure-Directory -Path $toolsRoot
Ensure-Directory -Path $locksRoot
Ensure-Directory -Path $depsRoot
Ensure-Directory -Path $workRoot
Ensure-Directory -Path $outRoot
Ensure-Directory -Path $compiledOutRoot

$env:TMP = Join-Path $workRoot "tmp"
$env:TEMP = $env:TMP
$env:PIP_CACHE_DIR = Join-Path $toolsRoot "pip-cache"
$env:PYTHONNOUSERSITE = "1"
$env:GIT_TERMINAL_PROMPT = "0"
Ensure-Directory -Path $env:TMP
Ensure-Directory -Path $env:PIP_CACHE_DIR

$repoMap = @{
    zimg = "https://github.com/sekrit-twc/zimg.git"
    libp2p = "https://github.com/sekrit-twc/libp2p.git"
}
$lockData = Load-OrCreateDependencyLock -LockPath $lockPath -RepoMap $repoMap -ForceRefresh $RefreshLocks.IsPresent
foreach ($repoName in $lockData["repos"].Keys) {
    Sync-GitDependency -Name $repoName -RepoInfo $lockData["repos"][$repoName] -DepsRoot $depsRoot
}

$vcVarsAllPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
Import-VcVarsEnvironment -VcVarsAllPath $vcVarsAllPath

$release = Get-VersionMacroValue -VersionFile (Join-Path $Script:RepoRoot "VAPOURSYNTH_VERSION")
$releaseExtra = Get-VersionExtraValue -VersionExtraFile (Join-Path $Script:RepoRoot "VAPOURSYNTH_VERSION_EXTRA")
$versionString = "$release$releaseExtra"
$wheelName = "VapourSynth-$versionString-cp312-abi3-win_amd64.whl"
$repoDistDir = Join-Path $Script:RepoRoot "dist"
$repoWheelPath = Join-Path $repoDistDir $wheelName

if ($BuildVspipeExeOnly) {
    Write-Step "Building standalone vspipe executable mode"
    Ensure-PortablePython -PythonRoot $pythonRoot -PythonVersionValue $PythonVersion -WorkRoot $workRoot
    Write-PinnedConstraintsFile -ConstraintsPath $constraintsPath
    $portablePythonExe = Get-PythonExecutablePath -PythonRoot $pythonRoot
    Ensure-PinnedPythonPackages -PythonExe $portablePythonExe -ConstraintsPath $constraintsPath
    $env:PATH = "$pythonRoot;$($pythonRoot)\Scripts;$($env:PATH)"
    $sourceMirrorRoot = Join-Path $workRoot "source"
    New-SourceMirror -MirrorRoot $sourceMirrorRoot -BuildRootAbsolute $buildRootAbsolute
    Copy-LockedDependenciesToMirror -SourceDepsRoot $depsRoot -MirrorRoot $sourceMirrorRoot

    if (-not (Test-Path -LiteralPath $repoWheelPath -PathType Leaf)) {
        Write-Step "Wheel not found for vspipe runtime at $repoWheelPath, building fallback wheel"
        Ensure-Directory -Path (Join-Path $sourceMirrorRoot "dist")
        Invoke-Checked -FilePath $portablePythonExe -Arguments @("-m", "build", "--no-isolation", "--wheel", "-Csetup-args=--debug") -WorkingDirectory $sourceMirrorRoot
        $mirrorDistDir = Join-Path $sourceMirrorRoot "dist"
        $builtWheels = @(Get-ChildItem -LiteralPath $mirrorDistDir -Filter "VapourSynth-$versionString-*.whl" -File)
        if ($builtWheels.Count -eq 0) {
            throw "Fallback build did not generate a VapourSynth wheel in $mirrorDistDir"
        }
        foreach ($wheel in $builtWheels) {
            Invoke-Checked -FilePath $portablePythonExe -Arguments @("-m", "wheel", "tags", "--remove", "--python-tag", "cp312", $wheel.Name) -WorkingDirectory $mirrorDistDir
        }
        $mirrorWheelPath = Join-Path $mirrorDistDir $wheelName
        if (-not (Test-Path -LiteralPath $mirrorWheelPath -PathType Leaf)) {
            throw "Fallback wheel build completed but expected wheel is missing: $mirrorWheelPath"
        }
        Ensure-Directory -Path $repoDistDir
        Copy-Item -LiteralPath $mirrorWheelPath -Destination $repoWheelPath -Force
    }

    Write-Step "Installing VapourSynth wheel into bundled Python runtime"
    Invoke-Checked -FilePath $portablePythonExe -Arguments @("-m", "pip", "install", "--no-deps", "--force-reinstall", $repoWheelPath)

    $mesonBuildDir = Join-Path $sourceMirrorRoot "build-vspipe"
    if (Test-Path -LiteralPath $mesonBuildDir -PathType Container) {
        Remove-Item -LiteralPath $mesonBuildDir -Recurse -Force
    }
    Invoke-Checked -FilePath "meson" -Arguments @("setup", $mesonBuildDir, $sourceMirrorRoot, "-Dbuildtype=release", "-Db_ndebug=if-release", "-Db_vscrt=md")
    Invoke-Checked -FilePath "meson" -Arguments @("compile", "-C", $mesonBuildDir, "vspipe")

    $vspipeOutDir = Join-Path $compiledOutRoot "vspipe-R$versionString"
    if (Test-Path -LiteralPath $vspipeOutDir -PathType Container) {
        Remove-Item -LiteralPath $vspipeOutDir -Recurse -Force
    }

    Write-Step "Staging standalone vspipe runtime bundle"
    Invoke-RobocopyMirror -Source $pythonRoot -Destination $vspipeOutDir
    $runtimePackageDir = Join-Path $vspipeOutDir "Lib\site-packages\vapoursynth"
    Ensure-Directory -Path $runtimePackageDir
    $vspipeExePath = Join-Path $mesonBuildDir "vspipe.exe"
    if (-not (Test-Path -LiteralPath $vspipeExePath -PathType Leaf)) {
        throw "Expected vspipe executable missing after build: $vspipeExePath"
    }
    Copy-Item -LiteralPath $vspipeExePath -Destination (Join-Path $runtimePackageDir "vspipe.exe") -Force

    Write-Step "vspipe build completed"
    Write-Step "Output directory: $vspipeOutDir"
    Write-Step "Run executable: $runtimePackageDir\\vspipe.exe"
    return
}

if (-not (Test-Path -LiteralPath $repoWheelPath -PathType Leaf)) {
    Write-Step "Expected wheel not found at $repoWheelPath, starting fallback wheel build"
    Ensure-PortablePython -PythonRoot $pythonRoot -PythonVersionValue $PythonVersion -WorkRoot $workRoot
    Write-PinnedConstraintsFile -ConstraintsPath $constraintsPath
    $portablePythonExe = Get-PythonExecutablePath -PythonRoot $pythonRoot
    Ensure-PinnedPythonPackages -PythonExe $portablePythonExe -ConstraintsPath $constraintsPath
    $env:PATH = "$pythonRoot;$($pythonRoot)\Scripts;$($env:PATH)"
    $sourceMirrorRoot = Join-Path $workRoot "source"
    New-SourceMirror -MirrorRoot $sourceMirrorRoot -BuildRootAbsolute $buildRootAbsolute
    Copy-LockedDependenciesToMirror -SourceDepsRoot $depsRoot -MirrorRoot $sourceMirrorRoot
    Ensure-Directory -Path (Join-Path $sourceMirrorRoot "dist")
    Invoke-Checked -FilePath $portablePythonExe -Arguments @("-m", "build", "--no-isolation", "--wheel", "-Csetup-args=--debug") -WorkingDirectory $sourceMirrorRoot
    $mirrorDistDir = Join-Path $sourceMirrorRoot "dist"
    $builtWheels = @(Get-ChildItem -LiteralPath $mirrorDistDir -Filter "VapourSynth-$versionString-*.whl" -File)
    if ($builtWheels.Count -eq 0) {
        throw "Fallback build did not generate a VapourSynth wheel in $mirrorDistDir"
    }
    foreach ($wheel in $builtWheels) {
        Invoke-Checked -FilePath $portablePythonExe -Arguments @("-m", "wheel", "tags", "--remove", "--python-tag", "cp312", $wheel.Name) -WorkingDirectory $mirrorDistDir
    }
    $mirrorWheelPath = Join-Path (Join-Path $sourceMirrorRoot "dist") $wheelName
    if (-not (Test-Path -LiteralPath $mirrorWheelPath -PathType Leaf)) {
        throw "Fallback build completed but expected wheel is missing: $mirrorWheelPath"
    }
    Ensure-Directory -Path $repoDistDir
    Copy-Item -LiteralPath $mirrorWheelPath -Destination $repoWheelPath -Force
}

$portableStageRoot = Join-Path $workRoot "buildp64"
if (Test-Path -LiteralPath $portableStageRoot -PathType Container) {
    Remove-Item -LiteralPath $portableStageRoot -Recurse -Force
}
Ensure-Directory -Path $portableStageRoot
Ensure-Directory -Path (Join-Path $portableStageRoot "wheel")

Copy-Item -LiteralPath $repoWheelPath -Destination (Join-Path $portableStageRoot "wheel") -Force
Copy-Item -LiteralPath (Join-Path $Script:RepoRoot "installer\vspipe.bat") -Destination $portableStageRoot -Force
Copy-Item -LiteralPath (Join-Path $Script:RepoRoot "installer\pip.bat") -Destination $portableStageRoot -Force

$portableZipPath = Join-Path $compiledOutRoot "VapourSynth64-Portable-R$versionString.zip"
if (Test-Path -LiteralPath $portableZipPath -PathType Leaf) {
    Remove-Item -LiteralPath $portableZipPath -Force
}
Write-Step "Creating portable ZIP: $portableZipPath"
Compress-Archive -Path (Join-Path $portableStageRoot "*") -CompressionLevel Optimal -DestinationPath $portableZipPath

Write-Step "Portable ZIP build completed"
Write-Step "Output: $portableZipPath"
