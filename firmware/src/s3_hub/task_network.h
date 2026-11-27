#pragma once

void initWiFiInitial();
void startMdnsService();
void enterWifiOfflineAp();
void TaskNetworkMqtt(void* pvParameters);