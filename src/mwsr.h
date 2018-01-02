#ifndef ithare_mtprimitives_queues_h_included
#define ithare_mtprimitives_queues_h_included

#include <inttypes.h>
#include <utility>
#include <atomic>
#include <condition_variable>
#include <set>//dbg-only

#include "mtcommon.h"
#include "mtdebug.h"
#include "casreactor.h"

#define ITHARE_MTPRIMITIVES_GENERIC_CAS_HANDLE
//  Jury is still out whether generic approach is better 
//    performance-wise they SEEM to be equal at least under MSVC, and readability-wise I'm _leaning_ towards ITHARE_MTPRIMITIVES_GENERIC_CAS_HANDLE, 
//    but there are still potential issues which can reverse it.

namespace ithare {
	namespace mtprimitives {

		namespace MWSRQueueFC_helpers {
			constexpr size_t QueueSize = 64;

			//helpers for MWSRQueueFC
			//*** mask_*() functions need to represent a coherent view, but nobody really cares about exact bit numbers outside of them ***//
			bool mask_getbit(uint64_t mask, int pos) {
				assert(pos >= 0);
				assert(pos < 64);
				return (mask & (uint64_t(1) << pos)) != 0;
			}

			uint64_t mask_setbit(uint64_t mask, int pos) {
				assert(pos >= 0);
				assert(pos < 64);
				return mask | (uint64_t(1) << pos);
			}

			/*bool mask_resetbit(uint64_t mask, int pos) {
				assert(pos >= 0);
				assert(pos < 64);
				return mask & ~(uint64_t(1) << pos);
			}*/

			uint64_t mask_shiftoutbit0(uint64_t mask) {
				return mask >> 1;
			}

			//*** EntranceReactor and ExitReactor ***//

			struct EntranceReactorData {//according to "CAS Reactor" doctrine, EntranceReactorData is THREAD-AGNOSTIC!
										//  All the thread sync belongs to EntranceReactorHandle
			private:
				//kinda-fields:
				//  firstIDToWrite (up to 64 bits, current implementation: 64 bits)
				//  lastIDToWrite (up to 64 bits, current implementation: signed 32-bit offset to firstIDToWrite)
				//  lockedThreadCount (up to 32 bits, current implementation: 32 bits)
				alignas(MT_CAS_SIZE)MT_CAS_DATA data;

			public:
				EntranceReactorData() {
				}
				EntranceReactorData(uint64_t firstToWrite, uint64_t lastToWrite) {
					memset(this, 0, sizeof(*this));
					setIDsToWrite(firstToWrite, lastToWrite);
				}
				MT_CAS_DATA getCasData() const {
					return data;
				}

