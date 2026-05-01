#define EIDSP_QUANTIZE_FILTERBANK   0

#include <porfavorfunciona2xd_inferencing.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

const char* ssid = "TU_NOMBRE_DE_RED_WIFI";
const char* password = "TU_CONTRASEÑA_WIFI";
const String botToken = "TU_TOKEN_DE_BOTFATHER";
const String chatId = "TU_CHAT_ID";

unsigned long lastAlertTime = 0;
const unsigned long alertCooldown = 10000;

typedef struct {
    int16_t *buffer;
    uint8_t buf_ready;
    uint32_t buf_count;
    uint32_t n_samples;
} inference_t;

static inference_t inference;
static const uint32_t sample_buffer_size = 2048;
static signed short sampleBuffer[sample_buffer_size];
static bool debug_nn = false; 
static bool record_status = true;

#define WINDOW_SIZE 3
static float history_buffer[WINDOW_SIZE] = {0.0, 0.0, 0.0};
static int history_index = 0;
static int readings_count = 0;

static bool microphone_inference_start(uint32_t n_samples);
static bool microphone_inference_record(void);
static int microphone_audio_signal_get_data(size_t offset, size_t length, float *out_ptr);
static void microphone_inference_end(void);
static int i2s_init(uint32_t sampling_rate);
static int i2s_deinit(void);

void enviarAlertaTelegram() {
    if (WiFi.status() != WL_CONNECTED) {
        ei_printf("🔄 WiFi desconectado. Intentando reconectar...\n");
        WiFi.disconnect();
        WiFi.begin(ssid, password);
        
        int intentos = 0;
        while (WiFi.status() != WL_CONNECTED && intentos < 10) {
            delay(500);
            Serial.print(".");
            intentos++;
        }
        Serial.println();
    }

    if (WiFi.status() == WL_CONNECTED) {
        WiFiClientSecure client;
        client.setInsecure(); 

        HTTPClient http;
        String url = "https://api.telegram.org/bot" + botToken + "/sendMessage?chat_id=" + chatId + "&text=🚨 ¡ALERTA! BugBeats ha detectado un chillido.";
        
        http.begin(client, url); 
        int httpResponseCode = http.GET();
        
        if (httpResponseCode == 200) {
            ei_printf("✅ ¡Mensaje entregado en Telegram! Código: %d\n", httpResponseCode);
        } else {
            ei_printf("❌ Error de comunicación con Telegram. Código HTTP: %d\n", httpResponseCode);
        }
        http.end();
    } else {
        ei_printf("❌ Error crítico: Imposible reconectar al WiFi. Alerta no enviada.\n");
    }
}

