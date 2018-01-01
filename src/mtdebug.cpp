#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS//for fopen()
#endif
#include <stdio.h>

#include "mtdebug.h"

namespace ithare {
	namespace mtprimitives {

		uint64_t mtDbgLogBuf[mtDbgLogBufSize] = {};
		std::atomic<size_t> mtDbgLogBufOffset = { 0 };

#ifdef _MSC_VER
		__declspec(noinline)
#endif
			void mtPrintDbgLog(int lastN) {
				assert(lastN < mtDbgLogBufSize);
				FILE* f = fopen("dbgLog.txt", "wt");
				for (size_t i = mtDbgLogBufOffset + mtDbgLogBufSize; i >= mtDbgLogBufOffset + mtDbgLogBufSize - lastN; --i) {
					size_t realOffset = i & 0x3FF;
					uint64_t entry = mtDbgLogBuf[realOffset];
					fprintf(f, "%02x: %8x\n", uint32_t(entry >> 32), uint32_t(entry));
				}
				fclose(f);
			}

	}//namespace mtprimitives
}//namespace ithare
