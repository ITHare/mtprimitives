#include <inttypes.h>
#include <string.h>
//#include <assert.h>
#include <utility>
#include <atomic>
#include <condition_variable>
#include <set>//dbg-only

inline void my_assert_fail() {//to put a breakpoint
	throw std::exception();
}

#define assert(expr) \
    if (!(expr)) my_assert_fail();

//#include <windows.h>
uint64_t dbgLogBuf[1024] = {};
std::atomic<size_t> dbgLogBufOffset = { 0 };

inline void dbgLog(uint32_t id, uint64_t param) {
	uint64_t entry = (uint64_t(id) << 32) | uint32_t(param);
	size_t offset = dbgLogBufOffset++ &0x3FF;
	dbgLogBuf[offset] = entry;
}

/*__declspec(noinline) void printDbgLog(int lastN) {
	assert(lastN < 1024);
	for (size_t i = dbgLogBufOffset + 1024; i >= dbgLogBufOffset + 1024 - lastN; --i) {
		size_t realOffset = i & 0x3FF;
		uint64_t entry = dbgLogBuf[realOffset];
		char tmp[256];
		snprintf(tmp,256,"%02x: % 8x\n", uint32_t(entry>>32), uint32_t(entry));
		OutputDebugStringA(tmp);
	}
}*/

/************************/

constexpr inline bool is_powerof2(size_t v) {//from https://stackoverflow.com/questions/10585450/how-do-i-check-if-a-template-parameter-is-a-power-of-two
    return v && ((v & (v - 1)) == 0);
}

/************************/
 
//QueueSize and QueueItem to be made template parameters of MWSRQueue 
const size_t QueueSize = 64;// SHOULD be power of 2 for performance reasons
static_assert(is_powerof2(QueueSize), "QueueSize MUST be power of 2");//probably should work even if this is violated, 
                                                                      //  but will be less efficient
struct QueueItem {
	int i;

	QueueItem() {
		i = 0;
	}
	QueueItem(int i_) : i(i_) {}
	int value() {
		return i;
	}
};

/*************************/

//*** CAS functions - MAY BE platform-dependent ***//

#define CAS_SIZE 16//x64 starting from Core Duo

#if CAS_SIZE == 16
struct CAS_DATA {
	uint64_t lo;
	uint64_t hi;
};
#else
#error
#endif

class CAS {
	//wrapper to simplify manual rewriting if it becomes necessary
	private:
	std::atomic<CAS_DATA> cas; 

	public:
	CAS() {
		CAS_DATA data;
		memset(&data,0,sizeof(CAS_DATA));
		cas = data;
	}
	CAS( CAS_DATA data ) {
		cas = data;
	}
	CAS_DATA load() {
		return cas.load();
	}
	bool compare_exchange_weak( CAS_DATA* expected, CAS_DATA desired ) {
		return cas.compare_exchange_weak(*expected, desired); 
	}
	bool is_lock_free() const {//if it happens to be NOT lock free but the platform does support CAS of CAS_SIZE - 
							   //	we'll have to use platform-specific stuff 
		return cas.is_lock_free();
	}
};	
//static_assert(sizeof(CAS)==CAS_SIZE, "something went badly wrong");


//*** mask_*() functions need to represent a coherent view, but nobody really cares about exact bit numbers outside of them ***//

bool mask_getbit(uint64_t mask,int pos) {
	assert( pos >= 0 );
	assert( pos < 64 );
	return (mask & ( uint64_t(1) << pos )) != 0;
}

uint64_t mask_setbit(uint64_t mask,int pos) {
	assert( pos >= 0 );
	assert( pos < 64 );
	return mask | (uint64_t(1) << pos );
}

bool mask_resetbit(uint64_t mask,int pos) {
	assert( pos >= 0 );
	assert( pos < 64 );
	return mask & ~(uint64_t(1) << pos );
}

uint64_t mask_shiftoutbit0(uint64_t mask) {
	return mask >> 1;
}

//*** EntranceReactor and ExitReactor ***//

struct EntranceReactorData {
	private:
	//kinda-fields:
	//  firstIDToWrite (up to 64 bits, current implementation: 64 bits)
	//  lastIDToWrite (up to 64 bits, current implementation: signed 32-bit offset to firstIDToWrite)
	//  lockedThreadCount (up to 32 bits, current implementation: 32 bits)
	alignas(CAS_SIZE) CAS_DATA data;
	
