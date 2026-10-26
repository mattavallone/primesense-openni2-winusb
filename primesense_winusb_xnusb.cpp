#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <SetupAPI.h>
#include <WinUSB.h>
#include <usbiodef.h>
#include <objbase.h>
#include <XnUSB.h>
#include <XnStatusCodes.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "SetupAPI.lib")
#pragma comment(lib, "WinUSB.lib")
#pragma comment(lib, "Ole32.lib")

struct XnUSBDeviceHandle {
    HANDLE file;
    WINUSB_INTERFACE_HANDLE usb;
    UCHAR alternateSetting;
    XnUSBDeviceSpeed speed;
    char devicePath[XN_FILE_MAX_PATH];
};

struct XnUSBEndPointHandle {
    XN_USB_DEV_HANDLE device;
    UCHAR pipeId;
    XnUSBEndPointType type;
    XnUSBDirectionType direction;
    ULONG maxPacketSize;
    OVERLAPPED pendingRead;
    bool hasPendingRead;
    HANDLE readThread;
    volatile LONG stopReadThread;
    XnUSBReadCallbackFunctionPtr readCallback;
    void* readCallbackData;
    ULONG readBufferSize;
    ULONG readTimeout;
};

namespace {

struct DeviceRegistration {
    XnUSBDeviceCallbackFunctionPtr callback;
    XnUSBEventCallbackFunctionPtr legacyCallback;
    void* cookie;
    void* legacyCookie;
    USHORT vendorId;
    USHORT productId;
    HANDLE watcherThread;
    volatile LONG stopWatcher;
};

DeviceRegistration* g_legacyRegistration = nullptr;

DWORD mapTransferError(DWORD error) {
    if (error == ERROR_SEM_TIMEOUT || error == ERROR_TIMEOUT ||
        error == WAIT_TIMEOUT) {
        return ERROR_SEM_TIMEOUT;
    }
    return error;
}

bool matchesHardwareId(HDEVINFO deviceInfoSet,
                       SP_DEVINFO_DATA& deviceInfoData,
                       USHORT vendorId, USHORT productId) {
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

    wchar_t targetId[32] = {};
    swprintf_s(targetId, L"VID_%04X&PID_%04X", vendorId, productId);
    for (const wchar_t* id = hardwareIds.data(); *id != L'\0';
         id += wcslen(id) + 1) {
        if (wcsstr(id, targetId) != nullptr) {
            return true;
        }
    }
    return false;
}

std::vector<GUID> getRegisteredInterfaceGuids(
    HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData) {
    DWORD instanceIdLength = 0;
    SetupDiGetDeviceInstanceIdW(deviceInfoSet, &deviceInfoData, nullptr, 0,
                                &instanceIdLength);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        instanceIdLength == 0) {
        return {};
    }

    std::vector<wchar_t> instanceId(instanceIdLength);
    if (!SetupDiGetDeviceInstanceIdW(deviceInfoSet, &deviceInfoData,
                                     instanceId.data(), instanceIdLength,
                                     nullptr)) {
        return {};
    }

    const std::wstring keyPath =
        L"SYSTEM\\CurrentControlSet\\Enum\\" + std::wstring(instanceId.data()) +
        L"\\Device Parameters";
    HKEY deviceParameters = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_QUERY_VALUE,
                      &deviceParameters) != ERROR_SUCCESS) {
        return {};
    }

    DWORD valueType = 0;
    DWORD valueSize = 0;
    LONG result = RegQueryValueExW(deviceParameters, L"DeviceInterfaceGUIDs",
                                   nullptr, &valueType, nullptr, &valueSize);
    if (result != ERROR_SUCCESS || valueType != REG_MULTI_SZ ||
        valueSize < sizeof(wchar_t)) {
        RegCloseKey(deviceParameters);
        return {};
    }

    std::vector<wchar_t> values(valueSize / sizeof(wchar_t));
    result = RegQueryValueExW(
        deviceParameters, L"DeviceInterfaceGUIDs", nullptr, &valueType,
        reinterpret_cast<LPBYTE>(values.data()), &valueSize);
    RegCloseKey(deviceParameters);
    if (result != ERROR_SUCCESS) {
        return {};
    }

    std::vector<GUID> guids;
    for (const wchar_t* value = values.data(); *value != L'\0';
         value += wcslen(value) + 1) {
        GUID guid = {};
        if (SUCCEEDED(CLSIDFromString(value, &guid))) {
            guids.push_back(guid);
        }
    }
    return guids;
}

