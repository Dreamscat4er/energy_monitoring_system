#include <queue.h>
#include <FreeRTOS.h>
#include <timers.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <MCMVoltSense.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>

// =======================
// OLED Configuration
// =======================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
#define ROTATION 0
#define ALT_SDA 7
#define ALT_SCL 15

Adafruit_SSD1306 display2(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, OLED_RESET);

// =======================
// DHT Sensor Configuration
// =======================
#define DHTPIN 5
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// =======================
// Current Sensor Configuration
// =======================
#define ACTectionRange 20 // Set Non-invasive AC Current Sensor detection range (5A,10A,20A)
#define VREF 3.3
#define ACPin 4

// =======================
// Voltage Sensor Configuration
// =======================
MCMmeter meter;

// =======================
// General Configuration
// =======================
#define RELAY_PIN 18

#if CONFIG_FREERTOS_UNICORE
static const BaseType_t app_cpu = 0;
#else
static const BaseType_t app_cpu = 1;
#endif

// =======================
// FreeRTOS Handles and Mutex
// =======================
TaskHandle_t readTemperatureTaskHandle = NULL;
TaskHandle_t readCurrentTaskHandle = NULL;
TaskHandle_t readVoltageTaskHandle = NULL;
SemaphoreHandle_t buffMutex;

// =======================
// Data Structures and Queue
// =======================
typedef enum {
  TEMP,
  AC_CURR,
  AC_VOLT,
  HUM,
} DataType;

typedef struct {
  DataType type;
  float value;
} QueueData;

QueueData buff[4] = {
  {TEMP, 0},
  {AC_CURR, 0},
  {AC_VOLT, 0},
  {HUM, 0}
};

QueueHandle_t queue;

// =======================
// Helper Functions
// =======================
float readACCurrentValue() {
  float peakVoltage = 0; // Vrms
  for (int i = 0; i < 5; i++) {
    peakVoltage += analogRead(ACPin);// Read peak voltage
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
  peakVoltage /= 5;
  float voltageVirtualValue = peakVoltage * 0.707; // Convert the peak voltage to the Virtual Value of voltage
  /* The circuit is amplified by 2 times, so it is divided by 2. */
  voltageVirtualValue = (voltageVirtualValue / 1024 * VREF) / 2;
  return voltageVirtualValue * ACTectionRange;
}

// =======================
// Task Implementations
// =======================
void readCurrent(void* parameters) {
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    float ac = readACCurrentValue();
    QueueData acData = {AC_CURR, ac};

    if (!isnan(ac)) {
      if (xQueueSend(queue, &acData, portMAX_DELAY) != pdPASS) {
        Serial.println("Failed to send current to queue");
      } else {
        Serial.println("Current value sent to queue");
      }
    } else {
      Serial.println("Failed to read current");
    }

    vTaskDelay(1500 / portTICK_PERIOD_MS);
    xTaskNotifyGive(readVoltageTaskHandle);
  }
}

void readTemperature(void* parameters) {
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    float temperature = dht.readTemperature();
    float hum = dht.readHumidity();

    QueueData tempData = {TEMP, temperature};
    QueueData humData = {HUM, hum};

    if (!isnan(temperature)) {
      if (xQueueSend(queue, &tempData, portMAX_DELAY) != pdPASS) {
        Serial.println("Failed to send temperature to queue");
      } else {
        Serial.println("Temperature value sent to queue");
      }
    } else {
      Serial.println("Failed to read temperature");
    }

    if (!isnan(hum)) {
      if (xQueueSend(queue, &humData, portMAX_DELAY) != pdPASS) {
        Serial.println("Failed to send humidity to queue");
      } else {
        Serial.println("Humidity value sent to queue");
      }
    } else {
      Serial.println("Failed to read humidity");
    }

    xTaskNotifyGive(readCurrentTaskHandle);
    vTaskDelay(1500 / portTICK_PERIOD_MS);
  }
}

