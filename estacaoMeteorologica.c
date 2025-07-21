#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/i2c.h"
#include "aht20.h"
#include "bmp280.h"
#include "ssd1306.h"
#include "Webserver.c" // Inclui o nosso servidor
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "pico/bootrom.h"
#include <math.h>

// --- Definições de Hardware ---
#define I2C_PORT_SENSORS i2c0
#define I2C_SDA_SENSORS 0
#define I2C_SCL_SENSORS 1

#define I2C_PORT_DISP i2c1
#define I2C_SDA_DISP 14
#define I2C_SCL_DISP 15
#define DISPLAY_ADDRESS 0x3C

#define BUTTON_A 5
#define BUTTON_B 6
#define DEBOUNCE_TIME 300

#define SEA_LEVEL_PRESSURE 101325.0

// --- Variáveis Globais para Dados Compartilhados ---
SemaphoreHandle_t xSensorDataMutex;
// Adicionadas as variáveis que faltavam
float temperatura_aht_global = 0.0, temperatura_bmp_global = 0.0, umidade_global = 0.0, pressao_global = 0.0, altitude_global = 0.0;
uint historico_temp_global[MAX_READINGS] = {0};
float temp_min_global = 18.0, temp_max_global = 30.0, temp_offset_global = 0.0;

enum MODE { NORMALMODE, CONFIGMODE };
volatile enum MODE current_mode = NORMALMODE;
volatile uint32_t last_time_button_A = 0;

// --- Função para Calcular Altitude ---
double calculate_altitude(double pressure) {
    return 44330.0 * (1.0 - pow(pressure / SEA_LEVEL_PRESSURE, 0.1903));
}

