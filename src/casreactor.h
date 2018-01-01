#ifndef ithare_mtprimitives_casreactor_h_included
#define ithare_mtprimitives_casreactor_h_included

namespace ithare {
	namespace mtprimitives {

		constexpr size_t CAS_SIZE = 16;//x64 starting from Core Duo

		struct CAS_DATA {
			uint64_t lo;
			uint64_t hi;
#ifdef ITHARE_MTPRIMITIVES_CAS_DUMMY
			bool dummy;
#endif
		};

		class CAS {
			//wrapper to simplify manual rewriting if it becomes necessary
		private:
			std::atomic<CAS_DATA> cas;

		public:
			CAS() {
				CAS_DATA data;
				memset(&data, 0, sizeof(CAS_DATA));
				cas = data;
			}
			CAS(CAS_DATA data) {
				cas = data;
			}
			CAS_DATA load() {
				return cas.load();
			}
			bool compare_exchange_weak(CAS_DATA* expected, CAS_DATA desired) {
				return cas.compare_exchange_weak(*expected, desired);
			}
			bool is_lock_free() const {//if it happens to be NOT lock free but the platform does support CAS of CAS_SIZE - 
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

		std::atomic<size_t> dbgCasOkCount = { 0 };
		std::atomic<size_t> dbgCasRetryCount = { 0 };

		template<class ReactorData>
		class CasReactorHandle {
		protected:
			CAS * cas;
			ReactorData last_read;

			CasReactorHandle(CAS& cas_)
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
						++dbgCasOkCount;
						last_read.data = new_data.data;
						return;//effectively returning ret
					}
					++dbgCasRetryCount;
				}
			}
			template<class ReturnType, class Func>
			void react_of_uint64_t(ReturnType& ret, uint64_t param, Func&&f) {//ugly; wish I could derive both ReturnType and param types from Func() itself
				while (true) {
					ReactorData new_data = last_read;
					ret = f(new_data, param);//MODIFIES new_data(!)

					bool ok = cas->compare_exchange_weak(&last_read.data, new_data.data);
					if (ok) {
						++dbgCasOkCount;
						last_read.data = new_data.data;
						return;//effectively returning ret
					}
					++dbgCasRetryCount;
				}
			}
		};

	}//namespace mtprimitives
}//namespace ithare

#endif

