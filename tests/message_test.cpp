#include <catch2/catch.hpp>

#include "core/ipc/message.hpp"

#include <cstddef>
#include <string>
#include <vector>

TEST_CASE("process list request round trips", "[ipc][message]")
{
    const mpfshost::ProcessFilter filter {1234, "explorer"};

    const auto encoded = mpfshost::encodeProcessListRequest(filter);
    const auto decoded = mpfshost::decodeProcessListRequest(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->pid == 1234);
    REQUIRE(decoded->nameFilter == "explorer");
}

TEST_CASE("process list response round trips", "[ipc][message]")
{
    const mpfshost::ProcessListResponse empty;
    const auto emptyDecoded = mpfshost::decodeProcessListResponse(mpfshost::encodeProcessListResponse(empty));
    REQUIRE(emptyDecoded.has_value());
    REQUIRE(emptyDecoded->processes.empty());

    mpfshost::ProcessListResponse single;
    single.processes.push_back({4, 0, "System", false});
    const auto singleDecoded = mpfshost::decodeProcessListResponse(mpfshost::encodeProcessListResponse(single));
    REQUIRE(singleDecoded.has_value());
    REQUIRE(singleDecoded->processes.size() == 1);
    REQUIRE(singleDecoded->processes[0].pid == 4);
    REQUIRE(singleDecoded->processes[0].parentPid == 0);
    REQUIRE(singleDecoded->processes[0].name == "System");
    REQUIRE_FALSE(singleDecoded->processes[0].wow64);

    mpfshost::ProcessListResponse many;
    many.processes.push_back({100, 4, "explorer.exe", false});
    many.processes.push_back({200, 100, "notepad.exe", true});
    many.processes.push_back({300, 0, "smss.exe", false});
    const auto manyDecoded = mpfshost::decodeProcessListResponse(mpfshost::encodeProcessListResponse(many));
    REQUIRE(manyDecoded.has_value());
    REQUIRE(manyDecoded->processes.size() == 3);
    REQUIRE(manyDecoded->processes[0].name == "explorer.exe");
    REQUIRE(manyDecoded->processes[1].pid == 200);
    REQUIRE(manyDecoded->processes[1].parentPid == 100);
    REQUIRE(manyDecoded->processes[1].wow64);
    REQUIRE(manyDecoded->processes[2].name == "smss.exe");
}

TEST_CASE("process list all response round trips", "[ipc][message]")
{
    const mpfshost::ProcessListAllResponse empty;
    const auto emptyDecoded = mpfshost::decodeProcessListAllResponse(mpfshost::encodeProcessListAllResponse(empty));
    REQUIRE(emptyDecoded.has_value());
    REQUIRE(emptyDecoded->processes.empty());

    mpfshost::ProcessListAllResponse response;
    response.processes.push_back({
        {4, 0, "System", false},
        2
    });
    response.processes.push_back({
        {100, 4, "gone.exe", true},
        0
    });
    const auto decoded = mpfshost::decodeProcessListAllResponse(mpfshost::encodeProcessListAllResponse(response));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->processes.size() == 2);
    REQUIRE(decoded->processes[0].process.name == "System");
    REQUIRE(decoded->processes[0].state == 2);
    REQUIRE(decoded->processes[1].process.pid == 100);
    REQUIRE(decoded->processes[1].process.wow64);
    REQUIRE(decoded->processes[1].state == 0);
}

TEST_CASE("malformed process payloads are rejected", "[ipc][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodeProcessListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeProcessListResponse(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeProcessListAllResponse(empty).has_value());

    mpfshost::ProcessListAllResponse trailing;
    trailing.processes.push_back({
        {1, 0, "alpha", false},
        1
    });
    auto trailingBytes = mpfshost::encodeProcessListAllResponse(trailing);
    trailingBytes.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeProcessListAllResponse(trailingBytes).has_value());

    mpfshost::ByteWriter absurdAllCount;
    absurdAllCount.writeU32(0xFFFFFFFFu);
    REQUIRE_FALSE(mpfshost::decodeProcessListAllResponse(absurdAllCount.bytes()).has_value());

    mpfshost::ByteWriter truncatedRequest;
    truncatedRequest.writeU32(1);
    truncatedRequest.writeU32(64);
    REQUIRE_FALSE(mpfshost::decodeProcessListRequest(truncatedRequest.bytes()).has_value());

    mpfshost::ByteWriter truncatedEntry;
    truncatedEntry.writeU32(1);
    truncatedEntry.writeU32(10);
    REQUIRE_FALSE(mpfshost::decodeProcessListResponse(truncatedEntry.bytes()).has_value());

    mpfshost::ByteWriter trailingData;
    trailingData.writeU32(0);
    trailingData.writeU8(1);
    REQUIRE_FALSE(mpfshost::decodeProcessListRequest(trailingData.bytes()).has_value());

    mpfshost::ByteWriter absurdCount;
    absurdCount.writeU32(0xFFFFFFFFu);
    REQUIRE_FALSE(mpfshost::decodeProcessListResponse(absurdCount.bytes()).has_value());

    mpfshost::ProcessListResponse response;
    response.processes.push_back({1, 0, "alpha", false});
    auto bytes = mpfshost::encodeProcessListResponse(response);
    bytes.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeProcessListResponse(bytes).has_value());
}

TEST_CASE("memory read request round trips", "[ipc][message]")
{
    const mpfshost::MemoryReadRequest request {4321, 0x7FF700000000ull, 0x1234, mpfshost::kMemoryReadFlagPartial};
    const auto decoded = mpfshost::decodeMemoryReadRequest(mpfshost::encodeMemoryReadRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->pid == 4321);
    REQUIRE(decoded->address == 0x7FF700000000ull);
    REQUIRE(decoded->size == 0x1234);
    REQUIRE(decoded->flags == mpfshost::kMemoryReadFlagPartial);
}

TEST_CASE("memory read end round trips", "[ipc][message]")
{
    const mpfshost::MemoryReadEnd ok {0, 4096, ""};
    const auto                    okDecoded = mpfshost::decodeMemoryReadEnd(mpfshost::encodeMemoryReadEnd(ok));
    REQUIRE(okDecoded.has_value());
    REQUIRE(okDecoded->status == 0);
    REQUIRE(okDecoded->bytesDelivered == 4096);
    REQUIRE(okDecoded->error.empty());

    const mpfshost::MemoryReadEnd failed {1, 2048, "partial read"};
    const auto                    failedDecoded = mpfshost::decodeMemoryReadEnd(mpfshost::encodeMemoryReadEnd(failed));
    REQUIRE(failedDecoded.has_value());
    REQUIRE(failedDecoded->status == 1);
    REQUIRE(failedDecoded->bytesDelivered == 2048);
    REQUIRE(failedDecoded->error == "partial read");
}

TEST_CASE("memory write messages round trip", "[ipc][message]")
{
    const mpfshost::MemoryWriteRequest request {
        99, 0x1000, {std::byte {0xDE}, std::byte {0xAD}, std::byte {0xBE}, std::byte {0xEF}}
    };
    const auto decoded = mpfshost::decodeMemoryWriteRequest(mpfshost::encodeMemoryWriteRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->pid == 99);
    REQUIRE(decoded->address == 0x1000);
    REQUIRE(decoded->data.size() == 4);
    REQUIRE(decoded->data[0] == std::byte {0xDE});
    REQUIRE(decoded->data[3] == std::byte {0xEF});

    const mpfshost::MemoryWriteResponse response {4};
    const auto responseDecoded = mpfshost::decodeMemoryWriteResponse(mpfshost::encodeMemoryWriteResponse(response));
    REQUIRE(responseDecoded.has_value());
    REQUIRE(responseDecoded->bytesWritten == 4);
}

TEST_CASE("scatter create and close messages round trip", "[ipc][message]")
{
    const mpfshost::ScatterCreateRequest createRequest {17};
    const auto                           createDecoded =
        mpfshost::decodeScatterCreateRequest(mpfshost::encodeScatterCreateRequest(createRequest));
    REQUIRE(createDecoded.has_value());
    REQUIRE(createDecoded->pid == 17);

    const mpfshost::ScatterCreateResponse createResponse {3};
    const auto                            createResponseDecoded =
        mpfshost::decodeScatterCreateResponse(mpfshost::encodeScatterCreateResponse(createResponse));
    REQUIRE(createResponseDecoded.has_value());
    REQUIRE(createResponseDecoded->scatterId == 3);

    const mpfshost::ScatterCloseRequest closeRequest {3};
    const auto closeDecoded = mpfshost::decodeScatterCloseRequest(mpfshost::encodeScatterCloseRequest(closeRequest));
    REQUIRE(closeDecoded.has_value());
    REQUIRE(closeDecoded->scatterId == 3);

    const mpfshost::ScatterCloseResponse closeResponse {3};
    const auto                           closeResponseDecoded =
        mpfshost::decodeScatterCloseResponse(mpfshost::encodeScatterCloseResponse(closeResponse));
    REQUIRE(closeResponseDecoded.has_value());
    REQUIRE(closeResponseDecoded->scatterId == 3);
}

TEST_CASE("cancel messages round trip", "[ipc][message]")
{
    const mpfshost::CancelRequest request {0x1122334455667788};
    const auto requestDecoded = mpfshost::decodeCancelRequest(mpfshost::encodeCancelRequest(request));
    REQUIRE(requestDecoded.has_value());
    REQUIRE(requestDecoded->requestId == 0x1122334455667788);

    const mpfshost::CancelResponse response {1, 42};
    const auto responseDecoded = mpfshost::decodeCancelResponse(mpfshost::encodeCancelResponse(response));
    REQUIRE(responseDecoded.has_value());
    REQUIRE(responseDecoded->cancelled == 1);
    REQUIRE(responseDecoded->requestId == 42);

    const mpfshost::CancelResponse declined {0, 42};
    const auto declinedDecoded = mpfshost::decodeCancelResponse(mpfshost::encodeCancelResponse(declined));
    REQUIRE(declinedDecoded.has_value());
    REQUIRE(declinedDecoded->cancelled == 0);
}

TEST_CASE("scatter prepare round trips read and write entries", "[ipc][message]")
{
    const mpfshost::ScatterPrepareRequest empty {5, {}};
    const auto emptyDecoded = mpfshost::decodeScatterPrepareRequest(mpfshost::encodeScatterPrepareRequest(empty));
    REQUIRE(emptyDecoded.has_value());
    REQUIRE(emptyDecoded->scatterId == 5);
    REQUIRE(emptyDecoded->entries.empty());

    mpfshost::ScatterPrepareRequest request {7, {}};
    request.entries.push_back({0x1000, 16, mpfshost::kScatterEntryRead, {}});
    request.entries.push_back({
        0x2000, 4, mpfshost::kScatterEntryWrite, {std::byte {1}, std::byte {2}, std::byte {3}, std::byte {4}}
    });

    const auto decoded = mpfshost::decodeScatterPrepareRequest(mpfshost::encodeScatterPrepareRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->scatterId == 7);
    REQUIRE(decoded->entries.size() == 2);
    REQUIRE(decoded->entries[0].address == 0x1000);
    REQUIRE(decoded->entries[0].size == 16);
    REQUIRE(decoded->entries[0].kind == mpfshost::kScatterEntryRead);
    REQUIRE(decoded->entries[0].data.empty());
    REQUIRE(decoded->entries[1].address == 0x2000);
    REQUIRE(decoded->entries[1].kind == mpfshost::kScatterEntryWrite);
    REQUIRE(decoded->entries[1].data.size() == 4);
    REQUIRE(decoded->entries[1].data[2] == std::byte {3});

    const mpfshost::ScatterPrepareResponse response {2};
    const auto                             responseDecoded =
        mpfshost::decodeScatterPrepareResponse(mpfshost::encodeScatterPrepareResponse(response));
    REQUIRE(responseDecoded.has_value());
    REQUIRE(responseDecoded->entryCount == 2);
}

TEST_CASE("scatter execute messages round trip", "[ipc][message]")
{
    const mpfshost::ScatterExecuteRequest request {9};
    const auto requestDecoded = mpfshost::decodeScatterExecuteRequest(mpfshost::encodeScatterExecuteRequest(request));
    REQUIRE(requestDecoded.has_value());
    REQUIRE(requestDecoded->scatterId == 9);

    const mpfshost::ScatterExecuteData data {
        2, {std::byte {0xAA}, std::byte {0xBB}}
    };
    const auto dataDecoded = mpfshost::decodeScatterExecuteData(mpfshost::encodeScatterExecuteData(data));
    REQUIRE(dataDecoded.has_value());
    REQUIRE(dataDecoded->entryIndex == 2);
    REQUIRE(dataDecoded->data.size() == 2);
    REQUIRE(dataDecoded->data[1] == std::byte {0xBB});

    mpfshost::ScatterExecuteEnd end {0, {}, ""};
    end.outcomes.push_back({0x1000, 16, 16, 1});
    end.outcomes.push_back({0x2000, 8, 0, 0});
    const auto endDecoded = mpfshost::decodeScatterExecuteEnd(mpfshost::encodeScatterExecuteEnd(end));
    REQUIRE(endDecoded.has_value());
    REQUIRE(endDecoded->status == 0);
    REQUIRE(endDecoded->outcomes.size() == 2);
    REQUIRE(endDecoded->outcomes[0].address == 0x1000);
    REQUIRE(endDecoded->outcomes[0].requestedSize == 16);
    REQUIRE(endDecoded->outcomes[0].bytesRead == 16);
    REQUIRE(endDecoded->outcomes[0].ok == 1);
    REQUIRE(endDecoded->outcomes[1].ok == 0);
    REQUIRE(endDecoded->error.empty());

    const mpfshost::ScatterExecuteEnd failed {1, {}, "execute failed"};
    const auto failedDecoded = mpfshost::decodeScatterExecuteEnd(mpfshost::encodeScatterExecuteEnd(failed));
    REQUIRE(failedDecoded.has_value());
    REQUIRE(failedDecoded->status == 1);
    REQUIRE(failedDecoded->outcomes.empty());
    REQUIRE(failedDecoded->error == "execute failed");
}