void appendDeviceInterfacePaths(const GUID& interfaceGuid, ULONG targetDevInst,
                                std::vector<std::string>& paths) {
    HDEVINFO deviceInfoSet = SetupDiGetClassDevsW(
        &interfaceGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return;
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

        const int charsNeeded = WideCharToMultiByte(
            CP_ACP, 0, detail->DevicePath, -1, nullptr, 0, nullptr, nullptr);
        if (charsNeeded <= 0) {
            continue;
        }
        std::string path(static_cast<size_t>(charsNeeded), '\0');
        if (WideCharToMultiByte(CP_ACP, 0, detail->DevicePath, -1,
                                &path[0], charsNeeded, nullptr, nullptr) > 0) {
            path.resize(std::strlen(path.c_str()));
            paths.push_back(path);
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
}

std::vector<std::string> findDevicePaths(USHORT vendorId, USHORT productId) {
    std::vector<std::string> paths;
    HDEVINFO deviceInfoSet = SetupDiGetClassDevsW(
        nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return paths;
    }

    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA deviceInfoData = {};
        deviceInfoData.cbSize = sizeof(deviceInfoData);
        if (!SetupDiEnumDeviceInfo(deviceInfoSet, index, &deviceInfoData)) {
            break;
        }
        if (!matchesHardwareId(deviceInfoSet, deviceInfoData, vendorId,
                               productId)) {
            continue;
        }

        wchar_t serviceName[128] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(
                deviceInfoSet, &deviceInfoData, SPDRP_SERVICE, nullptr,
                reinterpret_cast<PBYTE>(serviceName), sizeof(serviceName),
                nullptr) ||
            _wcsicmp(serviceName, L"WinUSB") != 0) {
            continue;
        }

        for (const GUID& interfaceGuid :
             getRegisteredInterfaceGuids(deviceInfoSet, deviceInfoData)) {
            appendDeviceInterfacePaths(interfaceGuid, deviceInfoData.DevInst,
                                       paths);
        }
    }
    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return paths;
}

void dispatchDeviceEvent(DeviceRegistration* registration,
                         XnUSBEventType eventType, const std::string& path) {
    if (registration->callback != nullptr) {
        XnUSBEventArgs args = {};
        args.strDevicePath = path.c_str();
        args.eventType = eventType;
        registration->callback(&args, registration->cookie);
    } else if (registration->legacyCallback != nullptr) {
        std::vector<char> mutablePath(path.begin(), path.end());
        mutablePath.push_back('\0');
        registration->legacyCallback(eventType, mutablePath.data(),
                                     registration->legacyCookie);
    }
}

DWORD WINAPI deviceWatcherThread(void* parameter) {
    DeviceRegistration* registration =
        static_cast<DeviceRegistration*>(parameter);
    std::vector<std::string> previousPaths;

    while (InterlockedCompareExchange(&registration->stopWatcher, FALSE,
                                      FALSE) == FALSE) {
        const std::vector<std::string> currentPaths =
            findDevicePaths(registration->vendorId, registration->productId);

        for (const std::string& path : currentPaths) {
            if (std::find(previousPaths.begin(), previousPaths.end(), path) ==
                previousPaths.end()) {
                dispatchDeviceEvent(registration, XN_USB_EVENT_DEVICE_CONNECT,
                                    path);
            }
        }
        for (const std::string& path : previousPaths) {
            if (std::find(currentPaths.begin(), currentPaths.end(), path) ==
                currentPaths.end()) {
                dispatchDeviceEvent(registration,
                                    XN_USB_EVENT_DEVICE_DISCONNECT, path);
            }
        }

        previousPaths = currentPaths;
        for (int slice = 0; slice < 5; ++slice) {
            if (InterlockedCompareExchange(&registration->stopWatcher, FALSE,
                                           FALSE) != FALSE) {
                break;
            }
            Sleep(100);
        }
    }
    return 0;
}

