#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <SetupAPI.h>
#include <WinUSB.h>
#include <usbiodef.h>
#include <objbase.h>

#include <algorithm>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "SetupAPI.lib")
#pragma comment(lib, "WinUSB.lib")
#pragma comment(lib, "Uuid.lib")
#pragma comment(lib, "Ole32.lib")

namespace {

const wchar_t* const kHardwareId = L"VID_1D27&PID_0601";

std::wstring getErrorMessage(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::wstring message = length != 0 ? buffer : L"Unknown error";
    if (buffer != nullptr) {
        LocalFree(buffer);
    }

    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

bool hasTargetHardwareId(HDEVINFO deviceInfoSet,
                         SP_DEVINFO_DATA& deviceInfoData) {
    DWORD requiredSize = 0;
    SetupDiGetDeviceRegistryPropertyW(deviceInfoSet, &deviceInfoData,
                                      SPDRP_HARDWAREID, nullptr, nullptr, 0,
                                      &requiredSize);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || requiredSize == 0) {
        return false;
    }

    std::vector<wchar_t> hardwareIds(requiredSize / sizeof(wchar_t));
    if (!SetupDiGetDeviceRegistryPropertyW(
            deviceInfoSet, &deviceInfoData, SPDRP_HARDWAREID, nullptr,
            reinterpret_cast<PBYTE>(hardwareIds.data()), requiredSize,
            nullptr)) {
        return false;
    }

    for (const wchar_t* id = hardwareIds.data(); *id != L'\0';
         id += wcslen(id) + 1) {
        if (wcsstr(id, kHardwareId) != nullptr) {
            std::wcout << L"Hardware ID: " << id << L'\n';
            return true;
        }
    }
    return false;
}

std::vector<GUID> getRegisteredInterfaceGuids(
    HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData) {
    std::vector<GUID> interfaceGuids;

    DWORD instanceIdLength = 0;
    SetupDiGetDeviceInstanceIdW(deviceInfoSet, &deviceInfoData, nullptr, 0,
                                &instanceIdLength);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        instanceIdLength == 0) {
        return interfaceGuids;
    }

    std::vector<wchar_t> instanceId(instanceIdLength);
    if (!SetupDiGetDeviceInstanceIdW(deviceInfoSet, &deviceInfoData,
                                     instanceId.data(), instanceIdLength,
                                     nullptr)) {
        return interfaceGuids;
    }

    const std::wstring keyPath =
        L"SYSTEM\\CurrentControlSet\\Enum\\" + std::wstring(instanceId.data()) +
        L"\\Device Parameters";
    HKEY deviceParameters = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_QUERY_VALUE,
                      &deviceParameters) != ERROR_SUCCESS) {
        return interfaceGuids;
    }

    DWORD valueType = 0;
    DWORD valueSize = 0;
    LONG result = RegQueryValueExW(deviceParameters, L"DeviceInterfaceGUIDs",
                                   nullptr, &valueType, nullptr, &valueSize);
    if (result != ERROR_SUCCESS || valueType != REG_MULTI_SZ ||
        valueSize < sizeof(wchar_t)) {
        RegCloseKey(deviceParameters);
        return interfaceGuids;
    }

    std::vector<wchar_t> values(valueSize / sizeof(wchar_t));
    result = RegQueryValueExW(
        deviceParameters, L"DeviceInterfaceGUIDs", nullptr, &valueType,
        reinterpret_cast<LPBYTE>(values.data()), &valueSize);
    RegCloseKey(deviceParameters);
    if (result != ERROR_SUCCESS) {
        return interfaceGuids;
    }

    for (const wchar_t* value = values.data(); *value != L'\0';
         value += wcslen(value) + 1) {
        GUID interfaceGuid = {};
        if (SUCCEEDED(CLSIDFromString(value, &interfaceGuid))) {
            interfaceGuids.push_back(interfaceGuid);
        } else {
            std::wcerr << L"Ignoring invalid registered interface GUID: "
                       << value << L'\n';
        }
    }
    return interfaceGuids;
}

