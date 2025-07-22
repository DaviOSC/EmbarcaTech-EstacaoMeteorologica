#include "Webserver.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "html.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h> 

#define MAX_READINGS 20
#define JSON_BUFFER_SIZE 2048


extern SemaphoreHandle_t xSensorDataMutex;
extern float temperatura_aht_global, temperatura_bmp_global, umidade_global, pressao_global, altitude_global;
extern float historico_temp_global[], historico_umid_global[], historico_press_global[];
extern float temp_min_global, temp_max_global, umid_min_global, umid_max_global, pres_min_global, pres_max_global;
extern float temp_offset_global, umid_offset_global, pres_offset_global;

// --- Protótipos de Funções Locais ---
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
void user_request(char *request_ptr, struct tcp_pcb *tpcb);
static void handle_root(struct tcp_pcb *tpcb);
static void handle_data(struct tcp_pcb *tpcb);
static void handle_config(struct tcp_pcb *tpcb, char *json_body);
static int parse_json_value(const char *json, const char *key, float *value);

// --- Inicialização da Wi-Fi ---
void init_cyw43()
{
    if (cyw43_arch_init())
    {
        printf("Falha ao inicializar o Wi-Fi\n");
    }
}

void connect_to_wifi()
{
    cyw43_arch_enable_sta_mode();
    printf("Conectando a %s...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000))
    {
        printf("Falha ao conectar.\n");
    }
    else
    {
        printf("Conectado! IP: %s\n", ipaddr_ntoa(netif_ip4_addr(netif_default)));
    }
}

// --- Inicialização do Servidor TCP ---
struct tcp_pcb *init_tcp_server()
{
    struct tcp_pcb *server = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!server)
        return NULL;

    if (tcp_bind(server, IP_ADDR_ANY, 80) != ERR_OK)
    {
        tcp_close(server);
        return NULL;
    }

    server = tcp_listen(server);
    tcp_accept(server, tcp_server_accept);
    printf("Servidor TCP iniciado na porta 80\n");
    return server;
}

