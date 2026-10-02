#include <ArduinoBLE.h>
#include <ble/ll_api.h>
#include <utility/ATT.h>
#include <Wire.h>
#include <LSM6DS3.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>

// XIAO nRF52840 Sense 板载 IMU
LSM6DS3 imu(I2C_MODE, 0x6A);

// 自定义 BLE 服务和数据通道 UUID。
// Python 接收端必须使用相同的 Characteristic UUID。
constexpr char DEVICE_NAME[] =
    "CatMotion-01";

constexpr char SERVICE_UUID[] =
    "5f2b0001-8a2f-4b7e-9d3c-112233445566";

constexpr char IMU_CHARACTERISTIC_UUID[] =
    "5f2b0002-8a2f-4b7e-9d3c-112233445566";

constexpr char ACTIVITY_CHARACTERISTIC_UUID[] =
    "5f2b0003-8a2f-4b7e-9d3c-112233445566";

// 默认同时发送：
// 1. 50 Hz 原始六轴数据，兼容原来的 ble_receive.py 和 CSV 格式。
// 2. 每 2 秒一次的本地分类结果。
// 如果将来只需要分类结果，可改为 false 以降低 BLE 流量。
constexpr bool STREAM_RAW_IMU = false;

// 长时间采集时不要持续写 USB 串口；无人读取可能填满缓冲区并阻塞采样。
constexpr bool DEBUG_ACTIVITY_SERIAL = false;

constexpr int8_t BLE_TX_POWER_DBM = 8;
constexpr uint32_t BATTERY_LOG_INTERVAL_MS = 60000;
constexpr uint8_t BATTERY_PERCENT_UNKNOWN = 255;
constexpr size_t BATTERY_ADC_SAMPLES = 8;
constexpr float BATTERY_ADC_REFERENCE_MV = 3300.0f;
// 板载分压电阻为 1M / 510K，ADC 读到的是电池电压的约 1/2.96。
constexpr float BATTERY_DIVIDER_RATIO = 1510.0f / 510.0f;

// 固定16字节数据包
struct __attribute__((packed)) ImuPacket {
  uint32_t timestamp_ms;

  // 单位：mg，即 g × 1000
  int16_t ax_mg;
  int16_t ay_mg;
  int16_t az_mg;

  // 单位：0.1 dps，即 dps × 10
  int16_t gx_dps10;
  int16_t gy_dps10;
  int16_t gz_dps10;
};

static_assert(
    sizeof(ImuPacket) == 16,
    "ImuPacket must be exactly 16 bytes"
);

enum ActivityState : uint8_t {
  RESTING_CANDIDATE = 0,
  WALKING_CANDIDATE = 1,
  IMPACT_CANDIDATE = 2,
  VIGOROUS_ACTIVITY = 3,
  OTHER_ACTIVITY = 4,
  WARMING_UP = 255,
};

// 每 2 秒发送一次，固定 16 字节。
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

struct WindowFeatures {
  float accelStdG;
  float accelP2pG;
  float accelMaxG;
  float gyroRmsDps;
  float gyroMaxDps;
  float periodicity;
};

// BLE 服务
BLEService motionService(SERVICE_UUID);

// BLE 数据通道：允许读取和 Notify，固定长度16字节
BLECharacteristic imuCharacteristic(
    IMU_CHARACTERISTIC_UUID,
    BLERead | BLENotify,
    sizeof(ImuPacket),
    true
);

BLECharacteristic activityCharacteristic(
    ACTIVITY_CHARACTERISTIC_UUID,
    BLERead | BLENotify,
    sizeof(ActivityPacket),
    true
);

constexpr uint32_t SAMPLE_INTERVAL_MS = 20;  // 50Hz
constexpr size_t WINDOW_SAMPLES = 100;       // 2秒
constexpr size_t WINDOW_STEP_SAMPLES = 100;  // 2秒
uint32_t nextSampleTime = 0;
uint32_t nextBatteryLogTime = 0;

float accelMagnitudeWindow[WINDOW_SAMPLES]{};
float gyroMagnitudeWindow[WINDOW_SAMPLES]{};
size_t windowWriteIndex = 0;
size_t windowSampleCount = 0;
size_t samplesSinceClassification = 0;

