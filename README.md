# Simulasi Sistem Kontrol Lift Multi-Lantai Berbasis FreeRTOS

## Deskripsi Project

Sistem kontrol lift 5 lantai berbasis FreeRTOS yang disimulasikan menggunakan ESP32 di Wokwi simulator. Project ini mendemonstrasikan konsep-konsep penting dalam Real-Time Operating System:

- **Priority-based preemptive scheduling**
- **Synchronization mechanisms** (Mutex, Binary Semaphore)
- **State machine** untuk kontrol lift dan pintu
- **Priority inheritance** untuk menghindari priority inversion
- **Elevator SCAN scheduling** untuk penjadwalan request lantai
- **Fault-tolerant emergency handling**

## Arsitektur Sistem

### Daftar Task dan Prioritas

| Task | Priority | Fungsi | Scheduling |
|------|----------|--------|------------|
| **EmergencyHandlerTask** | 4 (Highest) | Menangani emergency stop | Event-driven (wait on semaphore) |
| **LiftControlTask** | 3 (High) | Mengontrol pergerakan lift dengan SCAN scheduling | Periodic (1 detik) |
| **DoorControlTask** | 2 (Medium) | Mengontrol buka/tutup pintu (departure & arrival) | Periodic (500ms) |
| **RequestHandlerTask** | 1 (Medium-Low) | Membaca input tombol fisik | Periodic (50ms) |
| **LCDDisplayTask** | 0 (Lowest) | Menampilkan status real-time di LCD1602 | Periodic (500ms) |

### Tombol Fisik (Input)

| Tombol | GPIO | Fungsi |
|--------|------|--------|
| **Floor 1** | 13 | Request lantai 1 |
| **Floor 2** | 12 | Request lantai 2 |
| **Floor 3** | 14 | Request lantai 3 |
| **Floor 4** | 27 | Request lantai 4 |
| **Floor 5** | 26 | Request lantai 5 |
| **Emergency** | 25 | Trigger emergency stop |
| **Open Door** | 33 | Buka pintu manual |
| **Close Door** | 32 | Tutup pintu manual |

### Output

- **LCD1602 I2C** (SDA: GPIO 21, SCL: GPIO 22)
  - Baris atas: `Floor: X>Y UP/DOWN/IDLE`
  - Baris bawah: `Door:X  Req:N`

## Mekanisme FreeRTOS

### 1. Mutex (Lift State Mutex)

```c
liftStateMutex = xSemaphoreCreateMutex();
```

- **Fungsi**: Melindungi shared state dari race condition
- **Protected Variables**:
  - `currentFloor`
  - `targetFloor`
  - `direction`
  - `doorState`
  - `emergencyMode`
  - `manualDoorOpen`, `manualDoorClose`
  - `needsDepartureDoor`, `needsArrivalDoor`
  - `floorRequested[]`, `floorRequestOrder[]`
- **Feature**: Priority Inheritance (mencegah priority inversion)
- **Timeout**: Semua acquire menggunakan timeout 100ms untuk keamanan

### 2. Binary Semaphore (Emergency Semaphore)

```c
emergencySemaphore = xSemaphoreCreateBinary();
```

- **Fungsi**: Task-to-task communication untuk emergency handling
- **Given by**: RequestHandlerTask (saat tombol emergency ditekan)
- **Taken by**: EmergencyHandlerTask
- **Pattern**: Event-driven deferred processing

## Algoritma SCAN Scheduling

Sistem tidak menggunakan queue FIFO murni, melainkan algoritma elevator SCAN:

1. **Saat idle**: lift memilih request lantai **terdekat** dari posisi saat ini
2. **Jika ada tie** (jarak sama): dipilih berdasarkan urutan request pertama (FIFO tie-break)
3. **Selama bergerak satu arah**: lift berhenti di setiap lantai yang direquest di jalur tersebut
4. **Hanya balik arah** setelah melayani request terakhir di arah tersebut

### Contoh Alur

**Contoh 1 — Dari Lantai 2, request Lantai 5 lalu Lantai 4:**
Kedua request ke arah **ATAS**, maka yang terdekat dilayani dulu:
```
Lantai 2 → buka pintu (departure) → tutup → Lantai 3 → Lantai 4 (buka/tutup) → Lantai 5 (buka/tutup)
```

**Contoh 2 — Dari Lantai 3, request Lantai 5 lalu Lantai 1:**
Request pertama ke arah **ATAS**, lift naik dulu, lalu turun:
```
Lantai 3 → buka pintu (departure) → tutup → Lantai 4 → Lantai 5 (buka/tutup) → Lantai 4 → Lantai 3 → Lantai 2 → Lantai 1 (buka/tutup)
```

## Alur Kerja Lift

### Sequence Normal (Request Baru)

1. User menekan tombol lantai → `floorRequested[floor] = true`
2. `LiftControlTask` (saat idle) memilih target terdekat
3. `DoorControlTask` membuka dan menutup pintu di lantai asal (**departure door**)
4. `LiftControlTask` menggerakkan lift satu lantai per detik
5. Saat tiba di lantai tujuan, `DoorControlTask` membuka dan menutup pintu (**arrival door**)
6. Jika masih ada request, lift melanjutkan perjalanan

### Emergency Flow

1. User menekan tombol Emergency
2. `RequestHandlerTask` memberi `emergencySemaphore`
3. `EmergencyHandlerTask` (prioritas tertinggi) terbangun:
   - Set `emergencyMode = true`
   - Buka pintu
   - Hapus semua request
   - Tunggu 5 detik
   - Clear emergency mode

## Analisis Race Condition, Priority Inversion, dan Deadlock

### 1. Race Condition Prevention

Semua akses ke shared state `lift` dilakukan melalui `liftStateMutex` dengan timeout:

