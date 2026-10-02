#include <Arduino.h>
#include <BLEClient.h>
#include <BLEDevice.h>
#include <BLERemoteCharacteristic.h>
#include <BLERemoteService.h>
#include <BLEScan.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <cstring>
#include <esp_bt.h>
#include <esp_err.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy secrets.example.h to secrets.h and fill in Wi-Fi/WebSocket settings"
#endif

constexpr char BLE_RECEIVER_NAME[] = "CatActivity-RX-C3";
constexpr char TARGET_DEVICE_NAME[] = "CatMotion-01";
constexpr char SERVICE_UUID_TEXT[] =
    "5f2b0001-8a2f-4b7e-9d3c-112233445566";
constexpr char ACTIVITY_CHARACTERISTIC_UUID_TEXT[] =
    "5f2b0003-8a2f-4b7e-9d3c-112233445566";

constexpr uint32_t BLE_SCAN_SECONDS = 5;
constexpr uint32_t BLE_RESCAN_DELAY_MS = 1000;
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 10000;
constexpr uint32_t WS_RECONNECT_INTERVAL_MS = 5000;
constexpr uint32_t BATTERY_REPORT_INTERVAL_MS = 60000;
constexpr uint8_t SUPPORTED_PROTOCOL_VERSION = 1;
constexpr esp_power_level_t BLE_TX_POWER_LEVEL = ESP_PWR_LVL_P9;
// 新状态需要连续出现三次才会对外上报，避免猫咪动作来回闪烁。
constexpr uint8_t STATE_CHANGE_CONFIRMATIONS = 2;

const BLEUUID motionServiceUuid(SERVICE_UUID_TEXT);
const BLEUUID activityCharacteristicUuid(
    ACTIVITY_CHARACTERISTIC_UUID_TEXT
);

enum ActivityState : uint8_t {
  RESTING_CANDIDATE = 0,
  WALKING_CANDIDATE = 1,
  IMPACT_CANDIDATE = 2,
  VIGOROUS_ACTIVITY = 3,
  OTHER_ACTIVITY = 4,
  WARMING_UP = 255,
};

struct __attribute__((packed)) ActivityPacket {
  uint32_t timestamp_ms;
  uint8_t state;
  uint8_t protocol_version;
  uint16_t accel_std_mg;
  uint16_t accel_p2p_mg;
  uint16_t accel_max_mg;
  uint16_t gyro_rms_dps10;
  uint8_t periodicity_percent;
  uint8_t battery_percent;
};

static_assert(
    sizeof(ActivityPacket) == 16,
    "ActivityPacket must be exactly 16 bytes"
);

BLEScan* bleScan = nullptr;
BLEClient* bleClient = nullptr;
BLEAdvertisedDevice* targetDevice = nullptr;
BLERemoteCharacteristic* activityCharacteristic = nullptr;
QueueHandle_t activityQueue = nullptr;
WebSocketsClient relaySocket;

volatile bool bleConnectRequested = false;
volatile bool bleScanRequested = true;
volatile bool bleReceiverReady = false;
volatile bool relayConnected = false;
volatile uint32_t nextBleScanAt = 0;

bool relayStarted = false;
uint32_t lastWifiAttemptAt = 0;
uint32_t stateSequence = 0;
uint32_t lastStatusReportAt = 0;
ActivityPacket pendingPacket{};
bool hasPendingPacket = false;
ActivityPacket confirmedPacket{};
bool hasConfirmedPacket = false;
uint8_t candidateState = WARMING_UP;
uint8_t candidateStateConfirmations = 0;
String relayPath;

const char* activityName(uint8_t state) {
  switch (state) {
    case RESTING_CANDIDATE:
      return "resting_candidate";
    case WALKING_CANDIDATE:
      return "walking_candidate";
    case IMPACT_CANDIDATE:
      return "impact_candidate";
    case VIGOROUS_ACTIVITY:
      return "vigorous_activity";
    case OTHER_ACTIVITY:
      return "other_activity";
    case WARMING_UP:
      return "warming_up";
    default:
      return "unknown";
  }
}

