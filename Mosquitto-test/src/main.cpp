/**
 * ESP32-S3 Sensor Box: Microphone + DHT11 + MQ135 + Speaker + OLED
 * - Analog microphone (electret) on GPIO4
 * - DHT11 temperature/humidity on GPIO16 (Adafruit DHT library)
 * - MQ135 gas sensor (AO via voltage divider) on GPIO18
 * - PWM speaker on GPIO13
 * - I2C OLED display (SSD1306 128x64): SDA on GPIO10, SCL on GPIO9
 *
 * Mic detection uses a peak window with a consecutive-window debounce:
 * each window tracks the maximum deviation from a slowly tracked DC baseline;
 * a trigger requires that peak to clear NOISE_THRESHOLD for
 * MIC_TRIGGER_WINDOWS consecutive windows, so a single spike alone won't fire
 * it and the decision does not depend on the waveform's average/crest factor.
 * The electret module has a DC bias, so samples are converted to their
 * deviation from the baseline before the peak is taken.
 * Raw peak-to-peak and average are kept only as diagnostics.
 */

#include <Arduino.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

/** Pin definitions */
#define VOLUME_SENSOR_PIN 4   // ADC input for electret microphone module
#define TEMP_HUMIDITY_PIN 16  // DHT11 data pin
#define GAS_SENSOR_PIN 18     // ADC input for MQ135 (Flying-Fish module, AO pin)
#define SPEAKER_PIN 13        // PWM speaker driver pin
#define OLED_SDA 10           // OLED I2C data line (SDA)
#define OLED_SCL 9            // OLED I2C clock line (SCL)

/** OLED (SSD1306) configuration */
#define SCREEN_WIDTH 128      // OLED width in pixels
#define SCREEN_HEIGHT 64      // OLED height in pixels (change to 32 for the short version)
#define OLED_RESET -1         // No dedicated reset pin (share Arduino reset)
#define OLED_ADDR_DEFAULT 0x3C // Fallback 7-bit address (0x78 on the silkscreen)

/** ADC constants */
#define ADC_RESOLUTION 4095.0   // ESP32-S3 12-bit ADC max value
#define VREF 3.3                // Reference voltage in volts

/**
 * Noise threshold in ADC counts for the peak deviation of a window.
 * Calibrate with the printed "peak / avg / p-p" values: peak must stay low
 * in silence and rise clearly when noisy. Because the old average-confirmation
 * step is gone, this is now the actual peak threshold; raise it if the alarm
 * becomes too sensitive.
 */
#define NOISE_THRESHOLD 60

/** MQ135 configuration */
#define GAS_VCC 5.0             // MQ135 module supply voltage (V)
#define GAS_DIVIDER_RATIO 2.0   // Voltage divider: 10k+10k = /2, so multiply by 2 to get module voltage
#define GAS_RO_CLEAN_AIR 10000.0 // Approximate Ro in clean air (ohms) - needs calibration
#define GAS_RL 10000.0          // Load resistor on module (ohms)

/**
 * Air-quality index thresholds on Rs/Ro (lower ratio = more gas = worse).
 * Great >= 0.55, Good >= 0.35, Bad >= 0.20, otherwise Worse.
 * Lowered so clean air (~0.45 here) does not read "Bad"; tune as needed.
 */
#define AQ_GREAT_THRESHOLD 0.55
#define AQ_GOOD_THRESHOLD  0.35
#define AQ_BAD_THRESHOLD   0.20

/** Sequence limits */
#define MAX_STEPS 32            // Max steps in a beep sequence