```c
if (xSemaphoreTake(liftStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
  // Critical section
  lift.currentFloor = ...;
  xSemaphoreGive(liftStateMutex);
}
```

### 2. Priority Inversion Prevention

Mutex dibuat dengan priority inheritance secara otomatis:
```c
liftStateMutex = xSemaphoreCreateMutex();
```

### 3. Deadlock Prevention

- Hanya menggunakan **1 mutex** (`liftStateMutex`)
- Tidak ada nested mutex
- Semua blocking call menggunakan timeout
- Critical section dijaga singkat

## Cara Menjalankan di Wokwi

### Langkah-langkah:

1. **Buka Wokwi Simulator**
   - Akses: https://wokwi.com/
   - Login dengan akun Anda

2. **Create New Project**
   - Pilih "ESP32" template

3. **Upload File**
   - Copy isi `sketch.ino` ke editor code
   - Copy isi `diagram.json` ke tab diagram.json
   - Pastikan file tetap bernama `sketch.ino` (bukan `.c` atau `.cpp`)

4. **Start Simulation**
   - Klik tombol hijau "Start Simulation"
   - Buka Serial Monitor (ikon terminal di bawah)

5. **Interaksi:**
   - **Tekan tombol Floor 1-5**: Request lift ke lantai tersebut
   - **Tekan tombol Emergency**: Trigger emergency stop
   - **Tekan tombol Open/Close**: Kontrol pintu manual
   - **Monitor LCD**: Lihat status real-time lift

### Tips Simulasi:

- **Test Normal Operation**: Tekan beberapa tombol lantai, amati lift bergerak
- **Test SCAN Scheduling**: Dari Lantai 2, tekan Floor 5 lalu Floor 4 → lift akan berhenti di Floor 4 dulu
- **Test Reverse**: Dari Lantai 3, tekan Floor 5 lalu Floor 1 → lift naik ke 5, lalu turun ke 1
- **Test Emergency**: Tekan emergency saat lift bergerak, lihat immediate stop
- **Test Manual Door**: Tekan Open/Close saat pintu sedang bergerak

## Contoh Serial Log

```
╔═══════════════════════════════════════════════╗
║    LIFT CONTROL SYSTEM - FreeRTOS             ║
║    1 Lift × 5 Floors with LCD Display        ║
╚═══════════════════════════════════════════════╝

[INIT] LCD initialized
[INIT] GPIO initialized
[INIT] Mutexes created (with priority inheritance)
[INIT] Binary semaphore created

[INIT] Creating tasks...
[TASK] EmergencyHandlerTask started - Priority: 4
[TASK] LiftControlTask started - Priority: 3
[TASK] DoorControlTask started - Priority: 2
[TASK] RequestHandlerTask started - Priority: 1
[TASK] LCDDisplayTask started - Priority: 0

╔═══════════════════════════════════════════════╗
║  SYSTEM READY - Lift at Floor 1              ║
║  Use buttons to control the lift             ║
║  Status displayed on LCD1602                  ║
╚═══════════════════════════════════════════════╝

[BUTTON] Floor 5 pressed
[LIFT] New request: Floor 5
[DOOR] Opening (departure)...
[DOOR] Opened
[DOOR] Closing...
[DOOR] Closed

[LIFT] Moving UP: Floor 1 -> 2
[LIFT] Moving UP: Floor 2 -> 3
[LIFT] Moving UP: Floor 3 -> 4
[LIFT] Moving UP: Floor 4 -> 5
[LIFT] Arrived at Floor 5
[DOOR] Opening (arrival)...
[DOOR] Opened
[DOOR] Closing...
[DOOR] Closed
```

## Fitur-Fitur RTOS yang Didemonstrasikan

### ✅ Task Management
- 5 tasks dengan fungsi berbeda
- Priority-based scheduling (0-4)
- Periodic dan event-driven tasks

### ✅ Synchronization
- Mutex dengan priority inheritance
- Binary semaphore untuk emergency
- Timeout mechanism untuk safety

### ✅ State Machine
- State lift: IDLE, MOVING_UP, MOVING_DOWN
- State pintu: CLOSED, OPENING, OPEN, CLOSING
- Departure dan arrival door cycles

### ✅ Scheduling Analysis
- Elevator SCAN scheduling
- Direction-aware request handling
- Preemptive scheduling demonstration

### ✅ Fault Tolerance
- Emergency stop mechanism
- Timeout pada semua blocking operation
- Manual door override

## Struktur Kode

```
RTOS-P2K/
├── sketch.ino           # Main code dengan semua task implementation
├── diagram.json         # Wokwi circuit diagram
├── libraries.txt        # Daftar library Wokwi
└── README.md            # Dokumentasi project (file ini)
```

## Requirements

- **Platform**: ESP32
- **Framework**: Arduino + FreeRTOS
- **Simulator**: Wokwi (https://wokwi.com)
- **Development Tool**: VSCode dengan PlatformIO atau Wokwi online

## Konsep RTOS yang Dipelajari

1. **Real-Time Scheduling**: Priority-based preemptive scheduling
2. **Concurrency**: Multiple tasks running "simultaneously"
3. **Synchronization**: Mutex, semaphore
4. **Critical Section**: Protected shared resource access
5. **State Machine**: Lift dan door state management
6. **Elevator Algorithm**: SCAN scheduling untuk optimasi perjalanan lift
7. **Priority Inversion**: Penyebab dan solusinya
8. **Deadlock**: Prevention strategies
9. **Race Condition**: Detection dan prevention

## Author

Lift Control System FreeRTOS Simulation  
Educational Project untuk pembelajaran Real-Time Operating System

## License

MIT License - Free to use for educational purposes