void readVoltage(void* parameters) {
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    meter.analogVoltage(40, 2000);
    float voltage = meter.Vrms;
    QueueData vData = {AC_VOLT, voltage};

    if (!isnan(voltage)) {
      if (xQueueSend(queue, &vData, portMAX_DELAY) != pdPASS) {
        Serial.println("Failed to send voltage to queue");
      } else {
        Serial.println("Voltage value sent to queue");
      }
    } else {
      Serial.println("Failed to read voltage");
    }

    xTaskNotifyGive(readTemperatureTaskHandle);
    vTaskDelay(1500 / portTICK_PERIOD_MS);
  }
}

void displayValues(void* parameters) {
  while (1) {
    if (xSemaphoreTake(buffMutex, portMAX_DELAY)) {
      display2.clearDisplay();
      display2.setCursor(0, 0);
      display2.print(F("Current: "));
      display2.print(buff[AC_CURR].value);
      display2.print(F(" A"));

      display2.setCursor(0, 20);
      display2.print(F("Temp: "));
      display2.print(buff[TEMP].value);
      display2.print(F(" C"));

      display2.setCursor(0, 30);
      display2.print(F("Hum: "));
      display2.print(buff[HUM].value);
      display2.print(F(" %"));

      display2.setCursor(0, 50);
      display2.print(F("Voltage: "));
      display2.print(buff[AC_VOLT].value);
      display2.print(F(" V"));

      display2.display();
      xSemaphoreGive(buffMutex);
    }
    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

// void relayTask(void* parameters) {
  while (1) {
    digitalWrite(RELAY_PIN, (buff[HUM].value > 50) ? LOW : HIGH);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void queueHandle(void* parameters) {
  QueueData receivedItem;
  while (1) {
    if (xQueueReceive(queue, &receivedItem, portMAX_DELAY) == pdPASS) {
      if (xSemaphoreTake(buffMutex, portMAX_DELAY)) {
        buff[receivedItem.type] = receivedItem;
        Serial.print("Data updated: ");
        Serial.print(receivedItem.type);
        Serial.print(" -> ");
        Serial.println(receivedItem.value);
        xSemaphoreGive(buffMutex);
      }
    }
  }
}

// =======================
// Setup and Loop
// =======================
void setup() {
  Wire1.begin(ALT_SDA, ALT_SCL);
  Serial.begin(115200);
  queue = xQueueCreate(4, sizeof(QueueData));
  if (queue == NULL) {
    Serial.println("Error creating the queue");
    while (1);
  }

  pinMode(RELAY_PIN, OUTPUT);
  buffMutex = xSemaphoreCreateMutex();
  if (buffMutex == NULL) {
    Serial.println("Failed to create buffMutex");
    while (1);
  }

  dht.begin();// Setting up hum/temp sensor
  meter.VoltageStp(A1, 105, 1.7);// Setting up AC voltage sensor
  if (!display2.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("Display2 SSD1306 allocation failed"));
    while (1);
  }
  display2.clearDisplay();
  display2.setTextSize(1);
  display2.setTextColor(WHITE);
  display2.setCursor(0, 0);
  display2.setRotation(ROTATION);

  // Create tasks
  xTaskCreate(readCurrent, "ReadCurrent", 2048, NULL, 1, &readCurrentTaskHandle);
  xTaskCreate(readTemperature, "ReadTemperature", 2048, NULL, 1, &readTemperatureTaskHandle);
  xTaskCreate(readVoltage, "ReadVoltage", 2048, NULL, 1, &readVoltageTaskHandle);
  xTaskCreate(queueHandle, "QueueHandle", 2048, NULL, 1, NULL);
  xTaskCreate(displayValues, "DisplayValues", 2048, NULL, 1, NULL);
  xTaskCreate(relayTask, "RelayTask", 2048, NULL, 2, NULL);

  // Start the task sequence
  xTaskNotifyGive(readTemperatureTaskHandle);
  digitalWrite(RELAY_PIN, HIGH);
  Serial.println("Setup complete");
}

void loop() {
 
}