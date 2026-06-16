/*
 * SIMULASI SISTEM KONTROL LIFT BERBASIS FreeRTOS
 * Target: ESP32 dengan Wokwi Simulator
 * 
 * Fitur:
 * - 1 Lift dengan 5 lantai
 * - Tombol lantai 1-5 di dalam lift
 * - Tombol emergency
 * - Tombol buka/tutup pintu manual
 * - LCD1602 Display untuk status real-time
 * - Priority-based scheduling
 * - Elevator SCAN scheduling (direction-aware request handling)
 * - Mutex, Semaphore untuk sinkronisasi
 */

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>

// LCD I2C
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Pin Definitions - Lift Internal Buttons
#define FLOOR_1_BTN     13
#define FLOOR_2_BTN     12
#define FLOOR_3_BTN     14
#define FLOOR_4_BTN     27
#define FLOOR_5_BTN     26
#define EMERGENCY_BTN   25
#define OPEN_DOOR_BTN   33
#define CLOSE_DOOR_BTN  32

// LCD I2C Pins
#define LCD_SDA 21
#define LCD_SCL 22

// Elevator States
enum Direction { IDLE, MOVING_UP, MOVING_DOWN };
enum DoorState { DOOR_CLOSED, DOOR_OPENING, DOOR_OPEN, DOOR_CLOSING };

// Lift State Structure
typedef struct {
  int currentFloor;
  int targetFloor;
  Direction direction;       // current travel direction (IDLE/UP/DOWN)
  DoorState doorState;
  bool emergencyMode;
  bool manualDoorOpen;
  bool manualDoorClose;
  bool needsDepartureDoor;   // true = door must open/close before moving to new floor
  bool needsArrivalDoor;     // true = door must open/close after arriving at target floor
  bool floorRequested[6];    // index 1-5, true if floor has pending request
  int floorRequestOrder[6];  // request sequence number for tie-breaking
  int requestCounter;        // increments with each new request
} LiftState;

// Global Variables
LiftState lift = {
  1, 1, IDLE, DOOR_CLOSED,         // currentFloor, targetFloor, direction, doorState
  false, false, false,             // emergencyMode, manualDoorOpen, manualDoorClose
  false, false,                    // needsDepartureDoor, needsArrivalDoor
  {false, false, false, false, false, false},  // floorRequested[6]
  {0, 0, 0, 0, 0, 0},              // floorRequestOrder[6]
  0                                // requestCounter
};

// FreeRTOS Handles
SemaphoreHandle_t liftStateMutex;
SemaphoreHandle_t lcdMutex;
SemaphoreHandle_t emergencySemaphore;

TaskHandle_t emergencyTaskHandle;
TaskHandle_t liftControlTaskHandle;
TaskHandle_t doorTaskHandle;
TaskHandle_t requestHandlerTaskHandle;
TaskHandle_t lcdDisplayTaskHandle;

/*
 * Emergency Handler Task
 * Priority: 4 (Highest)
 */
void EmergencyHandlerTask(void *parameter) {
  Serial.println("[TASK] EmergencyHandlerTask started - Priority: 4");
  
  while (1) {
    if (xSemaphoreTake(emergencySemaphore, portMAX_DELAY) == pdTRUE) {
      Serial.println("\n[EMERGENCY] *** EMERGENCY ACTIVATED! ***");
      
      if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lift.emergencyMode = true;
        lift.direction = IDLE;
        lift.doorState = DOOR_OPEN;
        lift.needsDepartureDoor = false;
        lift.needsArrivalDoor = false;
        for (int f = 1; f <= 5; f++) {
          lift.floorRequested[f] = false;
          lift.floorRequestOrder[f] = 0;
        }
        lift.requestCounter = 0;
        xSemaphoreGive(liftStateMutex);
        
        Serial.println("[EMERGENCY] Lift stopped, door opened, requests cleared");
        Serial.println("[EMERGENCY] Wait 5 seconds to reset...");
        
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100));
        lift.emergencyMode = false;
        xSemaphoreGive(liftStateMutex);
        
        Serial.println("[EMERGENCY] Emergency cleared. System normal.\n");
      }
    }
  }
}

