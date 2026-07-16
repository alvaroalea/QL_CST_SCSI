/* =====================================================================
 * SCSI-1 bitbang initiator, dirigido por interrupción en el pin REQ.
 *
 * Idea central:
 *   - La ISR solo hace UN paso del handshake por flanco de REQ.
 *     No bloquea nunca (nada de "while" dentro de la ISR).
 *   - El main loop arma el "contexto de fase" (buffer, longitud,
 *     dirección) y luego espera a que la ISR marque phase_done.
 *   - Antes de cada fase, el main loop LEE C/D, I/O, MSG para saber
 *     qué fase ha puesto el target — nunca se asume la secuencia,
 *     porque el target puede saltar directo a STATUS si hay error.
 * ===================================================================== */

#include <stdint.h>
#include <stdbool.h>

/* ---- Definición de pines (activo-alto lógico; invierte en las
 *      macros si tu bus es activo-bajo físicamente) -------------------- */
#define PIN_REQ   2      // debe ser pin con capacidad de interrupción externa
#define PIN_ACK   3
#define PIN_CD    4
#define PIN_IO    5
#define PIN_MSG   6
#define PIN_BSY   7
#define PIN_SEL   8
#define DATA_PORT PORTD  // puerto de 8 bits para DB0-DB7 (+ paridad si aplica)
#define DATA_DDR  DDRD

#define DIR_IN  0
#define DIR_OUT 1

typedef enum {
    PH_COMMAND, PH_DATA_IN, PH_DATA_OUT, PH_STATUS, PH_MSG_IN
} phase_t;

/* =====================================================================
 * TIMEOUTS calculados a partir de la hoja de specs del RODIME RO652A:
 *   track-to-track 18ms | seek avg 85ms | seek MAX 180ms
 *   latencia rotacional avg 10.9ms (2746 RPM) | buffer FIFO 4KB
 *
 * Dos escalas de tiempo distintas por diseño:
 *   - "primer byte" de una fase DATA: aquí es donde el drive hace el
 *     seek físico + espera de latencia + posible retry interno tras
 *     un error de lectura (ECC). Discos de esta época pueden tardar
 *     varios segundos en un retry interno silencioso.
 *   - "bytes siguientes" una vez ya hay transferencia en curso: el
 *     disco ya está sirviendo desde el buffer FIFO a 0.937 MB/s, así
 *     que el cuello de botella pasa a ser TU propio bitbang, no el
 *     disco. Aquí el timeout solo debe pillar un fallo real de bus.
 * ===================================================================== */
#define TIMEOUT_SELECT_MS        250    // standard SCSI-1 selection timeout

#define TIMEOUT_CMD_FIRST_MS     500    // COMMAND: sin mecánica de por medio,
                                          // margen por si el drive está
                                          // terminando otra operación
#define TIMEOUT_CMD_BYTE_MS       50

#define TIMEOUT_DATA_FIRST_MS   4000    // seek MAX 180ms + latencia + margen
                                          // amplio para retries internos de
                                          // ECC/recalibración (drive viejo)
#define TIMEOUT_DATA_BYTE_MS      20    // ya en régimen de transferencia FIFO

#define TIMEOUT_STATUS_MS        500
#define TIMEOUT_MSGIN_MS         500

#define TIMEOUT_SPINUP_MS      20000    // espera de arranque de plato
                                          // (sin dato oficial: margen
                                          // generoso, ~15-20s típico en
                                          // Winchester de esta época)

#define MAX_RETRIES                 3

/* ---- Estado compartido con la ISR (todo volatile) --------------------- */
static volatile uint8_t  *xfer_buf;
static volatile uint16_t  xfer_len;
static volatile uint16_t  xfer_idx;
static volatile uint8_t   xfer_dir;
static volatile bool      phase_done;
static volatile uint32_t  last_activity_ms;   // actualizado por la ISR en
                                                // cada flanco de REQ; el
                                                // main loop lo usa para
                                                // detectar bus colgado

/* =====================================================================
 * ISR: se dispara en AMBOS flancos de REQ (CHANGE).
 * Un ciclo de byte = flanco de subida (REQ asserted) + flanco de
 * bajada (REQ deasserted). Esto reproduce exactamente el diagrama de
 * la sección 2, pero repartido en dos disparos de interrupción en
 * vez de dos "while" bloqueantes.
 * ===================================================================== */