bool isKnownActivity(uint8_t state) {
  return state <= OTHER_ACTIVITY || state == WARMING_UP;
}

bool publishPacket(const ActivityPacket& packet) {
  lastStatusReportAt = millis();

  if (!relayConnected) {
    pendingPacket = packet;
    hasPendingPacket = true;
    return false;
  }

  char message[256];
  const int length = snprintf(
      message,
      sizeof(message),
      "{\"type\":\"state\",\"protocol\":1,\"deviceId\":\"%s\","
      "\"state\":%u,\"batteryPercent\":%u,\"seq\":%lu,\"sourceMs\":%lu}",
      CAT_DEVICE_ID,
      packet.state,
      packet.battery_percent,
      static_cast<unsigned long>(stateSequence++),
      static_cast<unsigned long>(packet.timestamp_ms)
  );

  if (length <= 0 || static_cast<size_t>(length) >= sizeof(message)) {
    Serial.println("[WS] State message was too long");
    return false;
  }

  const bool sent = relaySocket.sendTXT(
      reinterpret_cast<uint8_t*>(message),
      static_cast<size_t>(length)
  );

  if (!sent) {
    pendingPacket = packet;
    hasPendingPacket = true;
    return false;
  }

  hasPendingPacket = false;
  Serial.printf(
      "[WS] Published %u,%s\n",
      packet.state,
      activityName(packet.state)
  );
  return true;
}

void handleActivityPacket(const ActivityPacket& packet) {
  if (packet.protocol_version != SUPPORTED_PROTOCOL_VERSION) {
    Serial.printf(
        "[BLE] Unsupported protocol version: %u\n",
        packet.protocol_version
    );
    return;
  }

  if (!isKnownActivity(packet.state)) {
    Serial.printf("[BLE] Unknown activity state: %u\n", packet.state);
    return;
  }

  Serial.printf(
      "[BLE] %lu,%u,%s,battery=%u%%\n",
      static_cast<unsigned long>(packet.timestamp_ms),
      packet.state,
      activityName(packet.state),
      packet.battery_percent
  );

  if (!hasConfirmedPacket) {
    confirmedPacket = packet;
    hasConfirmedPacket = true;
    candidateState = packet.state;
    candidateStateConfirmations = 0;
    publishPacket(confirmedPacket);
    return;
  }

  if (packet.state == confirmedPacket.state) {
    // 相同状态只按电量心跳周期上报，不会导致桌面端换图。
    confirmedPacket = packet;
    candidateState = packet.state;
    candidateStateConfirmations = 0;
    if (millis() - lastStatusReportAt >= BATTERY_REPORT_INTERVAL_MS) {
      publishPacket(confirmedPacket);
    }
    return;
  }

  if (packet.state != candidateState) {
    candidateState = packet.state;
    candidateStateConfirmations = 1;
  } else if (candidateStateConfirmations < STATE_CHANGE_CONFIRMATIONS) {
    ++candidateStateConfirmations;
  }

  Serial.printf(
      "[BLE] Candidate %u,%s (%u/%u)\n",
      candidateState,
      activityName(candidateState),
      candidateStateConfirmations,
      STATE_CHANGE_CONFIRMATIONS
  );

  if (candidateStateConfirmations < STATE_CHANGE_CONFIRMATIONS) {
    confirmedPacket.timestamp_ms = packet.timestamp_ms;
    confirmedPacket.battery_percent = packet.battery_percent;
    if (millis() - lastStatusReportAt >= BATTERY_REPORT_INTERVAL_MS) {
      publishPacket(confirmedPacket);
    }
    return;
  }

  confirmedPacket = packet;
  candidateStateConfirmations = 0;
  publishPacket(confirmedPacket);
}