DeviceRegistration* createRegistration(
    USHORT vendorId, USHORT productId,
    XnUSBDeviceCallbackFunctionPtr callback, void* cookie,
    XnUSBEventCallbackFunctionPtr legacyCallback, void* legacyCookie) {
    auto* registration = new (std::nothrow) DeviceRegistration();
    if (registration == nullptr) {
        return nullptr;
    }
    std::memset(registration, 0, sizeof(*registration));
    registration->callback = callback;
    registration->legacyCallback = legacyCallback;
    registration->cookie = cookie;
    registration->legacyCookie = legacyCookie;
    registration->vendorId = vendorId;
    registration->productId = productId;
    registration->watcherThread =
        CreateThread(nullptr, 0, deviceWatcherThread, registration, 0, nullptr);
    if (registration->watcherThread == nullptr) {
        delete registration;
        return nullptr;
    }
    return registration;
}

void destroyRegistration(DeviceRegistration* registration) {
    if (registration == nullptr) {
        return;
    }
    InterlockedExchange(&registration->stopWatcher, TRUE);
    WaitForSingleObject(registration->watcherThread, INFINITE);
    CloseHandle(registration->watcherThread);
    delete registration;
}

XnStatus openDevice(const char* path, XN_USB_DEV_HANDLE* result) {
    if (path == nullptr || result == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    *result = nullptr;

    const int wideChars = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
    if (wideChars <= 0) {
        return XN_STATUS_USB_DEVICE_NOT_FOUND;
    }
    std::vector<wchar_t> widePath(static_cast<size_t>(wideChars));
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, widePath.data(),
                            wideChars) <= 0) {
        return XN_STATUS_USB_DEVICE_NOT_FOUND;
    }

    HANDLE file = CreateFileW(widePath.data(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return XN_STATUS_USB_DEVICE_NOT_FOUND;
    }

    WINUSB_INTERFACE_HANDLE usb = nullptr;
    if (!WinUsb_Initialize(file, &usb)) {
        CloseHandle(file);
        return XN_STATUS_USB_DEVICE_NOT_FOUND;
    }

    XN_USB_DEV_HANDLE device = static_cast<XN_USB_DEV_HANDLE>(
        std::calloc(1, sizeof(XnUSBDeviceHandle)));
    if (device == nullptr) {
        WinUsb_Free(usb);
        CloseHandle(file);
        return XN_STATUS_ALLOC_FAILED;
    }

    device->file = file;
    device->usb = usb;
    device->alternateSetting = 0;
    strncpy_s(device->devicePath, path, _TRUNCATE);

    ULONG speed = 0;
    ULONG speedSize = sizeof(speed);
    if (WinUsb_QueryDeviceInformation(usb, DEVICE_SPEED, &speedSize, &speed)) {
        device->speed = speed == LowSpeed ? XN_USB_DEVICE_LOW_SPEED
                         : speed == FullSpeed ? XN_USB_DEVICE_FULL_SPEED
                                              : XN_USB_DEVICE_HIGH_SPEED;
    } else {
        device->speed = XN_USB_DEVICE_HIGH_SPEED;
    }

    *result = device;
    return XN_STATUS_OK;
}

DWORD waitForTransfer(HANDLE file, OVERLAPPED& overlapped, ULONG timeout,
                      ULONG* transferred) {
    const DWORD waitResult = WaitForSingleObject(overlapped.hEvent, timeout);
    if (waitResult == WAIT_TIMEOUT) {
        CancelIoEx(file, &overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        return ERROR_SEM_TIMEOUT;
    }
    if (waitResult != WAIT_OBJECT_0) {
        CancelIoEx(file, &overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        return GetLastError();
    }
    if (!GetOverlappedResult(file, &overlapped, transferred, FALSE)) {
        return mapTransferError(GetLastError());
    }
    return ERROR_SUCCESS;
}

DWORD transferControl(XN_USB_DEV_HANDLE device, WINUSB_SETUP_PACKET& setup,
                      UCHAR* buffer, ULONG length, ULONG timeout,
                      ULONG* transferred) {
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        return GetLastError();
    }

    ULONG immediateBytes = 0;
    const BOOL started = WinUsb_ControlTransfer(
        device->usb, setup, buffer, length, &immediateBytes, &overlapped);
    DWORD error = ERROR_SUCCESS;
    if (!started) {
        error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            error = waitForTransfer(device->file, overlapped, timeout,
                                   transferred);
        }
    } else {
        *transferred = immediateBytes;
    }
    CloseHandle(overlapped.hEvent);
    return error;
}