void ISR_scsi_req_change(void) {
    last_activity_ms = millis();     // marca de tiempo para el watchdog

    if (digitalRead(PIN_REQ) == HIGH) {
        /* --- Flanco de subida: target listo para transferir un byte --- */
        if (xfer_dir == DIR_IN) {
            xfer_buf[xfer_idx] = DATA_PORT_READ();      // captura byte
        } else {
            DATA_DDR  = 0xFF;                            // bus como salida
            DATA_PORT_WRITE(xfer_buf[xfer_idx]);         // coloca byte
        }
        digitalWrite(PIN_ACK, HIGH);                     // ACK_assert()

    } else {
        /* --- Flanco de bajada: target ya latcheó / retiró el dato --- */
        if (xfer_dir == DIR_OUT) {
            DATA_DDR = 0x00;                              // suelta el bus
        }
        digitalWrite(PIN_ACK, LOW);                       // ACK_deassert()

        xfer_idx++;
        if (xfer_idx >= xfer_len) {
            phase_done = true;      // el main loop lo detecta y avanza
        }
    }
}

typedef enum { RP_OK, RP_TIMEOUT_SEEK, RP_TIMEOUT_BUS } run_phase_result_t;

/* =====================================================================
 * Arma el contexto de una fase y bloquea hasta que la ISR complete
 * todos los bytes, o hasta que expire el timeout correspondiente.
 *
 * timeout_first_ms: aplica SOLO hasta que llega el primer REQ.
 *                    Es el hueco donde el disco hace seek+latencia
 *                    (o, en COMMAND/STATUS/MSG, sencillamente no hay
 *                    mecánica de por medio pero puede estar terminando
 *                    la operación anterior).
 * timeout_byte_ms:   aplica entre bytes sucesivos UNA VEZ que la
 *                     transferencia ya empezó. Debe ser corto: si el
 *                     disco se detiene aquí es un fallo real de bus,
 *                     no un seek legítimo.
 * ===================================================================== */
run_phase_result_t run_phase(uint8_t dir, uint8_t *buf, uint16_t len,
                              uint32_t timeout_first_ms,
                              uint32_t timeout_byte_ms) {
    noInterrupts();
    xfer_dir   = dir;
    xfer_buf   = buf;
    xfer_len   = len;
    xfer_idx   = 0;
    phase_done = (len == 0);
    last_activity_ms = millis();
    interrupts();

    while (!phase_done) {
        uint32_t limit = (xfer_idx == 0) ? timeout_first_ms : timeout_byte_ms;
        if (millis() - last_activity_ms > limit) {
            return (xfer_idx == 0) ? RP_TIMEOUT_SEEK : RP_TIMEOUT_BUS;
        }
        sleep_cpu();   // se despierta con cada interrupción o con el
                        // watchdog de timer si tu MCU lo soporta
    }
    return RP_OK;
}

/* =====================================================================
 * Detecta en qué fase ha puesto el target el bus, leyendo C/D, I/O, MSG.
 * Se llama SIEMPRE antes de decidir qué buffer/longitud usar.
 * ===================================================================== */
phase_t detect_phase(void) {
    bool cd  = digitalRead(PIN_CD);
    bool io  = digitalRead(PIN_IO);
    bool msg = digitalRead(PIN_MSG);

    if (!msg &&  cd && !io) return PH_COMMAND;
    if (!msg && !cd &&  io) return PH_DATA_IN;
    if (!msg && !cd && !io) return PH_DATA_OUT;
    if (!msg &&  cd &&  io) return PH_STATUS;
    /*  msg &&  cd &&  io */ return PH_MSG_IN;
}

/* =====================================================================
 * SELECTION (sin ATN, sin arbitraje: único iniciador)
 * ===================================================================== */
bool scsi_select(uint8_t target_id) {
    DATA_DDR = 0xFF;
    DATA_PORT_WRITE((1 << MY_INITIATOR_ID) | (1 << target_id));
    digitalWrite(PIN_SEL, HIGH);
    delayMicroseconds(2);            // bus settle delay

    uint32_t t0 = millis();
    while (digitalRead(PIN_BSY) == LOW) {
        if (millis() - t0 > TIMEOUT_SELECT_MS) {
            digitalWrite(PIN_SEL, LOW);
            DATA_DDR = 0x00;
            return false;
        }
    }
    digitalWrite(PIN_SEL, LOW);
    DATA_DDR = 0x00;                 // suelta el bus de datos
    return true;
}

/* =====================================================================
 * Ejecuta un comando SCSI completo: SELECT -> [fases hasta MSG_IN]
 * data_buf/data_len solo se usan si el comando implica fase de datos.
 * ===================================================================== */
typedef enum { EXEC_OK, EXEC_TIMEOUT, EXEC_NO_SELECT } exec_result_t;

