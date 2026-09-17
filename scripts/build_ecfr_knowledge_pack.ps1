[CmdletBinding()]
param(
    [string]$KnowledgeRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $PSScriptRoot "..\knowledge")),

    [Parameter(Mandatory = $true)]
    [ValidateSet("2026-09-03")]
    [string]$Snapshot,

    [Parameter(Mandatory = $true)]
    [ValidateSet(30000)]
    [int]$DocumentCount
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$env:PYTHONDONTWRITEBYTECODE = "1"

$PythonVersion = "3.11.9"
$PythonArchiveUrl =
    "https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip"
$PythonArchiveSha256 =
    "009d6bf7e3b2ddca3d784fa09f90fe54336d5b60f0e0f305c37f400bf83cfd3b"
$EmbeddingModel = "BAAI/bge-m3"
$EmbeddingRevision = "5617a9f61b028005a4858fdac845db406aefb181"
$EmbeddingDimensions = 1024
$SchemaVersion = 2

function Get-FullPath([string]$Path) {
    return [System.IO.Path]::GetFullPath($Path).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
}

function Test-IsFullyQualified([string]$Path) {
    return $Path -match '^[A-Za-z]:[\\/]' -or
        $Path -match '^\\\\[^\\]+\\[^\\]+'
}

function Test-IsSameOrChild([string]$Root, [string]$Candidate) {
    $rootPath = Get-FullPath $Root
    $candidatePath = Get-FullPath $Candidate
    if ($candidatePath.Equals(
            $rootPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    $prefix = $rootPath + [System.IO.Path]::DirectorySeparatorChar
    return $candidatePath.StartsWith(
        $prefix, [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-ChildPath([string]$Root, [string]$Candidate) {
    if (-not (Test-IsSameOrChild $Root $Candidate)) {
        throw "build path escaped the exact KnowledgeRoot"
    }
}

function Assert-NoReparseComponents([string]$Path) {
    $full = Get-FullPath $Path
    $current = [System.IO.Path]::GetPathRoot($full)
    foreach ($component in $full.Substring($current.Length).Split(
            [System.IO.Path]::DirectorySeparatorChar,
            [System.StringSplitOptions]::RemoveEmptyEntries)) {
        $current = Join-Path $current $component
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if (($item.Attributes -band
                    [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Knowledge Pack paths must not contain links"
            }
        }
    }
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Write-JsonUtf8NoBom([string]$Path, [object]$Value) {
    $json = $Value | ConvertTo-Json -Depth 12 -Compress
    [System.IO.File]::WriteAllText(
        $Path, $json, [System.Text.UTF8Encoding]::new($false))
}

function Invoke-PackPython([string[]]$Arguments, [string]$Failure) {
    & $script:PackPython -B @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw $Failure
    }
}

function Invoke-PackPythonReport(
    [string[]]$Arguments,
    [string]$ReportPath,
    [string]$Failure) {
    $result = & $script:PackPython -B @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw $Failure
    }
    [System.IO.File]::WriteAllText(
        $ReportPath,
        (($result -join [Environment]::NewLine) + [Environment]::NewLine),
        [System.Text.UTF8Encoding]::new($false))
}

function Get-CachedPythonSource([string]$Source) {
    if (-not (Test-Path -LiteralPath $script:PythonSourceRoot -PathType Container)) {
        throw "Python source cache is unavailable"
    }
    $sourceSha = [System.BitConverter]::ToString(
        [System.Security.Cryptography.SHA256]::Create().ComputeHash(
            [System.Text.Encoding]::UTF8.GetBytes($Source))).Replace("-", "").ToLowerInvariant()
    $sourcePath = Join-Path $script:PythonSourceRoot "$sourceSha.py"
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        [System.IO.File]::WriteAllText(
            $sourcePath, $Source, [System.Text.UTF8Encoding]::new($false))
    }
    return $sourcePath
}

function Invoke-PythonSource(
    [string]$Python,
    [string]$Source,
    [string[]]$ScriptArguments,
    [string]$Failure) {
    $sourcePath = Get-CachedPythonSource $Source
    & $Python -B -E -s -X utf8 $sourcePath @ScriptArguments
    if ($LASTEXITCODE -ne 0) {
        throw $Failure
    }
}

function Invoke-PythonSourceReport(
    [string]$Python,
    [string]$Source,
    [string[]]$ScriptArguments,
    [string]$ReportPath,
    [string]$Failure) {
    $sourcePath = Get-CachedPythonSource $Source
    $result = & $Python -B -E -s -X utf8 $sourcePath @ScriptArguments
    if ($LASTEXITCODE -ne 0) {
        throw $Failure
    }
    [System.IO.File]::WriteAllText(
        $ReportPath,
        (($result -join [Environment]::NewLine) + [Environment]::NewLine),
        [System.Text.UTF8Encoding]::new($false))
}

function Get-FileRecords([string]$Root, [string]$RelativeBase) {
    $base = Join-Path $Root $RelativeBase
    if (-not (Test-Path -LiteralPath $base -PathType Container)) {
        throw "required build directory is unavailable"
    }
    $records = @()
    foreach ($file in Get-ChildItem -LiteralPath $base -Recurse -File |
            Sort-Object FullName) {
        if (($file.Attributes -band
                [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Knowledge Pack files must not be links"
        }
        $relative = $file.FullName.Substring($Root.Length + 1).Replace("\", "/")
        $records += [ordered]@{
            path = $relative
            bytes = [int64]$file.Length
            sha256 = Get-Sha256 $file.FullName
        }
    }
    return @($records)
}

if (-not (Test-IsFullyQualified $KnowledgeRoot)) {
    throw "KnowledgeRoot must be absolute"
}

$SourceRoot = Get-FullPath (Join-Path $PSScriptRoot "..")
$KnowledgeRoot = Get-FullPath $KnowledgeRoot
if ((Test-IsSameOrChild $SourceRoot $KnowledgeRoot) -and
        $KnowledgeRoot -ne (Join-Path $SourceRoot "knowledge")) {
    throw "repository-local KnowledgeRoot must be the knowledge directory"
}
if ($KnowledgeRoot -match '(?i)[\\/]out[\\/]AgentFramework-Ready(?:[\\/]|$)') {
    throw "KnowledgeRoot must be outside Ready"
}
Assert-NoReparseComponents $KnowledgeRoot

if (-not (Test-Path -LiteralPath $KnowledgeRoot)) {
    [System.IO.Directory]::CreateDirectory($KnowledgeRoot) | Out-Null
}
if (-not (Test-Path -LiteralPath $KnowledgeRoot -PathType Container)) {
    throw "KnowledgeRoot must be a directory"
}
Assert-NoReparseComponents $KnowledgeRoot

$RequirementsLock = Join-Path $SourceRoot "rag\requirements-rag.lock"
$SidecarSource = Join-Path $SourceRoot "rag"
if (-not (Test-Path -LiteralPath $RequirementsLock -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $SidecarSource "agent_rag_cli.py") -PathType Leaf)) {
    throw "RAG build sources are unavailable"
}

$hostPythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
if ($null -eq $hostPythonCommand) {
    throw "Python 3.11 is required to resolve locked wheels"
}
$HostPython = $hostPythonCommand.Source
$hostVersion = (& $HostPython --version 2>&1) -join ""
if ($LASTEXITCODE -ne 0 -or $hostVersion -notmatch '^Python 3\.11(?:\.|$)') {
    throw "Python 3.11 is required to resolve locked wheels"
}

$cacheRoot = Join-Path $KnowledgeRoot ".build-cache"
$stagingParent = Join-Path $KnowledgeRoot ".staging"
$wheelhouse = Join-Path $cacheRoot "wheels-py311-win-amd64"
$pythonArchive = Join-Path $cacheRoot "python-$PythonVersion-embed-amd64.zip"
$script:PythonSourceRoot = Join-Path $cacheRoot "python-sources"
foreach ($path in @(
        $cacheRoot, $stagingParent, $wheelhouse, $pythonArchive,
        $script:PythonSourceRoot)) {
    Assert-ChildPath $KnowledgeRoot $path
}
[System.IO.Directory]::CreateDirectory($cacheRoot) | Out-Null
[System.IO.Directory]::CreateDirectory($stagingParent) | Out-Null
[System.IO.Directory]::CreateDirectory($wheelhouse) | Out-Null
[System.IO.Directory]::CreateDirectory($script:PythonSourceRoot) | Out-Null
Assert-NoReparseComponents $script:PythonSourceRoot

$sidecarDigestLines = @()
foreach ($file in Get-ChildItem -LiteralPath $SidecarSource -Recurse -File |
        Where-Object { $_.FullName -notmatch '[\\/]__pycache__[\\/]' } |
        Sort-Object FullName) {
    $relative = $file.FullName.Substring($SidecarSource.Length + 1).Replace("\", "/")
    $sidecarDigestLines += "$relative=$(Get-Sha256 $file.FullName)"
}
$sidecarDigest = [System.BitConverter]::ToString(
    [System.Security.Cryptography.SHA256]::Create().ComputeHash(
        [System.Text.Encoding]::UTF8.GetBytes($sidecarDigestLines -join "`n"))).Replace("-", "").ToLowerInvariant()

$intent = [ordered]@{
    schema_version = $SchemaVersion
    snapshot_date = $Snapshot
    document_count = $DocumentCount
    python_version = $PythonVersion
    python_archive_url = $PythonArchiveUrl
    python_archive_sha256 = $PythonArchiveSha256
    requirements_sha256 = Get-Sha256 $RequirementsLock
    embedding_model = $EmbeddingModel
    embedding_revision = $EmbeddingRevision
    embedding_dimensions = $EmbeddingDimensions
    sidecar_sha256 = $sidecarDigest
}
$intentJson = $intent | ConvertTo-Json -Depth 5 -Compress
$intentSha = [System.BitConverter]::ToString(
    [System.Security.Cryptography.SHA256]::Create().ComputeHash(
        [System.Text.Encoding]::UTF8.GetBytes($intentJson))).Replace("-", "").ToLowerInvariant()

$Destination = Join-Path $KnowledgeRoot "ecfr-$Snapshot"
Assert-ChildPath $KnowledgeRoot $Destination
if (Test-Path -LiteralPath $Destination) {
    $publishedPython = Join-Path $Destination "runtime\python.exe"
    $publishedSidecar = Join-Path $Destination "sidecar\agent_rag_cli.py"
    if (-not (Test-Path -LiteralPath $publishedPython -PathType Leaf) -or
        -not (Test-Path -LiteralPath $publishedSidecar -PathType Leaf)) {
        throw "published Knowledge Pack is incomplete"
    }
    $publishedIntent = Join-Path $Destination "build.intent.json"
    if (-not (Test-Path -LiteralPath $publishedIntent -PathType Leaf) -or
        (Get-Content -LiteralPath $publishedIntent -Raw -Encoding UTF8) -ne $intentJson) {
        throw "published Knowledge Pack build intent differs from current sources"
    }
    & $publishedPython -B -E -s -X utf8 $publishedSidecar verify-pack --pack-root $Destination
    if ($LASTEXITCODE -ne 0) {
        throw "published Knowledge Pack verification failed"
    }
    $PublishedRoot = Get-FullPath $Destination
} else {
    $resumeCandidates = @()
    foreach ($directory in Get-ChildItem -LiteralPath $stagingParent -Directory -ErrorAction SilentlyContinue) {
        if ($directory.Name -notlike "ecfr-$Snapshot-staging-*") {
            continue
        }
        $intentPath = Join-Path $directory.FullName "build.intent.json"
        $runtimeLockPath = Join-Path $directory.FullName "runtime.lock.json"
        if ((Test-Path -LiteralPath $intentPath -PathType Leaf) -and
            (Test-Path -LiteralPath $runtimeLockPath -PathType Leaf)) {
            try {
                $existingIntent = Get-Content -LiteralPath $intentPath -Raw -Encoding UTF8
                $runtimeLock = Get-Content -LiteralPath $runtimeLockPath -Raw -Encoding UTF8 |
                    ConvertFrom-Json
                if ($existingIntent -eq $intentJson -and
                    $runtimeLock.intent_sha256 -eq $intentSha) {
                    $resumeCandidates += $directory.FullName
                }
            } catch {
                # Preserve invalid staging trees without reusing them.
            }
        }
    }
    if ($resumeCandidates.Count -gt 1) {
        throw "multiple matching resumable staging directories exist"
    }
    if ($resumeCandidates.Count -eq 1) {
        $StagingRoot = Get-FullPath $resumeCandidates[0]
        Write-Host "Resuming a staging directory with an identical runtime lock."
    } else {
        $StagingRoot = Join-Path $stagingParent (
            "ecfr-$Snapshot-staging-" + [Guid]::NewGuid().ToString("N"))
        Assert-ChildPath $KnowledgeRoot $StagingRoot
        [System.IO.Directory]::CreateDirectory($StagingRoot) | Out-Null
        [System.IO.File]::WriteAllText(
            (Join-Path $StagingRoot "build.intent.json"), $intentJson,
            [System.Text.UTF8Encoding]::new($false))
    }
    Assert-NoReparseComponents $StagingRoot

    if (Test-Path -LiteralPath $pythonArchive -PathType Leaf) {
        if ((Get-Sha256 $pythonArchive) -ne $PythonArchiveSha256) {
            Remove-Item -LiteralPath $pythonArchive -Force
        }
    }
    if (-not (Test-Path -LiteralPath $pythonArchive -PathType Leaf)) {
        Write-Host "Downloading the pinned Python $PythonVersion embeddable runtime."
        $partialArchive = "$pythonArchive.partial-$([Guid]::NewGuid().ToString('N'))"
        Assert-ChildPath $KnowledgeRoot $partialArchive
        Invoke-WebRequest -Uri $PythonArchiveUrl -OutFile $partialArchive -UseBasicParsing
        if ((Get-Sha256 $partialArchive) -ne $PythonArchiveSha256) {
            Remove-Item -LiteralPath $partialArchive -Force
            throw "Python archive digest mismatch"
        }
        Move-Item -LiteralPath $partialArchive -Destination $pythonArchive
    }

    $RuntimeRoot = Join-Path $StagingRoot "runtime"
    $RuntimeLockPath = Join-Path $StagingRoot "runtime.lock.json"
    $script:PackPython = Join-Path $RuntimeRoot "python.exe"
    if (Test-Path -LiteralPath $RuntimeLockPath -PathType Leaf) {
        $verifyRuntime = @'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
lock = json.loads((root / "runtime.lock.json").read_text(encoding="utf-8"))
if set(lock) != {"schema_version","intent_sha256","python","requirements_sha256","wheels","files"}:
    raise SystemExit(2)
if lock["intent_sha256"] != sys.argv[2]:
    raise SystemExit(2)
for record in lock["files"]:
    path = root.joinpath(*record["path"].split("/"))
    if (not path.is_file() or path.stat().st_size != record["bytes"] or
        hashlib.sha256(path.read_bytes()).hexdigest() != record["sha256"]):
        raise SystemExit(2)
'@
        Invoke-PythonSource -Python $HostPython -Source $verifyRuntime `
            -ScriptArguments @($StagingRoot, $intentSha) `
            -Failure "resume runtime lock mismatch"
    } else {
        if (Test-Path -LiteralPath $RuntimeRoot) {
            throw "partial runtime without a lock is not resumable"
        }
        Write-Host "Resolving and recording the portable Python wheel set."
        & $HostPython -m pip download --disable-pip-version-check --only-binary=:all: `
            --require-hashes --requirement $RequirementsLock --dest $wheelhouse
        if ($LASTEXITCODE -ne 0) {
            throw "locked wheel download failed"
        }
        Expand-Archive -LiteralPath $pythonArchive -DestinationPath $RuntimeRoot
        $pthPath = Join-Path $RuntimeRoot "python311._pth"
        if (-not (Test-Path -LiteralPath $pthPath -PathType Leaf)) {
            throw "embedded Python path configuration is missing"
        }
        [System.IO.File]::WriteAllText(
            $pthPath,
            "python311.zip`r`n.`r`nLib/site-packages`r`n../sidecar`r`nimport site`r`n",
            [System.Text.Encoding]::ASCII)
        [System.IO.Directory]::CreateDirectory(
            (Join-Path $RuntimeRoot "Lib\site-packages")) | Out-Null
        & $HostPython -m pip install --disable-pip-version-check --no-index `
            --require-hashes --find-links $wheelhouse --requirement $RequirementsLock `
            --target (Join-Path $RuntimeRoot "Lib\site-packages")
        if ($LASTEXITCODE -ne 0) {
            throw "portable runtime dependency installation failed"
        }
        $wheelRecords = @()
        foreach ($wheel in Get-ChildItem -LiteralPath $wheelhouse -File |
                Sort-Object Name) {
            $wheelRecords += [ordered]@{
                name = $wheel.Name
                bytes = [int64]$wheel.Length
                sha256 = Get-Sha256 $wheel.FullName
            }
        }
        if ($wheelRecords.Count -eq 0) {
            throw "resolved wheel set is empty"
        }
        $runtimeRecords = Get-FileRecords $StagingRoot "runtime"
        $runtimeLock = [ordered]@{
            schema_version = $SchemaVersion
            intent_sha256 = $intentSha
            python = [ordered]@{
                version = $PythonVersion
                url = $PythonArchiveUrl
                sha256 = $PythonArchiveSha256
            }
            requirements_sha256 = Get-Sha256 $RequirementsLock
            wheels = @($wheelRecords)
            files = @($runtimeRecords)
        }
        Write-JsonUtf8NoBom $RuntimeLockPath $runtimeLock
    }

    $SidecarRoot = Join-Path $StagingRoot "sidecar"
    if (-not (Test-Path -LiteralPath $SidecarRoot)) {
        [System.IO.Directory]::CreateDirectory($SidecarRoot) | Out-Null
        Copy-Item -LiteralPath (Join-Path $SidecarSource "agent_rag_cli.py") `
            -Destination $SidecarRoot
        $SidecarPackage = Join-Path $SidecarRoot "agent_rag"
        [System.IO.Directory]::CreateDirectory($SidecarPackage) | Out-Null
        foreach ($sourceFile in Get-ChildItem `
                -LiteralPath (Join-Path $SidecarSource "agent_rag") `
                -File -Filter "*.py" | Sort-Object Name) {
            Copy-Item -LiteralPath $sourceFile.FullName -Destination $SidecarPackage
        }
    }
    Assert-NoReparseComponents $StagingRoot

    Write-Host "Validating portable runtime imports."
    $importProbe = @'
import defusedxml, numpy, sqlite3, torch, sentence_transformers
from agent_rag.cli import main
assert numpy.__version__
assert callable(main)
assert torch.empty((1,), device="cpu").numel() == 1
'@
    Invoke-PythonSource -Python $script:PackPython -Source $importProbe `
        -ScriptArguments @() `
        -Failure "portable runtime import verification failed"

    $ModelRoot = Join-Path $StagingRoot "model\bge-m3"
    if (-not (Test-Path -LiteralPath $ModelRoot -PathType Container)) {
        [System.IO.Directory]::CreateDirectory($ModelRoot) | Out-Null
        Write-Host "Downloading the pinned BGE-M3 model revision."
        $modelDownload = @'
from huggingface_hub import snapshot_download
import pathlib, sys
snapshot_download(repo_id=sys.argv[1], revision=sys.argv[2],
                  local_dir=pathlib.Path(sys.argv[3]))
'@
        Invoke-PythonSource -Python $script:PackPython -Source $modelDownload `
            -ScriptArguments @($EmbeddingModel, $EmbeddingRevision, $ModelRoot) `
            -Failure "pinned embedding model download failed"
    }
    Assert-NoReparseComponents $ModelRoot
    $modelRecords = Get-FileRecords $StagingRoot "model\bge-m3"
    if ($modelRecords.Count -eq 0) {
        throw "pinned embedding model is empty"
    }
    $modelLock = [ordered]@{
        schema_version = $SchemaVersion
        model = $EmbeddingModel
        revision = $EmbeddingRevision
        dimensions = $EmbeddingDimensions
        files = @($modelRecords)
    }
    Write-JsonUtf8NoBom (Join-Path $StagingRoot "model.lock.json") $modelLock

    $SidecarCli = Join-Path $SidecarRoot "agent_rag_cli.py"
    $ReportsRoot = Join-Path $StagingRoot "reports"
    [System.IO.Directory]::CreateDirectory($ReportsRoot) | Out-Null
    Invoke-PackPythonReport @(
        "-E", "-s", "-X", "utf8", $SidecarCli,
        "verify-model", "--model-root", $ModelRoot) `
        (Join-Path $ReportsRoot "model-verify.json") `
        "embedding model verification failed"

    Write-Host "Downloading and verifying the fixed eCFR snapshot."
    Invoke-PackPythonReport @(
        "-E", "-s", "-X", "utf8", $SidecarCli,
        "download-ecfr", "--snapshot", $Snapshot,
        "--staging-root", $StagingRoot) `
        (Join-Path $ReportsRoot "download.json") `
        "eCFR snapshot download failed"

    Write-Host "Selecting exactly $DocumentCount complete Markdown documents."
    Invoke-PackPythonReport @(
        "-E", "-s", "-X", "utf8", $SidecarCli,
        "build-corpus", "--snapshot", $Snapshot,
        "--staging-root", $StagingRoot, "--count", "$DocumentCount") `
        (Join-Path $ReportsRoot "corpus-command.json") `
        "eCFR corpus build failed"

    Write-Host "Building BM25 and normalized 1024-dimensional BGE-M3 vectors."
    Invoke-PackPythonReport @(
        "-E", "-s", "-X", "utf8", $SidecarCli,
        "build-index", "--pack-root", $StagingRoot,
        "--device", "auto", "--batch-size", "16") `
        (Join-Path $ReportsRoot "index-build.json") `
        "hybrid index build failed"

    $integrityProbe = @'
import json, pathlib, sqlite3, sys
root = pathlib.Path(sys.argv[1])
meta = json.loads((root / "index" / "vectors.json").read_text(encoding="utf-8"))
with sqlite3.connect(root / "index" / "metadata.sqlite3") as db:
    assert db.execute("PRAGMA integrity_check").fetchone() == ("ok",)
    rows = db.execute("SELECT COUNT(*) FROM chunks").fetchone()[0]
assert meta["dtype"] == "<f2" and meta["dimensions"] == 1024
assert meta["rows"] == rows
assert (root / "index" / "vectors.f16").stat().st_size == rows * 1024 * 2
print(json.dumps({"schema_version":2,"sqlite":"ok","rows":rows,
                  "dimensions":1024}, sort_keys=True, separators=(",",":")))
'@
    Invoke-PythonSourceReport -Python $script:PackPython -Source $integrityProbe `
        -ScriptArguments @($StagingRoot) `
        -ReportPath (Join-Path $ReportsRoot "integrity.json") `
        -Failure "SQLite or vector integrity verification failed"

    $vectors = Get-Content -LiteralPath (Join-Path $StagingRoot "index\vectors.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    $documentsPath = Join-Path $StagingRoot "manifest\documents.jsonl"
    $actualDocumentCount = (Get-Content -LiteralPath $documentsPath -Encoding UTF8 |
        Measure-Object -Line).Lines
    $actualMarkdownCount = (Get-ChildItem -LiteralPath (Join-Path $StagingRoot "corpus") `
        -Recurse -File -Filter "*.md" | Measure-Object).Count
    if ($actualDocumentCount -ne $DocumentCount -or
        $actualMarkdownCount -ne $DocumentCount) {
        throw "corpus document count is inconsistent"
    }

    function Write-CompletePackManifest([double]$DenseMinimum) {
        $runtimeDebris = @(Get-ChildItem -LiteralPath $SidecarRoot -Recurse -Force |
            Where-Object {
                $_.Name -eq "__pycache__" -or $_.Name -like "*.pyc"
            })
        if ($runtimeDebris.Count -ne 0) {
            throw "Knowledge Pack contains Python runtime debris"
        }
        $allRecords = @()
        $manifestFiles = @(Get-ChildItem -LiteralPath $StagingRoot -Recurse -File |
            Where-Object { $_.Name -ne "pack.json" } | Sort-Object FullName)
        $manifestTimer = [System.Diagnostics.Stopwatch]::StartNew()
        $lastManifestProgress = -5.0
        $completedManifestFiles = 0
        $manifestStartEvent = [ordered]@{
            schema_version = 1
            type = "progress"
            phase = "finalize-pack-files"
            mode = $null
            status = "running"
            completed = 0
            total = $manifestFiles.Count
            percent = 0.0
            elapsed_seconds = 0.0
            throughput_items_per_second = 0.0
            eta_seconds = $null
        }
        [Console]::Error.WriteLine(
            ($manifestStartEvent | ConvertTo-Json -Depth 4 -Compress))
        $lastManifestProgress = 0.0
        foreach ($file in $manifestFiles) {
            if (($file.Attributes -band
                    [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Knowledge Pack files must not be links"
            }
            $relative = $file.FullName.Substring($StagingRoot.Length + 1).Replace("\", "/")
            $allRecords += [ordered]@{
                path = $relative
                bytes = [int64]$file.Length
                sha256 = Get-Sha256 $file.FullName
            }
            $completedManifestFiles += 1
            if ($completedManifestFiles -eq $manifestFiles.Count -or
                $manifestTimer.Elapsed.TotalSeconds - $lastManifestProgress -ge 5.0) {
                $elapsed = $manifestTimer.Elapsed.TotalSeconds
                $throughput = if ($elapsed -gt 0.0) {
                    $completedManifestFiles / $elapsed
                } else { 0.0 }
                $eta = if ($throughput -gt 0.0) {
                    ($manifestFiles.Count - $completedManifestFiles) / $throughput
                } else { $null }
                $event = [ordered]@{
                    schema_version = 1
                    type = "progress"
                    phase = "finalize-pack-files"
                    mode = $null
                    status = if ($completedManifestFiles -eq $manifestFiles.Count) {
                        "completed"
                    } else { "running" }
                    completed = $completedManifestFiles
                    total = $manifestFiles.Count
                    percent = [Math]::Round(
                        100.0 * $completedManifestFiles / $manifestFiles.Count, 3)
                    elapsed_seconds = [Math]::Round($elapsed, 3)
                    throughput_items_per_second = [Math]::Round($throughput, 3)
                    eta_seconds = if ($null -eq $eta) { $null } else {
                        [Math]::Round($eta, 3)
                    }
                }
                [Console]::Error.WriteLine(
                    ($event | ConvertTo-Json -Depth 4 -Compress))
                $lastManifestProgress = $elapsed
            }
        }
        # Pack ID covers content; build.intent.json pins sidecar source.
        $identitySeed = "$Snapshot`n$DocumentCount`n$(Get-Sha256 $documentsPath)`n$($vectors.matrix_sha256)`n$EmbeddingRevision"
        $identityHash = [System.BitConverter]::ToString(
            [System.Security.Cryptography.SHA256]::Create().ComputeHash(
                [System.Text.Encoding]::UTF8.GetBytes($identitySeed))).Replace("-", "").ToLowerInvariant()
        $manifest = [ordered]@{
            schema_version = $SchemaVersion
            pack_id = "pack-" + $identityHash.Substring(0, 32)
            snapshot_date = $Snapshot
            document_count = $DocumentCount
            chunk_count = [int64]$vectors.rows
            embedding_model = $EmbeddingModel
            embedding_revision = $EmbeddingRevision
            embedding_dimensions = $EmbeddingDimensions
            relevance_dense_min = $DenseMinimum
            complete = $true
            files = @($allRecords)
        }
        Write-JsonUtf8NoBom (Join-Path $StagingRoot "pack.json") $manifest
    }

    $verifyPack = @'
import pathlib, sys
from agent_rag.pack import verify_complete_pack
m = verify_complete_pack(pathlib.Path(sys.argv[1]))
if m.document_count != 30000 or m.embedding_dimensions != 1024:
    raise SystemExit(2)
'@
    # Keep evaluation output external so all modes verify the same manifest.
    Write-CompletePackManifest -DenseMinimum -1.0
    Invoke-PythonSource -Python $script:PackPython -Source $verifyPack `
        -ScriptArguments @($StagingRoot) `
        -Failure "complete Knowledge Pack verification failed"

    $EvaluationWork = Join-Path $cacheRoot (
        "evaluation-$Snapshot-" + [Guid]::NewGuid().ToString("N"))
    Assert-ChildPath $KnowledgeRoot $EvaluationWork
    [System.IO.Directory]::CreateDirectory($EvaluationWork) | Out-Null
    $EvaluationCases = Join-Path $EvaluationWork "ecfr-cases.jsonl"
    $NegativeCases = Join-Path $SourceRoot "rag\eval\negative_cases.jsonl"
    $CuratedCases = Join-Path $SourceRoot "rag\eval\frozen_curated_cases.jsonl"
    Write-Host "Generating label-resolved retrieval cases, including the frozen curated holdout."
    Invoke-PackPython @(
        "-E", "-s", "-X", "utf8", $SidecarCli,
        "generate-eval", "--index-root", (Join-Path $StagingRoot "index"),
        "--negative-cases", $NegativeCases,
        "--curated-cases", $CuratedCases, "--output", $EvaluationCases) `
        "retrieval evaluation case generation failed"
    $modeReports = [ordered]@{}
    foreach ($mode in @("lexical", "dense", "hybrid")) {
        $modeReportPath = Join-Path $EvaluationWork "$mode.json"
        Write-Host "Evaluating $mode retrieval on the identical frozen case order."
        Invoke-PackPython @(
            "-E", "-s", "-X", "utf8", $SidecarCli,
            "evaluate", "--pack-root", $StagingRoot,
            "--cases", $EvaluationCases, "--mode", $mode,
            "--output", $modeReportPath) `
            "$mode retrieval evaluation failed"
        $modeReports[$mode] = Get-Content -LiteralPath $modeReportPath -Raw `
            -Encoding UTF8 | ConvertFrom-Json
    }
    $qualitySelection = Join-Path $EvaluationWork "selection.json"
    $qualityGate = @'
import json, pathlib, sys
from agent_rag.evaluation import select_retrieval_mode
reports = [json.loads(pathlib.Path(p).read_text(encoding="utf-8")) for p in sys.argv[1:4]]
selected = select_retrieval_mode(lexical=reports[0], dense=reports[1], hybrid=reports[2])
payload = {"schema_version": 2, "selection_split": "development", "selected_mode": selected}
pathlib.Path(sys.argv[4]).write_text(
    json.dumps(payload, sort_keys=True, separators=(",", ":")), encoding="utf-8"
)
'@
    Invoke-PythonSource -Python $script:PackPython -Source $qualityGate `
        -ScriptArguments @(
            (Join-Path $EvaluationWork "lexical.json"),
            (Join-Path $EvaluationWork "dense.json"),
            (Join-Path $EvaluationWork "hybrid.json"),
            $qualitySelection) `
        -Failure "selected retrieval quality gate failed"
    $selection = Get-Content -LiteralPath $qualitySelection -Raw -Encoding UTF8 |
        ConvertFrom-Json
    if ($selection.schema_version -ne $SchemaVersion -or
        $selection.selection_split -ne "development" -or
        $selection.selected_mode -notin @("dense", "hybrid")) {
        throw "retrieval mode selection is invalid"
    }
    $SelectedMode = [string]$selection.selected_mode
    $DenseMinimum = [double]$modeReports[$SelectedMode].threshold.dense_cosine_min
    if ([double]::IsNaN($DenseMinimum) -or [double]::IsInfinity($DenseMinimum) -or
        $DenseMinimum -lt -1.0 -or $DenseMinimum -gt 1.0) {
        throw "fitted dense threshold is invalid"
    }
    $EvaluationRoot = Join-Path $StagingRoot "eval"
    [System.IO.Directory]::CreateDirectory($EvaluationRoot) | Out-Null
    Copy-Item -LiteralPath $EvaluationCases `
        -Destination (Join-Path $EvaluationRoot "ecfr-cases.jsonl")
    Copy-Item -LiteralPath (Join-Path $SourceRoot "rag\eval\ecfr_eval_schema.json") `
        -Destination (Join-Path $EvaluationRoot "ecfr_eval_schema.json")
    $combinedReport = [ordered]@{
        schema_version = $SchemaVersion
        case_count = [int]$modeReports[$SelectedMode].case_count
        quality_gate = "passed"
        selection_split = "development"
        selected_mode = $SelectedMode
        relevance_dense_min = $DenseMinimum
        modes = $modeReports
    }
    Write-JsonUtf8NoBom (Join-Path $ReportsRoot "retrieval-eval.json") $combinedReport
    Write-CompletePackManifest -DenseMinimum $DenseMinimum
    Invoke-PythonSource -Python $script:PackPython -Source $verifyPack `
        -ScriptArguments @($StagingRoot) `
        -Failure "evaluated Knowledge Pack verification failed"

    $publishPack = @'
import pathlib, sys
from agent_rag.pack import atomic_publish
from agent_rag.progress import ProgressReporter
reporter = ProgressReporter(sys.stderr)
reporter.update("publish-pack-command", 0, 1)
try:
    atomic_publish(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]),
                   progress=reporter.update)
except (Exception, KeyboardInterrupt):
    reporter.fail("publish-pack-command")
    raise
reporter.update("publish-pack-command", 1, 1)
'@
    Write-Host "Publishing the verified Knowledge Pack atomically."
    Invoke-PythonSource -Python $script:PackPython -Source $publishPack `
        -ScriptArguments @($StagingRoot, $Destination) `
        -Failure "Knowledge Pack publication failed"
    # T4: stamp the canonical backend identity the native lease will seal
    # against. Recording it here gives the operator a side-by-side compare
    # against the C++ verifier's emit_progress message so a rotation of the
    # embedding model is visible without re-running verification.
    $leaseIdentityReport = @'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
manifest = json.loads((root / "pack.json").read_text(encoding="utf-8"))
identity = "{0}@{1}/{2}".format(
    manifest["embedding_model"], manifest["embedding_revision"],
    manifest["embedding_dimensions"])
report = {
    "schema_version": 2,
    "backend_identity": identity,
    "manifest_sha256_path": str(root / "pack.json"),
}
pathlib.Path(sys.argv[2]).write_text(
    json.dumps(report, sort_keys=True, separators=(",", ":")),
    encoding="utf-8")
'@
    $leaseIdentityPath = Join-Path $ReportsRoot "lease-identity.json"
    Invoke-PythonSource -Python $script:PackPython -Source $leaseIdentityReport `
        -ScriptArguments @($Destination, $leaseIdentityPath) `
        -Failure "Knowledge Pack lease identity stamping failed"
    $PublishedRoot = Get-FullPath $Destination
}

function Publish-ActivePointer([string]$PointerPath, [string]$PackRoot) {
    $pointerParent = Split-Path -Parent $PointerPath
    [System.IO.Directory]::CreateDirectory($pointerParent) | Out-Null
    Assert-NoReparseComponents $pointerParent
    $temporary = "$PointerPath.partial-$([Guid]::NewGuid().ToString('N'))"
    $pointer = [ordered]@{
        schema_version = 1
        pack_root = Split-Path -Leaf $PackRoot
    }
    Write-JsonUtf8NoBom $temporary $pointer
    if (Test-Path -LiteralPath $PointerPath -PathType Leaf) {
        [System.IO.File]::Replace($temporary, $PointerPath, $null)
    } else {
        [System.IO.File]::Move($temporary, $PointerPath)
    }
}

# One portable pointer next to the published pack.
Publish-ActivePointer (Join-Path $KnowledgeRoot "active-pack.json") $PublishedRoot

if ($KnowledgeRoot -eq (Join-Path $SourceRoot "knowledge")) {
    Write-Host "Knowledge Pack published and project active pointer updated."
} else {
    Write-Host "Knowledge Pack published to the external root."
    Write-Host "To activate it for AgentFramework, set AGENT_RAG_PACK_ROOT=$PublishedRoot"
}
