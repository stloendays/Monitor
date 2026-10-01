$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
    throw "Node.js 20+ is required and 'node' was not found on PATH."
}

$nodeMajor = [int]((node --version).TrimStart('v').Split('.')[0])
if ($nodeMajor -lt 20) {
    throw "Node.js 20+ is required. Current: $(node --version)"
}

Write-Host "Installing Monitor Hub MCP dependencies..."
npm install

Write-Host "Running tests..."
npm test

Write-Host "Checking source syntax..."
node --check .\src\config.mjs
node --check .\src\bridge.mjs
node --check .\src\index.mjs

Write-Host "Monitor Hub MCP is ready."
Write-Host "Server command: node $root\src\index.mjs"
