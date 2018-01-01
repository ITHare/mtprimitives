#include "casreactor.h"

#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
namespace ithare {
	namespace mtprimitives {
		std::atomic<size_t> mtDbgCasOkCount = { 0 };
		std::atomic<size_t> mtDbgCasRetryCount = { 0 };
	}
}
#endif