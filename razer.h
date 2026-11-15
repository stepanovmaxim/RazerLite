// razer.h - Razer HID protocol layer.
//
// Razer peripherals accept a 90-byte vendor report over a HID feature report
// (report id 0) on one of their collections. The same protocol answers on every
// modern mouse, so nothing here is model specific: the control collection, the
// transaction id and the supported commands are all discovered at runtime.
//
// Verified against a DeathAdder V2 X HyperSpeed (PID 009C): the control
// collection is the one with usage page 0x0001 and a 91-byte feature report,
// and transaction id 0x1F answers.

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpropdef.h>

#define RAZER_VID           0x1532
#define RAZER_REPORT_LEN    90
#define RAZER_BUFFER_LEN    91      // report id byte + report
#define RAZER_MAX_DEVICES   8
#define RAZER_MAX_STAGES    5

// response status byte
#define RAZER_STATUS_BUSY       0x01
#define RAZER_STATUS_OK         0x02
#define RAZER_STATUS_FAILURE    0x03
#define RAZER_STATUS_TIMEOUT    0x04
#define RAZER_STATUS_UNSUPPORTED 0x05

// variable storage selector
#define RAZER_NOSTORE       0x00
#define RAZER_VARSTORE      0x01

#pragma pack(push, 1)
typedef struct {
    BYTE  status;
    BYTE  transaction_id;
    USHORT remaining;           // big endian, always 0 for us
    BYTE  protocol_type;
    BYTE  data_size;
    BYTE  command_class;
    BYTE  command_id;
    BYTE  arguments[80];
    BYTE  crc;
    BYTE  reserved;
} RazerReport;
#pragma pack(pop)

typedef struct {
    WCHAR  path[512];
    WCHAR  name[128];
    USHORT pid;
    BOOL   bluetooth;           // paired over Bluetooth rather than USB/dongle

    // Over Bluetooth LE there is no vendor protocol at all: the mouse offers
    // plain HID plus Razer's own GATT service, which is not documented. What is
    // still available is the battery level (Windows reads it from the standard
    // GATT battery service and publishes it as a device property) and the DPI,
    // which the mouse announces by itself whenever its DPI button is pressed.
    BOOL   bleOnly;
    WCHAR  bleInstance[256];    // BTHLE device node, for the battery property

    // Substring that picks this device's HID collections out of the enumeration:
    // "pid_009c" for a USB device, the Bluetooth address for a paired one. Both
    // links can be present at once, and their collections must not be mixed up.
    WCHAR  matchKey[24];
    BYTE   tid;                 // transaction id that this device answers on
    HANDLE handle;              // cached, INVALID_HANDLE_VALUE when closed

    // A sleeping mouse still has a working transport: the receiver answers
    // every command with status 0x04 until the mouse wakes up. `probed` says
    // whether the capability list below was ever filled in for real.
    BOOL   awake;
    BOOL   probed;

    // capabilities, probed once per connect
    BOOL   capDpi;
    BOOL   capStages;
    BOOL   capPollRate;
    BOOL   capBattery;
    BOOL   capCharging;
    BOOL   capIdle;
    int    maxDpi;

    // last known state
    int    dpiX, dpiY;
    int    stageCount;
    int    activeStage;         // 1-based, 0 = unknown
    int    stageDpi[RAZER_MAX_STAGES];
    int    battery;             // 0..100, -1 unknown
    BOOL   charging;
    int    pollRate;            // Hz, 0 unknown
    int    idleSeconds;
    int    deviceMode;          // 0x00 onboard, 0x03 driver, -1 unknown
    BYTE   fwMajor, fwMinor;
    WCHAR  serial[32];
} RazerDevice;

// ----------------------------------------------------------------- internals

// Case-insensitive substring test; `needle` must already be lower case.
static BOOL RzPathContains(const WCHAR *path, const WCHAR *needle) {
    for (; *path; ++path) {
        int i = 0;
        while (needle[i]) {
            WCHAR c = path[i];
            if (c >= L'A' && c <= L'Z') c = (WCHAR)(c + 32);
            if (c != needle[i]) break;
            ++i;
        }
        if (!needle[i]) return TRUE;
    }
    return FALSE;
}

