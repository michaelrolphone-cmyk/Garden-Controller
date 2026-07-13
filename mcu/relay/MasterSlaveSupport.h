#pragma once

struct ReliableSchedule;
struct ReliableRunEvent;
struct ReliableObservedRun;

void slaveInternetTimePreInit();
void meshReliablePreInit();
void meshPreSetupInitNoTask();
void gardenArmPostSetupServices();
void meshReliablePostInit();
void slaveInternetTimePostInit();
void slaveScheduleCatchupPostInit();