// 使用 cat_imu_ble.csv 和 cat_imu_ble2.csv 调整；
// cat_imu_ble3.csv 仅用于留出验证。
constexpr float REST_ACCEL_STD_MAX_G = 0.025f;
constexpr float REST_ACCEL_P2P_MAX_G = 0.20f;
constexpr float REST_GYRO_RMS_MAX_DPS = 25.0f;

// 根据已标注为蹲着/静息的 cat_imu_ble1～4 调整，
// cat_imu_ble5 整份留作验证。
constexpr float RELAXED_REST_ACCEL_STD_MAX_G = 0.06f;
constexpr float RELAXED_REST_ACCEL_P2P_MAX_G = 0.40f;
constexpr float RELAXED_REST_GYRO_RMS_MAX_DPS = 80.0f;
constexpr float RELAXED_REST_GYRO_MAX_DPS = 350.0f;

constexpr float IMPACT_ACCEL_MAX_MIN_G = 2.20f;
constexpr float IMPACT_ACCEL_P2P_MIN_G = 1.50f;

constexpr float VIGOROUS_ACCEL_STD_MIN_G = 0.25f;
constexpr float VIGOROUS_GYRO_RMS_MIN_DPS = 160.0f;
constexpr float VIGOROUS_GYRO_MAX_MIN_DPS = 450.0f;

constexpr float WALKING_ACCEL_STD_MIN_G = 0.04f;
constexpr float WALKING_ACCEL_STD_MAX_G = 0.22f;
constexpr float WALKING_GYRO_RMS_MIN_DPS = 30.0f;
constexpr float WALKING_GYRO_RMS_MAX_DPS = 180.0f;
constexpr float WALKING_PERIODICITY_MIN = 0.35f;

struct BatteryCurvePoint {
  uint16_t millivolts;
  uint8_t percent;
};

// 单节锂电池的近似静置电压曲线，桌面端应将百分比视为估算值。
constexpr BatteryCurvePoint BATTERY_CURVE[] = {
  {3300, 0},
  {3500, 5},
  {3600, 10},
  {3700, 20},
  {3750, 30},
  {3800, 40},
  {3850, 50},
  {3900, 60},
  {3950, 70},
  {4000, 80},
  {4100, 90},
  {4200, 100},
};

uint16_t readBatteryMillivolts() {
  uint32_t rawSum = 0;
  for (size_t index = 0; index < BATTERY_ADC_SAMPLES; ++index) {
    rawSum += analogRead(PIN_VBAT);
  }

  const float rawAverage =
      static_cast<float>(rawSum) / BATTERY_ADC_SAMPLES;
  return static_cast<uint16_t>(lroundf(
      rawAverage
      * BATTERY_ADC_REFERENCE_MV
      * BATTERY_DIVIDER_RATIO
      / 4095.0f
  ));
}

uint8_t batteryPercentFromMillivolts(uint16_t millivolts) {
  if (millivolts < 2500 || millivolts > 4500) {
    return BATTERY_PERCENT_UNKNOWN;
  }

  if (millivolts <= BATTERY_CURVE[0].millivolts) return 0;

  constexpr size_t pointCount =
      sizeof(BATTERY_CURVE) / sizeof(BATTERY_CURVE[0]);
  if (millivolts >= BATTERY_CURVE[pointCount - 1].millivolts) {
    return 100;
  }

  for (size_t index = 1; index < pointCount; ++index) {
    const BatteryCurvePoint& upper = BATTERY_CURVE[index];
    if (millivolts > upper.millivolts) continue;

    const BatteryCurvePoint& lower = BATTERY_CURVE[index - 1];
    const uint32_t voltageOffset = millivolts - lower.millivolts;
    const uint32_t voltageRange = upper.millivolts - lower.millivolts;
    const uint32_t percentRange = upper.percent - lower.percent;
    return lower.percent + static_cast<uint8_t>(
        (voltageOffset * percentRange + voltageRange / 2)
        / voltageRange
    );
  }

  return BATTERY_PERCENT_UNKNOWN;
}