// A Bluetooth-attached device spells its vendor id differently - USB writes
// "VID_1532", Bluetooth writes "VID&00021532" - and carries the Bluetooth LE
// HID service guid in the path. Both forms have to be recognised, or the same
// mouse looks like a different one depending on how it is connected.
static BOOL RzIsBluetoothPath(const WCHAR *path) {
    return RzPathContains(path, L"vid&0002") ||
           RzPathContains(path, L"{00001812") ||
           RzPathContains(path, L"{00001124") ||
           RzPathContains(path, L"bthledevice") ||
           RzPathContains(path, L"bthenum");
}

static void RzZero(void *dst, int count) {
    BYTE *p = (BYTE *)dst;
    while (count--) *p++ = 0;
}

static BYTE RzCrc(const RazerReport *r) {
    const BYTE *b = (const BYTE *)r;
    BYTE crc = 0;
    for (int i = 2; i < 88; ++i) crc = (BYTE)(crc ^ b[i]);
    return crc;
}

static HANDLE RzOpenPath(const WCHAR *path, BOOL forRead) {
    DWORD access[3];
    access[0] = GENERIC_READ | GENERIC_WRITE;
    access[1] = forRead ? GENERIC_READ : GENERIC_WRITE;
    access[2] = 0;
    for (int i = 0; i < 3; ++i) {
        HANDLE h = CreateFileW(path, access[i], FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) return h;
    }
    return INVALID_HANDLE_VALUE;
}

static BOOL RzEnsureOpen(RazerDevice *dev) {
    if (dev->handle != INVALID_HANDLE_VALUE) return TRUE;
    dev->handle = RzOpenPath(dev->path, FALSE);
    return dev->handle != INVALID_HANDLE_VALUE;
}

static void RazerCloseDevice(RazerDevice *dev) {
    if (dev->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(dev->handle);
        dev->handle = INVALID_HANDLE_VALUE;
    }
}

// One request/response round trip. `args` is copied into the report arguments.
// Returns the status byte, or 0xFF when the transport itself failed.
static BYTE RazerCommandTid(RazerDevice *dev, BYTE tid, BYTE cls, BYTE id, BYTE size,
                            const BYTE *args, int argLen, RazerReport *out) {
    if (!RzEnsureOpen(dev)) return 0xFF;

    BYTE buffer[RAZER_BUFFER_LEN];
    RazerReport *req = (RazerReport *)(buffer + 1);
    RzZero(buffer, sizeof(buffer));
    req->transaction_id = tid;
    req->data_size      = size;
    req->command_class  = cls;
    req->command_id     = id;
    for (int i = 0; i < argLen && i < 80; ++i) req->arguments[i] = args[i];
    req->crc = RzCrc(req);

    if (!HidD_SetFeature(dev->handle, buffer, RAZER_BUFFER_LEN)) {
        RazerCloseDevice(dev);              // stale handle after a reconnect
        return 0xFF;
    }

    // The device needs a moment to prepare the answer; poll instead of picking
    // one worst-case delay. A wireless link occasionally answers BUSY first.
    for (int attempt = 0; attempt < 8; ++attempt) {
        Sleep(attempt == 0 ? 25 : 15);

        BYTE resp[RAZER_BUFFER_LEN];
        RzZero(resp, sizeof(resp));
        if (!HidD_GetFeature(dev->handle, resp, RAZER_BUFFER_LEN)) {
            RazerCloseDevice(dev);
            return 0xFF;
        }
        RazerReport *r = (RazerReport *)(resp + 1);
        if (r->command_class != cls || r->command_id != id) continue;
        if (r->status == RAZER_STATUS_BUSY) continue;
        if (out) *out = *r;
        return r->status;
    }
    return RAZER_STATUS_TIMEOUT;
}

static BYTE RazerCommand(RazerDevice *dev, BYTE cls, BYTE id, BYTE size,
                         const BYTE *args, int argLen, RazerReport *out) {
    return RazerCommandTid(dev, dev->tid, cls, id, size, args, argLen, out);
}

