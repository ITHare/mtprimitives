﻿#include <stdio.h>//NOT std::cout as it behaves weird in MT
#include <thread>
#include "../src/mwsr.h"

static_assert(is_powerof2(QueueSize), "QueueSize MUST be power of 2");//probably should work even if this is violated, 
																	  //  but will be less efficient
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

#define NWR 8
#define NITER 1000000
#define PRINTEVERY 10000

void pusher(int id) {
	for (int i = 0; i<NITER; ++i) {
		if (i%PRINTEVERY == 0)
			printf("%d: push(%d)\n", id, i);
		QueueItem qi(id,i);
		q.push(std::move(qi));
	}
}

int main() {
	CAS cas;
	printf("sizeof(CAS)=%d CAS is %s lock free\n", int(sizeof(CAS)), cas.is_lock_free() ? "" : " NOT ");

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
	for (int i = 0; i<NITER*NWR; ++i) {
		QueueItem qq = q.pop();
		if (i%PRINTEVERY == 0)
			printf("%d pop(): %d\n", i, qq.i);
		if (lastValue[qq.th] + 1 != qq.i)
			throw std::exception();
		++lastValue[qq.th];
	}
	for (int i = 0; i < NWR; ++i)
		th[i].join();

	//printDbgLog(0);
	return 0;
}

