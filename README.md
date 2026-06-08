# Simulasi Sistem Kontrol Lift Multi-Lantai Berbasis FreeRTOS

## Deskripsi Project

Sistem kontrol lift 5 lantai berbasis FreeRTOS yang disimulasikan menggunakan ESP32 di Wokwi simulator. Project ini mendemonstrasikan konsep-konsep penting dalam Real-Time Operating System:

- **Priority-based preemptive scheduling**
- **Inter-task communication** (Queue, Mutex, Semaphore)
- **Interrupt Service Routine (ISR)** dengan deferred processing
- **Synchronization mechanisms** untuk mencegah race condition
- **Priority inheritance** untuk menghindari priority inversion
- **Stack monitoring** untuk analisis penggunaan memori
- **Fault-tolerant emergency handling**

## Arsitektur Sistem

### Daftar Task dan Prioritas

| Task | Priority | Fungsi | Scheduling |
|------|----------|--------|------------|
| **EmergencyHandlerTask** | 4 (Highest) | Menangani emergency stop dengan deferred interrupt processing | Event-driven (wait on semaphore) |
| **ElevatorControlTask** | 3 (High) | Mengontrol pergerakan lift antar lantai | Periodic (1 detik) |
| **DoorControlTask** | 2 (Medium) | Mengontrol buka/tutup pintu dengan obstacle detection | Periodic (500ms) |
| **RequestHandlerTask** | 1 (Medium-Low) | Membaca input tombol dan menambahkan request ke queue | Periodic (100ms) |
| **DisplayLoggerTask** | 0 (Lowest) | Menampilkan status sistem dan monitoring stack | Periodic (5 detik) |

### Virtual Peripheral

#### Input (Push Buttons)
- **Floor 1-5 Buttons** (GPIO 13, 12, 14, 27, 26): Request lantai tujuan
- **Emergency Button** (GPIO 25): Trigger emergency stop (dengan interrupt)
- **Door Obstacle Button** (GPIO 33): Simulasi obstacle di pintu

#### Output (LED Indicators)
- **LED UP** (GPIO 19): Indikator lift naik
- **LED DOWN** (GPIO 18): Indikator lift turun
- **LED DOOR** (GPIO 5): Indikator pintu terbuka
- **LED EMERGENCY** (GPIO 17): Indikator mode emergency


## Mekanisme FreeRTOS

### 1. Queue (Floor Request Queue)

```c
QueueHandle_t floorRequestQueue;
floorRequestQueue = xQueueCreate(10, sizeof(int));
```

- **Fungsi**: Menyimpan request lantai dari user
- **Ukuran**: 10 items (setiap item adalah integer)
- **Producer**: RequestHandlerTask
- **Consumer**: ElevatorControlTask
- **Mode**: Non-blocking dengan timeout

### 2. Mutex (Elevator State Mutex)

```c
SemaphoreHandle_t elevatorStateMutex;
elevatorStateMutex = xSemaphoreCreateMutex();
```

- **Fungsi**: Melindungi shared state dari race condition
- **Protected Variables**:
  - currentFloor
  - targetFloor
  - elevatorDirection
  - doorState
  - emergencyMode
  - doorObstacle
- **Feature**: Priority Inheritance (mencegah priority inversion)
- **Timeout**: Semua acquire menggunakan timeout 100ms untuk keamanan

### 3. Binary Semaphore (Emergency Semaphore)

```c
SemaphoreHandle_t emergencySemaphore;
emergencySemaphore = xSemaphoreCreateBinary();
```

- **Fungsi**: ISR-to-Task communication untuk emergency handling
- **Given by**: ISR (emergencyButtonISR)
- **Taken by**: EmergencyHandlerTask
- **Pattern**: Deferred Interrupt Processing


## Alur ISR dan Deferred Processing

### Emergency Button Flow

1. **User menekan emergency button** → Hardware interrupt triggered
2. **ISR (emergencyButtonISR)** dijalankan:
   ```c
   void IRAM_ATTR emergencyButtonISR() {
     emergencyButtonPressed = true;
     xSemaphoreGiveFromISR(emergencySemaphore, &xHigherPriorityTaskWoken);
     portYIELD_FROM_ISR();
   }
   ```
   - ISR **TIDAK** melakukan processing berat
   - ISR hanya memberi semaphore
   - ISR sangat singkat dan cepat (< 10 instruksi)

3. **EmergencyHandlerTask terbangun** (prioritas tertinggi):
   ```c
   xSemaphoreTake(emergencySemaphore, portMAX_DELAY);
   // Lakukan processing berat di sini:
   // - Stop lift
   // - Buka pintu
   // - Clear queue
   // - Update LED
   ```
   - Processing berat dilakukan di task context (bukan ISR)
   - Aman menggunakan mutex, delay, printf, dll.

**Keuntungan Deferred Processing:**
- ISR tetap singkat → interrupt latency rendah
- Processing kompleks dilakukan di task → bisa preempted jika ada interrupt lain
- Lebih fleksibel dan aman


## Strategi Scheduling

### Preemptive Priority-Based Scheduling