TEST_CASE("scatter execute read and clear messages round trip", "[ipc][message]")
{
    const mpfshost::ScatterExecuteReadRequest request {4};
    const auto                                requestDecoded =
        mpfshost::decodeScatterExecuteReadRequest(mpfshost::encodeScatterExecuteReadRequest(request));
    REQUIRE(requestDecoded.has_value());
    REQUIRE(requestDecoded->scatterId == 4);

    const mpfshost::ScatterExecuteReadData data {
        1, {std::byte {0x11}, std::byte {0x22}}
    };
    const auto dataDecoded = mpfshost::decodeScatterExecuteReadData(mpfshost::encodeScatterExecuteReadData(data));
    REQUIRE(dataDecoded.has_value());
    REQUIRE(dataDecoded->entryIndex == 1);
    REQUIRE(dataDecoded->data.size() == 2);
    REQUIRE(dataDecoded->data[0] == std::byte {0x11});

    mpfshost::ScatterExecuteReadEnd end {0, {}, ""};
    end.outcomes.push_back({0x3000, 4, 4, 1});
    const auto endDecoded = mpfshost::decodeScatterExecuteReadEnd(mpfshost::encodeScatterExecuteReadEnd(end));
    REQUIRE(endDecoded.has_value());
    REQUIRE(endDecoded->status == 0);
    REQUIRE(endDecoded->outcomes.size() == 1);
    REQUIRE(endDecoded->outcomes[0].address == 0x3000);
    REQUIRE(endDecoded->outcomes[0].ok == 1);

    const mpfshost::ScatterClearRequest clearRequest {4};
    const auto                          clearRequestDecoded =
        mpfshost::decodeScatterClearRequest(mpfshost::encodeScatterClearRequest(clearRequest));
    REQUIRE(clearRequestDecoded.has_value());
    REQUIRE(clearRequestDecoded->scatterId == 4);

    const mpfshost::ScatterClearResponse clearResponse {4};
    const auto                           clearResponseDecoded =
        mpfshost::decodeScatterClearResponse(mpfshost::encodeScatterClearResponse(clearResponse));
    REQUIRE(clearResponseDecoded.has_value());
    REQUIRE(clearResponseDecoded->scatterId == 4);
}

TEST_CASE("malformed memory and scatter payloads are rejected", "[ipc][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodeMemoryReadRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeMemoryReadEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeMemoryWriteRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeScatterPrepareRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeScatterExecuteData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeScatterExecuteEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeScatterExecuteReadRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeScatterExecuteReadData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeScatterExecuteReadEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeScatterClearRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeScatterClearResponse(empty).has_value());

    mpfshost::ByteWriter truncatedRead;
    truncatedRead.writeU32(10);
    truncatedRead.writeU64(0x2000);
    REQUIRE_FALSE(mpfshost::decodeMemoryReadRequest(truncatedRead.bytes()).has_value());

    mpfshost::ByteWriter trailingRead;
    trailingRead.writeU32(10);
    trailingRead.writeU64(0x2000);
    trailingRead.writeU32(16);
    trailingRead.writeU8(mpfshost::kMemoryReadFlagPartial);
    trailingRead.writeU8(0);
    REQUIRE_FALSE(mpfshost::decodeMemoryReadRequest(trailingRead.bytes()).has_value());

    mpfshost::ByteWriter unknownReadFlags;
    unknownReadFlags.writeU32(10);
    unknownReadFlags.writeU64(0x2000);
    unknownReadFlags.writeU32(16);
    unknownReadFlags.writeU8(0x80);
    REQUIRE_FALSE(mpfshost::decodeMemoryReadRequest(unknownReadFlags.bytes()).has_value());

    mpfshost::ByteWriter impossibleEntryCount;
    impossibleEntryCount.writeU32(1);
    impossibleEntryCount.writeU32(0xFFFFFFFFu);
    REQUIRE_FALSE(mpfshost::decodeScatterPrepareRequest(impossibleEntryCount.bytes()).has_value());

    mpfshost::ByteWriter impossibleOutcomeCount;
    impossibleOutcomeCount.writeU8(0);
    impossibleOutcomeCount.writeU32(0xFFFFFFFFu);
    REQUIRE_FALSE(mpfshost::decodeScatterExecuteEnd(impossibleOutcomeCount.bytes()).has_value());

    auto writeBytes = mpfshost::encodeMemoryWriteRequest({1, 0x3000, {std::byte {1}}});
    writeBytes.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeMemoryWriteRequest(writeBytes).has_value());

    REQUIRE_FALSE(mpfshost::decodeCancelRequest(empty).has_value());

    mpfshost::ByteWriter truncatedCancel;
    truncatedCancel.writeU32(1);
    REQUIRE_FALSE(mpfshost::decodeCancelRequest(truncatedCancel.bytes()).has_value());

    mpfshost::ByteWriter truncatedCancelResponse;
    truncatedCancelResponse.writeU8(1);
    truncatedCancelResponse.writeU32(1);
    REQUIRE_FALSE(mpfshost::decodeCancelResponse(truncatedCancelResponse.bytes()).has_value());

    auto trailingCancelResponse = mpfshost::encodeCancelResponse({1, 7});
    trailingCancelResponse.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeCancelResponse(trailingCancelResponse).has_value());
}

TEST_CASE("module list request round trips", "[ipc][message]")
{
    const mpfshost::ModuleListRequest request {1234, mpfshost::kModuleFlagCounts, "kernel"};
    const auto decoded = mpfshost::decodeModuleListRequest(mpfshost::encodeModuleListRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->pid == 1234);
    REQUIRE(decoded->flags == mpfshost::kModuleFlagCounts);
    REQUIRE(decoded->nameFilter == "kernel");

    const mpfshost::ModuleListRequest allFlags {1,
                                                static_cast<std::uint16_t>(mpfshost::kModuleFlagCounts
                                                                           | mpfshost::kModuleFlagDebugInfo
                                                                           | mpfshost::kModuleFlagVersionInfo),
                                                ""};
    const auto allFlagsDecoded = mpfshost::decodeModuleListRequest(mpfshost::encodeModuleListRequest(allFlags));
    REQUIRE(allFlagsDecoded.has_value());
    REQUIRE(allFlagsDecoded->flags
            == (mpfshost::kModuleFlagCounts | mpfshost::kModuleFlagDebugInfo | mpfshost::kModuleFlagVersionInfo));
    REQUIRE(allFlagsDecoded->nameFilter.empty());
}

TEST_CASE("module list data round trips", "[ipc][message]")
{
    const mpfshost::ModuleListData empty;
    const auto                     emptyDecoded = mpfshost::decodeModuleListData(mpfshost::encodeModuleListData(empty));
    REQUIRE(emptyDecoded.has_value());
    REQUIRE(emptyDecoded->modules.empty());

    mpfshost::ModuleListData data;
    data.modules.push_back({0x7FF700000000ull,
                            0x7FF700001000ull,
                            0x20000,
                            0,
                            1,
                            0,
                            0,
                            0,
                            false,
                            "ntdll.dll",
                            "C:\\Windows\\System32\\ntdll.dll",
                            "",
                            "",
                            ""});
    data.modules.push_back({0x401000ull,
                            0x401500ull,
                            0x10000,
                            0x18000,
                            0,
                            8,
                            12,
                            34,
                            true,
                            "probe.exe",
                            "C:\\probe.exe",
                            "{DEADBEEF-0000-1111-2222-333344445555}",
                            "probe.pdb",
                            "1.2.3.4"});
    const auto decoded = mpfshost::decodeModuleListData(mpfshost::encodeModuleListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->modules.size() == 2);
    REQUIRE(decoded->modules[0].base == 0x7FF700000000ull);
    REQUIRE(decoded->modules[0].entry == 0x7FF700001000ull);
    REQUIRE(decoded->modules[0].imageSize == 0x20000);
    REQUIRE(decoded->modules[0].kind == 1);
    REQUIRE_FALSE(decoded->modules[0].wow64);
    REQUIRE(decoded->modules[0].name == "ntdll.dll");
    REQUIRE(decoded->modules[0].fullName == "C:\\Windows\\System32\\ntdll.dll");
    REQUIRE(decoded->modules[0].pdbGuid.empty());
    REQUIRE(decoded->modules[1].rawFileSize == 0x18000);
    REQUIRE(decoded->modules[1].sectionCount == 8);
    REQUIRE(decoded->modules[1].eatCount == 12);
    REQUIRE(decoded->modules[1].iatCount == 34);
    REQUIRE(decoded->modules[1].wow64);
    REQUIRE(decoded->modules[1].pdbGuid == "{DEADBEEF-0000-1111-2222-333344445555}");
    REQUIRE(decoded->modules[1].pdbName == "probe.pdb");
    REQUIRE(decoded->modules[1].version == "1.2.3.4");
}

TEST_CASE("module list end round trips", "[ipc][message]")
{
    const mpfshost::ModuleListEnd ok {0, 42, ""};
    const auto                    okDecoded = mpfshost::decodeModuleListEnd(mpfshost::encodeModuleListEnd(ok));
    REQUIRE(okDecoded.has_value());
    REQUIRE(okDecoded->status == 0);
    REQUIRE(okDecoded->moduleCount == 42);
    REQUIRE(okDecoded->error.empty());

    const mpfshost::ModuleListEnd failed {1, 3, "map failed"};
    const auto                    failedDecoded = mpfshost::decodeModuleListEnd(mpfshost::encodeModuleListEnd(failed));
    REQUIRE(failedDecoded.has_value());
    REQUIRE(failedDecoded->status == 1);
    REQUIRE(failedDecoded->moduleCount == 3);
    REQUIRE(failedDecoded->error == "map failed");
}

TEST_CASE("memory map messages round trip", "[ipc][message]")
{
    const mpfshost::MemoryMapRequest request {4321, true};
    const auto requestDecoded = mpfshost::decodeMemoryMapRequest(mpfshost::encodeMemoryMapRequest(request));
    REQUIRE(requestDecoded.has_value());
    REQUIRE(requestDecoded->pid == 4321);
    REQUIRE(requestDecoded->identifyModules);

    const mpfshost::MemoryMapData empty;
    const auto                    emptyDecoded = mpfshost::decodeMemoryMapData(mpfshost::encodeMemoryMapData(empty));
    REQUIRE(emptyDecoded.has_value());
    REQUIRE(emptyDecoded->regions.empty());

    mpfshost::MemoryMapData data;
    data.regions.push_back({0x10000,
                            0x1FFFF,
                            0x04,
                            0x1000,
                            1,
                            static_cast<std::uint16_t>(mpfshost::kMemoryMapFlagImage | mpfshost::kMemoryMapFlagFile),
                            0,
                            "image region"});
    data.regions.push_back({0x20000, 0x2FFFF, 0x02, 0x2000, 2, mpfshost::kMemoryMapFlagPrivate, 3, "private region"});
    const auto decoded = mpfshost::decodeMemoryMapData(mpfshost::encodeMemoryMapData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->regions.size() == 2);
    REQUIRE(decoded->regions[0].start == 0x10000);
    REQUIRE(decoded->regions[0].end == 0x1FFFF);
    REQUIRE(decoded->regions[0].protection == 0x04);
    REQUIRE(decoded->regions[0].commitCharge == 0x1000);
    REQUIRE(decoded->regions[0].vadType == 1);
    REQUIRE(decoded->regions[0].flags == (mpfshost::kMemoryMapFlagImage | mpfshost::kMemoryMapFlagFile));
    REQUIRE(decoded->regions[0].heapNumber == 0);
    REQUIRE(decoded->regions[0].description == "image region");
    REQUIRE(decoded->regions[1].flags == mpfshost::kMemoryMapFlagPrivate);
    REQUIRE(decoded->regions[1].heapNumber == 3);

    const mpfshost::MemoryMapEnd ok {0, 2, ""};
    const auto                   okDecoded = mpfshost::decodeMemoryMapEnd(mpfshost::encodeMemoryMapEnd(ok));
    REQUIRE(okDecoded.has_value());
    REQUIRE(okDecoded->status == 0);
    REQUIRE(okDecoded->regionCount == 2);
    REQUIRE(okDecoded->error.empty());

    const mpfshost::MemoryMapEnd failed {1, 0, "vad failed"};
    const auto                   failedDecoded = mpfshost::decodeMemoryMapEnd(mpfshost::encodeMemoryMapEnd(failed));
    REQUIRE(failedDecoded.has_value());
    REQUIRE(failedDecoded->status == 1);
    REQUIRE(failedDecoded->error == "vad failed");
}

TEST_CASE("vad-ex messages round trip", "[ipc][message]")
{
    const mpfshost::VadExListRequest request {4321, 16, 4};
    const auto requestDecoded = mpfshost::decodeVadExListRequest(mpfshost::encodeVadExListRequest(request));
    REQUIRE(requestDecoded.has_value());
    REQUIRE(requestDecoded->pid == 4321);
    REQUIRE(requestDecoded->pageOffset == 16);
    REQUIRE(requestDecoded->pageCount == 4);

    const mpfshost::VadExListData empty;
    const auto                    emptyDecoded = mpfshost::decodeVadExListData(mpfshost::encodeVadExListData(empty));
    REQUIRE(emptyDecoded.has_value());
    REQUIRE(emptyDecoded->entries.empty());

    mpfshost::VadExDescriptor page;
    page.type      = 1;
    page.pml       = 4;
    page.flags     = static_cast<std::uint8_t>(mpfshost::kVadExFlagHardware | mpfshost::kVadExFlagWrite);
    page.va        = 0x7FF000001000;
    page.pa        = 0x12345000;
    page.pte       = 0x8A00000012345678ull;
    page.protoType = 3;
    page.protoPa   = 0x22222000;
    page.protoPte  = 0x8A00000022222345ull;
    page.vaVadBase = 0x7FF000000000;

    mpfshost::VadExListData data;
    data.entries.push_back(page);
    const auto decoded = mpfshost::decodeVadExListData(mpfshost::encodeVadExListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->entries.size() == 1);
    REQUIRE(decoded->entries[0].type == 1);
    REQUIRE(decoded->entries[0].pml == 4);
    REQUIRE(decoded->entries[0].flags == (mpfshost::kVadExFlagHardware | mpfshost::kVadExFlagWrite));
    REQUIRE(decoded->entries[0].va == 0x7FF000001000);
    REQUIRE(decoded->entries[0].pa == 0x12345000);
    REQUIRE(decoded->entries[0].pte == 0x8A00000012345678ull);
    REQUIRE(decoded->entries[0].protoType == 3);
    REQUIRE(decoded->entries[0].protoPa == 0x22222000);
    REQUIRE(decoded->entries[0].protoPte == 0x8A00000022222345ull);
    REQUIRE(decoded->entries[0].vaVadBase == 0x7FF000000000);

    const mpfshost::VadExListEnd ok {0, 1, ""};
    const auto                   okDecoded = mpfshost::decodeVadExListEnd(mpfshost::encodeVadExListEnd(ok));
    REQUIRE(okDecoded.has_value());
    REQUIRE(okDecoded->status == 0);
    REQUIRE(okDecoded->entryCount == 1);
    REQUIRE(okDecoded->error.empty());

    const mpfshost::VadExListEnd failed {1, 0, "vad-ex failed"};
    const auto                   failedDecoded = mpfshost::decodeVadExListEnd(mpfshost::encodeVadExListEnd(failed));
    REQUIRE(failedDecoded.has_value());
    REQUIRE(failedDecoded->status == 1);
    REQUIRE(failedDecoded->error == "vad-ex failed");
}