/** Read scheduling (mostly non-blocking) */
const uint32_t MIC_INTERVAL_MS = 100;  // How often the mic is measured (ms)
const uint32_t MIC_WINDOW_MS = 50;     // Mic window (ms): >=1 cycle down to 20 Hz
const uint32_t DHT_INTERVAL_MS = 2000; // How often the DHT11 is read (ms)
const uint32_t DHT_RETRY_MS = 1000;    // Delay before a failed DHT read is retried (ms)
const uint32_t GAS_INTERVAL_MS = 1000; // How often the MQ135 is read (ms)
const uint32_t PRINT_INTERVAL_MS = 1000; // How often all values are printed together (ms)
const uint32_t DISPLAY_SWITCH_MS = 2000; // How often the OLED switches value pairs (ms)
const uint32_t DISPLAY_REFRESH_NORMAL_MS = 500; // OLED refresh in normal pages (ms)
const uint32_t DISPLAY_REFRESH_ALERT_MS = 100;  // OLED refresh while alert flashes (ms)
const uint32_t ALERT_FLASH_MS = 200;     // Alert flash half-period (ms)
const uint32_t ALERT_HOLD_MS = 5000;     // Alarm stays active this long after last noise (ms)
const uint32_t SPEAKER_GUARD_MS = 50;    // Ignore mic this long after a beep ends (ms)
const uint32_t BEEP_GAP_MS = 300;        // Silence between beep sequences (ms)
const uint8_t MIC_TRIGGER_WINDOWS = 2;   // Consecutive over-threshold windows needed to trigger
const uint8_t MAX_READ_RETRIES = 3;      // Total DHT attempts before marking it invalid

/** DHT sensor type */
#define DHT_TYPE DHT11

/** LEDC channel for speaker (ESP32 Arduino core v2 API) */
#define SPK_CH 0

/** DHT sensor instance - uses direct bit-banging for ESP32 */
DHT dht(TEMP_HUMIDITY_PIN, DHT_TYPE);

/** OLED display instance */
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
uint8_t oledAddr = OLED_ADDR_DEFAULT; // Actual address, set by the I2C scan
bool displayReady = false;            // True once display.begin() succeeds

/** Sequencer state */
struct Step { uint32_t hz; uint32_t ms; };
Step seq[MAX_STEPS];
int seqLen = 0;      // Number of steps in current sequence
int seqPos = 0;      // Current step index
bool playing = false; // True while a sequence is active
unsigned long stepStart = 0; // Timestamp of current step start
uint32_t lastSpeakerEnd = 0; // Timestamp when the last beep sequence ended

/** Timestamps of the last read of each sensor */
uint32_t lastMicRead = 0;
uint32_t lastDhtRead = 0;
uint32_t lastGasRead = 0;
uint32_t lastPrint = 0;
uint32_t lastPageSwitch = 0;
uint32_t lastDisplay = 0;
uint8_t  displayPage = 0;   // 0 = Mic + Air, 1 = Temp + Hum
bool     alertActive = false; // True while the noise level is above the threshold
uint32_t lastNoiseTime = 0;   // Timestamp of last noise trigger
bool     alarmLatched = false; // Latches true after first trigger, cleared after hold
uint8_t  noiseWindowCount = 0; // Consecutive windows above the threshold
uint8_t  dhtFailCount = 0;     // Consecutive DHT failures in the current cycle

/** Latest sensor values, kept so all can be printed together */
int   micLevel      = 0;      // Decision level compared to NOISE_THRESHOLD
int   micMax        = 0;      // Window maximum (diagnostic)
int   micAvg        = 0;      // Window average (diagnostic)
int   micP2P        = 0;      // Window peak-to-peak (diagnostic)
float micVoltage    = 0.0;    // Voltage of the decision level
int   micBaseline   = -1;     // Resting DC offset of the mic, -1 = not set yet
float dhtHumidity   = NAN;    // Last valid humidity
float dhtTemperature = NAN;   // Last valid temperature
bool  dhtValid      = false;  // Whether a valid DHT reading is available
float gasVoltage    = 0.0;    // Last MQ135 AO voltage
float gasRsRo       = 0.0;    // Last MQ135 Rs/Ro ratio

/**
 * @brief Initialize the LEDC timer and attach speaker pin
 * Sets up 2kHz PWM on the speaker channel, starts with tone off.
 */
void spkInit() {
  ledcSetup(SPK_CH, 2000, 8);   // channel 0, 2kHz, 8-bit resolution
  ledcAttachPin(SPEAKER_PIN, SPK_CH);
  ledcWriteTone(SPK_CH, 0);     // speaker off
}

/**
 * @brief Short beep at given frequency for duration
 * @param hz Frequency in hertz
 * @param ms Duration in milliseconds
 */
void spkBeep(uint32_t hz, uint32_t ms) {
  ledcWriteTone(SPK_CH, hz);
  delay(ms);
  ledcWriteTone(SPK_CH, 0);
}

/**
 * @brief Start the first step of the current sequence
 * Writes the first tone and records the start time.
 */