uint8_t readBatteryPercent() {
  return batteryPercentFromMillivolts(readBatteryMillivolts());
}

void logBatteryStatus() {
  if (!Serial) {
    return;
  }

  const uint16_t batteryMillivolts = readBatteryMillivolts();
  const uint8_t batteryPercent = batteryPercentFromMillivolts(
      batteryMillivolts
  );
  const bool charging = digitalRead(P0_17) == LOW;

  Serial.print("Battery: ");
  Serial.print(batteryMillivolts / 1000.0f, 2);
  Serial.print(" V, ");

  if (batteryPercent == BATTERY_PERCENT_UNKNOWN) {
    Serial.print("unknown");
  } else {
    Serial.print(batteryPercent);
    Serial.print("%");
  }

  Serial.print(", charge: ");
  if (charging) {
    Serial.println("charging at 50 mA");
  } else {
    Serial.println("not charging or full");
  }
}

bool parseBleAddress(const String& text, uint8_t address[6]) {
  unsigned int displayedBytes[6]{};
  const int parsedCount = sscanf(
      text.c_str(),
      "%x:%x:%x:%x:%x:%x",
      &displayedBytes[0],
      &displayedBytes[1],
      &displayedBytes[2],
      &displayedBytes[3],
      &displayedBytes[4],
      &displayedBytes[5]
  );

  if (parsedCount != 6) {
    return false;
  }

  // BLEDevice::address() 按人类阅读顺序显示，ATT 内部则低字节在前。
  for (size_t index = 0; index < 6; ++index) {
    if (displayedBytes[index] > UINT8_MAX) {
      return false;
    }
    address[5 - index] = static_cast<uint8_t>(displayedBytes[index]);
  }

  return true;
}

void configureAdvertisingTxPower() {
  // CordioHCIDriver::set_tx_power() 在当前 Seeed Mbed 中未实现，
  // 直接使用 nRF52840 所在的 Cordio Link Layer 设置广播功率。
  LlSetAdvTxPower(BLE_TX_POWER_DBM);

  int8_t configuredPower = 0;
  const uint8_t result = LlGetAdvTxPower(&configuredPower);

  if (result == LL_SUCCESS && configuredPower == BLE_TX_POWER_DBM) {
    Serial.print("BLE advertising TX power: +");
    Serial.print(configuredPower);
    Serial.println(" dBm");
    return;
  }

  Serial.print("WARNING: BLE advertising TX power setup failed, code ");
  Serial.println(result);
}

void configureConnectionTxPower(const BLEDevice& central) {
  uint8_t address[6]{};
  if (!parseBleAddress(central.address(), address)) {
    Serial.println("WARNING: Cannot parse BLE receiver address");
    return;
  }

  uint16_t connectionHandle = 0xffff;

  // Cordio 可能记录 public、random 或 identity 地址类型。
  for (uint8_t addressType = 0; addressType <= 3; ++addressType) {
    connectionHandle = ATT.connectionHandle(addressType, address);
    if (connectionHandle != 0xffff) {
      break;
    }
  }

  if (connectionHandle == 0xffff) {
    Serial.println("WARNING: Cannot find BLE connection handle");
    return;
  }

  const uint8_t result = LlSetAllPhyTxPowerLevel(
      connectionHandle,
      BLE_TX_POWER_DBM
  );

  if (result == LL_SUCCESS) {
    Serial.print("BLE connection TX power: +");
    Serial.print(BLE_TX_POWER_DBM);
    Serial.println(" dBm");
    return;
  }

  Serial.print("WARNING: BLE connection TX power setup failed, code ");
  Serial.println(result);
}

int16_t scaleToInt16(float value, float scale) {
  const long scaled = lroundf(value * scale);

  if (scaled > INT16_MAX) {
    return INT16_MAX;
  }

  if (scaled < INT16_MIN) {
    return INT16_MIN;
  }

  return static_cast<int16_t>(scaled);
}

uint16_t scaleToUint16(float value, float scale) {
  const long scaled = lroundf(value * scale);

  if (scaled <= 0) {
    return 0;
  }

  if (scaled > UINT16_MAX) {
    return UINT16_MAX;
  }

  return static_cast<uint16_t>(scaled);
}

