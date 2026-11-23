/**
 * ESP32-S3 Sensor Box: Microphone + DHT11 + Speaker
 * Reads analog mic on GPIO4, DHT11 temp/hum on GPIO16,
 * plays beeps on GPIO13 when noise exceeds threshold.
 */

#include <Arduino.h>
#include <dhtnew.h>

/** Pin definitions */
#define VOLUME_SENSOR_PIN 4   // ADC input for electret microphone module
#define TEMP_HUMIDITY_PIN 16  // DHT11 data pin
#define SPEAKER_PIN 13        // PWM speaker driver pin

/** ADC constants */
#define ADC_RESOLUTION 4095.0   // ESP32-S3 12-bit ADC max value
#define VREF 3.3                // Reference voltage in volts

/** Noise threshold: triggers speaker when mic ADC > this value */
#define NOISE_THRESHOLD 600     // Adjust: current ~270 idle, increase for more sensitivity

/** Sequence limits */
#define MAX_STEPS 32            // Max steps in a beep sequence

/** DHT read configuration */
const uint32_t READ_INTERVAL_MS = 500;  // How often to read sensors (ms)
const uint8_t MAX_READ_RETRIES = 3;     // Retry attempts for DHT reads

/** LEDC channel for speaker (ESP32 Arduino core v2 API) */
#define SPK_CH 0

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

/** Sequence step structure: frequency (Hz) and duration (ms) */
struct Step { uint32_t hz; uint32_t ms; };

/** DHT sensor instance on GPIO16 */
DHTNEW dht(TEMP_HUMIDITY_PIN);

/** Sequencer state */
Step seq[MAX_STEPS];
int seqLen = 0;      // Number of steps in current sequence
int seqPos = 0;      // Current step index
bool playing = false; // True while a sequence is active
unsigned long stepStart = 0; // Timestamp of current step start

/**
 * @brief Start the first step of the current sequence
 * Writes the first tone and records the start time.
 */
void startStep() {
  ledcWriteTone(SPK_CH, seq[seqPos].hz);
  stepStart = millis();
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
  Serial.print("Volume sensor (ADC) on pin: ");
  Serial.println(VOLUME_SENSOR_PIN);
  Serial.print("DHT11 on pin: ");
  Serial.println(TEMP_HUMIDITY_PIN);
  Serial.println("Ensure a 4.7k-10k pull-up resistor on the DHT11 data line");
  Serial.println();

  pinMode(VOLUME_SENSOR_PIN, INPUT);
  analogReadResolution(12);

  spkInit();

  dht.setType(11);
  dht.setWaitForReading(true);

  Serial.print("DHT11 warming up... ");
  delay(2000);
  Serial.println("done");
  Serial.println();
}

/**
 * @brief Read DHT11 with retry logic
 * Skips DHTLIB_WAITING_FOR_READ errors internally (library enforces 1s min interval).
 * @return DHTLIB_OK on success, or error code from library
 */
int readDHTWithRetry() {
  for (uint8_t attempt = 1; attempt <= MAX_READ_RETRIES; attempt++) {
    int result = dht.read();
    if (result == DHTLIB_OK) {
      return result;
    }
    if (result != DHTLIB_WAITING_FOR_READ && attempt < MAX_READ_RETRIES) {
      delay(200);  // Brief pause between retries
    }
  }
  return dht.read();
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
 * @main_loop Main application loop
 * Reads microphone and DHT11, prints to Serial,
 * plays beep sequence on GPIO13 when noise is too high.
 * Non-blocking: uses millis()-based sequencer for speaker tones.
 */
void loop() {
  uint32_t now = millis();

  // Advance to the next step when the current one is done
  if (playing && millis() - stepStart >= seq[seqPos].ms) {
    seqPos++;
    if (seqPos >= seqLen) {
      ledcWriteTone(SPK_CH, 0);
      playing = false;
    } else {
      startStep();
    }
  }

  int dhtResult = readDHTWithRetry();
  float humidity = dht.getHumidity();
  float temperature = dht.getTemperature();
  int volumeValue = analogRead(VOLUME_SENSOR_PIN);
  float voltage = (volumeValue / ADC_RESOLUTION) * VREF;

  if (dhtResult != DHTLIB_OK) {
    Serial.printf("[%lu] DHT11 read FAILED (error: %d)\n", (unsigned long)now, dhtResult);
    Serial.printf("[%lu] Volume (ADC): %-5d | %.2f V\n", (unsigned long)now, volumeValue, voltage);
  } else {
    Serial.printf("[%lu] Volume (ADC): %-5d | %.2f V | Humidity: %5.1f%% | Temperature: %5.1f°C\n",
                  (unsigned long)now, volumeValue, voltage, humidity, temperature);
  }

  if (volumeValue > NOISE_THRESHOLD) {
    if (!playing) {
      playSequence();
    }
  } else {
    ledcWriteTone(SPK_CH, 0);
    playing = false;
  }

  delay(READ_INTERVAL_MS);
}

/**
 * @todo: Calibrate NOISE_THRESHOLD for your environment
 * Current value (600) corresponds to ~0.5V idle; adjust based on ambient noise.
 *
 * @todo: Add LED indicator on built-in GPIO2 for visual noise level.
 *
 * @todo: Replace DHTNEW with Adafruit_DHT for better ESP32 reliability
 * (DHTNEW error -4 may persist without external 4.7-10k pull-up on GPIO16).
 */
