# ============================================================================
# push_to_github.ps1
# Script automatizado para inicializar el repositorio Git de NppMarkdownStudio,
# generar el .gitignore de C++/CMake/Visual Studio y subir todo a tu GitHub.
# ============================================================================
param(
    [string]$RepoName = "NppMarkdownStudio",
    [ValidateSet("public", "private")]
    [string]$Visibility = "public",
    [string]$GitHubUser = ""
)

$ErrorActionPreference = "Stop"

# 1. Crear archivo .gitignore para proyectos C++ / CMake / Visual Studio
$gitignoreContent = @"
# Carpetas de compilación CMake y Visual Studio
build/
out/
.vs/
x64/
x86/
ARM64/
Debug/
Release/
RelWithDebInfo/
MinSizeRel/

# Archivos temporales y binarios de usuario
*.user
*.suo
*.aps
*.pch
*.pdb
*.ilk
*.exp
*.obj
*.ipch
*.db
packages/
"@

Set-Content -Path ".gitignore" -Value $gitignoreContent -Encoding UTF8
Write-Host "[1/4] Archivo .gitignore generado." -ForegroundColor Cyan

# 2. Inicializar repositorio Git local y crear el commit inicial
if (-not (Test-Path ".git")) {
    git init
    git branch -M main
}

git add NppMarkdownStudio.cpp NppMarkdownStudio.h CMakeLists.txt README.md .gitignore
git commit -m "Initial commit: NppMarkdownStudio native C++ plugin for Notepad++"
Write-Host "[2/4] Commit inicial creado en la rama 'main'." -ForegroundColor Cyan

# 3. Subir usando GitHub CLI (gh) si está instalado, o mediante URL remota
if (Get-Command "gh" -ErrorAction SilentlyContinue) {
    Write-Host "[3/4] Creando repositorio '$RepoName' ($Visibility) en GitHub vía GitHub CLI..." -ForegroundColor Cyan
    gh repo create $RepoName --$Visibility --source=. --remote=origin --push
    Write-Host "[4/4] ¡Proyecto subido exitosamente a GitHub!" -ForegroundColor Green
} else {
    if ([string]::IsNullOrWhiteSpace($GitHubUser)) {
        $GitHubUser = Read-Host "Ingresa tu nombre de usuario de GitHub"
    }
    $remoteUrl = "https://github.com/$GitHubUser/$RepoName.git"
    Write-Host "[3/4] Configurando remote origin: $remoteUrl" -ForegroundColor Cyan
    if ((git remote) -contains "origin") {
        git remote set-url origin $remoteUrl
    } else {
        git remote add origin $remoteUrl
    }
    Write-Host "[4/4] Ejecutando git push -u origin main..." -ForegroundColor Cyan
    git push -u origin main
    Write-Host "¡Proyecto subido exitosamente a $remoteUrl!" -ForegroundColor Green
}