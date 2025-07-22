#include <stdio.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "aht20.h"
#include "bmp280.h"
#include "ssd1306.h"
#include "Webserver.c"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "pico/bootrom.h"
#include "config.h"
#include "pio_matrix.pio.h"

SemaphoreHandle_t xSensorDataMutex, xBuzzerSemaphore;

float temperatura_aht_global = 0.0, temperatura_bmp_global = 0.0, umidade_global = 0.0, pressao_global = 0.0, altitude_global = 0.0;
float historico_temp_global[MAX_READINGS] = {0};
float historico_umid_global[MAX_READINGS] = {0};
float historico_press_global[MAX_READINGS] = {0};
float temp_min_global = 18.0, temp_max_global = 40.0, temp_offset_global = 0.0;
float umid_min_global = 40.0, umid_max_global = 70.0, umid_offset_global = 0.0;
float pres_min_global = 980.0, pres_max_global = 1020.0, pres_offset_global = 0.0;

volatile bool g_alerta_ativo = false;

enum MODE
{
    CONNECTINGMODE,
    NORMALMODE,
    CONFIGMODE,
    INFOMODE
};
volatile enum MODE current_mode = CONNECTINGMODE;
volatile uint32_t last_time_button_A = 0;

double calculate_altitude(double pressure)
{
    return 44330.0 * (1.0 - pow(pressure / SEA_LEVEL_PRESSURE, 0.1903));
}