DWORD transferPipe(XN_USB_EP_HANDLE endpoint, UCHAR* buffer, ULONG length,
                   ULONG timeout, ULONG* transferred, bool read,
                   OVERLAPPED* suppliedOverlapped = nullptr) {
    OVERLAPPED localOverlapped = {};
    OVERLAPPED* overlapped = suppliedOverlapped;
    if (overlapped == nullptr) {
        overlapped = &localOverlapped;
    }
    if (overlapped->hEvent == nullptr) {
        overlapped->hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (overlapped->hEvent == nullptr) {
            return GetLastError();
        }
    }

    ULONG immediateBytes = 0;
    const BOOL started =
        read ? WinUsb_ReadPipe(endpoint->device->usb, endpoint->pipeId, buffer,
                               length, &immediateBytes, overlapped)
             : WinUsb_WritePipe(endpoint->device->usb, endpoint->pipeId,
                                buffer, length, &immediateBytes, overlapped);
    DWORD error = ERROR_SUCCESS;
    if (!started) {
        error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            error = waitForTransfer(endpoint->device->file, *overlapped,
                                    timeout, transferred);
        }
    } else {
        *transferred = immediateBytes;
    }

    if (suppliedOverlapped == nullptr) {
        CloseHandle(overlapped->hEvent);
        overlapped->hEvent = nullptr;
    }
    return error;
}

XnStatus readThreadMain(void* parameter) {
    XN_USB_EP_HANDLE endpoint = static_cast<XN_USB_EP_HANDLE>(parameter);
    std::unique_ptr<UCHAR[]> buffer(
        new (std::nothrow) UCHAR[endpoint->readBufferSize]);
    if (!buffer) {
        return XN_STATUS_ALLOC_FAILED;
    }
    while (InterlockedCompareExchange(&endpoint->stopReadThread, FALSE,
                                      FALSE) == FALSE) {
        XnUInt32 bytesRead = 0;
        const XnStatus status = xnUSBReadEndPoint(
            endpoint, buffer.get(), endpoint->readBufferSize,
            &bytesRead, endpoint->readTimeout);
        if (status == XN_STATUS_USB_TRANSFER_TIMEOUT) {
            continue;
        }
        if (status != XN_STATUS_OK) {
            if (InterlockedCompareExchange(&endpoint->stopReadThread, FALSE,
                                           FALSE) != FALSE) {
                break;
            }
            Sleep(1);
            continue;
        }
        if (endpoint->readCallback != nullptr &&
            !endpoint->readCallback(buffer.get(), bytesRead,
                                    endpoint->readCallbackData)) {
            break;
        }
    }
    return XN_STATUS_OK;
}

} // namespace

XnStatus xnUSBPlatformSpecificInit() {
    return XN_STATUS_OK;
}