TEST_CASE("unknown module flags are rejected", "[ipc][message]")
{
    mpfshost::ByteWriter writer;
    writer.writeU32(1234);
    writer.writeU16(0x8000);
    writer.writeString("");
    REQUIRE_FALSE(mpfshost::decodeModuleListRequest(writer.bytes()).has_value());
}

TEST_CASE("malformed module and memory map payloads are rejected", "[ipc][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodeModuleListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeModuleListData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeModuleListEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeMemoryMapRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeMemoryMapData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeMemoryMapEnd(empty).has_value());

    mpfshost::ByteWriter truncatedModuleRequest;
    truncatedModuleRequest.writeU32(1234);
    truncatedModuleRequest.writeU8(0);
    REQUIRE_FALSE(mpfshost::decodeModuleListRequest(truncatedModuleRequest.bytes()).has_value());

    mpfshost::ByteWriter trailingModuleRequest;
    trailingModuleRequest.writeU32(1234);
    trailingModuleRequest.writeU16(0);
    trailingModuleRequest.writeString("");
    trailingModuleRequest.writeU8(0);
    REQUIRE_FALSE(mpfshost::decodeModuleListRequest(trailingModuleRequest.bytes()).has_value());

    mpfshost::ByteWriter impossibleModuleCount;
    impossibleModuleCount.writeU32(0xFFFFFFFFu);
    REQUIRE_FALSE(mpfshost::decodeModuleListData(impossibleModuleCount.bytes()).has_value());

    mpfshost::ByteWriter truncatedRegion;
    truncatedRegion.writeU32(1);
    truncatedRegion.writeU64(0x1000);
    REQUIRE_FALSE(mpfshost::decodeMemoryMapData(truncatedRegion.bytes()).has_value());

    auto trailingMapRequest = mpfshost::encodeMemoryMapRequest({1, false});
    trailingMapRequest.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeMemoryMapRequest(trailingMapRequest).has_value());
}

TEST_CASE("thread list request round trips", "[ipc][message]")
{
    const mpfshost::ThreadListRequest request {4321, 4322, mpfshost::kThreadFlagCallstack};
    const auto requestDecoded = mpfshost::decodeThreadListRequest(mpfshost::encodeThreadListRequest(request));
    REQUIRE(requestDecoded.has_value());
    REQUIRE(requestDecoded->pid == 4321);
    REQUIRE(requestDecoded->tid == 4322);
    REQUIRE(requestDecoded->flags == mpfshost::kThreadFlagCallstack);

    const mpfshost::ThreadListRequest everyThread {4321, 0, 0};
    const auto everyDecoded = mpfshost::decodeThreadListRequest(mpfshost::encodeThreadListRequest(everyThread));
    REQUIRE(everyDecoded.has_value());
    REQUIRE(everyDecoded->tid == 0);
    REQUIRE(everyDecoded->flags == 0);
}

TEST_CASE("thread list data round trips with and without frames", "[ipc][message]")
{
    const mpfshost::ThreadListData empty;
    const auto                     emptyDecoded = mpfshost::decodeThreadListData(mpfshost::encodeThreadListData(empty));
    REQUIRE(emptyDecoded.has_value());
    REQUIRE(emptyDecoded->threads.empty());

    mpfshost::ThreadDescriptor running;
    running.tid                = 100;
    running.pid                = 4321;
    running.exitStatus         = 0;
    running.userTime           = 250;
    running.kernelTime         = 125;
    running.ethread            = 0xFFFF800000000001ull;
    running.teb                = 0x0000000000123000ull;
    running.startAddress       = 0x00007FF700001000ull;
    running.win32StartAddress  = 0x00007FF700002000ull;
    running.stackBaseUser      = 0x0000000001230000ull;
    running.stackLimitUser     = 0x0000000001220000ull;
    running.stackBaseKernel    = 0xFFFF800000100000ull;
    running.stackLimitKernel   = 0xFFFF8000000F0000ull;
    running.trapFrame          = 0xFFFF800000000123ull;
    running.instructionPointer = 0x00007FF700003000ull;
    running.stackPointer       = 0x000000000122F000ull;
    running.affinity           = 0x0000000000000003ull;
    running.createTime         = 133456789012345678ull;
    running.exitTime           = 0;
    running.impersonationToken = 0xFFFF800000000777ull;
    running.state              = 2;
    running.running            = true;
    running.priority           = 8;
    running.basePriority       = 8;
    running.suspendCount       = 0;
    running.waitReason         = 5;
    running.callstackStatus    = 0;

    mpfshost::ThreadDescriptor blocked;
    blocked.tid             = 101;
    blocked.pid             = 4321;
    blocked.state           = 5;
    blocked.running         = false;
    blocked.suspendCount    = 1;
    blocked.callstackStatus = 1;

    mpfshost::ThreadListData data;
    data.threads.push_back(running);
    data.threads.push_back(blocked);
    const auto decoded = mpfshost::decodeThreadListData(mpfshost::encodeThreadListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->threads.size() == 2);
    REQUIRE(decoded->threads[0].tid == 100);
    REQUIRE(decoded->threads[0].pid == 4321);
    REQUIRE(decoded->threads[0].userTime == 250);
    REQUIRE(decoded->threads[0].kernelTime == 125);
    REQUIRE(decoded->threads[0].ethread == 0xFFFF800000000001ull);
    REQUIRE(decoded->threads[0].teb == 0x0000000000123000ull);
    REQUIRE(decoded->threads[0].startAddress == 0x00007FF700001000ull);
    REQUIRE(decoded->threads[0].win32StartAddress == 0x00007FF700002000ull);
    REQUIRE(decoded->threads[0].stackBaseUser == 0x0000000001230000ull);
    REQUIRE(decoded->threads[0].stackLimitUser == 0x0000000001220000ull);
    REQUIRE(decoded->threads[0].stackBaseKernel == 0xFFFF800000100000ull);
    REQUIRE(decoded->threads[0].stackLimitKernel == 0xFFFF8000000F0000ull);
    REQUIRE(decoded->threads[0].trapFrame == 0xFFFF800000000123ull);
    REQUIRE(decoded->threads[0].instructionPointer == 0x00007FF700003000ull);
    REQUIRE(decoded->threads[0].stackPointer == 0x000000000122F000ull);
    REQUIRE(decoded->threads[0].affinity == 0x0000000000000003ull);
    REQUIRE(decoded->threads[0].createTime == 133456789012345678ull);
    REQUIRE(decoded->threads[0].exitTime == 0);
    REQUIRE(decoded->threads[0].impersonationToken == 0xFFFF800000000777ull);
    REQUIRE(decoded->threads[0].state == 2);
    REQUIRE(decoded->threads[0].running);
    REQUIRE(decoded->threads[0].priority == 8);
    REQUIRE(decoded->threads[0].basePriority == 8);
    REQUIRE(decoded->threads[0].suspendCount == 0);
    REQUIRE(decoded->threads[0].waitReason == 5);
    REQUIRE(decoded->threads[0].callstackStatus == 0);
    REQUIRE(decoded->threads[0].frames.empty());
    REQUIRE(decoded->threads[1].tid == 101);
    REQUIRE_FALSE(decoded->threads[1].running);
    REQUIRE(decoded->threads[1].suspendCount == 1);
    REQUIRE(decoded->threads[1].callstackStatus == 1);
    REQUIRE(decoded->threads[1].frames.empty());

    mpfshost::ThreadFrame frame;
    frame.index            = 0;
    frame.returnAddress    = 0x00007FF700004000ull;
    frame.stackPointer     = 0x000000000122F100ull;
    frame.frameBase        = 0x000000000122F200ull;
    frame.displacement     = 0x1A;
    frame.registersPresent = true;
    frame.module           = "probe.exe";
    frame.function         = "main";

    mpfshost::ThreadDescriptor withFrames;
    withFrames.tid             = 200;
    withFrames.pid             = 4321;
    withFrames.callstackStatus = 0;
    withFrames.frames.push_back(frame);
    withFrames.frames.push_back({1, 0x00007FF700005000ull, 0, 0, 0, false, "", ""});

    mpfshost::ThreadListData frameData;
    frameData.threads.push_back(withFrames);
    const auto frameDecoded = mpfshost::decodeThreadListData(mpfshost::encodeThreadListData(frameData));
    REQUIRE(frameDecoded.has_value());
    REQUIRE(frameDecoded->threads.size() == 1);
    REQUIRE(frameDecoded->threads[0].frames.size() == 2);
    REQUIRE(frameDecoded->threads[0].frames[0].index == 0);
    REQUIRE(frameDecoded->threads[0].frames[0].returnAddress == 0x00007FF700004000ull);
    REQUIRE(frameDecoded->threads[0].frames[0].stackPointer == 0x000000000122F100ull);
    REQUIRE(frameDecoded->threads[0].frames[0].frameBase == 0x000000000122F200ull);
    REQUIRE(frameDecoded->threads[0].frames[0].displacement == 0x1A);
    REQUIRE(frameDecoded->threads[0].frames[0].registersPresent);
    REQUIRE(frameDecoded->threads[0].frames[0].module == "probe.exe");
    REQUIRE(frameDecoded->threads[0].frames[0].function == "main");
    REQUIRE(frameDecoded->threads[0].frames[1].index == 1);
    REQUIRE(frameDecoded->threads[0].frames[1].returnAddress == 0x00007FF700005000ull);
    REQUIRE_FALSE(frameDecoded->threads[0].frames[1].registersPresent);
    REQUIRE(frameDecoded->threads[0].frames[1].module.empty());
    REQUIRE(frameDecoded->threads[0].frames[1].function.empty());
}

TEST_CASE("thread list end round trips", "[ipc][message]")
{
    const mpfshost::ThreadListEnd ok {0, 42, ""};
    const auto                    okDecoded = mpfshost::decodeThreadListEnd(mpfshost::encodeThreadListEnd(ok));
    REQUIRE(okDecoded.has_value());
    REQUIRE(okDecoded->status == 0);
    REQUIRE(okDecoded->threadCount == 42);
    REQUIRE(okDecoded->error.empty());

    const mpfshost::ThreadListEnd failed {1, 3, "thread map failed"};
    const auto                    failedDecoded = mpfshost::decodeThreadListEnd(mpfshost::encodeThreadListEnd(failed));
    REQUIRE(failedDecoded.has_value());
    REQUIRE(failedDecoded->status == 1);
    REQUIRE(failedDecoded->threadCount == 3);
    REQUIRE(failedDecoded->error == "thread map failed");
}

TEST_CASE("unknown thread flags are rejected", "[ipc][message]")
{
    mpfshost::ByteWriter writer;
    writer.writeU32(4321);
    writer.writeU32(0);
    writer.writeU16(0x8000);
    REQUIRE_FALSE(mpfshost::decodeThreadListRequest(writer.bytes()).has_value());
}

TEST_CASE("malformed thread payloads are rejected", "[ipc][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodeThreadListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeThreadListData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeThreadListEnd(empty).has_value());

    mpfshost::ByteWriter truncatedRequest;
    truncatedRequest.writeU32(4321);
    truncatedRequest.writeU32(0);
    REQUIRE_FALSE(mpfshost::decodeThreadListRequest(truncatedRequest.bytes()).has_value());

    auto trailingRequest = mpfshost::encodeThreadListRequest({4321, 0, 0});
    trailingRequest.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeThreadListRequest(trailingRequest).has_value());

    mpfshost::ByteWriter impossibleThreadCount;
    impossibleThreadCount.writeU32(0xFFFFFFFFu);
    REQUIRE_FALSE(mpfshost::decodeThreadListData(impossibleThreadCount.bytes()).has_value());

    mpfshost::ByteWriter impossibleFrameCount;
    impossibleFrameCount.writeU32(1);
    impossibleFrameCount.writeU32(100);
    impossibleFrameCount.writeU32(4321);
    impossibleFrameCount.writeU32(0);
    impossibleFrameCount.writeU32(0);
    impossibleFrameCount.writeU32(0);
    for (int i = 0; i < 15; ++i)
    {
        impossibleFrameCount.writeU64(0);
    }
    for (int i = 0; i < 7; ++i)
    {
        impossibleFrameCount.writeU8(0);
    }
    impossibleFrameCount.writeU32(0xFFFFFFFFu);
    REQUIRE_FALSE(mpfshost::decodeThreadListData(impossibleFrameCount.bytes()).has_value());

    mpfshost::ByteWriter truncatedThread;
    truncatedThread.writeU32(1);
    truncatedThread.writeU32(100);
    REQUIRE_FALSE(mpfshost::decodeThreadListData(truncatedThread.bytes()).has_value());
}

TEST_CASE("vm list response round trips", "[ipc][message]")
{
    const mpfshost::VmListResponse empty;
    const auto                     emptyDecoded = mpfshost::decodeVmListResponse(mpfshost::encodeVmListResponse(empty));
    REQUIRE(emptyDecoded.has_value());
    REQUIRE(emptyDecoded->vms.empty());

    mpfshost::VmListResponse single;
    single.vms.push_back({"physical", 0x100000000ull, 1, 2, 0, 22631, 4242, true, false, false});
    const auto singleDecoded = mpfshost::decodeVmListResponse(mpfshost::encodeVmListResponse(single));
    REQUIRE(singleDecoded.has_value());
    REQUIRE(singleDecoded->vms.size() == 1);
    REQUIRE(singleDecoded->vms[0].name == "physical");
    REQUIRE(singleDecoded->vms[0].gpaMax == 0x100000000ull);
    REQUIRE(singleDecoded->vms[0].kind == 1);
    REQUIRE(singleDecoded->vms[0].systemKind == 2);
    REQUIRE(singleDecoded->vms[0].versionBuild == 22631);
    REQUIRE(singleDecoded->vms[0].vmMemPid == 4242);
    REQUIRE(singleDecoded->vms[0].active);
    REQUIRE_FALSE(singleDecoded->vms[0].readOnly);
    REQUIRE_FALSE(singleDecoded->vms[0].physicalOnly);

    mpfshost::VmListResponse unnamed;
    unnamed.vms.push_back({"", 0, 0, 0, 7, 0, 0, false, true, true});
    const auto unnamedDecoded = mpfshost::decodeVmListResponse(mpfshost::encodeVmListResponse(unnamed));
    REQUIRE(unnamedDecoded.has_value());
    REQUIRE(unnamedDecoded->vms.size() == 1);
    REQUIRE(unnamedDecoded->vms[0].name.empty());
    REQUIRE(unnamedDecoded->vms[0].partitionId == 7);
    REQUIRE_FALSE(unnamedDecoded->vms[0].active);
    REQUIRE(unnamedDecoded->vms[0].readOnly);
    REQUIRE(unnamedDecoded->vms[0].physicalOnly);
}