void startStep() {
  ledcWriteTone(SPK_CH, seq[seqPos].hz);
  stepStart = millis();
}

/**
 * @brief Scan the I2C bus for a connected device.
 * Prints every address that answers, and returns the first one found
 * (preferring the usual OLED addresses 0x3C / 0x3D when present).
 * @return The 7-bit address of the first device, or 0 if none found
 */
uint8_t findI2CDevice() {
  uint8_t firstFound = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("I2C device found at 0x%02X\n", addr);
      if (firstFound == 0) firstFound = addr;
      if (addr == 0x3C || addr == 0x3D) return addr; // Prefer an OLED
    }
  }
  return firstFound;
}

/**
 * @brief Arduino setup: initialize serial, pins, sensors, and speaker
 */
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("ESP32-S3 Sensor Box - Serial Monitor");
  Serial.println("=====================================");
  Serial.print("Microphone (ADC) on pin: ");
  Serial.println(VOLUME_SENSOR_PIN);
  Serial.print("DHT11 on pin: ");
  Serial.println(TEMP_HUMIDITY_PIN);
  Serial.print("MQ135 (ADC) on pin: ");
  Serial.println(GAS_SENSOR_PIN);
  Serial.print("Speaker on pin: ");
  Serial.println(SPEAKER_PIN);
  Serial.print("OLED SDA/SCL on pins: ");
  Serial.print(OLED_SDA); Serial.print("/"); Serial.println(OLED_SCL);
  Serial.println("Using Adafruit DHT library");
  Serial.println();

  pinMode(VOLUME_SENSOR_PIN, INPUT);
  pinMode(GAS_SENSOR_PIN, INPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db); // Explicit ~0-3.1 V range for both ADC inputs

  // Start I2C on the OLED pins and auto-detect the display address
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000); // Faster OLED refresh; most SSD1306 modules support 400 kHz
  Serial.printf("Scanning I2C (SDA=%d, SCL=%d)...\n", OLED_SDA, OLED_SCL);
  uint8_t found = findI2CDevice();
  if (found != 0) {
    oledAddr = found;
  }
  Serial.printf("Using OLED address 0x%02X\n", oledAddr);
  displayReady = display.begin(SSD1306_SWITCHCAPVCC, oledAddr);
  if (!displayReady) {
    Serial.println("SSD1306 init FAILED (check wiring/address/controller)");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Sensor Box");
    display.println("OLED ready");
    display.display();
  }

  spkInit();

  dht.begin();

  Serial.print("DHT11 warming up... ");
  delay(2000);
  Serial.println("done");
  Serial.println();

  lastPageSwitch = millis();  // Start the OLED on page 0 for a full interval
}

/** Result of a microphone sampling window */
struct MicWindow {
  int maxLevel;    // Peak |sample - DC baseline| in the window (decision value)
  int avgLevel;    // Average |sample - DC baseline| (diagnostic only)
  int peakToPeak;  // Raw peak-to-peak amplitude (diagnostic only)
};

/**
 * @brief Sample the microphone for one window, removing the DC bias.
 * The resting offset is tracked with a slow exponential moving average; each
 * sample is converted to its absolute deviation from that offset. The window
 * returns the peak deviation (used for the decision) plus average and raw
 * peak-to-peak diagnostics.
 * @return MicWindow with AC peak/average levels and the raw peak-to-peak
 */
MicWindow readMicWindow() {
  int minValue = 4095;
  int maxValue = 0;
  int maxLevel = 0;
  uint32_t sum = 0;
  uint32_t sumLevel = 0;
  uint32_t count = 0;
  uint32_t start = micros();
  const uint32_t windowUs = MIC_WINDOW_MS * 1000UL;

  while (micros() - start < windowUs) {
    int value = analogRead(VOLUME_SENSOR_PIN);
    if (value < minValue) minValue = value;
    if (value > maxValue) maxValue = value;
    sum += value;

    if (micBaseline >= 0) {
      int level = abs(value - micBaseline);
      if (level > maxLevel) maxLevel = level;
      sumLevel += level;
    }
    count++;
  }

  int mean = (count > 0) ? (int)(sum / count) : 0;

  MicWindow result;
  result.maxLevel = maxLevel;
  result.avgLevel = (count > 0) ? (int)(sumLevel / count) : 0;
  result.peakToPeak = maxValue - minValue;

  // Track the resting DC offset slowly: follows drift, ignores short noise.
  // On the very first window there is no baseline yet, so use the window's
  // peak-to-peak as an initial level and establish the baseline afterwards.
  if (micBaseline < 0) {
    micBaseline = mean;
    result.maxLevel = (maxValue >= minValue) ? (maxValue - minValue) / 2 : 0;
    result.avgLevel = result.maxLevel;
  } else {
    micBaseline = (micBaseline * 7 + mean) / 8;
  }

  return result;
}

