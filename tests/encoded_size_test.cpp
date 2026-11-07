#include <catch2/catch.hpp>

#include "core/ipc/message.hpp"
#include "core/server/map_batching.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    template<typename Entry, typename SizeFunction>
    void checkEncodedSize(const std::vector<std::byte>& payload, const Entry& entry, SizeFunction encodedSize)
    {
        REQUIRE(payload.size() == sizeof(std::uint32_t) + encodedSize(entry));
    }
} // namespace

TEST_CASE("every encoded-size helper matches the wire layout", "[encoded-size]")
{
    using namespace mpfshost;

    VfsEntry vfsEntry;
    vfsEntry.name           = "name";
    vfsEntry.size           = 0x10;
    vfsEntry.creationTime   = 0x20;
    vfsEntry.lastAccessTime = 0x30;
    vfsEntry.lastWriteTime  = 0x40;
    vfsEntry.isDirectory    = true;
    VfsListData vfsData;
    vfsData.entries.push_back(vfsEntry);
    checkEncodedSize(encodeVfsListData(vfsData), vfsEntry, vfsEntryEncodedSize);

    HandleDescriptor handle;
    handle.vaObject           = 0x1000;
    handle.handle             = 1;
    handle.grantedAccess      = 2;
    handle.typeIndex          = 3;
    handle.handleCount        = 4;
    handle.pointerCount       = 5;
    handle.objectCreateInfo   = 6;
    handle.securityDescriptor = 7;
    handle.poolTag            = 8;
    handle.text               = "text";
    handle.type               = "type";
    HandleListData handleData;
    handleData.handles.push_back(handle);
    checkEncodedSize(encodeHandleListData(handleData), handle, handleEncodedSize);

    ExportDescriptor exportEntry;
    exportEntry.vaFunction           = 0x1000;
    exportEntry.ordinal              = 1;
    exportEntry.functionsArrayOffset = 2;
    exportEntry.namesArrayOffset     = 3;
    exportEntry.function             = "function";
    exportEntry.forwardedFunction    = "forwarded";
    ExportListData exportData;
    exportData.exports.push_back(exportEntry);
    checkEncodedSize(encodeExportListData(exportData), exportEntry, exportEncodedSize);

    ImportDescriptor importEntry;
    importEntry.vaFunction            = 0x1000;
    importEntry.is32Bit               = true;
    importEntry.hint                  = 1;
    importEntry.firstThunkRva         = 2;
    importEntry.originalFirstThunkRva = 3;
    importEntry.function              = "function";
    importEntry.module                = "module";
    ImportListData importData;
    importData.imports.push_back(importEntry);
    checkEncodedSize(encodeImportListData(importData), importEntry, importEncodedSize);

    UnloadedModuleDescriptor module;
    module.vaBase        = 0x1000;
    module.imageSize     = 0x2000;
    module.isWow64       = true;
    module.checksum      = 3;
    module.timeDateStamp = 4;
    module.unloadTime    = 5;
    module.text          = "module";
    UnloadedModuleListData moduleData;
    moduleData.modules.push_back(module);
    checkEncodedSize(encodeUnloadedModuleListData(moduleData), module, unloadedModuleEncodedSize);

    ServiceDescriptor service;
    service.objectAddress           = 0x1000;
    service.ordinal                 = 1;
    service.startType               = 2;
    service.serviceType             = 3;
    service.state                   = 4;
    service.controlsAccepted        = 5;
    service.win32ExitCode           = 6;
    service.serviceSpecificExitCode = 7;
    service.checkPoint              = 8;
    service.waitHint                = 9;
    service.pid                     = 10;
    service.name                    = "name";
    service.displayName             = "display";
    service.imagePath               = "image";
    service.path                    = "path";
    service.userType                = "user";
    service.account                 = "account";
    ServiceListData serviceData;
    serviceData.services.push_back(service);
    checkEncodedSize(encodeServiceListData(serviceData), service, serviceEncodedSize);

    UserDescriptor user;
    user.name    = "name";
    user.regHive = 0x1000;
    user.sid     = "S-1-0-0";
    UserListData userData;
    userData.users.push_back(user);
    checkEncodedSize(encodeUserListData(userData), user, userEncodedSize);

    NetworkEntryDescriptor networkEntry;
    networkEntry.pid            = 1;
    networkEntry.process        = "process";
    networkEntry.state          = 2;
    networkEntry.addressFamily  = 3;
    networkEntry.local.valid    = true;
    networkEntry.local.address  = "127.0.0.1:80";
    networkEntry.remote.valid   = true;
    networkEntry.remote.address = "127.0.0.1:81";
    networkEntry.objectAddress  = 0x1000;
    networkEntry.createdTime    = 0x2000;
    networkEntry.poolTag        = 4;
    NetworkListData networkData;
    networkData.entries.push_back(networkEntry);
    checkEncodedSize(encodeNetworkListData(networkData), networkEntry, networkEntryEncodedSize);

    HeapDescriptor heap;
    heap.address = 0x1000;
    heap.index   = 1;
    heap.number  = 2;
    heap.type    = 3;
    heap.is32Bit = true;
    HeapListData heapData;
    heapData.heaps.push_back(heap);
    checkEncodedSize(encodeHeapListData(heapData), heap, heapEncodedSize);

    HeapAllocDescriptor allocation;
    allocation.address = 0x1000;
    allocation.size    = 0x20;
    allocation.type    = 1;
    HeapAllocListData allocationData;
    allocationData.allocations.push_back(allocation);
    checkEncodedSize(encodeHeapAllocListData(allocationData), allocation, heapAllocEncodedSize);

    KernelDriverDescriptor driver;
    driver.va             = 0x1000;
    driver.vaDriverStart  = 0x2000;
    driver.cbDriverSize   = 0x3000;
    driver.vaDeviceObject = 0x4000;
    driver.majorFunctions = {1, 2};
    driver.name           = "name";
    driver.path           = "path";
    driver.serviceKeyName = "service";
    KDriverListData driverData;
    driverData.drivers.push_back(driver);
    checkEncodedSize(encodeKDriverListData(driverData), driver, kernelDriverEncodedSize);

    KernelDeviceDescriptor device;
    device.va                 = 0x1000;
    device.depth              = 1;
    device.deviceType         = 2;
    device.vaDriverObject     = 0x2000;
    device.vaAttachedDevice   = 0x3000;
    device.vaFileSystemDevice = 0x4000;
    device.deviceTypeName     = "type";
    device.volumeInfo         = "volume";
    KDeviceListData deviceData;
    deviceData.devices.push_back(device);
    checkEncodedSize(encodeKDeviceListData(deviceData), device, kernelDeviceEncodedSize);

    KernelObjectDescriptor object;
    object.va                = 0x1000;
    object.vaParent          = 0x2000;
    object.childCount        = 3;
    object.children          = {1, 2};
    object.childrenTruncated = true;
    object.name              = "name";
    object.type              = "type";
    KObjectListData objectData;
    objectData.objects.push_back(object);
    checkEncodedSize(encodeKObjectListData(objectData), object, kernelObjectEncodedSize);

    PteDescriptor pte;
    pte.vaBase        = 0x1000;
    pte.pages         = 1;
    pte.pageFlags     = 2;
    pte.softwarePages = 3;
    pte.wow64         = true;
    pte.text          = "text";
    PteListData pteData;
    pteData.entries.push_back(pte);
    checkEncodedSize(encodePteListData(pteData), pte, pteEncodedSize);

    VadExDescriptor vadex;
    vadex.type      = 1;
    vadex.pml       = 4;
    vadex.flags     = 0x11;
    vadex.va        = 0x1000;
    vadex.pa        = 0x2000;
    vadex.pte       = 0x3000;
    vadex.protoType = 2;
    vadex.protoPa   = 0x4000;
    vadex.protoPte  = 0x5000;
    vadex.vaVadBase = 0x6000;
    VadExListData vadexData;
    vadexData.entries.push_back(vadex);
    checkEncodedSize(encodeVadExListData(vadexData), vadex, vadExEncodedSize);

    PoolDescriptor pool;
    pool.va         = 0x1000;
    pool.bytes      = 0x20;
    pool.tag        = 0x40;
    pool.alloc      = true;
    pool.poolType   = 1;
    pool.subsegment = 2;
    PoolListData poolData;
    poolData.entries.push_back(pool);
    checkEncodedSize(encodePoolListData(poolData), pool, poolEncodedSize);

    PoolTagDescriptor poolTag;
    poolTag.tag     = 0x40;
    poolTag.entries = 3;
    PoolTagListData poolTagData;
    poolTagData.tags.push_back(poolTag);
    checkEncodedSize(encodePoolTagListData(poolTagData), poolTag, poolTagEncodedSize);

    PfnDescriptor pfn;
    pfn.pfn            = 1;
    pfn.typeExtended   = 2;
    pfn.pageLocation   = 3;
    pfn.pid            = 4;
    pfn.va             = 0x1000;
    pfn.vaPte          = 0x2000;
    pfn.originalPte    = 0x3000;
    pfn.pteFrame       = 5;
    pfn.referenceCount = 6;
    pfn.cacheAttribute = 7;
    pfn.priority       = 8;
    pfn.flags          = 9;
    pfn.pageColor      = 10;
    PfnListData pfnData;
    pfnData.entries.push_back(pfn);
    checkEncodedSize(encodePfnListData(pfnData), pfn, pfnEncodedSize);
}