TEST_CASE("physical read messages round trip", "[ipc][message]")
{
    const mpfshost::PhysicalReadRequest request {3, 0x123456000ull, 0x3000};
    const auto decoded = mpfshost::decodePhysicalReadRequest(mpfshost::encodePhysicalReadRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->vmIndex == 3);
    REQUIRE(decoded->gpa == 0x123456000ull);
    REQUIRE(decoded->size == 0x3000);

    const mpfshost::PhysicalReadEnd ok {0, 0x3000, ""};
    const auto                      okDecoded = mpfshost::decodePhysicalReadEnd(mpfshost::encodePhysicalReadEnd(ok));
    REQUIRE(okDecoded.has_value());
    REQUIRE(okDecoded->status == 0);
    REQUIRE(okDecoded->bytesDelivered == 0x3000);
    REQUIRE(okDecoded->error.empty());

    const mpfshost::PhysicalReadEnd failed {1, 0, "gpa read failed"};
    const auto failedDecoded = mpfshost::decodePhysicalReadEnd(mpfshost::encodePhysicalReadEnd(failed));
    REQUIRE(failedDecoded.has_value());
    REQUIRE(failedDecoded->status == 1);
    REQUIRE(failedDecoded->bytesDelivered == 0);
    REQUIRE(failedDecoded->error == "gpa read failed");
}

TEST_CASE("physical write messages round trip", "[ipc][message]")
{
    const mpfshost::PhysicalWriteRequest request {
        2, 0xF000, {std::byte {0x40}, std::byte {0x41}, std::byte {0x42}}
    };
    const auto decoded = mpfshost::decodePhysicalWriteRequest(mpfshost::encodePhysicalWriteRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->vmIndex == 2);
    REQUIRE(decoded->gpa == 0xF000);
    REQUIRE(decoded->data.size() == 3);
    REQUIRE(decoded->data[0] == std::byte {0x40});
    REQUIRE(decoded->data[2] == std::byte {0x42});

    const mpfshost::MemoryWriteResponse response {3};
    const auto responseDecoded = mpfshost::decodeMemoryWriteResponse(mpfshost::encodeMemoryWriteResponse(response));
    REQUIRE(responseDecoded.has_value());
    REQUIRE(responseDecoded->bytesWritten == 3);
}

TEST_CASE("gpa translate messages round trip", "[ipc][message]")
{
    const mpfshost::GpaTranslateRequest request {1, 0x200001000ull};
    const auto requestDecoded = mpfshost::decodeGpaTranslateRequest(mpfshost::encodeGpaTranslateRequest(request));
    REQUIRE(requestDecoded.has_value());
    REQUIRE(requestDecoded->vmIndex == 1);
    REQUIRE(requestDecoded->gpa == 0x200001000ull);

    const mpfshost::GpaTranslateResponse both {0x100001000ull, 0x7FF600001000ull, true, true};
    const auto bothDecoded = mpfshost::decodeGpaTranslateResponse(mpfshost::encodeGpaTranslateResponse(both));
    REQUIRE(bothDecoded.has_value());
    REQUIRE(bothDecoded->pa == 0x100001000ull);
    REQUIRE(bothDecoded->va == 0x7FF600001000ull);
    REQUIRE(bothDecoded->paPresent);
    REQUIRE(bothDecoded->vaPresent);

    const mpfshost::GpaTranslateResponse paOnly {0x100002000ull, 0, true, false};
    const auto paOnlyDecoded = mpfshost::decodeGpaTranslateResponse(mpfshost::encodeGpaTranslateResponse(paOnly));
    REQUIRE(paOnlyDecoded.has_value());
    REQUIRE(paOnlyDecoded->paPresent);
    REQUIRE_FALSE(paOnlyDecoded->vaPresent);
    REQUIRE(paOnlyDecoded->va == 0);
}

TEST_CASE("virt2phys messages round trip", "[ipc][message]")
{
    const mpfshost::Virt2PhysRequest request {4321, 0x7FF700004000ull};
    const auto requestDecoded = mpfshost::decodeVirt2PhysRequest(mpfshost::encodeVirt2PhysRequest(request));
    REQUIRE(requestDecoded.has_value());
    REQUIRE(requestDecoded->pid == 4321);
    REQUIRE(requestDecoded->va == 0x7FF700004000ull);

    const mpfshost::Virt2PhysResponse response {0x345678000ull};
    const auto responseDecoded = mpfshost::decodeVirt2PhysResponse(mpfshost::encodeVirt2PhysResponse(response));
    REQUIRE(responseDecoded.has_value());
    REQUIRE(responseDecoded->pa == 0x345678000ull);
}

TEST_CASE("physical scatter request round trips", "[ipc][message]")
{
    const mpfshost::PhysicalScatterRequest empty {1, {}};
    const auto emptyDecoded = mpfshost::decodePhysicalScatterRequest(mpfshost::encodePhysicalScatterRequest(empty));
    REQUIRE(emptyDecoded.has_value());
    REQUIRE(emptyDecoded->vmIndex == 1);
    REQUIRE(emptyDecoded->entries.empty());

    mpfshost::PhysicalScatterRequest request {2, {}};
    request.entries.push_back({0x1000, 16, mpfshost::kScatterEntryRead, {}});
    request.entries.push_back({
        0x2000, 8, mpfshost::kScatterEntryWrite, {std::byte {1}, std::byte {2}, std::byte {3}, std::byte {4}}
    });
    const auto decoded = mpfshost::decodePhysicalScatterRequest(mpfshost::encodePhysicalScatterRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->vmIndex == 2);
    REQUIRE(decoded->entries.size() == 2);
    REQUIRE(decoded->entries[0].address == 0x1000);
    REQUIRE(decoded->entries[0].size == 16);
    REQUIRE(decoded->entries[0].kind == mpfshost::kScatterEntryRead);
    REQUIRE(decoded->entries[1].address == 0x2000);
    REQUIRE(decoded->entries[1].kind == mpfshost::kScatterEntryWrite);
    REQUIRE(decoded->entries[1].data.size() == 4);
}

TEST_CASE("malformed physical payloads are rejected", "[ipc][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodeVmListResponse(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePhysicalReadRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePhysicalReadEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePhysicalWriteRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeGpaTranslateRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeGpaTranslateResponse(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeVirt2PhysRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeVirt2PhysResponse(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePhysicalScatterRequest(empty).has_value());

    mpfshost::ByteWriter truncatedRead;
    truncatedRead.writeU32(1);
    truncatedRead.writeU64(0x1000);
    REQUIRE_FALSE(mpfshost::decodePhysicalReadRequest(truncatedRead.bytes()).has_value());

    auto trailingRead = mpfshost::encodePhysicalReadRequest({1, 0x1000, 16});
    trailingRead.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodePhysicalReadRequest(trailingRead).has_value());

    auto trailingTranslate = mpfshost::encodeGpaTranslateResponse({0x1000, 0x2000, true, true});
    trailingTranslate.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeGpaTranslateResponse(trailingTranslate).has_value());

    auto trailingVirt2Phys = mpfshost::encodeVirt2PhysRequest({1, 0x1000});
    trailingVirt2Phys.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeVirt2PhysRequest(trailingVirt2Phys).has_value());

    mpfshost::ByteWriter impossibleVmCount;
    impossibleVmCount.writeU32(0xFFFFFFFFu);
    REQUIRE_FALSE(mpfshost::decodeVmListResponse(impossibleVmCount.bytes()).has_value());

    mpfshost::ByteWriter impossibleEntryCount;
    impossibleEntryCount.writeU32(1);
    impossibleEntryCount.writeU32(0xFFFFFFFFu);
    REQUIRE_FALSE(mpfshost::decodePhysicalScatterRequest(impossibleEntryCount.bytes()).has_value());

    auto trailingScatter = mpfshost::encodePhysicalScatterRequest({1, {{0x1000, 8, mpfshost::kScatterEntryRead, {}}}});
    trailingScatter.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodePhysicalScatterRequest(trailingScatter).has_value());
}

TEST_CASE("process info request round trips", "[core][message]")
{
    const mpfshost::ProcessInfoRequest byPid {1234, {}};
    const auto byPidDecoded = mpfshost::decodeProcessInfoRequest(mpfshost::encodeProcessInfoRequest(byPid));
    REQUIRE(byPidDecoded.has_value());
    REQUIRE(byPidDecoded->pid == 1234);
    REQUIRE(byPidDecoded->name.empty());

    const mpfshost::ProcessInfoRequest byName {0, "notepad.exe"};
    const auto byNameDecoded = mpfshost::decodeProcessInfoRequest(mpfshost::encodeProcessInfoRequest(byName));
    REQUIRE(byNameDecoded.has_value());
    REQUIRE(byNameDecoded->pid == 0);
    REQUIRE(byNameDecoded->name == "notepad.exe");
}

TEST_CASE("process info response round trips", "[core][message]")
{
    mpfshost::ProcessInfoResponse response;
    response.process        = {4321, 1234, "notepad.exe", true};
    response.state          = 3;
    response.memoryModel    = 2;
    response.systemType     = 1;
    response.userOnly       = true;
    response.dtb            = 0x1ad000;
    response.dtbUser        = 0x1ae000;
    response.eprocessVa     = 0xffff800000123000ull;
    response.pebVa          = 0x000000a1b2c3d000ull;
    response.peb32Va        = 0x7ffd0000;
    response.sessionId      = 2;
    response.luid           = 0x000000000003e7;
    response.integrityLevel = 4;
    response.sid            = "S-1-5-21-1000";
    response.imagePath      = "C:\\Windows\\notepad.exe";
    response.kernelPath     = "C:\\Windows\\System32\\ntoskrnl.exe";
    response.cmdline        = "\"notepad.exe\" a.txt";

    const auto decoded = mpfshost::decodeProcessInfoResponse(mpfshost::encodeProcessInfoResponse(response));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->process.pid == 4321);
    REQUIRE(decoded->process.parentPid == 1234);
    REQUIRE(decoded->process.name == "notepad.exe");
    REQUIRE(decoded->process.wow64);
    REQUIRE(decoded->state == 3);
    REQUIRE(decoded->memoryModel == 2);
    REQUIRE(decoded->systemType == 1);
    REQUIRE(decoded->userOnly);
    REQUIRE(decoded->dtb == 0x1ad000);
    REQUIRE(decoded->dtbUser == 0x1ae000);
    REQUIRE(decoded->eprocessVa == 0xffff800000123000ull);
    REQUIRE(decoded->pebVa == 0x000000a1b2c3d000ull);
    REQUIRE(decoded->peb32Va == 0x7ffd0000);
    REQUIRE(decoded->sessionId == 2);
    REQUIRE(decoded->luid == 0x000000000003e7);
    REQUIRE(decoded->integrityLevel == 4);
    REQUIRE(decoded->sid == "S-1-5-21-1000");
    REQUIRE(decoded->imagePath == "C:\\Windows\\notepad.exe");
    REQUIRE(decoded->kernelPath == "C:\\Windows\\System32\\ntoskrnl.exe");
    REQUIRE(decoded->cmdline == "\"notepad.exe\" a.txt");
}

TEST_CASE("malformed process info payloads are rejected", "[core][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodeProcessInfoRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeProcessInfoResponse(empty).has_value());

    auto truncatedRequest = mpfshost::encodeProcessInfoRequest({7, "explorer.exe"});
    truncatedRequest.pop_back();
    REQUIRE_FALSE(mpfshost::decodeProcessInfoRequest(truncatedRequest).has_value());

    auto trailingRequest = mpfshost::encodeProcessInfoRequest({7, "explorer.exe"});
    trailingRequest.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeProcessInfoRequest(trailingRequest).has_value());

    auto trailingResponse = mpfshost::encodeProcessInfoResponse(mpfshost::ProcessInfoResponse {});
    trailingResponse.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeProcessInfoResponse(trailingResponse).has_value());

    mpfshost::ProcessInfoResponse oversized;
    oversized.cmdline = std::string(mpfshost::kMaxMapStringLength + 1, 'x');
    REQUIRE_FALSE(mpfshost::decodeProcessInfoResponse(mpfshost::encodeProcessInfoResponse(oversized)).has_value());

    mpfshost::ByteWriter truncatedWriter;
    truncatedWriter.writeU32(9);
    truncatedWriter.writeString("explorer.exe");
    truncatedWriter.writeU32(9); // process descriptor stops halfway through
    REQUIRE_FALSE(mpfshost::decodeProcessInfoResponse(truncatedWriter.bytes()).has_value());
}

TEST_CASE("symbol lookup round trips", "[core][message]")
{
    const mpfshost::SymbolLookupRequest request {1234, "kernel32.dll", "CreateFileW"};
    const auto decodedRequest = mpfshost::decodeSymbolLookupRequest(mpfshost::encodeSymbolLookupRequest(request));
    REQUIRE(decodedRequest.has_value());
    REQUIRE(decodedRequest->pid == 1234);
    REQUIRE(decodedRequest->module == "kernel32.dll");
    REQUIRE(decodedRequest->symbol == "CreateFileW");

    const mpfshost::SymbolLookupRequest baseOnly {1234, "kernel32.dll", {}};
    const auto decodedBaseOnly = mpfshost::decodeSymbolLookupRequest(mpfshost::encodeSymbolLookupRequest(baseOnly));
    REQUIRE(decodedBaseOnly.has_value());
    REQUIRE(decodedBaseOnly->symbol.empty());

    const mpfshost::SymbolLookupResponse found {0x7ffb10000000ull, 0x7ffb10012340ull, 0x12340, true};
    const auto decodedFound = mpfshost::decodeSymbolLookupResponse(mpfshost::encodeSymbolLookupResponse(found));
    REQUIRE(decodedFound.has_value());
    REQUIRE(decodedFound->moduleBase == 0x7ffb10000000ull);
    REQUIRE(decodedFound->address == 0x7ffb10012340ull);
    REQUIRE(decodedFound->offset == 0x12340);
    REQUIRE(decodedFound->found);

    const mpfshost::SymbolLookupResponse missing {0x7ffb10000000ull, 0, 0, false};
    const auto decodedMissing = mpfshost::decodeSymbolLookupResponse(mpfshost::encodeSymbolLookupResponse(missing));
    REQUIRE(decodedMissing.has_value());
    REQUIRE(decodedMissing->moduleBase == 0x7ffb10000000ull);
    REQUIRE(decodedMissing->address == 0);
    REQUIRE_FALSE(decodedMissing->found);
}

