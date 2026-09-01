#include <Arduino.h>

// -----------------------------------------------------------
// ピン設定 (最大9個まで対応可能)
// {ボタンピン, LEDピン}
// -----------------------------------------------------------
const int buzzerCount = 9;
const int buzzerPins[buzzerCount][2] = {
  {2, 3},   // ID 1
  {4, 5},   // ID 2
  {6, 7},   // ID 3
  {8, 9},   // ID 4
  {10, 11}, // ID 5
  {12, 13}, // ID 6
  {A0, A1}, // ID 7
  {A2, A3}, // ID 8
  {A4, A5}  // ID 9
};

// 状態管理
int activeBuzzerID = 0; // 点灯中のID (0なら消灯)
unsigned long previousMillis = 0;
const long interval = 100; // 点滅間隔

void setup() {
  // ★高速通信設定
  Serial.begin(115200);
  Serial.setTimeout(10); // 読み取りタイムアウト短縮

  for (int i = 0; i < buzzerCount; i++) {
    pinMode(buzzerPins[i][0], INPUT_PULLUP); // ボタン(入力)
    pinMode(buzzerPins[i][1], OUTPUT);       // LED(出力)
    digitalWrite(buzzerPins[i][1], LOW);     // 初期は消灯
  }
}

void loop() {
  // ------------------------------------------
  // 1. ボタン入力送信 (まだ誰も光っていない時)
  // ------------------------------------------
  if (activeBuzzerID == 0) {
    for (int i = 0; i < buzzerCount; i++) {
      // ボタンが押されたら(LOW)
      if (digitalRead(buzzerPins[i][0]) == LOW) {
        // ID (1〜9) をPCへ送信
        Serial.print(i + 1);
        
        // 通信が速いのでチャタリング防止待機は短めに
        delay(50); 
        break; 
      }
    }
  }

  // ------------------------------------------
  // 2. PCからの受信処理
  // ------------------------------------------
  if (Serial.available() > 0) {
    char cmd = Serial.read();

    // --- 点灯指令 ('o' + 番号) ---
    if (cmd == 'o') {
      int id = Serial.parseInt(); 
      
      if (id >= 1 && id <= buzzerCount) {
        activeBuzzerID = id;
        previousMillis = millis();
        
        // ★重要: 点滅ループを待たずに、指令が来た瞬間に一度光らせる
        // これで見た目の反応速度が向上します
        digitalWrite(buzzerPins[activeBuzzerID - 1][1], HIGH);
      }
    }
    // --- リセット指令 ('r') ---
    else if (cmd == 'r') {
      // 全消灯
      for (int i = 0; i < buzzerCount; i++) {
        digitalWrite(buzzerPins[i][1], LOW);
      }
      activeBuzzerID = 0;
    }
  }

  // ------------------------------------------
  // 3. LED点滅制御 (非同期)
  // ------------------------------------------
  if (activeBuzzerID > 0) {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
      previousMillis = currentMillis;
      
      int pin = buzzerPins[activeBuzzerID - 1][1];
      // XORロジックで反転 (点滅)
      digitalWrite(pin, !digitalRead(pin)); 
    }
  }
}
