# Показывает, что видно от мыши Razer по текущей связи: все HID-коллекции,
# какая из них управляющая (feature report 91 байт), какой transaction id
# отвечает и что возвращают основные команды.
#
# Запускать по очереди для донгла и для Bluetooth, чтобы сравнить.

$ErrorActionPreference = 'Stop'

if (-not ('HidProbe' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public class HidProbe
{
    [StructLayout(LayoutKind.Sequential)]
    public struct HIDD_ATTRIBUTES { public int Size; public ushort VendorID, ProductID, VersionNumber; }

    [StructLayout(LayoutKind.Sequential)]
    public struct HIDP_CAPS {
        public ushort Usage, UsagePage;
        public ushort InputReportByteLength, OutputReportByteLength, FeatureReportByteLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)] public ushort[] Reserved;
        public ushort NumberLinkCollectionNodes;
        public ushort NumberInputButtonCaps, NumberInputValueCaps, NumberInputDataIndices;
        public ushort NumberOutputButtonCaps, NumberOutputValueCaps, NumberOutputDataIndices;
        public ushort NumberFeatureButtonCaps, NumberFeatureValueCaps, NumberFeatureDataIndices;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVICE_INTERFACE_DATA { public int cbSize; public Guid g; public int Flags; public IntPtr R; }

    [DllImport("hid.dll")] public static extern void HidD_GetHidGuid(out Guid g);
    [DllImport("hid.dll")] public static extern bool HidD_GetAttributes(IntPtr h, ref HIDD_ATTRIBUTES a);
    [DllImport("hid.dll")] public static extern bool HidD_GetPreparsedData(IntPtr h, out IntPtr p);
    [DllImport("hid.dll")] public static extern bool HidD_FreePreparsedData(IntPtr p);
    [DllImport("hid.dll")] public static extern int  HidP_GetCaps(IntPtr p, out HIDP_CAPS c);
    [DllImport("hid.dll")] public static extern bool HidD_SetFeature(IntPtr h, byte[] b, int len);
    [DllImport("hid.dll")] public static extern bool HidD_GetFeature(IntPtr h, byte[] b, int len);
    [DllImport("hid.dll", CharSet = CharSet.Unicode)] public static extern bool HidD_GetProductString(IntPtr h, byte[] b, int len);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SetupDiGetClassDevs(ref Guid g, IntPtr e, IntPtr h, int f);
    [DllImport("setupapi.dll")]
    public static extern bool SetupDiEnumDeviceInterfaces(IntPtr s, IntPtr d, ref Guid g, int i, ref SP_DEVICE_INTERFACE_DATA a);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, EntryPoint = "SetupDiGetDeviceInterfaceDetailW")]
    public static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr s, ref SP_DEVICE_INTERFACE_DATA a, IntPtr d, int size, ref int req, IntPtr dd);
    [DllImport("setupapi.dll")] public static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateFile(string p, uint a, uint s, IntPtr sa, uint d, uint f, IntPtr t);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);

    public class Dev { public string Path, Product; public ushort Pid, Usage, UsagePage, InLen, OutLen, FeatLen; }

    public static IntPtr Open(string path) {
        IntPtr h = CreateFile(path, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h == (IntPtr)(-1)) h = CreateFile(path, 0x40000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h == (IntPtr)(-1)) h = CreateFile(path, 0, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        return h;
    }

    public static List<Dev> Enumerate(ushort vid) {
        var list = new List<Dev>();
        Guid g; HidD_GetHidGuid(out g);
        IntPtr set = SetupDiGetClassDevs(ref g, IntPtr.Zero, IntPtr.Zero, 0x12);
        var ifd = new SP_DEVICE_INTERFACE_DATA(); ifd.cbSize = Marshal.SizeOf(ifd);
        for (int i = 0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref g, i, ref ifd); i++) {
            int req = 0;
            SetupDiGetDeviceInterfaceDetail(set, ref ifd, IntPtr.Zero, 0, ref req, IntPtr.Zero);
            if (req <= 0) continue;
            IntPtr buf = Marshal.AllocHGlobal(req);
            Marshal.WriteInt32(buf, IntPtr.Size == 8 ? 8 : 6);
            string path = null;
            if (SetupDiGetDeviceInterfaceDetail(set, ref ifd, buf, req, ref req, IntPtr.Zero))
                path = Marshal.PtrToStringUni(new IntPtr(buf.ToInt64() + 4));
            Marshal.FreeHGlobal(buf);
            if (path == null) continue;

            IntPtr h = Open(path);
            if (h == (IntPtr)(-1)) continue;
            var attr = new HIDD_ATTRIBUTES(); attr.Size = Marshal.SizeOf(attr);
            if (HidD_GetAttributes(h, ref attr) && attr.VendorID == vid) {
                IntPtr pp;
                if (HidD_GetPreparsedData(h, out pp)) {
                    HIDP_CAPS caps;
                    if (HidP_GetCaps(pp, out caps) == 0x110000) {
                        var name = new byte[256];
                        string product = HidD_GetProductString(h, name, name.Length)
                            ? System.Text.Encoding.Unicode.GetString(name).TrimEnd('\0') : "";
                        list.Add(new Dev { Path = path, Product = product, Pid = attr.ProductID,
                                           Usage = caps.Usage, UsagePage = caps.UsagePage,
                                           InLen = caps.InputReportByteLength,
                                           OutLen = caps.OutputReportByteLength,
                                           FeatLen = caps.FeatureReportByteLength });
                    }
                    HidD_FreePreparsedData(pp);
                }
            }
            CloseHandle(h);
        }
        SetupDiDestroyDeviceInfoList(set);
        return list;
    }
}
'@
}

