/*
 * RF Signal Cloner/Jammer
 * Hardware: ESP32 + SH1106 OLED + 433MHz RF Modules
 * Created by: Nihal MP
 * 
 * Features:
 * - Read RF signals (315/433MHz)
 * - Store and replay signals
 * - RF jamming mode (for testing only)
 * - Button-based navigation
 * 
 * Pin Configuration:
 * - OLED: I2C (SDA=21, SCL=22)
 * - RF TX: GPIO 4
 * - RF RX: GPIO 5
 * - Buttons: UP=12, DOWN=13, SELECT=15, BACK=27
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <RCSwitch.h>

// ============================================
// CONFIGURATION
// ============================================

// OLED Configuration (SH1106 128x64)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// RF Module Pins
#define RF_TX_PIN 4
#define RF_RX_PIN 5

// Button Pins
#define BTN_UP 12
#define BTN_DOWN 13
#define BTN_SELECT 15
#define BTN_BACK 27

// Timing Constants
#define DEBOUNCE_DELAY 200
#define RF_CHECK_INTERVAL 3000
#define ANIM_SPEED 120
#define TRANSMIT_INTERVAL 500
#define JAM_INTERVAL 50

// ============================================
// GLOBAL OBJECTS
// ============================================

RCSwitch rfReceiver = RCSwitch();
RCSwitch rfTransmitter = RCSwitch();

// ============================================
// SYSTEM STATE
// ============================================

enum Mode { MENU, READ, EMULATE, JAM };
Mode currentMode = MENU;
int menuSelection = 0;
const int menuItems = 3;

// RF Status
bool rfModuleConnected = false;
unsigned long lastRFCheck = 0;

// Button State
struct ButtonState {
  bool up;
  bool down;
  bool select;
  bool back;
  bool upLast;
  bool downLast;
  bool selectLast;
  bool backLast;
  unsigned long lastPress;
};
ButtonState btnState = {false, false, false, false, false, false, false, false, 0};

// RF Signal Storage
struct RFSignal {
  unsigned long value;
  unsigned int bitLength;
  unsigned int protocol;
  unsigned int pulseLength;
  bool valid;
  unsigned long captureTime;
};
RFSignal capturedSignal = {0, 0, 0, 0, false, 0};

// Transmission State
bool isTransmitting = false;
bool isJamming = false;
unsigned long lastTransmit = 0;
unsigned long lastJam = 0;
int transmitCount = 0;

// Animation
unsigned int animFrame = 0;
unsigned long lastAnimUpdate = 0;

// ============================================
// BOOT ANIMATION
// ============================================

void bootAnimation() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(10, 5);
  display.println("RF CLONER");
  
  display.setTextSize(1);
  display.setCursor(25, 25);
  display.println("by Nihal MP");
  
  // Animated signal waves
  for(int frame = 0; frame < 3; frame++) {
    for(int x = 0; x < SCREEN_WIDTH; x += 6) {
      int y = 45 + sin((x + frame * 30) * 0.08) * 6;
      display.drawPixel(x, y, SH110X_WHITE);
    }
    
    // Progress bar
    int progress = map(frame, 0, 2, 0, SCREEN_WIDTH - 20);
    display.drawRect(10, 56, SCREEN_WIDTH - 20, 6, SH110X_WHITE);
    display.fillRect(11, 57, progress, 4, SH110X_WHITE);
    
    display.display();
    delay(400);
  }
  
  delay(600);
}

// ============================================
// RF MODULE DETECTION
// ============================================

bool checkRFModule() {
  // Method 1: Check if receiver ever gets data (wait 100ms)
  unsigned long start = millis();
  while(millis() - start < 100) {
    if(rfReceiver.available()) {
      rfReceiver.resetAvailable();
      return true;
    }
  }
  
  // Method 2: Check TX pin can be controlled
  pinMode(RF_TX_PIN, OUTPUT);
  digitalWrite(RF_TX_PIN, HIGH);
  delay(10);
  bool txOk = digitalRead(RF_TX_PIN) == HIGH;
  digitalWrite(RF_TX_PIN, LOW);
  
  // For testing: Uncomment below to bypass detection
  // return true;
  
  return txOk; // At minimum, TX should work
}

// ============================================
// BUTTON HANDLING
// ============================================

void readButtons() {
  btnState.up = (digitalRead(BTN_UP) == LOW);
  btnState.down = (digitalRead(BTN_DOWN) == LOW);
  btnState.select = (digitalRead(BTN_SELECT) == LOW);
  btnState.back = (digitalRead(BTN_BACK) == LOW);
}

bool buttonRisingEdge(bool current, bool last) {
  return (current && !last);
}

void handleButtons() {
  readButtons();
  
  unsigned long now = millis();
  if(now - btnState.lastPress < DEBOUNCE_DELAY) {
    return; // Skip if debounce time hasn't passed
  }
  
  // UP Button - Menu navigation
  if(buttonRisingEdge(btnState.up, btnState.upLast)) {
    if(currentMode == MENU) {
      menuSelection--;
      if(menuSelection < 0) menuSelection = menuItems - 1;
      Serial.println("Menu UP -> " + String(menuSelection));
    }
    btnState.lastPress = now;
  }
  
  // DOWN Button - Menu navigation
  if(buttonRisingEdge(btnState.down, btnState.downLast)) {
    if(currentMode == MENU) {
      menuSelection++;
      if(menuSelection >= menuItems) menuSelection = 0;
      Serial.println("Menu DOWN -> " + String(menuSelection));
    }
    btnState.lastPress = now;
  }
  
  // SELECT Button - Confirm/Toggle
  if(buttonRisingEdge(btnState.select, btnState.selectLast)) {
    Serial.println("SELECT pressed in mode: " + String(currentMode));
    
    if(currentMode == MENU) {
      // Enter selected mode
      switch(menuSelection) {
        case 0:
          currentMode = READ;
          Serial.println("-> READ MODE");
          break;
        case 1:
          currentMode = EMULATE;
          Serial.println("-> EMULATE MODE");
          break;
        case 2:
          currentMode = JAM;
          Serial.println("-> JAM MODE");
          break;
      }
      isTransmitting = false;
      isJamming = false;
      transmitCount = 0;
    }
    else if(currentMode == EMULATE) {
      if(capturedSignal.valid) {
        isTransmitting = !isTransmitting;
        transmitCount = 0;
        lastTransmit = 0;
        Serial.println("Transmit: " + String(isTransmitting ? "ON" : "OFF"));
      } else {
        Serial.println("ERROR: No signal captured!");
      }
    }
    else if(currentMode == JAM) {
      isJamming = !isJamming;
      lastJam = 0;
      Serial.println("Jamming: " + String(isJamming ? "ON" : "OFF"));
    }
    
    btnState.lastPress = now;
  }
  
  // BACK Button - Return to menu
  if(buttonRisingEdge(btnState.back, btnState.backLast)) {
    Serial.println("BACK -> MENU");
    currentMode = MENU;
    isTransmitting = false;
    isJamming = false;
    btnState.lastPress = now;
  }
  
  // Update last states
  btnState.upLast = btnState.up;
  btnState.downLast = btnState.down;
  btnState.selectLast = btnState.select;
  btnState.backLast = btnState.back;
}

// ============================================
// RF OPERATIONS
// ============================================

void handleRFReception() {
  if(rfReceiver.available()) {
    unsigned long value = rfReceiver.getReceivedValue();
    
    if(value != 0) { // Ignore zero values (noise)
      capturedSignal.value = value;
      capturedSignal.bitLength = rfReceiver.getReceivedBitlength();
      capturedSignal.protocol = rfReceiver.getReceivedProtocol();
      capturedSignal.pulseLength = rfReceiver.getReceivedDelay();
      capturedSignal.valid = true;
      capturedSignal.captureTime = millis();
      
      Serial.println("=== SIGNAL CAPTURED ===");
      Serial.print("Value: "); Serial.println(capturedSignal.value, HEX);
      Serial.print("Bits: "); Serial.println(capturedSignal.bitLength);
      Serial.print("Protocol: "); Serial.println(capturedSignal.protocol);
      Serial.print("Pulse: "); Serial.println(capturedSignal.pulseLength);
    }
    
    rfReceiver.resetAvailable();
  }
}

void transmitSignal() {
  unsigned long now = millis();
  
  if(now - lastTransmit >= TRANSMIT_INTERVAL) {
    if(capturedSignal.valid) {
      rfTransmitter.setProtocol(capturedSignal.protocol);
      rfTransmitter.setPulseLength(capturedSignal.pulseLength);
      rfTransmitter.send(capturedSignal.value, capturedSignal.bitLength);
      
      transmitCount++;
      lastTransmit = now;
      
      Serial.println("TX #" + String(transmitCount));
    }
  }
}

void jamSignals() {
  unsigned long now = millis();
  
  if(now - lastJam >= JAM_INTERVAL) {
    // Rotate through protocols
    int protocol = (animFrame % 3) + 1;
    rfTransmitter.setProtocol(protocol);
    
    // Send random data
    unsigned long randomValue = random(1, 16777215); // 24-bit max
    rfTransmitter.send(randomValue, 24);
    
    lastJam = now;
  }
}

// ============================================
// DISPLAY FUNCTIONS
// ============================================

void displayWarning(const char* message, const char* detail) {
  display.clearDisplay();
  
  // Animated warning triangle
  if((millis() / 400) % 2 == 0) {
    display.fillTriangle(64, 15, 54, 33, 74, 33, SH110X_WHITE);
    display.fillTriangle(61, 25, 67, 25, 64, 30, SH110X_BLACK);
    display.fillCircle(64, 34, 2, SH110X_WHITE);
  }
  
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  
  // Center main message
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(message, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 42);
  display.println(message);
  
  // Detail text
  display.getTextBounds(detail, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 52);
  display.println(detail);
  
  display.display();
}

void drawMenu() {
  display.clearDisplay();
  
  // Header
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(32, 2);
  display.println("RF CLONER");
  display.drawLine(0, 12, SCREEN_WIDTH, 12, SH110X_WHITE);
  
  // Menu items with selection highlight
  const char* items[] = {"READ", "EMULATE", "JAM"};
  
  for(int i = 0; i < menuItems; i++) {
    int y = 20 + i * 13;
    
    if(i == menuSelection) {
      display.fillRect(5, y - 2, SCREEN_WIDTH - 10, 11, SH110X_WHITE);
      display.setTextColor(SH110X_BLACK);
      display.setCursor(15, y);
      display.print("> ");
      display.print(items[i]);
      display.setTextColor(SH110X_WHITE);
    } else {
      display.setCursor(20, y);
      display.print(items[i]);
    }
  }
  
  // Status footer
  display.drawLine(0, 57, SCREEN_WIDTH, 57, SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(3, 59);
  display.print("RF:");
  display.print(rfModuleConnected ? "OK" : "ERR");
  
  if(capturedSignal.valid) {
    display.setCursor(70, 59);
    display.print("SIG:OK");
  }
  
  display.display();
}

void drawReadMode() {
  display.clearDisplay();
  
  // Header
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(32, 2);
  display.println("READ MODE");
  display.drawLine(0, 12, SCREEN_WIDTH, 12, SH110X_WHITE);
  
  // Scanning animation
  int scanPos = (animFrame * 3) % (SCREEN_WIDTH - 20);
  display.drawLine(10, 22, 10 + scanPos, 22, SH110X_WHITE);
  display.fillCircle(10 + scanPos, 22, 2, SH110X_WHITE);
  
  display.setCursor(15, 28);
  display.println("Scanning RF...");
  
  // Signal info if captured
  if(capturedSignal.valid) {
    display.setCursor(5, 38);
    display.print("VAL: 0x");
    display.println(capturedSignal.value, HEX);
    
    display.setCursor(5, 46);
    display.print("BITS: ");
    display.print(capturedSignal.bitLength);
    display.print(" PRO: ");
    display.print(capturedSignal.protocol);
    
    // Age indicator
    unsigned long age = (millis() - capturedSignal.captureTime) / 1000;
    if(age < 60) {
      display.setCursor(5, 54);
      display.print("Age: ");
      display.print(age);
      display.print("s");
    }
  } else {
    display.setCursor(10, 45);
    display.println("No signal yet...");
  }
  
  // Footer
  display.setCursor(5, 57);
  display.println("BACK=Menu");
  
  display.display();
}

void drawEmulateMode() {
  display.clearDisplay();
  
  // Header
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(20, 2);
  display.println("EMULATE MODE");
  display.drawLine(0, 12, SCREEN_WIDTH, 12, SH110X_WHITE);
  
  if(!capturedSignal.valid) {
    // No signal captured
    display.setCursor(10, 28);
    display.println("No signal!");
    display.setCursor(5, 40);
    display.println("Capture in READ");
    display.setCursor(15, 48);
    display.println("mode first");
  } else {
    // Signal details
    display.setCursor(5, 18);
    display.print("VAL: 0x");
    display.println(capturedSignal.value, HEX);
    
    display.setCursor(5, 26);
    display.print("BITS: ");
    display.print(capturedSignal.bitLength);
    
    display.setCursor(5, 34);
    display.print("PROTO: ");
    display.print(capturedSignal.protocol);
    
    // Transmission status
    if(isTransmitting) {
      // Animated transmission waves
      for(int i = 0; i < 4; i++) {
        int x = 15 + i * 25 + (animFrame * 2) % 25;
        display.fillCircle(x, 45, 2, SH110X_WHITE);
        display.drawCircle(x, 45, 4, SH110X_WHITE);
      }
      
      display.setCursor(20, 52);
      display.print("TX #");
      display.print(transmitCount);
    } else {
      display.setCursor(15, 45);
      display.println("SELECT to TX");
    }
  }
  
  // Footer
  display.setCursor(5, 57);
  display.println("BACK=Menu");
  
  display.display();
}

void drawJamMode() {
  display.clearDisplay();
  
  // Header
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(32, 2);
  display.println("JAM MODE");
  display.drawLine(0, 12, SCREEN_WIDTH, 12, SH110X_WHITE);
  
  // Warning
  display.setTextSize(1);
  display.setCursor(8, 20);
  display.println("! USE RESPONSIBLY !");
  display.setCursor(8, 30);
  display.println("Testing only!");
  
  if(isJamming) {
    // Chaotic jamming animation
    for(int i = 0; i < 40; i++) {
      int x = random(0, SCREEN_WIDTH);
      int y = random(40, 52);
      display.drawPixel(x, y, SH110X_WHITE);
    }
    
    display.setCursor(25, 53);
    display.println("JAMMING...");
  } else {
    display.setCursor(15, 45);
    display.println("SELECT=Start");
  }
  
  // Footer
  display.setCursor(5, 57);
  display.println("BACK=Stop");
  
  display.display();
}

// ============================================
// MAIN SETUP
// ============================================

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n=================================");
  Serial.println("RF Cloner - Starting...");
  Serial.println("Created by: Nihal MP");
  Serial.println("=================================\n");
  
  // Initialize buttons
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);
  Serial.println("Buttons: UP=12 DN=13 SEL=15 BACK=27");
  
  // Initialize button states
  readButtons();
  btnState.upLast = btnState.up;
  btnState.downLast = btnState.down;
  btnState.selectLast = btnState.select;
  btnState.backLast = btnState.back;
  
  // Initialize OLED
  delay(250);
  if(!display.begin(0x3C, true)) {
    Serial.println("ERROR: Display init failed!");
    while(1) delay(1000);
  }
  
  Serial.println("Display: OK (SH1106 128x64)");
  display.clearDisplay();
  display.display();
  
  // Boot animation
  bootAnimation();
  
  // Initialize RF modules
  rfReceiver.enableReceive(digitalPinToInterrupt(RF_RX_PIN));
  rfTransmitter.enableTransmit(RF_TX_PIN);
  Serial.println("RF RX: GPIO5 | RF TX: GPIO4");
  
  // Check RF module
  Serial.print("Checking RF module... ");
  rfModuleConnected = checkRFModule();
  Serial.println(rfModuleConnected ? "CONNECTED" : "NOT FOUND");
  
  if(!rfModuleConnected) {
    Serial.println("WARNING: RF module not detected!");
    Serial.println("Check connections and power.");
  }
  
  currentMode = MENU;
  Serial.println("\n=== READY ===\n");
}

// ============================================
// MAIN LOOP
// ============================================

void loop() {
  // Periodic RF module check
  if(millis() - lastRFCheck > RF_CHECK_INTERVAL) {
    bool wasConnected = rfModuleConnected;
    rfModuleConnected = checkRFModule();
    
    if(wasConnected != rfModuleConnected) {
      Serial.println("RF Status changed: " + String(rfModuleConnected ? "CONNECTED" : "DISCONNECTED"));
    }
    
    lastRFCheck = millis();
  }
  
  // Update animation frame (prevent overflow)
  if(millis() - lastAnimUpdate > ANIM_SPEED) {
    animFrame = (animFrame + 1) % 1000; // Reset at 1000
    lastAnimUpdate = millis();
  }
  
  // Handle button input
  handleButtons();
  
  // Execute mode-specific logic
  switch(currentMode) {
    case MENU:
      drawMenu();
      break;
      
    case READ:
      if(!rfModuleConnected) {
        displayWarning("RF Module Error", "Check Connection");
      } else {
        handleRFReception();
        drawReadMode();
      }
      break;
      
    case EMULATE:
      if(!rfModuleConnected) {
        displayWarning("RF Module Error", "Check Connection");
      } else {
        if(isTransmitting) {
          transmitSignal();
        }
        drawEmulateMode();
      }
      break;
      
    case JAM:
      if(!rfModuleConnected) {
        displayWarning("RF Module Error", "Check Connection");
      } else {
        if(isJamming) {
          jamSignals();
        }
        drawJamMode();
      }
      break;
  }
  
  delay(30); // Reduced delay for more responsive input
}

// =================================
// END OF CODE
// Created by: Nihal MP
// =================================