static BOOL RazerOk(RazerDevice *dev, BYTE cls, BYTE id, BYTE size,
                    const BYTE *args, int argLen, RazerReport *out) {
    return RazerCommand(dev, cls, id, size, args, argLen, out) == RAZER_STATUS_OK;
}

// ------------------------------------------------------------------ commands

static BOOL RazerGetSerial(RazerDevice *dev) {
    RazerReport r;
    if (!RazerOk(dev, 0x00, 0x82, 0x16, NULL, 0, &r)) return FALSE;
    char ascii[23];
    for (int i = 0; i < 22; ++i) ascii[i] = (char)r.arguments[i];
    ascii[22] = 0;
    MultiByteToWideChar(CP_ACP, 0, ascii, -1, dev->serial, 32);
    dev->serial[31] = 0;
    return TRUE;
}

static BOOL RazerGetDpi(RazerDevice *dev) {
    BYTE args[1] = { RAZER_VARSTORE };
    RazerReport r;
    if (!RazerOk(dev, 0x04, 0x85, 0x07, args, 1, &r)) return FALSE;
    dev->dpiX = ((int)r.arguments[1] << 8) | r.arguments[2];
    dev->dpiY = ((int)r.arguments[3] << 8) | r.arguments[4];
    return TRUE;
}

static BOOL RazerSetDpi(RazerDevice *dev, int x, int y, BOOL persist) {
    BYTE args[7];
    args[0] = persist ? RAZER_VARSTORE : RAZER_NOSTORE;
    args[1] = (BYTE)((x >> 8) & 0xFF); args[2] = (BYTE)(x & 0xFF);
    args[3] = (BYTE)((y >> 8) & 0xFF); args[4] = (BYTE)(y & 0xFF);
    args[5] = 0; args[6] = 0;
    return RazerOk(dev, 0x04, 0x05, 0x07, args, 7, NULL);
}

// The stage table is read/written as a whole: {varstore, active, count,
// then 7 bytes per stage: id, xhi, xlo, yhi, ylo, 0, 0}.
static BOOL RazerGetStages(RazerDevice *dev) {
    BYTE args[1] = { RAZER_VARSTORE };
    RazerReport r;
    if (!RazerOk(dev, 0x04, 0x86, 0x26, args, 1, &r)) return FALSE;

    int active = r.arguments[1];
    int count  = r.arguments[2];
    if (count < 1 || count > RAZER_MAX_STAGES) return FALSE;

    dev->activeStage = active;
    dev->stageCount  = count;
    for (int i = 0; i < count; ++i) {
        const BYTE *s = r.arguments + 3 + i * 7;
        dev->stageDpi[i] = ((int)s[1] << 8) | s[2];
    }
    return TRUE;
}

static BOOL RazerSetStages(RazerDevice *dev, int active, int count, const int *dpi) {
    if (count < 1 || count > RAZER_MAX_STAGES) return FALSE;
    if (active < 1) active = 1;
    if (active > count) active = count;

    BYTE args[3 + RAZER_MAX_STAGES * 7];
    RzZero(args, sizeof(args));
    args[0] = RAZER_VARSTORE;
    args[1] = (BYTE)active;
    args[2] = (BYTE)count;
    for (int i = 0; i < count; ++i) {
        BYTE *s = args + 3 + i * 7;
        s[0] = (BYTE)(i + 1);
        s[1] = (BYTE)((dpi[i] >> 8) & 0xFF); s[2] = (BYTE)(dpi[i] & 0xFF);
        s[3] = s[1];                          s[4] = s[2];
    }
    return RazerOk(dev, 0x04, 0x06, 0x26, args, 3 + count * 7, NULL);
}

static int RzPollValueToHz(BYTE v) {
    switch (v) {
    case 1: return 1000;
    case 2: return 500;
    case 4: return 250;
    case 8: return 125;
    default: return 0;
    }
}

static BYTE RzHzToPollValue(int hz) {
    switch (hz) {
    case 1000: return 1;
    case 500:  return 2;
    case 250:  return 4;
    case 125:  return 8;
    default:   return 0;
    }
}