TEST_CASE("malformed symbol lookup payloads are rejected", "[core][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodeSymbolLookupRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeSymbolLookupResponse(empty).has_value());

    auto truncatedRequest = mpfshost::encodeSymbolLookupRequest({7, "kernel32.dll", "CreateFileW"});
    truncatedRequest.pop_back();
    REQUIRE_FALSE(mpfshost::decodeSymbolLookupRequest(truncatedRequest).has_value());

    auto trailingRequest = mpfshost::encodeSymbolLookupRequest({7, "kernel32.dll", {}});
    trailingRequest.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeSymbolLookupRequest(trailingRequest).has_value());

    auto trailingResponse = mpfshost::encodeSymbolLookupResponse({});
    trailingResponse.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeSymbolLookupResponse(trailingResponse).has_value());

    mpfshost::SymbolLookupRequest oversized {7, std::string(mpfshost::kMaxMapStringLength + 1, 'm'), {}};
    REQUIRE_FALSE(mpfshost::decodeSymbolLookupRequest(mpfshost::encodeSymbolLookupRequest(oversized)).has_value());
}

TEST_CASE("module-by-address messages round trip", "[core][message]")
{
    const mpfshost::ModuleByAddressRequest request {4321, 0x7ffb10012340ull};
    const auto decodedRequest = mpfshost::decodeModuleByAddressRequest(mpfshost::encodeModuleByAddressRequest(request));
    REQUIRE(decodedRequest.has_value());
    REQUIRE(decodedRequest->pid == 4321);
    REQUIRE(decodedRequest->address == 0x7ffb10012340ull);

    mpfshost::ModuleByAddressResponse response;
    response.base      = 0x7ffb10000000ull;
    response.entry     = 0x7ffb10001000ull;
    response.imageSize = 0x200000;
    response.offset    = 0x12340;
    response.kind      = 2;
    response.wow64     = true;
    response.name      = "notepad.exe";
    response.fullName  = "C:\\Windows\\notepad.exe";

    const auto decodedResponse =
        mpfshost::decodeModuleByAddressResponse(mpfshost::encodeModuleByAddressResponse(response));
    REQUIRE(decodedResponse.has_value());
    REQUIRE(decodedResponse->base == 0x7ffb10000000ull);
    REQUIRE(decodedResponse->entry == 0x7ffb10001000ull);
    REQUIRE(decodedResponse->imageSize == 0x200000);
    REQUIRE(decodedResponse->offset == 0x12340);
    REQUIRE(decodedResponse->kind == 2);
    REQUIRE(decodedResponse->wow64);
    REQUIRE(decodedResponse->name == "notepad.exe");
    REQUIRE(decodedResponse->fullName == "C:\\Windows\\notepad.exe");
}

TEST_CASE("malformed module-by-address payloads are rejected", "[core][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodeModuleByAddressRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeModuleByAddressResponse(empty).has_value());

    auto truncatedRequest = mpfshost::encodeModuleByAddressRequest({7, 0x1000});
    truncatedRequest.pop_back();
    REQUIRE_FALSE(mpfshost::decodeModuleByAddressRequest(truncatedRequest).has_value());

    auto trailingResponse = mpfshost::encodeModuleByAddressResponse({});
    trailingResponse.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeModuleByAddressResponse(trailingResponse).has_value());

    mpfshost::ModuleByAddressResponse oversized;
    oversized.name = std::string(mpfshost::kMaxMapStringLength + 1, 'm');
    REQUIRE_FALSE(
        mpfshost::decodeModuleByAddressResponse(mpfshost::encodeModuleByAddressResponse(oversized)).has_value());
}

TEST_CASE("process-by-dtb messages round trip", "[core][message]")
{
    const mpfshost::ProcessByDtbRequest request {0x1aa000};
    const auto decodedRequest = mpfshost::decodeProcessByDtbRequest(mpfshost::encodeProcessByDtbRequest(request));
    REQUIRE(decodedRequest.has_value());
    REQUIRE(decodedRequest->dtb == 0x1aa000);

    mpfshost::ProcessByDtbResponse response;
    response.process     = {1234, 4, "notepad.exe", false};
    response.dtb         = 0x1aa000;
    response.dtbUser     = 0x1ab000;
    response.matchedUser = true;

    const auto decodedResponse = mpfshost::decodeProcessByDtbResponse(mpfshost::encodeProcessByDtbResponse(response));
    REQUIRE(decodedResponse.has_value());
    REQUIRE(decodedResponse->process.pid == 1234);
    REQUIRE(decodedResponse->process.parentPid == 4);
    REQUIRE(decodedResponse->process.name == "notepad.exe");
    REQUIRE_FALSE(decodedResponse->process.wow64);
    REQUIRE(decodedResponse->dtb == 0x1aa000);
    REQUIRE(decodedResponse->dtbUser == 0x1ab000);
    REQUIRE(decodedResponse->matchedUser);
}

TEST_CASE("malformed process-by-dtb payloads are rejected", "[core][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodeProcessByDtbRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeProcessByDtbResponse(empty).has_value());

    auto truncatedRequest = mpfshost::encodeProcessByDtbRequest({0x1000});
    truncatedRequest.pop_back();
    REQUIRE_FALSE(mpfshost::decodeProcessByDtbRequest(truncatedRequest).has_value());

    auto trailingResponse = mpfshost::encodeProcessByDtbResponse({});
    trailingResponse.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeProcessByDtbResponse(trailingResponse).has_value());
}

TEST_CASE("pdb messages round trip", "[core][message]")
{
    const mpfshost::PdbLoadRequest loadRequest {1234, 0xfffff80000000000ull};
    const auto decodedLoadRequest = mpfshost::decodePdbLoadRequest(mpfshost::encodePdbLoadRequest(loadRequest));
    REQUIRE(decodedLoadRequest.has_value());
    REQUIRE(decodedLoadRequest->pid == 1234);
    REQUIRE(decodedLoadRequest->moduleBase == 0xfffff80000000000ull);

    const mpfshost::PdbLoadResponse loadResponse {"nt"};
    const auto decodedLoadResponse = mpfshost::decodePdbLoadResponse(mpfshost::encodePdbLoadResponse(loadResponse));
    REQUIRE(decodedLoadResponse.has_value());
    REQUIRE(decodedLoadResponse->module == "nt");

    const mpfshost::PdbSymbolNameRequest nameRequest {"nt", 0xfffff80000123456ull};
    const auto                           decodedNameRequest =
        mpfshost::decodePdbSymbolNameRequest(mpfshost::encodePdbSymbolNameRequest(nameRequest));
    REQUIRE(decodedNameRequest.has_value());
    REQUIRE(decodedNameRequest->module == "nt");
    REQUIRE(decodedNameRequest->address == 0xfffff80000123456ull);

    const mpfshost::PdbSymbolNameResponse nameResponse {"PsGetCurrentProcess", 4};
    const auto                            decodedNameResponse =
        mpfshost::decodePdbSymbolNameResponse(mpfshost::encodePdbSymbolNameResponse(nameResponse));
    REQUIRE(decodedNameResponse.has_value());
    REQUIRE(decodedNameResponse->symbol == "PsGetCurrentProcess");
    REQUIRE(decodedNameResponse->displacement == 4);

    const mpfshost::PdbSymbolAddressRequest addressRequest {"nt", "PsGetCurrentProcess"};
    const auto                              decodedAddressRequest =
        mpfshost::decodePdbSymbolAddressRequest(mpfshost::encodePdbSymbolAddressRequest(addressRequest));
    REQUIRE(decodedAddressRequest.has_value());
    REQUIRE(decodedAddressRequest->module == "nt");
    REQUIRE(decodedAddressRequest->symbol == "PsGetCurrentProcess");

    const mpfshost::PdbSymbolAddressResponse addressResponse {0xfffff80000123456ull};
    const auto                               decodedAddressResponse =
        mpfshost::decodePdbSymbolAddressResponse(mpfshost::encodePdbSymbolAddressResponse(addressResponse));
    REQUIRE(decodedAddressResponse.has_value());
    REQUIRE(decodedAddressResponse->address == 0xfffff80000123456ull);

    const mpfshost::PdbTypeSizeRequest typeSizeRequest {"nt", "_EPROCESS"};
    const auto                         decodedTypeSizeRequest =
        mpfshost::decodePdbTypeSizeRequest(mpfshost::encodePdbTypeSizeRequest(typeSizeRequest));
    REQUIRE(decodedTypeSizeRequest.has_value());
    REQUIRE(decodedTypeSizeRequest->module == "nt");
    REQUIRE(decodedTypeSizeRequest->type == "_EPROCESS");

    const mpfshost::PdbTypeSizeResponse typeSizeResponse {0x4d0};
    const auto                          decodedTypeSizeResponse =
        mpfshost::decodePdbTypeSizeResponse(mpfshost::encodePdbTypeSizeResponse(typeSizeResponse));
    REQUIRE(decodedTypeSizeResponse.has_value());
    REQUIRE(decodedTypeSizeResponse->size == 0x4d0);

    const mpfshost::PdbTypeChildOffsetRequest childOffsetRequest {"nt", "_EPROCESS", "UniqueProcessId"};
    const auto                                decodedChildOffsetRequest =
        mpfshost::decodePdbTypeChildOffsetRequest(mpfshost::encodePdbTypeChildOffsetRequest(childOffsetRequest));
    REQUIRE(decodedChildOffsetRequest.has_value());
    REQUIRE(decodedChildOffsetRequest->module == "nt");
    REQUIRE(decodedChildOffsetRequest->type == "_EPROCESS");
    REQUIRE(decodedChildOffsetRequest->child == "UniqueProcessId");

    const mpfshost::PdbTypeChildOffsetResponse childOffsetResponse {0x440};
    const auto                                 decodedChildOffsetResponse =
        mpfshost::decodePdbTypeChildOffsetResponse(mpfshost::encodePdbTypeChildOffsetResponse(childOffsetResponse));
    REQUIRE(decodedChildOffsetResponse.has_value());
    REQUIRE(decodedChildOffsetResponse->offset == 0x440);
}

TEST_CASE("malformed pdb payloads are rejected", "[core][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodePdbLoadRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePdbLoadResponse(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePdbSymbolNameRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePdbSymbolNameResponse(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePdbSymbolAddressRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePdbSymbolAddressResponse(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePdbTypeSizeRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePdbTypeSizeResponse(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePdbTypeChildOffsetRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePdbTypeChildOffsetResponse(empty).has_value());

    auto trailing = mpfshost::encodePdbSymbolAddressResponse({});
    trailing.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodePdbSymbolAddressResponse(trailing).has_value());

    mpfshost::PdbSymbolNameRequest oversized {"nt", 0};
    oversized.module.assign(mpfshost::kMaxMapStringLength + 1, 'm');
    REQUIRE_FALSE(mpfshost::decodePdbSymbolNameRequest(mpfshost::encodePdbSymbolNameRequest(oversized)).has_value());
}

TEST_CASE("module layout round trips", "[core][message]")
{
    const mpfshost::ModuleLayoutRequest request {1234, "notepad.exe"};
    const auto decodedRequest = mpfshost::decodeModuleLayoutRequest(mpfshost::encodeModuleLayoutRequest(request));
    REQUIRE(decodedRequest.has_value());
    REQUIRE(decodedRequest->pid == 1234);
    REQUIRE(decodedRequest->module == "notepad.exe");

    mpfshost::ModuleLayoutResponse response;
    response.moduleBase = 0x00007ff700000000ull;
    response.sections.push_back({" .text", response.moduleBase + 0x1000, 0x2000, 0x400, 0x2000, 0x60000020});
    response.sections.push_back({".rdata", response.moduleBase + 0x3000, 0x1000, 0x2400, 0x1000, 0x40000040});
    for (std::uint32_t i = 0; i < mpfshost::kModuleDataDirectoryCount; ++i)
    {
        response.directories.push_back({i, i * 0x1000, i * 0x20});
    }

    const auto decoded = mpfshost::decodeModuleLayoutResponse(mpfshost::encodeModuleLayoutResponse(response));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->moduleBase == 0x00007ff700000000ull);
    REQUIRE(decoded->sections.size() == 2);
    REQUIRE(decoded->sections[0].name == " .text");
    REQUIRE(decoded->sections[0].virtualAddress == response.moduleBase + 0x1000);
    REQUIRE(decoded->sections[0].virtualSize == 0x2000);
    REQUIRE(decoded->sections[0].rawPointer == 0x400);
    REQUIRE(decoded->sections[0].rawSize == 0x2000);
    REQUIRE(decoded->sections[0].characteristics == 0x60000020);
    REQUIRE(decoded->sections[1].name == ".rdata");
    REQUIRE(decoded->directories.size() == mpfshost::kModuleDataDirectoryCount);
    REQUIRE(decoded->directories.at(15).index == 15);
    REQUIRE(decoded->directories.at(15).virtualAddress == 15 * 0x1000);
    REQUIRE(decoded->directories.at(15).size == 15 * 0x20);
}

TEST_CASE("module layout round trips an empty layout", "[core][message]")
{
    const mpfshost::ModuleLayoutResponse empty {0x1234, {}, {}};
    const auto decoded = mpfshost::decodeModuleLayoutResponse(mpfshost::encodeModuleLayoutResponse(empty));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->moduleBase == 0x1234);
    REQUIRE(decoded->sections.empty());
    REQUIRE(decoded->directories.empty());
}

TEST_CASE("malformed module layout payloads are rejected", "[core][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodeModuleLayoutRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeModuleLayoutResponse(empty).has_value());

    auto truncatedRequest = mpfshost::encodeModuleLayoutRequest({7, "notepad.exe"});
    truncatedRequest.pop_back();
    REQUIRE_FALSE(mpfshost::decodeModuleLayoutRequest(truncatedRequest).has_value());

    auto trailingRequest = mpfshost::encodeModuleLayoutRequest({7, "notepad.exe"});
    trailingRequest.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeModuleLayoutRequest(trailingRequest).has_value());

    mpfshost::ByteWriter tooManySections;
    tooManySections.writeU64(0x1000);
    tooManySections.writeU32(static_cast<std::uint32_t>(mpfshost::kMaxModuleSections + 1));
    REQUIRE_FALSE(mpfshost::decodeModuleLayoutResponse(tooManySections.bytes()).has_value());

    mpfshost::ByteWriter tooManyDirectories;
    tooManyDirectories.writeU64(0x1000);
    tooManyDirectories.writeU32(0);
    tooManyDirectories.writeU32(static_cast<std::uint32_t>(mpfshost::kModuleDataDirectoryCount + 1));
    REQUIRE_FALSE(mpfshost::decodeModuleLayoutResponse(tooManyDirectories.bytes()).has_value());

    mpfshost::ModuleLayoutResponse oversized;
    oversized.sections.push_back({std::string(mpfshost::kMaxMapStringLength + 1, 's'), 0x1000, 0, 0, 0, 0});
    REQUIRE_FALSE(mpfshost::decodeModuleLayoutResponse(mpfshost::encodeModuleLayoutResponse(oversized)).has_value());

    auto trailing = mpfshost::encodeModuleLayoutResponse({0x1000, {}, {}});
    trailing.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeModuleLayoutResponse(trailing).has_value());
}

