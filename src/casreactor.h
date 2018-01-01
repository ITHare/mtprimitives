#ifndef ithare_mtprimitives_casreactor_h_included
#define ithare_mtprimitives_casreactor_h_included

#include <atomic>
#include "mtcommon.h"

namespace ithare {
	namespace mtprimitives {

		constexpr size_t MT_CAS_SIZE = 16;//x64 starting from Core Duo

		struct MT_CAS_DATA {
			uint64_t lo;
			uint64_t hi;
#ifdef ITHARE_MTPRIMITIVES_CAS_DUMMY
			bool dummy;
#endif
		};

		class MT_CAS {
			//wrapper to simplify manual rewriting if it becomes necessary
		private:
			std::atomic<MT_CAS_DATA> cas;

		public:
			MT_CAS() {
				MT_CAS_DATA data;
				memset(&data, 0, sizeof(MT_CAS_DATA));
				cas = data;
			}
			MT_CAS(MT_CAS_DATA data) {
				cas = data;
			}
			MT_CAS_DATA load() {
				return cas.load();
			}
			bool compare_exchange_weak(MT_CAS_DATA* expected, MT_CAS_DATA desired) {
				return cas.compare_exchange_weak(*expected, desired);
			}
			bool is_lock_free() const {//if it happens to be NOT lock free but the platform does support MT_CAS of MT_CAS_SIZE - 
									   //	we'll have to use platform-specific stuff 
#ifndef ITHARE_MTPRIMITIVES_CAS_DUMMY
				return cas.is_lock_free();
#else
									   //for whatever reason, with larger sizes above line causes linker error under Clang
				return false;
#endif
			}
		};

		// Generic CasReactorHandle

#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
		extern std::atomic<size_t> mtDbgCasOkCount;
		extern std::atomic<size_t> mtDbgCasRetryCount;
#endif

		template<class ReactorData>
		class CasReactorHandle {
		protected:
			MT_CAS* cas;
			ReactorData last_read;

			CasReactorHandle(MT_CAS& cas_)
				: cas(&cas_) {
				last_read.data = cas->load();
			}
			template<class ReturnType, class Func>
			void react_of_void(ReturnType& ret, Func&&f) {//ugly; wish I could derive both ReturnType and param types from Func() itself
														  //last_read.data = cas->load();
				while (true) {
					ReactorData new_data = last_read;
					ret = f(new_data);//MODIFIES new_data(!)

					bool ok = cas->compare_exchange_weak(&last_read.data, new_data.data);
					if (ok) {
#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
						++mtDbgCasOkCount;
#endif
						last_read.data = new_data.data;
						return;//effectively returning ret
					}
#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
					++mtDbgCasRetryCount;
#endif
				}
			}
			template<class ReturnType, class Func>
			void react_of_uint64_t(ReturnType& ret, uint64_t param, Func&&f) {//ugly; wish I could derive both ReturnType and param types from Func() itself
				while (true) {
					ReactorData new_data = last_read;
					ret = f(new_data, param);//MODIFIES new_data(!)

					bool ok = cas->compare_exchange_weak(&last_read.data, new_data.data);
					if (ok) {
						++mtDbgCasOkCount;
						last_read.data = new_data.data;
						return;//effectively returning ret
					}
#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
					++mtDbgCasRetryCount;
#endif
				}
			}
		};

	}//namespace mtprimitives
}//namespace ithare

#endif