void setup()
{
    Serial.begin(115200);
    while (!Serial);

    Serial.println("\n--- Iniciando Sistema BugBeats ---");
    Serial.print("Conectando a WiFi: ");
    Serial.println(ssid);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    int intentos = 0;
    while (WiFi.status() != WL_CONNECTED && intentos < 20) {
        delay(500);
        Serial.print(".");
        intentos++;
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("✅ WiFi conectado exitosamente. IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("❌ Advertencia: No se pudo conectar al WiFi. El bot offline temporalmente.");
    }

    Serial.println("Edge Impulse Inferencing Demo - VERSION LACCEI (PROMEDIO 3s)");

    ei_printf("Inferencing settings:\n");
    ei_printf("\tInterval: ");
    ei_printf_float((float)EI_CLASSIFIER_INTERVAL_MS);
    ei_printf(" ms.\n");
    ei_printf("\tFrame size: %d\n", EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);
    ei_printf("\tSample length: %d ms.\n", EI_CLASSIFIER_RAW_SAMPLE_COUNT / 16);
    ei_printf("\tNo. of classes: %d\n", sizeof(ei_classifier_inferencing_categories) / sizeof(ei_classifier_inferencing_categories[0]));

    ei_printf("\nStarting continious inference in 2 seconds...\n");
    ei_sleep(2000);

    if (microphone_inference_start(EI_CLASSIFIER_RAW_SAMPLE_COUNT) == false) {
        ei_printf("ERR: Could not allocate audio buffer\r\n");
        return;
    }
    ei_printf("Recording...\n");
}

void loop()
{
    bool m = microphone_inference_record();
    if (!m) {
        ei_printf("ERR: Failed to record audio...\n");
        return;
    }

    signal_t signal;
    signal.total_length = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    signal.get_data = &microphone_audio_signal_get_data;
    ei_impulse_result_t result = { 0 };

    EI_IMPULSE_ERROR r = run_classifier(&signal, &result, debug_nn);
    if (r != EI_IMPULSE_OK) {
        ei_printf("ERR: Failed to run classifier (%d)\n", r);
        return;
    }

    float score_rata = result.classification[1].value;
    
    history_buffer[history_index] = score_rata;
    history_index = (history_index + 1) % WINDOW_SIZE;
    if (readings_count < WINDOW_SIZE) {
        readings_count++;
    }

    float suma = 0;
    for (int i = 0; i < WINDOW_SIZE; i++) {
        suma += history_buffer[i];
    }
    float promedio_rata = suma / WINDOW_SIZE;

    ei_printf("\n--- Evaluacion del Entorno (Ventana 3s) ---\n");
    ei_printf("Instantaneo: %.4f | PROMEDIO: %.4f\n", score_rata, promedio_rata);

    if (readings_count == WINDOW_SIZE) {
        if (promedio_rata > 0.65) { 
            ei_printf("✅ Ambiente Normal\n");
        } else {
            ei_printf("🚨 ¡ALERTA! CHILLIDO CONFIRMADO (3s) 🚨\n");
            
            if (millis() - lastAlertTime > alertCooldown) {
                enviarAlertaTelegram();
                lastAlertTime = millis();
            } else {
                ei_printf("⏳ Alerta silenciada temporalmente (Anti-Spam)...\n");
            }
        }
    } else {
        ei_printf("⏳ Recopilando primeros 3 segundos...\n");
    }
}

static void audio_inference_callback(uint32_t n_bytes)
{
    for(int i = 0; i < n_bytes>>1; i++) {
        inference.buffer[inference.buf_count++] = sampleBuffer[i];

        if(inference.buf_count >= inference.n_samples) {
          inference.buf_count = 0;
          inference.buf_ready = 1;
        }
    }
}

static void capture_samples(void* arg) {
  const int32_t i2s_bytes_to_read = (uint32_t)arg;
  size_t bytes_read = 0;
  
  int32_t* tempBuffer32 = (int32_t*)malloc(i2s_bytes_to_read * 2);

  while (record_status) {
    i2s_read((i2s_port_t)1, (void*)tempBuffer32, i2s_bytes_to_read * 2, &bytes_read, portMAX_DELAY);

    if (bytes_read <= 0) {
      ei_printf("Error in I2S read : %d", bytes_read);
    } 
    else {
        int samples_read = bytes_read / 4; 

        for (int x = 0; x < samples_read; x++) {
            int16_t sample16 = tempBuffer32[x] >> 14;
            
            int ganancia = 5; 
            int32_t audio_amplificado = sample16 * ganancia;
            
            if (audio_amplificado > 32767) audio_amplificado = 32767;
            if (audio_amplificado < -32768) audio_amplificado = -32768;
            
            sampleBuffer[x] = (int16_t)audio_amplificado;
        }

        if (record_status) {
            audio_inference_callback(samples_read * 2);
        } else {
            break;
        }
    }
  }
  free(tempBuffer32);
  vTaskDelete(NULL);
}

static bool microphone_inference_start(uint32_t n_samples)
{
    inference.buffer = (int16_t *)malloc(n_samples * sizeof(int16_t));
    if(inference.buffer == NULL) {
        return false;
    }
    inference.buf_count  = 0;
    inference.n_samples  = n_samples;
    inference.buf_ready  = 0;

    if (i2s_init(EI_CLASSIFIER_FREQUENCY)) {
        ei_printf("Failed to start I2S!");
    }

    ei_sleep(100);
    record_status = true;
    xTaskCreate(capture_samples, "CaptureSamples", 1024 * 32, (void*)sample_buffer_size, 10, NULL);
    return true;
}

static bool microphone_inference_record(void)
{
    bool ret = true;
    while (inference.buf_ready == 0) {
        delay(10);
    }
    inference.buf_ready = 0;
    return ret;
}

static int microphone_audio_signal_get_data(size_t offset, size_t length, float *out_ptr)
{
    numpy::int16_to_float(&inference.buffer[offset], out_ptr, length);
    return 0;
}

static void microphone_inference_end(void)
{
    i2s_deinit();
    ei_free(inference.buffer);
}

static int i2s_init(uint32_t sampling_rate) {
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = sampling_rate,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, 
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  
      .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, 
      .dma_buf_count = 32,
      .dma_buf_len = 1024,                        
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0,                            
  };
  
  i2s_pin_config_t pin_config = {
      .bck_io_num = D1,    
      .ws_io_num = D2,     
      .data_out_num = -1,  
      .data_in_num = D3,   
  };
  
  esp_err_t ret = 0;

  ret = i2s_driver_install((i2s_port_t)1, &i2s_config, 0, NULL);
  if (ret != ESP_OK) {
    ei_printf("Error in i2s_driver_install");
  }

  ret = i2s_set_pin((i2s_port_t)1, &pin_config);
  if (ret != ESP_OK) {
    ei_printf("Error in i2s_set_pin");
  }

  ret = i2s_zero_dma_buffer((i2s_port_t)1);
  if (ret != ESP_OK) {
    ei_printf("Error in initializing dma buffer with 0");
  }

  return int(ret);
}

static int i2s_deinit(void) {
    i2s_driver_uninstall((i2s_port_t)1);
    return 0;
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_MICROPHONE
#error "Invalid model for current sensor."
#endif