XnStatus xnUSBPlatformSpecificShutdown() {
    return XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBEnumerateDevices(
    XnUInt16 vendorId, XnUInt16 productId,
    const XnUSBConnectionString** devicePaths, XnUInt32* count) {
    if (devicePaths == nullptr || count == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    *devicePaths = nullptr;
    *count = 0;

    const std::vector<std::string> paths =
        findDevicePaths(vendorId, productId);
    if (paths.empty()) {
        return XN_STATUS_OK;
    }

    auto* results = static_cast<XnUSBConnectionString*>(
        std::calloc(paths.size(), sizeof(XnUSBConnectionString)));
    if (results == nullptr) {
        return XN_STATUS_ALLOC_FAILED;
    }

    for (size_t index = 0; index < paths.size(); ++index) {
        if (paths[index].size() >= sizeof(XnUSBConnectionString)) {
            std::free(results);
            return XN_STATUS_ERROR;
        }
        std::memcpy(results[index], paths[index].c_str(),
                    paths[index].size() + 1);
    }
    *devicePaths = results;
    *count = static_cast<XnUInt32>(paths.size());
    return XN_STATUS_OK;
}

XN_C_API void XN_C_DECL xnUSBFreeDevicesList(
    const XnUSBConnectionString* devicePaths) {
    std::free(const_cast<XnUSBConnectionString*>(devicePaths));
}

XN_C_API XnStatus XN_C_DECL xnUSBOpenDevice(
    XnUInt16, XnUInt16, void*, void* devicePath,
    XN_USB_DEV_HANDLE* device) {
    return openDevice(static_cast<const char*>(devicePath), device);
}

XN_C_API XnStatus XN_C_DECL xnUSBOpenDeviceByPath(
    const XnUSBConnectionString devicePath, XN_USB_DEV_HANDLE* device) {
    return openDevice(devicePath, device);
}

XN_C_API XnStatus XN_C_DECL xnUSBCloseDevice(XN_USB_DEV_HANDLE device) {
    if (device == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    WinUsb_Free(device->usb);
    CloseHandle(device->file);
    std::free(device);
    return XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBGetDeviceSpeed(
    XN_USB_DEV_HANDLE device, XnUSBDeviceSpeed* speed) {
    if (device == nullptr || speed == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    *speed = device->speed;
    return XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBSetConfig(XN_USB_DEV_HANDLE, XnUInt8) {
    return XN_STATUS_OS_UNSUPPORTED_FUNCTION;
}

XN_C_API XnStatus XN_C_DECL xnUSBGetConfig(XN_USB_DEV_HANDLE, XnUInt8*) {
    return XN_STATUS_OS_UNSUPPORTED_FUNCTION;
}

XN_C_API XnStatus XN_C_DECL xnUSBSetInterface(
    XN_USB_DEV_HANDLE device, XnUInt8, XnUInt8 alternateSetting) {
    if (device == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    if (!WinUsb_SetCurrentAlternateSetting(device->usb, alternateSetting)) {
        return XN_STATUS_USB_SET_INTERFACE_FAILED;
    }
    device->alternateSetting = alternateSetting;
    return XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBGetInterface(
    XN_USB_DEV_HANDLE device, XnUInt8* interfaceNumber,
    XnUInt8* alternateSetting) {
    if (device == nullptr || interfaceNumber == nullptr ||
        alternateSetting == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    UCHAR currentSetting = 0;
    if (!WinUsb_GetCurrentAlternateSetting(device->usb, &currentSetting)) {
        return XN_STATUS_USB_GET_INTERFACE_FAILED;
    }
    *interfaceNumber = 0;
    *alternateSetting = currentSetting;
    return XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBOpenEndPoint(
    XN_USB_DEV_HANDLE device, XnUInt16 endpointId, XnUSBEndPointType type,
    XnUSBDirectionType direction, XN_USB_EP_HANDLE* endpointResult) {
    if (device == nullptr || endpointResult == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    *endpointResult = nullptr;

    USB_INTERFACE_DESCRIPTOR descriptor = {};
    if (!WinUsb_QueryInterfaceSettings(device->usb, 0, &descriptor)) {
        return XN_STATUS_USB_OPEN_ENDPOINT_FAILED;
    }

    bool foundId = false;
    for (UCHAR index = 0; index < descriptor.bNumEndpoints; ++index) {
        WINUSB_PIPE_INFORMATION pipe = {};
        if (!WinUsb_QueryPipe(device->usb, 0, index, &pipe)) {
            return XN_STATUS_USB_OPEN_ENDPOINT_FAILED;
        }
        if (pipe.PipeId != endpointId) {
            continue;
        }
        foundId = true;

        const XnUSBEndPointType actualType =
            pipe.PipeType == UsbdPipeTypeBulk       ? XN_USB_EP_BULK
            : pipe.PipeType == UsbdPipeTypeIsochronous
                ? XN_USB_EP_ISOCHRONOUS
            : pipe.PipeType == UsbdPipeTypeInterrupt ? XN_USB_EP_INTERRUPT
                                                      : XN_USB_EP_BULK;
        if (actualType != type) {
            return XN_STATUS_USB_WRONG_ENDPOINT_TYPE;
        }
        const XnUSBDirectionType actualDirection =
            (pipe.PipeId & 0x80) != 0 ? XN_USB_DIRECTION_IN
                                      : XN_USB_DIRECTION_OUT;
        if (actualDirection != direction) {
            return XN_STATUS_USB_WRONG_ENDPOINT_DIRECTION;
        }

        auto* endpoint = new (std::nothrow) XnUSBEndPointHandle();
        if (endpoint == nullptr) {
            return XN_STATUS_ALLOC_FAILED;
        }
        std::memset(endpoint, 0, sizeof(*endpoint));
        endpoint->device = device;
        endpoint->pipeId = pipe.PipeId;
        endpoint->type = type;
        endpoint->direction = direction;
        endpoint->maxPacketSize = pipe.MaximumPacketSize;
        InterlockedExchange(&endpoint->stopReadThread, FALSE);
        *endpointResult = endpoint;
        return XN_STATUS_OK;
    }
    return foundId ? XN_STATUS_USB_WRONG_ENDPOINT_TYPE
                   : XN_STATUS_USB_ENDPOINT_NOT_FOUND;
}

XN_C_API XnStatus XN_C_DECL xnUSBCloseEndPoint(XN_USB_EP_HANDLE endpoint) {
    if (endpoint == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    if (endpoint->readThread != nullptr) {
        xnUSBShutdownReadThread(endpoint);
    }
    if (endpoint->hasPendingRead) {
        xnUSBAbortEndPoint(endpoint);
        CloseHandle(endpoint->pendingRead.hEvent);
    }
    delete endpoint;
    return XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBGetEndPointMaxPacketSize(
    XN_USB_EP_HANDLE endpoint, XnUInt32* packetSize) {
    if (endpoint == nullptr || packetSize == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    *packetSize = endpoint->maxPacketSize;
    return XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBAbortEndPoint(XN_USB_EP_HANDLE endpoint) {
    if (endpoint == nullptr ||
        !WinUsb_AbortPipe(endpoint->device->usb, endpoint->pipeId)) {
        return XN_STATUS_USB_ABORT_FAILED;
    }
    return XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBFlushEndPoint(XN_USB_EP_HANDLE endpoint) {
    if (endpoint == nullptr ||
        !WinUsb_FlushPipe(endpoint->device->usb, endpoint->pipeId)) {
        return XN_STATUS_USB_RESET_FAILED;
    }
    return XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBResetEndPoint(XN_USB_EP_HANDLE endpoint) {
    if (endpoint == nullptr ||
        !WinUsb_ResetPipe(endpoint->device->usb, endpoint->pipeId)) {
        return XN_STATUS_USB_RESET_FAILED;
    }
    return XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBSendControl(
    XN_USB_DEV_HANDLE device, XnUSBControlType type, XnUInt8 request,
    XnUInt16 value, XnUInt16 index, XnUChar* buffer, XnUInt32 bufferSize,
    XnUInt32 timeout) {
    if (device == nullptr || (buffer == nullptr && bufferSize != 0)) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    WINUSB_SETUP_PACKET setup = {};
    setup.RequestType = static_cast<UCHAR>(type << 5);
    setup.Request = request;
    setup.Value = value;
    setup.Index = index;
    setup.Length = static_cast<USHORT>(bufferSize);
    ULONG transferred = 0;
    const DWORD error = transferControl(device, setup, buffer, bufferSize,
                                        timeout, &transferred);
    if (error == ERROR_SEM_TIMEOUT) {
        return XN_STATUS_USB_TRANSFER_TIMEOUT;
    }
    if (error != ERROR_SUCCESS) {
        return XN_STATUS_USB_CONTROL_SEND_FAILED;
    }
    return transferred == bufferSize ? XN_STATUS_OK
                                     : XN_STATUS_USB_GOT_UNEXPECTED_BYTES;
}

XN_C_API XnStatus XN_C_DECL xnUSBReceiveControl(
    XN_USB_DEV_HANDLE device, XnUSBControlType type, XnUInt8 request,
    XnUInt16 value, XnUInt16 index, XnUChar* buffer, XnUInt32 bufferSize,
    XnUInt32* bytesReceived, XnUInt32 timeout) {
    if (device == nullptr || buffer == nullptr || bytesReceived == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    if (bufferSize == 0 || bufferSize > USHRT_MAX) {
        return XN_STATUS_USB_BUFFER_TOO_SMALL;
    }
    WINUSB_SETUP_PACKET setup = {};
    setup.RequestType = static_cast<UCHAR>(0x80 | (type << 5));
    setup.Request = request;
    setup.Value = value;
    setup.Index = index;
    setup.Length = static_cast<USHORT>(bufferSize);
    ULONG transferred = 0;
    const DWORD error = transferControl(device, setup, buffer, bufferSize,
                                        timeout, &transferred);
    if (error == ERROR_SEM_TIMEOUT) {
        return XN_STATUS_USB_TRANSFER_TIMEOUT;
    }
    if (error != ERROR_SUCCESS) {
        return XN_STATUS_USB_CONTROL_RECV_FAILED;
    }
    *bytesReceived = transferred;
    if (*bytesReceived == 0) {
        return XN_STATUS_USB_NOT_ENOUGH_DATA;
    }
    if (*bytesReceived > bufferSize) {
        return XN_STATUS_USB_TOO_MUCH_DATA;
    }
    return XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBReadEndPoint(
    XN_USB_EP_HANDLE endpoint, XnUChar* buffer, XnUInt32 bufferSize,
    XnUInt32* bytesReceived, XnUInt32 timeout) {
    if (endpoint == nullptr || buffer == nullptr || bytesReceived == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    if (endpoint->direction != XN_USB_DIRECTION_IN) {
        return XN_STATUS_USB_WRONG_ENDPOINT_DIRECTION;
    }
    if (bufferSize == 0) {
        return XN_STATUS_USB_BUFFER_TOO_SMALL;
    }
    ULONG transferred = 0;
    const DWORD error = transferPipe(endpoint, buffer, bufferSize, timeout,
                                     &transferred, true);
    if (error == ERROR_SEM_TIMEOUT) {
        return XN_STATUS_USB_TRANSFER_TIMEOUT;
    }
    if (error != ERROR_SUCCESS) {
        return XN_STATUS_USB_ENDPOINT_READ_FAILED;
    }
    *bytesReceived = transferred;
    return *bytesReceived == 0 ? XN_STATUS_USB_NOT_ENOUGH_DATA : XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBWriteEndPoint(
    XN_USB_EP_HANDLE endpoint, XnUChar* buffer, XnUInt32 bufferSize,
    XnUInt32 timeout) {
    if (endpoint == nullptr || buffer == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    if (endpoint->direction != XN_USB_DIRECTION_OUT) {
        return XN_STATUS_USB_WRONG_ENDPOINT_DIRECTION;
    }
    if (bufferSize == 0) {
        return XN_STATUS_USB_BUFFER_TOO_SMALL;
    }
    ULONG bytesWritten = 0;
    const DWORD error = transferPipe(endpoint, buffer, bufferSize, timeout,
                                     &bytesWritten, false);
    if (error == ERROR_SEM_TIMEOUT) {
        return XN_STATUS_USB_TRANSFER_TIMEOUT;
    }
    if (error != ERROR_SUCCESS) {
        return XN_STATUS_USB_ENDPOINT_WRITE_FAILED;
    }
    return bytesWritten == bufferSize ? XN_STATUS_OK
                                      : XN_STATUS_USB_GOT_UNEXPECTED_BYTES;
}

XN_C_API XnStatus XN_C_DECL xnUSBQueueReadEndPoint(
    XN_USB_EP_HANDLE endpoint, XnUChar* buffer, XnUInt32 bufferSize,
    XnUInt32) {
    if (endpoint == nullptr || buffer == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    if (endpoint->direction != XN_USB_DIRECTION_IN) {
        return XN_STATUS_USB_WRONG_ENDPOINT_DIRECTION;
    }
    if (endpoint->hasPendingRead) {
        return XN_STATUS_USB_OVERLAPIO_FAILED;
    }
    if (bufferSize == 0) {
        return XN_STATUS_USB_BUFFER_TOO_SMALL;
    }
    endpoint->pendingRead = {};
    endpoint->pendingRead.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (endpoint->pendingRead.hEvent == nullptr) {
        return XN_STATUS_USB_OVERLAPIO_FAILED;
    }
    ULONG immediateBytes = 0;
    const BOOL started = WinUsb_ReadPipe(
        endpoint->device->usb, endpoint->pipeId, buffer, bufferSize,
        &immediateBytes, &endpoint->pendingRead);
    if (!started && GetLastError() != ERROR_IO_PENDING) {
        CloseHandle(endpoint->pendingRead.hEvent);
        endpoint->pendingRead = {};
        return XN_STATUS_USB_ENDPOINT_READ_FAILED;
    }
    endpoint->hasPendingRead = true;
    return XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBFinishReadEndPoint(
    XN_USB_EP_HANDLE endpoint, XnUInt32* bytesReceived, XnUInt32 timeout) {
    if (endpoint == nullptr || bytesReceived == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    if (!endpoint->hasPendingRead) {
        return XN_STATUS_USB_OVERLAPIO_FAILED;
    }
    const DWORD result =
        WaitForSingleObject(endpoint->pendingRead.hEvent, timeout);
    if (result == WAIT_TIMEOUT) {
        CancelIoEx(endpoint->device->file, &endpoint->pendingRead);
        WaitForSingleObject(endpoint->pendingRead.hEvent, INFINITE);
        CloseHandle(endpoint->pendingRead.hEvent);
        endpoint->pendingRead = {};
        endpoint->hasPendingRead = false;
        return XN_STATUS_USB_TRANSFER_TIMEOUT;
    }
    ULONG transferred = 0;
    const BOOL complete =
        result == WAIT_OBJECT_0 &&
        GetOverlappedResult(endpoint->device->file, &endpoint->pendingRead,
                            &transferred, FALSE);
    CloseHandle(endpoint->pendingRead.hEvent);
    endpoint->pendingRead = {};
    endpoint->hasPendingRead = false;
    if (!complete) {
        return XN_STATUS_USB_OVERLAPIO_FAILED;
    }
    *bytesReceived = transferred;
    return XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBInitReadThread(
    XN_USB_EP_HANDLE endpoint, XnUInt32 bufferSize, XnUInt32,
    XnUInt32 timeout, XnUSBReadCallbackFunctionPtr callback,
    void* callbackData) {
    if (endpoint == nullptr || callback == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    if (endpoint->readThread != nullptr) {
        return XN_STATUS_USB_READTHREAD_ALREADY_INIT;
    }
    if (endpoint->direction != XN_USB_DIRECTION_IN || bufferSize == 0) {
        return XN_STATUS_USB_WRONG_ENDPOINT_DIRECTION;
    }
    endpoint->readBufferSize = bufferSize;
    endpoint->readTimeout = timeout;
    endpoint->readCallback = callback;
    endpoint->readCallbackData = callbackData;
    InterlockedExchange(&endpoint->stopReadThread, FALSE);
    endpoint->readThread = CreateThread(nullptr, 0, [](LPVOID context) -> DWORD {
        readThreadMain(context);
        return 0;
    }, endpoint, 0, nullptr);
    return endpoint->readThread == nullptr ? XN_STATUS_ERROR : XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBShutdownReadThread(
    XN_USB_EP_HANDLE endpoint) {
    if (endpoint == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    if (endpoint->readThread == nullptr) {
        return XN_STATUS_USB_READTHREAD_NOT_INIT;
    }
    InterlockedExchange(&endpoint->stopReadThread, TRUE);
    WinUsb_AbortPipe(endpoint->device->usb, endpoint->pipeId);
    const DWORD result = WaitForSingleObject(endpoint->readThread, 10000);
    CloseHandle(endpoint->readThread);
    endpoint->readThread = nullptr;
    return result == WAIT_OBJECT_0 ? XN_STATUS_OK
                                   : XN_STATUS_USB_READTHREAD_SHUTDOWN_FAILED;
}

XN_C_API XnStatus XN_C_DECL xnUSBRegisterToConnectivityEvents(
    XnUInt16 vendorId, XnUInt16 productId,
    XnUSBDeviceCallbackFunctionPtr callback, void* cookie,
    XnRegistrationHandle* registration) {
    if (callback == nullptr || registration == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    DeviceRegistration* item = createRegistration(
        vendorId, productId, callback, cookie, nullptr, nullptr);
    if (item == nullptr) {
        return XN_STATUS_ERROR;
    }
    *registration = reinterpret_cast<XnRegistrationHandle>(item);
    return XN_STATUS_OK;
}

XN_C_API void XN_C_DECL xnUSBUnregisterFromConnectivityEvents(
    XnRegistrationHandle registration) {
    auto* item = reinterpret_cast<DeviceRegistration*>(registration);
    if (item == g_legacyRegistration) {
        g_legacyRegistration = nullptr;
    }
    destroyRegistration(item);
}

XN_C_API XnStatus XN_C_DECL xnUSBIsDevicePresent(
    XnUInt16 vendorId, XnUInt16 productId, void*, XnBool* present) {
    if (present == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    *present = findDevicePaths(vendorId, productId).empty() ? FALSE : TRUE;
    return XN_STATUS_OK;
}

XN_C_API XnStatus XN_C_DECL xnUSBSetCallbackHandler(
    XnUInt16 vendorId, XnUInt16 productId, void*,
    XnUSBEventCallbackFunctionPtr callback, void* callbackData) {
    if (callback == nullptr) {
        return XN_STATUS_NULL_INPUT_PTR;
    }
    if (g_legacyRegistration != nullptr) {
        destroyRegistration(g_legacyRegistration);
        g_legacyRegistration = nullptr;
    }
    g_legacyRegistration = createRegistration(
        vendorId, productId, nullptr, nullptr, callback, callbackData);
    if (g_legacyRegistration == nullptr) {
        return XN_STATUS_ERROR;
    }
    return XN_STATUS_OK;
}
