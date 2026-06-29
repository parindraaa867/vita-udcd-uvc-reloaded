using System;
using System.Diagnostics;
using System.IO;
using System.Management;
using System.Windows.Forms;
using System.Drawing;
using System.Reflection;
using Microsoft.Win32;

// Background tray app: watches for the PS Vita UVC device and instantly opens
// the viewer (Edge app-mode, no browser chrome) when it's plugged in; closes
// it on unplug. Right-click the tray icon for options.
namespace VitaViewer
{
    class App : ApplicationContext
    {
        NotifyIcon tray;
        bool present = false;
        DateTime lastLaunch = DateTime.MinValue;
        ManagementEventWatcher watcher;
        Timer poll;

        string UserData() { return Path.Combine(Path.GetTempPath(), "vitaviewer"); }

        // The viewer HTML is embedded in this exe; extract it once to temp.
        string ViewerHtml()
        {
            string dir = UserData();
            string path = Path.Combine(dir, "viewer.html");
            try
            {
                Directory.CreateDirectory(dir);
                using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("index.html"))
                using (var f = File.Create(path))
                    s.CopyTo(f);
            }
            catch { }
            return path;
        }

        public App()
        {
            tray = new NotifyIcon { Icon = SystemIcons.Application, Visible = true,
                                    Text = "Vita UVC Viewer - watching for Vita" };
            var menu = new ContextMenuStrip();
            menu.Items.Add("Open viewer now", null, (s, e) => Launch());
            var su = new ToolStripMenuItem("Start with Windows") { Checked = IsStartup() };
            su.Click += (s, e) => { SetStartup(!IsStartup()); su.Checked = IsStartup(); };
            menu.Items.Add(su);
            menu.Items.Add(new ToolStripSeparator());
            menu.Items.Add("Quit", null, (s, e) => { Shutdown(); ExitThread(); });
            tray.ContextMenuStrip = menu;
            tray.DoubleClick += (s, e) => Launch();

            try
            {
                watcher = new ManagementEventWatcher(
                    new WqlEventQuery("SELECT * FROM Win32_DeviceChangeEvent"));
                watcher.EventArrived += (s, e) => Scan();
                watcher.Start();
            }
            catch { }

            poll = new Timer { Interval = 3000 };   // fallback poll
            poll.Tick += (s, e) => Scan();
            poll.Start();
            Scan();
        }

        bool VitaPresent()
        {
            try
            {
                using (var q = new ManagementObjectSearcher(
                    "SELECT Name FROM Win32_PnPEntity WHERE Name LIKE '%PSVita%' " +
                    "OR Name LIKE '%PS Vita%' OR PNPDeviceID LIKE '%PID_1337%'"))
                {
                    foreach (var o in q.Get()) return true;
                }
            }
            catch { }
            return false;
        }

        void Scan()
        {
            bool now = VitaPresent();
            if (now && !present) { present = true; Launch(); }
            else if (!now && present) { present = false; CloseViewer(); }
            tray.Text = present ? "Vita UVC Viewer - connected" : "Vita UVC Viewer - watching for Vita";
        }

        // Find Edge windows we launched (tagged by our unique user-data-dir).
        bool ViewerRunning()
        {
            try
            {
                using (var q = new ManagementObjectSearcher(
                    "SELECT ProcessId FROM Win32_Process WHERE Name='msedge.exe' " +
                    "AND CommandLine LIKE '%vitaviewer%'"))
                    foreach (var o in q.Get()) return true;
            }
            catch { }
            return false;
        }

        void Launch()
        {
            if ((DateTime.Now - lastLaunch).TotalSeconds < 3) return; // debounce
            if (ViewerRunning()) return;                              // never duplicate
            string edge = EdgePath();
            if (edge == null || !File.Exists(ViewerHtml())) return;
            lastLaunch = DateTime.Now;
            var psi = new ProcessStartInfo { FileName = edge, UseShellExecute = false };
            psi.Arguments =
                "--app=\"file:///" + ViewerHtml().Replace('\\', '/') + "\" " +
                "--user-data-dir=\"" + UserData() + "\" " +
                "--use-fake-ui-for-media-stream " +   // auto-grant camera, no prompt
                "--test-type " +                      // hide the "unsupported flag" warning bar
                "--no-first-run --disable-features=Translate --window-size=1280,720";
            try { Process.Start(psi); } catch { }
        }

        void CloseViewer()
        {
            try
            {
                using (var q = new ManagementObjectSearcher(
                    "SELECT ProcessId FROM Win32_Process WHERE Name='msedge.exe' " +
                    "AND CommandLine LIKE '%vitaviewer%'"))
                    foreach (ManagementObject o in q.Get())
                    {
                        try
                        {
                            int pid = Convert.ToInt32(o["ProcessId"]);
                            Process.Start(new ProcessStartInfo("taskkill", "/PID " + pid + " /T /F")
                            { CreateNoWindow = true, UseShellExecute = false });
                        }
                        catch { }
                    }
            }
            catch { }
        }

        void Shutdown()
        {
            CloseViewer();
            try { if (watcher != null) watcher.Stop(); } catch { }
            tray.Visible = false;
        }

        static string EdgePath()
        {
            string[] p = {
                @"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
                @"C:\Program Files\Microsoft\Edge\Application\msedge.exe" };
            foreach (var x in p) if (File.Exists(x)) return x;
            return null;
        }

        static bool IsStartup()
        {
            using (var k = Registry.CurrentUser.OpenSubKey(
                @"Software\Microsoft\Windows\CurrentVersion\Run"))
                return k != null && k.GetValue("VitaViewer") != null;
        }

        static void SetStartup(bool on)
        {
            using (var k = Registry.CurrentUser.OpenSubKey(
                @"Software\Microsoft\Windows\CurrentVersion\Run", true))
            {
                if (on) k.SetValue("VitaViewer", "\"" + Application.ExecutablePath + "\"");
                else k.DeleteValue("VitaViewer", false);
            }
        }
    }

    static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.Run(new App());
        }
    }
}
