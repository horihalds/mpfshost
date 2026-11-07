#ifndef MPFSHOST_CORE_IPC_MESSAGE_CODEC_HPP
#define MPFSHOST_CORE_IPC_MESSAGE_CODEC_HPP
#pragma once

#include "core/ipc/message.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace mpfshost
{
    namespace detail
    {
        inline void writeTarget(ByteWriter& writer, const TargetDescriptor& target)
        {
            writer.writeString(target.name);
            writer.writeU32(target.qemuPid);
            writer.writeString(target.qmpSocketPath);
        }

        inline bool readTarget(ByteReader& reader, TargetDescriptor& target)
        {
            target.name          = reader.readString();
            target.qemuPid       = reader.readU32();
            target.qmpSocketPath = reader.readString();
            return reader.ok();
        }

        inline void writeProcess(ByteWriter& writer, const ProcessDescriptor& process)
        {
            writer.writeU32(process.pid);
            writer.writeU32(process.parentPid);
            writer.writeU8(process.wow64 ? 1 : 0);
            writer.writeString(process.name);
        }

        inline bool readProcess(ByteReader& reader, ProcessDescriptor& process)
        {
            process.pid       = reader.readU32();
            process.parentPid = reader.readU32();
            process.wow64     = reader.readU8() != 0;
            process.name      = reader.readString();
            return reader.ok();
        }

        // Text that MemProcFS produced is capped host-side too; rejecting anything longer keeps a malformed frame
        // from inflating a struct that the session then renders.
        inline bool readCappedString(ByteReader& reader, std::string& value)
        {
            value = reader.readString();
            return reader.ok() && value.size() <= kMaxMapStringLength;
        }

        inline void writeScatterEntry(ByteWriter& writer, const ScatterEntry& entry)
        {
            writer.writeU64(entry.address);
            writer.writeU32(entry.size);
            writer.writeU8(entry.kind);
            writer.writeBytes(entry.data);
        }

        inline bool readScatterEntry(ByteReader& reader, ScatterEntry& entry)
        {
            entry.address = reader.readU64();
            entry.size    = reader.readU32();
            entry.kind    = reader.readU8();
            entry.data    = reader.readBytes();
            return reader.ok();
        }

        inline void writeScatterOutcome(ByteWriter& writer, const ScatterOutcome& outcome)
        {
            writer.writeU64(outcome.address);
            writer.writeU32(outcome.requestedSize);
            writer.writeU32(outcome.bytesRead);
            writer.writeU8(outcome.ok);
        }

        inline bool readScatterOutcome(ByteReader& reader, ScatterOutcome& outcome)
        {
            outcome.address       = reader.readU64();
            outcome.requestedSize = reader.readU32();
            outcome.bytesRead     = reader.readU32();
            outcome.ok            = reader.readU8();
            return reader.ok();
        }

        inline void writeVfsEntry(ByteWriter& writer, const VfsEntry& entry)
        {
            writer.writeString(entry.name);
            writer.writeU64(entry.size);
            writer.writeU64(entry.creationTime);
            writer.writeU64(entry.lastAccessTime);
            writer.writeU64(entry.lastWriteTime);
            writer.writeU8(entry.isDirectory ? 1 : 0);
        }

        inline bool readVfsEntry(ByteReader& reader, VfsEntry& entry)
        {
            entry.name           = reader.readString();
            entry.size           = reader.readU64();
            entry.creationTime   = reader.readU64();
            entry.lastAccessTime = reader.readU64();
            entry.lastWriteTime  = reader.readU64();
            entry.isDirectory    = reader.readU8() != 0;
            return reader.ok();
        }

        inline void writeModule(ByteWriter& writer, const ModuleDescriptor& module)
        {
            writer.writeU64(module.base);
            writer.writeU64(module.entry);
            writer.writeU32(module.imageSize);
            writer.writeU32(module.rawFileSize);
            writer.writeU16(module.kind);
            writer.writeU16(module.sectionCount);
            writer.writeU32(module.eatCount);
            writer.writeU32(module.iatCount);
            writer.writeU8(module.wow64 ? 1 : 0);
            writer.writeString(module.name);
            writer.writeString(module.fullName);
            writer.writeString(module.pdbGuid);
            writer.writeString(module.pdbName);
            writer.writeString(module.version);
        }

        inline bool readModule(ByteReader& reader, ModuleDescriptor& module)
        {
            module.base         = reader.readU64();
            module.entry        = reader.readU64();
            module.imageSize    = reader.readU32();
            module.rawFileSize  = reader.readU32();
            module.kind         = reader.readU16();
            module.sectionCount = reader.readU16();
            module.eatCount     = reader.readU32();
            module.iatCount     = reader.readU32();
            module.wow64        = reader.readU8() != 0;
            module.name         = reader.readString();
            module.fullName     = reader.readString();
            module.pdbGuid      = reader.readString();
            module.pdbName      = reader.readString();
            module.version      = reader.readString();
            return reader.ok();
        }

        inline void writeRegion(ByteWriter& writer, const MemoryMapRegion& region)
        {
            writer.writeU64(region.start);
            writer.writeU64(region.end);
            writer.writeU32(region.protection);
            writer.writeU32(region.commitCharge);
            writer.writeU16(region.vadType);
            writer.writeU16(region.flags);
            writer.writeU8(region.heapNumber);
            writer.writeString(region.description);
        }

        inline bool readRegion(ByteReader& reader, MemoryMapRegion& region)
        {
            region.start        = reader.readU64();
            region.end          = reader.readU64();
            region.protection   = reader.readU32();
            region.commitCharge = reader.readU32();
            region.vadType      = reader.readU16();
            region.flags        = reader.readU16();
            region.heapNumber   = reader.readU8();
            region.description  = reader.readString();
            return reader.ok();
        }

        inline void writeVadEx(ByteWriter& writer, const VadExDescriptor& entry)
        {
            writer.writeU32(entry.type);
            writer.writeU8(entry.pml);
            writer.writeU8(entry.flags);
            writer.writeU64(entry.va);
            writer.writeU64(entry.pa);
            writer.writeU64(entry.pte);
            writer.writeU32(entry.protoType);
            writer.writeU64(entry.protoPa);
            writer.writeU64(entry.protoPte);
            writer.writeU64(entry.vaVadBase);
        }

        inline bool readVadEx(ByteReader& reader, VadExDescriptor& entry)
        {
            entry.type      = reader.readU32();
            entry.pml       = reader.readU8();
            entry.flags     = reader.readU8();
            entry.va        = reader.readU64();
            entry.pa        = reader.readU64();
            entry.pte       = reader.readU64();
            entry.protoType = reader.readU32();
            entry.protoPa   = reader.readU64();
            entry.protoPte  = reader.readU64();
            entry.vaVadBase = reader.readU64();
            return reader.ok();
        }

        inline void writeHandle(ByteWriter& writer, const HandleDescriptor& handle)
        {
            writer.writeU64(handle.vaObject);
            writer.writeU32(handle.handle);
            writer.writeU32(handle.grantedAccess);
            writer.writeU8(handle.typeIndex);
            writer.writeU64(handle.handleCount);
            writer.writeU64(handle.pointerCount);
            writer.writeU64(handle.objectCreateInfo);
            writer.writeU64(handle.securityDescriptor);
            writer.writeU32(handle.poolTag);
            writer.writeString(handle.text);
            writer.writeString(handle.type);
        }

        inline bool readHandle(ByteReader& reader, HandleDescriptor& handle)
        {
            handle.vaObject           = reader.readU64();
            handle.handle             = reader.readU32();
            handle.grantedAccess      = reader.readU32();
            handle.typeIndex          = reader.readU8();
            handle.handleCount        = reader.readU64();
            handle.pointerCount       = reader.readU64();
            handle.objectCreateInfo   = reader.readU64();
            handle.securityDescriptor = reader.readU64();
            handle.poolTag            = reader.readU32();
            handle.text               = reader.readString();
            handle.type               = reader.readString();
            return reader.ok();
        }

        inline void writeExport(ByteWriter& writer, const ExportDescriptor& entry)
        {
            writer.writeU64(entry.vaFunction);
            writer.writeU32(entry.ordinal);
            writer.writeU32(entry.functionsArrayOffset);
            writer.writeU32(entry.namesArrayOffset);
            writer.writeString(entry.function);
            writer.writeString(entry.forwardedFunction);
        }

        inline bool readExport(ByteReader& reader, ExportDescriptor& entry)
        {
            entry.vaFunction           = reader.readU64();
            entry.ordinal              = reader.readU32();
            entry.functionsArrayOffset = reader.readU32();
            entry.namesArrayOffset     = reader.readU32();
            entry.function             = reader.readString();
            entry.forwardedFunction    = reader.readString();
            return reader.ok();
        }

        inline void writeImport(ByteWriter& writer, const ImportDescriptor& entry)
        {
            writer.writeU64(entry.vaFunction);
            writer.writeU8(entry.is32Bit ? 1 : 0);
            writer.writeU16(entry.hint);
            writer.writeU32(entry.firstThunkRva);
            writer.writeU32(entry.originalFirstThunkRva);
            writer.writeString(entry.function);
            writer.writeString(entry.module);
        }

        inline bool readImport(ByteReader& reader, ImportDescriptor& entry)
        {
            entry.vaFunction            = reader.readU64();
            entry.is32Bit               = reader.readU8() != 0;
            entry.hint                  = reader.readU16();
            entry.firstThunkRva         = reader.readU32();
            entry.originalFirstThunkRva = reader.readU32();
            entry.function              = reader.readString();
            entry.module                = reader.readString();
            return reader.ok();
        }

        inline void writeUnloadedModule(ByteWriter& writer, const UnloadedModuleDescriptor& module)
        {
            writer.writeU64(module.vaBase);
            writer.writeU32(module.imageSize);
            writer.writeU8(module.isWow64 ? 1 : 0);
            writer.writeU32(module.checksum);
            writer.writeU32(module.timeDateStamp);
            writer.writeU64(module.unloadTime);
            writer.writeString(module.text);
        }

        inline bool readUnloadedModule(ByteReader& reader, UnloadedModuleDescriptor& module)
        {
            module.vaBase        = reader.readU64();
            module.imageSize     = reader.readU32();
            module.isWow64       = reader.readU8() != 0;
            module.checksum      = reader.readU32();
            module.timeDateStamp = reader.readU32();
            module.unloadTime    = reader.readU64();
            module.text          = reader.readString();
            return reader.ok();
        }

        inline void writeService(ByteWriter& writer, const ServiceDescriptor& service)
        {
            writer.writeU64(service.objectAddress);
            writer.writeU32(service.ordinal);
            writer.writeU32(service.startType);
            writer.writeU32(service.serviceType);
            writer.writeU32(service.state);
            writer.writeU32(service.controlsAccepted);
            writer.writeU32(service.win32ExitCode);
            writer.writeU32(service.serviceSpecificExitCode);
            writer.writeU32(service.checkPoint);
            writer.writeU32(service.waitHint);
            writer.writeU32(service.pid);
            writer.writeString(service.name);
            writer.writeString(service.displayName);
            writer.writeString(service.imagePath);
            writer.writeString(service.path);
            writer.writeString(service.userType);
            writer.writeString(service.account);
        }

        inline bool readService(ByteReader& reader, ServiceDescriptor& service)
        {
            service.objectAddress           = reader.readU64();
            service.ordinal                 = reader.readU32();
            service.startType               = reader.readU32();
            service.serviceType             = reader.readU32();
            service.state                   = reader.readU32();
            service.controlsAccepted        = reader.readU32();
            service.win32ExitCode           = reader.readU32();
            service.serviceSpecificExitCode = reader.readU32();
            service.checkPoint              = reader.readU32();
            service.waitHint                = reader.readU32();
            service.pid                     = reader.readU32();
            service.name                    = reader.readString();
            service.displayName             = reader.readString();
            service.imagePath               = reader.readString();
            service.path                    = reader.readString();
            service.userType                = reader.readString();
            service.account                 = reader.readString();
            return reader.ok();
        }

        inline void writeUser(ByteWriter& writer, const UserDescriptor& user)
        {
            writer.writeString(user.name);
            writer.writeU64(user.regHive);
            writer.writeString(user.sid);
        }

        inline bool readUser(ByteReader& reader, UserDescriptor& user)
        {
            user.name    = reader.readString();
            user.regHive = reader.readU64();
            user.sid     = reader.readString();
            return reader.ok();
        }

        inline void writeNetworkEndpoint(ByteWriter& writer, const NetworkEndpoint& endpoint)
        {
            writer.writeU8(endpoint.valid ? 1 : 0);
            writer.writeString(endpoint.address);
        }

        inline bool readNetworkEndpoint(ByteReader& reader, NetworkEndpoint& endpoint)
        {
            endpoint.valid   = reader.readU8() != 0;
            endpoint.address = reader.readString();
            return reader.ok();
        }

        inline void writeNetworkEntry(ByteWriter& writer, const NetworkEntryDescriptor& entry)
        {
            writer.writeU32(entry.pid);
            writer.writeString(entry.process);
            writer.writeU32(entry.state);
            writer.writeU16(entry.addressFamily);
            writeNetworkEndpoint(writer, entry.local);
            writeNetworkEndpoint(writer, entry.remote);
            writer.writeU64(entry.objectAddress);
            writer.writeU64(entry.createdTime);
            writer.writeU32(entry.poolTag);
        }

        inline bool readNetworkEntry(ByteReader& reader, NetworkEntryDescriptor& entry)
        {
            entry.pid           = reader.readU32();
            entry.process       = reader.readString();
            entry.state         = reader.readU32();
            entry.addressFamily = reader.readU16();
            if (!readNetworkEndpoint(reader, entry.local) || !readNetworkEndpoint(reader, entry.remote))
            {
                return false;
            }
            entry.objectAddress = reader.readU64();
            entry.createdTime   = reader.readU64();
            entry.poolTag       = reader.readU32();
            return reader.ok();
        }

        inline void writeHeap(ByteWriter& writer, const HeapDescriptor& heap)
        {
            writer.writeU64(heap.address);
            writer.writeU32(heap.index);
            writer.writeU32(heap.number);
            writer.writeU8(heap.type);
            writer.writeU8(heap.is32Bit);
        }

        inline bool readHeap(ByteReader& reader, HeapDescriptor& heap)
        {
            heap.address = reader.readU64();
            heap.index   = reader.readU32();
            heap.number  = reader.readU32();
            heap.type    = reader.readU8();
            heap.is32Bit = reader.readU8();
            return reader.ok();
        }

        inline void writeHeapAlloc(ByteWriter& writer, const HeapAllocDescriptor& allocation)
        {
            writer.writeU64(allocation.address);
            writer.writeU32(allocation.size);
            writer.writeU8(allocation.type);
        }

        inline bool readHeapAlloc(ByteReader& reader, HeapAllocDescriptor& allocation)
        {
            allocation.address = reader.readU64();
            allocation.size    = reader.readU32();
            allocation.type    = reader.readU8();
            return reader.ok();
        }

        // MemProcFS keeps one MajorFunction[28] slot per IRP_MJ_* value; the wire carries a trimmed prefix, so the
        // decoder must not assume the full length and must reject anything longer than the slot count.
        inline constexpr std::uint32_t kKernelDriverMajorFunctionCount = 28;

        inline void writeKernelDriver(ByteWriter& writer, const KernelDriverDescriptor& driver)
        {
            writer.writeU64(driver.va);
            writer.writeU64(driver.vaDriverStart);
            writer.writeU64(driver.cbDriverSize);
            writer.writeU64(driver.vaDeviceObject);
            writer.writeU32(static_cast<std::uint32_t>(driver.majorFunctions.size()));
            for (const auto function : driver.majorFunctions)
            {
                writer.writeU64(function);
            }
            writer.writeString(driver.name);
            writer.writeString(driver.path);
            writer.writeString(driver.serviceKeyName);
        }

        inline bool readKernelDriver(ByteReader& reader, KernelDriverDescriptor& driver)
        {
            driver.va             = reader.readU64();
            driver.vaDriverStart  = reader.readU64();
            driver.cbDriverSize   = reader.readU64();
            driver.vaDeviceObject = reader.readU64();
            const auto count      = reader.readU32();
            if (!reader.ok() || count > kKernelDriverMajorFunctionCount)
            {
                return false;
            }
            driver.majorFunctions.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                driver.majorFunctions.push_back(reader.readU64());
            }
            driver.name           = reader.readString();
            driver.path           = reader.readString();
            driver.serviceKeyName = reader.readString();
            return reader.ok();
        }

        inline void writeKernelDevice(ByteWriter& writer, const KernelDeviceDescriptor& device)
        {
            writer.writeU64(device.va);
            writer.writeU32(device.depth);
            writer.writeU32(device.deviceType);
            writer.writeU64(device.vaDriverObject);
            writer.writeU64(device.vaAttachedDevice);
            writer.writeU64(device.vaFileSystemDevice);
            writer.writeString(device.deviceTypeName);
            writer.writeString(device.volumeInfo);
        }

        inline bool readKernelDevice(ByteReader& reader, KernelDeviceDescriptor& device)
        {
            device.va                 = reader.readU64();
            device.depth              = reader.readU32();
            device.deviceType         = reader.readU32();
            device.vaDriverObject     = reader.readU64();
            device.vaAttachedDevice   = reader.readU64();
            device.vaFileSystemDevice = reader.readU64();
            device.deviceTypeName     = reader.readString();
            device.volumeInfo         = reader.readString();
            return reader.ok();
        }

        inline void writeKernelObject(ByteWriter& writer, const KernelObjectDescriptor& object)
        {
            writer.writeU64(object.va);
            writer.writeU64(object.vaParent);
            writer.writeU64(object.childCount);
            writer.writeU8(object.childrenTruncated ? 1 : 0);
            writer.writeU32(static_cast<std::uint32_t>(object.children.size()));
            for (const auto child : object.children)
            {
                writer.writeU64(child);
            }
            writer.writeString(object.name);
            writer.writeString(object.type);
        }

        inline bool readKernelObject(ByteReader& reader, KernelObjectDescriptor& object)
        {
            object.va                = reader.readU64();
            object.vaParent          = reader.readU64();
            object.childCount        = reader.readU64();
            object.childrenTruncated = reader.readU8() != 0;
            const auto count         = reader.readU32();
            if (!reader.ok() || count > kMaxKernelObjectChildren)
            {
                return false;
            }
            object.children.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                object.children.push_back(reader.readU64());
            }
            object.name = reader.readString();
            object.type = reader.readString();
            return reader.ok();
        }

        inline void writePte(ByteWriter& writer, const PteDescriptor& entry)
        {
            writer.writeU64(entry.vaBase);
            writer.writeU64(entry.pages);
            writer.writeU64(entry.pageFlags);
            writer.writeU32(entry.softwarePages);
            writer.writeU8(entry.wow64);
            writer.writeString(entry.text);
        }

        inline bool readPte(ByteReader& reader, PteDescriptor& entry)
        {
            entry.vaBase        = reader.readU64();
            entry.pages         = reader.readU64();
            entry.pageFlags     = reader.readU64();
            entry.softwarePages = reader.readU32();
            entry.wow64         = reader.readU8();
            entry.text          = reader.readString();
            return reader.ok();
        }

        inline void writePool(ByteWriter& writer, const PoolDescriptor& entry)
        {
            writer.writeU64(entry.va);
            writer.writeU32(entry.bytes);
            writer.writeU32(entry.tag);
            writer.writeU8(entry.alloc);
            writer.writeU8(entry.poolType);
            writer.writeU8(entry.subsegment);
        }

        inline bool readPool(ByteReader& reader, PoolDescriptor& entry)
        {
            entry.va         = reader.readU64();
            entry.bytes      = reader.readU32();
            entry.tag        = reader.readU32();
            entry.alloc      = reader.readU8();
            entry.poolType   = reader.readU8();
            entry.subsegment = reader.readU8();
            return reader.ok();
        }

        inline void writePoolTag(ByteWriter& writer, const PoolTagDescriptor& entry)
        {
            writer.writeU32(entry.tag);
            writer.writeU32(entry.entries);
        }

        inline bool readPoolTag(ByteReader& reader, PoolTagDescriptor& entry)
        {
            entry.tag     = reader.readU32();
            entry.entries = reader.readU32();
            return reader.ok();
        }

        inline void writePfn(ByteWriter& writer, const PfnDescriptor& entry)
        {
            writer.writeU32(entry.pfn);
            writer.writeU8(entry.typeExtended);
            writer.writeU8(entry.pageLocation);
            writer.writeU32(entry.pid);
            writer.writeU64(entry.va);
            writer.writeU64(entry.vaPte);
            writer.writeU64(entry.originalPte);
            writer.writeU32(entry.pteFrame);
            writer.writeU16(entry.referenceCount);
            writer.writeU8(entry.cacheAttribute);
            writer.writeU8(entry.priority);
            writer.writeU8(entry.flags);
            writer.writeU8(entry.pageColor);
        }

        inline bool readPfn(ByteReader& reader, PfnDescriptor& entry)
        {
            entry.pfn            = reader.readU32();
            entry.typeExtended   = reader.readU8();
            entry.pageLocation   = reader.readU8();
            entry.pid            = reader.readU32();
            entry.va             = reader.readU64();
            entry.vaPte          = reader.readU64();
            entry.originalPte    = reader.readU64();
            entry.pteFrame       = reader.readU32();
            entry.referenceCount = reader.readU16();
            entry.cacheAttribute = reader.readU8();
            entry.priority       = reader.readU8();
            entry.flags          = reader.readU8();
            entry.pageColor      = reader.readU8();
            return reader.ok();
        }

        inline void writeThreadFrame(ByteWriter& writer, const ThreadFrame& frame)
        {
            writer.writeU32(frame.index);
            writer.writeU64(frame.returnAddress);
            writer.writeU64(frame.stackPointer);
            writer.writeU64(frame.frameBase);
            writer.writeU32(frame.displacement);
            writer.writeU8(frame.registersPresent ? 1 : 0);
            writer.writeString(frame.module);
            writer.writeString(frame.function);
        }

        inline bool readThreadFrame(ByteReader& reader, ThreadFrame& frame)
        {
            frame.index            = reader.readU32();
            frame.returnAddress    = reader.readU64();
            frame.stackPointer     = reader.readU64();
            frame.frameBase        = reader.readU64();
            frame.displacement     = reader.readU32();
            frame.registersPresent = reader.readU8() != 0;
            frame.module           = reader.readString();
            frame.function         = reader.readString();
            return reader.ok();
        }

        inline void writeThread(ByteWriter& writer, const ThreadDescriptor& thread)
        {
            writer.writeU32(thread.tid);
            writer.writeU32(thread.pid);
            writer.writeU32(thread.exitStatus);
            writer.writeU32(thread.userTime);
            writer.writeU32(thread.kernelTime);
            writer.writeU64(thread.ethread);
            writer.writeU64(thread.teb);
            writer.writeU64(thread.startAddress);
            writer.writeU64(thread.win32StartAddress);
            writer.writeU64(thread.stackBaseUser);
            writer.writeU64(thread.stackLimitUser);
            writer.writeU64(thread.stackBaseKernel);
            writer.writeU64(thread.stackLimitKernel);
            writer.writeU64(thread.trapFrame);
            writer.writeU64(thread.instructionPointer);
            writer.writeU64(thread.stackPointer);
            writer.writeU64(thread.affinity);
            writer.writeU64(thread.createTime);
            writer.writeU64(thread.exitTime);
            writer.writeU64(thread.impersonationToken);
            writer.writeU8(thread.state);
            writer.writeU8(thread.running ? 1 : 0);
            writer.writeU8(thread.priority);
            writer.writeU8(thread.basePriority);
            writer.writeU8(thread.suspendCount);
            writer.writeU8(thread.waitReason);
            writer.writeU8(thread.callstackStatus);
            writer.writeU32(static_cast<std::uint32_t>(thread.frames.size()));
            for (const auto& frame : thread.frames)
            {
                writeThreadFrame(writer, frame);
            }
        }

        inline bool readThread(ByteReader& reader, ThreadDescriptor& thread, std::size_t payloadSize)
        {
            thread.tid                = reader.readU32();
            thread.pid                = reader.readU32();
            thread.exitStatus         = reader.readU32();
            thread.userTime           = reader.readU32();
            thread.kernelTime         = reader.readU32();
            thread.ethread            = reader.readU64();
            thread.teb                = reader.readU64();
            thread.startAddress       = reader.readU64();
            thread.win32StartAddress  = reader.readU64();
            thread.stackBaseUser      = reader.readU64();
            thread.stackLimitUser     = reader.readU64();
            thread.stackBaseKernel    = reader.readU64();
            thread.stackLimitKernel   = reader.readU64();
            thread.trapFrame          = reader.readU64();
            thread.instructionPointer = reader.readU64();
            thread.stackPointer       = reader.readU64();
            thread.affinity           = reader.readU64();
            thread.createTime         = reader.readU64();
            thread.exitTime           = reader.readU64();
            thread.impersonationToken = reader.readU64();
            thread.state              = reader.readU8();
            thread.running            = reader.readU8() != 0;
            thread.priority           = reader.readU8();
            thread.basePriority       = reader.readU8();
            thread.suspendCount       = reader.readU8();
            thread.waitReason         = reader.readU8();
            thread.callstackStatus    = reader.readU8();
            const auto frameCount     = reader.readU32();
            if (!reader.ok() || frameCount > payloadSize)
            {
                return false;
            }
            thread.frames.reserve(frameCount);
            for (std::uint32_t i = 0; i < frameCount; ++i)
            {
                ThreadFrame frame;
                if (!readThreadFrame(reader, frame))
                {
                    return false;
                }
                thread.frames.push_back(std::move(frame));
            }
            return reader.ok();
        }

        inline void writeVm(ByteWriter& writer, const VmDescriptor& vm)
        {
            writer.writeString(vm.name);
            writer.writeU64(vm.gpaMax);
            writer.writeU16(vm.kind);
            writer.writeU16(vm.systemKind);
            writer.writeU32(vm.partitionId);
            writer.writeU32(vm.versionBuild);
            writer.writeU32(vm.vmMemPid);
            writer.writeU8(vm.active ? 1 : 0);
            writer.writeU8(vm.readOnly ? 1 : 0);
            writer.writeU8(vm.physicalOnly ? 1 : 0);
        }

        inline bool readVm(ByteReader& reader, VmDescriptor& vm)
        {
            vm.name         = reader.readString();
            vm.gpaMax       = reader.readU64();
            vm.kind         = reader.readU16();
            vm.systemKind   = reader.readU16();
            vm.partitionId  = reader.readU32();
            vm.versionBuild = reader.readU32();
            vm.vmMemPid     = reader.readU32();
            vm.active       = reader.readU8() != 0;
            vm.readOnly     = reader.readU8() != 0;
            vm.physicalOnly = reader.readU8() != 0;
            return reader.ok();
        }

        inline void writePageRanges(ByteWriter& writer, const std::vector<PageRange>& ranges)
        {
            writer.writeU32(static_cast<std::uint32_t>(ranges.size()));
            for (const auto& range : ranges)
            {
                writer.writeU64(range.address);
                writer.writeU64(range.size);
            }
        }

        inline bool readPageRanges(ByteReader& reader, std::vector<PageRange>& ranges)
        {
            const auto count = reader.readU32();
            if (!reader.ok() || count > kMaxPageCount)
            {
                return false;
            }
            ranges.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                PageRange range;
                range.address = reader.readU64();
                range.size    = reader.readU64();
                if (!reader.ok())
                {
                    return false;
                }
                ranges.push_back(range);
            }
            return true;
        }
    } // namespace detail
} // namespace mpfshost

#endif