/** Result of the per-window peak decision */
struct MicDecision {
  int level;           // Peak level of the window
  bool aboveThreshold; // true if the peak clears NOISE_THRESHOLD
};

/**
 * @brief Compare one window's peak against the noise threshold.
 * The consecutive-window debounce is applied by the caller, so a single
 * spike cannot trigger the alarm.
 * @param w The sampled window
 * @return The window peak and whether it is above the threshold
 */
MicDecision decideMicLevel(const MicWindow &w) {
  return { w.maxLevel, w.maxLevel > NOISE_THRESHOLD };
}

/**
 * @brief Read DHT11 once, forcing a real sensor read.
 * Returns true on success, false on failure. Retry scheduling is handled by
 * loop(), so this function never blocks for long.
 * @param humidity output parameter for humidity reading
 * @param temperature output parameter for temperature reading
 * @return true if read successful, false otherwise
 */
bool readDHT(float& humidity, float& temperature) {
  // DHT::read(true) forces the actual sensor read; readHumidity() and
  // readTemperature() then return the cached values. Without force, the
  // Adafruit library returns the previous result for up to 2 s, so retrying
  // in a fast loop would keep returning the same failure.
  if (!dht.read(true)) {
    return false;
  }

  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    return false;
  }

  humidity = h;
  temperature = t;
  return true;
}

/**
 * @brief Read MQ135 gas sensor (AO pin via voltage divider)
 * Returns sensor voltage and estimated Rs/Ro ratio. Invalid readings are
 * returned as NaN so they display as "--" instead of a false "Worse".
 * @param gasVoltage output: sensor voltage at AO (0-GAS_VCC)
 * @param rsRoRatio output: Rs/Ro ratio (lower = more gas)
 * @return true if the reading is valid, false if it is out of range
 */
bool readMQ135(float& gasVoltage, float& rsRoRatio) {
  // analogReadMilliVolts() applies the ESP32 ADC calibration, so we do not
  // need to assume a 3.3 V full scale.
  float vPin = analogReadMilliVolts(GAS_SENSOR_PIN) / 1000.0f;

  // Convert back to module AO voltage (reverse voltage divider)
  gasVoltage = vPin * GAS_DIVIDER_RATIO;

  // Calculate sensor resistance Rs from voltage divider with RL:
  // Vout = Vcc * RL / (Rs + RL)  =>  Rs = RL * (Vcc - Vout) / Vout
  // where Vout = gasVoltage, Vcc = GAS_VCC (module supply).
  if (gasVoltage <= 0.01f || gasVoltage >= GAS_VCC - 0.01f) {
    rsRoRatio = NAN;
    return false;
  }

  float rs = GAS_RL * (GAS_VCC - gasVoltage) / gasVoltage;
  rsRoRatio = rs / GAS_RO_CLEAN_AIR;
  return true;
}

/**
 * @brief Translate the MQ135 Rs/Ro ratio into a qualitative air-quality index.
 * Lower Rs/Ro means more gas, i.e. worse air.
 * @param rsRoRatio Rs/Ro ratio from readMQ135()
 * @return One of "Great", "Good", "Bad", "Worse" (or "--" if invalid)
 */
const char* airQualityLabel(float rsRoRatio) {
  if (isnan(rsRoRatio)) return "--";
  if (rsRoRatio >= AQ_GREAT_THRESHOLD) return "Great";
  if (rsRoRatio >= AQ_GOOD_THRESHOLD)  return "Good";
  if (rsRoRatio >= AQ_BAD_THRESHOLD)   return "Bad";
  return "Worse";
}

/**
 * @brief Play a short ascending/descending beep sequence
 * Sequence: 1000Hz/100ms, 800Hz/100ms, 600Hz/100ms, 800Hz/100ms
 * Resets sequencer state and starts playback.
 */