				friend class EntranceReactorHandle;
				friend class CasReactorHandle<EntranceReactorData>;
			private:
				uint64_t getFirstIDToWrite() const {
					return data.lo;
				}
				void setFirstIDToWrite(uint64_t val) {
					uint32_t lc = getLockedThreadCount();
					uint64_t last = getLastIDToWrite();
					data.lo = val;
					int64_t offset = last - val;
					assert((int32_t)offset == offset);
					uint64_t oldHi = data.hi;
					data.hi = (data.hi & 0xFFFFFFFF'00000000LL) | uint32_t(int32_t(offset));
					assert(getFirstIDToWrite() == val);
					assert(getLastIDToWrite() == last);
					assert(lc == getLockedThreadCount());
				}
				uint64_t getLastIDToWrite() const {
					return data.lo + int32_t(data.hi & 0xFFFFFFFFLL);
				}
				void setLastIDToWrite(uint64_t val) {
					uint32_t lc = getLockedThreadCount();
					uint64_t first = getFirstIDToWrite();
					int64_t offset = val - data.lo;
					assert((int32_t)offset == offset);
					data.hi = (data.hi & 0xFFFFFFFF'00000000LL) | uint32_t(int32_t(offset));
					assert(getLastIDToWrite() == val);
					assert(getFirstIDToWrite() == first);
					assert(lc == getLockedThreadCount());
				}
				void setIDsToWrite(uint64_t first, uint64_t last) {
					uint32_t lc = getLockedThreadCount();
					data.lo = first;
					int64_t offset = last - first;
					assert((int32_t)offset == offset);
					data.hi = (data.hi & 0xFFFFFFFF'00000000LL) | uint32_t(int32_t(offset));
					assert(getLastIDToWrite() == last);
					assert(getFirstIDToWrite() == first);
					assert(lc == getLockedThreadCount());
				}
				uint32_t getLockedThreadCount() const {
					return data.hi >> 32;
				}
				void setLockedThreadCount(uint32_t val) {
					data.hi = (data.hi & 0xFFFFFFFFLL) | ((uint64_t)val << 32);
				}
			};

#ifndef ITHARE_MTPRIMITIVES_CAS_DUMMY
			static_assert(sizeof(EntranceReactorData) == MT_CAS_SIZE, "size of ReactorData MUST match CAS_SIZE");
#endif

			class EntranceReactorHandle : public CasReactorHandle<EntranceReactorData> {
				//private:
				//CAS* cas;
				//EntranceReactorData last_read;

			public:
				EntranceReactorHandle(MT_CAS& cas_)
					: CasReactorHandle<EntranceReactorData>(cas_) {
				}
#ifdef ITHARE_MTPRIMITIVES_GENERIC_CAS_HANDLE
				std::pair<bool, uint64_t> allocateNextID() {
					std::pair<bool, uint64_t> ret;
					react_of_void(ret, [](bool& earlyExit, EntranceReactorData& new_data) -> std::pair<bool, uint64_t> {
						uint64_t firstW = new_data.getFirstIDToWrite();
						//MAY be >= lastIDToWrite()

						//regardless of ID being available, we'll try to increment 
						//	(but if our ID is not available for processing yet - we'll ask to lock)
						uint64_t newW = firstW + 1;
						assert(newW > firstW);//overflow check
						new_data.setFirstIDToWrite(newW);

						bool willLock = false;
						if (newW >= new_data.getLastIDToWrite()) {
							willLock = true;
							uint32_t lockedCount = new_data.getLockedThreadCount();
							//mtDbgLog(0x101, lockedCount);
							uint32_t newLockedCount = lockedCount + 1;
							assert(newLockedCount > lockedCount);//overflow check
							new_data.setLockedThreadCount(newLockedCount);
						}//if(newW >= ...)

						return std::pair<bool, uint64_t>(willLock, firstW);
					});
					return ret;
				}
				void unlock() {
					int dummy;
					react_of_void(dummy, [](bool& earlyExit,EntranceReactorData& new_data) {
						uint32_t lockedCount = new_data.getLockedThreadCount();
						uint32_t newLockedCount = lockedCount - 1;
						//mtDbgLog(0x111, lockedCount);
						assert(newLockedCount < lockedCount);//underflow check
						new_data.setLockedThreadCount(newLockedCount);
						return 0;//dummy
					});
				}
				bool moveLastToWrite(uint64_t newLastW) {
					//returns 'shouldUnlock'
					bool ret;
					react_of_uint64_t(ret, newLastW, [](bool& earlyExit,EntranceReactorData& new_data, uint64_t newLastW) -> bool {
						uint64_t lastW = new_data.getLastIDToWrite();
						assert(lastW <= newLastW);

						uint32_t lockedCount = new_data.getLockedThreadCount();

						new_data.setLastIDToWrite(newLastW);
						return lockedCount > 0;
					});
					return ret;
				}
#else
				std::pair<bool, uint64_t> allocateNextID() {
					//returns tuple (shouldLock,newID); newID is returned regardless of shouldLock
					while (true) {
						EntranceReactorData new_data = last_read;
						uint64_t firstW = new_data.getFirstIDToWrite();
						//MAY be >= lastIDToWrite()

						//regardless of ID being available, we'll try to increment 
						//	(but if our ID is not available for processing yet - we'll ask to lock)
						uint64_t newW = firstW + 1;
						assert(newW > firstW);//overflow check
						new_data.setFirstIDToWrite(newW);

						bool willLock = false;
						if (newW >= new_data.getLastIDToWrite()) {
							willLock = true;
							uint32_t lockedCount = new_data.getLockedThreadCount();
							//mtDbgLog(0x101, lockedCount);
							uint32_t newLockedCount = lockedCount + 1;
							assert(newLockedCount > lockedCount);//overflow check
							new_data.setLockedThreadCount(newLockedCount);
						}//if(newW >= ...)

						bool ok = cas->compare_exchange_weak(&last_read.data, new_data.data);
						if (ok) {
#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
							++mtDbgCasOkCount;
#endif
							last_read.data = new_data.data;
							return std::pair<bool, uint64_t>(willLock, firstW);
						}
						//else
						//	continue;
#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
						++mtDbgCasRetryCount;
#endif
						//if(willLock)
						//	mtDbgLog(0x102, 0);
					}//while(true)
				}//allocateNextID()
				void unlock() {
					while (true) {
						EntranceReactorData new_data = last_read;
						uint32_t lockedCount = new_data.getLockedThreadCount();
						uint32_t newLockedCount = lockedCount - 1;
						//mtDbgLog(0x111, lockedCount);
						assert(newLockedCount < lockedCount);//underflow check
						new_data.setLockedThreadCount(newLockedCount);
						bool ok = cas->compare_exchange_weak(&last_read.data, new_data.data);
						if (ok) {
#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
							++mtDbgCasOkCount;
#endif
							last_read.data = new_data.data;
							return;
						}
						//else
						//	continue;
#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
						++mtDbgCasRetryCount;
#endif
						//mtDbgLog(0x112, 0);
					}
				}
				bool moveLastToWrite(uint64_t newLastW) {
					//returns 'shouldUnlock'
					while (true) {
						EntranceReactorData new_data = last_read;
						uint64_t lastW = new_data.getLastIDToWrite();
						assert(lastW <= newLastW);

						uint32_t lockedCount = new_data.getLockedThreadCount();

						new_data.setLastIDToWrite(newLastW);
						bool ok = cas->compare_exchange_weak(&last_read.data, new_data.data);
						if (ok) {
#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
							++mtDbgCasOkCount;
#endif
							last_read.data = new_data.data;
							return lockedCount > 0;
						}
						//else
						//	continue;
#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
						++mtDbgCasRetryCount;
#endif
					}//while(true)
				}
#endif
			};

			class ExitReactorHandle;

			class ExitReactorData {//according to "CAS Reactor" doctrine, ExitReactorData is THREAD-AGNOSTIC!
								   //  All the thread sync belongs to ExitReactorHandle
			private:
				alignas(MT_CAS_SIZE)MT_CAS_DATA data;
				//kinda-fields:
				//  firstIDToRead (up to 64 bits, current implementation: 63 bits)
				//  completedWritesMask (bitmask, up to 64 bits, current implementation: 64 bits)
				//  readerIsLocked (bool, current implementation: 1 bit)

			public:
				static_assert(QueueSize <= 64, "QueueSize cannot exceed number of bits in mask");
				const static int EntranceFirstToWrite = 0;
				constexpr static int EntranceLastToWrite = QueueSize;

			public:
				ExitReactorData() {
					memset(this, 0, sizeof(*this));
					setFirstIDToRead(EntranceFirstToWrite);
				}

				friend class ExitReactorHandle;
				friend class CasReactorHandle<ExitReactorData>;
			private:
				uint64_t getFirstIDToRead() {
					return data.hi & 0x7FFF'FFFFLL;
				}
				void setFirstIDToRead(uint64_t val) {
					assert((val & 0x8000'0000LL) == 0);
					data.hi = (data.hi & 0x8000'0000LL) | val;
				}
				uint64_t getCompletedWritesMask() {
					return data.lo;
				}
				void setCompletedWritesMask(uint64_t val) {
					data.lo = val;
				}
				bool getReaderIsLocked() {
					return (data.hi & 0x8000'0000LL) != 0;
				}
				void setReaderIsLocked() {
					data.hi |= 0x8000'0000LL;
				}
				void setReaderIsUnlocked() {
					data.hi &= ~0x8000'0000LL;
				}
			};

			class ExitReactorHandle : public CasReactorHandle<ExitReactorData> {
#ifndef ITHARE_MTPRIMITIVES_CAS_DUMMY
				static_assert(sizeof(ExitReactorData) == MT_CAS_SIZE, "size of ReactorData MUST match CAS_SIZE");
#endif

			private:
				//MT_CAS * cas;
				//ExitReactorData last_read;

			public:
				ExitReactorHandle(MT_CAS& cas_)
					: CasReactorHandle<ExitReactorData>(cas_) {
				}

#ifdef ITHARE_MTPRIMITIVES_GENERIC_CAS_HANDLE
				bool writeCompleted(uint64_t id) {
					bool ret0;
					react_of_uint64_t(ret0, id, [](bool& earlyExit,ExitReactorData& new_data, uint64_t id) -> bool {
						uint64_t firstR = new_data.getFirstIDToRead();
						assert(id >= firstR);
						assert(id < firstR + QueueSize);

						uint64_t mask = new_data.getCompletedWritesMask();
						assert(!mask_getbit(mask, int(id - firstR)));
						uint64_t newMask = mask_setbit(mask, int(id - firstR));
						new_data.setCompletedWritesMask(newMask);

						bool ret = false;
						if (new_data.getReaderIsLocked()) {
							new_data.setReaderIsUnlocked();
							ret = true;
						}
						return ret;
					});
					return ret0;
				}
				std::pair<size_t, uint64_t> startRead() {
					//first == 0 means 'nothing to read'
					std::pair<size_t, uint64_t> ret;
					react_of_void(ret, [](bool& earlyExit, ExitReactorData& new_data) -> std::pair<size_t, uint64_t> {
						assert(!new_data.getReaderIsLocked());
						uint64_t mask = new_data.getCompletedWritesMask();

						if (mask_getbit(mask, 0)) {
							earlyExit = true;//yes, leaving without modifying state
							int n = 1;
							for (; n < QueueSize; ++n)
								if (!mask_getbit(mask, n))
									break;
							return std::pair<size_t, uint64_t>(n, new_data.getFirstIDToRead());
						}
						else {
							new_data.setReaderIsLocked();
						}

						return std::pair<size_t, uint64_t>(0, 0);
					});
					return ret;
				}
				uint64_t readCompleted(size_t sz,uint64_t id) {
					assert(sz <= QueueSize);
					uint64_t ret0;
					react_of_uint64_t_uint64_t(ret0, sz, id, [](bool& earlyExit, ExitReactorData& new_data, uint64_t sz, uint64_t id) -> uint64_t {
						//returns newLastW
						uint64_t mask = new_data.getCompletedWritesMask();
						assert(mask_getbit(mask, 0));

						uint64_t firstR = new_data.getFirstIDToRead();
						uint64_t newFirstR = firstR + sz;
						assert(newFirstR > firstR);//overflow check
						new_data.setFirstIDToRead(newFirstR);

						uint64_t newMask = mask;
						for (size_t i = 0; i < sz; ++i)//TODO: optimize
							newMask = mask_shiftoutbit0(newMask);
						new_data.setCompletedWritesMask(newMask);
						uint64_t newLastW = newFirstR + QueueSize;

						return newLastW;
						});
					return ret0;
				}

#else
				bool writeCompleted(uint64_t id) {
					while (true) {
						ExitReactorData new_data = last_read;
						uint64_t firstR = new_data.getFirstIDToRead();
						assert(id >= firstR);
						assert(id < firstR + QueueSize);

						uint64_t mask = new_data.getCompletedWritesMask();
						assert(!mask_getbit(mask, int(id - firstR)));
						uint64_t newMask = mask_setbit(mask, int(id - firstR));
						new_data.setCompletedWritesMask(newMask);

						bool ret = false;
						if (new_data.getReaderIsLocked()) {
							new_data.setReaderIsUnlocked();
							ret = true;
						}

						bool ok = cas->compare_exchange_weak(&last_read.data, new_data.data);
						if (ok) {
							last_read.data = new_data.data;
							return ret;
						}
						//else
						//	continue;
					}//while(true)		
				}
				std::pair<bool, uint64_t> startRead() {
					//returns tuple (rdok,rdID)
					//IMPORTANT: it is HIGHLY DESIRABLE to call startRead() close time-wise to constructor or reread()
					while (true) {
						ExitReactorData new_data = last_read;
						assert(!new_data.getReaderIsLocked());
						uint64_t mask = new_data.getCompletedWritesMask();

						if (mask_getbit(mask, 0))
							return std::pair<bool, uint64_t>(true, new_data.getFirstIDToRead());//yes, leaving without modifying state
						else {
							new_data.setReaderIsLocked();
						}

						bool ok = cas->compare_exchange_weak(&last_read.data, new_data.data);
						if (ok) {
							last_read.data = new_data.data;
							return std::pair<bool, uint64_t>(false, 0);
						}
						//else
						//	continue;
					}
				}
				uint64_t readCompleted(uint64_t id) {
					//returns newLastW
					while (true) {
						ExitReactorData new_data = last_read;
						uint64_t mask = new_data.getCompletedWritesMask();
						assert(mask_getbit(mask, 0));

						uint64_t firstR = new_data.getFirstIDToRead();
						uint64_t newFirstR = firstR + 1;
						assert(newFirstR > firstR);//overflow check
						new_data.setFirstIDToRead(newFirstR);

						uint64_t newMask = mask_shiftoutbit0(mask);
						new_data.setCompletedWritesMask(newMask);
						uint64_t newLastW = newFirstR + QueueSize;

						bool ok = cas->compare_exchange_weak(&last_read.data, new_data.data);
						if (ok) {
							last_read.data = new_data.data;
							return newLastW;
						}
						//else
						//	continue;
					}//while(true)
				}
#endif
			};

			//*** Locking Primitives ***/

			/*** IMPORTANT: semantics of ALL Locked* primitives
				 implies that they MUST unlock properly regardless of
				 potential races between thread-being-locked and unlock()
				 I.e., it should work BOTH if unlock() came earlier than
				 thread-being-locked reached wait(), AND if
				 unlock() came after thread-being-locked reached wait()
			***/

			class LockedSingleThread {
			private:
				int lockCount = 0;//MAY be both >0 and <0
				std::mutex mx;
				std::condition_variable cv;

			public:
				void lockAndWait() {
					//dbgRLockCountInc++;
					std::unique_lock<std::mutex> lock(mx);
					assert(lockCount == -1 || lockCount == 0);
					lockCount++;
					while (lockCount > 0) {
						cv.wait(lock);
					}
				}
				void unlock() {
					//dbgRLockCountDec++;
					std::unique_lock<std::mutex> lock(mx);
					lockCount--;
					lock.unlock();
					cv.notify_one();
				}
			};

			struct LockedThreadsListLockItem {
				uint64_t itemId;//item we're waiting for
				std::mutex mx;
				std::condition_variable cv;
				LockedThreadsListLockItem* next;
			};
			thread_local LockedThreadsListLockItem LockedThreadsList_data;//moved outside of LockedThreadsList to avoid strange reported bugs with thread_local members 

			class LockedThreadsList {
				//along the lines of LockedSingleThread
			private:
				uint64_t unlockUpTo = 0;
				std::mutex mx;
				//std::condition_variable cv;
				LockedThreadsListLockItem* first;

			public:
				void lockAndWait(uint64_t itemId) {
					mtDbgLog(0x11, itemId);
					std::unique_lock<std::mutex> lock(mx);
					//mtDbgLog(0x12, itemId);
					LockedThreadsList_data.itemId = itemId;
					insertSorted(&LockedThreadsList_data);
					while (itemId >= unlockUpTo) {
						LockedThreadsList_data.cv.wait(lock);
					}
					removeFromList(&LockedThreadsList_data);
				}

				void unlockAllUpTo(uint64_t id) {
					mtDbgLog(0x21, id);
					std::unique_lock<std::mutex> lock(mx);
					//mtDbgLog(0x22, id);
					assert(id >= unlockUpTo);
					unlockUpTo = id;
					for (LockedThreadsListLockItem* it = first; it != nullptr; it = it->next) {
						if (it->itemId < unlockUpTo) {
							it->cv.notify_one();
						}
					}
				}

			private:
				void insertSorted(LockedThreadsListLockItem* item) {
					LockedThreadsListLockItem* prev = nullptr;
					for (LockedThreadsListLockItem* it = first; it != nullptr; prev = it, it = it->next) {
						assert(it != item);
						assert(it->itemId != item->itemId);
						if (item->itemId < it->itemId) {
							insertBetweenHelper(item, prev, it);
							return;
						}
					}//for(item)
					//if not handled yet...
					insertBetweenHelper(item, prev, nullptr);
				}

				void insertBetweenHelper(LockedThreadsListLockItem* it, LockedThreadsListLockItem* prev, LockedThreadsListLockItem* item) {
					//dbgAssertNoLoop();
					if (prev == nullptr) {
						assert(item == first);
						it->next = first;
						first = it;
					}
					else {
						assert(item == prev->next);
						it->next = item;
						prev->next = it;
					}
					//dbgAssertNoLoop();
				}

				void removeFromList(LockedThreadsListLockItem* item) {
					assert(first != nullptr);
					LockedThreadsListLockItem* prev = nullptr;
					for (LockedThreadsListLockItem* it = first;; it = it->next) {
						if (it == item) {
							if (prev == nullptr)
								first = it->next;
							else
								prev->next = it->next;
							return;
						}
						prev = it;
					}
					//dbgAssertNoLoop();
				}
				void dbgAssertNoLoop() {
					std::set<LockedThreadsListLockItem*> all;
					for (LockedThreadsListLockItem* it = first; it != nullptr; it = it->next) {
						auto found = all.find(it);
						assert(found == all.end());
						all.insert(it);
					}
				}
			};
		}//namespace MWSRQueueFC_helpers

		//*** MWSRQueueFC ***//
		template<class QueueItem>
		class MWSRQueueFC {//'FC' stands for 'Flow Control'; TODO: rename into QueueMWSRDemandBlockingFlowControl
			static constexpr size_t QueueSize = MWSRQueueFC_helpers::QueueSize;
			static_assert(mt_is_powerof2(QueueSize), "QueueSize MUST be power of 2");//probably should work even if this is violated, 
																					 //  but will be less efficient

		private:
			QueueItem items[QueueSize];
			MT_CAS entrance;
			MWSRQueueFC_helpers::LockedThreadsList lockedWriters;
			MT_CAS exit;
			MWSRQueueFC_helpers::LockedSingleThread lockedReader;
			
			//'already read' kinda-cache
			QueueItem rdItems[QueueSize - 1];
			size_t rdBegin = 0;
			size_t rdEnd = 0;

		public:
			MWSRQueueFC()
				: entrance(MWSRQueueFC_helpers::EntranceReactorData(
					MWSRQueueFC_helpers::ExitReactorData::EntranceFirstToWrite,
					MWSRQueueFC_helpers::ExitReactorData::EntranceLastToWrite).getCasData()) {
			}
			void push(QueueItem&& item) {
				MWSRQueueFC_helpers::EntranceReactorHandle ent(entrance);
				std::pair<bool, uint64_t> ok_id = ent.allocateNextID();
				if (ok_id.first) {
#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
					++dbgPushLockedCount;
#endif
					lockedWriters.lockAndWait(ok_id.second);
					ent.unlock();
				}
				else {
#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
					++dbgPushUnlockedCount;
#endif
				}
				size_t idx = index(ok_id.second);
				items[idx] = std::move(item);
				MWSRQueueFC_helpers::ExitReactorHandle ex(exit);
				bool unlock = ex.writeCompleted(ok_id.second);
				if (unlock)
					lockedReader.unlock();
			}
			QueueItem pop() {
				if (rdBegin < rdEnd) {
					return std::move(rdItems[rdBegin++]);
				}
				assert(rdBegin == rdEnd);
				while (true) {
					MWSRQueueFC_helpers::ExitReactorHandle ex(exit);
					std::pair<size_t, uint64_t> sz_id = ex.startRead();
					size_t sz = sz_id.first;
					assert(sz <= QueueSize);
					if (!sz) {
#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
						++dbgPopLockedCount;
#endif
						lockedReader.lockAndWait();
						//unlocking ex is done by ex.writeCompleted()
						continue;//while(true)
					}
					else {
#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
						++dbgPopUnlockedCount;
#endif

					}

					uint64_t id = sz_id.second;
					size_t idx = index(id);
					QueueItem ret = std::move(items[idx]);
					assert(rdBegin == rdEnd);
					rdBegin = rdEnd = 0;
					for (size_t i = 1; i < sz; ++i) {
						rdItems[rdEnd++] = std::move(items[index(id + i)]);
					}
					assert(rdEnd < QueueSize - 1);

					uint64_t newLastW = ex.readCompleted(sz,id);

					MWSRQueueFC_helpers::EntranceReactorHandle ent(entrance);
					bool shouldUnlock = ent.moveLastToWrite(newLastW);
					if (shouldUnlock)
						lockedWriters.unlockAllUpTo(id + sz - 1 +QueueSize);

					//std::cout << "pop():" << re	t.value() << std::endl;
					return ret;
				}//while(true)
			}

#ifdef ITHARE_MTPRIMITIVES_STATCOUNTS
			std::atomic<size_t> dbgPushUnlockedCount = { 0 };
			std::atomic<size_t> dbgPushLockedCount = { 0 };
			std::atomic<size_t> dbgPopUnlockedCount = { 0 };
			std::atomic<size_t> dbgPopLockedCount = { 0 };
#endif

		private:
			size_t index(uint64_t i) {
				return i % QueueSize;//should be fast as long as QueueSize is power of 2
			}
		};
	}//namespace mtprimitives
}//namespace ithare

#endif//ithare_mtprimitives_queues_h_included