exec_result_t scsi_exec(uint8_t target_id, uint8_t *cdb, uint8_t cdb_len,
                         uint8_t *data_buf, uint16_t data_len, uint8_t data_dir,
                         uint8_t *status_out) {

    if (!scsi_select(target_id)) return EXEC_NO_SELECT;

    bool done = false;
    while (!done) {
        phase_t ph = detect_phase();
        run_phase_result_t r = RP_OK;

        switch (ph) {
            case PH_COMMAND:
                r = run_phase(DIR_OUT, cdb, cdb_len,
                               TIMEOUT_CMD_FIRST_MS, TIMEOUT_CMD_BYTE_MS);
                break;

            case PH_DATA_IN:
                r = run_phase(DIR_IN, data_buf, data_len,
                               TIMEOUT_DATA_FIRST_MS, TIMEOUT_DATA_BYTE_MS);
                break;

            case PH_DATA_OUT:
                r = run_phase(DIR_OUT, data_buf, data_len,
                               TIMEOUT_DATA_FIRST_MS, TIMEOUT_DATA_BYTE_MS);
                break;

            case PH_STATUS:
                r = run_phase(DIR_IN, status_out, 1,
                               TIMEOUT_STATUS_MS, TIMEOUT_STATUS_MS);
                break;

            case PH_MSG_IN: {
                uint8_t msg;
                r = run_phase(DIR_IN, &msg, 1,
                               TIMEOUT_MSGIN_MS, TIMEOUT_MSGIN_MS);
                done = true;   // tras MSG_IN, target suelta BSY
                break;
            }
        }

        if (r != RP_OK) {
            scsi_bus_reset();          // ver más abajo: pulso en RST
            return EXEC_TIMEOUT;
        }
    }

    uint32_t t0 = millis();
    while (digitalRead(PIN_BSY) == HIGH) {
        if (millis() - t0 > TIMEOUT_STATUS_MS) {
            scsi_bus_reset();
            return EXEC_TIMEOUT;       // BSY nunca se soltó: bus atascado
        }
    }
    return EXEC_OK;
}

/* =====================================================================
 * Recuperación de bus: pulso en RST cuando algo se ha colgado a media
 * transferencia. Tras esto, el PRÓXIMO comando que se mande generará
 * un UNIT ATTENTION que hay que limpiar con REQUEST SENSE.
 * ===================================================================== */
void scsi_bus_reset(void) {
    pinMode(PIN_RST, OUTPUT);
    digitalWrite(PIN_RST, HIGH);   // asserted (lógica activa-alto abstraída)
    delayMicroseconds(30);         // SCSI-1 exige mínimo 25us
    digitalWrite(PIN_RST, LOW);
    delay(250);                    // deja que el drive termine su reset interno
}

/* =====================================================================
 * REQUEST SENSE (0x03) — formato "sense" de 4 bytes suficiente para
 * SCSI-1 (no hace falta el extended sense de 18 bytes de SCSI-2 salvo
 * que el firmware del RO652 lo requiera; comprobar en la práctica y
 * pedir más bytes si el campo "additional sense length" lo indica).
 * ===================================================================== */
typedef struct {
    uint8_t error_code;     // bit7 = valid, bits6-0 = 0x00/0x70 típicamente
    uint8_t segment;
    uint8_t sense_key;      // bits3-0 = sense key (bits7-4 pueden traer flags)
    uint8_t info[4];        // información adicional / LBA del error
} sense_data_t;

#define SK_NO_SENSE        0x00
#define SK_RECOVERED       0x01
#define SK_NOT_READY       0x02
#define SK_MEDIUM_ERROR    0x03
#define SK_HARDWARE_ERROR  0x04
#define SK_ILLEGAL_REQUEST 0x05
#define SK_UNIT_ATTENTION  0x06

bool scsi_request_sense(sense_data_t *out) {
    uint8_t cdb[6] = { 0x03, 0x00, 0x00, 0x00, sizeof(sense_data_t), 0x00 };
    uint8_t status;
    exec_result_t r = scsi_exec(TARGET_ID, cdb, 6,
                                 (uint8_t*)out, sizeof(sense_data_t),
                                 DIR_IN, &status);
    return (r == EXEC_OK);
}

/* =====================================================================
 * Decide qué hacer tras un CHECK CONDITION. Devuelve true si el
 * llamador debe reintentar el comando original, false si debe abortar.
 * ===================================================================== */