void vWebServerTask(void *pvParameters)
{
    init_cyw43();
    connect_to_wifi();

    struct tcp_pcb *server = init_tcp_server();
    if (!server)
    {
        printf("Falha ao iniciar servidor TCP\n");
        vTaskDelete(NULL);
    }
    current_mode = CONFIGMODE;
    while (true)
    {
        cyw43_arch_poll();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void vSensorTask(void *pvParameters)
{
    aht20_init(I2C_PORT_SENSORS);
    struct bmp280_calib_param bmp_params;
    bmp280_init(I2C_PORT_SENSORS);
    bmp280_get_calib_params(I2C_PORT_SENSORS, &bmp_params);

    AHT20_Data aht_data;
    int32_t raw_temp, raw_press;

    while (true)
    {
        aht20_read(I2C_PORT_SENSORS, &aht_data);
        bmp280_read_raw(I2C_PORT_SENSORS, &raw_temp, &raw_press);
        int32_t temp_bmp_conv = bmp280_convert_temp(raw_temp, &bmp_params);
        int32_t press_bmp_conv = bmp280_convert_pressure(raw_press, raw_temp, &bmp_params);

        if (xSemaphoreTake(xSensorDataMutex, portMAX_DELAY) == pdTRUE)
        {
            temperatura_aht_global = aht_data.temperature + temp_offset_global;
            umidade_global = aht_data.humidity + umid_offset_global;
            temperatura_bmp_global = (temp_bmp_conv / 100.0) + temp_offset_global;
            pressao_global = press_bmp_conv / 100.0 + pres_offset_global;
            altitude_global = calculate_altitude(pressao_global * 100.0);

            if (temperatura_aht_global < temp_min_global || temperatura_aht_global > temp_max_global ||
                umidade_global < umid_min_global || umidade_global > umid_max_global ||
                pressao_global < pres_min_global || pressao_global > pres_max_global)
            {
                g_alerta_ativo = true;
            }
            else
            {
                g_alerta_ativo = false;
            }

            for (int i = 0; i < MAX_READINGS - 1; i++)
            {
                historico_temp_global[i] = historico_temp_global[i + 1];
                historico_umid_global[i] = historico_umid_global[i + 1];
                historico_press_global[i] = historico_press_global[i + 1];
            }
            historico_temp_global[MAX_READINGS - 1] = temperatura_aht_global;
            historico_umid_global[MAX_READINGS - 1] = umidade_global;
            historico_press_global[MAX_READINGS - 1] = pressao_global;

            xSemaphoreGive(xSensorDataMutex);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
void vMatrixTask()
{
    // Inicializa o PIO e carrega o programa
    PIO pio = pio0;
    uint offset = pio_add_program(pio, &pio_matrix_program);
    uint sm = pio_claim_unused_sm(pio, true);
    pio_matrix_program_init(pio, sm, offset, OUT_PIN);

    bool alerta_local; // Variável para guardar o estado do alerta

    while (true)
    {
        if (xSemaphoreTake(xSensorDataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            alerta_local = g_alerta_ativo;
            xSemaphoreGive(xSensorDataMutex);

            if (alerta_local)
            {
                pio_drawn(ALERT_PATTERN, 0, pio, sm);
            }
            else
            {
                pio_drawn(NORMAL_PATTERN, 0, pio, sm);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // Atualiza a matriz a cada 100ms
    }
}

void vDisplayTask(void *pvParameters)
{
    ssd1306_t ssd;
    ssd1306_init(&ssd, 128, 64, false, DISPLAY_ADDRESS, I2C_PORT_DISP);
    ssd1306_config(&ssd);
    bool cor = true;

    int config_page = 0;
    int config_timer = 0;
    const int PAGE_DISPLAY_TIME = 6;

    while (true)
    {
        ssd1306_fill(&ssd, !cor);
        ssd1306_rect(&ssd, 2, 2, 124, 60, cor, !cor);

        switch (current_mode)
        {
        case NORMALMODE:
        {
            static int normal_page = 0;
            config_timer++;
            if (config_timer >= PAGE_DISPLAY_TIME)
            {
                config_timer = 0;
                normal_page = (normal_page + 1) % 2; // Alterna entre 0 (AHT) e 1 (BMP)
            }
            char l1[24], l2[24], l3[24], l4[24];
            if (xSemaphoreTake(xSensorDataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                if (normal_page == 0)
                {
                    snprintf(l1, sizeof(l1), "AHT T: %.1fC", temperatura_aht_global);
                    snprintf(l2, sizeof(l2), "Umid: %.1f%%", umidade_global);
                    snprintf(l3, sizeof(l3), "Alt: %.0fm", altitude_global);
                    snprintf(l4, sizeof(l4), "Pres: %.0fhPa", pressao_global);
                }
                else
                {
                    snprintf(l1, sizeof(l1), "BMP T: %.1fC", temperatura_bmp_global);
                    snprintf(l2, sizeof(l2), "Umid: %.1f%%", umidade_global);
                    snprintf(l3, sizeof(l3), "Alt: %.0fm", altitude_global);
                    snprintf(l4, sizeof(l4), "Pres: %.0fhPa", pressao_global);
                }
                xSemaphoreGive(xSensorDataMutex);
            }
            ssd1306_draw_string(&ssd, "Estacao Met.", 20, 5);
            ssd1306_line(&ssd, 3, 15, 125, 15, cor);
            ssd1306_draw_string(&ssd, l1, 8, 18);
            ssd1306_draw_string(&ssd, l2, 8, 28);
            ssd1306_draw_string(&ssd, l3, 8, 38);
            ssd1306_draw_string(&ssd, l4, 8, 48);
            break;
        }
        case CONNECTINGMODE:
        {
            ssd1306_draw_string(&ssd, "Conectando...", 15, 20);
            ssd1306_draw_string(&ssd, "Aguarde", 35, 35);
            break;
        }
        case INFOMODE:
        {
            config_timer++;
            if (config_timer >= PAGE_DISPLAY_TIME)
            {
                config_timer = 0;
                config_page = (config_page + 1) % 3; // Alterna entre 0, 1 e 2
            }

            char l_min[24], l_max[24], l_offset[24];

            ssd1306_draw_string(&ssd, "Configuracoes", 15, 5);
            ssd1306_line(&ssd, 3, 15, 125, 15, cor);

            if (xSemaphoreTake(xSensorDataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                switch (config_page)
                {
                case 0: // Página de Temperatura
                    ssd1306_draw_string(&ssd, "Temperatura", 20, 20);
                    snprintf(l_min, sizeof(l_min), "Min: %.1f C", temp_min_global);
                    snprintf(l_max, sizeof(l_max), "Max: %.1f C", temp_max_global);
                    snprintf(l_offset, sizeof(l_offset), "Offset: %.1f", temp_offset_global);
                    break;
                case 1: // Página de Umidade
                    ssd1306_draw_string(&ssd, "Umidade", 35, 20);
                    snprintf(l_min, sizeof(l_min), "Min: %.0f %%", umid_min_global);
                    snprintf(l_max, sizeof(l_max), "Max: %.0f %%", umid_max_global);
                    snprintf(l_offset, sizeof(l_offset), "Offset: %.1f", umid_offset_global);
                    break;
                case 2: // Página de Pressão
                    ssd1306_draw_string(&ssd, "Pressao", 35, 20);
                    snprintf(l_min, sizeof(l_min), "Min: %.0f hPa", pres_min_global);
                    snprintf(l_max, sizeof(l_max), "Max: %.0f hPa", pres_max_global);
                    snprintf(l_offset, sizeof(l_offset), "Offset: %.1f", pres_offset_global);
                    break;
                }
                xSemaphoreGive(xSensorDataMutex);
            }

            ssd1306_draw_string(&ssd, l_min, 10, 32);
            ssd1306_draw_string(&ssd, l_max, 10, 42);
            ssd1306_draw_string(&ssd, l_offset, 10, 52);
            break;
        }
        case CONFIGMODE:
        {
            char l2[24], l3[24], l4[24];
            int32_t rssi = 0;
            cyw43_wifi_get_rssi(&cyw43_state, &rssi);
            snprintf(l2, sizeof(l2), ipaddr_ntoa(netif_ip4_addr(netif_default)));
            snprintf(l3, sizeof(l3), "Sinal: %d dBm", rssi);
            snprintf(l4, sizeof(l4), "Rede: %s", WIFI_SSID);
            ssd1306_draw_string(&ssd, "Status Wi-Fi", 25, 5);
            ssd1306_line(&ssd, 3, 15, 125, 15, cor);
            ssd1306_draw_string(&ssd, "IP:", 8, 18);
            ssd1306_draw_string(&ssd, l2, 8, 28);
            ssd1306_draw_string(&ssd, l3, 8, 40);
            ssd1306_draw_string(&ssd, l4, 8, 52);
            break;
        }
        }
        ssd1306_send_data(&ssd);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void vButtonBuzzerTask(void *pvParameters)
{
    // Usa o pino A
    pwm_init_buzzer(BUZZER_PIN_A);

    while (true)
    {
        // Espera pelo semáforo (liberado pelo botão)
        if (xSemaphoreTake(xBuzzerSemaphore, portMAX_DELAY) == pdTRUE)
        {
            // Toca um beep curto no pino A
            beep(BUZZER_PIN_A, 100);
        }
    }
}

void vAlertBuzzerTask(void *pvParameters)
{
    pwm_init_buzzer(BUZZER_PIN_B);
    bool alerta_local;

    while (true)
    {
        // Verifica o estado de alerta de forma segura usando o mutex
        if (xSemaphoreTake(xSensorDataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            alerta_local = g_alerta_ativo;
            xSemaphoreGive(xSensorDataMutex);
        }

        if (alerta_local)
        {
            beep(BUZZER_PIN_B, 200);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
void vLedTask(void *pvParameters)
{
    gpio_init(LED_GREEN);
    gpio_init(LED_RED);
    gpio_init(LED_BLUE);
    gpio_set_dir(LED_GREEN, GPIO_OUT);
    gpio_set_dir(LED_RED, GPIO_OUT);
    gpio_set_dir(LED_BLUE, GPIO_OUT);

    while (true)
    {
        if (current_mode == CONNECTINGMODE)
        {
            gpio_put(LED_BLUE, true); // Azul para conectando
            gpio_put(LED_GREEN, false);
            gpio_put(LED_RED, false);
        }
        else if (current_mode == NORMALMODE)
        {
            gpio_put(LED_BLUE, false);
            gpio_put(LED_GREEN, true); // Verde para normal
            gpio_put(LED_RED, false);
        }
        else if (current_mode == CONFIGMODE)
        {
            gpio_put(LED_BLUE, false);
            gpio_put(LED_GREEN, true); // Amarelo (Vermelho + Verde) para config
            gpio_put(LED_RED, true);
        }
        else
        { // INFOMODE
            gpio_put(LED_BLUE, false);
            gpio_put(LED_GREEN, false);
            gpio_put(LED_RED, true); // Vermelho para alerta
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Delay pequeno para resposta rápida
    }
}

void gpio_irq_handler(uint gpio, uint32_t events)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (gpio == BUTTON_A && (now - last_time_button_A > DEBOUNCE_TIME))
    {
        last_time_button_A = now;
        if (current_mode != CONNECTINGMODE)
        {
            if (current_mode == CONFIGMODE)
            {
                current_mode = NORMALMODE;
            }
            else if (current_mode == NORMALMODE)
            {
                current_mode = INFOMODE;
            }
            else
            {
                current_mode = CONFIGMODE;
            }
        }
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(xBuzzerSemaphore, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
    if (gpio == BUTTON_B)
    {
        reset_usb_boot(0, 0);
    }
}

int main()
{
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
    xBuzzerSemaphore = xSemaphoreCreateBinary();

    xTaskCreate(vSensorTask, "SensorTask", 512, NULL, 1, NULL);
    xTaskCreate(vDisplayTask, "DisplayTask", 512, NULL, 1, NULL);
    xTaskCreate(vWebServerTask, "WebServerTask", 1024, NULL, 2, NULL);
    xTaskCreate(vButtonBuzzerTask, "ButtonBuzzerTask", 256, NULL, 1, NULL);
    xTaskCreate(vAlertBuzzerTask, "AlertBuzzerTask", 256, NULL, 1, NULL);
    xTaskCreate(vLedTask, "LedTask", 256, NULL, 1, NULL);
    xTaskCreate(vMatrixTask, "MatrixTask", 256, NULL, 1, NULL);

    vTaskStartScheduler();

    while (true)
    {
    };
}