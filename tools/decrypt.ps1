# tools/decrypt.ps1 - the pluggable decryption step: "encrypted file in, decrypted file out".
# The port never decrypts anything itself. Setup hands each encrypted file (the game's 1.04 EBOOT.BIN and the
# firmware's liblv2 / libsysmodule / libsre .sprx) to a decryptor program and checks what comes back:
#   1. the program exited with code 0;  2. it wrote the output file;  3. the output is an ELF.
# (The 1.04 build's own fingerprint check in rebuild_win.py comes after, as before.)
#
# The decryptor is a command template with {in} and {out}, from config.local.json's "decryptor", e.g.
#     "decryptor": "\"C:\\Tools\\mydecrypt.exe\" \"{in}\" \"{out}\""
# Default: tools\rpcs3-adapter.ps1 (RPCS3's own --decrypt, run hidden). Files that are already decrypted pass
# through the adapter unchanged.
#
#   . tools\decrypt.ps1;  Invoke-Decryptor -InPath <encrypted> -OutPath <decrypted>
#   tools\decrypt.ps1 -In <encrypted> -Out <decrypted>          (one file; exit 0 = done)
param([string]$In = "", [string]$Out = "")

# Split a decryptor template into parts, honouring double quotes:
#   powershell "-ExecutionPolicy Bypass -File tools\rpcs3-adapter.ps1" "{in}" "{out}"
#   -> powershell | -ExecutionPolicy Bypass -File tools\rpcs3-adapter.ps1 | {in} | {out}
# A pre-quoted part without {in}/{out} is fixed command-line text (it may hold several
# arguments, as the adapter's "powershell ..." line does). The {in}/{out} parts are paths.
function Split-DecryptorCommand([string]$Template) {
    $tokens = New-Object System.Collections.ArrayList
    $cur = New-Object System.Text.StringBuilder
    $quoted = $false
    $tokenQuoted = $false
    $started = $false
    foreach ($c in $Template.ToCharArray()) {
        if ($c -eq '"') { $quoted = -not $quoted; $tokenQuoted = $true; $started = $true; continue }
        if (-not $quoted -and [char]::IsWhiteSpace($c)) {
            if ($started) {
                [void]$tokens.Add([pscustomobject]@{ Text = $cur.ToString(); Quoted = $tokenQuoted })
                $cur.Clear() | Out-Null
                $started = $false; $tokenQuoted = $false
            }
            continue
        }
        [void]$cur.Append($c); $started = $true
    }
    if ($started) {
        [void]$tokens.Add([pscustomobject]@{ Text = $cur.ToString(); Quoted = $tokenQuoted })
    }
    , $tokens
}

# config.local.json's "decryptor", or "" = the default: RPCS3 through tools\rpcs3-adapter.ps1.
function Get-DecryptorTemplate {
    $root = Split-Path -Parent $PSScriptRoot
    $cfg = Join-Path $root "config.local.json"
    if (Test-Path -LiteralPath $cfg) {
        try { $d = [string](Get-Content -LiteralPath $cfg -Raw | ConvertFrom-Json).decryptor; if ($d.Trim()) { return $d } } catch {}
    }
    return ""
}

function Invoke-Decryptor {
    param([string]$InPath, [string]$OutPath)

    if ($null -eq $script:DecryptorTemplate) { $script:DecryptorTemplate = Get-DecryptorTemplate }
    if (Test-Path -LiteralPath $OutPath) { Remove-Item -LiteralPath $OutPath -Force }

    if (-not $script:DecryptorTemplate) {
        # The default: the RPCS3 adapter, each path quoted as one argument (folder names with spaces or [ ] work)
        $exe = "powershell"
        $argLine = "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $PSScriptRoot 'rpcs3-adapter.ps1')`" `"$InPath`" `"$OutPath`""
    } else {
        # A configured tool: <program> ... "{in}" ... "{out}" (quote a fixed argument that holds spaces as its own part)
        $tokens = Split-DecryptorCommand $script:DecryptorTemplate
        if ($tokens.Count -lt 1) { throw "Bad decryptor config: $script:DecryptorTemplate" }
        $exe = $tokens[0].Text
        $argList = New-Object System.Collections.ArrayList
        for ($i = 1; $i -lt $tokens.Count; $i++) {
            $t = $tokens[$i]
            if ($t.Quoted -and -not ($t.Text.Contains('{in}') -or $t.Text.Contains('{out}'))) {
                [void]$argList.Add($t.Text)
            } else {
                $a = $t.Text.Replace('{in}', $InPath).Replace('{out}', $OutPath)
                if ($a -match '\s' -or $a -eq '') { $a = '"' + $a + '"' }
                [void]$argList.Add($a)
            }
        }
        $argLine = $argList -join ' '
    }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName               = $exe
    $psi.Arguments              = $argLine
    $psi.UseShellExecute        = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.CreateNoWindow         = $true

    $p = [System.Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync()     # both streams at once: a full pipe must not stall the tool
    $stdout = $p.StandardOutput.ReadToEnd()
    $stderr = $errTask.Result
    $p.WaitForExit()

    # Check 1: tool exited cleanly
    if ($p.ExitCode -ne 0) {
        throw "The decryptor failed on '$InPath' (exit $($p.ExitCode)).`n$stdout`n$stderr"
    }

    # Check 2: it produced the file
    if (-not (Test-Path -LiteralPath $OutPath)) {
        throw "The decryptor didn't produce '$OutPath'.`n$stdout`n$stderr"
    }

    # Check 3: it's actually an ELF
    $fs  = [IO.File]::OpenRead($OutPath)
    $sig = New-Object byte[] 4
    $n   = $fs.Read($sig, 0, 4)
    $fs.Close()
    if ($n -lt 4 -or $sig[0] -ne 0x7F -or $sig[1] -ne 0x45 -or
                     $sig[2] -ne 0x4C -or $sig[3] -ne 0x46) {
        throw "The decryptor didn't produce a valid ELF ('$OutPath').`n$stdout`n$stderr"
    }
}

# Run directly for one file (rebuild_win.py uses this for the firmware modules)
if ($In -and $Out) {
    try { Invoke-Decryptor -InPath $In -OutPath $Out; exit 0 }
    catch { [Console]::Error.WriteLine("decrypt.ps1: $($_.Exception.Message)"); exit 1 }
}