void playSequence() {
  seq[0].hz = 1000; seq[0].ms = 100;
  seq[1].hz = 800;  seq[1].ms = 100;
  seq[2].hz = 600;  seq[2].ms = 100;
  seq[3].hz = 800;  seq[3].ms = 100;
  seqLen = 4;
  seqPos = 0;
  playing = true;
  startStep();
}

/**
 * @brief Stop the speaker and remember when it stopped.
 * The timestamp is used to keep the buzzer's own 600-1000 Hz beeps from being
 * counted as new noise by the microphone.
 */
void stopSpeaker(uint32_t now) {
  ledcWriteTone(SPK_CH, 0);
  if (playing) {
    playing = false;
    lastSpeakerEnd = now;
  }
}

/**
 * @brief True while the speaker is playing or its echo can still reach the mic.
 */
bool micMuted(uint32_t now) {
  if (playing) {
    return true;
  }
  return (uint32_t)(now - lastSpeakerEnd) < SPEAKER_GUARD_MS;
}

/**
 * @brief Draw the full-screen flashing noise alert.
 * Inverts the display on a timer and shows large "NOISE!" text so it
 * is impossible to miss while the beeper is triggered.
 */
void drawAlert() {
  bool flash = (millis() / ALERT_FLASH_MS) & 1;

  display.clearDisplay();
  display.invertDisplay(flash);
  display.setTextColor(SSD1306_WHITE);
  display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);

  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(28, 10);
  display.print("NOISE!");

  display.setTextSize(1);
  display.setCursor(0, 44);
  display.printf("  Mic level: %d  ", micLevel);

  display.display();
}

/**
 * @brief Render the latest sensor values on the OLED display.
 * If the noise alert is active, draws the flashing alert instead.
 * Otherwise shows two big values at a time; the pair alternates every
 * DISPLAY_SWITCH_MS: page 0 = Mic + Air, page 1 = Temp + Hum.
 * Uses the stored values, so it never blocks on sensor reads.
 */
void updateDisplay() {
  if (!displayReady) {
    return;
  }
  if (alertActive) {
    drawAlert();
    return;
  }

  display.invertDisplay(false);  // Make sure the alert's invert is cleared
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Small header showing which pair is on screen
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(displayPage == 0 ? "MIC + AIR" : "TEMP + HUM");

  // Two large values
  display.setTextSize(2);

  display.setCursor(0, 16);
  if (displayPage == 0) {
    display.printf("Mic %d", micLevel);
  } else if (dhtValid) {
    display.printf("T %.1fC", dhtTemperature);
  } else {
    display.print("T --");
  }

  display.setCursor(0, 40);
  if (displayPage == 0) {
    display.printf("Air %s", airQualityLabel(gasRsRo));
  } else if (dhtValid) {
    display.printf("H %.1f%%", dhtHumidity);
  } else {
    display.print("H --");
  }

  display.display();
}

/**
 * @main_loop Main application loop
 * Scheduling is driven by millis():
 * - Microphone measured every MIC_INTERVAL_MS (peak + debounce drives the speaker)
 * - DHT11 read every DHT_INTERVAL_MS, with non-blocking retries
 * - MQ135 read every GAS_INTERVAL_MS
 * The speaker sequencer is also driven by millis(), no long blocking delays.
 */