float chronologicalValue(const float* values, size_t index) {
  // 窗口填满后，windowWriteIndex 指向最旧样本。
  return values[(windowWriteIndex + index) % WINDOW_SAMPLES];
}

float calculatePeriodicity(float accelMean) {
  float bestCorrelation = 0.0f;

  // 50 Hz 下检查约 0.24～1.00 秒的周期。
  for (size_t lag = 12; lag <= 50; ++lag) {
    float sumXY = 0.0f;
    float sumX2 = 0.0f;
    float sumY2 = 0.0f;

    for (size_t index = 0; index + lag < WINDOW_SAMPLES; ++index) {
      const float x = chronologicalValue(
          accelMagnitudeWindow,
          index
      ) - accelMean;
      const float y = chronologicalValue(
          accelMagnitudeWindow,
          index + lag
      ) - accelMean;

      sumXY += x * y;
      sumX2 += x * x;
      sumY2 += y * y;
    }

    const float denominator = sqrtf(sumX2 * sumY2);
    if (denominator <= 0.00000001f) {
      continue;
    }

    const float correlation = sumXY / denominator;
    if (correlation > bestCorrelation) {
      bestCorrelation = correlation;
    }
  }

  return bestCorrelation;
}

WindowFeatures calculateWindowFeatures() {
  float accelSum = 0.0f;
  float gyroSquareSum = 0.0f;
  float accelMin = chronologicalValue(accelMagnitudeWindow, 0);
  float accelMax = accelMin;
  float gyroMax = 0.0f;

  for (size_t index = 0; index < WINDOW_SAMPLES; ++index) {
    const float accel = chronologicalValue(
        accelMagnitudeWindow,
        index
    );
    const float gyro = chronologicalValue(
        gyroMagnitudeWindow,
        index
    );

    accelSum += accel;
    gyroSquareSum += gyro * gyro;
    accelMin = fminf(accelMin, accel);
    accelMax = fmaxf(accelMax, accel);
    gyroMax = fmaxf(gyroMax, gyro);
  }

  const float accelMean = accelSum / WINDOW_SAMPLES;
  float accelVarianceSum = 0.0f;

  for (size_t index = 0; index < WINDOW_SAMPLES; ++index) {
    const float difference = chronologicalValue(
        accelMagnitudeWindow,
        index
    ) - accelMean;
    accelVarianceSum += difference * difference;
  }

  WindowFeatures features{};
  features.accelStdG = sqrtf(
      accelVarianceSum / WINDOW_SAMPLES
  );
  features.accelP2pG = accelMax - accelMin;
  features.accelMaxG = accelMax;
  features.gyroRmsDps = sqrtf(
      gyroSquareSum / WINDOW_SAMPLES
  );
  features.gyroMaxDps = gyroMax;
  features.periodicity = calculatePeriodicity(accelMean);

  return features;
}

ActivityState classifyWindow(const WindowFeatures& features) {
  if (
      features.accelStdG <= REST_ACCEL_STD_MAX_G
      && features.accelP2pG <= REST_ACCEL_P2P_MAX_G
      && features.gyroRmsDps <= REST_GYRO_RMS_MAX_DPS
  ) {
    return RESTING_CANDIDATE;
  }

  if (
      features.accelMaxG >= IMPACT_ACCEL_MAX_MIN_G
      && features.accelP2pG >= IMPACT_ACCEL_P2P_MIN_G
  ) {
    return IMPACT_CANDIDATE;
  }

  if (
      features.accelStdG >= VIGOROUS_ACCEL_STD_MIN_G
      || features.gyroRmsDps >= VIGOROUS_GYRO_RMS_MIN_DPS
      || features.gyroMaxDps >= VIGOROUS_GYRO_MAX_MIN_DPS
  ) {
    return VIGOROUS_ACTIVITY;
  }

  if (
      features.accelStdG >= WALKING_ACCEL_STD_MIN_G
      && features.accelStdG <= WALKING_ACCEL_STD_MAX_G
      && features.gyroRmsDps >= WALKING_GYRO_RMS_MIN_DPS
      && features.gyroRmsDps <= WALKING_GYRO_RMS_MAX_DPS
      && features.periodicity >= WALKING_PERIODICITY_MIN
  ) {
    return WALKING_CANDIDATE;
  }

  // 保持蹲姿时允许偶尔转头。放在走路判断之后，
  // 避免把有明显周期性的活动吞进静息。
  if (
      features.accelStdG <= RELAXED_REST_ACCEL_STD_MAX_G
      && features.accelP2pG <= RELAXED_REST_ACCEL_P2P_MAX_G
      && features.gyroRmsDps <= RELAXED_REST_GYRO_RMS_MAX_DPS
      && features.gyroMaxDps <= RELAXED_REST_GYRO_MAX_DPS
  ) {
    return RESTING_CANDIDATE;
  }

  return OTHER_ACTIVITY;
}