/*
 * Lift Control Task
 * Priority: 3
 * 
 * Elevator (SCAN) scheduling:
 * - Service all pending floors in the current travel direction first,
 *   in ascending/descending order along the way.
 * - Only reverse direction after the last request in the current direction.
 * - When idle, pick the nearest requested floor (FIFO tie-break).
 */
void LiftControlTask(void *parameter) {
  Serial.println("[TASK] LiftControlTask started - Priority: 3");
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  auto hasAnyRequest = []() -> bool {
    for (int f = 1; f <= 5; f++) {
      if (lift.floorRequested[f]) return true;
    }
    return false;
  };
  
  auto nearestRequestedFloor = [](int currentFloor) -> int {
    int bestFloor = -1;
    int minDist = 999;
    int bestOrder = 999999;
    for (int f = 1; f <= 5; f++) {
      if (lift.floorRequested[f]) {
        int dist = abs(f - currentFloor);
        int order = lift.floorRequestOrder[f];
        if (dist < minDist || (dist == minDist && order < bestOrder)) {
          minDist = dist;
          bestFloor = f;
          bestOrder = order;
        }
      }
    }
    return bestFloor;
  };
  
  auto nextRequestAbove = [](int floor) -> int {
    for (int f = floor + 1; f <= 5; f++) {
      if (lift.floorRequested[f]) return f;
    }
    return -1;
  };
  
  auto nextRequestBelow = [](int floor) -> int {
    for (int f = floor - 1; f >= 1; f--) {
      if (lift.floorRequested[f]) return f;
    }
    return -1;
  };
  
  auto clearRequest = [](int floor) {
    lift.floorRequested[floor] = false;
    lift.floorRequestOrder[floor] = 0;
  };
  
  while (1) {
    // Snapshot state under mutex
    int current, target;
    Direction dir;
    DoorState door;
    bool needDep, needArr, emergency;
    
    if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      current = lift.currentFloor;
      target = lift.targetFloor;
      dir = lift.direction;
      door = lift.doorState;
      needDep = lift.needsDepartureDoor;
      needArr = lift.needsArrivalDoor;
      emergency = lift.emergencyMode;
      xSemaphoreGive(liftStateMutex);
    } else {
      vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(500));
      continue;
    }
    
    // Safety: don't move during emergency or while door is busy
    if (emergency || door != DOOR_CLOSED || needDep || needArr) {
      vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(500));
      continue;
    }
    
    if (dir == IDLE) {
      // Lift is idle: choose nearest request
      if (hasAnyRequest()) {
        int newTarget = nearestRequestedFloor(current);
        
        if (newTarget == current) {
          // Request is for the current floor: just open the arrival door
          if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            lift.needsArrivalDoor = true;
            clearRequest(current);
            xSemaphoreGive(liftStateMutex);
          }
          Serial.printf("[LIFT] Arrived at Floor %d\n", current);
        } else {
          Direction newDir = (newTarget > current) ? MOVING_UP : MOVING_DOWN;
          
          if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            lift.targetFloor = newTarget;
            lift.direction = newDir;
            lift.needsDepartureDoor = true;
            xSemaphoreGive(liftStateMutex);
          }
          Serial.printf("[LIFT] New request: Floor %d\n", newTarget);
        }
      }
    } else {
      // Currently traveling
      if (lift.floorRequested[current]) {
        // Stop at this requested floor for arrival door
        if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          lift.targetFloor = current;
          lift.needsArrivalDoor = true;
          clearRequest(current);
          xSemaphoreGive(liftStateMutex);
        }
        Serial.printf("[LIFT] Arrived at Floor %d\n", current);
      } else {
        // No stop needed here: target the nearest request in current direction
        int newTarget = (dir == MOVING_UP) ? nextRequestAbove(current) : nextRequestBelow(current);
        
        if (newTarget == -1) {
          // No more requests in current direction: reverse or idle
          int reverseTarget = (dir == MOVING_UP) ? nextRequestBelow(current) : nextRequestAbove(current);
          if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (reverseTarget != -1) {
              lift.targetFloor = reverseTarget;
              lift.direction = (dir == MOVING_UP) ? MOVING_DOWN : MOVING_UP;
            } else {
              lift.direction = IDLE;
              lift.targetFloor = current;
            }
            xSemaphoreGive(liftStateMutex);
          }
        } else {
          if (newTarget != target) {
            if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              lift.targetFloor = newTarget;
              xSemaphoreGive(liftStateMutex);
            }
            target = newTarget;
          }
          
          // Continue moving toward target
          if (current < target) {
            if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              lift.currentFloor++;
              xSemaphoreGive(liftStateMutex);
            }
            Serial.printf("[LIFT] Moving UP: Floor %d -> %d\n", current, current + 1);
          } else if (current > target) {
            if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              lift.currentFloor--;
              xSemaphoreGive(liftStateMutex);
            }
            Serial.printf("[LIFT] Moving DOWN: Floor %d -> %d\n", current, current - 1);
          }
        }
      }
    }
    
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));
  }
}