TEST_CASE("virtual query round trips", "[core][message]")
{
    const mpfshost::VirtualQueryRequest request {1234, 0x00007ff700001008ull};
    const auto decodedRequest = mpfshost::decodeVirtualQueryRequest(mpfshost::encodeVirtualQueryRequest(request));
    REQUIRE(decodedRequest.has_value());
    REQUIRE(decodedRequest->pid == 1234);
    REQUIRE(decodedRequest->address == 0x00007ff700001008ull);

    mpfshost::VirtualRegion region;
    region.baseAddress       = 0x00007ff700000000ull;
    region.allocationBase    = 0x00007ff6ffff0000ull;
    region.regionSize        = 0x2000;
    region.allocationProtect = 0x80;
    region.state             = 0x1000;
    region.protect           = 0x20;
    region.type              = 0x1000000;
    region.partitionId       = 2;

    const auto decoded = mpfshost::decodeVirtualQueryResponse(mpfshost::encodeVirtualQueryResponse({region}));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->region.baseAddress == 0x00007ff700000000ull);
    REQUIRE(decoded->region.allocationBase == 0x00007ff6ffff0000ull);
    REQUIRE(decoded->region.regionSize == 0x2000);
    REQUIRE(decoded->region.allocationProtect == 0x80);
    REQUIRE(decoded->region.state == 0x1000);
    REQUIRE(decoded->region.protect == 0x20);
    REQUIRE(decoded->region.type == 0x1000000);
    REQUIRE(decoded->region.partitionId == 2);
}

TEST_CASE("malformed virtual query payloads are rejected", "[core][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodeVirtualQueryRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeVirtualQueryResponse(empty).has_value());

    auto truncatedRequest = mpfshost::encodeVirtualQueryRequest({7, 0x1000});
    truncatedRequest.pop_back();
    REQUIRE_FALSE(mpfshost::decodeVirtualQueryRequest(truncatedRequest).has_value());

    auto trailingRequest = mpfshost::encodeVirtualQueryRequest({7, 0x1000});
    trailingRequest.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeVirtualQueryRequest(trailingRequest).has_value());

    auto trailingResponse = mpfshost::encodeVirtualQueryResponse({});
    trailingResponse.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeVirtualQueryResponse(trailingResponse).has_value());
}

TEST_CASE("memory search messages round trip", "[core][message]")
{
    mpfshost::SearchTerm term;
    term.pattern = {std::byte {0xDE}, std::byte {0x00}, std::byte {0xEF}};
    term.mask    = {std::byte {0x00}, std::byte {0xFF}, std::byte {0x00}};
    const mpfshost::MemorySearchRequest request {7, term, 4, 0x1000, 0x9000, 256};

    const auto decoded = mpfshost::decodeMemorySearchRequest(mpfshost::encodeMemorySearchRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->pid == 7);
    REQUIRE(decoded->term.pattern == term.pattern);
    REQUIRE(decoded->term.mask == term.mask);
    REQUIRE(decoded->alignBytes == 4);
    REQUIRE(decoded->minAddress == 0x1000);
    REQUIRE(decoded->maxAddress == 0x9000);
    REQUIRE(decoded->maxResults == 256);

    const std::vector<std::uint64_t> hits = {0x1000, 0x2000, 0x3000};
    const auto decodedHits                = mpfshost::decodeMemorySearchData(mpfshost::encodeMemorySearchData({hits}));
    REQUIRE(decodedHits.has_value());
    REQUIRE(decodedHits->addresses == hits);

    const auto end = mpfshost::decodeMemorySearchEnd(mpfshost::encodeMemorySearchEnd({0, 3, 1, ""}));
    REQUIRE(end.has_value());
    REQUIRE(end->status == 0);
    REQUIRE(end->hitCount == 3);
    REQUIRE(end->truncated == 1);

    auto longerThanCap = term;
    longerThanCap.pattern.assign(mpfshost::kMaxSearchPatternLength + 1, std::byte {0x00});
    longerThanCap.mask.assign(mpfshost::kMaxSearchPatternLength + 1, std::byte {0x00});
    REQUIRE_FALSE(
        mpfshost::decodeMemorySearchRequest(mpfshost::encodeMemorySearchRequest({7, longerThanCap, 1, 0, 0, 1}))
            .has_value());

    auto mismatchedMask = term;
    mismatchedMask.mask.pop_back();
    REQUIRE_FALSE(
        mpfshost::decodeMemorySearchRequest(mpfshost::encodeMemorySearchRequest({7, mismatchedMask, 1, 0, 0, 1}))
            .has_value());

    REQUIRE_FALSE(mpfshost::decodeMemorySearchRequest(std::vector<std::byte> {}).has_value());
    REQUIRE_FALSE(mpfshost::decodeMemorySearchData(std::vector<std::byte> {}).has_value());
    REQUIRE_FALSE(mpfshost::decodeMemorySearchEnd(std::vector<std::byte> {}).has_value());
}

TEST_CASE("page read messages round trip", "[core][message]")
{
    const mpfshost::PageReadRequest request {
        4321, {{0x1000, 0x2000}, {0x9000, 4}}
    };
    const auto decodedRequest = mpfshost::decodePageReadRequest(mpfshost::encodePageReadRequest(request));
    REQUIRE(decodedRequest.has_value());
    REQUIRE(decodedRequest->pid == 4321);
    REQUIRE(decodedRequest->ranges.size() == 2);
    REQUIRE(decodedRequest->ranges.at(0).address == 0x1000);
    REQUIRE(decodedRequest->ranges.at(0).size == 0x2000);
    REQUIRE(decodedRequest->ranges.at(1).address == 0x9000);
    REQUIRE(decodedRequest->ranges.at(1).size == 4);

    mpfshost::PageReadData data;
    data.pages.push_back({0x1000, true, std::vector<std::byte>(mpfshost::kPageSize, std::byte {0xAA})});
    data.pages.push_back({0x2000, false, {}});
    const auto decoded = mpfshost::decodePageReadData(mpfshost::encodePageReadData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->pages.size() == 2);
    REQUIRE(decoded->pages.at(0).address == 0x1000);
    REQUIRE(decoded->pages.at(0).ok);
    REQUIRE(decoded->pages.at(0).data.size() == mpfshost::kPageSize);
    REQUIRE(decoded->pages.at(1).address == 0x2000);
    REQUIRE_FALSE(decoded->pages.at(1).ok);
    REQUIRE(decoded->pages.at(1).data.empty());

    const mpfshost::PageReadEnd end {1, 2, "a page could not be read"};
    const auto                  decodedEnd = mpfshost::decodePageReadEnd(mpfshost::encodePageReadEnd(end));
    REQUIRE(decodedEnd.has_value());
    REQUIRE(decodedEnd->status == 1);
    REQUIRE(decodedEnd->pageCount == 2);
    REQUIRE(decodedEnd->error == "a page could not be read");
}

TEST_CASE("prefetch messages round trip", "[core][message]")
{
    const mpfshost::PrefetchRequest request {7, {{0x1000, 0x3000}}};
    const auto decodedRequest = mpfshost::decodePrefetchRequest(mpfshost::encodePrefetchRequest(request));
    REQUIRE(decodedRequest.has_value());
    REQUIRE(decodedRequest->pid == 7);
    REQUIRE(decodedRequest->ranges.size() == 1);
    REQUIRE(decodedRequest->ranges.at(0).address == 0x1000);
    REQUIRE(decodedRequest->ranges.at(0).size == 0x3000);

    const auto decoded = mpfshost::decodePrefetchResponse(mpfshost::encodePrefetchResponse({3}));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->pageCount == 3);
}

TEST_CASE("config set messages round trip and expose only the allow-list", "[core][message]")
{
    const mpfshost::ConfigSetRequest request {mpfshost::ConfigOption::ProcessDtb, 4, 0x1aa000};
    const auto decodedRequest = mpfshost::decodeConfigSetRequest(mpfshost::encodeConfigSetRequest(request));
    REQUIRE(decodedRequest.has_value());
    CHECK(decodedRequest->option == mpfshost::ConfigOption::ProcessDtb);
    CHECK(decodedRequest->pid == 4u);
    CHECK(decodedRequest->value == 0x1aa000u);

    const auto decodedResponse = mpfshost::decodeConfigSetResponse(mpfshost::encodeConfigSetResponse({0x1aa000}));
    REQUIRE(decodedResponse.has_value());
    CHECK(decodedResponse->value == 0x1aa000u);

    CHECK(mpfshost::parseConfigOption("refresh-all") == mpfshost::ConfigOption::RefreshAll);
    CHECK(mpfshost::parseConfigOption("process-dtb") == mpfshost::ConfigOption::ProcessDtb);
    CHECK_FALSE(mpfshost::parseConfigOption("no-such-option").has_value());
    CHECK(mpfshost::configOptionName(mpfshost::ConfigOption::ReadCacheTicks) == "readcache-ticks");
    CHECK(mpfshost::configOptionUsesPid(mpfshost::ConfigOption::RefreshSpecificProcess));
    CHECK_FALSE(mpfshost::configOptionUsesPid(mpfshost::ConfigOption::RefreshAll));
    CHECK(mpfshost::configOptionRequiresValue(mpfshost::ConfigOption::TickPeriod));
    CHECK_FALSE(mpfshost::configOptionRequiresValue(mpfshost::ConfigOption::RefreshAll));

    mpfshost::ByteWriter unknownOption;
    unknownOption.writeU8(0xFE);
    unknownOption.writeU32(0);
    unknownOption.writeU64(0);
    REQUIRE_FALSE(mpfshost::decodeConfigSetRequest(unknownOption.bytes()).has_value());
}

TEST_CASE("malformed page and prefetch payloads are rejected", "[core][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodePageReadRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePageReadData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePageReadEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePrefetchRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodePrefetchResponse(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeConfigSetRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeConfigSetResponse(empty).has_value());

    auto truncatedRequest = mpfshost::encodePageReadRequest({7, {{0x1000, 0x1000}}});
    truncatedRequest.pop_back();
    REQUIRE_FALSE(mpfshost::decodePageReadRequest(truncatedRequest).has_value());

    mpfshost::ByteWriter tooManyRanges;
    tooManyRanges.writeU32(7);
    tooManyRanges.writeU32(static_cast<std::uint32_t>(mpfshost::kMaxPageCount + 1));
    REQUIRE_FALSE(mpfshost::decodePageReadRequest(tooManyRanges.bytes()).has_value());

    mpfshost::ByteWriter tooManyPages;
    tooManyPages.writeU32(static_cast<std::uint32_t>(mpfshost::kMaxPageCount + 1));
    REQUIRE_FALSE(mpfshost::decodePageReadData(tooManyPages.bytes()).has_value());

    auto trailingEnd = mpfshost::encodePageReadEnd({0, 0, {}});
    trailingEnd.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodePageReadEnd(trailingEnd).has_value());

    auto trailingPrefetch = mpfshost::encodePrefetchResponse({0});
    trailingPrefetch.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodePrefetchResponse(trailingPrefetch).has_value());

    auto trailingConfig = mpfshost::encodeConfigSetResponse({0});
    trailingConfig.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeConfigSetResponse(trailingConfig).has_value());
}

TEST_CASE("handle list payloads round trip", "[ipc][message]")
{
    const auto request = mpfshost::decodeHandleListRequest(mpfshost::encodeHandleListRequest({1234, "svc"}));
    REQUIRE(request.has_value());
    REQUIRE(request->pid == 1234);
    REQUIRE(request->filter == "svc");

    mpfshost::HandleListData   data;
    mpfshost::HandleDescriptor first;
    first.vaObject           = 0xffffa00000001000;
    first.handle             = 0x1c;
    first.grantedAccess      = 0x1f0003;
    first.typeIndex          = 7;
    first.handleCount        = 3;
    first.pointerCount       = 1;
    first.objectCreateInfo   = 0xffffa00000001000;
    first.securityDescriptor = 0xffffa00000002000;
    first.poolTag            = 0x466c7462;
    first.text               = "\\Device\\SomeObject";
    first.type               = "File";
    data.handles.push_back(first);
    data.handles.push_back({});

    const auto decodedData = mpfshost::decodeHandleListData(mpfshost::encodeHandleListData(data));
    REQUIRE(decodedData.has_value());
    REQUIRE(decodedData->handles.size() == 2);
    REQUIRE(decodedData->handles[0].vaObject == first.vaObject);
    REQUIRE(decodedData->handles[0].handle == 0x1c);
    REQUIRE(decodedData->handles[0].grantedAccess == 0x1f0003);
    REQUIRE(decodedData->handles[0].typeIndex == 7);
    REQUIRE(decodedData->handles[0].handleCount == 3);
    REQUIRE(decodedData->handles[0].pointerCount == 1);
    REQUIRE(decodedData->handles[0].poolTag == 0x466c7462);
    REQUIRE(decodedData->handles[0].text == "\\Device\\SomeObject");
    REQUIRE(decodedData->handles[0].type == "File");
    REQUIRE(decodedData->handles[1].text.empty());

    const auto end = mpfshost::decodeHandleListEnd(mpfshost::encodeHandleListEnd({0, 2, 1, ""}));
    REQUIRE(end.has_value());
    REQUIRE(end->handleCount == 2);
    REQUIRE(end->truncated == 1);
}

TEST_CASE("export list payloads round trip", "[ipc][message]")
{
    const auto request = mpfshost::decodeExportListRequest(mpfshost::encodeExportListRequest({4, "ntdll.dll"}));
    REQUIRE(request.has_value());
    REQUIRE(request->pid == 4);
    REQUIRE(request->module == "ntdll.dll");

    mpfshost::ExportListData data;
    data.exports.push_back({0x7ff800001000, 12, 12, 0, "NtCreateFile", ""});
    data.exports.push_back({0, 13, 13, 1, "Forwarded", "KERNEL32.Sleep"});

    const auto decoded = mpfshost::decodeExportListData(mpfshost::encodeExportListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->exports.size() == 2);
    REQUIRE(decoded->exports[0].vaFunction == 0x7ff800001000);
    REQUIRE(decoded->exports[0].ordinal == 12);
    REQUIRE(decoded->exports[0].functionsArrayOffset == 12);
    REQUIRE(decoded->exports[0].function == "NtCreateFile");
    REQUIRE(decoded->exports[0].forwardedFunction.empty());
    REQUIRE(decoded->exports[1].forwardedFunction == "KERNEL32.Sleep");

    const auto end = mpfshost::decodeExportListEnd(mpfshost::encodeExportListEnd({0, 2, 0, ""}));
    REQUIRE(end.has_value());
    REQUIRE(end->exportCount == 2);
    REQUIRE(end->truncated == 0);
}

