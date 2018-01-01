#include <stdio.h>//NOT std::cout as it behaves weird in MT
#include <thread>
#include <chrono>
#include "../src/mwsr.h"

class Benchmark {
	std::chrono::high_resolution_clock::time_point start;

public:
	Benchmark() {
		start = std::chrono::high_resolution_clock::now();
	}
	int32_t us() {
		auto stop = std::chrono::high_resolution_clock::now();
		auto length = std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
		return (int32_t)length.count();
	}
};

struct QueueItem {
	int th;
	int i;

	QueueItem() {
		th = 0;
		i = 0;
	}
	QueueItem(int th_, int i_) : th(th_), i(i_) {}
};

MWSRQueue<QueueItem> q;

#define RDLOAD 0
#define NWR 2
#define WRWAIT 0
#define NITER 1000000
#define PRINTEVERY 1000000

std::atomic<size_t> fakeResult = {0};

void fake_load(size_t x) {
	size_t delta = 1;
	for(size_t i=0; i < RDLOAD ; ++i ) {
		delta *= x;
	}
	
	fakeResult += delta;
}

void pusher(int id) {
	for (int i = 0; i<NITER; ++i) {
		std::this_thread::sleep_for( std::chrono::microseconds(WRWAIT)); 
		if (i%PRINTEVERY == 0)
			printf("th=%d: push(%d)\n", id, i);
		QueueItem qi(id,i);
		q.push(std::move(qi));
	}
}


int main() {
	CAS cas;
	printf("sizeof(void*)=%d sizeof(CAS)=%d CAS is %s lock free\n", int(sizeof(void*)), int(sizeof(CAS)), cas.is_lock_free() ? "" : " NOT ");

	/*for (int i = 0; i < 32; ++i) {
	QueueItem item(i);
	q.push(std::move(item));
	}
	for (int i = 0; i < 32; ++i) {
	QueueItem qq = q.pop();
	std::cout << qq.value() << std::endl;
	}*/

	std::thread th[NWR];
	int lastValue[NWR];
	for (int i = 0; i < NWR; ++i) {
		lastValue[i] = -1;
		th[i] = std::thread(pusher, i);
	}

	Benchmark bm;
	for (int i = 0; i<NITER*NWR; ++i) {
		QueueItem qq = q.pop();
		if (i%PRINTEVERY == 0)
			printf("%d pop(): %d (th=%d)\n", i, qq.i, qq.th);
		if (lastValue[qq.th] + 1 != qq.i)
			throw std::exception();
		++lastValue[qq.th];
		
		fake_load(qq.i);
	}
	for (int i = 0; i < NWR; ++i)
		th[i].join();

	int us = bm.us();
	printf("fakeResult=%d\n", int(size_t(fakeResult)));
	printf("PUSH unlocked/locked=%d/%d POP unlocked/locked=%d/%d\n", int(size_t(dbgPushUnlockedCount)), int(size_t(dbgPushLockedCount)),int(size_t(dbgPopUnlockedCount)),int(size_t(dbgPopLockedCount)));
	printf("CAS ok=%d CAS retry=%d\n", int(size_t(dbgCasOkCount)), int(size_t(dbgCasRetryCount)));
	printf("Took %d microseconds\n", us);
	//printDbgLog(0);
	return 0;
}