// Função auxiliar para enviar a página HTML principal
static void handle_root(struct tcp_pcb *tpcb)
{
    char http_header[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";
    tcp_write(tpcb, http_header, sizeof(http_header) - 1, TCP_WRITE_FLAG_COPY);

    tcp_write(tpcb, HTML_BODY2, strlen(HTML_BODY2), TCP_WRITE_FLAG_COPY);

}

// Função para montar e enviar o JSON com os dados dos sensores
static void handle_data(struct tcp_pcb *tpcb)
{
    char json_buffer[JSON_BUFFER_SIZE];
    char temp_buffer[512]; // Buffer para os arrays do histórico

    if (xSemaphoreTake(xSensorDataMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        // Formata os arrays do histórico
        char hist_temp_str[256] = "";
        char hist_umid_str[256] = "";
        char hist_pres_str[256] = "";

        for (int i = 0; i < MAX_READINGS; i++)
        {
            sprintf(temp_buffer, "%.1f%s", historico_temp_global[i], (i < MAX_READINGS - 1) ? "," : "");
            strcat(hist_temp_str, temp_buffer);
            sprintf(temp_buffer, "%.1f%s", historico_umid_global[i], (i < MAX_READINGS - 1) ? "," : "");
            strcat(hist_umid_str, temp_buffer);
            sprintf(temp_buffer, "%.1f%s", historico_press_global[i], (i < MAX_READINGS - 1) ? "," : "");
            strcat(hist_pres_str, temp_buffer);
        }

        // Monta o JSON completo
        snprintf(json_buffer, JSON_BUFFER_SIZE,
                 "{\"temperatura_aht\":%.1f,\"umidade\":%.1f,\"temperatura_bmp\":%.1f,\"pressao\":%.1f,\"altitude\":%.0f,"
                 "\"historico\":{\"temperatura\":[%s],\"umidade\":[%s],\"pressao\":[%s]},"
                 "\"config\":{\"limites\":{\"temp_min\":%.1f,\"temp_max\":%.1f,\"umid_min\":%.1f,\"umid_max\":%.1f,\"pres_min\":%.1f,\"pres_max\":%.1f},"
                 "\"offsets\":{\"temp_offset\":%.1f,\"umid_offset\":%.1f,\"pres_offset\":%.1f}}}", // <-- Mude "temp" para "temp_offset", etc.
                 temperatura_aht_global, umidade_global, temperatura_bmp_global, pressao_global, altitude_global,
                 hist_temp_str, hist_umid_str, hist_pres_str,
                 temp_min_global, temp_max_global, umid_min_global, umid_max_global, pres_min_global, pres_max_global,
                 temp_offset_global, umid_offset_global, pres_offset_global);

        xSemaphoreGive(xSensorDataMutex);
    }
    else
    {
        // Caso não consiga o mutex, envia um JSON de erro
        snprintf(json_buffer, JSON_BUFFER_SIZE, "{\"error\":\"Nao foi possivel acessar os dados dos sensores.\"}");
    }

    char http_header[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n";
    tcp_write(tpcb, http_header, sizeof(http_header) - 1, TCP_WRITE_FLAG_COPY);
    tcp_write(tpcb, json_buffer, strlen(json_buffer), TCP_WRITE_FLAG_COPY);
}

// Função para analisar o corpo JSON e atualizar as configurações
static void handle_config(struct tcp_pcb *tpcb, char *json_body)
{

    float t_min, t_max, h_min, h_max, p_min, p_max;
    float t_off, h_off, p_off;

    printf("\n--- INICIO DO JSON RECEBIDO PELO SERVIDOR ---\n");
    printf("%s", json_body);
    printf("\n--- FIM DO JSON RECEBIDO PELO SERVIDOR ---\n\n");

    // Busca os objetos limites e offsets
    char *limites = strstr(json_body, "\"limites\"");
    char *offsets = strstr(json_body, "\"offsets\"");

    int success = 1;
    if (limites) {
        success &= parse_json_value(limites, "temp_min", &t_min);
        success &= parse_json_value(limites, "temp_max", &t_max);
        success &= parse_json_value(limites, "umid_min", &h_min);
        success &= parse_json_value(limites, "umid_max", &h_max);
        success &= parse_json_value(limites, "pres_min", &p_min);
        success &= parse_json_value(limites, "pres_max", &p_max);
    } else {
        success = 0;
    }
    if (offsets) {
        success &= parse_json_value(offsets, "temp_offset", &t_off);
        success &= parse_json_value(offsets, "umid_offset", &h_off);
        success &= parse_json_value(offsets, "pres_offset", &p_off);
    } else {
        success = 0;
    }

    if (success && xSemaphoreTake(xSensorDataMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        if (t_min >= t_max || h_min >= h_max || p_min >= p_max) {
                printf("Erro: Minimo deve ser menor que Maximo!\n");
                char http_response[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nMinimo deve ser menor que Maximo!";
                tcp_write(tpcb, http_response, sizeof(http_response) - 1, TCP_WRITE_FLAG_COPY);
                xSemaphoreGive(xSensorDataMutex);
                return;
            }
        
        temp_min_global = t_min;
        temp_max_global = t_max;
        umid_min_global = h_min;
        umid_max_global = h_max;
        pres_min_global = p_min;
        pres_max_global = p_max;
        temp_offset_global = t_off;
        umid_offset_global = h_off;
        pres_offset_global = p_off;

        xSemaphoreGive(xSensorDataMutex);

        printf("\n--- NOVAS CONFIGURACOES SALVAS ---\n");
        printf("Limites de Alerta:\n");
        printf("  Temperatura: Min=%.1f C, Max=%.1f C\n", temp_min_global, temp_max_global);
        printf("  Umidade:     Min=%.1f %%, Max=%.1f %%\n", umid_min_global, umid_max_global);
        printf("  Pressao:     Min=%.1f hPa, Max=%.1f hPa\n", pres_min_global, pres_max_global);
        printf("Calibracao (Offsets):\n");
        printf("  Temperatura: %.1f C\n", temp_offset_global);
        printf("  Umidade:     %.1f %%\n", umid_offset_global);
        printf("  Pressao:     %.1f hPa\n", pres_offset_global);
        printf("----------------------------------\n\n");

        char http_response[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
        tcp_write(tpcb, http_response, sizeof(http_response) - 1, TCP_WRITE_FLAG_COPY);
    }
    else
    {
        printf("Erro ao salvar configuracoes: falha na analise do JSON ou timeout do mutex.\n");
        char http_response[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        tcp_write(tpcb, http_response, sizeof(http_response) - 1, TCP_WRITE_FLAG_COPY);
    }
}

// Roteador de requisições
void user_request(char *request_ptr, struct tcp_pcb *tpcb)
{
    if (strncmp(request_ptr, "GET /data", 9) == 0)
    {
        handle_data(tpcb);
    }
    else if (strncmp(request_ptr, "GET / ", 6) == 0)
    {
        handle_root(tpcb);
    }
    else if (strncmp(request_ptr, "POST /config", 12) == 0)
    {
        // Encontra o início do corpo JSON
        char *body = strstr(request_ptr, "\r\n\r\n");
        if (body)
        {
            handle_config(tpcb, body + 4);
        }
    }
    else
    {
        // Rota não encontrada
        char http_response[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        tcp_write(tpcb, http_response, sizeof(http_response) - 1, TCP_WRITE_FLAG_COPY);
    }
}

// Callback de aceitação de conexão do lwIP
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    if (err != ERR_OK || newpcb == NULL)
    {
        return ERR_VAL;
    }

    tcp_setprio(newpcb, TCP_PRIO_NORMAL);

    tcp_recv(newpcb, tcp_server_recv);
    return ERR_OK;
}

// Callback de recebimento de dados do lwIP
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if (p == NULL)
    {
        tcp_close(tpcb);
        return ERR_OK;
    }

    if (p->tot_len > 0)
    {
        char request_buffer[REQUEST_BUFFER_SIZE] = {0};
        pbuf_copy_partial(p, request_buffer, p->tot_len, 0);

        user_request(request_buffer, tpcb);

        tcp_recved(tpcb, p->tot_len);
    }

    pbuf_free(p);

    tcp_close(tpcb);

    return ERR_OK;
}

static int parse_json_value(const char *json, const char *key, float *value)
{
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    char *key_ptr = strstr(json, search_key);
    if (key_ptr)
    {
        char *value_ptr = key_ptr + strlen(search_key);

        while (*value_ptr == ' ' || *value_ptr == '\t')
        {
            value_ptr++;
        }
        if (*value_ptr != ':')
        {
            return 0;
        }
        value_ptr++; 

        while (*value_ptr == ' ' || *value_ptr == '\t')
        {
            value_ptr++;
        }

        *value = atof(value_ptr);
        return 1;
    }
    return 0; 
}