TEST_CASE("import list payloads round trip", "[ipc][message]")
{
    const auto request = mpfshost::decodeImportListRequest(mpfshost::encodeImportListRequest({4, "kernel32.dll"}));
    REQUIRE(request.has_value());
    REQUIRE(request->module == "kernel32.dll");

    mpfshost::ImportListData data;
    data.imports.push_back({0x7ff800002000, true, 42, 0x3000, 0x2000, "Sleep", "kernel32.dll"});

    const auto decoded = mpfshost::decodeImportListData(mpfshost::encodeImportListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->imports.size() == 1);
    REQUIRE(decoded->imports[0].vaFunction == 0x7ff800002000);
    REQUIRE(decoded->imports[0].is32Bit);
    REQUIRE(decoded->imports[0].hint == 42);
    REQUIRE(decoded->imports[0].firstThunkRva == 0x3000);
    REQUIRE(decoded->imports[0].originalFirstThunkRva == 0x2000);
    REQUIRE(decoded->imports[0].function == "Sleep");
    REQUIRE(decoded->imports[0].module == "kernel32.dll");

    const auto end = mpfshost::decodeImportListEnd(mpfshost::encodeImportListEnd({0, 1, 0, ""}));
    REQUIRE(end.has_value());
    REQUIRE(end->importCount == 1);
}

TEST_CASE("unloaded module list payloads round trip", "[ipc][message]")
{
    const auto request =
        mpfshost::decodeUnloadedModuleListRequest(mpfshost::encodeUnloadedModuleListRequest({1000, "dll"}));
    REQUIRE(request.has_value());
    REQUIRE(request->pid == 1000);
    REQUIRE(request->filter == "dll");

    mpfshost::UnloadedModuleListData data;
    data.modules.push_back({0x7ff800010000, 0x12000, false, 0xabcdef, 0x12345678, 0, "gone.dll"});

    const auto decoded = mpfshost::decodeUnloadedModuleListData(mpfshost::encodeUnloadedModuleListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->modules.size() == 1);
    REQUIRE(decoded->modules[0].vaBase == 0x7ff800010000);
    REQUIRE(decoded->modules[0].imageSize == 0x12000);
    REQUIRE_FALSE(decoded->modules[0].isWow64);
    REQUIRE(decoded->modules[0].checksum == 0xabcdef);
    REQUIRE(decoded->modules[0].timeDateStamp == 0x12345678);
    REQUIRE(decoded->modules[0].text == "gone.dll");

    const auto end = mpfshost::decodeUnloadedModuleListEnd(mpfshost::encodeUnloadedModuleListEnd({0, 1, 0, ""}));
    REQUIRE(end.has_value());
    REQUIRE(end->moduleCount == 1);
}

TEST_CASE("service list payloads round trip", "[ipc][message]")
{
    const auto request = mpfshost::decodeServiceListRequest(mpfshost::encodeServiceListRequest({"spool"}));
    REQUIRE(request.has_value());
    REQUIRE(request->filter == "spool");

    mpfshost::ServiceDescriptor service;
    service.objectAddress           = 0xffff800012340000ull;
    service.ordinal                 = 7;
    service.startType               = 2;
    service.serviceType             = 0x20;
    service.state                   = 4;
    service.controlsAccepted        = 1;
    service.win32ExitCode           = 0x400;
    service.serviceSpecificExitCode = 0xbeef;
    service.checkPoint              = 3;
    service.waitHint                = 2000;
    service.pid                     = 4321;
    service.name                    = "DemoSvc";
    service.displayName             = "Démo Service";
    service.imagePath               = "C:\\Windows\\demo.sys";
    service.path                    = "\\Registry\\Machine\\System\\DemoSvc";
    service.userType                = "Driver";
    service.account                 = "LocalSystem";

    mpfshost::ServiceListData data;
    data.services.push_back(service);
    data.services.push_back({});

    const auto decoded = mpfshost::decodeServiceListData(mpfshost::encodeServiceListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->services.size() == 2);
    REQUIRE(decoded->services[0].objectAddress == 0xffff800012340000ull);
    REQUIRE(decoded->services[0].ordinal == 7);
    REQUIRE(decoded->services[0].startType == 2);
    REQUIRE(decoded->services[0].serviceType == 0x20);
    REQUIRE(decoded->services[0].state == 4);
    REQUIRE(decoded->services[0].controlsAccepted == 1);
    REQUIRE(decoded->services[0].win32ExitCode == 0x400);
    REQUIRE(decoded->services[0].serviceSpecificExitCode == 0xbeef);
    REQUIRE(decoded->services[0].checkPoint == 3);
    REQUIRE(decoded->services[0].waitHint == 2000);
    REQUIRE(decoded->services[0].pid == 4321);
    REQUIRE(decoded->services[0].name == "DemoSvc");
    REQUIRE(decoded->services[0].displayName == "Démo Service");
    REQUIRE(decoded->services[0].imagePath == "C:\\Windows\\demo.sys");
    REQUIRE(decoded->services[0].path == "\\Registry\\Machine\\System\\DemoSvc");
    REQUIRE(decoded->services[0].userType == "Driver");
    REQUIRE(decoded->services[0].account == "LocalSystem");
    REQUIRE(decoded->services[1].name.empty());

    const auto end = mpfshost::decodeServiceListEnd(mpfshost::encodeServiceListEnd({0, 2, 1, ""}));
    REQUIRE(end.has_value());
    REQUIRE(end->serviceCount == 2);
    REQUIRE(end->truncated == 1);
}

TEST_CASE("user list payloads round trip", "[ipc][message]")
{
    const auto request = mpfshost::decodeUserListRequest(mpfshost::encodeUserListRequest({"admin"}));
    REQUIRE(request.has_value());
    REQUIRE(request->filter == "admin");

    mpfshost::UserListData data;
    data.users.push_back({"Administrator", 0xffffa0001000ull, "S-1-5-21-1"});
    data.users.push_back({"Guest", 0, ""});

    const auto decoded = mpfshost::decodeUserListData(mpfshost::encodeUserListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->users.size() == 2);
    REQUIRE(decoded->users[0].name == "Administrator");
    REQUIRE(decoded->users[0].regHive == 0xffffa0001000ull);
    REQUIRE(decoded->users[0].sid == "S-1-5-21-1");
    REQUIRE(decoded->users[1].regHive == 0);
    REQUIRE(decoded->users[1].sid.empty());

    const auto end = mpfshost::decodeUserListEnd(mpfshost::encodeUserListEnd({0, 2, 0, ""}));
    REQUIRE(end.has_value());
    REQUIRE(end->userCount == 2);
    REQUIRE(end->truncated == 0);
}

TEST_CASE("network list payloads round trip", "[ipc][message]")
{
    const auto scoped = mpfshost::decodeNetworkListRequest(mpfshost::encodeNetworkListRequest({4321, "demo"}));
    REQUIRE(scoped.has_value());
    REQUIRE(scoped->pid == 4321);
    REQUIRE(scoped->filter == "demo");

    const auto wholeMachine = mpfshost::decodeNetworkListRequest(mpfshost::encodeNetworkListRequest({0, ""}));
    REQUIRE(wholeMachine.has_value());
    REQUIRE(wholeMachine->pid == 0);
    REQUIRE(wholeMachine->filter.empty());

    mpfshost::NetworkEntryDescriptor entry;
    entry.pid            = 4321;
    entry.process        = "demo.exe";
    entry.state          = 2;
    entry.addressFamily  = 2;
    entry.local.valid    = true;
    entry.local.address  = "10.0.0.5:445";
    entry.remote.valid   = false;
    entry.remote.address = "";
    entry.objectAddress  = 0xffff900000001000ull;
    entry.createdTime    = 0x01d0000000000000ull;
    entry.poolTag        = 0x546e6f43;

    mpfshost::NetworkListData data;
    data.entries.push_back(entry);

    const auto decoded = mpfshost::decodeNetworkListData(mpfshost::encodeNetworkListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->entries.size() == 1);
    REQUIRE(decoded->entries[0].pid == 4321);
    REQUIRE(decoded->entries[0].process == "demo.exe");
    REQUIRE(decoded->entries[0].state == 2);
    REQUIRE(decoded->entries[0].addressFamily == 2);
    REQUIRE(decoded->entries[0].local.valid);
    REQUIRE(decoded->entries[0].local.address == "10.0.0.5:445");
    REQUIRE_FALSE(decoded->entries[0].remote.valid);
    REQUIRE(decoded->entries[0].objectAddress == 0xffff900000001000ull);
    REQUIRE(decoded->entries[0].createdTime == 0x01d0000000000000ull);
    REQUIRE(decoded->entries[0].poolTag == 0x546e6f43);

    const auto end = mpfshost::decodeNetworkListEnd(mpfshost::encodeNetworkListEnd({0, 1, 0, ""}));
    REQUIRE(end.has_value());
    REQUIRE(end->entryCount == 1);
}

TEST_CASE("heap list payloads round trip", "[ipc][message]")
{
    const auto request = mpfshost::decodeHeapListRequest(mpfshost::encodeHeapListRequest({4321}));
    REQUIRE(request.has_value());
    REQUIRE(request->pid == 4321);

    mpfshost::HeapDescriptor heap;
    heap.address = 0x000001c000010000ull;
    heap.index   = 2;
    heap.number  = 7;
    heap.type    = 1;
    heap.is32Bit = 1;

    mpfshost::HeapListData data;
    data.heaps.push_back(heap);

    const auto decoded = mpfshost::decodeHeapListData(mpfshost::encodeHeapListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->heaps.size() == 1);
    REQUIRE(decoded->heaps[0].address == 0x000001c000010000ull);
    REQUIRE(decoded->heaps[0].index == 2);
    REQUIRE(decoded->heaps[0].number == 7);
    REQUIRE(decoded->heaps[0].type == 1);
    REQUIRE(decoded->heaps[0].is32Bit == 1);

    const auto empty = mpfshost::decodeHeapListData(mpfshost::encodeHeapListData({}));
    REQUIRE(empty.has_value());
    REQUIRE(empty->heaps.empty());

    const auto end = mpfshost::decodeHeapListEnd(mpfshost::encodeHeapListEnd({0, 1, 1, ""}));
    REQUIRE(end.has_value());
    REQUIRE(end->heapCount == 1);
    REQUIRE(end->truncated == 1);

    const auto failed =
        mpfshost::decodeHeapListEnd(mpfshost::encodeHeapListEnd({1, 0, 0, "process 4321 is not present"}));
    REQUIRE(failed.has_value());
    REQUIRE(failed->status == 1);
    REQUIRE(failed->error == "process 4321 is not present");
}

TEST_CASE("heap allocation list payloads round trip", "[ipc][message]")
{
    const auto request =
        mpfshost::decodeHeapAllocListRequest(mpfshost::encodeHeapAllocListRequest({4321, 0x000001c000010000ull}));
    REQUIRE(request.has_value());
    REQUIRE(request->pid == 4321);
    REQUIRE(request->heap == 0x000001c000010000ull);

    mpfshost::HeapAllocDescriptor allocation;
    allocation.address = 0x000001c000020000ull;
    allocation.size    = 0x1000;
    allocation.type    = 5;

    mpfshost::HeapAllocListData data;
    data.allocations.push_back(allocation);

    const auto decoded = mpfshost::decodeHeapAllocListData(mpfshost::encodeHeapAllocListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->allocations.size() == 1);
    REQUIRE(decoded->allocations[0].address == 0x000001c000020000ull);
    REQUIRE(decoded->allocations[0].size == 0x1000);
    REQUIRE(decoded->allocations[0].type == 5);

    const auto empty = mpfshost::decodeHeapAllocListData(mpfshost::encodeHeapAllocListData({}));
    REQUIRE(empty.has_value());
    REQUIRE(empty->allocations.empty());

    const auto end = mpfshost::decodeHeapAllocListEnd(mpfshost::encodeHeapAllocListEnd({0, 1, 0, ""}));
    REQUIRE(end.has_value());
    REQUIRE(end->allocationCount == 1);
    REQUIRE(end->truncated == 0);

    const auto failed =
        mpfshost::decodeHeapAllocListEnd(mpfshost::encodeHeapAllocListEnd({1, 0, 0, "heap allocation listing failed"}));
    REQUIRE(failed.has_value());
    REQUIRE(failed->status == 1);
    REQUIRE(failed->error == "heap allocation listing failed");
}

TEST_CASE("kernel driver list payloads round trip", "[ipc][message]")
{
    const auto filtered = mpfshost::decodeKDriverListRequest(mpfshost::encodeKDriverListRequest({"ntfs"}));
    REQUIRE(filtered.has_value());
    REQUIRE(filtered->filter == "ntfs");

    const auto unfiltered = mpfshost::decodeKDriverListRequest(mpfshost::encodeKDriverListRequest({}));
    REQUIRE(unfiltered.has_value());
    REQUIRE(unfiltered->filter.empty());

    mpfshost::KernelDriverDescriptor driver;
    driver.va             = 0xfffff80110000000ull;
    driver.vaDriverStart  = 0xfffff80110004000ull;
    driver.cbDriverSize   = 0x1a0000;
    driver.vaDeviceObject = 0xffffc10a00000000ull;
    driver.majorFunctions = {0, 0xfffff80110005000ull, 0, 0xfffff80110006000ull};
    driver.name           = "Ntfs";
    driver.path           = "\\SystemRoot\\System32\\drivers\\ntfs.sys";
    driver.serviceKeyName = "Ntfs";

    mpfshost::KDriverListData data;
    data.drivers.push_back(driver);

    const auto decoded = mpfshost::decodeKDriverListData(mpfshost::encodeKDriverListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->drivers.size() == 1);
    REQUIRE(decoded->drivers[0].va == 0xfffff80110000000ull);
    REQUIRE(decoded->drivers[0].vaDriverStart == 0xfffff80110004000ull);
    REQUIRE(decoded->drivers[0].cbDriverSize == 0x1a0000);
    REQUIRE(decoded->drivers[0].vaDeviceObject == 0xffffc10a00000000ull);
    REQUIRE(decoded->drivers[0].majorFunctions.size() == 4);
    REQUIRE(decoded->drivers[0].majorFunctions[0] == 0);
    REQUIRE(decoded->drivers[0].majorFunctions[3] == 0xfffff80110006000ull);
    REQUIRE(decoded->drivers[0].name == "Ntfs");
    REQUIRE(decoded->drivers[0].path == "\\SystemRoot\\System32\\drivers\\ntfs.sys");
    REQUIRE(decoded->drivers[0].serviceKeyName == "Ntfs");

    // An untouched driver ships no dispatch array at all.
    mpfshost::KDriverListData plain;
    plain.drivers.push_back({});
    const auto plainDecoded = mpfshost::decodeKDriverListData(mpfshost::encodeKDriverListData(plain));
    REQUIRE(plainDecoded.has_value());
    REQUIRE(plainDecoded->drivers[0].majorFunctions.empty());

    const auto end = mpfshost::decodeKDriverListEnd(mpfshost::encodeKDriverListEnd({0, 1, 1, ""}));
    REQUIRE(end.has_value());
    REQUIRE(end->driverCount == 1);
    REQUIRE(end->truncated == 1);
}