static BOOL RazerGetPollRate(RazerDevice *dev) {
    RazerReport r;
    if (!RazerOk(dev, 0x00, 0x85, 0x01, NULL, 0, &r)) return FALSE;
    int hz = RzPollValueToHz(r.arguments[0]);
    if (!hz) return FALSE;
    dev->pollRate = hz;
    return TRUE;
}

static BOOL RazerSetPollRate(RazerDevice *dev, int hz) {
    BYTE v = RzHzToPollValue(hz);
    if (!v) return FALSE;
    return RazerOk(dev, 0x00, 0x05, 0x01, &v, 1, NULL);
}

static BOOL RazerGetBattery(RazerDevice *dev) {
    RazerReport r;
    if (!RazerOk(dev, 0x07, 0x80, 0x02, NULL, 0, &r)) return FALSE;
    dev->battery = (int)r.arguments[1] * 100 / 255;
    return TRUE;
}

static BOOL RazerGetCharging(RazerDevice *dev) {
    RazerReport r;
    if (!RazerOk(dev, 0x07, 0x84, 0x02, NULL, 0, &r)) return FALSE;
    dev->charging = r.arguments[1] != 0;
    return TRUE;
}

static BOOL RazerGetIdleTime(RazerDevice *dev) {
    RazerReport r;
    if (!RazerOk(dev, 0x07, 0x83, 0x02, NULL, 0, &r)) return FALSE;
    dev->idleSeconds = ((int)r.arguments[0] << 8) | r.arguments[1];
    return TRUE;
}

static BOOL RazerSetIdleTime(RazerDevice *dev, int seconds) {
    BYTE args[2] = { (BYTE)((seconds >> 8) & 0xFF), (BYTE)(seconds & 0xFF) };
    return RazerOk(dev, 0x07, 0x03, 0x02, args, 2, NULL);
}

static int RazerGetDeviceMode(RazerDevice *dev) {
    RazerReport r;
    if (!RazerOk(dev, 0x00, 0x84, 0x02, NULL, 0, &r)) return -1;
    return r.arguments[0];
}

static BOOL RazerSetDeviceMode(RazerDevice *dev, BYTE mode) {
    BYTE args[2] = { mode, 0x00 };
    return RazerOk(dev, 0x00, 0x04, 0x02, args, 2, NULL);
}

// ----------------------------------------------------------------- discovery

// Sensor ceilings for models where offering the full ladder would be wrong.
// Anything unlisted gets the generic ceiling - the device clamps or rejects a
// value it cannot do, and every write is read back anyway.
static int RzMaxDpiFor(USHORT pid) {
    switch (pid) {
    case 0x009C: return 14000;   // DeathAdder V2 X HyperSpeed
    case 0x0083: return 16000;   // Basilisk X HyperSpeed
    case 0x008A: return  8500;   // Viper Mini
    case 0x008C: return  8500;   // DeathAdder V2 Mini
    case 0x0094: case 0x0095: return 18000;  // Orochi V2
    case 0x0096: return 18000;   // Naga X
    case 0x0098: return  6400;   // DeathAdder Essential (2021)
    default:     return 16000;
    }
}

static BOOL RzIsControlCollection(HANDLE h, USHORT *pidOut) {
    HIDD_ATTRIBUTES attr;
    attr.Size = sizeof(attr);
    if (!HidD_GetAttributes(h, &attr) || attr.VendorID != RAZER_VID) return FALSE;

    PHIDP_PREPARSED_DATA pp = NULL;
    BOOL match = FALSE;
    if (HidD_GetPreparsedData(h, &pp)) {
        HIDP_CAPS caps;
        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS)
            match = (caps.FeatureReportByteLength >= RAZER_BUFFER_LEN);
        HidD_FreePreparsedData(pp);
    }
    if (match && pidOut) *pidOut = attr.ProductID;
    return match;
}

#define RZ_ABSENT   0
#define RZ_ASLEEP   1
#define RZ_AWAKE    2