/*
 * Door Control Task
 * Priority: 2
 * 
 * Handles two door scenarios:
 * 1. Departure door: opens/closes door BEFORE lift moves to a new floor
 * 2. Arrival door:   opens/closes door AFTER lift arrives at target floor
 */
void DoorControlTask(void *parameter) {
  Serial.println("[TASK] DoorControlTask started - Priority: 2");
  TickType_t xLastWakeTime = xTaskGetTickCount();
  bool doorProcessed = true;  // true to prevent door opening on startup without a request
  
  while (1) {
    bool shouldOpenDoor = false;
    bool manualOpen = false, manualClose = false;
    bool wasDepartureDoor = false;
    bool wasArrivalDoor = false;
    
    if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      manualOpen = lift.manualDoorOpen;
      manualClose = lift.manualDoorClose;
      lift.manualDoorOpen = false;
      lift.manualDoorClose = false;
      
      // Departure door: new request received, open door before moving
      if (lift.needsDepartureDoor &&
          lift.doorState == DOOR_CLOSED &&
          !lift.emergencyMode) {
        shouldOpenDoor = true;
        wasDepartureDoor = true;
      }
      // Arrival door: lift arrived at target floor
      else if (lift.needsArrivalDoor &&
               lift.doorState == DOOR_CLOSED &&
               !lift.emergencyMode) {
        shouldOpenDoor = true;
        wasArrivalDoor = true;
      }
      // Manual open override
      else if (manualOpen && lift.doorState == DOOR_CLOSED) {
        shouldOpenDoor = true;
      }
      
      // Reset doorProcessed when lift starts moving (direction changes from IDLE)
      if (lift.direction != IDLE) {
        doorProcessed = false;
      }
      
      xSemaphoreGive(liftStateMutex);
    }
    
    // Manual close door
    if (manualClose) {
      if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (lift.doorState == DOOR_OPEN) {
          Serial.println("[DOOR] Manual close triggered");
          lift.doorState = DOOR_CLOSING;
          xSemaphoreGive(liftStateMutex);
          
          vTaskDelay(pdMS_TO_TICKS(500));
          
          xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100));
          lift.doorState = DOOR_CLOSED;
          xSemaphoreGive(liftStateMutex);
          
          Serial.println("[DOOR] Closed manually");
          doorProcessed = true;
        } else {
          xSemaphoreGive(liftStateMutex);
        }
      }
    }
    
    if (shouldOpenDoor) {
      if (wasDepartureDoor) {
        Serial.println("[DOOR] Opening (departure)...");
      } else if (wasArrivalDoor) {
        Serial.println("[DOOR] Opening (arrival)...");
      } else {
        Serial.println("[DOOR] Opening...");
      }
      
      if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lift.doorState = DOOR_OPENING;
        if (wasDepartureDoor) {
          lift.needsDepartureDoor = false;  // clear departure flag
        }
        if (wasArrivalDoor) {
          lift.needsArrivalDoor = false;    // clear arrival flag
        }
        xSemaphoreGive(liftStateMutex);
      }
      
      vTaskDelay(pdMS_TO_TICKS(500));
      
      if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lift.doorState = DOOR_OPEN;
        xSemaphoreGive(liftStateMutex);
      }
      Serial.println("[DOOR] Opened");
      
      vTaskDelay(pdMS_TO_TICKS(3000));
      
      Serial.println("[DOOR] Closing...");
      if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lift.doorState = DOOR_CLOSING;
        xSemaphoreGive(liftStateMutex);
      }
      
      vTaskDelay(pdMS_TO_TICKS(500));
      
      if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lift.doorState = DOOR_CLOSED;
        xSemaphoreGive(liftStateMutex);
      }
      Serial.println("[DOOR] Closed\n");
      
      doorProcessed = true;
    }
    
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(500));
  }
}

