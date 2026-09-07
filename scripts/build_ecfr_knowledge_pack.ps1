[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$KnowledgeRoot,

    [Parameter(Mandatory = $true)]
    [ValidateSet("2026-09-03")]
    [string]$Snapshot,

    [Parameter(Mandatory = $true)]
    [ValidateSet(30000)]
    [int]$DocumentCount
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

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
    & $script:PackPython @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw $Failure
    }
}

function Invoke-PackPythonReport(
    [string[]]$Arguments,
    [string]$ReportPath,
    [string]$Failure) {
    $result = & $script:PackPython @Arguments
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
if (Test-IsSameOrChild $SourceRoot $KnowledgeRoot) {
    throw "KnowledgeRoot must be outside the source repository"
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
$hostVersion = & $HostPython -c "import sys; print('.'.join(map(str, sys.version_info[:2])))"
if ($LASTEXITCODE -ne 0 -or $hostVersion.Trim() -ne "3.11") {
    throw "Python 3.11 is required to resolve locked wheels"
}

$cacheRoot = Join-Path $KnowledgeRoot ".build-cache"
$stagingParent = Join-Path $KnowledgeRoot ".staging"
$wheelhouse = Join-Path $cacheRoot "wheels-py311-win-amd64"
$pythonArchive = Join-Path $cacheRoot "python-$PythonVersion-embed-amd64.zip"
foreach ($path in @($cacheRoot, $stagingParent, $wheelhouse, $pythonArchive)) {
    Assert-ChildPath $KnowledgeRoot $path
}
[System.IO.Directory]::CreateDirectory($cacheRoot) | Out-Null
[System.IO.Directory]::CreateDirectory($stagingParent) | Out-Null
[System.IO.Directory]::CreateDirectory($wheelhouse) | Out-Null

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
    & $publishedPython -E -s -X utf8 $publishedSidecar verify-pack --pack-root $Destination
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
                # An invalid partial staging tree is never reused or deleted.
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
        & $HostPython -c $verifyRuntime $StagingRoot $intentSha
        if ($LASTEXITCODE -ne 0) {
            throw "resume runtime lock mismatch"
        }
    } else {
        if (Test-Path -LiteralPath $RuntimeRoot) {
            throw "partial runtime without a lock is not resumable"
        }
        Write-Host "Resolving and recording the portable Python wheel set."
        & $HostPython -m pip download --disable-pip-version-check --only-binary=:all: `
            --requirement $RequirementsLock --dest $wheelhouse
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
            --find-links $wheelhouse --requirement $RequirementsLock `
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
        Copy-Item -LiteralPath (Join-Path $SidecarSource "agent_rag") `
            -Destination $SidecarRoot -Recurse
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
    Invoke-PackPython @("-E", "-s", "-X", "utf8", "-c", $importProbe) `
        "portable runtime import verification failed"

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
        Invoke-PackPython @(
            "-E", "-s", "-X", "utf8", "-c", $modelDownload,
            $EmbeddingModel, $EmbeddingRevision, $ModelRoot) `
            "pinned embedding model download failed"
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
    $integrity = & $script:PackPython -E -s -X utf8 -c $integrityProbe $StagingRoot
    if ($LASTEXITCODE -ne 0) {
        throw "SQLite or vector integrity verification failed"
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $ReportsRoot "integrity.json"),
        (($integrity -join [Environment]::NewLine) + [Environment]::NewLine),
        [System.Text.UTF8Encoding]::new($false))

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

    $allRecords = @()
    foreach ($file in Get-ChildItem -LiteralPath $StagingRoot -Recurse -File |
            Where-Object { $_.Name -ne "pack.json" } | Sort-Object FullName) {
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
    }
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
        relevance_dense_min = 0.0
        complete = $true
        files = @($allRecords)
    }
    Write-JsonUtf8NoBom (Join-Path $StagingRoot "pack.json") $manifest

    $verifyPack = @'
import pathlib, sys
from agent_rag.pack import verify_complete_pack
m = verify_complete_pack(pathlib.Path(sys.argv[1]))
if m.document_count != 30000 or m.embedding_dimensions != 1024:
    raise SystemExit(2)
'@
    Invoke-PackPython @(
        "-E", "-s", "-X", "utf8", "-c", $verifyPack, $StagingRoot) `
        "complete Knowledge Pack verification failed"

    $publishPack = @'
import pathlib, sys
from agent_rag.pack import atomic_publish
atomic_publish(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
'@
    Write-Host "Publishing the verified Knowledge Pack atomically."
    Invoke-PackPython @(
        "-E", "-s", "-X", "utf8", "-c", $publishPack,
        $StagingRoot, $Destination) `
        "Knowledge Pack publication failed"
    $PublishedRoot = Get-FullPath $Destination
}

function Publish-ActivePointer([string]$PointerPath, [string]$PackRoot) {
    $pointerParent = Split-Path -Parent $PointerPath
    [System.IO.Directory]::CreateDirectory($pointerParent) | Out-Null
    Assert-NoReparseComponents $pointerParent
    $temporary = "$PointerPath.partial-$([Guid]::NewGuid().ToString('N'))"
    $pointer = [ordered]@{
        schema_version = 1
        pack_root = $PackRoot
    }
    Write-JsonUtf8NoBom $temporary $pointer
    if (Test-Path -LiteralPath $PointerPath -PathType Leaf) {
        [System.IO.File]::Replace($temporary, $PointerPath, $null)
    } else {
        [System.IO.File]::Move($temporary, $PointerPath)
    }
}

# One pointer travels with the external root for audit/recovery. The second is
# the only EXE-relative discovery location used by a double-click Ready launch.
Publish-ActivePointer (Join-Path $KnowledgeRoot "active-pack.json") $PublishedRoot
$ReadyPointer = Join-Path $SourceRoot "out\AgentFramework-Knowledge\active-pack.json"
if ($ReadyPointer -match '(?i)[\\/]AgentFramework-Ready[\\/]') {
    throw "active pack pointer must remain outside Ready"
}
Publish-ActivePointer $ReadyPointer $PublishedRoot

Write-Host "Knowledge Pack published and active pointer updated."