TEST_CASE("kernel device list payloads round trip", "[ipc][message]")
{
    const auto request = mpfshost::decodeKDeviceListRequest(mpfshost::encodeKDeviceListRequest({"disk"}));
    REQUIRE(request.has_value());
    REQUIRE(request->filter == "disk");

    mpfshost::KernelDeviceDescriptor device;
    device.va                 = 0xffffc10a00000000ull;
    device.depth              = 3;
    device.deviceType         = 0x00000007;
    device.vaDriverObject     = 0xfffff80110001000ull;
    device.vaAttachedDevice   = 0xffffc10a00001000ull;
    device.vaFileSystemDevice = 0xffffc10a00002000ull;
    device.deviceTypeName     = "FILE_DEVICE_DISK";
    device.volumeInfo         = "C:";

    mpfshost::KDeviceListData data;
    data.devices.push_back(device);

    const auto decoded = mpfshost::decodeKDeviceListData(mpfshost::encodeKDeviceListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->devices.size() == 1);
    REQUIRE(decoded->devices[0].va == 0xffffc10a00000000ull);
    REQUIRE(decoded->devices[0].depth == 3);
    REQUIRE(decoded->devices[0].deviceType == 0x00000007);
    REQUIRE(decoded->devices[0].vaDriverObject == 0xfffff80110001000ull);
    REQUIRE(decoded->devices[0].vaAttachedDevice == 0xffffc10a00001000ull);
    REQUIRE(decoded->devices[0].vaFileSystemDevice == 0xffffc10a00002000ull);
    REQUIRE(decoded->devices[0].deviceTypeName == "FILE_DEVICE_DISK");
    REQUIRE(decoded->devices[0].volumeInfo == "C:");

    const auto empty = mpfshost::decodeKDeviceListData(mpfshost::encodeKDeviceListData({}));
    REQUIRE(empty.has_value());
    REQUIRE(empty->devices.empty());

    const auto end = mpfshost::decodeKDeviceListEnd(mpfshost::encodeKDeviceListEnd({0, 1, 0, ""}));
    REQUIRE(end.has_value());
    REQUIRE(end->deviceCount == 1);
    REQUIRE(end->truncated == 0);
}

TEST_CASE("kernel object list payloads round trip at the child cap", "[ipc][message]")
{
    const auto request = mpfshost::decodeKObjectListRequest(mpfshost::encodeKObjectListRequest({"device"}));
    REQUIRE(request.has_value());
    REQUIRE(request->filter == "device");

    mpfshost::KernelObjectDescriptor object;
    object.va         = 0xffffc10a00003000ull;
    object.vaParent   = 0xffffc10a00000000ull;
    object.childCount = 1000;
    for (std::uint64_t i = 0; i < mpfshost::kMaxKernelObjectChildren; ++i)
    {
        object.children.push_back(0xffffc10a00100000ull + i * 0x10);
    }
    object.childrenTruncated = true;
    object.name              = "\\Device\\HarddiskVolume1";
    object.type              = "Device";

    mpfshost::KObjectListData data;
    data.objects.push_back(object);

    const auto decoded = mpfshost::decodeKObjectListData(mpfshost::encodeKObjectListData(data));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->objects.size() == 1);
    REQUIRE(decoded->objects[0].va == 0xffffc10a00003000ull);
    REQUIRE(decoded->objects[0].vaParent == 0xffffc10a00000000ull);
    REQUIRE(decoded->objects[0].childCount == 1000);
    REQUIRE(decoded->objects[0].children.size() == mpfshost::kMaxKernelObjectChildren);
    REQUIRE(decoded->objects[0].children[0] == 0xffffc10a00100000ull);
    REQUIRE(decoded->objects[0].childrenTruncated);
    REQUIRE(decoded->objects[0].name == "\\Device\\HarddiskVolume1");
    REQUIRE(decoded->objects[0].type == "Device");

    const auto empty = mpfshost::decodeKObjectListData(mpfshost::encodeKObjectListData({}));
    REQUIRE(empty.has_value());
    REQUIRE(empty->objects.empty());

    const auto end = mpfshost::decodeKObjectListEnd(mpfshost::encodeKObjectListEnd({0, 1, 1, ""}));
    REQUIRE(end.has_value());
    REQUIRE(end->objectCount == 1);
    REQUIRE(end->truncated == 1);
}

TEST_CASE("malformed map area payloads are rejected", "[ipc][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodeHandleListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeHandleListData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeHandleListEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeExportListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeExportListData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeExportListEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeImportListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeImportListData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeImportListEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeUnloadedModuleListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeUnloadedModuleListData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeUnloadedModuleListEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeServiceListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeServiceListData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeServiceListEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeUserListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeUserListData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeUserListEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeNetworkListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeNetworkListData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeNetworkListEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeHeapListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeHeapListData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeHeapListEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeHeapAllocListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeHeapAllocListData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeHeapAllocListEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeKDriverListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeKDriverListData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeKDriverListEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeKDeviceListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeKDeviceListData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeKDeviceListEnd(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeKObjectListRequest(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeKObjectListData(empty).has_value());
    REQUIRE_FALSE(mpfshost::decodeKObjectListEnd(empty).has_value());

    // A dispatch array longer than the 28 IRP slots and a child array past the per-object cap are both refused
    // instead of being allocated.
    mpfshost::ByteWriter oversizedDispatch;
    oversizedDispatch.writeU32(1);
    oversizedDispatch.writeU64(0);
    oversizedDispatch.writeU64(0);
    oversizedDispatch.writeU64(0);
    oversizedDispatch.writeU64(0);
    oversizedDispatch.writeU32(29);
    for (std::uint32_t i = 0; i < 29; ++i)
    {
        oversizedDispatch.writeU64(0);
    }
    oversizedDispatch.writeString("");
    oversizedDispatch.writeString("");
    oversizedDispatch.writeString("");
    REQUIRE_FALSE(mpfshost::decodeKDriverListData(oversizedDispatch.bytes()).has_value());

    mpfshost::ByteWriter oversizedChildren;
    oversizedChildren.writeU32(1);
    oversizedChildren.writeU64(0);
    oversizedChildren.writeU64(0);
    oversizedChildren.writeU64(0);
    oversizedChildren.writeU8(0);
    oversizedChildren.writeU32(static_cast<std::uint32_t>(mpfshost::kMaxKernelObjectChildren + 1));
    for (std::size_t i = 0; i <= mpfshost::kMaxKernelObjectChildren; ++i)
    {
        oversizedChildren.writeU64(0);
    }
    oversizedChildren.writeString("");
    oversizedChildren.writeString("");
    REQUIRE_FALSE(mpfshost::decodeKObjectListData(oversizedChildren.bytes()).has_value());

    auto trailingDeviceData = mpfshost::encodeKDeviceListData({});
    trailingDeviceData.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeKDeviceListData(trailingDeviceData).has_value());

    auto truncatedObjectData = mpfshost::encodeKObjectListData({});
    truncatedObjectData.pop_back();
    REQUIRE_FALSE(mpfshost::decodeKObjectListData(truncatedObjectData).has_value());

    auto truncatedHeapRequest = mpfshost::encodeHeapListRequest({4321});
    truncatedHeapRequest.resize(sizeof(std::uint16_t));
    REQUIRE_FALSE(mpfshost::decodeHeapListRequest(truncatedHeapRequest).has_value());

    mpfshost::HeapListData truncatedHeapSource;
    truncatedHeapSource.heaps.push_back({});
    auto truncatedHeapData = mpfshost::encodeHeapListData(truncatedHeapSource);
    truncatedHeapData.pop_back();
    REQUIRE_FALSE(mpfshost::decodeHeapListData(truncatedHeapData).has_value());

    auto trailingHeapAllocData = mpfshost::encodeHeapAllocListData({});
    trailingHeapAllocData.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeHeapAllocListData(trailingHeapAllocData).has_value());

    // A connection request without its pid field and a data frame cut mid-entry must both be refused.
    auto truncatedNetworkRequest = mpfshost::encodeNetworkListRequest({0, ""});
    truncatedNetworkRequest.resize(sizeof(std::uint16_t));
    REQUIRE_FALSE(mpfshost::decodeNetworkListRequest(truncatedNetworkRequest).has_value());

    mpfshost::NetworkEntryDescriptor truncatedEntry;
    truncatedEntry.pid     = 4321;
    truncatedEntry.process = "demo.exe";
    mpfshost::NetworkListData truncatedSource;
    truncatedSource.entries.push_back(truncatedEntry);
    auto truncatedNetworkData = mpfshost::encodeNetworkListData(truncatedSource);
    truncatedNetworkData.pop_back();
    REQUIRE_FALSE(mpfshost::decodeNetworkListData(truncatedNetworkData).has_value());

    auto trailingServiceData = mpfshost::encodeServiceListData({});
    trailingServiceData.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeServiceListData(trailingServiceData).has_value());

    auto truncatedHandleData = mpfshost::encodeHandleListData({});
    truncatedHandleData.pop_back();
    REQUIRE_FALSE(mpfshost::decodeHandleListData(truncatedHandleData).has_value());

    mpfshost::ByteWriter bogusCount;
    bogusCount.writeU32(2);
    bogusCount.writeU32(0);
    REQUIRE_FALSE(mpfshost::decodeHandleListData(bogusCount.bytes()).has_value());

    auto trailingEnd = mpfshost::encodeHandleListEnd({0, 0, 0, {}});
    trailingEnd.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeHandleListEnd(trailingEnd).has_value());
}

TEST_CASE("capabilities response round trips", "[ipc][message]")
{
    const mpfshost::CapabilitiesResponse empty {"1.2.3", {}};
    const auto emptyDecoded = mpfshost::decodeCapabilitiesResponse(mpfshost::encodeCapabilitiesResponse(empty));
    REQUIRE(emptyDecoded.has_value());
    REQUIRE(emptyDecoded->serverVersion == "1.2.3");
    REQUIRE(emptyDecoded->requestTypes.empty());

    mpfshost::CapabilitiesResponse response {
        "0.1.0", {1, 0x002C, 0xFFFF}
    };
    const auto decoded = mpfshost::decodeCapabilitiesResponse(mpfshost::encodeCapabilitiesResponse(response));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->serverVersion == "0.1.0");
    REQUIRE(decoded->requestTypes == response.requestTypes);
}

TEST_CASE("malformed capabilities payloads are rejected", "[ipc][message]")
{
    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodeCapabilitiesResponse(empty).has_value());

    mpfshost::ByteWriter bogusCount;
    bogusCount.writeString("1");
    bogusCount.writeU32(0xFFFFFFFF);
    REQUIRE_FALSE(mpfshost::decodeCapabilitiesResponse(bogusCount.bytes()).has_value());

    auto trailing = mpfshost::encodeCapabilitiesResponse({"1", {1}});
    trailing.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeCapabilitiesResponse(trailing).has_value());
}

TEST_CASE("input event requests round trip", "[ipc][message][input]")
{
    mpfshost::InputEventRequest request;

    mpfshost::InputEvent key;
    key.kind = mpfshost::InputEventKind::Key;
    key.key  = "a";
    key.down = true;
    request.events.push_back(key);

    mpfshost::InputEvent move;
    move.kind     = mpfshost::InputEventKind::PointerMove;
    move.absolute = false;
    move.x        = -12;
    move.y        = 34;
    request.events.push_back(move);

    mpfshost::InputEvent button;
    button.kind   = mpfshost::InputEventKind::PointerButton;
    button.button = mpfshost::InputMouseButton::Right;
    button.down   = false;
    request.events.push_back(button);

    const auto decoded = mpfshost::decodeInputEventRequest(mpfshost::encodeInputEventRequest(request));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->events.size() == 3);
    REQUIRE(decoded->events[0].key == "a");
    REQUIRE(decoded->events[0].down);
    REQUIRE_FALSE(decoded->events[1].absolute);
    REQUIRE(decoded->events[1].x == -12);
    REQUIRE(decoded->events[1].y == 34);
    REQUIRE(decoded->events[2].button == mpfshost::InputMouseButton::Right);
    REQUIRE_FALSE(decoded->events[2].down);
}

TEST_CASE("malformed input event requests are rejected", "[ipc][message][input]")
{
    REQUIRE_FALSE(mpfshost::decodeInputEventRequest({}).has_value());

    mpfshost::ByteWriter unknownKind;
    unknownKind.writeU32(1);
    unknownKind.writeU8(9);
    REQUIRE_FALSE(mpfshost::decodeInputEventRequest(unknownKind.bytes()).has_value());

    mpfshost::ByteWriter badKey;
    badKey.writeU32(1);
    badKey.writeU8(0);
    badKey.writeU8(1);
    badKey.writeString("A B");
    REQUIRE_FALSE(mpfshost::decodeInputEventRequest(badKey.bytes()).has_value());

    mpfshost::ByteWriter absoluteOutOfRange;
    absoluteOutOfRange.writeU32(1);
    absoluteOutOfRange.writeU8(1);
    absoluteOutOfRange.writeU8(1);
    absoluteOutOfRange.writeU32(0x8000);
    absoluteOutOfRange.writeU32(0);
    REQUIRE_FALSE(mpfshost::decodeInputEventRequest(absoluteOutOfRange.bytes()).has_value());

    auto trailing = mpfshost::encodeInputEventRequest({});
    trailing.push_back(std::byte {0});
    REQUIRE_FALSE(mpfshost::decodeInputEventRequest(trailing).has_value());
}

TEST_CASE("input event responses round trip", "[ipc][message][input]")
{
    const auto decoded = mpfshost::decodeInputEventResponse(mpfshost::encodeInputEventResponse({7}));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->eventsSent == 7);

    const std::vector<std::byte> empty;
    REQUIRE_FALSE(mpfshost::decodeInputEventResponse(empty).has_value());
}