/*
 * Request Handler Task
 * Priority: 1
 */
void RequestHandlerTask(void *parameter) {
  Serial.println("[TASK] RequestHandlerTask started - Priority: 1");
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  // Button pins arrays
  int floorButtons[5] = {FLOOR_1_BTN, FLOOR_2_BTN, FLOOR_3_BTN, FLOOR_4_BTN, FLOOR_5_BTN};
  int doorOpenBtn = OPEN_DOOR_BTN;
  int doorCloseBtn = CLOSE_DOOR_BTN;
  int emergencyBtn = EMERGENCY_BTN;
  
  bool lastFloorState[5];
  bool lastOpenState, lastCloseState, lastEmergencyState;
  unsigned long lastFloorPress[5] = {0};
  unsigned long lastOpenPress = 0, lastClosePress = 0, lastEmergencyPress = 0;
  const unsigned long debounce = 100;  // reduced for Wokwi simulation responsiveness
  
  for (int i = 0; i < 5; i++) {
    lastFloorState[i] = digitalRead(floorButtons[i]);
    Serial.printf("[BTN-DBG] Floor %d button (GPIO %d) initial: %s\n", 
                  i+1, floorButtons[i], lastFloorState[i] ? "HIGH" : "LOW");
  }
  lastOpenState = digitalRead(doorOpenBtn);
  lastCloseState = digitalRead(doorCloseBtn);
  lastEmergencyState = digitalRead(emergencyBtn);
  
  Serial.println("\n╔═══════════════════════════════════════════════╗");
  Serial.println("║  LIFT SYSTEM READY                           ║");
  Serial.println("║  Use buttons to control the lift             ║");
  Serial.println("╚═══════════════════════════════════════════════╝\n");
  
  while (1) {
    unsigned long currentTime = millis();
    
    // === FLOOR BUTTONS ===
    for (int i = 0; i < 5; i++) {
      bool state = digitalRead(floorButtons[i]);
      
      if (state != lastFloorState[i]) {
        Serial.printf("[BTN-DBG] Floor %d: %s -> %s\n", i+1,
                      lastFloorState[i] ? "HIGH" : "LOW",
                      state ? "HIGH" : "LOW");
      }
      
      if (state == LOW && lastFloorState[i] == HIGH) {
        if ((currentTime - lastFloorPress[i]) > debounce) {
          int floor = i + 1;
          
          if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (!lift.floorRequested[floor]) {
              lift.floorRequested[floor] = true;
              lift.floorRequestOrder[floor] = ++lift.requestCounter;
            }
            xSemaphoreGive(liftStateMutex);
          }
          
          Serial.printf("\n[BUTTON] Floor %d pressed\n", floor);
          lastFloorPress[i] = currentTime;
        }
      }
      lastFloorState[i] = state;
    }
    
    // === OPEN DOOR BUTTON ===
    {
      bool state = digitalRead(doorOpenBtn);
      if (state != lastOpenState) {
        Serial.printf("[BTN-DBG] Open: %s -> %s\n",
                      lastOpenState ? "HIGH" : "LOW",
                      state ? "HIGH" : "LOW");
      }
      if (state == LOW && lastOpenState == HIGH) {
        if ((currentTime - lastOpenPress) > debounce) {
          Serial.println("\n[BUTTON] Manual door open");
          if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            lift.manualDoorOpen = true;
            xSemaphoreGive(liftStateMutex);
          }
          lastOpenPress = currentTime;
        }
      }
      lastOpenState = state;
    }
    
    // === CLOSE DOOR BUTTON ===
    {
      bool state = digitalRead(doorCloseBtn);
      if (state != lastCloseState) {
        Serial.printf("[BTN-DBG] Close: %s -> %s\n",
                      lastCloseState ? "HIGH" : "LOW",
                      state ? "HIGH" : "LOW");
      }
      if (state == LOW && lastCloseState == HIGH) {
        if ((currentTime - lastClosePress) > debounce) {
          Serial.println("\n[BUTTON] Manual door close");
          if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            lift.manualDoorClose = true;
            xSemaphoreGive(liftStateMutex);
          }
          lastClosePress = currentTime;
        }
      }
      lastCloseState = state;
    }
    
    // === EMERGENCY BUTTON ===
    {
      bool state = digitalRead(emergencyBtn);
      if (state != lastEmergencyState) {
        Serial.printf("[BTN-DBG] Emergency: %s -> %s\n",
                      lastEmergencyState ? "HIGH" : "LOW",
                      state ? "HIGH" : "LOW");
      }
      if (state == LOW && lastEmergencyState == HIGH) {
        if ((currentTime - lastEmergencyPress) > debounce) {
          Serial.println("\n[BUTTON] Emergency triggered!");
          xSemaphoreGive(emergencySemaphore);
          lastEmergencyPress = currentTime;
        }
      }
      lastEmergencyState = state;
    }
    
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(50));
  }
}

