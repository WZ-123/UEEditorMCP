param(
    [switch]$BootstrapIfMissing = $true
)

$ErrorActionPreference = "Stop"

$PluginDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginDir)
$PythonDir = Join-Path $PluginDir "Python"
$VenvPython = Join-Path $PythonDir ".venv\Scripts\python.exe"
$SetupScript = Join-Path $PluginDir "setup_mcp.ps1"

if (-not (Test-Path $VenvPython)) {
    if ($BootstrapIfMissing -and (Test-Path $SetupScript)) {
        Write-Host "[UEEditorMCP] Missing venv, running setup_mcp.ps1..." -ForegroundColor Yellow
        & $SetupScript
        if ($LASTEXITCODE -ne 0) {
            throw "setup_mcp.ps1 failed with exit code $LASTEXITCODE."
        }
    } else {
        throw "Missing $VenvPython. Run Plugins/UEEditorMCP/setup_mcp.ps1 first."
    }
}

$vscodeDir = Join-Path $ProjectRoot ".vscode"
$vscodeMcpJsonPath = Join-Path $vscodeDir "mcp.json"
$cursorDir = Join-Path $ProjectRoot ".cursor"
$cursorMcpJsonPath = Join-Path $cursorDir "mcp.json"

if (-not (Test-Path $vscodeDir)) {
    New-Item -ItemType Directory -Path $vscodeDir -Force | Out-Null
}
if (-not (Test-Path $cursorDir)) {
    New-Item -ItemType Directory -Path $cursorDir -Force | Out-Null
}

$venvPythonForward = $VenvPython.Replace('\', '/')
$pythonPathForward = $PythonDir.Replace('\', '/')
$workspaceRootForward = $ProjectRoot.Replace('\', '/')

$desiredVscodeJson = @"
{
  "servers": {
    "ue-editor-mcp": {
      "command": "$venvPythonForward",
      "args": ["-m", "ue_editor_mcp.server_unified"],
      "env": {
        "PYTHONPATH": "$pythonPathForward"
      }
    },
    "ue-editor-mcp-logs": {
      "command": "$venvPythonForward",
      "args": ["-m", "ue_editor_mcp.server_unreal_logs"],
      "env": {
        "PYTHONPATH": "$pythonPathForward"
      }
    },
    "ue-editor-mcp-insights": {
      "command": "$venvPythonForward",
      "args": ["-m", "ue_editor_mcp.server_unreal_insights"],
      "env": {
        "PYTHONPATH": "$pythonPathForward",
        "WORKSPACE_ROOT": "$workspaceRootForward"
      }
    }
  }
}
"@

$desiredCursorJson = @"
{
  "mcpServers": {
    "ue-editor-mcp": {
      "type": "stdio",
      "command": "$venvPythonForward",
      "args": ["-m", "ue_editor_mcp.server_unified"],
      "env": {
        "PYTHONPATH": "$pythonPathForward"
      }
    },
    "ue-editor-mcp-logs": {
      "type": "stdio",
      "command": "$venvPythonForward",
      "args": ["-m", "ue_editor_mcp.server_unreal_logs"],
      "env": {
        "PYTHONPATH": "$pythonPathForward"
      }
    },
    "ue-editor-mcp-insights": {
      "type": "stdio",
      "command": "$venvPythonForward",
      "args": ["-m", "ue_editor_mcp.server_unreal_insights"],
      "env": {
        "PYTHONPATH": "$pythonPathForward",
        "WORKSPACE_ROOT": "$workspaceRootForward"
      }
    }
  }
}
"@

function Write-IfChanged {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$Content
    )
    $current = ""
    if (Test-Path $Path) {
        $current = [System.IO.File]::ReadAllText($Path)
    }
    if ($current -ne $Content) {
        [System.IO.File]::WriteAllText(
            $Path,
            $Content,
            [System.Text.UTF8Encoding]::new($false)
        )
        Write-Host "[UEEditorMCP] Updated $Path" -ForegroundColor Green
    } else {
        Write-Host "[UEEditorMCP] Up to date: $Path" -ForegroundColor DarkGray
    }
}

Write-IfChanged -Path $vscodeMcpJsonPath -Content $desiredVscodeJson
Write-IfChanged -Path $cursorMcpJsonPath -Content $desiredCursorJson
