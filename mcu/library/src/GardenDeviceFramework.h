#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#ifndef GDF_DEFAULT_MASTER_POLL_WAIT_SECONDS
#define GDF_DEFAULT_MASTER_POLL_WAIT_SECONDS 25
#endif
#ifndef GDF_EVENT_OUTBOX_SIZE
#define GDF_EVENT_OUTBOX_SIZE 32
#endif

namespace Garden {

enum class SyncStatus : uint8_t { Offline, Online, InSync, DirtyLocal, Conflict, Error };

struct GardenDeviceOptions {
  const char* deviceType = "garden-device";
  const char* devicePrefix = "GardenDevice";
  const char* firmwareVersion = "dev";
  uint16_t localPort = 80;
  const char* defaultLocalApPassword = "change-me";
  const char* defaultAdminPassword = "change-me";
  const char* defaultMasterSsid = "";
  const char* defaultMasterPassword = "";
  const char* defaultMasterBaseUrl = "http://garden-master.local";
  const char* defaultServerBaseUrl = "";
  const char* defaultApiToken = "";
  bool keepLocalApAlwaysOn = true;
  bool useHttpsInsecure = true;
  uint16_t masterPollWaitSeconds = GDF_DEFAULT_MASTER_POLL_WAIT_SECONDS;
  uint32_t heartbeatIntervalMs = 15000UL;
  uint32_t eventFlushIntervalMs = 10000UL;
  uint32_t offlineRetryIntervalMs = 5000UL;
};

class GardenDeviceBase {
public:
  explicit GardenDeviceBase(const GardenDeviceOptions& options);
  virtual ~GardenDeviceBase() = default;
  void begin();
  void loop();
  const String& deviceId() const { return _deviceId; }
  const String& deviceType() const { return _deviceType; }
  const String& firmwareVersion() const { return _firmwareVersion; }
  const String& localApSsid() const { return _localApSsid; }
  const String& masterBaseUrl() const { return _masterBaseUrl; }
  bool masterConnected() const { return WiFi.status() == WL_CONNECTED; }
  bool localConfigDirty() const { return _dirtyLocalConfig; }
  uint32_t configVersion() const { return _configVersion; }
  const String& configHash() const { return _configHash; }
  SyncStatus syncStatus() const { return _syncStatus; }
  void markLocalConfigDirty(const char* reason = "local-admin");
  void markConfigClean(uint32_t appliedVersion, const String& appliedHash);
  bool enqueueEvent(const char* type, JsonObjectConst payload);
  bool enqueueEvent(const char* type, const String& payloadJson);
  bool enqueueSimpleEvent(const char* type);
  void saveBaseConfig();
  void loadBaseConfig();
  void factoryResetBase();
  void connectToMasterWifi(bool wait);
  void startLocalAp();
  void restartLocalAp();
  void buildStateJson(JsonDocument& doc);
  String buildStateJsonString();

protected:
  virtual void onDeviceBegin() {}
  virtual void onDeviceLoop() {}
  virtual void appendCapabilities(JsonArray capabilities) { (void)capabilities; }
  virtual void appendDeviceState(JsonObject state) { (void)state; }
  virtual void appendDeviceConfig(JsonObject config) { (void)config; }
  virtual bool applyDeviceConfig(JsonObjectConst config) { (void)config; return true; }
  virtual bool handleDeviceMessage(const String& type, JsonObjectConst message, JsonObjectConst payload) { (void)type; (void)message; (void)payload; return false; }
  virtual void onBeforeFactoryReset() {}
  virtual void onAfterConfigApplied() {}
  virtual void onSyncStatusChanged(SyncStatus status) { (void)status; }
  WebServer& server() { return _server; }
  Preferences& prefs() { return _prefs; }
  void sendJson(int code, JsonDocument& doc);
  void sendOk();
  void sendError(int code, const char* message);
  void setupBaseRoutes();

private:
  GardenDeviceOptions _options;
  WebServer _server;
  Preferences _prefs;
  String _deviceId, _deviceType, _devicePrefix, _firmwareVersion;
  String _localApSsid, _localApPassword, _adminPassword;
  String _masterSsid, _masterPassword, _masterBaseUrl, _serverBaseUrl, _apiToken;
  uint32_t _configVersion = 1;
  String _configHash = "";
  bool _dirtyLocalConfig = false;
  String _dirtyReason = "";
  SyncStatus _syncStatus = SyncStatus::Offline;
  uint32_t _lastHeartbeatMs = 0, _lastEventFlushMs = 0, _lastMasterPollMs = 0, _lastOfflineRetryMs = 0;
  String _lastSyncMessage = "";
  struct OutboxEvent { bool used=false; String id; String type; String payloadJson; uint32_t createdAt=0; };
  OutboxEvent _outbox[GDF_EVENT_OUTBOX_SIZE];
  uint16_t _eventSeq = 0;
  void ensureDeviceIdentity();
  String generateDeviceId();
  String defaultLocalApSsidForId(const String& id);
  String makeEventId();
  void setSyncStatus(SyncStatus status);
  const char* syncStatusString() const;
  bool httpBeginUrl(HTTPClient& http, WiFiClient& plain, WiFiClientSecure& secure, const String& url);
  void addAuthHeaders(HTTPClient& http);
  bool postJson(const String& baseUrl, const String& path, const String& body, String& response, int& code);
  bool getJson(const String& baseUrl, const String& path, String& response, int& code);
  void syncWithMaster();
  void sendHeartbeat();
  void pollMasterMessages();
  void flushEvents();
  void pushDirtyConfigToMaster();
  bool applyMessage(JsonObjectConst message);
  bool applyConfigBundle(JsonObjectConst payload);
  bool validateIncomingConfigVersion(uint32_t incomingVersion, const String& incomingHash);
  String computeConfigHash();
  uint32_t fnv1a(const String& text);
  void handleRoot();
  void handleState();
  void handleConfigGet();
  void handleConfigPost();
  void handleNetworkPost();
  void handleFactoryReset();
  void handlePushNow();
  void handlePullNow();
  bool requireLocalAdmin();
  String htmlEscape(const String& s);
  String localAdminHtml();
};
} // namespace Garden