/*
 * LCD Display Task
 * Priority: 0 (Lowest)
 * 
 * Format LCD 16x2:
 * Line 1: Floor & Direction
 * Line 2: Door & Queue Status
 */
void LCDDisplayTask(void *parameter) {
  Serial.println("[TASK] LCDDisplayTask started - Priority: 0");
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  while (1) {
    if (xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      char line1[17] = {0};
      char line2[17] = {0};
      
      // Read Lift State
      int curr = 1, targ = 1;
      char door = 'C';
      char emg = ' ';
      const char* dirStr = "IDLE";
      int queueCount = 0;
      
      if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        curr = lift.currentFloor;
        targ = lift.targetFloor;
        
        // Direction as full word
        if (lift.emergencyMode) {
          emg = '!';
        } else if (lift.direction == MOVING_UP) {
          dirStr = "UP";
        } else if (lift.direction == MOVING_DOWN) {
          dirStr = "DOWN";
        } else {
          dirStr = "IDLE";
        }
        
        // Door
        if (lift.doorState == DOOR_OPEN) door = 'O';
        else if (lift.doorState == DOOR_OPENING) door = 'o';
        else if (lift.doorState == DOOR_CLOSING) door = 'c';
        else door = 'C';
        
        // Count pending requests
        queueCount = 0;
        for (int f = 1; f <= 5; f++) {
          if (lift.floorRequested[f]) queueCount++;
        }
        
        xSemaphoreGive(liftStateMutex);
      }
      
      // Format Line 1: Floor: 1>3 UP
      if (emg == '!') {
        snprintf(line1, 17, "Floor:%d>%d EMRG!", curr, targ);
      } else {
        snprintf(line1, 17, "Floor:%d>%d %s", curr, targ, dirStr);
      }
      
      // Format Line 2: Door:O  Req:2
      snprintf(line2, 17, "Door:%c  Req:%d   ", door, queueCount);
      
      // Update LCD
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(line1);
      lcd.setCursor(0, 1);
      lcd.print(line2);
      
      xSemaphoreGive(lcdMutex);
    }
    
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(500));
  }
}


