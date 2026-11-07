#include "host/vmm_search.hpp"

#include "core/server/request_deadline.hpp"

#include <algorithm>
#include <limits>

namespace mpfshost
{
    namespace
    {
        constexpr std::uint64_t kPageOffsetMask = kPageSize - 1;

        struct Collector
        {
            std::vector<std::uint64_t>* hits {};
            std::size_t                 maxResults {};
            bool                        truncated {};
            bool                        aborted {};
        };

        // MemProcFS stops the search as soon as this returns FALSE, so the request budget can only be honoured
        // here. Returning at the cap is what makes `truncated` distinguishable from a finished window, but
        // MemProcFS reports that early stop as a failed call, so the caller keeps the hits collected so far.
        BOOL collectHit(VMMDLL_MEM_SEARCH_CONTEXT* ctx, QWORD va, DWORD /*iSearch*/)
        {
            auto* collector = static_cast<Collector*>(ctx->pvUserPtrOpt);
            if (collector == nullptr)
            {
                return 0;
            }
            if (requestExpired())
            {
                ctx->fAbortRequested = 1;
                collector->aborted   = true;
                return 0;
            }

            collector->hits->push_back(static_cast<std::uint64_t>(va));
            if (collector->hits->size() >= collector->maxResults)
            {
                collector->truncated = true;
                return 0;
            }
            return 1;
        }
    } // namespace

    bool searchVmmMemory(VMM_HANDLE                  handle,
                         const MemorySearchRequest&  request,
                         std::vector<std::uint64_t>& hits,
                         bool&                       truncated,
                         std::string&                error)
    {
        if (handle == nullptr || request.pid == 0 || request.term.pattern.empty()
            || request.term.pattern.size() > kMaxSearchPatternLength)
        {
            error = "invalid search parameters";
            return false;
        }
        if (request.maxResults == 0 || request.maxResults > kMaxSearchHits)
        {
            error = "search result count is out of range";
            return false;
        }
        if (requestExpired())
        {
            error = "request deadline exceeded";
            return false;
        }

        // MemProcFS wants a page-aligned window: the minimum rounds down so a match inside a partly selected
        // page is still found, the maximum rounds up so the last page is not cut short.
        const auto    vaMin = request.minAddress & ~kPageOffsetMask;
        std::uint64_t vaMax = 0;
        if (request.maxAddress != 0)
        {
            if (request.maxAddress > std::numeric_limits<std::uint64_t>::max() - kPageOffsetMask)
            {
                vaMax = 0;
            }
            else
            {
                vaMax = (request.maxAddress + kPageOffsetMask) & ~kPageOffsetMask;
            }
            if (vaMax <= vaMin)
            {
                error = "search window is empty";
                return false;
            }
        }

        VMMDLL_MEM_SEARCH_CONTEXT_SEARCHENTRY entry {};
        entry.cbAlign = static_cast<DWORD>(request.alignBytes);
        entry.cb      = static_cast<DWORD>(request.term.pattern.size());
        for (std::size_t index = 0; index < request.term.pattern.size(); ++index)
        {
            entry.pb[index]         = std::to_integer<BYTE>(request.term.pattern[index]);
            entry.pbSkipMask[index] = std::to_integer<BYTE>(request.term.mask[index]);
        }

        hits.clear();
        truncated = false;

        Collector collector {&hits, request.maxResults};

        VMMDLL_MEM_SEARCH_CONTEXT context {};
        context.dwVersion      = VMMDLL_MEM_SEARCH_VERSION;
        context.cMaxResult     = static_cast<DWORD>(request.maxResults);
        context.cSearch        = 1;
        context.pSearch        = &entry;
        context.vaMin          = vaMin;
        context.vaMax          = vaMax;
        context.pvUserPtrOpt   = &collector;
        context.pfnResultOptCB = &collectHit;

        const auto success = VMMDLL_MemSearch(handle, request.pid, &context, nullptr, nullptr) != 0;
        if (collector.aborted)
        {
            hits.clear();
            error = "request deadline exceeded";
            return false;
        }
        if (!success && !collector.truncated)
        {
            hits.clear();
            error = "VMMDLL_MemSearch failed";
            return false;
        }

        std::sort(hits.begin(), hits.end());
        // The window MemProcFS scans is page-aligned, so it also sees matches that start just outside the one the
        // client asked for. Keep only those whose start address is inside the requested window.
        const auto windowStart = request.minAddress;
        const auto windowEnd   = request.maxAddress;
        std::erase_if(hits,
                      [windowStart, windowEnd](std::uint64_t hit)
                      {
                          return hit < windowStart || (windowEnd != 0 && hit >= windowEnd);
                      });
        truncated = collector.truncated;
        return true;
    }
} // namespace mpfshost
