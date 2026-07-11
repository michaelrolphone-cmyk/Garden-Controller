#pragma once

struct ReliableSchedule;
struct ReliableRunEvent;
struct ReliableObservedRun;

void slaveInternetTimePreInit();
void meshReliablePreInit();
void meshEarlyInit();
void meshReliablePostInit();
void slaveInternetTimePostInit();
void slaveScheduleCatchupPostInit();
