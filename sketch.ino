/*
 * SIMULASI SISTEM KONTROL LIFT MULTI-LANTAI BERBASIS FreeRTOS
 * Target: ESP32 dengan Wokwi Simulator
 * 
 * Fitur RTOS:
 * - 5 Tasks dengan priority-based scheduling
 * - Queue untuk floor request
 * - Mutex untuk shared state protection (priority inheritance)
 * - Binary Semaphore untuk ISR deferred processing
 * - ISR untuk emergency button
 * - Stack monitoring
 * - Fault-tolerant emergency handling
 */

#include <Arduino.h>

// Pin Definitions
#define FLOOR_1_BTN     13
#define FLOOR_2_BTN     12
#define FLOOR_3_BTN     14
#define FLOOR_4_BTN     27
#define FLOOR_5_BTN     26
#define EMERGENCY_BTN   25
#define DOOR_OBSTACLE_BTN 33

#define LED_UP          19
#define LED_DOWN        18
#define LED_DOOR        5
#define LED_EMERGENCY   17

// Elevator States
enum Direction { IDLE, MOVING_UP, MOVING_DOWN };
enum DoorState { DOOR_CLOSED, DOOR_OPENING, DOOR_OPEN, DOOR_CLOSING };

// Shared State Structure (Protected by mutex)
typedef struct {
  int currentFloor;
  int targetFloor;
  Direction direction;
  DoorState doorState;
  bool emergencyMode;
  bool doorObstacle;
} ElevatorState;

// Global Variables
ElevatorState elevatorState = {1, 1, IDLE, DOOR_CLOSED, false, false};

// FreeRTOS Handles
QueueHandle_t floorRequestQueue;
SemaphoreHandle_t elevatorStateMutex;
SemaphoreHandle_t emergencySemaphore;
TaskHandle_t emergencyTaskHandle;

TaskHandle_t elevatorControlTaskHandle;
TaskHandle_t doorControlTaskHandle;
TaskHandle_t requestHandlerTaskHandle;
TaskHandle_t displayLoggerTaskHandle;

// ISR Variables
volatile bool emergencyButtonPressed = false;

/*
 * ISR untuk Emergency Button
 * Prinsip: ISR harus singkat dan cepat
 * - Tidak melakukan processing berat
 * - Hanya memberi semaphore untuk deferred processing
 * - EmergencyHandlerTask akan melakukan processing sesungguhnya
 */
void IRAM_ATTR emergencyButtonISR() {
  emergencyButtonPressed = true;
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  
  // Gunakan FromISR variant untuk signal dari interrupt context
  xSemaphoreGiveFromISR(emergencySemaphore, &xHigherPriorityTaskWoken);
  
  // Yield jika task prioritas lebih tinggi terbangun
  if (xHigherPriorityTaskWoken) {
    portYIELD_FROM_ISR();
  }
}

/*
 * Emergency Handler Task - PRIORITAS TERTINGGI
 * Priority: 4 (Highest)
 * Fungsi: Menangani kondisi emergency dengan deferred interrupt processing
 * 
 * Strategi:
 * - Wait pada binary semaphore dari ISR
 * - Ketika emergency aktif: stop lift, buka pintu, clear queue
 * - Gunakan mutex untuk update shared state dengan aman
 */
