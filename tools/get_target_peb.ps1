# Pull r5apex_dx12.exe PEB base via NtQueryInformationProcess.
# Uses PROCESS_QUERY_LIMITED_INFORMATION (0x1000) — limited handle + PBI passes AC.
# Output: "PEB = 0x<hex>" — consumed by run_installer.sh / drain.sh --peb.

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ApexPeb {
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern IntPtr OpenProcess(int access, bool inherit, int pid);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("ntdll.dll")]
  public static extern int NtQueryInformationProcess(
      IntPtr h, int c, IntPtr i, int s, out int r);
  public static IntPtr Get(int pid) {
    var h = OpenProcess(0x1000, false, pid); // PROCESS_QUERY_LIMITED_INFORMATION
    if (h == IntPtr.Zero) throw new Exception("OpenProcess: " + Marshal.GetLastWin32Error());
    try {
      var b = Marshal.AllocHGlobal(48);
      try {
        int n; var st = NtQueryInformationProcess(h, 0, b, 48, out n);
        if (st != 0) throw new Exception("NtQueryInformationProcess: 0x" + st.ToString("X"));
        return Marshal.ReadIntPtr(b, 8); // PROCESS_BASIC_INFORMATION.PebBaseAddress
      } finally { Marshal.FreeHGlobal(b); }
    } finally { CloseHandle(h); }
  }
}
'@

$p = Get-Process -Name r5apex_dx12 -ErrorAction Stop
"PID = {0}" -f $p.Id
"PEB = 0x{0:X}" -f [ApexPeb]::Get($p.Id).ToInt64()
