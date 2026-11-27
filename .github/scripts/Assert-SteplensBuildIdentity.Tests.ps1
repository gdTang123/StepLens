# 身份解析回归。windows-build 在 checkout 之后、耗时构建之前用 pwsh 执行本文件。
# 通过则退出 0。任一用例与预期不符则退出 1，并写出失败名。非零退出阻断该 job。
# 不替代 workflow 里对真实身份文件的两次 Assert-SteplensBuildIdentity 检查。

$ErrorActionPreference = "Stop"
. "$PSScriptRoot/Assert-SteplensBuildIdentity.ps1"

$merge = "41b77a32afd33a7d5c5b6ad4ceba711b7d69fbb2"
$tree = "5252d16286ffb49d307a34f1748c8acfc1c214ac"
$head = "0014c9a8a49c2146de76e9738f19eca82aeb11e4"
$base = "f72de23aec22e0b6941711af879e65fec795badd"
$other = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
$short = "41b77a32afd33a7d5c5b6ad4ceba711b7d69fbb"
$failures = @()

function Invoke-Case {
    param([string]$Name, [scriptblock]$Body, [string]$ExpectThrow)
    try {
        & $Body
        if ($ExpectThrow) {
            $script:failures += "$Name : expected throw matching $ExpectThrow"
        }
    } catch {
        $message = "$_"
        if (-not $ExpectThrow) {
            $script:failures += "$Name : unexpected throw $message"
        } elseif ($message -notmatch [regex]::Escape($ExpectThrow)) {
            $script:failures += "$Name : throw [$message] did not contain [$ExpectThrow]"
        }
    }
}

$lf = "sha=$merge`ntree=$tree`n"
$crlf = "sha=$merge`r`ntree=$tree`r`n"
$parents = @($base, $head)

Invoke-Case "lf-two-line" {
    Assert-SteplensBuildIdentity -IdentityText $lf -CheckoutSha $merge -CheckoutTree $tree -Parents $parents -SecondParentTree $tree
}
Invoke-Case "crlf-two-line" {
    Assert-SteplensBuildIdentity -IdentityText $crlf -CheckoutSha $merge -CheckoutTree $tree -Parents $parents -SecondParentTree $tree
}
Invoke-Case "bare-cr-rejected" {
    Assert-SteplensBuildIdentity -IdentityText "sha=$merge`rtree=$tree`r" -CheckoutSha $merge -CheckoutTree $tree -Parents $parents -SecondParentTree $tree
} "构建身份缺 sha"
Invoke-Case "missing-sha" {
    Assert-SteplensBuildIdentity -IdentityText "tree=$tree`n" -CheckoutSha $merge -CheckoutTree $tree
} "构建身份缺 sha"
Invoke-Case "missing-tree" {
    Assert-SteplensBuildIdentity -IdentityText "sha=$merge`n" -CheckoutSha $merge -CheckoutTree $tree
} "构建身份缺 tree"
Invoke-Case "short-sha" {
    Assert-SteplensBuildIdentity -IdentityText "sha=$short`ntree=$tree`n" -CheckoutSha $merge -CheckoutTree $tree
} "构建身份缺 sha"
Invoke-Case "unknown-sha" {
    Assert-SteplensBuildIdentity -IdentityText "sha=unknown`ntree=$tree`n" -CheckoutSha $merge -CheckoutTree $tree
} "构建身份缺 sha"
Invoke-Case "wrong-sha" {
    Assert-SteplensBuildIdentity -IdentityText "sha=$other`ntree=$tree`n" -CheckoutSha $merge -CheckoutTree $tree
} "不一致"
Invoke-Case "wrong-tree" {
    Assert-SteplensBuildIdentity -IdentityText "sha=$merge`ntree=$other`n" -CheckoutSha $merge -CheckoutTree $tree
} "不一致"
Invoke-Case "second-parent-tree-mismatch" {
    Assert-SteplensBuildIdentity -IdentityText $lf -CheckoutSha $merge -CheckoutTree $tree -Parents $parents -SecondParentTree $other
} "第二父"
Invoke-Case "single-line-not-accepted" {
    Assert-SteplensBuildIdentity -IdentityText "sha=$merge tree=$tree" -CheckoutSha $merge -CheckoutTree $tree
} "构建身份缺 sha"
Invoke-Case "single-line-byte-diag" {
    Assert-SteplensBuildIdentity -IdentityText "sha=$merge tree=$tree" -CheckoutSha $merge -CheckoutTree $tree
} "prefix_hex="

if ($failures.Count -gt 0) {
    Write-Host "FAILED $($failures.Count)"
    $failures | ForEach-Object { Write-Host $_ }
    exit 1
}
Write-Host "OK identity-regression"
exit 0