void relayEvent(
    WStype_t type,
    uint8_t* payload,
    size_t length
) {
  switch (type) {
    case WStype_CONNECTED:
      relayConnected = true;
      Serial.printf(
          "[WS] Connected to %s%s\n",
          WS_HOST,
          relayPath.c_str()
      );
      if (hasPendingPacket) {
        publishPacket(pendingPacket);
      } else if (hasConfirmedPacket) {
        // 服务端重连后补发已确认状态，无需恢复周期性重复上报。
        publishPacket(confirmedPacket);
      }
      break;

    case WStype_DISCONNECTED:
      relayConnected = false;
      Serial.println("[WS] Disconnected; automatic reconnect enabled");
      break;

    case WStype_TEXT:
      Serial.printf(
          "[WS] Server: %.*s\n",
          static_cast<int>(length),
          reinterpret_cast<const char*>(payload)
      );
      break;

    case WStype_ERROR:
      relayConnected = false;
      Serial.println("[WS] Connection error");
      break;

    default:
      break;
  }
}

void startRelaySocket() {
  relayPath = String(WS_PATH)
      + "?device=" + CAT_DEVICE_ID
      + "&role=device&token=" + CAT_PUBLISH_TOKEN;

  relaySocket.onEvent(relayEvent);
  relaySocket.setReconnectInterval(WS_RECONNECT_INTERVAL_MS);
  relaySocket.enableHeartbeat(20000, 4000, 2);

  if (WS_USE_TLS) {
    relaySocket.beginSSL(WS_HOST, WS_PORT, relayPath.c_str());
  } else {
    relaySocket.begin(WS_HOST, WS_PORT, relayPath.c_str());
  }

  relayStarted = true;
  Serial.printf("[WS] Connecting to %s:%u\n", WS_HOST, WS_PORT);
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!relayStarted) startRelaySocket();
    return;
  }

  relayConnected = false;
  const uint32_t now = millis();
  if (now - lastWifiAttemptAt < WIFI_RECONNECT_INTERVAL_MS) return;

  lastWifiAttemptAt = now;
  Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void activityNotifyCallback(
    BLERemoteCharacteristic*,
    uint8_t* data,
    size_t length,
    bool
) {
  if (length != sizeof(ActivityPacket)) return;

  ActivityPacket packet{};
  std::memcpy(&packet, data, sizeof(packet));
  xQueueOverwrite(activityQueue, &packet);
}

class ReceiverClientCallbacks final : public BLEClientCallbacks {
 public:
  void onConnect(BLEClient*) override {}

  void onDisconnect(BLEClient*) override {
    bleReceiverReady = false;
    activityCharacteristic = nullptr;
    bleScanRequested = true;
    nextBleScanAt = millis() + BLE_RESCAN_DELAY_MS;
    Serial.println("[BLE] CatMotion disconnected");
  }
};

class TargetScanCallbacks final : public BLEAdvertisedDeviceCallbacks {
 public:
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (
        !advertisedDevice.haveServiceUUID()
        || !advertisedDevice.isAdvertisingService(motionServiceUuid)
    ) {
      return;
    }

    BLEDevice::getScan()->stop();
    if (targetDevice != nullptr) delete targetDevice;
    targetDevice = new BLEAdvertisedDevice(advertisedDevice);
    bleConnectRequested = true;
    bleScanRequested = false;
  }
};

void bleScanComplete(BLEScanResults) {
  if (!bleConnectRequested && !bleReceiverReady) {
    bleScanRequested = true;
    nextBleScanAt = millis() + BLE_RESCAN_DELAY_MS;
  }
}