bool scsi_recover_error(uint8_t retry_count) {
    sense_data_t sense;

    if (!scsi_request_sense(&sense)) {
        return false;               // ni siquiera REQUEST SENSE respondió
    }

    switch (sense.sense_key & 0x0F) {

        case SK_UNIT_ATTENTION:
            // Normal tras power-on / bus reset. Ya se limpió al leer el
            // sense; el comando original se puede reintentar sin más.
            return true;

        case SK_NOT_READY:
            // El plato puede seguir en spin-up o haciendo recalibración.
            // Esperar y dejar que el llamador reintente.
            delay(500);
            return (retry_count < MAX_RETRIES);

        case SK_RECOVERED:
            // El drive corrigió el error internamente (ECC). No hace
            // falta reintentar: el dato ya es válido si vino con STATUS
            // GOOD... pero si llegamos aquí fue CHECK CONDITION, así
            // que igualmente vale la pena reintentar una vez.
            return (retry_count < 1);

        case SK_MEDIUM_ERROR:
            // Sector defectuoso / error de lectura irrecuperable.
            // Reintentar hasta MAX_RETRIES (a veces un segundo intento
            // tras recalibración sí funciona en discos viejos), luego
            // reportar fallo definitivo de ese sector al llamador.
            return (retry_count < MAX_RETRIES);

        case SK_HARDWARE_ERROR:
            // Fallo de electrónica/mecánica: no tiene sentido reintentar
            // indefinidamente. Un solo reintento por si fue transitorio.
            return (retry_count < 1);

        case SK_ILLEGAL_REQUEST:
            // Error nuestro (CDB mal formado, LBA fuera de rango).
            // Reintentar no sirve de nada: es un bug del firmware.
            return false;

        default:
            return (retry_count < MAX_RETRIES);
    }
}

/* =====================================================================
 * Espera a que el disco esté listo tras power-on (spin-up). Se hace
 * con TEST UNIT READY en un bucle con backoff, no con un timeout fijo
 * corto: en un RO652 el plato puede tardar bastantes segundos en
 * alcanzar las 2746 RPM nominales.
 * ===================================================================== */
bool scsi_wait_ready(void) {
    uint8_t cdb[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };  // TEST UNIT READY
    uint32_t t0 = millis();

    while (millis() - t0 < TIMEOUT_SPINUP_MS) {
        uint8_t status;
        exec_result_t r = scsi_exec(TARGET_ID, cdb, 6, NULL, 0, DIR_IN, &status);

        if (r == EXEC_OK && status == 0x00) return true;   // GOOD: listo

        if (r == EXEC_OK && status == 0x02) {               // CHECK_CONDITION
            sense_data_t sense;
            scsi_request_sense(&sense);
            // SK_NOT_READY esperado mientras el plato sube de velocidad;
            // cualquier otra cosa aquí ya es sospechoso.
            if ((sense.sense_key & 0x0F) != SK_NOT_READY) return false;
        }
        delay(500);   // backoff simple entre sondeos
    }
    return false;     // spin-up nunca completó: drive probablemente muerto
}

/* =====================================================================
 * API de alto nivel
 * ===================================================================== */
bool scsi_read_sector(uint32_t lba, uint8_t *out512) {
    uint8_t cdb[6] = { 0x08,
                        (lba >> 16) & 0x1F,
                        (lba >> 8) & 0xFF,
                        lba & 0xFF,
                        1,              // 1 bloque
                        0x00 };

    for (uint8_t attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        uint8_t status;
        exec_result_t r = scsi_exec(TARGET_ID, cdb, 6, out512, 512, DIR_IN, &status);

        if (r == EXEC_TIMEOUT) continue;              // bus reset ya hecho, reintenta
        if (r == EXEC_NO_SELECT) return false;         // disco no responde en absoluto
        if (status == 0x00) return true;               // GOOD

        if (!scsi_recover_error(attempt)) return false; // CHECK_CONDITION no recuperable
        // si scsi_recover_error devuelve true, se reintenta el mismo LBA
    }
    return false;   // se agotaron los reintentos
}

bool scsi_write_sector(uint32_t lba, uint8_t *in512) {
    uint8_t cdb[6] = { 0x0A,
                        (lba >> 16) & 0x1F,
                        (lba >> 8) & 0xFF,
                        lba & 0xFF,
                        1,
                        0x00 };

    for (uint8_t attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        uint8_t status;
        exec_result_t r = scsi_exec(TARGET_ID, cdb, 6, in512, 512, DIR_OUT, &status);

        if (r == EXEC_TIMEOUT) continue;
        if (r == EXEC_NO_SELECT) return false;
        if (status == 0x00) return true;

        if (!scsi_recover_error(attempt)) return false;
    }
    return false;
}

/* =====================================================================
 * setup(): registrar la interrupción UNA vez
 * ===================================================================== */
void setup(void) {
    pinMode(PIN_REQ, INPUT);
    pinMode(PIN_ACK, OUTPUT);
    pinMode(PIN_CD,  INPUT);
    pinMode(PIN_IO,  INPUT);
    pinMode(PIN_MSG, INPUT);
    pinMode(PIN_BSY, INPUT);
    pinMode(PIN_SEL, OUTPUT);

    attachInterrupt(digitalPinToInterrupt(PIN_REQ),
                     ISR_scsi_req_change, CHANGE);
}
