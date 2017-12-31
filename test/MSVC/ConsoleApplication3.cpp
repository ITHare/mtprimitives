// ConsoleApplication3.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <stdio.h>//NOT std::cout as it behaves weird in MT
#include <thread>
#include "../../src/mwsr.h"

MWSRQueue q;

#define NWR 3
#define NITER 1000000
#define PRINTEVERY 10000

void pusher(int id) {
	for (int i = 0; i<NITER; ++i) {
		if (i%PRINTEVERY == 0)
			printf("%d: push(%d)\n", id, i);
		q.push(i);
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
	for (int i = 0; i < NWR; ++i)
		th[i] = std::thread(pusher, i);
	for (int i = 0; i<NITER*NWR; ++i) {
		QueueItem qq = q.pop();
		if (i%PRINTEVERY == 0)
			printf("%d pop(): %d\n", i, qq.value());
	}
	for (int i = 0; i < NWR; ++i)
		th[i].join();

	//printDbgLog(0);
	return 0;
}


