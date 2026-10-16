#include <Arduino.h>

#define SPK_PIN 13
#define MAX_STEPS 64

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  void spkInit()           { ledcAttach(SPK_PIN, 2000, 8); }
  void spkOn(uint32_t hz)  { ledcWriteTone(SPK_PIN, hz); }
  void spkOff()            { ledcWriteTone(SPK_PIN, 0); }
#else
  #define SPK_CH 0
  void spkInit()           { ledcSetup(SPK_CH, 2000, 8); ledcAttachPin(SPK_PIN, SPK_CH); }
  void spkOn(uint32_t hz)  { ledcWriteTone(SPK_CH, hz); }
  void spkOff()            { ledcWriteTone(SPK_CH, 0); }
#endif

struct Step { uint32_t hz; uint32_t ms; };

Step seq[MAX_STEPS];
int seqLen = 0;
int seqPos = 0;
bool playing = false;
unsigned long stepStart = 0;

void startStep() {
  spkOn(seq[seqPos].hz);
  stepStart = millis();
}

void setup() {
  Serial.begin(115200);
  spkInit();
  spkOff();
  Serial.println("Ready. Usage: beep <hz> <ms> [<hz> <ms> ...]");
}

void loop() {
  // Advance to the next step when the current one is done
  if (playing && millis() - stepStart >= seq[seqPos].ms) {
    seqPos++;
    if (seqPos >= seqLen) {
      spkOff();
      playing = false;
    } else {
      startStep();
    }
  }

  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;

    // Split into tokens
    String tok[2 * MAX_STEPS + 1];
    int n = 0;
    int pos = 0;
    while (pos < (int)line.length() && n < 2 * MAX_STEPS + 1) {
      int sp = line.indexOf(' ', pos);
      if (sp < 0) sp = line.length();
      if (sp > pos) tok[n++] = line.substring(pos, sp);
      pos = sp + 1;
    }

    bool ok = (pos >= (int)line.length()) && n >= 3 && (n % 2 == 1) &&
              tok[0].equalsIgnoreCase("beep");

    if (ok) {
      int count = 0;
      for (int i = 1; i < n; i += 2) {
        long hz = tok[i].toInt();
        long ms = tok[i + 1].toInt();
        if (hz < 0 || ms <= 0) { ok = false; break; }
        seq[count].hz = hz;
        seq[count].ms = ms;
        count++;
      }
      if (ok) {
        seqLen = count;
        seqPos = 0;
        playing = true;
        startStep();
        Serial.print("OK: ");
        Serial.print(count);
        Serial.println(" step(s)");
        return;
      }
    }
    Serial.println("Error. Usage: beep <hz> <ms> [<hz> <ms> ...] (max 32 steps)");
  }
}

/*

# Himne del Barçca
beep 523 800 494 600 440 200 392 200 330 200 392 200 523 200 659 800 698 400 784 300 698 100 659 200 392 200 523 200 659 200 587 800 523 800 330 1200 392 400 523 400 262 150 0 50 262 150 0 50 262 350 0 450 330 1200 392 400 523 400 262 150 0 50 262 150 0 50 262 350 0 450

beep 330 2000 392 400 523 400 0 2000 330 2000 392 400 523 400 0 2000 330 600 392 200 523 400 587 400 523 200 494 1400 440 600 494 200 523 400 494 200 440 200 392 1200 349 600 392 200 440 400 392 200 349 200 330 1200 294 580 0 20 294 200 330 400 370 400

*/