void EmergencyHandlerTask(void *parameter) {
  Serial.println("[TASK] EmergencyHandlerTask started - Priority: 4 (HIGHEST)");
  
  while (1) {
    // Wait untuk semaphore dari ISR (blocking wait - OK untuk emergency task)
    if (xSemaphoreTake(emergencySemaphore, portMAX_DELAY) == pdTRUE) {
      Serial.println("\n[EMERGENCY] *** EMERGENCY BUTTON PRESSED! ***");
      
      // Acquire mutex dengan timeout untuk keamanan
      if (xSemaphoreTake(elevatorStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Critical Section Start
        elevatorState.emergencyMode = true;
        elevatorState.direction = IDLE;
        elevatorState.doorState = DOOR_OPEN;
        // Critical Section End
        xSemaphoreGive(elevatorStateMutex);
        
        // Clear semua request di queue
        xQueueReset(floorRequestQueue);
        
        // Update LED
        digitalWrite(LED_UP, LOW);
        digitalWrite(LED_DOWN, LOW);
        digitalWrite(LED_DOOR, HIGH);
        digitalWrite(LED_EMERGENCY, HIGH);
        
        Serial.println("[EMERGENCY] Lift stopped, door opened, queue cleared");
        Serial.println("[EMERGENCY] Press emergency button again to reset");
        
        // Wait untuk emergency reset (simulated dengan delay)
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        // Reset emergency mode
        if (xSemaphoreTake(elevatorStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          elevatorState.emergencyMode = false;
          digitalWrite(LED_EMERGENCY, LOW);
          xSemaphoreGive(elevatorStateMutex);
          Serial.println("[EMERGENCY] Emergency mode cleared. System normal.\n");
        }
      } else {
        Serial.println("[EMERGENCY] ERROR: Could not acquire mutex!");
      }
    }
  }
}

/*
 * Elevator Control Task - PRIORITAS TINGGI
 * Priority: 3
 * Fungsi: Mengontrol pergerakan lift dari lantai ke lantai
 * 
 * Alur:
 * - Ambil request dari queue
 * - Tentukan arah (UP/DOWN)
 * - Simulasi pergerakan dengan delay periodik
 * - Update current floor
 * - Berhenti saat mencapai target floor
 */
void ElevatorControlTask(void *parameter) {
  Serial.println("[TASK] ElevatorControlTask started - Priority: 3 (HIGH)");
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  Serial.println("[ELEVATOR] Ready to receive floor requests from queue");
  
  while (1) {
    int requestedFloor;
    
    // Check apakah ada request di queue (non-blocking check)
    if (xQueueReceive(floorRequestQueue, &requestedFloor, 0) == pdTRUE) {
      Serial.printf("[ELEVATOR] *** New request received from queue: Floor %d ***\n", requestedFloor);
      
      // Acquire mutex untuk update target floor
      if (xSemaphoreTake(elevatorStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        elevatorState.targetFloor = requestedFloor;
        Serial.printf("[ELEVATOR] Target floor updated to: %d\n", requestedFloor);
        xSemaphoreGive(elevatorStateMutex);
      }
    }
    
    // Main elevator control logic
    if (xSemaphoreTake(elevatorStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      // Simpan state lokal untuk meminimalkan critical section
      int current = elevatorState.currentFloor;
      int target = elevatorState.targetFloor;
      bool emergency = elevatorState.emergencyMode;
      DoorState door = elevatorState.doorState;
      
      xSemaphoreGive(elevatorStateMutex);  // Release mutex secepat mungkin
      
      // Jangan gerakkan lift jika emergency atau pintu belum tertutup
      if (emergency) {
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(500));
        continue;
      }
      
      if (door != DOOR_CLOSED) {
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(500));
        continue;
      }
      
      // Tentukan arah dan gerakkan lift
      if (current < target) {
        // Naik
        if (xSemaphoreTake(elevatorStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          elevatorState.direction = MOVING_UP;
          elevatorState.currentFloor++;
          xSemaphoreGive(elevatorStateMutex);
        }
        
        digitalWrite(LED_UP, HIGH);
        digitalWrite(LED_DOWN, LOW);
        Serial.printf("[ELEVATOR] Moving UP: Floor %d -> %d\n", current, current + 1);
        
      } else if (current > target) {
        // Turun
        if (xSemaphoreTake(elevatorStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          elevatorState.direction = MOVING_DOWN;
          elevatorState.currentFloor--;
          xSemaphoreGive(elevatorStateMutex);
        }
        
        digitalWrite(LED_UP, LOW);
        digitalWrite(LED_DOWN, HIGH);
        Serial.printf("[ELEVATOR] Moving DOWN: Floor %d -> %d\n", current, current - 1);
        
      } else {
        // Sudah di target floor
        if (xSemaphoreTake(elevatorStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          // Only log arrival if we were moving before
          if (elevatorState.direction != IDLE) {
            Serial.printf("[ELEVATOR] Arrived at Floor %d\n", current);
          }
          elevatorState.direction = IDLE;
          xSemaphoreGive(elevatorStateMutex);
        }
        
        digitalWrite(LED_UP, LOW);
        digitalWrite(LED_DOWN, LOW);
      }
    }
    
    // Delay periodik untuk simulasi pergerakan (1 detik per lantai)
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));
  }
}

/*
 * Door Control Task - PRIORITAS SEDANG
 * Priority: 2
 * Fungsi: Mengontrol pembukaan dan penutupan pintu
 * 
 * Alur:
 * - Cek apakah lift sudah sampai di target floor
 * - Buka pintu (simulasi dengan delay)
 * - Tunggu beberapa detik
 * - Cek door obstacle sensor
 * - Tutup pintu jika aman
 */
void DoorControlTask(void *parameter) {
  Serial.println("[TASK] DoorControlTask started - Priority: 2 (MEDIUM)");
  TickType_t xLastWakeTime = xTaskGetTickCount();
  bool doorProcessed = false;  // Flag to track if door already processed for current floor
  
  while (1) {
    bool shouldOpenDoor = false;
    bool obstacleDetected = digitalRead(DOOR_OBSTACLE_BTN) == LOW;
    
    // Update door obstacle state
    if (xSemaphoreTake(elevatorStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      elevatorState.doorObstacle = obstacleDetected;
      
      // Buka pintu hanya jika:
      // 1. Lift idle dan di target floor
      // 2. Pintu tertutup
      // 3. Tidak emergency
      // 4. Lift baru saja tiba (direction berubah dari MOVING ke IDLE)
      if (elevatorState.direction == IDLE && 
          elevatorState.currentFloor == elevatorState.targetFloor &&
          elevatorState.doorState == DOOR_CLOSED &&
          !elevatorState.emergencyMode &&
          !doorProcessed) {
        shouldOpenDoor = true;
      }
      
      // Reset flag jika lift mulai bergerak
      if (elevatorState.direction != IDLE) {
        doorProcessed = false;
      }
      
      xSemaphoreGive(elevatorStateMutex);
    }
    
    if (shouldOpenDoor) {
      // Proses buka pintu
      Serial.println("[DOOR] Opening door...");
      if (xSemaphoreTake(elevatorStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        elevatorState.doorState = DOOR_OPENING;
        xSemaphoreGive(elevatorStateMutex);
      }
      
      vTaskDelay(pdMS_TO_TICKS(500));  // Simulasi waktu buka pintu
      
      if (xSemaphoreTake(elevatorStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        elevatorState.doorState = DOOR_OPEN;
        xSemaphoreGive(elevatorStateMutex);
      }
      digitalWrite(LED_DOOR, HIGH);
      Serial.println("[DOOR] Door opened");
      
      // Tunggu penumpang keluar/masuk
      vTaskDelay(pdMS_TO_TICKS(3000));
      
      // Cek obstacle sebelum tutup pintu
      bool canCloseDoor = false;
      while (!canCloseDoor) {
        obstacleDetected = digitalRead(DOOR_OBSTACLE_BTN) == LOW;
        
        if (obstacleDetected) {
          Serial.println("[DOOR] Obstacle detected! Cannot close door. Reopening...");
          vTaskDelay(pdMS_TO_TICKS(2000));
        } else {
          canCloseDoor = true;  // Aman untuk tutup
        }
      }
      
      // Proses tutup pintu
      Serial.println("[DOOR] Closing door...");
      if (xSemaphoreTake(elevatorStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        elevatorState.doorState = DOOR_CLOSING;
        xSemaphoreGive(elevatorStateMutex);
      }
      
      vTaskDelay(pdMS_TO_TICKS(500));  // Simulasi waktu tutup pintu
      
      if (xSemaphoreTake(elevatorStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        elevatorState.doorState = DOOR_CLOSED;
        xSemaphoreGive(elevatorStateMutex);
      }
      digitalWrite(LED_DOOR, LOW);
      Serial.println("[DOOR] Door closed\n");
      
      // Set flag bahwa door sudah diproses untuk floor ini
      doorProcessed = true;
    }
    
    // Delay periodik
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(500));
  }
}

/*
 * Request Handler Task - PRIORITAS SEDANG-RENDAH
 * Priority: 1
 * Fungsi: Menerima request lantai dari Serial Monitor atau Button
 * 
 * INPUT METHODS:
 * 1. Serial Monitor: Ketik angka 1-5 untuk request floor
 * 2. Button: (jika wiring benar di Wokwi)
 * 
 * Commands:
 * - Type '1' to '5' = Request floor 1-5
 * - Type 'e' = Emergency stop
 * - Type 'o' = Toggle obstacle
 */
void RequestHandlerTask(void *parameter) {
  Serial.println("[TASK] RequestHandlerTask started - Priority: 1 (MEDIUM-LOW)");
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  // Array untuk debouncing button
  bool lastButtonState[5];
  unsigned long lastPressTime[5] = {0, 0, 0, 0, 0};
  const unsigned long debounceDelay = 300;
  
  int buttonPins[5] = {FLOOR_1_BTN, FLOOR_2_BTN, FLOOR_3_BTN, FLOOR_4_BTN, FLOOR_5_BTN};
  
  // Initialize button states
  for (int i = 0; i < 5; i++) {
    lastButtonState[i] = digitalRead(buttonPins[i]);
  }
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║  ELEVATOR READY - TWO INPUT MODES:   ║");
  Serial.println("╠════════════════════════════════════════╣");
  Serial.println("║  1. SERIAL INPUT (Recommended)        ║");
  Serial.println("║     Type '1' to '5' → Request floor   ║");
  Serial.println("║     Type 'e' → Emergency stop         ║");
  Serial.println("║     Type 'o' → Toggle obstacle        ║");
  Serial.println("║                                        ║");
  Serial.println("║  2. BUTTON INPUT (if wired correctly) ║");
  Serial.println("║     Click buttons in Wokwi diagram    ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  int pollCount = 0;
  
  while (1) {
    unsigned long currentTime = millis();
    
    // === METHOD 1: SERIAL INPUT (ALWAYS WORKS) ===
    if (Serial.available() > 0) {
      char input = Serial.read();
      
      // Floor request (1-5)
      if (input >= '1' && input <= '5') {
        int requestedFloor = input - '0';
        
        Serial.printf("\n[SERIAL INPUT] Floor %d requested\n", requestedFloor);
        
        if (xQueueSend(floorRequestQueue, &requestedFloor, pdMS_TO_TICKS(100)) == pdTRUE) {
          Serial.printf("[REQUEST] Floor %d added to queue successfully ✓\n\n", requestedFloor);
        } else {
          Serial.printf("[REQUEST] ERROR: Queue full! ✗\n\n");
        }
      }
      // Emergency command
      else if (input == 'e' || input == 'E') {
        Serial.println("\n[SERIAL INPUT] Emergency triggered!");
        xSemaphoreGive(emergencySemaphore);
      }
      // Obstacle toggle
      else if (input == 'o' || input == 'O') {
        Serial.println("\n[SERIAL INPUT] Obstacle toggled (not implemented in serial mode)\n");
      }
    }
    
    // === METHOD 2: BUTTON INPUT (May not work if wiring issue) ===
    // Debug: Print button states periodically
    pollCount++;
    if (pollCount >= 100) {
      Serial.print("[BUTTON DEBUG] States: ");
      for (int i = 0; i < 5; i++) {
        Serial.printf("F%d=%s ", i+1, digitalRead(buttonPins[i]) == HIGH ? "H" : "L");
      }
      Serial.println("(H=Released, L=Pressed)");
      pollCount = 0;
    }
    
    // Check emergency mode
    bool emergency = false;
    if (xSemaphoreTake(elevatorStateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      emergency = elevatorState.emergencyMode;
      xSemaphoreGive(elevatorStateMutex);
    }
    
    if (!emergency) {
      // Read button states
      for (int i = 0; i < 5; i++) {
        bool currentState = digitalRead(buttonPins[i]);
        
        // Detect button press (HIGH->LOW transition)
        if (currentState == LOW && lastButtonState[i] == HIGH) {
          if ((currentTime - lastPressTime[i]) > debounceDelay) {
            int requestedFloor = i + 1;
            
            Serial.printf("\n[BUTTON INPUT] Floor %d button pressed (GPIO %d)\n", requestedFloor, buttonPins[i]);
            
            if (xQueueSend(floorRequestQueue, &requestedFloor, pdMS_TO_TICKS(100)) == pdTRUE) {
              Serial.printf("[REQUEST] Floor %d added to queue successfully ✓\n\n", requestedFloor);
            } else {
              Serial.printf("[REQUEST] ERROR: Queue full! ✗\n\n");
            }
            
            lastPressTime[i] = currentTime;
          }
        }
        
        lastButtonState[i] = currentState;
      }
    }
    
    // Delay periodik
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(50));
  }
}

/*
 * Display Logger Task - PRIORITAS RENDAH
 * Priority: 0 (Lowest)
 * Fungsi: Menampilkan status sistem secara periodik dan monitoring stack
 * 
 * Output:
 * - Current floor, target floor, direction
 * - Door state
 * - Emergency mode
 * - Queue status
 * - Stack high water mark setiap task
 */
void DisplayLoggerTask(void *parameter) {
  Serial.println("[TASK] DisplayLoggerTask started - Priority: 0 (LOWEST)");
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  while (1) {
    // Tunggu 5 detik antara log
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(5000));
    
    Serial.println("\n========== SYSTEM STATUS ==========");
    
    // Baca elevator state
    if (xSemaphoreTake(elevatorStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      Serial.printf("Current Floor: %d\n", elevatorState.currentFloor);
      Serial.printf("Target Floor: %d\n", elevatorState.targetFloor);
      
      Serial.print("Direction: ");
      switch(elevatorState.direction) {
        case IDLE: Serial.println("IDLE"); break;
        case MOVING_UP: Serial.println("MOVING UP"); break;
        case MOVING_DOWN: Serial.println("MOVING DOWN"); break;
      }
      
      Serial.print("Door State: ");
      switch(elevatorState.doorState) {
        case DOOR_CLOSED: Serial.println("CLOSED"); break;
        case DOOR_OPENING: Serial.println("OPENING"); break;
        case DOOR_OPEN: Serial.println("OPEN"); break;
        case DOOR_CLOSING: Serial.println("CLOSING"); break;
      }
      
      Serial.printf("Emergency Mode: %s\n", elevatorState.emergencyMode ? "ACTIVE" : "Normal");
      Serial.printf("Door Obstacle: %s\n", elevatorState.doorObstacle ? "DETECTED" : "Clear");
      
      xSemaphoreGive(elevatorStateMutex);
    }
    
    // Queue status
    UBaseType_t queueWaiting = uxQueueMessagesWaiting(floorRequestQueue);
    UBaseType_t queueAvailable = uxQueueSpacesAvailable(floorRequestQueue);
    Serial.printf("Queue Status: %d requests waiting, %d spaces available\n", queueWaiting, queueAvailable);
    
    // Stack monitoring - High Water Mark (minimum free stack space)
    Serial.println("\n--- Stack Monitoring (High Water Mark) ---");
    if (emergencyTaskHandle != NULL) {
      UBaseType_t hwm = uxTaskGetStackHighWaterMark(emergencyTaskHandle);
      Serial.printf("EmergencyHandlerTask: %u words remaining\n", hwm);
    }
    if (elevatorControlTaskHandle != NULL) {
      UBaseType_t hwm = uxTaskGetStackHighWaterMark(elevatorControlTaskHandle);
      Serial.printf("ElevatorControlTask: %u words remaining\n", hwm);
    }
    if (doorControlTaskHandle != NULL) {
      UBaseType_t hwm = uxTaskGetStackHighWaterMark(doorControlTaskHandle);
      Serial.printf("DoorControlTask: %u words remaining\n", hwm);
    }
    if (requestHandlerTaskHandle != NULL) {
      UBaseType_t hwm = uxTaskGetStackHighWaterMark(requestHandlerTaskHandle);
      Serial.printf("RequestHandlerTask: %u words remaining\n", hwm);
    }
    if (displayLoggerTaskHandle != NULL) {
      UBaseType_t hwm = uxTaskGetStackHighWaterMark(displayLoggerTaskHandle);
      Serial.printf("DisplayLoggerTask: %u words remaining\n", hwm);
    }
    
    // Heap memory
    Serial.printf("Free Heap: %u bytes\n", esp_get_free_heap_size());
    
    Serial.println("===================================\n");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n");
  Serial.println("=====================================");
  Serial.println("  ELEVATOR CONTROL SYSTEM - FreeRTOS");
  Serial.println("  5-Floor Simulation with Wokwi");
  Serial.println("=====================================\n");
  
  // Initialize Pins
  // Input buttons dengan pull-up internal
  pinMode(FLOOR_1_BTN, INPUT_PULLUP);
  pinMode(FLOOR_2_BTN, INPUT_PULLUP);
  pinMode(FLOOR_3_BTN, INPUT_PULLUP);
  pinMode(FLOOR_4_BTN, INPUT_PULLUP);
  pinMode(FLOOR_5_BTN, INPUT_PULLUP);
  pinMode(EMERGENCY_BTN, INPUT_PULLUP);
  pinMode(DOOR_OBSTACLE_BTN, INPUT_PULLUP);
  
  // Output LEDs
  pinMode(LED_UP, OUTPUT);
  pinMode(LED_DOWN, OUTPUT);
  pinMode(LED_DOOR, OUTPUT);
  pinMode(LED_EMERGENCY, OUTPUT);
  
  // Initial state
  digitalWrite(LED_UP, LOW);
  digitalWrite(LED_DOWN, LOW);
  digitalWrite(LED_DOOR, LOW);
  digitalWrite(LED_EMERGENCY, LOW);
  
  Serial.println("[INIT] GPIO initialized");
  
  // Debug: Test button states
  delay(100);
  Serial.println("[DEBUG] Initial button states (HIGH=released, LOW=pressed):");
  Serial.printf("  Floor 1 (GPIO %d): %s\n", FLOOR_1_BTN, digitalRead(FLOOR_1_BTN) ? "HIGH" : "LOW");
  Serial.printf("  Floor 2 (GPIO %d): %s\n", FLOOR_2_BTN, digitalRead(FLOOR_2_BTN) ? "HIGH" : "LOW");
  Serial.printf("  Floor 3 (GPIO %d): %s\n", FLOOR_3_BTN, digitalRead(FLOOR_3_BTN) ? "HIGH" : "LOW");
  Serial.printf("  Floor 4 (GPIO %d): %s\n", FLOOR_4_BTN, digitalRead(FLOOR_4_BTN) ? "HIGH" : "LOW");
  Serial.printf("  Floor 5 (GPIO %d): %s\n", FLOOR_5_BTN, digitalRead(FLOOR_5_BTN) ? "HIGH" : "LOW");
  Serial.printf("  Emergency (GPIO %d): %s\n", EMERGENCY_BTN, digitalRead(EMERGENCY_BTN) ? "HIGH" : "LOW");
  Serial.printf("  Obstacle (GPIO %d): %s\n", DOOR_OBSTACLE_BTN, digitalRead(DOOR_OBSTACLE_BTN) ? "HIGH" : "LOW");
  
  // Create Queue
  // Queue untuk menyimpan floor request (max 10 request)
  floorRequestQueue = xQueueCreate(10, sizeof(int));
  if (floorRequestQueue == NULL) {
    Serial.println("[ERROR] Failed to create queue!");
    while(1);
  }
  Serial.println("[INIT] Floor request queue created (size: 10)");
  
  // Create Mutex
  // Mutex dengan priority inheritance untuk mencegah priority inversion
  // Priority inheritance: jika task prioritas rendah hold mutex yang dibutuhkan
  // task prioritas tinggi, maka task prioritas rendah akan temporary dinaikkan
  // prioritasnya agar cepat selesai dan release mutex
  elevatorStateMutex = xSemaphoreCreateMutex();
  if (elevatorStateMutex == NULL) {
    Serial.println("[ERROR] Failed to create mutex!");
    while(1);
  }
  Serial.println("[INIT] Elevator state mutex created (with priority inheritance)");
  
  // Create Binary Semaphore
  // Untuk ISR -> Task communication (deferred interrupt processing)
  emergencySemaphore = xSemaphoreCreateBinary();
  if (emergencySemaphore == NULL) {
    Serial.println("[ERROR] Failed to create binary semaphore!");
    while(1);
  }
  Serial.println("[INIT] Emergency binary semaphore created");
  
  // Attach Interrupt untuk Emergency Button
  attachInterrupt(digitalPinToInterrupt(EMERGENCY_BTN), emergencyButtonISR, FALLING);
  Serial.println("[INIT] Emergency button interrupt attached (FALLING edge)");
  
  Serial.println("\n[INIT] Creating tasks with priorities...");
  
  // Create Tasks dengan priority berbeda
  // Priority tinggi = angka lebih besar
  // Stack size: 2048 words (8KB) per task
  
  // Task 1: Emergency Handler (Priority 4 - HIGHEST)
  xTaskCreate(
    EmergencyHandlerTask,
    "EmergencyHandler",
    2048,
    NULL,
    4,  // Prioritas tertinggi
    &emergencyTaskHandle
  );
  
  // Task 2: Elevator Control (Priority 3 - HIGH)
  xTaskCreate(
    ElevatorControlTask,
    "ElevatorControl",
    2048,
    NULL,
    3,  // Prioritas tinggi
    &elevatorControlTaskHandle
  );
  
  // Task 3: Door Control (Priority 2 - MEDIUM)
  xTaskCreate(
    DoorControlTask,
    "DoorControl",
    2048,
    NULL,
    2,  // Prioritas sedang
    &doorControlTaskHandle
  );
  
  // Task 4: Request Handler (Priority 1 - MEDIUM-LOW)
  xTaskCreate(
    RequestHandlerTask,
    "RequestHandler",
    2048,
    NULL,
    1,  // Prioritas sedang-rendah
    &requestHandlerTaskHandle
  );
  
  // Task 5: Display Logger (Priority 0 - LOWEST)
  xTaskCreate(
    DisplayLoggerTask,
    "DisplayLogger",
    2048,
    NULL,
    0,  // Prioritas terendah
    &displayLoggerTaskHandle
  );
  
  Serial.println("[INIT] All tasks created successfully!");
  Serial.println("\n========================================");
  Serial.println("[SYSTEM] Elevator starting at Floor 1");
  Serial.println("[SYSTEM] Door CLOSED - Ready for requests");
  Serial.println("========================================");
  Serial.println("[INFO] Press floor buttons (1-5) to request elevator");
  Serial.println("[INFO] Press emergency button to trigger emergency stop");
  Serial.println("[INFO] Press door obstacle button to simulate obstacle");
  Serial.println("========================================\n");
  
  // Scheduler akan dijalankan otomatis oleh Arduino framework
  // vTaskStartScheduler() tidak diperlukan di Arduino ESP32
}

void loop() {
  // Loop kosong - semua logic ada di FreeRTOS tasks
  // Arduino loop() tetap berjalan tapi tidak digunakan
  vTaskDelay(pdMS_TO_TICKS(1000));
}