// Finds the transaction id this device answers on. Wireless models use 0x1F or
// 0x3F, wired ones usually 0x00 or 0x08 - probing is cheaper than a table.
// A sleeping mouse answers nothing but still frames a reply, which is enough to
// recognise the device and settle on a transaction id.
static int RzProbeTransactionId(RazerDevice *dev) {
    static const BYTE candidates[] = { 0x1F, 0x3F, 0x08, 0x00, 0x9F, 0xFF };
    BYTE framed = 0;
    BOOL anyFramed = FALSE;

    for (int i = 0; i < (int)ARRAYSIZE(candidates); ++i) {
        RazerReport r;
        BYTE status = RazerCommandTid(dev, candidates[i], 0x00, 0x81, 0x02, NULL, 0, &r);
        if (status == RAZER_STATUS_OK) {
            dev->tid     = candidates[i];
            dev->fwMajor = r.arguments[0];
            dev->fwMinor = r.arguments[1];
            return RZ_AWAKE;
        }
        if (status == RAZER_STATUS_TIMEOUT && !anyFramed) {
            framed = candidates[i];
            anyFramed = TRUE;
        }
    }
    if (anyFramed) {
        dev->tid = framed;
        return RZ_ASLEEP;
    }
    return RZ_ABSENT;
}

// Asks the device what it can do. Unsupported commands answer with status 0x05,
// which is how the menu decides what to show. Returns FALSE while the mouse is
// asleep, so the caller knows to ask again later.
static BOOL RazerProbeCaps(RazerDevice *dev) {
    dev->maxDpi = RzMaxDpiFor(dev->pid);

    RazerReport r;
    if (RazerCommand(dev, 0x00, 0x81, 0x02, NULL, 0, &r) != RAZER_STATUS_OK) {
        dev->awake = FALSE;
        return FALSE;
    }
    dev->fwMajor = r.arguments[0];
    dev->fwMinor = r.arguments[1];

    dev->battery     = -1;
    dev->capDpi      = RazerGetDpi(dev);
    dev->capStages   = RazerGetStages(dev);
    dev->capPollRate = RazerGetPollRate(dev);
    dev->capBattery  = RazerGetBattery(dev);
    dev->capCharging = RazerGetCharging(dev);
    dev->capIdle     = RazerGetIdleTime(dev);
    dev->deviceMode  = RazerGetDeviceMode(dev);
    RazerGetSerial(dev);

    dev->awake  = TRUE;
    dev->probed = TRUE;
    return TRUE;
}

// ------------------------------------------------------------ Bluetooth LE

// {104EA319-6EE2-4701-BD47-8DDBF425BBE5},2 - what Windows fills in from the
// standard GATT battery service, and what Settings shows for a paired device.
static const DEVPROPKEY RZ_DEVPKEY_Bluetooth_Battery = {
    { 0x104EA319, 0x6EE2, 0x4701, { 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5 } }, 2
};
static const DEVPROPKEY RZ_DEVPKEY_NAME = {
    { 0xB725F130, 0x47EF, 0x101A, { 0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC } }, 10
};

// Razer's own GATT service. Its presence marks the device as a Razer one no
// matter what vendor id the Bluetooth firmware reports - a DeathAdder V2 X
// announces 068E over BLE rather than Razer's own 1532.
static const WCHAR RZ_BLE_SERVICE[] = L"{52401523-f97c-7f90-0e7f-6c6f4e36db1c}";

static WCHAR *RzGetDeviceIdList(const WCHAR *enumerator) {
    ULONG len = 0;
    ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    if (CM_Get_Device_ID_List_SizeW(&len, enumerator, flags) != CR_SUCCESS || len < 2) return NULL;
    WCHAR *buffer = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len * sizeof(WCHAR));
    if (!buffer) return NULL;
    if (CM_Get_Device_ID_ListW(enumerator, buffer, len, flags) != CR_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, buffer);
        return NULL;
    }
    return buffer;
}

