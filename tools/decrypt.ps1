# tools/decrypt.ps1 - the pluggable decryption step: "encrypted file in, decrypted file out".
# The port never decrypts anything itself. Setup hands each encrypted file (the game's 1.04 EBOOT.BIN and the
# firmware's liblv2 / libsysmodule / libsre .sprx) to a decryptor program and checks what comes back:
#   1. the program exited with code 0;  2. it wrote the output file;  3. the output is an ELF.
# (The 1.04 build's own fingerprint check in rebuild_win.py comes after, as before.)
#
# The decryptor is a command template with {in} and {out}, from config.local.json's "decryptor", e.g.
#     "decryptor": "\"C:\\Tools\\mydecrypt.exe\" \"{in}\" \"{out}\""
# Default: tools\ps3tool.py (Sami Enazi's, in the public repository; tools\sony_files.py ps3tool downloads it when
# a release zip came without it), run as "python tools\ps3tool.py decrypt <in> <out>" by Setup's Python
# (work\venv, which has pycryptodome); when it is missing or fails and RPCS3 is set up (config.local.json "rpcs3"),
# tools\rpcs3-adapter.ps1 (RPCS3's own --decrypt, run hidden). Files that are already decrypted pass through the
# adapter unchanged.
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

# The Python that runs ps3tool: the slim Setup's (v0.6.0: with pycryptodome, which makes it fast), else an older
# Setup's venv, else its Python.
function Get-Ps3ToolPython {
    $root = Split-Path -Parent $PSScriptRoot
    foreach ($p in (Join-Path $root "tools-cache\slim\python\python.exe"), (Join-Path $root "work\venv\Scripts\python.exe"), (Join-Path $root "tools-cache\python\python.exe")) {
        if (Test-Path -LiteralPath $p) { return $p }
    }
    $null
}

# The default decryptors in order: "@ps3tool" (when tools\ps3tool.py and a Python are there), then "" = RPCS3 (when
# config.local.json names it, or rpcs3.exe is on PATH).
function Get-DefaultDecryptors {
    $root = Split-Path -Parent $PSScriptRoot
    $list = @()
    $tool = Join-Path $root "tools\ps3tool.py"
    $py = Get-Ps3ToolPython
    if ((Test-Path -LiteralPath $tool) -and $py) { $list += "@ps3tool" }
    $rp = $env:PORT_RPCS3   # (Setup.exe / setup_build.ps1 -Rpcs3, for this build)
    $cfg = Join-Path $root "config.local.json"
    if (-not $rp -and (Test-Path -LiteralPath $cfg)) { try { $rp = [string](Get-Content -LiteralPath $cfg -Raw | ConvertFrom-Json).rpcs3 } catch {} }
    if (($rp -and (Test-Path -LiteralPath (Join-Path $rp "rpcs3.exe"))) -or (Get-Command "rpcs3.exe" -ErrorAction SilentlyContinue)) { $list += "" }   # "" = the RPCS3 adapter
    , $list
}

# config.local.json's "decryptor", or "" = the default (Get-DefaultDecryptors: ps3tool, then RPCS3).
function Get-DecryptorTemplate {
    $root = Split-Path -Parent $PSScriptRoot
    $cfg = Join-Path $root "config.local.json"
    if (Test-Path -LiteralPath $cfg) {
        try { $d = [string](Get-Content -LiteralPath $cfg -Raw | ConvertFrom-Json).decryptor; if ($d.Trim()) { return $d } } catch {}
    }
    return ""
}

# One decryptor: a configured template, or the default ones in order (ps3tool, then RPCS3): the first that works.
function Invoke-Decryptor {
    param([string]$InPath, [string]$OutPath)

    if ($null -eq $script:DecryptorTemplate) { $script:DecryptorTemplate = Get-DecryptorTemplate }
    if ($script:DecryptorTemplate) { Invoke-DecryptorWith -Template $script:DecryptorTemplate -InPath $InPath -OutPath $OutPath; return }
    $defaults = Get-DefaultDecryptors
    if ($defaults.Count -eq 0) {
        throw "nothing can decrypt '$InPath': tools\ps3tool.py is missing (tools\sony_files.py ps3tool downloads it) and RPCS3 is not set up"
    }
    $errors = @()
    foreach ($d in $defaults) {
        try { Invoke-DecryptorWith -Template $d -InPath $InPath -OutPath $OutPath; return }
        catch { $errors += "$(if ($d) { 'ps3tool' } else { 'RPCS3' }): $($_.Exception.Message)" }
        if ($d -and $defaults.Count -gt 1) { "  ps3tool could not decrypt $(Split-Path -Leaf $InPath): trying RPCS3" }
    }
    throw ($errors -join "`n")
}

# $Template "" = the RPCS3 adapter, "@ps3tool" = python tools\ps3tool.py decrypt <in> <out>
function Invoke-DecryptorWith {
    param([string]$Template, [string]$InPath, [string]$OutPath)

    if (Test-Path -LiteralPath $OutPath) { Remove-Item -LiteralPath $OutPath -Force }

    if ($Template -eq "@ps3tool") {
        # ps3tool as a program, each path quoted as one argument (folder names with spaces or [ ] work)
        $exe = Get-Ps3ToolPython
        $tool = Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\ps3tool.py'
        $extra = ""
        # A PSN copy's license (setup_build.ps1 -Rap): handed to ps3tool by path when its "decrypt" takes one (its --help
        # names --rap); the file is never read here
        if ($script:Ps3ToolRap) {
            if ($null -eq $script:Ps3ToolDecryptHelp) {
                $script:Ps3ToolDecryptHelp = ""
                try { $script:Ps3ToolDecryptHelp = (& $exe -I $tool decrypt --help 2>&1 | Out-String) } catch {}
            }
            if ($script:Ps3ToolDecryptHelp -match '--rap\b') { $extra = " --rap `"$($script:Ps3ToolRap)`"" }
            else {
                # no --rap option: the current ps3tool looks the license up itself, in the folder PS3_RAP_DIR names
                # (an older one ignores the variable and can't unlock a PSN copy: RPCS3 is then tried, when it is there)
                $env:PS3_RAP_DIR = Split-Path -Parent $script:Ps3ToolRap
                "  your license: ps3tool looks for it in $($env:PS3_RAP_DIR)"
            }
        }
        $argLine = "-I `"$tool`" decrypt$extra `"$InPath`" `"$OutPath`""
    } elseif (-not $Template) {
        # The default: the RPCS3 adapter, each path quoted as one argument (folder names with spaces or [ ] work)
        $exe = "powershell"
        $argLine = "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $PSScriptRoot 'rpcs3-adapter.ps1')`" `"$InPath`" `"$OutPath`""
    } else {
        # A configured tool: <program> ... "{in}" ... "{out}" (quote a fixed argument that holds spaces as its own part)
        $tokens = Split-DecryptorCommand $Template
        if ($tokens.Count -lt 1) { throw "Bad decryptor config: $Template" }
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