// --- Tarefa para o Servidor Web ---
void vWebServerTask(void *pvParameters) {
    init_cyw43();
    connect_to_wifi();
    struct tcp_pcb *server = init_tcp_server();
    if (!server) {
        printf("Falha ao iniciar servidor TCP\n");
        vTaskDelete(NULL);
    }
    while(true) {
        cyw43_arch_poll();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- Tarefa para Leitura dos Sensores ---
void vSensorTask(void *pvParameters) {
    aht20_init(I2C_PORT_SENSORS);
    struct bmp280_calib_param bmp_params;
    bmp280_init(I2C_PORT_SENSORS);
    bmp280_get_calib_params(I2C_PORT_SENSORS, &bmp_params);
    
    AHT20_Data aht_data;
    int32_t raw_temp, raw_press;

    while (true) {
        aht20_read(I2C_PORT_SENSORS, &aht_data);
        bmp280_read_raw(I2C_PORT_SENSORS, &raw_temp, &raw_press);
        int32_t temp_bmp_conv = bmp280_convert_temp(raw_temp, &bmp_params);
        int32_t press_bmp_conv = bmp280_convert_pressure(raw_press, raw_temp, &bmp_params);

        if (xSemaphoreTake(xSensorDataMutex, portMAX_DELAY) == pdTRUE) {
            temperatura_aht_global = aht_data.temperature;
            umidade_global = aht_data.humidity;
            temperatura_bmp_global = temp_bmp_conv / 100.0;
            pressao_global = press_bmp_conv; // Mantém em Pascals
            altitude_global = calculate_altitude(pressao_global);

            for (int i = 0; i < MAX_READINGS - 1; i++) {
                historico_temp_global[i] = historico_temp_global[i + 1];
            }
            historico_temp_global[MAX_READINGS - 1] = (uint)temperatura_aht_global;
            
            xSemaphoreGive(xSensorDataMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// --- Tarefa para o Display OLED (MODIFICADA) ---
void vDisplayTask(void *pvParameters) {
    ssd1306_t ssd;
    ssd1306_init(&ssd, 128, 64, false, DISPLAY_ADDRESS, I2C_PORT_DISP);
    ssd1306_config(&ssd);

    bool cor = true;

    while(true) {
        ssd1306_fill(&ssd, !cor); // Limpa o display com a cor invertida

        if (current_mode == NORMALMODE) {
            char linha1[24], linha2[24], linha3[24], linha4[24], linha5[24];
            
            if (xSemaphoreTake(xSensorDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // Formata todas as 5 strings com os dados globais
                snprintf(linha1, sizeof(linha1), "AHT: %.1fC", temperatura_aht_global);
                snprintf(linha2, sizeof(linha2), "Umid: %.1f%%", umidade_global);
                snprintf(linha3, sizeof(linha3), "BMP: %.1fC", temperatura_bmp_global);
                snprintf(linha4, sizeof(linha4), "Pres: %.0fhPa", pressao_global / 100.0); // Converte Pa para hPa
                snprintf(linha5, sizeof(linha5), "Alt: %.0fm", altitude_global);
                xSemaphoreGive(xSensorDataMutex);
            }

            // Desenha a interface no display
            ssd1306_rect(&ssd, 2, 2, 124, 60, cor, !cor);
            ssd1306_draw_string(&ssd, "Estacao Met.", 20, 5);
            ssd1306_line(&ssd, 3, 15, 125, 15, cor);
            
            // Exibe os 5 dados
            ssd1306_draw_string(&ssd, linha1, 8, 18);
            ssd1306_draw_string(&ssd, linha2, 8, 28);
            ssd1306_draw_string(&ssd, linha3, 8, 38);
            ssd1306_draw_string(&ssd, linha4, 8, 48);
            // Coloca a altitude no lado direito para caber
            ssd1306_draw_string(&ssd, linha5, 70, 48);


        } else { // CONFIGMODE
            char linha2[24], linha3[24], linha4[24];
            int32_t rssi = 0;
            cyw43_wifi_get_rssi(&cyw43_state, &rssi);

            snprintf(linha2, sizeof(linha2), ipaddr_ntoa(netif_ip4_addr(netif_default)));
            snprintf(linha3, sizeof(linha3), "Sinal: %d dBm", rssi);
            snprintf(linha4, sizeof(linha4), "Rede: %s", WIFI_SSID);

            ssd1306_rect(&ssd, 2, 2, 124, 60, cor, !cor);
            ssd1306_draw_string(&ssd, "Status Wi-Fi", 25, 5);
            ssd1306_line(&ssd, 3, 15, 125, 15, cor);
            ssd1306_draw_string(&ssd, "IP:", 8, 18);
            ssd1306_draw_string(&ssd, linha2, 8, 28);
            ssd1306_draw_string(&ssd, linha3, 8, 40);
            ssd1306_draw_string(&ssd, linha4, 8, 52);
        }
        
        ssd1306_send_data(&ssd);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}


// --- Tratamento de Interrupção para Botões ---
void gpio_irq_handler(uint gpio, uint32_t events) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (gpio == BUTTON_A && (now - last_time_button_A > DEBOUNCE_TIME)) {
        last_time_button_A = now;
        current_mode = (current_mode == NORMALMODE) ? CONFIGMODE : NORMALMODE;
    }
    if (gpio == BUTTON_B) {
        reset_usb_boot(0, 0);
    }
}

// --- Função Principal (main) ---
int main() {
    stdio_init_all();

    i2c_init(I2C_PORT_SENSORS, 100 * 1000);
    gpio_set_function(I2C_SDA_SENSORS, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_SENSORS, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_SENSORS);
    gpio_pull_up(I2C_SCL_SENSORS);

    i2c_init(I2C_PORT_DISP, 400 * 1000);
    gpio_set_function(I2C_SDA_DISP, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_DISP, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_DISP);
    gpio_pull_up(I2C_SCL_DISP);

    gpio_init(BUTTON_A);
    gpio_set_dir(BUTTON_A, GPIO_IN);
    gpio_pull_up(BUTTON_A);
    gpio_init(BUTTON_B);
    gpio_set_dir(BUTTON_B, GPIO_IN);
    gpio_pull_up(BUTTON_B);
    gpio_set_irq_enabled_with_callback(BUTTON_A, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler);
    gpio_set_irq_enabled(BUTTON_B, GPIO_IRQ_EDGE_FALL, true);

    xSensorDataMutex = xSemaphoreCreateMutex();

    xTaskCreate(vSensorTask, "SensorTask", 512, NULL, 1, NULL);
    xTaskCreate(vDisplayTask, "DisplayTask", 512, NULL, 1, NULL);
    xTaskCreate(vWebServerTask, "WebServerTask", 1024, NULL, 2, NULL);

    vTaskStartScheduler();

    while (true) {};
}