function New-RazerReport {
    param([byte]$Tid, [byte]$Class, [byte]$Id, [byte]$Size, [byte[]]$Data = @())
    $r = New-Object byte[] 91
    $r[2] = $Tid; $r[6] = $Size; $r[7] = $Class; $r[8] = $Id
    for ($i = 0; $i -lt $Data.Length; $i++) { $r[9 + $i] = $Data[$i] }
    $crc = 0; for ($i = 3; $i -le 88; $i++) { $crc = $crc -bxor $r[$i] }
    $r[89] = [byte]$crc
    return $r
}

function Invoke-Razer {
    param([IntPtr]$H, [byte]$Tid, [byte]$Class, [byte]$Id, [byte]$Size, [byte[]]$Data = @())
    $req = New-RazerReport -Tid $Tid -Class $Class -Id $Id -Size $Size -Data $Data
    if (-not [HidProbe]::HidD_SetFeature($H, $req, 91)) { return $null }
    for ($t = 0; $t -lt 6; $t++) {
        Start-Sleep -Milliseconds 25
        $resp = New-Object byte[] 91
        if (-not [HidProbe]::HidD_GetFeature($H, $resp, 91)) { return $null }
        if ($resp[7] -eq $Class -and $resp[8] -eq $Id -and $resp[1] -ne 1) { return ,$resp[1..90] }
    }
    return $null
}

Write-Host '=== HID-коллекции Razer (VID 1532) ===' -ForegroundColor Cyan
$devs = [HidProbe]::Enumerate(0x1532)
if (-not $devs) { Write-Host 'ничего не найдено' -ForegroundColor Yellow; return }

foreach ($d in $devs) {
    $bt = if ($d.Path -match 'vid&0002|\{00001812|\{00001124|bthle|bthenum') { 'BT ' } else { 'USB' }
    "{0}  PID={1:X4} UP={2:X4} U={3:X2} in={4,3} out={5,3} feat={6,3}  {7}" -f `
        $bt, $d.Pid, $d.UsagePage, $d.Usage, $d.InLen, $d.OutLen, $d.FeatLen, $d.Product
    "     $($d.Path)"
}

Write-Host "`n=== Управляющие коллекции (feature = 91) ===" -ForegroundColor Cyan
$ctrl = $devs | Where-Object { $_.FeatLen -ge 91 }
if (-not $ctrl) {
    Write-Host 'Управляющей коллекции нет — по этой связи настройками управлять нельзя.' -ForegroundColor Yellow
    return
}

foreach ($d in $ctrl) {
    $h = [HidProbe]::Open($d.Path)
    if ($h -eq [IntPtr](-1)) { Write-Host "PID $('{0:X4}' -f $d.Pid): не открывается" -ForegroundColor Yellow; continue }
    foreach ($tid in @(0x1f, 0x3f, 0x08, 0x00, 0x9f, 0xff)) {
        $r = Invoke-Razer -H $h -Tid $tid -Class 0x00 -Id 0x81 -Size 0x02
        if ($r -and $r[0] -eq 2) {
            Write-Host ("PID {0:X4}: transaction id 0x{1:X2} отвечает, прошивка {2}.{3}" -f $d.Pid, $tid, $r[8], $r[9]) -ForegroundColor Green
            $dpi = Invoke-Razer -H $h -Tid $tid -Class 0x04 -Id 0x85 -Size 0x07 -Data @([byte]0x01)
            if ($dpi -and $dpi[0] -eq 2) { "        DPI: {0}" -f ([int]$dpi[9] * 256 + [int]$dpi[10]) }
            $st = Invoke-Razer -H $h -Tid $tid -Class 0x04 -Id 0x86 -Size 0x26 -Data @([byte]0x01)
            if ($st -and $st[0] -eq 2) { "        ступени: активна {0} из {1}" -f $st[9], $st[10] }
            $bat = Invoke-Razer -H $h -Tid $tid -Class 0x07 -Id 0x80 -Size 0x02
            if ($bat -and $bat[0] -eq 2) { "        заряд: {0}%" -f ([int]$bat[9] * 100 / 255) }
            break
        } elseif ($r) {
            Write-Host ("PID {0:X4}: tid 0x{1:X2} -> статус 0x{2:X2}" -f $d.Pid, $tid, $r[0]) -ForegroundColor DarkGray
        }
    }
    [HidProbe]::CloseHandle($h) | Out-Null
}
