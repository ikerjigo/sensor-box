#include <Arduino.h>

// Thermistor configuration
#define THERMISTOR_PIN 5
#define ADC_RESOLUTION 4095.0  // ESP32-S3 12-bit ADC
#define SERIES_RESISTOR 10000.0  // 10kOhm series resistor
#define THERMISTOR_NOMINAL 10000.0  // 10kOhm at nominal temperature
#define TEMPERATURE_NOMINAL 25.0  // degrees Celsius
#define B_CONSTANT 3950.0  // B constant for thermistor
#define TEMPERATURE_MIN 0.0  // Min temp to display
#define TEMPERATURE_MAX 100.0  // Max temp to display
#define CALIBRATION_OFFSET 2.5  // Calibration offset in degrees Celsius
#define NUM_SAMPLES 10  // Number of ADC samples for averaging

#define VREF 3.3  // Reference voltage

float readThermistorTemperature() {
  // Read multiple samples and average
  float sum = 0.0;
  for (int i = 0; i < NUM_SAMPLES; i++) {
    sum += analogRead(THERMISTOR_PIN);
    delay(2);
  }
  float adcAverage = sum / NUM_SAMPLES;
  
  // Convert ADC reading to voltage
  float voltage = (adcAverage / ADC_RESOLUTION) * VREF;
  
  // Calculate resistance using voltage divider
  float thermistorResistance = (SERIES_RESISTOR * voltage) / (VREF - voltage);
  
  // Steinhart-Hart equation (simplified B-parameter form)
  float temperatureKelvin = 1.0 / (
    (1.0 / (TEMPERATURE_NOMINAL + 273.15)) + 
    (1.0 / B_CONSTANT) * log(thermistorResistance / THERMISTOR_NOMINAL)
  );
  
  return temperatureKelvin - 273.15 - CALIBRATION_OFFSET;  // Convert to Celsius, apply calibration
}

void setup() {
  Serial.begin(115200);
  delay(100);
  
  // Configure ADC pin
  pinMode(THERMISTOR_PIN, INPUT);
  analogReadResolution(12);  // 12-bit ADC for ESP32-S3
  
  Serial.println("========================================");
  Serial.println("Thermistor Reading - GPIO Pin 5");
  Serial.println("========================================");
  Serial.println("Waiting for stable readings...");
  Serial.println();
}

void loop() {
  float temperature = readThermistorTemperature();
  
  // Get ADC raw value and voltage for debugging
  int adcValue = analogRead(THERMISTOR_PIN);
  float voltage = (adcValue / ADC_RESOLUTION) * VREF;
  
  Serial.println("----------------------------------------");
  Serial.printf("ADC Value: %d / 4095\n", adcValue);
  Serial.printf("Voltage: %.3f V\n", voltage);
  Serial.printf("Temperature: %.2f °C\n", temperature);
  
  if (temperature < TEMPERATURE_MIN || temperature > TEMPERATURE_MAX) {
    Serial.printf("WARNING: Temperature out of range! (%.2f °C)\n", temperature);
  }
  
  Serial.println();
  Serial.println("Waiting 2 seconds...");
  
  delay(2000);
}