std::vector<std::wstring> getInterfacePathsForDevice(
    const GUID& interfaceGuid, ULONG targetDevInst) {
    std::vector<std::wstring> paths;
    HDEVINFO deviceInfoSet = SetupDiGetClassDevsW(
        &interfaceGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return paths;
    }

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData = {};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(deviceInfoSet, nullptr, &interfaceGuid,
                                         index, &interfaceData)) {
            break;
        }

        SP_DEVINFO_DATA deviceInfoData = {};
        deviceInfoData.cbSize = sizeof(deviceInfoData);
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &interfaceData,
                                         nullptr, 0, &requiredSize, nullptr);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
            requiredSize < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }

        std::vector<BYTE> detailBuffer(requiredSize);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
            detailBuffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(
                deviceInfoSet, &interfaceData, detail, requiredSize, nullptr,
                &deviceInfoData) ||
            deviceInfoData.DevInst != targetDevInst) {
            continue;
        }

        paths.push_back(detail->DevicePath);
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return paths;
}

const wchar_t* pipeTypeName(UCHAR pipeType) {
    switch (pipeType) {
    case UsbdPipeTypeControl:
        return L"control";
    case UsbdPipeTypeIsochronous:
        return L"isochronous";
    case UsbdPipeTypeBulk:
        return L"bulk";
    case UsbdPipeTypeInterrupt:
        return L"interrupt";
    default:
        return L"unknown";
    }
}

bool inspectWinUsbHandle(WINUSB_INTERFACE_HANDLE usbHandle) {
    USB_DEVICE_DESCRIPTOR deviceDescriptor = {};
    ULONG descriptorLength = 0;
    if (!WinUsb_GetDescriptor(
            usbHandle, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
            reinterpret_cast<PUCHAR>(&deviceDescriptor),
            sizeof(deviceDescriptor), &descriptorLength)) {
        const DWORD error = GetLastError();
        std::wcerr << L"Could not read the USB device descriptor: "
                   << getErrorMessage(error) << L" (" << error << L")\n";
        return false;
    }

    std::wcout << L"USB descriptor read successfully.\n"
               << L"  USB version: " << std::hex << deviceDescriptor.bcdUSB
               << L"\n  Device version: " << deviceDescriptor.bcdDevice
               << L"\n  Configurations: " << std::dec
               << static_cast<unsigned>(deviceDescriptor.bNumConfigurations)
               << L"\n";

    bool foundSetting = false;
    for (UCHAR alternateIndex = 0; alternateIndex < 16; ++alternateIndex) {
        USB_INTERFACE_DESCRIPTOR interfaceDescriptor = {};
        if (!WinUsb_QueryInterfaceSettings(usbHandle, alternateIndex,
                                           &interfaceDescriptor)) {
            if (!foundSetting) {
                const DWORD error = GetLastError();
                std::wcerr << L"Could not query the default interface setting: "
                           << getErrorMessage(error) << L" (" << error
                           << L")\n";
                return false;
            }
            break;
        }

        foundSetting = true;
        std::wcout << L"Interface "
                   << static_cast<unsigned>(
                          interfaceDescriptor.bInterfaceNumber)
                   << L" (alternate setting "
                   << static_cast<unsigned>(
                          interfaceDescriptor.bAlternateSetting)
                   << L"): "
                   << static_cast<unsigned>(interfaceDescriptor.bNumEndpoints)
                   << L" endpoint(s)\n";
        for (UCHAR pipeIndex = 0;
             pipeIndex < interfaceDescriptor.bNumEndpoints; ++pipeIndex) {
            WINUSB_PIPE_INFORMATION pipeInfo = {};
            if (!WinUsb_QueryPipe(usbHandle, alternateIndex, pipeIndex,
                                  &pipeInfo)) {
                const DWORD error = GetLastError();
                std::wcerr << L"  Could not query endpoint "
                           << static_cast<unsigned>(pipeIndex) << L": "
                           << getErrorMessage(error) << L" (" << error
                           << L")\n";
                return false;
            }

            std::wcout << L"  Endpoint 0x" << std::hex
                       << static_cast<unsigned>(pipeInfo.PipeId) << std::dec
                       << L": " << pipeTypeName(pipeInfo.PipeType)
                       << L", max packet "
                       << pipeInfo.MaximumPacketSize << L" bytes\n";
        }
    }

    return true;
}

} // namespace