	public:
	EntranceReactorData() {
	}
	EntranceReactorData( uint64_t firstToWrite, uint64_t lastToWrite ) {
		memset(this,0, sizeof(*this));
		setIDsToWrite( firstToWrite, lastToWrite );
	}
	CAS_DATA getCasData() const {
		return data;
	}

	friend class EntranceReactorHandle;
	private:
		uint64_t getFirstIDToWrite() const {
			return data.lo;
		}
		void setFirstIDToWrite( uint64_t val ) {
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
			return data.lo + int32_t(data.hi&0xFFFFFFFFLL);
		}
		void setLastIDToWrite( uint64_t val ) {
			uint32_t lc = getLockedThreadCount();
			uint64_t first = getFirstIDToWrite();
			int64_t offset = val - data.lo;
			assert((int32_t)offset == offset);
			data.hi = (data.hi & 0xFFFFFFFF'00000000LL) | uint32_t(int32_t(offset));
			assert(getLastIDToWrite() == val);
			assert(getFirstIDToWrite() == first);
			assert(lc == getLockedThreadCount());
		}
		void setIDsToWrite( uint64_t first, uint64_t last ) {
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
		void setLockedThreadCount( uint32_t val ) {
			data.hi = (data.hi & 0xFFFFFFFFLL) | ((uint64_t)val << 32);
		}
};

static_assert( sizeof(EntranceReactorData) == CAS_SIZE, "size of ReactorData MUST match CAS_SIZE" );

class EntranceReactorHandle {
	private:
	CAS* cas;
	EntranceReactorData last_read;

	public:
		EntranceReactorHandle( CAS& cas_ ) 
		: cas( &cas_ ) {
			last_read.data = cas->load(); 
		}
		std::pair<bool,uint64_t> allocateNextID() {
			//returns tuple (shouldLock,newID); newID is returned regardless of shouldLock
			while(true) {
				EntranceReactorData new_data = last_read;
				uint64_t firstW = new_data.getFirstIDToWrite();
				//MAY be >= lastIDToWrite()
				
				//regardless of ID being available, we'll try to increment 
				//	(but if our ID is not available for processing yet - we'll ask to lock)
				uint64_t newW = firstW + 1;
				assert( newW > firstW );//overflow check
				new_data.setFirstIDToWrite( newW );
				
				bool willLock = false;
				if( newW >= new_data.getLastIDToWrite() ) {
					willLock = true;
					uint32_t lockedCount = new_data.getLockedThreadCount();
					//dbgLog(0x101, lockedCount);
					uint32_t newLockedCount = lockedCount + 1;
					assert(newLockedCount > lockedCount);//overflow check
					new_data.setLockedThreadCount( newLockedCount );
				}//if(newW >= ...)

				bool ok = cas->compare_exchange_weak( &last_read.data, new_data.data );
				if (ok) {
					last_read.data = new_data.data;
					return std::pair<bool, uint64_t>(willLock, firstW);
				}
				//else
				//	continue;
				//if(willLock)
				//	dbgLog(0x102, 0);
			}//while(true)
		}//allocateNextID()
		void unlock() {
			while (true) {
				EntranceReactorData new_data = last_read;
				uint32_t lockedCount = new_data.getLockedThreadCount();
				uint32_t newLockedCount = lockedCount - 1;
				//dbgLog(0x111, lockedCount);
				assert(newLockedCount < lockedCount);//underflow check
				new_data.setLockedThreadCount(newLockedCount);
				bool ok = cas->compare_exchange_weak(&last_read.data, new_data.data);
				if (ok) {
					last_read.data = new_data.data;
					return;
				}
				//else
				//	continue;
				//dbgLog(0x112, 0);
			}
		}
		bool moveLastToWrite( uint64_t newLastW ) {
			//returns 'shouldUnlock'
			while(true) {
				EntranceReactorData new_data = last_read;
				uint64_t lastW = new_data.getLastIDToWrite();
				assert( lastW <= newLastW );

				uint32_t lockedCount = new_data.getLockedThreadCount();

				new_data.setLastIDToWrite( newLastW );
				bool ok = cas->compare_exchange_weak( &last_read.data, new_data.data );
				if (ok) {
					last_read.data = new_data.data;
					return lockedCount > 0;
				}
				//else
				//	continue;
			}//while(true)
		}
	};

class ExitReactorData {
	private:
	alignas(CAS_SIZE) CAS_DATA data;
	//kinda-fields:
	//  firstIDToRead (up to 64 bits, current implementation: 63 bits)
	//  completedWritesMask (bitmask, up to 64 bits, current implementation: 64 bits)
	//  readerIsLocked (bool, current implementation: 1 bit)
	
	public:
	static_assert(QueueSize<=64,"QueueSize cannot exceed number of bits in mask");
	const static int EntranceFirstToWrite = 0;
	const static int EntranceLastToWrite = QueueSize;
	
	public:
	ExitReactorData() {
		memset(this,0, sizeof(*this));
		setFirstIDToRead( EntranceFirstToWrite );
	}

	friend class ExitReactorHandle;
	private:
	uint64_t getFirstIDToRead() {
		return data.hi & 0x7FFF'FFFFLL;
	}
	void setFirstIDToRead( uint64_t val ) {
		assert( (val & 0x8000'0000LL) == 0 );
		data.hi = ( data.hi & 0x8000'0000LL ) | val;
	}
	uint64_t getCompletedWritesMask() {
		return data.lo;
	}
	void setCompletedWritesMask( uint64_t val ) {
		data.lo = val;
	}
	bool getReaderIsLocked() {
		return ( data.hi & 0x8000'0000LL ) != 0;
	}
	void setReaderIsLocked() {
		data.hi |= 0x8000'0000LL;
	}
	void setReaderIsUnlocked() {
		data.hi &= ~0x8000'0000LL;
	}
};

static_assert( sizeof(ExitReactorData) == CAS_SIZE, "size of ReactorData MUST match CAS_SIZE" );

class ExitReactorHandle {
	private:
	CAS* cas;
	ExitReactorData last_read;
	
	public:
	ExitReactorHandle( CAS& cas_ ) 
		: cas( &cas_ ) {
			last_read.data = cas->load(); 
		}
	/*--- 
	 * void reread() {
		cas_load_acquire( &last_read, data ); 
	}*/
	bool writeCompleted( uint64_t id ) {
		while(true) {
			ExitReactorData new_data = last_read;
			uint64_t firstR = new_data.getFirstIDToRead();
			assert( id >= firstR );
			assert( id < firstR + QueueSize);

			uint64_t mask = new_data.getCompletedWritesMask();
			assert(!mask_getbit(mask,int(id-firstR)));
			uint64_t newMask = mask_setbit(mask,int(id-firstR));
			new_data.setCompletedWritesMask(newMask);

			bool ret = false;
			if( new_data.getReaderIsLocked() ) {
				new_data.setReaderIsUnlocked();
				ret = true;
			}

			bool ok = cas->compare_exchange_weak( &last_read.data, new_data.data );
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
		while(true) {
			ExitReactorData new_data = last_read;
			assert( !new_data.getReaderIsLocked() );
			uint64_t mask = new_data.getCompletedWritesMask();

			if(mask_getbit(mask,0))
				return std::pair<bool,uint64_t>( true, new_data.getFirstIDToRead() );//yes, leaving without modifying state
			else {
				new_data.setReaderIsLocked();
			}

			bool ok = cas->compare_exchange_weak( &last_read.data, new_data.data );
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
		while(true) {
			ExitReactorData new_data = last_read;
			uint64_t mask = new_data.getCompletedWritesMask();
			assert(mask_getbit(mask,0));
		
			uint64_t firstR = new_data.getFirstIDToRead();
			uint64_t newFirstR = firstR + 1;
			assert( newFirstR > firstR );//overflow check
			new_data.setFirstIDToRead(newFirstR);

			uint64_t newMask = mask_shiftoutbit0( mask );
			new_data.setCompletedWritesMask(newMask);
			uint64_t newLastW = newFirstR + QueueSize;

			bool ok = cas->compare_exchange_weak( &last_read.data, new_data.data );
			if (ok) {
				last_read.data = new_data.data;
				return newLastW;
			}
			//else
			//	continue;
		}//while(true)
	}
};

//*** Locking Primitives ***/

/*** IMPORTANT: semantics of ALL Locked* primitives 
     implies that they MUST unlock properly regardless of
     potential races between thread-being-locked and unlock()
     I.e., it should work BOTH if unlock() came earlier than
     thread-being-locked reached wait(), AND if 
     unlock() came after thread-being-locked reached wait()
***/

//std::atomic<int> dbgRLockCountInc = 0;
//std::atomic<int> dbgRLockCountDec = 0;

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
		while(lockCount>0) {
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

/*
struct LockedThreadsListLockItem {
	uint64_t itemId;//item we're waiting for
	std::mutex mx;
	std::condition_variable cv;
	int lockCount = 0;//MAY be both >0 and <0
	LockedThreadsListLockItem* next;
};
thread_local LockedThreadsListLockItem lockedThreadsList_data;//moved outside of LockedThreadsList to avoid strange reported bugs with thread_local members 

class LockedThreadsLockItemList {
	private:
	std::mutex mx;
	LockedThreadsListLockItem* first = nullptr;
	
	public:
	LockedThreadsListLockItem* extractFirstIf(uint64_t id) {
		//std::cout << "extractFirstIf():" << id << std::endl;
		std::unique_lock<std::mutex> lock(mx);
		if (first == nullptr)
			return nullptr;
		//std::cout << "extractFirstIf(): first=" << first->itemId << std::endl;
		if (id < first->itemId)
			return nullptr;
		LockedThreadsListLockItem* item = first;
		first = item->next;
		//std::cout << "extractFirstIf(): extracted:" << item->itemId << std::endl;
		return item;
	}
	void insertSorted(LockedThreadsListLockItem* it) {
		//std::cout << "insertSorted():" << it->itemId << std::endl;
		std::unique_lock<std::mutex> lock(mx);
		dbgLog(0x31, it->itemId);
		LockedThreadsListLockItem* prev=nullptr;
		bool handled = false;
		for(LockedThreadsListLockItem* item=first; item != nullptr ; prev=item, item = item->next) {
			if(item->itemId > it->itemId) {
				handled = true;
				insertBetweenHelper(it,prev,item);
				break;//for(item)
			}
		}//for(item)
		if(!handled)
			insertBetweenHelper(it,prev,nullptr);
	}

	private:
	void insertBetweenHelper(LockedThreadsListLockItem* it, LockedThreadsListLockItem* prev, LockedThreadsListLockItem* item) {
		if(prev==nullptr) {
			assert(item==first);
			lockedThreadsList_data.next = first;
			first = it;
		}
		else {
			assert(item==prev->next);
			it->next = item;
			prev->next = it;
		}
	}
};

std::atomic<uint64_t> dbgLastUnlockAllUpTo = 0;
std::atomic<uint64_t> dbgLastLockAndWait = 0;
std::atomic<uint64_t> dbgLockCountInc = 0;
std::atomic<uint64_t> dbgLockCountDec = 0;

class LockedThreadsList {
	//along the lines of LockedSingleThread
	private:
	LockedThreadsLockItemList lockedList;
	
	public:
	void lockAndWait(uint64_t itemId) {
		dbgLastLockAndWait = itemId;
		lockedThreadsList_data.itemId = itemId;

		dbgLog(0x11, itemId);
		lockedList.insertSorted(&lockedThreadsList_data);
		dbgLog(0x12, itemId);

		std::unique_lock<std::mutex> lock(lockedThreadsList_data.mx);
		assert(lockedThreadsList_data.lockCount == -1 || lockedThreadsList_data.lockCount == 0);
		lockedThreadsList_data.lockCount++;
		dbgLog(0x14, lockedThreadsList_data.lockCount);
		dbgLockCountInc++;
		while(lockedThreadsList_data.lockCount>0) {
			lockedThreadsList_data.cv.wait(lock);
		}
	}
	void unlockAllUpTo(uint64_t id) {
		dbgLog(0x21, id);
		dbgLastUnlockAllUpTo = id;
		while (true) {
			LockedThreadsListLockItem* item = lockedList.extractFirstIf(id);
			if (item == nullptr) {
				//dbgLog(0x22, id);
				return;
			}
			dbgLog(0x23, id);
			assert(id >= item->itemId);

			std::unique_lock<std::mutex> lock(item->mx);
			item->lockCount--;
			dbgLockCountDec++;
			dbgLog(0x24, item->lockCount);
			lock.unlock();
			item->cv.notify_one();
		}
	}
};*/

struct LockedThreadsListLockItem {
	uint64_t itemId;//item we're waiting for
	std::mutex mx;
	std::condition_variable cv;
	LockedThreadsListLockItem* next;
};
thread_local LockedThreadsListLockItem lockedThreadsList_data;//moved outside of LockedThreadsList to avoid strange reported bugs with thread_local members 


class LockedThreadsList {
	//along the lines of LockedSingleThread
private:
	uint64_t unlockUpTo = 0;
	std::mutex mx;
	//std::condition_variable cv;
	LockedThreadsListLockItem* first;

public:
	void lockAndWait(uint64_t itemId) {
		dbgLog(0x11, itemId);
		std::unique_lock<std::mutex> lock(mx);
		dbgLog(0x12, itemId);
		lockedThreadsList_data.itemId = itemId;
		insertSorted(&lockedThreadsList_data);
		while (itemId >= unlockUpTo){
			lockedThreadsList_data.cv.wait(lock);
		}
		removeFromList(&lockedThreadsList_data);
	}

	void unlockAllUpTo(uint64_t id) {
		dbgLog(0x21, id);
		std::unique_lock<std::mutex> lock(mx);
		dbgLog(0x22, id);
		assert(id >= unlockUpTo);
		unlockUpTo = id;
		for (LockedThreadsListLockItem* it = first; it != nullptr; it=it->next) {
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
		//dbgNoLoop();
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
		//dbgNoLoop();
	}

	void removeFromList(LockedThreadsListLockItem* item) {
		assert(first != nullptr);
		LockedThreadsListLockItem* prev = nullptr;
		for (LockedThreadsListLockItem* it = first;; it=it->next) {
			if (it == item) {
				if (prev == nullptr)
					first = it->next;
				else
					prev->next = it->next;
				return;
			}
			prev = it;
		}
		//dbgNoLoop();
	}
	void dbgNoLoop() {
		std::set<LockedThreadsListLockItem*> all;
		for (LockedThreadsListLockItem* it = first; it != nullptr ; it = it->next) {
			auto found = all.find(it);
			assert(found == all.end());
			all.insert(it);
		}
	}
};

//*** MWSRQueue ***//
std::atomic<int> dbgPushCount = 0;
std::atomic<int> dbgPopCount = 0;

class MWSRQueue {
	private:
	QueueItem items[QueueSize];
	CAS entrance;
	LockedThreadsList lockedWriters;
	CAS exit;
	LockedSingleThread lockedReader;
	
	public:
	MWSRQueue() 
	: entrance( EntranceReactorData(ExitReactorData::EntranceFirstToWrite, ExitReactorData::EntranceLastToWrite).getCasData() ) {
	}
	void push(QueueItem&& item) {
		dbgPushCount++;
		//std::cout << "push():" << item.value() << std::endl;
		EntranceReactorHandle ent(entrance);
		std::pair<bool,uint64_t> ok_id = ent.allocateNextID();
		if(ok_id.first) {
			lockedWriters.lockAndWait(ok_id.second);
			ent.unlock();
		}
		size_t idx = index(ok_id.second);
		items[idx] = std::move(item);
		ExitReactorHandle ex(exit);
		bool unlock = ex.writeCompleted(ok_id.second);
		if( unlock )
			lockedReader.unlock();
	}
	QueueItem pop() {
		dbgPopCount++;
		while(true) {
			ExitReactorHandle ex(exit);
			std::pair<bool,uint64_t> ok_id = ex.startRead();
			if( !ok_id.first ) {
				lockedReader.lockAndWait();
				//unlocking ex is done by ex.writeCompleted()
				continue;//while(true)
			}
		
			uint64_t id = ok_id.second;
			size_t idx = index(id);
			QueueItem ret = std::move(items[idx]);
			uint64_t newLastW = ex.readCompleted(id);

			EntranceReactorHandle ent(entrance);
			bool shouldUnlock = ent.moveLastToWrite(newLastW);
			if(shouldUnlock)
				lockedWriters.unlockAllUpTo(id + QueueSize);

			//std::cout << "pop():" << re	t.value() << std::endl;
			return ret;
		}//while(true)
	}
	
	private:
	size_t index(uint64_t i) {
		return i % QueueSize;//should be fast as long as QueueSize is power of 2
	}
};