FreeRTOS menggunakan **preemptive priority-based scheduling**:

1. **Task prioritas tinggi selalu berjalan duluan**
   - EmergencyHandlerTask (P4) akan preempt task lain saat emergency terjadi
   - ElevatorControlTask (P3) mengontrol lift dengan prioritas tinggi

2. **Periodic Tasks menggunakan vTaskDelayUntil()**
   ```c
   TickType_t xLastWakeTime = xTaskGetTickCount();
   while(1) {
     // Do work
     vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));
   }
   ```
   - Menjamin periode yang konsisten
   - Task masuk BLOCKED state saat delay → CPU diberikan ke task lain

3. **Critical Section dijaga singkat**
   - Mutex hanya di-hold saat akses shared data
   - Immediately release setelah selesai
   - Minimize blocking time

4. **Event-driven Task (EmergencyHandlerTask)**
   - Wait pada semaphore (BLOCKED state)
   - Terbangun hanya saat emergency terjadi
   - Tidak membuang CPU cycle


## Analisis Race Condition, Priority Inversion, dan Deadlock

### 1. Race Condition Prevention

**Masalah Potensial:**
- Banyak task mengakses `elevatorState` secara concurrent
- Tanpa proteksi → race condition → data corruption

**Solusi:**
```c
// Task A
xSemaphoreTake(elevatorStateMutex, timeout);
elevatorState.currentFloor = 3;  // Critical section
xSemaphoreGive(elevatorStateMutex);

// Task B (concurrent)
xSemaphoreTake(elevatorStateMutex, timeout);
int floor = elevatorState.currentFloor;  // Terlindungi
xSemaphoreGive(elevatorStateMutex);
```

**Strategi:**
- Semua akses ke shared state harus acquire mutex dulu
- Critical section dijaga singkat
- Timeout digunakan untuk menghindari hang forever

### 2. Priority Inversion Prevention

**Masalah:**
```
TaskHigh (P3) butuh mutex
  ↓ blocked karena...
TaskLow (P1) hold mutex
  ↓ tapi preempted oleh...
TaskMedium (P2) → TaskHigh terpaksa nunggu TaskMedium selesai!
```

**Solusi: Priority Inheritance**
```c
elevatorStateMutex = xSemaphoreCreateMutex();  // Auto priority inheritance
```

- Saat TaskLow hold mutex yang dibutuhkan TaskHigh
- TaskLow **sementara dinaikkan prioritasnya** = priority TaskHigh
- TaskLow cepat selesai → release mutex → TaskHigh bisa jalan
- TaskLow kembali ke priority aslinya

**Di project ini:**
- ElevatorControlTask (P3) sering butuh mutex
- RequestHandlerTask (P1) juga akses mutex
- Tanpa priority inheritance → RequestHandlerTask bisa block ElevatorControlTask
- Dengan priority inheritance → masalah teratasi


### 3. Deadlock Prevention

**Masalah Potensial:**
```
Task A: Lock mutex1 → tunggu mutex2
Task B: Lock mutex2 → tunggu mutex1
→ Deadlock! Keduanya saling tunggu forever
```

**Strategi Prevention dalam project:**

1. **Hanya menggunakan 1 mutex** (elevatorStateMutex)
   - Tidak ada nested mutex
   - Tidak mungkin circular wait

2. **Timeout pada semua acquire**
   ```c
   if (xSemaphoreTake(elevatorStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
     // Critical section
     xSemaphoreGive(elevatorStateMutex);
   } else {
     // Timeout - handle error
   }
   ```
   - Jika deadlock terjadi, task tidak hang forever
   - Task bisa retry atau log error

3. **Urutan resource acquisition yang konsisten**
   - Semua task acquire mutex dengan cara yang sama
   - Tidak ada nested locking

4. **Short critical sections**
   - Minimize waktu hold mutex
   - Kurangi kemungkinan contention


## Cara Menjalankan di Wokwi

### Langkah-langkah:

1. **Buka Wokwi Simulator**
   - Akses: https://wokwi.com/
   - Login dengan akun Anda

2. **Create New Project**
   - Pilih "ESP32" template
   - Atau gunakan "New Arduino Project"

3. **Upload File**
   - Copy isi `sketch.ino` ke editor code
   - Copy isi `diagram.json` ke tab diagram.json
   
4. **Start Simulation**
   - Klik tombol hijau "Start Simulation"
   - Buka Serial Monitor (ikon terminal di bawah)

5. **Interaksi:**
   - **Tekan tombol Floor 1-5**: Request lift ke lantai tersebut
   - **Tekan tombol Emergency**: Trigger emergency stop
   - **Tekan tombol Door Obstacle**: Simulasi obstacle menghalangi pintu
   - **Monitor Serial**: Lihat log real-time dari sistem

### Tips Simulasi:

- **Test Normal Operation**: Tekan beberapa tombol lantai, amati lift bergerak
- **Test Queue**: Tekan banyak tombol sekaligus, lihat request diproses berurutan
- **Test Emergency**: Tekan emergency saat lift bergerak, lihat immediate stop
- **Test Door Obstacle**: Tekan obstacle saat pintu akan tutup, lihat pintu buka kembali
- **Monitor Stack**: Perhatikan stack high water mark di log setiap 5 detik


