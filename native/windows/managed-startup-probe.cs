// SPDX-License-Identifier: LGPL-2.1-or-later
using System;
using System.Diagnostics;

class ManagedStartupProbe
{
    static int Main(string[] args)
    {
        if (args.Length != 1 && args.Length != 3) return 2;
        int expected = Int32.Parse(args[0]);
        Console.WriteLine("CLR process bits: {0}; expected: {1}; prefix: {2}",
            IntPtr.Size * 8, expected, Environment.GetEnvironmentVariable("WINEDOWS_PREFIX"));
        if (IntPtr.Size * 8 != expected || String.IsNullOrEmpty(Environment.GetEnvironmentVariable("WINEDOWS_PREFIX"))) return 1;
        if (args.Length == 3)
        {
            var start = new ProcessStartInfo(args[1], args[2]);
            start.UseShellExecute = false;
            start.CreateNoWindow = true;
            start.RedirectStandardOutput = true;
            start.RedirectStandardError = true;
            using (var child = Process.Start(start))
            {
                if (!child.WaitForExit(30000)) { child.Kill(); return 124; }
                Console.Write(child.StandardOutput.ReadToEnd());
                Console.Error.Write(child.StandardError.ReadToEnd());
                Console.WriteLine("Managed child exit: {0}", child.ExitCode);
                if (child.ExitCode != 0) return 1;
            }
        }
        Console.WriteLine("PASS managed startup and process architecture");
        return 0;
    }
}
