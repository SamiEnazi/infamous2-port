# A build that keeps off the game (testspeed, 2026-11-08): tools\rebuild_win.py on the E-cores only, at
# idle priority, with few jobs, so it can run OUTSIDE the game lock while a measured run goes on --
# IF the experiment in PLAN.md ("builds off the lock") says it does not disturb the run.
#   powershell -File tools\build_bg.ps1 [-Only compile] [-Jobs 4] [-Priority Idle|BelowNormal]
# (PORT_TK / PORT_DIR / PORT_BUILD as for rebuild_win.py). This process pins itself first; every child
# (python, cmake, ninja, clang-cl, lld-link) inherits the affinity and the idle priority class.
param(
    [int]$Jobs = 4,
    [ValidateSet("Idle", "BelowNormal")][string]$Priority = "Idle",
    [string]$Only = "compile"         # rebuild_win.py --only <steps>
)
if (-not ('PortCpuSets' -as [type])) { Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class PortCpuSets {
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetSystemCpuSetInformation(IntPtr info, int len, out int ret, IntPtr proc, int flags);
  // affinity mask of the logical processors with the lowest efficiency class (the E-cores on a hybrid CPU)
  public static long ECoreMask() {
    int n; GetSystemCpuSetInformation(IntPtr.Zero, 0, out n, IntPtr.Zero, 0);
    IntPtr b = Marshal.AllocHGlobal(n); GetSystemCpuSetInformation(b, n, out n, IntPtr.Zero, 0);
    long mask = 0; int lo = 255, off = 0;
    while (off < n) { int sz = Marshal.ReadInt32(b, off); byte e = Marshal.ReadByte(b, off + 18); if (e < lo) lo = e; off += sz; }
    off = 0;
    while (off < n) { int sz = Marshal.ReadInt32(b, off); byte lp = Marshal.ReadByte(b, off + 14); byte e = Marshal.ReadByte(b, off + 18); if (e == lo) mask |= 1L << lp; off += sz; }
    Marshal.FreeHGlobal(b); return mask;
  } }
"@ }
$mask = [PortCpuSets]::ECoreMask()
$me = Get-Process -Id $PID
$me.ProcessorAffinity = [IntPtr]$mask
$me.PriorityClass = $Priority
$env:PORT_JOBS = [string]$Jobs
$RebuildArgs = @("--only", $Only)
"build_bg: E-core mask 0x{0:X}, priority {1}, {2} jobs: rebuild_win.py {3}" -f $mask, $Priority, $Jobs, ($RebuildArgs -join ' ')
& python -I (Join-Path $PSScriptRoot "rebuild_win.py") @RebuildArgs
exit $LASTEXITCODE