void loop() {
  uint32_t now = millis();

  // Advance to the next step when the current one is done
  if (playing && now - stepStart >= seq[seqPos].ms) {
    seqPos++;
    if (seqPos >= seqLen) {
      stopSpeaker(millis());
    } else {
      startStep();
    }
  }

  // Microphone: 50 ms peak window + consecutive-window debounce
  now = millis();
  if (now - lastMicRead >= MIC_INTERVAL_MS) {
    lastMicRead = now;

    MicWindow window = readMicWindow();
    MicDecision decision = decideMicLevel(window);
    now = millis(); // the sampling window has advanced time

    micLevel   = decision.level;
    micMax     = window.maxLevel;
    micAvg     = window.avgLevel;
    micP2P     = window.peakToPeak;
    micVoltage = (micLevel / ADC_RESOLUTION) * VREF;

    // The buzzer's own beeps must not re-trigger the alarm, so ignore the mic
    // while playing and briefly afterwards. Otherwise reset the debounce.
    if (decision.aboveThreshold && !micMuted(now)) {
      if (noiseWindowCount < MIC_TRIGGER_WINDOWS) {
        noiseWindowCount++;
      }
    } else {
      noiseWindowCount = 0;
    }

    if (noiseWindowCount >= MIC_TRIGGER_WINDOWS) {
      lastNoiseTime = now;
      alarmLatched = true;
    }

    alertActive = alarmLatched && (now - lastNoiseTime < ALERT_HOLD_MS);

    if (alertActive) {
      // Leave a gap between sequences so the mic can listen to the room.
      if (!playing && (uint32_t)(now - lastSpeakerEnd) >= BEEP_GAP_MS) {
        playSequence();
      }
    } else {
      stopSpeaker(now);
      alarmLatched = false;
    }
  }

  // DHT11: temperature and humidity (slow, non-blocking retry scheduling)
  now = millis();
  if (now - lastDhtRead >= DHT_INTERVAL_MS) {
    lastDhtRead = now;

    float humidity, temperature;
    if (readDHT(humidity, temperature)) {
      dhtHumidity = humidity;
      dhtTemperature = temperature;
      dhtValid = true;
      dhtFailCount = 0;
    } else {
      now = millis();
      dhtFailCount++;
      if (dhtFailCount < MAX_READ_RETRIES) {
        // Retry after DHT_RETRY_MS instead of blocking with delay().
        lastDhtRead = now - (DHT_INTERVAL_MS - DHT_RETRY_MS);
      } else {
        // Give up for this cycle; mark invalid only after repeated failures.
        dhtFailCount = 0;
        dhtValid = false;
      }
    }
  }

  // MQ135: gas sensor voltage and Rs/Ro ratio (updates the stored values)
  now = millis();
  if (now - lastGasRead >= GAS_INTERVAL_MS) {
    lastGasRead = now;
    readMQ135(gasVoltage, gasRsRo);
  }

  // Rotate the OLED value pair every DISPLAY_SWITCH_MS (only affects normal pages)
  now = millis();
  if (now - lastPageSwitch >= DISPLAY_SWITCH_MS) {
    lastPageSwitch = now;
    displayPage ^= 1;
  }

  // Refresh the OLED: fast enough for the alert flash, slower for normal pages.
  now = millis();
  uint32_t displayRefresh = alertActive ? DISPLAY_REFRESH_ALERT_MS : DISPLAY_REFRESH_NORMAL_MS;
  if (now - lastDisplay >= displayRefresh) {
    lastDisplay = now;
    updateDisplay();
  }

  // Print all values together at the same moment
  now = millis();
  if (now - lastPrint >= PRINT_INTERVAL_MS) {
    lastPrint = now;

    Serial.printf("[%lu] Mic peak=%-4d avg=%-4d p-p=%-4d -> lvl=%-4d (%.2fV) %s | ",
                  (unsigned long)now, micMax, micAvg, micP2P, micLevel, micVoltage,
                  alertActive ? "ALERT" : "ok");
    if (dhtValid) {
      Serial.printf("Hum: %5.1f%% | Temp: %5.1f C | ", dhtHumidity, dhtTemperature);
    } else {
      Serial.printf("Hum: --    | Temp: --    | ");
    }
    if (isnan(gasRsRo)) {
      Serial.printf("Gas: %.2fV Rs/Ro: -- | Air: %s\n",
                    gasVoltage, airQualityLabel(gasRsRo));
    } else {
      Serial.printf("Gas: %.2fV Rs/Ro: %.2f | Air: %s\n",
                    gasVoltage, gasRsRo, airQualityLabel(gasRsRo));
    }
  }

  delay(1);
}

/**
 * @todo: Calibrate NOISE_THRESHOLD for your environment.
 * Watch the printed "peak / avg / p-p" values: peak must stay low in silence
 * and rise clearly when shouting/blowing. If it is too sensitive, raise the
 * threshold; if low-pitched sounds are missed, lower it or increase the mic
 * gain. The longer 50 ms window and peak detector no longer penalise low
 * frequencies, but the electret module's own frequency response remains.
 *
 * @todo: Add LED indicator on built-in GPIO2 for visual noise level.
 *
 * @todo: Calibrate GAS_RO_CLEAN_AIR for the MQ135 after 24-48h burn-in.
 */