void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n");
  Serial.println("╔═══════════════════════════════════════════════╗");
  Serial.println("║    LIFT CONTROL SYSTEM - FreeRTOS             ║");
  Serial.println("║    1 Lift × 5 Floors with LCD Display        ║");
  Serial.println("╚═══════════════════════════════════════════════╝\n");
  
  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("LIFT SYSTEM");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");
  delay(2000);
  
  Serial.println("[INIT] LCD initialized");
  
  // Initialize GPIO - Internal Buttons
  pinMode(FLOOR_1_BTN, INPUT_PULLUP);
  pinMode(FLOOR_2_BTN, INPUT_PULLUP);
  pinMode(FLOOR_3_BTN, INPUT_PULLUP);
  pinMode(FLOOR_4_BTN, INPUT_PULLUP);
  pinMode(FLOOR_5_BTN, INPUT_PULLUP);
  pinMode(EMERGENCY_BTN, INPUT_PULLUP);
  pinMode(OPEN_DOOR_BTN, INPUT_PULLUP);
  pinMode(CLOSE_DOOR_BTN, INPUT_PULLUP);
  
  Serial.println("[INIT] GPIO initialized");
  
  // Create Mutexes
  liftStateMutex = xSemaphoreCreateMutex();
  lcdMutex = xSemaphoreCreateMutex();
  
  if (!liftStateMutex || !lcdMutex) {
    Serial.println("[ERROR] Failed to create mutexes!");
    while(1);
  }
  Serial.println("[INIT] Mutexes created (with priority inheritance)");
  
  // Create Binary Semaphore
  emergencySemaphore = xSemaphoreCreateBinary();
  if (!emergencySemaphore) {
    Serial.println("[ERROR] Failed to create semaphore!");
    while(1);
  }
  Serial.println("[INIT] Binary semaphore created");
  
  Serial.println("\n[INIT] Creating tasks...");
  
  // Emergency Task (Priority 4 - Highest)
  xTaskCreate(EmergencyHandlerTask, "Emergency", 4096, NULL, 4, &emergencyTaskHandle);
  
  // Lift Control Task (Priority 3)
  xTaskCreate(LiftControlTask, "LiftControl", 4096, NULL, 3, &liftControlTaskHandle);
  
  // Door Control Task (Priority 2)
  xTaskCreate(DoorControlTask, "DoorControl", 4096, NULL, 2, &doorTaskHandle);
  
  // Request Handler Task (Priority 1)
  xTaskCreate(RequestHandlerTask, "RequestHandler", 4096, NULL, 1, &requestHandlerTaskHandle);
  
  // LCD Display Task (Priority 0 - Lowest)
  xTaskCreate(LCDDisplayTask, "LCDDisplay", 4096, NULL, 0, &lcdDisplayTaskHandle);
  
  Serial.println("[INIT] All tasks created successfully!");
  Serial.println("\n╔═══════════════════════════════════════════════╗");
  Serial.println("║  SYSTEM READY - Lift at Floor 1              ║");
  Serial.println("║  Use buttons to control the lift             ║");
  Serial.println("║  Status displayed on LCD1602                  ║");
  Serial.println("╚═══════════════════════════════════════════════╝\n");
}

void loop() {
  // Empty - all logic in FreeRTOS tasks
  vTaskDelay(pdMS_TO_TICKS(1000));
}