## Contoh Serial Log

```
=====================================
  ELEVATOR CONTROL SYSTEM - FreeRTOS
  5-Floor Simulation with Wokwi
=====================================

[INIT] GPIO initialized
[INIT] Floor request queue created (size: 10)
[INIT] Elevator state mutex created (with priority inheritance)
[INIT] Emergency binary semaphore created
[INIT] Emergency button interrupt attached (FALLING edge)

[INIT] Creating tasks with priorities...
[INIT] All tasks created successfully!

[SYSTEM] Elevator starting at Floor 1
[SYSTEM] Press floor buttons (1-5) to request elevator
[SYSTEM] Press emergency button to trigger emergency stop
[SYSTEM] Press door obstacle button to simulate obstacle

[TASK] EmergencyHandlerTask started - Priority: 4 (HIGHEST)
[TASK] ElevatorControlTask started - Priority: 3 (HIGH)
[TASK] DoorControlTask started - Priority: 2 (MEDIUM)
[TASK] RequestHandlerTask started - Priority: 1 (MEDIUM-LOW)
[TASK] DisplayLoggerTask started - Priority: 0 (LOWEST)

[REQUEST] Floor 3 button pressed - Added to queue
[ELEVATOR] New request received: Floor 3
[ELEVATOR] Moving UP: Floor 1 -> 2
[ELEVATOR] Moving UP: Floor 2 -> 3
[ELEVATOR] Arrived at Floor 3
[DOOR] Opening door...
[DOOR] Door opened
[DOOR] Closing door...
[DOOR] Door closed

========== SYSTEM STATUS ==========
Current Floor: 3
Target Floor: 3
Direction: IDLE
Door State: CLOSED
Emergency Mode: Normal
Door Obstacle: Clear
Queue Status: 0 requests waiting, 10 spaces available

--- Stack Monitoring (High Water Mark) ---
EmergencyHandlerTask: 1654 words remaining
ElevatorControlTask: 1432 words remaining
DoorControlTask: 1398 words remaining
RequestHandlerTask: 1512 words remaining
DisplayLoggerTask: 1287 words remaining
Free Heap: 245632 bytes
===================================

[REQUEST] Floor 5 button pressed - Added to queue
[ELEVATOR] New request received: Floor 5
[ELEVATOR] Moving UP: Floor 3 -> 4
[ELEVATOR] Moving UP: Floor 4 -> 5

[EMERGENCY] *** EMERGENCY BUTTON PRESSED! ***
[EMERGENCY] Lift stopped, door opened, queue cleared
[EMERGENCY] Press emergency button again to reset
[EMERGENCY] Emergency mode cleared. System normal.
```


## Fitur-Fitur RTOS yang Didemonstrasikan

### ✅ Task Management
- 5 tasks dengan fungsi berbeda
- Priority-based scheduling (0-4)
- Periodic dan event-driven tasks

### ✅ Inter-Task Communication
- **Queue**: Request lantai dari user ke elevator control
- **Mutex**: Proteksi shared elevator state
- **Binary Semaphore**: ISR to task notification

### ✅ Synchronization
- Mutex dengan priority inheritance
- Timeout mechanism untuk safety
- Critical section management

### ✅ Interrupt Handling
- Hardware interrupt pada emergency button
- ISR singkat dan cepat (< 10 instruksi)
- Deferred interrupt processing

### ✅ Memory Management
- Stack monitoring (high water mark)
- Heap monitoring
- Stack size allocation per task

### ✅ Scheduling Analysis
- Preemptive scheduling demonstration
- Priority inversion handling
- CPU utilization optimization

### ✅ Fault Tolerance
- Emergency stop mechanism
- Timeout pada semua blocking operation
- Graceful degradation


## Struktur Kode

```
ElevatorSimulation2/
├── sketch.ino           # Main code dengan semua task implementation
├── diagram.json         # Wokwi circuit diagram
├── README.md           # Dokumentasi project (file ini)
└── penjelasan_task.md  # Penjelasan detail setiap task
```

## Requirements

- **Platform**: ESP32
- **Framework**: Arduino + FreeRTOS
- **Simulator**: Wokwi (https://wokwi.com)
- **Development Tool**: VSCode dengan PlatformIO atau Wokwi online

## Konsep RTOS yang Dipelajari

1. **Real-Time Scheduling**: Priority-based preemptive scheduling
2. **Concurrency**: Multiple tasks running "simultaneously"
3. **Synchronization**: Mutex, semaphore, queue
4. **Critical Section**: Protected shared resource access
5. **Interrupt Handling**: ISR dan deferred processing
6. **Memory Management**: Stack allocation dan monitoring
7. **Priority Inversion**: Penyebab dan solusinya
8. **Deadlock**: Prevention strategies
9. **Race Condition**: Detection dan prevention

## Author

Elevator Control System FreeRTOS Simulation  
Educational Project untuk pembelajaran Real-Time Operating System

## License

MIT License - Free to use for educational purposes
