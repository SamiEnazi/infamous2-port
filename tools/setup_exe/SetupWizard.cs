// SetupWizard.cs -- Setup.exe of the inFAMOUS 2 Windows port: one big Install button.
//
// Install does everything, asking only what it truly cannot find out:
//   1. Setup.exe downloaded on its own: the port as ZIP files from GitHub (no git) into a folder
//      next to Setup.exe (else %LOCALAPPDATA%\inFAMOUS2-port), then Setup.exe copies itself there.
//   2. RPCS3 and the game: found on this PC (RPCS3's own game list for the game); a small "Where
//      is ...?" dialog only when one is not found.
//   3. The checks of tools\setup_common.ps1; a one-line reason only for what the player must fix
//      (the PS3 firmware or the 1.04 update missing in RPCS3, a PC that can't run the port).
//   4. tools\setup_build.ps1 -AcceptMsvcLicense -Cleanup: it downloads its own build tools into
//      tools-cache\ (portable: nothing is installed, no admin), builds, and deletes the tools again.
//      Its lines "== [n/10] <step>", "  progress: <n>% <what>", "== FAILED: <why>" drive the bar.
//   5. A big Play button.
// Pressing Install accepts Microsoft's license for the C++ files (the line under the button says so).
//
// build.cmd compiles it with the C# compiler that ships with Windows (.NET Framework 4.5+, C# 5):
// no $"", no => members, no ?., no nameof, no out var. Non-ASCII text is written as \u escapes.
//
// Test switches (README.md): --test-build, --rpcs3 <folder>, --disc <folder>, --folder <folder>,
// --no-shortcut, --no-cleanup, --only-cache, --exit-when-done, --page done|ask.

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;

[assembly: AssemblyTitle("inFAMOUS 2 PC port Setup")]
[assembly: AssemblyDescription("Builds inFAMOUS 2 for Windows from your own copy of the game")]
[assembly: AssemblyProduct("inFAMOUS 2 native Windows port (fan project)")]
[assembly: AssemblyCopyright("MIT license")]
[assembly: AssemblyVersion("2.0.0.0")]
[assembly: AssemblyFileVersion("2.0.0.0")]

namespace Inf2Setup
{
    // ------------------------------------------------------------------ start

    static class Program
    {
        [STAThread]
        static int Main(string[] args)
        {
            try { Native.SetProcessDPIAware(); } catch (Exception) { }
            // GitHub needs TLS 1.2; an app built without a target framework gets the old default
            try { ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072; } catch (Exception) { }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
            Application.ThreadException += delegate(object s, ThreadExceptionEventArgs e) { Oops(e.Exception); };
            AppDomain.CurrentDomain.UnhandledException += delegate(object s, UnhandledExceptionEventArgs e) { Oops(e.ExceptionObject as Exception); };

            Opts opt = Opts.Parse(args);
            Theme.Init(FitScale());
            string exeDir = Path.GetDirectoryName(Path.GetFullPath(Application.ExecutablePath));
            bool first;
            // one Setup per folder: two builds in the same folder would trip over each other
            using (Mutex m = new Mutex(true, "inFAMOUS2-port-setup-" + Util.Hash(exeDir.ToLowerInvariant()), out first))
            {
                if (!first)
                {
                    MessageBox.Show("Setup is already open.", MainForm.Title, MessageBoxButtons.OK, MessageBoxIcon.Information);
                    return 3;
                }
                MainForm f = new MainForm(opt, exeDir);
                Application.Run(f);
                return f.ExitCode;
            }
        }

        // The screen's DPI scale, lowered when the window would not fit.
        static float FitScale()
        {
            float s = 1f;
            try { using (Graphics g = Graphics.FromHwnd(IntPtr.Zero)) s = g.DpiX / 96f; } catch (Exception) { }
            try
            {
                Rectangle wa = Screen.PrimaryScreen.WorkingArea;
                float fitH = (wa.Height - 16) / (float)(MainForm.BaseH + MainForm.LogH + 40);
                float fitW = (wa.Width - 16) / (float)(MainForm.BaseW + 16);
                s = Math.Min(s, Math.Min(fitH, fitW));
            }
            catch (Exception) { }
            return Math.Max(0.75f, s);
        }

