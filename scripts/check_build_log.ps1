# 构建日志里不能出现“异常模型 / 编译选项被覆盖”的命令行警告。
# 针串拆开写，避免本脚本自己的源码被 grep 当成命中。
param(
    [Parameter(Mandatory = $true)][string]$Log
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $Log)) {
    Write-Host "找不到构建日志 $Log"
    exit 1
}

$needle = 'D902' + '5'
$hits = @(Select-String -LiteralPath $Log -Pattern $needle -SimpleMatch)
if ($hits.Count -gt 0) {
    $hits | ForEach-Object { Write-Host $_.Line }
    Write-Host "构建日志里有 $($hits.Count) 条编译器命令行覆盖警告"
    exit 1
}

Write-Host "构建日志里的编译器命令行覆盖警告: 0"
exit 0
