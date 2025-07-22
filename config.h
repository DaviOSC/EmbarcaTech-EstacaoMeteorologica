#define LED_PIN_RED 13
#define LED_PIN_GREEN 11
#define LED_PIN_BLUE 12
#define BUZZER_PIN_A 21
#define BUZZER_PIN_B 10
#define OUT_PIN 7
#define BUTTON_A 5
#define BUTTON_B 6

#define LED_GREEN 11
#define LED_RED 13
#define LED_BLUE 12

#define I2C_PORT_SENSORS i2c0
#define I2C_SDA_SENSORS 0
#define I2C_SCL_SENSORS 1

#define I2C_PORT_DISP i2c1
#define I2C_SDA_DISP 14
#define I2C_SCL_DISP 15
#define DISPLAY_ADDRESS 0x3C

#define DEBOUNCE_TIME 300
#define NUM_PIXELS 25
#define SEA_LEVEL_PRESSURE 101325.0
#define BUZZER_FREQUENCY 4000/// Frequência do buzzer em Hz

// Definições dos padrões de cores para a Matriz de LEDs
double NORMAL_PATTERN[NUM_PIXELS][3] = {
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}
};

double ALERT_PATTERN[NUM_PIXELS][3] = {
    {0, 0, 0}, {0, 0, 0}, {1, 1, 0}, {0, 0, 0}, {0, 0, 0},
    {0, 0, 0}, {1, 1, 0}, {1, 0, 0}, {1, 1, 0}, {0, 0, 0},
    {1, 1, 0}, {1, 1, 0}, {1, 0, 0}, {1, 1, 0}, {1, 1, 0},
    {1, 1, 0}, {1, 1, 0}, {1, 1, 0}, {1, 1, 0}, {1, 1, 0},
    {1, 1, 0}, {1, 1, 0}, {1, 0, 0}, {1, 1, 0}, {1, 1, 0}
};

// Função para inicializar o buzzer
void pwm_init_buzzer(uint pin) {
    // Configurar o pino como saída de PWM
    gpio_set_function(pin, GPIO_FUNC_PWM);

    // Obter o slice do PWM associado ao pino
    uint slice_num = pwm_gpio_to_slice_num(pin);

    // Configurar o PWM com frequência desejada
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, clock_get_hz(clk_sys) / (BUZZER_FREQUENCY * 4096)); // Divisor de clock
    pwm_init(slice_num, &config, true);

    // Iniciar o PWM no nível baixo
    pwm_set_gpio_level(pin, 0);
}

// Função para emitir um beep com duração especificada
void beep(uint pin, uint duration_ms) {
    // Obter o slice do PWM associado ao pino
    uint slice_num = pwm_gpio_to_slice_num(pin);

    // Configurar o duty cycle para 50% (ativo)
    pwm_set_gpio_level(pin, 2048);

    // Temporização
    sleep_ms(duration_ms);

    // Desativar o sinal PWM (duty cycle 0)
    pwm_set_gpio_level(pin, 0);
}

uint32_t matrix_rgb(double r, double g, double b)
{
    unsigned char R, G, B;
    R = r * 255;
    G = g * 255;
    B = b * 255;
    return (G << 24) | (R << 16) | (B << 8);
}

// rotina para acionar a matrix de leds
void pio_drawn(double desenho[][3], uint32_t valor_led, PIO pio, uint sm)
{
    for (int16_t i = NUM_PIXELS - 1; i >= 0; i--)
    {
        double r = desenho[i][0] * 0.1; // Reduz a intensidade da cor
        double g = desenho[i][1] * 0.1;
        double b = desenho[i][2] * 0.1;

        valor_led = matrix_rgb(r, g, b);
        pio_sm_put_blocking(pio, sm, valor_led);
    }
}