        static void Oops(Exception e)
        {
            try
            {
                MessageBox.Show("Setup ran into a problem it did not expect:\n\n" + (e != null ? e.Message : "unknown error") +
                    "\n\nPlease report it on GitHub (SamiEnazi/infamous2-port, Issues) with a screenshot of this message.",
                    MainForm.Title, MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            catch (Exception) { }
        }
    }

    // Command-line switches, for testing (README.md).
    sealed class Opts
    {
        public string Page, Disc, Rpcs3, Folder;
        public bool TestBuild, NoShortcut, ExitWhenDone, NoCleanup, OnlyCache;

        public static Opts Parse(string[] a)
        {
            Opts o = new Opts();
            for (int i = 0; i < a.Length; i++)
            {
                string k = a[i].ToLowerInvariant();
                string v = i + 1 < a.Length ? a[i + 1] : null;
                switch (k)
                {
                    case "--page": o.Page = v; i++; break;
                    case "--disc": o.Disc = v; i++; break;
                    case "--rpcs3": o.Rpcs3 = v; i++; break;
                    case "--folder": o.Folder = v; i++; break;
                    case "--test-build": o.TestBuild = true; break;
                    case "--no-shortcut": o.NoShortcut = true; break;
                    case "--exit-when-done": o.ExitWhenDone = true; break;
                    case "--no-cleanup": o.NoCleanup = true; break;
                    case "--only-cache": o.OnlyCache = true; break;
                }
            }
            return o;
        }
    }

    // ------------------------------------------------------------------ look

    // The game's menu theme (toolkit vk_overlay_theme.cpp, "hero"): charcoal panels, electric blue.
    static class Theme
    {
        public static readonly Color Bg = Color.FromArgb(14, 16, 21);
        public static readonly Color Card = Color.FromArgb(22, 26, 34);
        public static readonly Color CardHi = Color.FromArgb(30, 36, 47);
        public static readonly Color Line = Color.FromArgb(44, 52, 67);
        public static readonly Color Text = Color.FromArgb(236, 241, 247);
        public static readonly Color Dim = Color.FromArgb(153, 166, 184);
        public static readonly Color Accent = Color.FromArgb(63, 169, 255);
        public static readonly Color AccentHover = Color.FromArgb(104, 188, 255);
        public static readonly Color AccentDown = Color.FromArgb(40, 140, 225);
        public static readonly Color AccentHi = Color.FromArgb(150, 210, 255);
        public static readonly Color SelBg = Color.FromArgb(17, 40, 64);
        public static readonly Color Good = Color.FromArgb(74, 201, 120);
        public static readonly Color Bad = Color.FromArgb(255, 110, 110);
        public static readonly Color Ink = Color.FromArgb(6, 18, 32);      // text on the blue
        public static readonly Color LogBg = Color.FromArgb(9, 11, 15);

        public static float Scale = 1f;
        public static Font Body, Small, Bold, H1, H2, Btn, BtnHuge, Mono;

        // Fonts in pixels times Scale, so text and layout always scale together.
        public static void Init(float scale)
        {
            Scale = scale;
            Body = Px("Segoe UI", 14.5f, FontStyle.Regular);
            Small = Px("Segoe UI", 12.5f, FontStyle.Regular);
            Bold = Px("Segoe UI", 15f, FontStyle.Bold);
            H1 = Px("Segoe UI", 34f, FontStyle.Bold);
            H2 = Px("Segoe UI", 17f, FontStyle.Bold);
            Btn = Px("Segoe UI", 13.5f, FontStyle.Bold);
            BtnHuge = Px("Segoe UI", 24f, FontStyle.Bold);
            Mono = Px("Consolas", 12f, FontStyle.Regular);
        }

        static Font Px(string face, float px, FontStyle st) { return new Font(face, px * Scale, st, GraphicsUnit.Pixel); }

        public static int S(double v) { return (int)Math.Round(v * Scale); }

        public static GraphicsPath Round(RectangleF r, float radius)
        {
            GraphicsPath p = new GraphicsPath();
            float d = Math.Min(radius * 2, Math.Min(r.Width, r.Height));
            if (d <= 1) { p.AddRectangle(r); return p; }
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        public static LinkLabel Link(string text, Font f)
        {
            LinkLabel l = new LinkLabel();
            l.Text = text; l.Font = f; l.AutoSize = true; l.UseMnemonic = false;
            l.LinkColor = Accent; l.ActiveLinkColor = AccentHi; l.VisitedLinkColor = Accent;
            l.LinkBehavior = LinkBehavior.HoverUnderline; l.ForeColor = Dim;
            l.BackColor = Bg;
            return l;
        }
    }

    interface IAnimated { bool Animating { get; } }

    // One shared animation clock for the progress bar's shine.
    static class Anim
    {
        public static int Frame;
        static readonly List<Control> live = new List<Control>();

        public static void Add(Control c)
        {
            live.Add(c);
            c.Disposed += delegate { live.Remove(c); };
        }

        public static void Tick()
        {
            Frame++;
            foreach (Control c in live.ToArray())
            {
                IAnimated a = c as IAnimated;
                if (a != null && a.Animating && c.Visible && c.IsHandleCreated) c.Invalidate();
            }
        }
    }

    static class Native
    {
        [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
        [DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);
        [DllImport("kernel32.dll")] static extern uint SetThreadExecutionState(uint flags);

        // Keep the PC from going to sleep during a long build.
        public static void KeepAwake(bool on)
        {
            try { SetThreadExecutionState(on ? 0x80000001u : 0x80000000u); } catch (Exception) { }
        }

        // A dark title bar to match (Windows 10 1809+ and 11; ignored elsewhere).
        public static void DarkTitle(IntPtr h, Color caption)
        {
            try
            {
                int on = 1;
                if (DwmSetWindowAttribute(h, 20, ref on, 4) != 0) DwmSetWindowAttribute(h, 19, ref on, 4);
                int c = caption.R | (caption.G << 8) | (caption.B << 16);
                DwmSetWindowAttribute(h, 35, ref c, 4);
            }
            catch (Exception) { }
        }
    }

    // The icon: embedded by build.cmd (/resource), else tools\setup.ico beside Setup.exe.
    static class Res
    {
        static byte[] ico;

        static byte[] IcoBytes(string dir)
        {
            if (ico != null) return ico;
            try
            {
                using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream("setup.ico"))
                {
                    if (s != null) { MemoryStream m = new MemoryStream(); s.CopyTo(m); ico = m.ToArray(); return ico; }
                }
            }
            catch (Exception) { }
            try
            {
                string p = Path.Combine(dir, "tools\\setup.ico");
                if (File.Exists(p)) ico = File.ReadAllBytes(p);
            }
            catch (Exception) { }
            return ico;
        }

        public static Icon WindowIcon(string dir)
        {
            byte[] b = IcoBytes(dir);
            if (b != null) { try { return new Icon(new MemoryStream(b)); } catch (Exception) { } }
            try { return Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { return null; }
        }

        // The biggest picture in the .ico (the 256 px one is a PNG), for the logo.
        public static Image Logo(string dir)
        {
            byte[] b = IcoBytes(dir);
            if (b == null || b.Length < 22) return null;
            try
            {
                int n = BitConverter.ToUInt16(b, 4), best = -1, bestW = 0;
                for (int i = 0; i < n; i++)
                {
                    int o = 6 + 16 * i;
                    int w = b[o] == 0 ? 256 : b[o];
                    if (w > bestW) { bestW = w; best = o; }
                }
                if (best < 0) return null;
                int size = (int)BitConverter.ToUInt32(b, best + 8), off = (int)BitConverter.ToUInt32(b, best + 12);
                if (b[off] == 0x89 && b[off + 1] == 0x50)
                {
                    using (MemoryStream m = new MemoryStream(b, off, size))
                    using (Image img = Image.FromStream(m))
                        return new Bitmap(img);
                }
                using (Icon ic = new Icon(new MemoryStream(b), bestW, bestW)) return ic.ToBitmap();
            }
            catch (Exception) { return null; }
        }
    }

    // ------------------------------------------------------------------ helpers

    static class Util
    {
        public static readonly string[] Titles = { "BCES01143", "BCES01144", "BCES01229", "BCUS98125", "NPUA80638" };

        // Re-read PATH from the registry (Machine + User), so the checks see what the PC has now.
        public static void RefreshPath()
        {
            try
            {
                string m = Environment.GetEnvironmentVariable("Path", EnvironmentVariableTarget.Machine) ?? "";
                string u = Environment.GetEnvironmentVariable("Path", EnvironmentVariableTarget.User) ?? "";
                string cur = Environment.GetEnvironmentVariable("Path") ?? "";
                HashSet<string> seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                List<string> parts = new List<string>();
                foreach (string s in (m + ";" + u + ";" + cur).Split(';'))
                {
                    string t = Environment.ExpandEnvironmentVariables(s.Trim());
                    if (t.Length == 0) continue;
                    if (seen.Add(t.TrimEnd('\\'))) parts.Add(t);
                }
                Environment.SetEnvironmentVariable("Path", string.Join(";", parts));
            }
            catch (Exception) { }
        }

        public static string PowerShellExe()
        {
            string p = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell\\v1.0\\powershell.exe");
            return File.Exists(p) ? p : "powershell.exe";
        }

        // Only Windows' own folders: the build's PATH in the --only-cache test.
        public static string WindowsPath()
        {
            string w = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
            return string.Join(";", new string[] { Path.Combine(w, "System32"), w, Path.Combine(w, "System32\\Wbem"),
                                                   Path.Combine(w, "System32\\WindowsPowerShell\\v1.0") });
        }

        // A PowerShell single-quoted string.
        public static string Q(string s) { return "'" + (s ?? "").Replace("'", "''") + "'"; }

        public static bool HasPort(string dir)
        {
            try { return !string.IsNullOrEmpty(dir) && File.Exists(Path.Combine(dir, "tools\\setup_common.ps1")); }
            catch (Exception) { return false; }
        }

        // The port with its toolkit and port base (a finished download or a git clone).
        public static bool PortComplete(string dir)
        {
            try
            {
                return HasPort(dir) && File.Exists(Path.Combine(dir, "toolkit\\CMakeLists.txt")) &&
                       File.Exists(Path.Combine(dir, "community\\erikomis-infamous2-recomp\\infamous2-lle\\CMakeLists.txt"));
            }
            catch (Exception) { return false; }
        }

        public static string BuiltExe(string repo) { return Path.Combine(repo, "work\\port\\build\\Infamous2Recomp.exe"); }

        // Where a downloaded-alone Setup.exe puts the game: next to itself, else in the user's local
        // app data, else on the system drive; a plain path (build tools dislike other alphabets) with
        // about 15 GB free. An earlier download is used again.
        public static string DefaultTarget(string exeDir)
        {
            string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            string sys = Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.Windows));
            if (string.IsNullOrEmpty(sys)) sys = "C:\\";
            string[] cands = { Path.Combine(exeDir, "inFAMOUS2-port"), Path.Combine(local, "inFAMOUS2-port"), Path.Combine(sys, "inFAMOUS2-port") };
            foreach (string c in cands) if (HasPort(c)) return c;
            foreach (string c in cands)
            {
                try
                {
                    if (!IsAscii(c)) continue;
                    if (Directory.Exists(c) && !EmptyDir(c)) continue;
                    DriveInfo di = new DriveInfo(Path.GetPathRoot(c));
                    if (!di.IsReady || di.AvailableFreeSpace < (15L << 30)) continue;
                    string parent = Path.GetDirectoryName(c);
                    Directory.CreateDirectory(parent);
                    string probe = Path.Combine(parent, ".inf2-setup-probe");
                    File.WriteAllText(probe, "");
                    File.Delete(probe);
                    return c;
                }
                catch (Exception) { }
            }
            return cands[1];
        }

        // config.local.json is written by setup_build.ps1 (it keeps "rpcs3").
        public static string ReadConfig(string repo, string key)
        {
            try
            {
                string f = Path.Combine(repo, "config.local.json");
                if (!File.Exists(f)) return null;
                Dictionary<string, object> d = new JavaScriptSerializer().DeserializeObject(File.ReadAllText(f)) as Dictionary<string, object>;
                object v;
                if (d != null && d.TryGetValue(key, out v) && v is string) return (string)v;
            }
            catch (Exception) { }
            return null;
        }

        static bool IsRpcs3(string d)
        {
            try { return !string.IsNullOrEmpty(d) && File.Exists(Path.Combine(d, "rpcs3.exe")); } catch (Exception) { return false; }
        }

        // rpcs3.exe in dir or one folder below it (or two, for the top of a drive)
        static string Look(string dir, int depth, Stopwatch sw)
        {
            if (sw.ElapsedMilliseconds > 6000 || string.IsNullOrEmpty(dir)) return null;
            try
            {
                if (!Directory.Exists(dir)) return null;
                if (IsRpcs3(dir)) return dir;
                if (depth == 0) return null;
                foreach (string sub in Directory.GetDirectories(dir))
                {
                    string n = Path.GetFileName(sub);
                    if (n.StartsWith("$") || n.Equals("Windows", StringComparison.OrdinalIgnoreCase) ||
                        n.Equals("System Volume Information", StringComparison.OrdinalIgnoreCase) ||
                        n.Equals("ProgramData", StringComparison.OrdinalIgnoreCase) || n.Equals("Users", StringComparison.OrdinalIgnoreCase))
                        continue;
                    string r = Look(sub, depth - 1, sw);
                    if (r != null) return r;
                }
            }
            catch (Exception) { }
            return null;
        }

        // RPCS3 is portable: the saved choice, a running RPCS3, next to Setup.exe, the user's folders,
        // Program Files, the top of each drive. A few seconds at most.
        public static string FindRpcs3(string exeDir, string repo)
        {
            if (repo != null) { string c = ReadConfig(repo, "rpcs3"); if (IsRpcs3(c)) return c; }
            try
            {
                foreach (Process p in Process.GetProcessesByName("rpcs3"))
                {
                    try { string d = Path.GetDirectoryName(p.MainModule.FileName); if (IsRpcs3(d)) return d; } catch (Exception) { }
                }
            }
            catch (Exception) { }
            Stopwatch sw = Stopwatch.StartNew();
            string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            List<string> one = new List<string>();
            one.Add(exeDir);
            try { one.Add(Path.GetDirectoryName(exeDir)); } catch (Exception) { }
            foreach (string sub in new string[] { "Desktop", "Downloads", "Documents", "" }) one.Add(Path.Combine(home, sub));
            one.Add(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles));
            one.Add(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86));
            foreach (string d in one) { string r = Look(d, 1, sw); if (r != null) return r; }
            List<string> roots = new List<string>();
            try
            {
                foreach (DriveInfo di in DriveInfo.GetDrives())
                    if (di.DriveType == DriveType.Fixed && di.IsReady) roots.Add(di.RootDirectory.FullName);
            }
            catch (Exception) { }
            roots.Sort(delegate(string a, string b) { return string.Compare(a, b, StringComparison.OrdinalIgnoreCase); });
            foreach (string d in roots) { string r = Look(d, 2, sw); if (r != null) return r; }
            return null;
        }