int wmain() {
    HDEVINFO deviceInfoSet = SetupDiGetClassDevsW(
        nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        std::wcerr << L"Could not enumerate USB devices: "
                   << getErrorMessage(error) << L" (" << error << L")\n";
        return 1;
    }

    bool foundDevice = false;
    bool openedDevice = false;
    bool inspectionSucceeded = false;

    for (DWORD deviceIndex = 0;; ++deviceIndex) {
        SP_DEVINFO_DATA deviceInfoData = {};
        deviceInfoData.cbSize = sizeof(deviceInfoData);
        if (!SetupDiEnumDeviceInfo(deviceInfoSet, deviceIndex,
                                   &deviceInfoData)) {
            const DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_ITEMS) {
                std::wcerr << L"USB device enumeration failed: "
                           << getErrorMessage(error) << L" (" << error
                           << L")\n";
                SetupDiDestroyDeviceInfoList(deviceInfoSet);
                return 1;
            }
            break;
        }

        if (!hasTargetHardwareId(deviceInfoSet, deviceInfoData)) {
            continue;
        }

        foundDevice = true;
        wchar_t instanceId[512] = {};
        if (SetupDiGetDeviceInstanceIdW(deviceInfoSet, &deviceInfoData,
                                        instanceId, 512, nullptr)) {
            std::wcout << L"Device instance: " << instanceId << L'\n';
        }
        wchar_t serviceName[256] = {};
        if (SetupDiGetDeviceRegistryPropertyW(
                deviceInfoSet, &deviceInfoData, SPDRP_SERVICE, nullptr,
                reinterpret_cast<PBYTE>(serviceName), sizeof(serviceName),
                nullptr)) {
            std::wcout << L"Driver service: " << serviceName << L'\n';
        }

        std::vector<std::wstring> paths;
        const std::vector<GUID> interfaceGuids =
            getRegisteredInterfaceGuids(deviceInfoSet, deviceInfoData);
        for (const GUID& interfaceGuid : interfaceGuids) {
            wchar_t guidText[64] = {};
            if (StringFromGUID2(interfaceGuid, guidText, 64) != 0) {
                std::wcout << L"Registered interface GUID: " << guidText
                           << L'\n';
            }
            const std::vector<std::wstring> registeredPaths =
                getInterfacePathsForDevice(interfaceGuid,
                                           deviceInfoData.DevInst);
            paths.insert(paths.end(), registeredPaths.begin(),
                         registeredPaths.end());
        }

        const std::vector<std::wstring> genericPaths =
            getInterfacePathsForDevice(GUID_DEVINTERFACE_USB_DEVICE,
                                       deviceInfoData.DevInst);
        for (const std::wstring& genericPath : genericPaths) {
            if (std::find(paths.begin(), paths.end(), genericPath) ==
                paths.end()) {
                paths.push_back(genericPath);
            }
        }
        if (paths.empty()) {
            std::wcerr << L"No device interface path was registered for this "
                          L"device function.\n";
        }

        for (const std::wstring& path : paths) {
            std::wcout << L"Trying device path: " << path << L'\n';
            HANDLE deviceHandle = CreateFileW(
                path.c_str(), GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
            if (deviceHandle == INVALID_HANDLE_VALUE) {
                const DWORD error = GetLastError();
                std::wcerr << L"Device open failed: " << getErrorMessage(error)
                           << L" (" << error << L")\n";
                continue;
            }

            WINUSB_INTERFACE_HANDLE usbHandle = nullptr;
            if (!WinUsb_Initialize(deviceHandle, &usbHandle)) {
                const DWORD error = GetLastError();
                std::wcerr << L"WinUSB initialization failed: "
                           << getErrorMessage(error) << L" (" << error
                           << L")\n";
                CloseHandle(deviceHandle);
                continue;
            }

            openedDevice = true;
            inspectionSucceeded = inspectWinUsbHandle(usbHandle);
            WinUsb_Free(usbHandle);
            CloseHandle(deviceHandle);
            if (inspectionSucceeded) {
                break;
            }
        }
        if (inspectionSucceeded) {
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);

    if (!foundDevice) {
        std::wcerr << L"No present USB device matched " << kHardwareId
                   << L".\n";
        return 2;
    }
    if (!openedDevice) {
        std::wcerr << L"The device is present, but no registered interface "
                      L"could be opened through WinUSB.\n";
        return 3;
    }
    if (!inspectionSucceeded) {
        return 4;
    }

    std::wcout << L"WinUSB access and descriptor/interface inspection "
                  L"succeeded. This does not yet verify the PrimeSense "
                  L"streaming protocol.\n";
    return 0;
}
