# 构建身份行锚校验。只接受两行 sha=/tree=，各为精确 40 位小写十六进制。
# 只把 CRLF 收成 LF。裸 CR 不归一，行锚因此失败。不在行内搜 sha=。
# 缺字段时带上最小字节诊断（长度、CR/LF 个数、前 64 字节十六进制）。这不增加通过条件。

function Format-SteplensIdentityByteDiag {
    param([AllowNull()][string]$Text)
    if ($null -eq $Text) { $Text = "" }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
    $cr = 0
    $lf = 0
    foreach ($b in $bytes) {
        if ($b -eq 13) { $cr++ }
        elseif ($b -eq 10) { $lf++ }
    }
    $prefixLen = [Math]::Min(64, $bytes.Length)
    $hex = ""
    if ($prefixLen -gt 0) {
        $slice = New-Object byte[] $prefixLen
        [Array]::Copy($bytes, 0, $slice, 0, $prefixLen)
        $hex = ([System.BitConverter]::ToString($slice)).Replace("-", "").ToLowerInvariant()
    }
    return "bytes=$($bytes.Length) cr=$cr lf=$lf prefix_hex=$hex"
}

function Read-SteplensBuildIdentityFields {
    param([AllowNull()][string]$Text)
    if ($null -eq $Text) { $Text = "" }
    $normalized = $Text.Replace("`r`n", "`n")
    $sha = $null
    $tree = $null
    $shaMatch = [regex]::Match($normalized, "(?m)^sha=([0-9a-f]{40})$")
    if ($shaMatch.Success) { $sha = $shaMatch.Groups[1].Value }
    $treeMatch = [regex]::Match($normalized, "(?m)^tree=([0-9a-f]{40})$")
    if ($treeMatch.Success) { $tree = $treeMatch.Groups[1].Value }
    return @{ Sha = $sha; Tree = $tree }
}

function Assert-SteplensBuildIdentity {
    param(
        [AllowNull()][string]$IdentityText,
        [Parameter(Mandatory = $true)][string]$CheckoutSha,
        [Parameter(Mandatory = $true)][string]$CheckoutTree,
        [string[]]$Parents = @(),
        [string]$SecondParentTree,
        [switch]$ResolveSecondParentWithGit
    )
    if ($null -eq $IdentityText) { $IdentityText = "" }
    $fields = Read-SteplensBuildIdentityFields -Text $IdentityText
    if (-not $fields.Sha) {
        throw "构建身份缺 sha: $IdentityText diag=$(Format-SteplensIdentityByteDiag -Text $IdentityText)"
    }
    if (-not $fields.Tree) {
        throw "构建身份缺 tree: $IdentityText diag=$(Format-SteplensIdentityByteDiag -Text $IdentityText)"
    }
    $cmakeSha = $fields.Sha
    $cmakeTree = $fields.Tree
    $parentList = @($Parents | Where-Object { $_ -and $_.Trim() -ne "" })
    Write-Host "ci_identity checkout_head=$CheckoutSha checkout_tree=$CheckoutTree cmake_sha=$cmakeSha cmake_tree=$cmakeTree parent_count=$($parentList.Count) parents=$($parentList -join ' ') github_sha=$env:GITHUB_SHA"
    if ($cmakeSha -ne $CheckoutSha) {
        throw "CMake 编译 SHA $cmakeSha 与当前 checkout $CheckoutSha 不一致（过期重配/未重配）"
    }
    if ($cmakeTree -ne $CheckoutTree) {
        throw "CMake 编译 tree $cmakeTree 与当前 checkout tree $CheckoutTree 不一致"
    }
    if ($parentList.Count -ge 2) {
        $secondTree = $SecondParentTree
        if ($ResolveSecondParentWithGit) {
            $second = (git rev-parse "HEAD^2").Trim()
            $secondTree = (git rev-parse "HEAD^2^{tree}").Trim()
            Write-Host "ci_identity synthetic_merge=1 second_parent=$second second_parent_tree=$secondTree trees_equal=$($CheckoutTree -eq $secondTree)"
        } else {
            Write-Host "ci_identity synthetic_merge=1 second_parent_tree=$secondTree trees_equal=$($CheckoutTree -eq $secondTree)"
        }
        if ($CheckoutTree -ne $secondTree) {
            throw "合成 merge 树 $CheckoutTree 与第二父（受审 HEAD）树 $secondTree 不同，不能把本次身份写成受审提交"
        }
    }
}