const char* activityName(ActivityState state) {
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
    default:
      return "warming_up";
  }
}

void publishActivity(
    uint32_t timestampMs,
    ActivityState state,
    const WindowFeatures& features
) {
  ActivityPacket packet{};
  packet.timestamp_ms = timestampMs;
  packet.state = static_cast<uint8_t>(state);
  packet.protocol_version = 1;
  packet.accel_std_mg = scaleToUint16(
      features.accelStdG,
      1000.0f
  );
  packet.accel_p2p_mg = scaleToUint16(
      features.accelP2pG,
      1000.0f
  );
  packet.accel_max_mg = scaleToUint16(
      features.accelMaxG,
      1000.0f
  );
  packet.gyro_rms_dps10 = scaleToUint16(
      features.gyroRmsDps,
      10.0f
  );
  packet.periodicity_percent = static_cast<uint8_t>(
      fminf(fmaxf(features.periodicity, 0.0f), 1.0f) * 100.0f
  );
  packet.battery_percent = readBatteryPercent();

  activityCharacteristic.writeValue(
      reinterpret_cast<const uint8_t*>(&packet),
      sizeof(packet)
  );

  if (DEBUG_ACTIVITY_SERIAL && Serial) {
    Serial.print(timestampMs);
    Serial.print(",");
    Serial.println(activityName(state));
  }
}

void addWindowSample(
    uint32_t timestampMs,
    float accelMagnitude,
    float gyroMagnitude
) {
  accelMagnitudeWindow[windowWriteIndex] = accelMagnitude;
  gyroMagnitudeWindow[windowWriteIndex] = gyroMagnitude;
  windowWriteIndex = (windowWriteIndex + 1) % WINDOW_SAMPLES;

  if (windowSampleCount < WINDOW_SAMPLES) {
    ++windowSampleCount;
  }

  ++samplesSinceClassification;

  if (
      windowSampleCount < WINDOW_SAMPLES
      || samplesSinceClassification < WINDOW_STEP_SAMPLES
  ) {
    return;
  }

  samplesSinceClassification = 0;
  const WindowFeatures features = calculateWindowFeatures();
  publishActivity(
      timestampMs,
      classifyWindow(features),
      features
  );
}

void stopWithError(const char* message) {
  Serial.println(message);

  while (true) {
    digitalWrite(LED_BUILTIN, LOW);
    delay(100);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(100);
  }
}