// The Bluetooth address is the last underscore-separated field of the middle
// section of a BTHLEDEVICE instance id:
//   BTHLEDEVICE\{service}_DEV_VID&02068E_PID&009D_REV&0001_AABBCCDDEEFF\A&...
// Note the section ends at the LAST backslash, not the first: the enumerator
// name in front contributes one of its own.
static BOOL RzExtractBleAddress(const WCHAR *instanceId, WCHAR *out, int cch) {
    const WCHAR *end = NULL, *lastUnderscore = NULL;
    for (const WCHAR *p = instanceId; *p; ++p)
        if (*p == L'\\') end = p;
    if (!end) return FALSE;
    for (const WCHAR *p = instanceId; p < end; ++p)
        if (*p == L'_') lastUnderscore = p;
    if (!lastUnderscore) return FALSE;

    const WCHAR *addr = lastUnderscore + 1;
    int len = (int)(end - addr);
    if (len != 12 || len >= cch) return FALSE;
    for (int i = 0; i < len; ++i) {
        WCHAR c = addr[i];
        if (c >= L'A' && c <= L'Z') c = (WCHAR)(c + 32);
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return FALSE;
        out[i] = c;
    }
    out[len] = 0;
    return TRUE;
}

static BOOL RzBleReadBattery(const WCHAR *instanceId, int *percent) {
    DEVINST devInst;
    if (CM_Locate_DevNodeW(&devInst, (DEVINSTID_W)instanceId, CM_LOCATE_DEVNODE_NORMAL)
        != CR_SUCCESS)
        return FALSE;
    DEVPROPTYPE type = 0;
    BYTE value = 0;
    ULONG size = sizeof(value);
    if (CM_Get_DevNode_PropertyW(devInst, &RZ_DEVPKEY_Bluetooth_Battery, &type,
                                 &value, &size, 0) != CR_SUCCESS || type != DEVPROP_TYPE_BYTE)
        return FALSE;
    *percent = value;
    return TRUE;
}

static BOOL RzBleReadName(const WCHAR *instanceId, WCHAR *out, int cch) {
    DEVINST devInst;
    if (CM_Locate_DevNodeW(&devInst, (DEVINSTID_W)instanceId, CM_LOCATE_DEVNODE_NORMAL)
        != CR_SUCCESS)
        return FALSE;
    DEVPROPTYPE type = 0;
    ULONG size = (ULONG)(cch * sizeof(WCHAR));
    if (CM_Get_DevNode_PropertyW(devInst, &RZ_DEVPKEY_NAME, &type, (PBYTE)out, &size, 0)
        != CR_SUCCESS || type != DEVPROP_TYPE_STRING)
        return FALSE;
    return TRUE;
}

// Adds every Razer mouse that is paired over Bluetooth. They are found by
// address: first the GATT services are scanned for Razer's own service, then
// the matching BTHLE node supplies the name and battery.
static int RazerFindBleDevices(RazerDevice *out, int maxDevices, int found) {
    WCHAR *services = RzGetDeviceIdList(L"BTHLEDEVICE");
    if (!services) return found;

    WCHAR *nodes = RzGetDeviceIdList(L"BTHLE");

    for (WCHAR *id = services; *id && found < maxDevices; id += lstrlenW(id) + 1) {
        if (!RzPathContains(id, RZ_BLE_SERVICE)) continue;

        WCHAR address[16];
        if (!RzExtractBleAddress(id, address, ARRAYSIZE(address))) continue;

        BOOL duplicate = FALSE;
        for (int k = 0; k < found; ++k)
            if (out[k].bleOnly && !lstrcmpiW(out[k].matchKey, address)) { duplicate = TRUE; break; }
        if (duplicate) continue;

        RazerDevice *dev = &out[found];
        RzZero(dev, sizeof(*dev));
        dev->handle     = INVALID_HANDLE_VALUE;
        dev->bluetooth  = TRUE;
        dev->bleOnly    = TRUE;
        dev->battery    = -1;
        dev->deviceMode = -1;
        dev->maxDpi     = 0;
        lstrcpynW(dev->matchKey, address, ARRAYSIZE(dev->matchKey));

        // the BTHLE node for this address carries the name and the battery
        if (nodes) {
            for (WCHAR *node = nodes; *node; node += lstrlenW(node) + 1) {
                if (!RzPathContains(node, address)) continue;
                lstrcpynW(dev->bleInstance, node, ARRAYSIZE(dev->bleInstance));
                break;
            }
        }
        if (!dev->bleInstance[0]) continue;

        WCHAR name[96];
        name[0] = 0;
        if (RzBleReadName(dev->bleInstance, name, ARRAYSIZE(name)) && name[0])
            wsprintfW(dev->name, L"%.90s (Bluetooth)", name);
        else
            lstrcpyW(dev->name, L"Razer mouse (Bluetooth)");

        dev->capBattery = RzBleReadBattery(dev->bleInstance, &dev->battery);
        dev->probed = TRUE;
        dev->awake  = TRUE;
        ++found;
    }

    if (nodes) HeapFree(GetProcessHeap(), 0, nodes);
    HeapFree(GetProcessHeap(), 0, services);
    return found;
}