// Código html do webserver
const char HTML_BODY[] =
    "<!DOCTYPE html><html lang=\"pt-br\"><head><meta charset=\"UTF-8\"><title>Estação</title>"
    "<script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script><style>"
    "body{font-family:sans-serif;background:#f0f4f8;margin:0;padding:10px}h1,h2,h3{color:#1e3a5f;text-align:center}"
    ".mf{display:flex;flex-wrap:wrap;gap:20px;justify-content:center;align-items:flex-start}"
    ".lc{display:flex;flex-direction:column;gap:20px;flex:1 1 340px;min-width:340px;max-width:420px}"
    ".cd{background:#fff;padding:15px;margin:0;border:1px solid #eee}"
    ".cd.ch{flex:2 1 600px;min-width:320px;max-width:700px}"
    "h2{margin-top:0;margin-bottom:15px;color:#0056b3}.dg{display:grid;grid-template-columns:1fr 1fr;text-align:center;gap:10px}"
    ".dp p{margin:0;font-size:1em;color:#555}.dp .v{font-size:1.8em;font-weight:bold;color:#000}"
    ".ct{width:100%;border-collapse:collapse;margin-top:10px}.ct th,.ct td{padding:8px;border:1px solid #ddd;text-align:center}"
    "input{font-size:1em;padding:5px;width:90px;text-align:center}button{font-size:1.1em;padding:10px 25px;border:none;background:#007bff;color:white;cursor:pointer;width:100%;margin-top:20px}"
    ".cc{position:relative;height:200px;width:98%;margin:auto}"
    "@media (max-width:900px){.mf{flex-direction:column;gap:10px}.lc{max-width:100%}.cd.ch{max-width:100%}}"
    "</style></head>"
    "<body><h1>Estação Meteorológica</h1>"
    "<div class=\"mf\">"
    "<div class=\"lc\">"
    "<div class=\"cd\"><h2>Atuais</h2><div class=\"dg\">"
    "<div class=\"dp\"><p>Temp. AHT</p><span class=\"v\" id=\"t_aht\">?.?</span>&deg;C</div>"
    "<div class=\"dp\"><p>Umidade</p><span class=\"v\" id=\"h\">?.?</span>%</div>"
    "<div class=\"dp\"><p>Temp. BMP</p><span class=\"v\" id=\"t_bmp\">?.?</span>&deg;C</div>"
    "<div class=\"dp\"><p>Altitude</p><span class=\"v\" id=\"alt\">?</span>m</div>"
    "<div class=\"dp\" style=\"grid-column:1/-1\"><p>Pressão</p><span class=\"v\" id=\"p\">?.?</span> hPa</div>"
    "</div></div>"
    "<div class=\"cd\"><h2>Ajustes</h2><form onsubmit=\"s(event)\">"
    "<h3>Alertas</h3><table class=\"ct\">"
    "<tr><th>Parâm.</th><th>Mín.</th><th>Máx.</th></tr>"
    "<tr><td>Temp (°C)</td><td><input type=\"number\" id=\"i_t_min\"></td><td><input type=\"number\" id=\"i_t_max\"></td></tr>"
    "<tr><td>Umid (%)</td><td><input type=\"number\" id=\"i_h_min\"></td><td><input type=\"number\" id=\"i_h_max\"></td></tr>"
    "<tr><td>Press (hPa)</td><td><input type=\"number\" id=\"i_p_min\"></td><td><input type=\"number\" id=\"i_p_max\"></td></tr>"
    "</table><h3 style=\"margin-top:20px\">Calibrar</h3><table class=\"ct\">"
    "<tr><th>Parâm.</th><th>Offset</th></tr>"
    "<tr><td>Temp.</td><td><input type=\"number\" id=\"i_t_off\"></td></tr>"
    "<tr><td>Umid.</td><td><input type=\"number\" id=\"i_h_off\"></td></tr>"
    "<tr><td>Press.</td><td><input type=\"number\" id=\"i_p_off\"></td></tr>"
    "</table><button type=\"submit\">Salvar</button></form></div>"
    "</div>"
    "<div class=\"cd ch\"><h2>Gráficos</h2>"
    "<h3>Temperatura AHT</h3><div class=\"cc\"><canvas id=\"g_t\"></canvas></div>"
    "<h3>Umidade</h3><div class=\"cc\"><canvas id=\"g_h\"></canvas></div>"
    "<h3>Pressão</h3><div class=\"cc\"><canvas id=\"g_p\"></canvas></div>"
    "</div>"
    "</div>"
    "<script>"
    "const E=(id)=>document.getElementById(id);let g={};function c(i,l,o){const t=E(i).getContext('2d');return new Chart(t,{type:'line',data:{datasets:[{label:l,data:[],borderColor:o}]},options:{responsive:!0,maintainAspectRatio:!1}})}"
    "function p(d){if(d.config){const l=d.config.limites;const o=d.config.offsets;E('i_t_min').value=l.temp_min;E('i_t_max').value=l.temp_max;E('i_h_min').value=l.umid_min;E('i_h_max').value=l.umid_max;E('i_p_min').value=l.pres_min;E('i_p_max').value=l.pres_max;E('i_t_off').value=o.temp_offset;E('i_h_off').value=o.umid_offset;E('i_p_off').value=o.pres_offset;}}"
    "function u(d){E('t_aht').innerText=d.temperatura_aht.toFixed(1);E('h').innerText=d.umidade.toFixed(1);E('t_bmp').innerText=d.temperatura_bmp.toFixed(1);E('p').innerText=d.pressao.toFixed(1);E('alt').innerText=d.altitude.toFixed(0);if(d.config){const l=d.config.limites;E('t_aht').style.color=d.temperatura_aht<l.temp_min||d.temperatura_aht>l.temp_max?'red':'#000';E('t_bmp').style.color=d.temperatura_bmp<l.temp_min||d.temperatura_bmp>l.temp_max?'red':'#000';E('h').style.color=d.umidade<l.umid_min||d.umidade>l.umid_max?'red':'#000';E('p').style.color=d.pressao<l.pres_min||d.pressao>l.pres_max?'red':'#000';}if(d.historico){const h=d.historico;const l=Array.from({length:h.temperatura.length},(_,i)=>i+1);g.t.data.labels=l;g.t.data.datasets[0].data=h.temperatura;g.t.update('none');g.h.data.labels=l;g.h.data.datasets[0].data=h.umidade;g.h.update('none');g.p.data.labels=l;g.p.data.datasets[0].data=h.pressao;g.p.update('none')}}"
    "function f(){fetch('/data').then(r=>r.json()).then(d=>u(d))}"
    "function i(){fetch('/data').then(r=>r.json()).then(d=>{p(d);u(d)})}"
    "function s(e){e.preventDefault();const d={limites:{temp_min:Number(E('i_t_min').value),temp_max:Number(E('i_t_max').value),umid_min:Number(E('i_h_min').value),umid_max:Number(E('i_h_max').value),pres_min:Number(E('i_p_min').value),pres_max:Number(E('i_p_max').value)},offsets:{temp_offset:Number(E('i_t_off').value),umid_offset:Number(E('i_h_off').value),pres_offset:Number(E('i_p_off').value)}};fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)}).then(async r=>{const msg=await r.text();alert(r.ok?'Salvo':'Erro: '+msg);});}"
    "document.addEventListener('DOMContentLoaded',()=>{g.t=c('g_t','Temp','#FF6384');g.h=c('g_h','Umid','#36A2EB');g.p=c('g_p','Press','#FFCE56');i();setInterval(f,5000)})"
    "</script></body></html>";