        public static bool IsDisc(string d)
        {
            try { return !string.IsNullOrEmpty(d) && File.Exists(Path.Combine(d, "PS3_GAME\\PARAM.SFO")); } catch (Exception) { return false; }
        }

        // The disc folder from RPCS3's game list: config\games.yml, lines like "BCES01143: <folder>/".
        public static string DetectDisc(string rpcs3)
        {
            try
            {
                if (string.IsNullOrEmpty(rpcs3)) return null;
                string yml = Path.Combine(rpcs3, "config\\games.yml");
                if (!File.Exists(yml)) return null;
                Dictionary<string, string> map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                foreach (string raw in File.ReadAllLines(yml))
                {
                    int c = raw.IndexOf(':');
                    if (c <= 0) continue;
                    string key = raw.Substring(0, c).Trim().Trim('"', '\'');
                    string val = raw.Substring(c + 1).Trim().Trim('"', '\'');
                    if (val.Length == 0) continue;
                    if (val.StartsWith("$(EmulatorDir)", StringComparison.OrdinalIgnoreCase)) val = Path.Combine(rpcs3, val.Substring(14).TrimStart('/', '\\'));
                    val = val.Replace('/', '\\').TrimEnd('\\');
                    if (!Path.IsPathRooted(val)) val = Path.Combine(rpcs3, val);
                    map[key] = val;
                }
                foreach (string id in Titles)
                {
                    string v;
                    if (map.TryGetValue(id, out v) && IsDisc(v)) return v;
                }
            }
            catch (Exception) { }
            return null;
        }

        // Forgiving game-folder pick: the PS3_GAME folder itself, or the folder one level above the disc.
        public static string NormalizeDisc(string p)
        {
            if (string.IsNullOrEmpty(p)) return p;
            try
            {
                p = p.TrimEnd('\\');
                if (string.Equals(Path.GetFileName(p), "PS3_GAME", StringComparison.OrdinalIgnoreCase)) return Path.GetDirectoryName(p);
                if (IsDisc(p)) return p;
                foreach (string id in Titles)
                    if (IsDisc(Path.Combine(p, id))) return Path.Combine(p, id);
                foreach (string d in Directory.GetDirectories(p))
                    if (IsDisc(d)) return d;
            }
            catch (Exception) { }
            return p;
        }

        public static bool IsAscii(string s)
        {
            foreach (char ch in s) if (ch > 126) return false;
            return true;
        }

        public static bool SamePath(string a, string b)
        {
            try { return string.Equals(Path.GetFullPath(a).TrimEnd('\\'), Path.GetFullPath(b).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase); }
            catch (Exception) { return false; }
        }

        public static bool EmptyDir(string d)
        {
            try { using (IEnumerator<string> en = Directory.EnumerateFileSystemEntries(d).GetEnumerator()) return !en.MoveNext(); }
            catch (Exception) { return false; }
        }