bool connectToCatMotion() {
  if (targetDevice == nullptr) return false;

  Serial.printf(
      "[BLE] Found %s at %s, connecting...\n",
      TARGET_DEVICE_NAME,
      targetDevice->getAddress().toString().c_str()
  );

  if (bleClient == nullptr) {
    bleClient = BLEDevice::createClient();
    bleClient->setClientCallbacks(new ReceiverClientCallbacks());
  }

  if (bleClient->isConnected()) bleClient->disconnect();
  if (!bleClient->connect(targetDevice)) return false;

  const esp_err_t connectionPowerResult = esp_ble_tx_power_set_enhanced(
      ESP_BLE_ENHANCED_PWR_TYPE_CONN,
      bleClient->getConnId(),
      BLE_TX_POWER_LEVEL
  );
  if (connectionPowerResult != ESP_OK) {
    Serial.printf(
        "[BLE] Failed to set connection TX power: %s\n",
        esp_err_to_name(connectionPowerResult)
    );
  }

  BLERemoteService* service = bleClient->getService(motionServiceUuid);
  if (service == nullptr) {
    bleClient->disconnect();
    return false;
  }

  activityCharacteristic = service->getCharacteristic(
      activityCharacteristicUuid
  );
  if (
      activityCharacteristic == nullptr
      || !activityCharacteristic->canNotify()
  ) {
    bleClient->disconnect();
    return false;
  }

  activityCharacteristic->registerForNotify(activityNotifyCallback);
  if (!bleClient->isConnected()) return false;

  bleReceiverReady = true;
  Serial.println("[BLE] Receiving CatMotion activity notifications");
  return true;
}

void startBleScan() {
  bleScanRequested = false;
  bleScan->clearResults();
  Serial.printf("[BLE] Scanning for %s...\n", TARGET_DEVICE_NAME);

  if (!bleScan->start(BLE_SCAN_SECONDS, bleScanComplete, false)) {
    bleScanRequested = true;
    nextBleScanAt = millis() + BLE_RESCAN_DELAY_MS;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Cat BLE to public WebSocket bridge - ESP32-C3 SuperMini");

  activityQueue = xQueueCreate(1, sizeof(ActivityPacket));
  if (activityQueue == nullptr) {
    Serial.println("[FATAL] Failed to create activity queue");
    while (true) delay(1000);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  lastWifiAttemptAt = millis() - WIFI_RECONNECT_INTERVAL_MS;
  maintainWifi();

  BLEDevice::init(BLE_RECEIVER_NAME);
  const esp_err_t defaultPowerResult = esp_ble_tx_power_set_enhanced(
      ESP_BLE_ENHANCED_PWR_TYPE_DEFAULT,
      0,
      BLE_TX_POWER_LEVEL
  );
  const esp_err_t scanPowerResult = esp_ble_tx_power_set_enhanced(
      ESP_BLE_ENHANCED_PWR_TYPE_SCAN,
      0,
      BLE_TX_POWER_LEVEL
  );
  const esp_err_t initPowerResult = esp_ble_tx_power_set_enhanced(
      ESP_BLE_ENHANCED_PWR_TYPE_INIT,
      0,
      BLE_TX_POWER_LEVEL
  );
  if (
      defaultPowerResult == ESP_OK
      && scanPowerResult == ESP_OK
      && initPowerResult == ESP_OK
  ) {
    Serial.println("[BLE] TX power set to +9 dBm");
  } else {
    Serial.printf(
        "[BLE] TX power setup failed: default=%s, scan=%s, init=%s\n",
        esp_err_to_name(defaultPowerResult),
        esp_err_to_name(scanPowerResult),
        esp_err_to_name(initPowerResult)
    );
  }

  bleScan = BLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(new TargetScanCallbacks());
  bleScan->setInterval(1349);
  bleScan->setWindow(449);
  bleScan->setActiveScan(true);

  bleScanRequested = true;
  nextBleScanAt = 0;
}

void loop() {
  maintainWifi();
  if (relayStarted) relaySocket.loop();

  ActivityPacket packet{};
  while (xQueueReceive(activityQueue, &packet, 0) == pdTRUE) {
    handleActivityPacket(packet);
  }

  if (bleConnectRequested) {
    bleConnectRequested = false;
    if (!connectToCatMotion()) {
      Serial.println("[BLE] Connection failed; retrying");
      bleReceiverReady = false;
      bleScanRequested = true;
      nextBleScanAt = millis() + BLE_RESCAN_DELAY_MS;
    }
  }

  if (
      !bleReceiverReady
      && !bleConnectRequested
      && bleScanRequested
      && !bleScan->isScanning()
      && static_cast<int32_t>(millis() - nextBleScanAt) >= 0
  ) {
    startBleScan();
  }

  delay(5);
}