// Fills `out` with every Razer device that answers the protocol. Returns count.
static int RazerFindDevices(RazerDevice *out, int maxDevices) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return 0;

    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(iface);
    union {
        SP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
        BYTE raw[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 512 * sizeof(WCHAR)];
    } buffer;

    int found = 0;
    for (DWORD i = 0; found < maxDevices &&
                      SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &iface); ++i) {
        buffer.detail.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &iface, &buffer.detail,
                                              sizeof(buffer), NULL, NULL))
            continue;

        HANDLE h = RzOpenPath(buffer.detail.DevicePath, FALSE);
        if (h == INVALID_HANDLE_VALUE) continue;

        USHORT pid = 0;
        BOOL isControl = RzIsControlCollection(h, &pid);
        WCHAR product[128];
        product[0] = 0;
        if (isControl) HidD_GetProductString(h, product, sizeof(product));
        CloseHandle(h);
        if (!isControl) continue;

        // A model can expose more than one 91-byte collection; the first one
        // that answers is the control interface, the rest are the same mouse.
        // The transport is part of the identity: a mouse on its dongle and the
        // same mouse over Bluetooth are two entries, not one.
        BOOL bluetooth = RzIsBluetoothPath(buffer.detail.DevicePath);
        BOOL duplicate = FALSE;
        for (int k = 0; k < found; ++k)
            if (out[k].pid == pid && out[k].bluetooth == bluetooth) { duplicate = TRUE; break; }
        if (duplicate) continue;

        RazerDevice *dev = &out[found];
        RzZero(dev, sizeof(*dev));
        dev->handle    = INVALID_HANDLE_VALUE;
        dev->pid       = pid;
        dev->bluetooth = RzIsBluetoothPath(buffer.detail.DevicePath);
        wsprintfW(dev->matchKey, L"pid_%04x", pid);
        lstrcpynW(dev->path, buffer.detail.DevicePath, ARRAYSIZE(dev->path));
        if (product[0]) lstrcpynW(dev->name, product, ARRAYSIZE(dev->name));
        else            wsprintfW(dev->name, L"Razer device %04X", pid);
        // the same mouse can be present twice at once - on its dongle and over
        // Bluetooth - so the menu has to be able to tell them apart
        if (dev->bluetooth) {
            int len = lstrlenW(dev->name);
            if (len < ARRAYSIZE(dev->name) - 14) lstrcatW(dev->name, L" (Bluetooth)");
        }

        int state = RzProbeTransactionId(dev);
        if (state == RZ_ABSENT) {
            RazerCloseDevice(dev);
            continue;
        }
        dev->battery    = -1;
        dev->deviceMode = -1;
        dev->maxDpi     = RzMaxDpiFor(dev->pid);
        if (state == RZ_AWAKE) RazerProbeCaps(dev);
        ++found;
    }

    SetupDiDestroyDeviceInfoList(devInfo);

    // A mouse on its dongle and the same mouse over Bluetooth are separate
    // entries: only one of them is live at a time, and they can do different
    // things.
    found = RazerFindBleDevices(out, maxDevices, found);
    return found;
}