        public static void OpenUrl(string url)
        {
            try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); } catch (Exception) { }
        }

        public static void KillTree(int pid)
        {
            try
            {
                ProcessStartInfo si = new ProcessStartInfo("taskkill.exe", "/T /F /PID " + pid);
                si.UseShellExecute = false; si.CreateNoWindow = true;
                using (Process p = Process.Start(si)) p.WaitForExit(15000);
            }
            catch (Exception) { }
        }

        public static string Hash(string s)
        {
            uint h = 2166136261;
            foreach (char c in s) { h ^= c; h *= 16777619; }
            return h.ToString("x8");
        }

        // "[k/N] ..." as ninja prints it (N > 10, so cmake's "[0/2] Re-checking" is skipped).
        public static bool Ninja(string t, out int k, out int n)
        {
            k = n = 0;
            if (t.Length < 5 || t[0] != '[') return false;
            int i = 1;
            while (i < t.Length && char.IsDigit(t[i])) k = k * 10 + (t[i++] - '0');
            if (i == 1 || i >= t.Length || t[i] != '/') return false;
            int s = ++i;
            while (i < t.Length && char.IsDigit(t[i])) n = n * 10 + (t[i++] - '0');
            return i > s && i < t.Length && t[i] == ']' && n > 10 && k <= n;
        }

        // "... 45% ..." -> 45, else -1
        public static int Percent(string t)
        {
            int p = t.IndexOf('%');
            if (p < 1) return -1;
            int s = p;
            while (s > 0 && char.IsDigit(t[s - 1])) s--;
            int v;
            return p > s && int.TryParse(t.Substring(s, p - s), out v) ? v : -1;
        }

        // Run a hidden program and collect its output (used for the checks).
        public static int Capture(string file, string args, string dir, int timeoutMs, out string stdout, out string stderr)
        {
            ProcessStartInfo si = new ProcessStartInfo(file, args);
            si.UseShellExecute = false; si.CreateNoWindow = true;
            si.RedirectStandardOutput = true; si.RedirectStandardError = true; si.RedirectStandardInput = true;
            si.StandardOutputEncoding = Encoding.UTF8; si.StandardErrorEncoding = Encoding.UTF8;
            if (dir != null && Directory.Exists(dir)) si.WorkingDirectory = dir;
            using (Process p = Process.Start(si))
            {
                try { p.StandardInput.Close(); } catch (Exception) { }
                string o = "", e = "";
                Thread to = new Thread(delegate() { try { o = p.StandardOutput.ReadToEnd(); } catch (Exception) { } });
                Thread te = new Thread(delegate() { try { e = p.StandardError.ReadToEnd(); } catch (Exception) { } });
                to.IsBackground = true; te.IsBackground = true;
                to.Start(); te.Start();
                if (!p.WaitForExit(timeoutMs))
                {
                    KillTree(p.Id);
                    to.Join(3000); te.Join(3000);
                    stdout = o; stderr = "it did not answer within " + (timeoutMs / 1000) + " seconds";
                    return -1;
                }
                to.Join(10000); te.Join(10000);
                stdout = o; stderr = e;
                return p.ExitCode;
            }
        }
    }

    // ------------------------------------------------------------------ the checks

    sealed class Row
    {
        public bool? Ok;
        public string Name = "", Detail = "";
    }

    static class Checker
    {
        // Get-SetupChecks of tools\setup_common.ps1, as JSON, in a hidden PowerShell.
        public static List<Row> Run(string repo, string disc, string rpcs3, out string error)
        {
            error = null;
            List<Row> rows = new List<Row>();
            try
            {
                string common = Path.Combine(repo, "tools\\setup_common.ps1");
                if (!File.Exists(common)) { error = "tools\\setup_common.ps1 is missing in " + repo; return rows; }
                string cmd = "& { [Console]::OutputEncoding = [Text.Encoding]::UTF8; . " + Util.Q(common) +
                             "; Get-SetupChecks -Disc " + Util.Q(disc) + " -Rpcs3 " + Util.Q(rpcs3) + " | ConvertTo-Json -Depth 3 }";
                string so, se;
                int code = Util.Capture(Util.PowerShellExe(), "-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" + cmd + "\"",
                                        repo, 180000, out so, out se);
                int a = so.IndexOf('['), b = so.IndexOf('{');
                int start = a >= 0 && (b < 0 || a < b) ? a : b;
                if (start < 0)
                {
                    string first = (se ?? "").Trim();
                    int nl = first.IndexOf('\n');
                    if (nl > 0) first = first.Substring(0, nl).Trim();
                    error = first.Length > 0 ? first : "PowerShell gave no answer (exit code " + code + ")";
                    return rows;
                }
                object o = new JavaScriptSerializer().DeserializeObject(so.Substring(start));
                object[] arr = o as object[];
                if (arr == null && o is Dictionary<string, object>) arr = new object[] { o };
                if (arr == null) { error = "the checker's answer could not be read"; return rows; }
                foreach (object x in arr)
                {
                    Dictionary<string, object> d = x as Dictionary<string, object>;
                    if (d == null) continue;
                    Row r = new Row();
                    object v;
                    if (d.TryGetValue("Ok", out v) && v is bool) r.Ok = (bool)v;
                    r.Name = Str(d, "Name");
                    r.Detail = Str(d, "Detail");
                    rows.Add(r);
                }
            }
            catch (Exception ex) { error = ex.Message; }
            return rows;
        }

        static string Str(Dictionary<string, object> d, string k)
        {
            object v;
            if (!d.TryGetValue(k, out v) || v == null) return "";
            object[] arr = v as object[];
            if (arr != null)
            {
                List<string> parts = new List<string>();
                foreach (object p in arr) parts.Add(Convert.ToString(p));
                return string.Join(" ", parts);
            }
            return Convert.ToString(v);
        }
    }

    // ------------------------------------------------------------------ running work

    // Work running in the background; its output lines are queued for the UI timer.
    interface IJob
    {
        ConcurrentQueue<string> Lines { get; }
        bool Exited { get; }
        int ExitCode { get; }
        void Kill();
    }

    // A hidden program, read on background threads (the window never waits). A segment ended by a
    // lone \r (a progress meter) is dropped: the scripts print whole lines.
    sealed class Job : IJob
    {
        readonly ConcurrentQueue<string> lines = new ConcurrentQueue<string>();
        volatile bool exited;
        int exitCode = -1;
        Process proc;

        public ConcurrentQueue<string> Lines { get { return lines; } }
        public bool Exited { get { return exited; } }
        public int ExitCode { get { return exitCode; } }

        public void Start(string file, string args, string dir, IDictionary<string, string> env)
        {
            ProcessStartInfo si = new ProcessStartInfo(file, args);
            si.UseShellExecute = false; si.CreateNoWindow = true;
            si.RedirectStandardOutput = true; si.RedirectStandardError = true; si.RedirectStandardInput = true;
            si.StandardOutputEncoding = Encoding.UTF8; si.StandardErrorEncoding = Encoding.UTF8;
            if (dir != null && Directory.Exists(dir)) si.WorkingDirectory = dir;
            if (env != null) foreach (KeyValuePair<string, string> kv in env) si.EnvironmentVariables[kv.Key] = kv.Value;
            proc = Process.Start(si);
            try { proc.StandardInput.Close(); } catch (Exception) { }   // nothing may wait for typed input
            Thread a = Pump(proc.StandardOutput), b = Pump(proc.StandardError);
            Thread w = new Thread(delegate()
            {
                a.Join(); b.Join();
                try { proc.WaitForExit(); exitCode = proc.ExitCode; } catch (Exception) { exitCode = -1; }
                exited = true;
            });
            w.IsBackground = true;
            w.Start();
        }

        Thread Pump(StreamReader r)
        {
            Thread t = new Thread(delegate()
            {
                char[] buf = new char[8192];
                StringBuilder sb = new StringBuilder();
                bool cr = false;
                try
                {
                    int n;
                    while ((n = r.Read(buf, 0, buf.Length)) > 0)
                    {
                        for (int i = 0; i < n; i++)
                        {
                            char c = buf[i];
                            if (cr)
                            {
                                cr = false;
                                if (c == '\n') { lines.Enqueue(sb.ToString()); sb.Length = 0; continue; }
                                sb.Length = 0;                       // "\r" alone: a meter redrawing itself
                            }
                            if (c == '\r') cr = true;
                            else if (c == '\n') { lines.Enqueue(sb.ToString()); sb.Length = 0; }
                            else sb.Append(c);
                        }
                    }
                }
                catch (Exception) { }
                if (sb.Length > 0) lines.Enqueue(sb.ToString());
            });
            t.IsBackground = true;
            t.Start();
            return t;
        }

        public void Kill()
        {
            try { if (proc != null && !proc.HasExited) Util.KillTree(proc.Id); } catch (Exception) { }
        }
    }

    // The port as ZIP files from GitHub, no git: the port at the newest commit of main, and the
    // toolkit and the port base at the commits that commit pins (GitHub's API names them). Each part
    // is unpacked next to its place and moved in when complete, so a stopped download never leaves a
    // half folder that looks whole. Lines: "== [n/4] ...", "progress: <n>% ...", "error: <why>".
    sealed class PortDownload : IJob
    {
        const string Repo = "SamiEnazi/infamous2-port";
        readonly ConcurrentQueue<string> lines = new ConcurrentQueue<string>();
        volatile bool exited, cancel;
        int exitCode = -1;
        readonly string target;
        long doneBytes;
        const long TotalGuess = 38L << 20;      // the three ZIP files together (GitHub sends no sizes)

        public PortDownload(string target) { this.target = target; }

        public ConcurrentQueue<string> Lines { get { return lines; } }
        public bool Exited { get { return exited; } }
        public int ExitCode { get { return exitCode; } }
        public void Kill() { cancel = true; }

        public void Start()
        {
            Thread t = new Thread(Run);
            t.IsBackground = true;
            t.Start();
        }

        void Say(string s) { lines.Enqueue(s); }

        void Run()
        {
            try
            {
                Say("== [1/4] asking GitHub for the newest version");
                string commit = Sha(Api("repos/" + Repo + "/commits/main"));
                string tk = Sha(Api("repos/" + Repo + "/contents/toolkit?ref=" + commit));
                string cm = Sha(Api("repos/" + Repo + "/contents/community/erikomis-infamous2-recomp?ref=" + commit));
                Say("  port " + commit + ", toolkit " + tk + ", port base " + cm);
                string temp = Path.Combine(Path.GetTempPath(), "inFAMOUS2-setup-download");
                Directory.CreateDirectory(temp);
                Part(2, "the port", Repo, commit, target, temp);
                Part(3, "the toolkit", "SamiEnazi/ps3recomp", tk, Path.Combine(target, "toolkit"), temp);
                Part(4, "the port base", "erikomis/infamous2-recomp", cm, Path.Combine(target, "community\\erikomis-infamous2-recomp"), temp);
                File.WriteAllText(Path.Combine(target, "port-version.txt"),
                    "Downloaded by Setup.exe " + DateTime.Now.ToString("yyyy-MM-dd HH:mm") + "\r\n" + Repo + " " + commit +
                    "\r\ntoolkit SamiEnazi/ps3recomp " + tk + "\r\ncommunity/erikomis-infamous2-recomp erikomis/infamous2-recomp " + cm + "\r\n");
                try { Directory.Delete(temp, true); } catch (Exception) { }
                Say("progress: 100% downloaded");
                exitCode = 0;
            }
            catch (Exception ex)
            {
                Say("error: " + (cancel ? "stopped" : ex.Message));
                exitCode = 1;
            }
            exited = true;
        }

        // One part: the ZIP of repo at sha, unpacked into dest. Skipped when dest already has it.
        void Part(int step, string what, string repo, string sha, string dest, string temp)
        {
            Say("== [" + step + "/4] downloading " + what);
            string mark = Path.Combine(dest, ".zip-source");
            if (File.Exists(mark) && File.ReadAllText(mark).Trim() == repo + " " + sha) { Say("  " + what + ": already there"); return; }
            string zip = Path.Combine(temp, repo.Replace('/', '-') + ".zip");
            Download("https://codeload.github.com/" + repo + "/zip/" + sha, zip, what);
            Say("progress: " + Pct() + "% unpacking " + what);
            string tmp = dest.TrimEnd('\\') + ".unzip";
            if (Directory.Exists(tmp)) Directory.Delete(tmp, true);
            string tmpFull = Path.GetFullPath(tmp).TrimEnd('\\') + "\\";
            using (ZipArchive za = ZipFile.OpenRead(zip))
            {
                foreach (ZipArchiveEntry e in za.Entries)
                {
                    if (cancel) throw new OperationCanceledException();
                    string name = e.FullName.Replace('/', '\\');
                    int cut = name.IndexOf('\\');
                    if (cut < 0) continue;
                    name = name.Substring(cut + 1);                 // without the archive's top folder
                    if (name.Length == 0) continue;
                    string outPath = Path.GetFullPath(Path.Combine(tmp, name));
                    if (!outPath.StartsWith(tmpFull, StringComparison.OrdinalIgnoreCase)) throw new IOException("a bad path in the ZIP file: " + e.FullName);
                    if (name.EndsWith("\\")) { Directory.CreateDirectory(outPath); continue; }
                    Directory.CreateDirectory(Path.GetDirectoryName(outPath));
                    e.ExtractToFile(outPath, true);
                }
            }
            File.WriteAllText(Path.Combine(tmp, ".zip-source"), repo + " " + sha);
            if (Directory.Exists(dest))
            {
                if (!Util.EmptyDir(dest)) throw new IOException(dest + " is not empty");
                Directory.Delete(dest);
            }
            Directory.CreateDirectory(Path.GetDirectoryName(dest.TrimEnd('\\')));
            Directory.Move(tmp, dest);
            File.Delete(zip);
            Say("  " + what + ": " + repo + " " + sha.Substring(0, 7));
        }

        int Pct() { return (int)Math.Min(99, 100 * doneBytes / TotalGuess); }

        HttpWebRequest Request(string url)
        {
            HttpWebRequest r = (HttpWebRequest)WebRequest.Create(url);
            r.UserAgent = "inFAMOUS2-port-setup";
            r.Timeout = 60000; r.ReadWriteTimeout = 120000;
            return r;
        }

        // GitHub's API (no sign-in: 60 questions an hour per network, Setup asks 3)
        string Api(string path)
        {
            for (int attempt = 1; ; attempt++)
            {
                try
                {
                    HttpWebRequest r = Request("https://api.github.com/" + path);
                    r.Accept = "application/vnd.github+json";
                    using (WebResponse resp = r.GetResponse())
                    using (StreamReader sr = new StreamReader(resp.GetResponseStream(), Encoding.UTF8))
                        return sr.ReadToEnd();
                }
                catch (WebException ex)
                {
                    HttpWebResponse hr = ex.Response as HttpWebResponse;
                    if (hr != null && (int)hr.StatusCode == 403)
                        throw new IOException("GitHub is limiting questions from this network right now. Wait a few minutes, then press Try again.");
                    if (attempt == 2 || cancel) throw new IOException("could not reach GitHub (" + ex.Message + "). Check the internet connection.");
                    Say("  GitHub: " + ex.Message + " -- trying once more");
                    Thread.Sleep(3000);
                }
            }
        }

        static string Sha(string json)
        {
            Dictionary<string, object> d = new JavaScriptSerializer().DeserializeObject(json) as Dictionary<string, object>;
            object v;
            if (d == null || !d.TryGetValue("sha", out v) || !(v is string) || ((string)v).Length < 7) throw new IOException("GitHub's answer has no commit");
            return (string)v;
        }

        void Download(string url, string path, string what)
        {
            long start = doneBytes;
            for (int attempt = 1; ; attempt++)
            {
                try
                {
                    doneBytes = start;
                    HttpWebRequest r = Request(url);
                    using (WebResponse resp = r.GetResponse())
                    using (Stream s = resp.GetResponseStream())
                    using (FileStream f = File.Create(path))
                    {
                        byte[] buf = new byte[1 << 16];
                        int n;
                        long got = 0;
                        DateTime last = DateTime.MinValue;
                        while ((n = s.Read(buf, 0, buf.Length)) > 0)
                        {
                            if (cancel) throw new OperationCanceledException();
                            f.Write(buf, 0, n);
                            got += n; doneBytes += n;
                            if ((DateTime.Now - last).TotalMilliseconds > 400)
                            {
                                last = DateTime.Now;
                                Say(string.Format("progress: {0}% downloading {1} ({2:0.0} MB)", Pct(), what, got / 1048576.0));
                            }
                        }
                    }
                    return;
                }
                catch (OperationCanceledException) { throw; }
                catch (Exception ex)
                {
                    if (attempt == 2 || cancel) throw new IOException("downloading " + what + " failed (" + ex.Message + "). Check the internet connection.");
                    Say("  " + what + ": " + ex.Message + " -- trying once more");
                    Thread.Sleep(3000);
                }
            }
        }
    }

    // ------------------------------------------------------------------ controls

    enum Kind { Primary, Secondary }

    // A big friendly rounded button that stays a real Button (keyboard, Enter, screen readers).
    class FlatBtn : Button
    {
        readonly Kind kind;
        bool hover, down;

        public FlatBtn(string text, Kind k, Font font)
        {
            kind = k; Text = text; Font = font;
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.ResizeRedraw, true);
            FlatStyle = FlatStyle.Flat; FlatAppearance.BorderSize = 0; UseVisualStyleBackColor = false;
            UseMnemonic = false; Cursor = Cursors.Hand; ForeColor = Theme.Text;
        }

        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hover = false; down = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnMouseDown(MouseEventArgs e) { if (e.Button == MouseButtons.Left) { down = true; Invalidate(); } base.OnMouseDown(e); }
        protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }
        protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }
        protected override void OnTextChanged(EventArgs e) { Invalidate(); base.OnTextChanged(e); }
        protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
        protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.Clear(Parent != null ? Parent.BackColor : Theme.Bg);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            bool cue = Focused && ShowFocusCues && Enabled;
            Color fill, border = Color.Empty, fore;
            if (kind == Kind.Primary)
            {
                fill = !Enabled ? Theme.Card : down ? Theme.AccentDown : hover ? Theme.AccentHover : Theme.Accent;
                fore = Enabled ? Theme.Ink : Theme.Dim;
            }
            else
            {
                fill = down ? Theme.SelBg : hover ? Theme.CardHi : Theme.Card;
                border = hover || cue ? Theme.Accent : Theme.Line;
                fore = hover ? Theme.AccentHi : Theme.Text;
            }
            RectangleF r = new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f);
            using (GraphicsPath p = Theme.Round(r, Theme.S(kind == Kind.Primary ? 12 : 7)))
            {
                using (SolidBrush b = new SolidBrush(fill)) g.FillPath(b, p);
                if (border != Color.Empty) using (Pen pen = new Pen(border, Math.Max(1f, Theme.Scale))) g.DrawPath(pen, p);
            }
            if (cue && kind == Kind.Primary)
            {
                float d = Theme.S(4);
                using (GraphicsPath p = Theme.Round(new RectangleF(d, d, Width - 2 * d - 1, Height - 2 * d - 1), Theme.S(9)))
                using (Pen pen = new Pen(Theme.Ink, Math.Max(1f, Theme.Scale)))
                    g.DrawPath(pen, p);
            }
            TextRenderer.DrawText(g, Text, Font, ClientRectangle, fore,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
        }
    }

    class Bar : Control, IAnimated
    {
        double frac;
        bool running;

        public Bar()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            SetStyle(ControlStyles.Selectable, false);
            TabStop = false;
            Anim.Add(this);
        }

        public double Value
        {
            get { return frac; }
            set
            {
                double v = Math.Max(0, Math.Min(1, value));
                if (Math.Abs(v - frac) > 0.0005) { frac = v; Invalidate(); }
            }
        }
        public bool Running { get { return running; } set { running = value; Invalidate(); } }
        public bool Animating { get { return running; } }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.Clear(Parent != null ? Parent.BackColor : Theme.Bg);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            float h = Height - 1f, rad = h / 2;
            RectangleF track = new RectangleF(0, 0, Width - 1f, h);
            using (GraphicsPath p = Theme.Round(track, rad))
            {
                using (SolidBrush b = new SolidBrush(Theme.Card)) g.FillPath(b, p);
                using (Pen pen = new Pen(Theme.Line)) g.DrawPath(pen, p);
            }
            float fw = (float)(track.Width * frac);
            if (frac > 0 && fw < h) fw = h;
            Color fill = Theme.Accent;
            if (fw > 0)
            {
                using (GraphicsPath p = Theme.Round(new RectangleF(0, 0, fw, h), rad))
                {
                    Color dark = Color.FromArgb(fill.R * 45 / 100, fill.G * 45 / 100, fill.B * 45 / 100);
                    using (LinearGradientBrush b = new LinearGradientBrush(new RectangleF(0, 0, Math.Max(2f, fw), h), dark, fill, LinearGradientMode.Horizontal))
                        g.FillPath(b, p);
                    if (running)
                    {
                        // a light band sweeping along the filled part: it is alive
                        float band = Math.Max(h * 4, 70 * Theme.Scale);
                        float x = (Anim.Frame * 7f * Theme.Scale) % (fw + band) - band;
                        RectangleF br = new RectangleF(x, 0, band, h);
                        using (LinearGradientBrush lb = new LinearGradientBrush(br, Color.Transparent, Color.Transparent, LinearGradientMode.Horizontal))
                        {
                            ColorBlend cb = new ColorBlend(3);
                            cb.Colors = new Color[] { Color.FromArgb(0, 255, 255, 255), Color.FromArgb(95, 255, 255, 255), Color.FromArgb(0, 255, 255, 255) };
                            cb.Positions = new float[] { 0f, 0.5f, 1f };
                            lb.InterpolationColors = cb;
                            Region old = g.Clip;
                            g.SetClip(p);
                            g.FillRectangle(lb, br);
                            g.Clip = old;
                            old.Dispose();
                        }
                    }
                }
            }
            else if (running)
            {
                // nothing done yet: a short block gliding along the track
                float bw = track.Width / 5;
                float x = (Anim.Frame * 6f * Theme.Scale) % (track.Width + bw) - bw;
                RectangleF br = RectangleF.Intersect(new RectangleF(x, 0, bw, h), track);
                if (br.Width > 2) using (GraphicsPath p = Theme.Round(br, rad)) using (SolidBrush b = new SolidBrush(fill)) g.FillPath(b, p);
            }
        }
    }

    // The logo, drawn smooth at any size.
    class Logo : Control
    {
        readonly Image img;

        public Logo(Image img)
        {
            this.img = img;
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
            SetStyle(ControlStyles.Selectable, false);
            TabStop = false;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.Clear(Theme.Bg);
            if (img == null) return;
            e.Graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
            e.Graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
            e.Graphics.DrawImage(img, ClientRectangle);
        }
    }

    // The one question Setup may ask: "Where is RPCS3?" / "Where is your inFAMOUS 2 game folder?"
    sealed class AskDialog : Form
    {
        public string Result;
        readonly bool rpcs3;
        readonly Label msg;

        public AskDialog(bool askRpcs3, string why, Icon icon)
        {
            rpcs3 = askRpcs3;
            Text = MainForm.Title;
            Icon = icon;
            AutoScaleMode = AutoScaleMode.None;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false; MinimizeBox = false; ShowInTaskbar = false;
            StartPosition = FormStartPosition.CenterParent;
            BackColor = Theme.Bg; ForeColor = Theme.Text; Font = Theme.Body;
            ClientSize = new Size(Theme.S(480), Theme.S(210));
            int w = ClientSize.Width - Theme.S(56);
            Label t = L(askRpcs3 ? "Where is RPCS3?" : "Where is your inFAMOUS 2 game folder?", Theme.H2, Theme.Text, Theme.S(26), w, Theme.S(30));
            Label s = L(why, Theme.Body, Theme.Dim, Theme.S(64), w, Theme.S(44));
            msg = L("", Theme.Small, Theme.Bad, Theme.S(110), w, Theme.S(22));
            FlatBtn browse = new FlatBtn("Browse...", Kind.Primary, Theme.Btn);
            browse.Size = new Size(Theme.S(150), Theme.S(42));
            browse.Location = new Point(Theme.S(28), Theme.S(146));
            FlatBtn cancel = new FlatBtn("Cancel", Kind.Secondary, Theme.Btn);
            cancel.Size = new Size(Theme.S(110), Theme.S(42));
            cancel.Location = new Point(browse.Right + Theme.S(12), browse.Top);
            Controls.AddRange(new Control[] { t, s, msg, browse, cancel });
            browse.Click += delegate { Browse(); };
            cancel.Click += delegate { DialogResult = DialogResult.Cancel; Close(); };
            AcceptButton = browse; CancelButton = cancel;
        }

        static Label L(string text, Font f, Color c, int y, int w, int h)
        {
            Label l = new Label();
            l.AutoSize = false; l.UseMnemonic = false; l.Font = f; l.ForeColor = c; l.Text = text;
            l.Location = new Point(Theme.S(28), y); l.Size = new Size(w, h);
            return l;
        }

        protected override void OnHandleCreated(EventArgs e) { base.OnHandleCreated(e); Native.DarkTitle(Handle, Theme.Bg); }

        void Browse()
        {
            string pick = null;
            if (rpcs3)
            {
                using (OpenFileDialog d = new OpenFileDialog())
                {
                    d.Title = "Find rpcs3.exe";
                    d.Filter = "RPCS3 (rpcs3.exe)|rpcs3.exe|Programs (*.exe)|*.exe";
                    if (d.ShowDialog(this) == DialogResult.OK) pick = Path.GetDirectoryName(d.FileName);
                }
                if (pick == null) return;
                if (!File.Exists(Path.Combine(pick, "rpcs3.exe"))) { msg.Text = "That folder has no rpcs3.exe."; return; }
            }
            else
            {
                using (FolderBrowserDialog d = new FolderBrowserDialog())
                {
                    d.Description = "Choose your inFAMOUS 2 game folder: the one that holds PS3_GAME.";
                    d.ShowNewFolderButton = false;
                    if (d.ShowDialog(this) == DialogResult.OK) pick = Util.NormalizeDisc(d.SelectedPath);
                }
                if (pick == null) return;
                if (!Util.IsDisc(pick)) { msg.Text = "That folder has no PS3_GAME folder in it."; return; }
            }
            Result = pick;
            DialogResult = DialogResult.OK;
            Close();
        }
    }

    // ------------------------------------------------------------------ the window

    sealed class MainForm : Form
    {
        public const string Title = "inFAMOUS 2 PC port - Setup";
        public const int BaseW = 640, BaseH = 470, LogH = 240;
        const string IssuesUrl = "https://github.com/SamiEnazi/infamous2-port/issues/new";
        const string MsvcLicense = "https://go.microsoft.com/fwlink/?LinkId=2179911";
        const string Intro = "Downloads the build tools it needs, builds the game from your copy, then cleans up.";

        enum State { Start, Working, Failed, Done }
        enum JobKind { None, Download, Build }

        readonly Opts opt;
        readonly string exeDir;
        string repo, target, rpcs3, disc;
        string foundRpcs3;               // looked up in the background at start
        volatile bool searching;
        public int ExitCode;

        readonly FlatBtn big;
        readonly Bar bar;
        readonly Label status, line;
        readonly LinkLabel license, details, copyLog, again;
        readonly TextBox logBox;
        readonly System.Windows.Forms.Timer pump, anim;
        bool showLog, forceClose;
        State state;

        IJob job;
        JobKind jobKind;
        StreamWriter logFile;
        readonly List<string> log = new List<string>();
        bool logDirty;
        // progress, from the job's lines
        int jStep, jSteps = 1, bSub = -1, ninjaDone, ninjaTotal;
        double jFrac;
        string failReason, compileLog;
        bool sawDone;
        long compilePos = -1;

        static int S(double v) { return Theme.S(v); }

        public MainForm(Opts o, string dir)
        {
            opt = o; exeDir = dir;
            if (Util.HasPort(dir)) repo = dir;     // else Setup.exe was downloaded on its own: Install gets the port first
            ExitCode = o.TestBuild ? 2 : 0;

            Text = Title;
            Icon = Res.WindowIcon(dir);
            AutoScaleMode = AutoScaleMode.None;
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Theme.Bg; ForeColor = Theme.Text; Font = Theme.Body;
            ClientSize = new Size(S(BaseW), S(BaseH));
            int cw = ClientSize.Width;

            Logo logo = new Logo(Res.Logo(dir));
            logo.Size = new Size(S(92), S(92));
            logo.Location = new Point((cw - logo.Width) / 2, S(34));
            Label title = Center("inFAMOUS 2", Theme.H1, Theme.Text, S(132), S(50));
            Label sub = Center("PC port", Theme.Body, Theme.Accent, S(182), S(24));

            big = new FlatBtn("Install", Kind.Primary, Theme.BtnHuge);
            big.Size = new Size(S(330), S(76));
            big.Location = new Point((cw - big.Width) / 2, S(232));
            big.Name = "install";
            big.Click += delegate { OnBig(); };
            bar = new Bar();
            bar.Size = new Size(S(460), S(18));
            bar.Location = new Point((cw - bar.Width) / 2, S(252));
            bar.Visible = false;
            status = Center("", Theme.Bold, Theme.Text, S(282), S(28));
            line = Center(Intro, Theme.Body, Theme.Dim, S(330), S(48));
            license = Theme.Link("By installing you accept Microsoft's build tools license", Theme.Small);
            license.LinkArea = new LinkArea(license.Text.IndexOf("Microsoft"), license.Text.Length - license.Text.IndexOf("Microsoft"));
            license.LinkClicked += delegate { Util.OpenUrl(MsvcLicense); };
            license.Location = new Point((cw - license.PreferredWidth) / 2, S(386));

            details = Theme.Link("Details", Theme.Small);
            details.Location = new Point(S(18), S(BaseH) - S(32));
            details.LinkClicked += delegate { ToggleLog(); };
            copyLog = Theme.Link("Copy log", Theme.Small);
            copyLog.Location = new Point(details.Left + TextRenderer.MeasureText("Hide details", Theme.Small).Width + S(16), details.Top);
            copyLog.LinkClicked += delegate { CopyLog(); };
            copyLog.Visible = false;
            again = Theme.Link("Install again (after an update)", Theme.Small);
            again.Location = new Point(cw - S(18) - again.PreferredWidth, details.Top);
            again.LinkClicked += delegate { Install(); };
            again.Visible = false;

            logBox = new TextBox();
            logBox.Multiline = true; logBox.ReadOnly = true; logBox.ScrollBars = ScrollBars.Vertical; logBox.WordWrap = true;
            logBox.BorderStyle = BorderStyle.None;
            logBox.BackColor = Theme.LogBg; logBox.ForeColor = Color.FromArgb(190, 200, 215); logBox.Font = Theme.Mono;
            logBox.Location = new Point(S(14), S(BaseH));
            logBox.Size = new Size(cw - S(28), S(LogH) - S(14));

            Controls.AddRange(new Control[] { logo, title, sub, big, bar, status, line, license, details, copyLog, again, logBox });
            AcceptButton = big;

            pump = new System.Windows.Forms.Timer();
            pump.Interval = 200;
            pump.Tick += delegate { OnPump(); };
            pump.Start();
            anim = new System.Windows.Forms.Timer();
            anim.Interval = 40;
            anim.Tick += delegate { Anim.Tick(); };
            anim.Start();
        }

        Label Center(string text, Font f, Color c, int y, int h)
        {
            Label l = new Label();
            l.AutoSize = false; l.UseMnemonic = false; l.Font = f; l.ForeColor = c; l.Text = text;
            l.TextAlign = ContentAlignment.MiddleCenter;
            l.Location = new Point(S(30), y); l.Size = new Size(ClientSize.Width - S(60), h);
            return l;
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            Native.DarkTitle(Handle, Theme.Bg);
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            bool built = repo != null && File.Exists(Util.BuiltExe(repo));
            if (opt.Page == "done" || (built && !opt.TestBuild)) SetState(State.Done);
            else SetState(State.Start);
            if (opt.Page == "done") return;
            if (opt.Page == "ask") { Ask(true, "Setup could not find RPCS3 on this PC. Show it where rpcs3.exe is."); return; }
            // look for RPCS3 now, so Install can start right away
            if (opt.Rpcs3 == null)
            {
                searching = true;
                string d = exeDir, r = repo;
                ThreadPool.QueueUserWorkItem(delegate { foundRpcs3 = Util.FindRpcs3(d, r); searching = false; });
            }
            if (opt.TestBuild) Install();
        }

        // ---------------------------------------------------------- the screen

        void SetState(State s)
        {
            state = s;
            bool work = s == State.Working;
            big.Visible = !work;
            big.Text = s == State.Done ? "Play" : s == State.Failed ? "Try again" : "Install";
            bar.Visible = work;
            bar.Running = work;
            status.Visible = work || s == State.Done;
            if (s == State.Done) { status.Text = ""; status.Visible = false; }
            line.Visible = s != State.Working;
            if (s == State.Start) { line.Text = Intro; line.ForeColor = Theme.Dim; }
            if (s == State.Done) { line.Text = "Ready to play."; line.ForeColor = Theme.Dim; }
            license.Visible = s == State.Start || s == State.Failed;
            copyLog.Visible = s == State.Failed;
            again.Visible = s == State.Done && !opt.TestBuild;
            Text = Title;
            if (!work && big.Visible) big.Focus();
        }

        void Status(string s) { status.Text = s; }

        void Fail(string reason)
        {
            failReason = reason;
            Native.KeepAwake(false);
            SetState(State.Failed);
            line.Text = reason;
            line.ForeColor = Theme.Bad;
            AddLog("== stopped: " + reason);
            ExitCode = 1;
            if (opt.ExitWhenDone) CloseSoon();
        }

        void ToggleLog()
        {
            showLog = !showLog;
            ClientSize = new Size(ClientSize.Width, S(BaseH) + (showLog ? S(LogH) : 0));
            details.Text = showLog ? "Hide details" : "Details";
            logDirty = true;
            ShowLogTail();
        }

        void OnBig()
        {
            if (state == State.Done) Play();
            else Install();
        }

        // ---------------------------------------------------------- Install: everything, in order

        void Install()
        {
            if (job != null) return;
            failReason = null;
            SetState(State.Working);
            bar.Value = 0;
            Native.KeepAwake(true);
            if (repo == null)
            {
                target = string.IsNullOrEmpty(opt.Folder) ? Util.DefaultTarget(exeDir) : Path.GetFullPath(opt.Folder);
                if (Util.PortComplete(target)) { repo = target; }
                else { StartDownload(); return; }
            }
            FindThenCheck();
        }

        void StartDownload()
        {
            ResetProgress();
            BeginLog(null);
            AddLog("> download the port into " + target);
            Status("Downloading the game's build files\u2026");
            PortDownload d = new PortDownload(target);
            jobKind = JobKind.Download;
            job = d;
            d.Start();
        }

        void DownloadEnded(int code)
        {
            if (code != 0 || !Util.PortComplete(target)) { Fail(failReason ?? "The download stopped. Check the internet connection, then press Try again."); return; }
            // keep a copy of Setup.exe in the game folder, for next time
            try
            {
                string dst = Path.Combine(target, "Setup.exe");
                if (!Util.SamePath(dst, Application.ExecutablePath)) File.Copy(Application.ExecutablePath, dst, true);
            }
            catch (Exception) { }
            repo = target;
            FindThenCheck();
        }

        // RPCS3 and the game: found, or asked for in one small dialog; then the checks.
        void FindThenCheck()
        {
            Status("Looking for RPCS3 and your game\u2026");
            bar.Value = 0;
            if (searching) { After(300, FindThenCheck); return; }
            string rp = opt.Rpcs3 ?? (rpcs3 != null && File.Exists(Path.Combine(rpcs3, "rpcs3.exe")) ? rpcs3 : null) ?? foundRpcs3;
            if (rp == null || !File.Exists(Path.Combine(rp, "rpcs3.exe")))
                rp = Ask(true, "Setup could not find RPCS3 on this PC. Show it where rpcs3.exe is.");
            if (rp == null) { Fail("Setup needs RPCS3 with your game in it. Press Try again when it is ready."); return; }
            rpcs3 = rp;
            string dc = opt.Disc ?? (Util.IsDisc(disc) ? disc : null) ?? Util.DetectDisc(rp);
            if (!Util.IsDisc(dc)) dc = Ask(false, "Setup could not find the game in RPCS3's game list. Show it the folder that holds PS3_GAME.");
            if (dc == null) { Fail("Setup needs your inFAMOUS 2 game folder (the one with PS3_GAME). Press Try again."); return; }
            disc = dc;
            AddLog("RPCS3: " + rpcs3);
            AddLog("game: " + disc);
            Status("Checking your PC and your game\u2026");
            string root = repo, d1 = disc, r1 = rpcs3;
            ThreadPool.QueueUserWorkItem(delegate
            {
                string err;
                List<Row> rows = Checker.Run(root, d1, r1, out err);
                Post(delegate { Checked(rows, err); });
            });
        }

        string Ask(bool askRpcs3, string why)
        {
            using (AskDialog d = new AskDialog(askRpcs3, why, Icon))
                return d.ShowDialog(this) == DialogResult.OK ? d.Result : null;
        }

        // One line, and only for what the player must fix; a wrong folder is asked for again.
        void Checked(List<Row> rows, string err)
        {
            if (err != null) { Fail("Setup could not run its checks: " + err); return; }
            foreach (Row r in rows) AddLog(string.Format("  {0,-5} {1}: {2}", r.Ok == true ? "ok" : r.Ok == false ? "MISS" : "note", r.Name, r.Detail));
            foreach (Row r in rows)
            {
                if (r.Ok != false) continue;
                string n = r.Name;
                if (n == "64-bit Windows" || n == "CPU with AVX2" || n == "Vulkan graphics driver") { Fail("This PC can't run the game: " + r.Detail + "."); return; }
                if (n.StartsWith("Game disc", StringComparison.Ordinal))
                {
                    disc = Ask(false, "That folder is not inFAMOUS 2 (" + r.Detail + "). Show Setup the right game folder.");
                    if (disc == null) { Fail("Setup needs your inFAMOUS 2 game folder. Press Try again."); return; }
                    FindThenCheck();
                    return;
                }
                if (n.StartsWith("RPCS3", StringComparison.Ordinal))
                {
                    rpcs3 = Ask(true, "RPCS3 is not where Setup thought. Show it where rpcs3.exe is.");
                    if (rpcs3 == null) { Fail("Setup needs RPCS3 with your game in it. Press Try again when it is ready."); return; }
                    FindThenCheck();
                    return;
                }
                if (n == "PS3 firmware in RPCS3") { Fail("RPCS3 has no PS3 firmware yet. In RPCS3: File > Install Firmware, then press Try again."); return; }
                if (n.StartsWith("Game update", StringComparison.Ordinal)) { Fail("The game's 1.04 update is not in RPCS3 yet. In RPCS3: File > Install Packages, then press Try again."); return; }
                Fail(n + ": " + r.Detail);
                return;
            }
            if (Process.GetProcessesByName("rpcs3").Length > 0) { Fail("Close RPCS3 (Setup uses it to unlock your game), then press Try again."); return; }
            StartBuild();
        }

        void StartBuild()
        {
            string script = Path.Combine(repo, "tools\\setup_build.ps1");
            Dictionary<string, string> env = null;
            if (opt.OnlyCache)
            {
                // test: only the downloaded tools; this PC's own installs are out of sight
                env = new Dictionary<string, string>();
                env["PORT_ONLY_CACHE"] = "1";
                env["Path"] = Util.WindowsPath();
            }
            else Util.RefreshPath();
            // pressing Install accepts Microsoft's license (the line under the button says so)
            string cmd = "[Console]::OutputEncoding = [Text.Encoding]::UTF8; & " + Util.Q(script) + " -Disc " + Util.Q(disc) +
                         " -Rpcs3 " + Util.Q(rpcs3) + " -AcceptMsvcLicense" + (opt.NoShortcut ? " -NoShortcut" : "") +
                         (opt.NoCleanup ? "" : " -Cleanup") + "; exit [int]$LASTEXITCODE";
            ResetProgress();
            compileLog = Path.Combine(repo, "work\\logs\\compile.log");
            Status("Getting ready\u2026");
            BeginLog(Path.Combine(repo, "work\\setup.log"));
            AddLog("RPCS3: " + rpcs3);
            AddLog("game: " + disc);
            jobKind = JobKind.Build;
            AddLog("> setup_build.ps1" + (opt.OnlyCache ? " (PORT_ONLY_CACHE=1, Windows-only PATH)" : ""));
            Job j = new Job();
            try { j.Start(Util.PowerShellExe(), "-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" + cmd + "\"", repo, env); }
            catch (Exception ex) { CloseLogFile(); Fail("Setup could not start PowerShell: " + ex.Message); return; }
            job = j;
        }

        // ---------------------------------------------------------- progress

        // How much of the whole build each step of setup_build.ps1 is (percent, on the test PC).
        static readonly double[] StepWeight = { 0, 1, 22, 4, 2, 3, 10, 1, 52, 1, 4 };
        static readonly double[] DownloadWeight = { 0, 3, 10, 72, 15 };
        // tools\rebuild_win.py's own steps inside step 8 ("[ppu]", ...), and their share of it.
        static readonly string[] SubNames = { "inputs", "port", "firmware", "ppu", "spu", "jobs", "compile" };
        static readonly double[] SubWeight = { 2, 3, 10, 22, 4, 9, 50 };
        const int ToolsStep = 2, CompileStep = 8, CleanupStep = 10;

        void ResetProgress()
        {
            jStep = 0; jSteps = 1; jFrac = 0; bSub = -1; ninjaDone = ninjaTotal = 0;
            failReason = null; sawDone = false; compilePos = -1;
        }

        void JobLine(string line)
        {
            AddLog(line);
            string t = line.Trim();
            int k, n;
            if (t.StartsWith("== [", StringComparison.Ordinal))
            {
                int slash = t.IndexOf('/'), close = t.IndexOf(']');
                if (slash > 4 && int.TryParse(t.Substring(4, slash - 4), out n)) { jStep = n; jFrac = 0; bSub = -1; ninjaTotal = 0; }
                if (slash > 0 && close > slash && int.TryParse(t.Substring(slash + 1, close - slash - 1), out n)) jSteps = n;
            }
            else if (t.StartsWith("== FAILED:", StringComparison.Ordinal)) failReason = t.Substring(10).Trim();
            else if (t.StartsWith("== done", StringComparison.Ordinal)) sawDone = true;
            else if (t.StartsWith("error:", StringComparison.Ordinal)) failReason = t.Substring(6).Trim();
            else if (t.StartsWith("progress:", StringComparison.Ordinal)) { int pct = Util.Percent(t); if (pct >= 0) jFrac = pct / 100.0; }
            else if (jobKind == JobKind.Build && jStep == CompileStep && t.Length > 2 && t[0] == '[' && t[t.Length - 1] == ']' &&
                     Array.IndexOf(SubNames, t.Substring(1, t.Length - 2)) >= 0)
            {
                bSub = Array.IndexOf(SubNames, t.Substring(1, t.Length - 2));
                if (bSub == SubNames.Length - 1)
                {
                    // the compile writes to work\logs\compile.log: follow ninja's [k/N] there from here on
                    try { compilePos = File.Exists(compileLog) ? new FileInfo(compileLog).Length : 0; } catch (Exception) { compilePos = 0; }
                }
            }
            else if (Util.Ninja(t, out k, out n)) jFrac = k / (double)n;
        }

        void TailCompileLog()
        {
            if (jStep != CompileStep || bSub != SubNames.Length - 1 || compilePos < 0) return;
            try
            {
                using (FileStream fs = new FileStream(compileLog, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    if (fs.Length < compilePos) compilePos = 0;
                    if (fs.Length == compilePos) return;
                    fs.Seek(compilePos, SeekOrigin.Begin);
                    int want = (int)Math.Min(fs.Length - compilePos, 8 << 20), got = 0, r;
                    byte[] buf = new byte[want];
                    while (got < want && (r = fs.Read(buf, got, want - got)) > 0) got += r;
                    int last = got > 0 ? Array.LastIndexOf(buf, (byte)'\n', got - 1) : -1;
                    if (last < 0) { if (got == (8 << 20)) compilePos += got; return; }
                    int start = 0;
                    for (int i = 0; i <= last; i++)
                    {
                        if (buf[i] != '\n') continue;
                        if (buf[start] == '[' && i - start > 4)
                        {
                            int k, n;
                            if (Util.Ninja(Encoding.ASCII.GetString(buf, start, Math.Min(32, i - start)), out k, out n)) { ninjaDone = k; ninjaTotal = n; }
                        }
                        start = i + 1;
                    }
                    compilePos += last + 1;
                }
            }
            catch (Exception) { }
        }

        double JobFraction()
        {
            double[] w = jobKind == JobKind.Download ? DownloadWeight : StepWeight;
            if (jStep < 1 || jSteps + 1 != w.Length) return 0;
            int st = Math.Min(jStep, jSteps);
            double done = 0;
            for (int i = 1; i < st; i++) done += w[i];
            double f = jFrac;
            if (jobKind == JobKind.Build && st == CompileStep)
            {
                f = 0;
                if (bSub >= 0)
                {
                    double d = 0;
                    for (int j = 0; j < bSub; j++) d += SubWeight[j];
                    if (bSub == SubNames.Length - 1 && ninjaTotal > 0) d += SubWeight[bSub] * ninjaDone / (double)ninjaTotal;
                    f = d / 100.0;
                }
            }
            return (done + w[st] * Math.Min(1, f)) / 100.0;
        }

        // One short line: "Downloading tools... 45%", "Building... 3 of 7", "Cleaning up...".
        string StatusText()
        {
            if (jobKind == JobKind.Download) return "Downloading the game's build files\u2026 " + (int)(JobFraction() * 100) + "%";
            if (jStep < ToolsStep) return "Getting ready\u2026";
            if (jStep == ToolsStep) return "Downloading tools\u2026 " + (int)(jFrac * 100) + "%";
            if (jStep < CleanupStep) return string.Format("Building\u2026 {0} of 7", jStep - ToolsStep);
            return "Cleaning up\u2026";
        }

        void OnPump()
        {
            if (job == null) return;
            IJob j = job;
            string ln;
            int count = 0;
            while (count < 4000 && j.Lines.TryDequeue(out ln)) { count++; JobLine(ln); }
            if (jobKind == JobKind.Build) TailCompileLog();
            double v = JobFraction();
            bar.Value = v;
            status.Text = StatusText();
            if (jobKind == JobKind.Build) Text = (int)(v * 100) + "% - " + Title;
            ShowLogTail();
            if (j.Exited && j.Lines.IsEmpty)
            {
                job = null;
                AddLog("[exit code " + j.ExitCode + "]");
                CloseLogFile();
                ShowLogTail();
                if (jobKind == JobKind.Download) DownloadEnded(j.ExitCode);
                else BuildEnded(j.ExitCode);
            }
        }

        void BuildEnded(int code)
        {
            Native.KeepAwake(false);
            if (code == 0 && sawDone)
            {
                bar.Value = 1;
                ExitCode = 0;
                SetState(State.Done);
                if (opt.ExitWhenDone) CloseSoon();
                return;
            }
            Fail("The build stopped: " + (failReason ?? (code == 0 ? "it ended without finishing" : "exit code " + code)) +
                 ". Press Try again, or Copy log for a bug report.");
        }

        // ---------------------------------------------------------- the log (behind "Details")

        void BeginLog(string logPath)
        {
            CloseLogFile();
            if (logPath != null)
            {
                try
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(logPath));
                    logFile = new StreamWriter(new FileStream(logPath, FileMode.Create, FileAccess.Write, FileShare.ReadWrite), new UTF8Encoding(false));
                    logFile.AutoFlush = true;
                    foreach (string l in log) logFile.WriteLine(l);
                }
                catch (Exception) { logFile = null; }
            }
            AddLog(string.Format("Setup.exe {0:yyyy-MM-dd HH:mm}, Windows {1}", DateTime.Now, Environment.OSVersion.Version));
        }

        void AddLog(string line)
        {
            log.Add(line);
            if (log.Count > 30000) log.RemoveRange(0, 10000);
            logDirty = true;
            if (logFile != null) { try { logFile.WriteLine(line); } catch (Exception) { } }
        }

        void CloseLogFile()
        {
            if (logFile == null) return;
            try { logFile.Dispose(); } catch (Exception) { }
            logFile = null;
        }

        // The last 200 lines (not while the player is reading in the box).
        void ShowLogTail()
        {
            if (!showLog || !logDirty || logBox.Focused) return;
            logDirty = false;
            int from = Math.Max(0, log.Count - 200);
            logBox.Text = string.Join("\r\n", log.GetRange(from, log.Count - from));
            logBox.SelectionStart = logBox.TextLength;
            logBox.ScrollToCaret();
        }

        void CopyLog()
        {
            string all = string.Join("\r\n", log);
            if (all.Length > 60000) all = "(the start is cut; the whole log is in work\\setup.log)\r\n" + all.Substring(all.Length - 60000);
            try
            {
                Clipboard.SetText(all.Length > 0 ? all : "(empty log)");
                copyLog.Text = "Copied: paste it into a bug report";
                copyLog.LinkArea = new LinkArea(0, copyLog.Text.Length);
                After(3000, delegate { copyLog.Text = "Copy log"; copyLog.LinkArea = new LinkArea(0, 8); });
                Util.OpenUrl(IssuesUrl);
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Could not copy the log: " + ex.Message, Title, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }

        // ---------------------------------------------------------- play

        void Play()
        {
            if (repo == null) return;
            string ps1 = Path.Combine(repo, "tools\\play.ps1");
            if (!File.Exists(ps1)) { MessageBox.Show(this, "tools\\play.ps1 is missing in " + repo + ".", Title); return; }
            try
            {
                ProcessStartInfo si = new ProcessStartInfo(Util.PowerShellExe(),
                    "-NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File \"" + ps1 + "\"");
                si.UseShellExecute = true;
                si.WindowStyle = ProcessWindowStyle.Minimized;
                si.WorkingDirectory = repo;
                Process.Start(si);
            }
            catch (Exception ex) { MessageBox.Show(this, "Could not start the game: " + ex.Message, Title); }
        }

        // ---------------------------------------------------------- small things

        void Post(MethodInvoker m)
        {
            try { if (IsHandleCreated && !IsDisposed) BeginInvoke(m); } catch (Exception) { }
        }

        void After(int ms, MethodInvoker m)
        {
            System.Windows.Forms.Timer t = new System.Windows.Forms.Timer();
            t.Interval = ms;
            t.Tick += delegate { t.Stop(); t.Dispose(); if (!IsDisposed) m(); };
            t.Start();
        }

        void CloseSoon() { After(2500, delegate { forceClose = true; Close(); }); }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            if (job != null && !job.Exited && !forceClose && e.CloseReason == CloseReason.UserClosing)
            {
                if (MessageBox.Show(this, "Setup is still working.\n\nStop it and close Setup?", Title, MessageBoxButtons.YesNo,
                                    MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2) != DialogResult.Yes)
                {
                    e.Cancel = true;
                    return;
                }
            }
            if (job != null && !job.Exited) job.Kill();
            Native.KeepAwake(false);
            CloseLogFile();
            base.OnFormClosing(e);
        }
    }
}
