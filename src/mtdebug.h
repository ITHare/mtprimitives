#ifndef ithare_mtprimitives_mtdebug_h_included
#define ithare_mtprimitives_mtdebug_h_included

#include <exception>
#include <atomic>
#include "mtcommon.h"

#ifdef ITHARE_MTPRIMITIVES_INTERNAL_DBG
//my personal debugging preferences
#ifdef NDEBUG
#define assert(expr)
#else
namespace ithare {
	namespace mtprimitives {
		inline void my_assert_fail() {//to put a breakpoint
			throw std::exception();
		}
	}
}

#define assert(expr) \
    if (!(expr)) my_assert_fail();
#endif//NDEBUG

#else
#include <assert.h>
#endif

namespace ithare {
	namespace mtprimitives {
		constexpr size_t mtDbgLogBufSize = 1024;
		static_assert(mt_is_powerof2(mtDbgLogBufSize),"");
		constexpr size_t mtDbgLogBufSizeMask = mtDbgLogBufSize - 1;

		//dbgLogBuf: ultra-fast logging not-too-likely to cause too significant pattern changes
		extern uint64_t mtDbgLogBuf[1024];
		extern std::atomic<size_t> mtDbgLogBufOffset;

		inline void mtDbgLog(uint32_t id, uint64_t param) {
			uint64_t entry = (uint64_t(id) << 32) | uint32_t(param);
			size_t offset = mtDbgLogBufOffset++ & mtDbgLogBufSizeMask;
			mtDbgLogBuf[offset] = entry;
		}

		void mtPrintDbgLog(int lastN);
		//at least in MSVC debugger, printDbgLog() CAN be used in Watch window, for example as ithare::mtprimitives::dbgLog(100); 
		//  will create file dbgLog.txt with a printout
		//  To use it in this manner in Release mode, MAKE SURE to call mtPrintDbgLog() somewhere - to prevent linker from eliminating it
	}
}

#endif//ithare_mtprimitives_mtdebug_h_included