void setup() {
  // XIAO nRF52840 Sense 充电电流控制
  // HIGH = 50mA，LOW = 100mA
  pinMode(P0_13, OUTPUT);
  digitalWrite(P0_13, HIGH);

  // 充电芯片状态脚为低电平时，表示正在充电。
  pinMode(P0_17, INPUT);

  // LOW 开启板载电池分压读取通路。Seeed 建议读取时保持 LOW。
  pinMode(PIN_VBAT_ENABLE, OUTPUT);
  digitalWrite(PIN_VBAT_ENABLE, LOW);
  analogReadResolution(12);
  analogReference(AR_VDD);
  analogAcquisitionTime(AT_40_US);
    
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  Serial.begin(115200);

  // 最多等待串口3秒。
  // 将来使用电池启动时，不会永远卡在这里。
  const uint32_t serialWaitStart = millis();

  while (!Serial && millis() - serialWaitStart < 3000) {
    delay(10);
  }

  Serial.println("Starting CatMotion BLE...");

  if (imu.begin() != 0) {
    stopWithError("ERROR: IMU init failed");
  }

  Serial.println("IMU OK");

  if (!BLE.begin()) {
    stopWithError("ERROR: BLE init failed");
  }

  configureAdvertisingTxPower();

  // ESP32-C3 接收端扫描时看到的蓝牙名称
  BLE.setLocalName(DEVICE_NAME);

  // 广播 Motion Service
  BLE.setAdvertisedService(motionService);

  // BLE 单位为 1.25ms，请求 7.5～15ms 连接间隔。
  // 该间隔可以稳定承载 50Hz 原始数据通知。
  BLE.setConnectionInterval(6, 12);

  // 将 IMU 数据通道加入 Service
  motionService.addCharacteristic(imuCharacteristic);
  motionService.addCharacteristic(activityCharacteristic);

  // 将 Service 加入 BLE
  BLE.addService(motionService);

  // 设置一个初始空数据包
  ImuPacket initialPacket{};

  imuCharacteristic.writeValue(
      reinterpret_cast<const uint8_t*>(&initialPacket),
      sizeof(initialPacket)
  );

  ActivityPacket initialActivityPacket{};
  initialActivityPacket.state = WARMING_UP;
  initialActivityPacket.protocol_version = 1;
  initialActivityPacket.battery_percent = readBatteryPercent();

  activityCharacteristic.writeValue(
      reinterpret_cast<const uint8_t*>(&initialActivityPacket),
      sizeof(initialActivityPacket)
  );

  // 开始广播
  BLE.advertise();

  Serial.print("BLE device name: ");
  Serial.println(DEVICE_NAME);

  Serial.print("BLE address: ");
  Serial.println(BLE.address());

  Serial.println("Local classification is running...");

  nextSampleTime = millis();
  nextBatteryLogTime = nextSampleTime;
}

void loop() {
  BLE.poll();

  BLEDevice central = BLE.central();
  bool connected = false;

  if (central) {
    connected = central.connected();
  }

  static bool wasConnected = false;

  if (connected && !wasConnected) {
    Serial.print("Connected: ");
    Serial.println(central.address());
    configureConnectionTxPower(central);
  } else if (!connected && wasConnected) {
    Serial.println("BLE receiver disconnected");
  }

  wasConnected = connected;

  const uint32_t now = millis();

  if (static_cast<int32_t>(now - nextBatteryLogTime) >= 0) {
    logBatteryStatus();
    nextBatteryLogTime = now + BATTERY_LOG_INTERVAL_MS;
  }

  if (static_cast<int32_t>(now - nextSampleTime) < 0) {
    return;
  }

  // 如果串口或 BLE 曾造成长延迟，不快速补读旧样本。
  if (now - nextSampleTime > SAMPLE_INTERVAL_MS * 2) {
    nextSampleTime = now + SAMPLE_INTERVAL_MS;
  } else {
    nextSampleTime += SAMPLE_INTERVAL_MS;
  }

  const float ax = imu.readFloatAccelX();
  const float ay = imu.readFloatAccelY();
  const float az = imu.readFloatAccelZ();
  const float gx = imu.readFloatGyroX();
  const float gy = imu.readFloatGyroY();
  const float gz = imu.readFloatGyroZ();

  addWindowSample(
      now,
      sqrtf(ax * ax + ay * ay + az * az),
      sqrtf(gx * gx + gy * gy + gz * gz)
  );

  if (STREAM_RAW_IMU && connected) {
    ImuPacket packet{};
    packet.timestamp_ms = now;
    packet.ax_mg = scaleToInt16(ax, 1000.0f);
    packet.ay_mg = scaleToInt16(ay, 1000.0f);
    packet.az_mg = scaleToInt16(az, 1000.0f);
    packet.gx_dps10 = scaleToInt16(gx, 10.0f);
    packet.gy_dps10 = scaleToInt16(gy, 10.0f);
    packet.gz_dps10 = scaleToInt16(gz, 10.0f);

    imuCharacteristic.writeValue(
        reinterpret_cast<const uint8_t*>(&packet),
        sizeof(packet)
    );
  }
}
