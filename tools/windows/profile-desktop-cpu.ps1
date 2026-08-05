[CmdletBinding()]
param(
    [string]$Scenario = "build/perf-section-toggle.json",
    [string]$OutputRoot = "build/profiles/section-toggle",
    [switch]$Elevated
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $Elevated) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host "Requesting temporary administrator access for this profiling capture..."
        $quote = {
            param([string]$Value)
            return '"' + $Value.Replace('"', '\"') + '"'
        }
        $arguments = @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", (& $quote $PSCommandPath),
            "-Scenario", (& $quote $Scenario),
            "-OutputRoot", (& $quote $OutputRoot),
            "-Elevated"
        )
        $process = Start-Process powershell.exe `
            -Verb RunAs `
            -ArgumentList $arguments `
            -Wait `
            -PassThru
        exit $process.ExitCode
    }
}

$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
function Resolve-RepositoryPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repository $Path))
}

$scenarioPath = Resolve-RepositoryPath $Scenario
$outputPath = Resolve-RepositoryPath $OutputRoot
$executable = Join-Path $repository "build\cmake\windows-x64\native\RelWithDebInfo\strata_desktop.exe"
$resources = Join-Path $repository "src\main\resources"
$wpr = Join-Path $env:SystemRoot "System32\wpr.exe"
$xperf = "${env:ProgramFiles(x86)}\Windows Kits\10\Windows Performance Toolkit\xperf.exe"

foreach ($required in @($scenarioPath, $executable, $resources, $wpr, $xperf)) {
    if (-not (Test-Path $required)) { throw "Required profiling input does not exist: $required" }
}

New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$benchmarkPath = Join-Path $outputPath "benchmark"
$tracePath = Join-Path $outputPath "cpu.etl"
$detailPath = Join-Path $outputPath "cpu-functions.txt"
$stackPath = Join-Path $outputPath "cpu-stacks.txt"
$symbolCache = Join-Path $repository "build\profiles\symbols"
New-Item -ItemType Directory -Force -Path $benchmarkPath, $symbolCache | Out-Null

Write-Host "Starting Windows sampled CPU trace..."
& $wpr -start CPU -filemode
if ($LASTEXITCODE -ne 0) {
    throw "WPR could not start the elevated CPU-profiling session."
}

$benchmarkExitCode = 0
try {
    & $executable `
        --performance $scenarioPath `
        --output $benchmarkPath `
        --resources $resources
    $benchmarkExitCode = $LASTEXITCODE
}
finally {
    Write-Host "Stopping and merging trace..."
    & $wpr -stop $tracePath
    if ($LASTEXITCODE -ne 0) { throw "WPR could not stop and merge the trace." }
}
if ($benchmarkExitCode -ne 0) {
    throw "strata_desktop exited with code $benchmarkExitCode. The trace was still saved to '$tracePath'."
}

$binaryDirectory = Split-Path $executable
$env:_NT_SYMBOL_PATH = "$binaryDirectory;srv*$symbolCache*https://msdl.microsoft.com/download/symbols"
$env:_NT_SYMCACHE_PATH = $symbolCache

Write-Host "Resolving PDBs and producing function report..."
& $xperf -i $tracePath -symbols -o $detailPath -a profile -detail
if ($LASTEXITCODE -ne 0) { throw "xperf could not produce the function report." }

Write-Host "Producing sampled call-stack report..."
& $xperf -i $tracePath -symbols -o $stackPath -a stack -process "strata_desktop\.exe" -event Profile -butterfly 1
if ($LASTEXITCODE -ne 0) { throw "xperf could not produce the stack report." }

Write-Host "CPU profile complete:" -ForegroundColor Green
Write-Host "  ETL:       $tracePath"
Write-Host "  Functions: $detailPath"
Write-Host "  Stacks:    $stackPath"
Write-Host "  Benchmark: $(Join-Path $benchmarkPath 'performance.json')"
