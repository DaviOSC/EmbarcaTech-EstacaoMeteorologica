#include "Webserver.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"


#define MAX_READINGS 20

// --- Variáveis Externas (Definidas no seu estacaoMeteorologica.c) ---
extern SemaphoreHandle_t xSensorDataMutex;
// Adicionadas as variáveis que faltavam
extern float temperatura_aht_global, temperatura_bmp_global, umidade_global, pressao_global, altitude_global;
extern uint historico_temp_global[];
extern float temp_min_global, temp_max_global, temp_offset_global;
// --- Fim das Variáveis Externas ---


void init_cyw43() {
    if (cyw43_arch_init()) {
        printf("Falha ao inicializar o Wi-Fi\n");
    }
}

void connect_to_wifi() {
    cyw43_arch_enable_sta_mode();
    printf("Conectando a %s...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("Falha ao conectar.\n");
    } else {
        printf("Conectado! IP: %s\n", ipaddr_ntoa(netif_ip4_addr(netif_default)));
    }
}

struct tcp_pcb* init_tcp_server() {
    struct tcp_pcb *server = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!server) return NULL;
    if (tcp_bind(server, IP_ADDR_ANY, 80) != ERR_OK) {
        tcp_close(server);
        return NULL;
    }
    server = tcp_listen(server);
    tcp_accept(server, tcp_server_accept);
    printf("Servidor TCP iniciado na porta 80\n");
    return server;
}

// --- Funções de Callback do Servidor TCP ---

static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }
    tcp_recv(newpcb, tcp_server_recv);
    return ERR_OK;
}

static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (!p) {
        tcp_close(tpcb);
        return ERR_OK;
    }

    char *request = (char *)p->payload;

    // Lógica de Roteamento
    if (strstr(request, "GET /data")) {
        // --- Requisição para DADOS (JSON) ---
        char json[1024];
        char json_history[512];
        char *ptr = json_history;
        
        if (xSemaphoreTake(xSensorDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            ptr += sprintf(ptr, "[");
            for (int i = 0; i < MAX_READINGS; i++) {
                ptr += sprintf(ptr, "%u%s", historico_temp_global[i], (i < MAX_READINGS - 1) ? "," : "");
            }
            ptr += sprintf(ptr, "]");
            
            // JSON MODIFICADO para incluir todos os dados
            snprintf(json, sizeof(json), 
                "{\"temperatura_aht\":%.1f,\"umidade\":%.1f,\"temperatura_bmp\":%.1f,\"pressao\":%.1f,\"altitude\":%.0f,"
                "\"historico\":{\"temperatura\":%s},"
                "\"config\":{\"limites\":{\"temp_min\":%.1f,\"temp_max\":%.1f},\"offsets\":{\"temp\":%.1f}}}",
                temperatura_aht_global, umidade_global, temperatura_bmp_global, pressao_global / 100.0, altitude_global,
                json_history, temp_min_global, temp_max_global, temp_offset_global
            );
            xSemaphoreGive(xSensorDataMutex);
        }
        
        char response[1500];
        snprintf(response, sizeof(response),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
                 strlen(json), json);
        tcp_write(tpcb, response, strlen(response), TCP_WRITE_FLAG_COPY);

    } else if (strstr(request, "POST /config")) {
        const char *ok_response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
        tcp_write(tpcb, ok_response, strlen(ok_response), TCP_WRITE_FLAG_COPY);
        
    } else {
        // --- Requisição para a PÁGINA PRINCIPAL (HTML) ---
        static char html[4096];
        
        if (xSemaphoreTake(xSensorDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // HTML MODIFICADO para exibir todos os dados
            snprintf(html, sizeof(html),
                "<!DOCTYPE html><html lang=\"pt-br\"><head><meta charset=\"UTF-8\"><title>Estacao</title>"
                "<script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script><style>"
                "body{font-family:sans-serif;text-align:center;padding:10px}p{font-size:1.2em;margin:8px 0}span{font-weight:bold}form>div{margin-bottom:10px}"
                "label{display:inline-block;width:90px;text-align:right}input{width:80px;padding:5px}.chart-container{position:relative;height:180px;width:95%%;margin:auto}</style></head>"
                "<body><h1>Estacao Meteorologica</h1>"
                "<p>Temp AHT: <span id=\"t_aht\">%.1f</span>&deg;C | Umid: <span id=\"h\">%.1f</span> %%%%</p>" // Linha 1
                "<p>Temp BMP: <span id=\"t_bmp\">%.1f</span>&deg;C</p>" // Linha 2
                "<p>Press: <span id=\"p\">%.1f</span> hPa | Alt: <span id=\"alt\">%.0f</span> m</p>" // Linha 3
                "<div class=\"chart-container\"><canvas id=\"g\"></canvas></div><hr><form onsubmit=\"s(event)\"><h2>Config.</h2>"
                "<div><label>Min:</label><input id=\"t_min\" value=\"%.1f\"></div><div><label>Max:</label><input id=\"t_max\" value=\"%.1f\"></div>"
                "<div><label>Offset:</label><input id=\"t_off\" value=\"%.1f\"></div><button type=\"submit\">Salvar</button></form>"
                // JAVASCRIPT MODIFICADO para atualizar todos os campos
                "<script>let c;function u(){fetch('/data').then(r=>r.json()).then(d=>{document.getElementById('t_aht').innerText=d.temperatura_aht.toFixed(1);document.getElementById('h').innerText=d.umidade.toFixed(1);document.getElementById('t_bmp').innerText=d.temperatura_bmp.toFixed(1);document.getElementById('p').innerText=d.pressao.toFixed(1);document.getElementById('alt').innerText=d.altitude.toFixed(0);if(c&&d.historico&&d.historico.temperatura){c.data.labels=Array.from({length:d.historico.temperatura.length},(_,i)=>i+1);c.data.datasets[0].data=d.historico.temperatura;c.update('none')}if(d.config){document.getElementById('t_min').value=d.config.limites.temp_min;document.getElementById('t_max').value=d.config.limites.temp_max;document.getElementById('t_off').value=d.config.offsets.temp}})}function s(e){e.preventDefault();const d={limites:{temp_min:Number(document.getElementById('t_min').value),temp_max:Number(document.getElementById('t_max').value)},offsets:{temp:Number(document.getElementById('t_off').value)}};fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)}).then(r=>alert(r.ok?'Salvo':'Erro'))}document.addEventListener('DOMContentLoaded',()=>{const t=document.getElementById('g').getContext('2d');c=new Chart(t,{type:'line',data:{datasets:[{label:'Temp AHT',data:[],borderColor:'red'}]},options:{responsive:true,maintainAspectRatio:false}});u();setInterval(u,5000)})</script>"
                "</body></html>",
                temperatura_aht_global, umidade_global, temperatura_bmp_global, pressao_global / 100.0, altitude_global,
                temp_min_global, temp_max_global, temp_offset_global
            );
            xSemaphoreGive(xSensorDataMutex);
        }
        
        char response[4500];
        snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n%s", strlen(html), html);
        tcp_write(tpcb, response, strlen(response), TCP_WRITE_FLAG_COPY);
    }

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    tcp_output(tpcb);
    return ERR_OK;
}
