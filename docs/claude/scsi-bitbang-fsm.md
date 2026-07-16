# Máquina de estados SCSI-1 para bitbanging (iniciador único, sin ATN)

## 1. Fases del bus (nivel alto)

El iniciador reconoce la fase leyendo las líneas **C/D, I/O, MSG** que el
target pone estables justo antes/durante REQ. No hay arbitraje porque eres
el único iniciador en el bus.

```mermaid
stateDiagram-v2
    [*] --> BUS_FREE

    BUS_FREE --> SELECTION: Iniciador quiere mandar comando

    state SELECTION {
        [*] --> SEL_ASSERT
        SEL_ASSERT --> SEL_WAIT_BSY: Pon ID_iniciador+ID_target en DB,\nafirma SEL (sin ATN)
        SEL_WAIT_BSY --> SEL_OK: Target afirma BSY (timeout ~250ms)
        SEL_WAIT_BSY --> SEL_TIMEOUT: No responde
    }
    SELECTION --> BUS_FREE: SEL_TIMEOUT (disco no responde)
    SELECTION --> COMMAND: SEL_OK\n(suelta SEL cuando BSY activo)

    COMMAND --> COMMAND: Handshake REQ/ACK\n(6 bytes del CDB, C/D=1,I/O=0,MSG=0)

    COMMAND --> DATA_IN: si comando de lectura,\ntarget cambia C/D=0,I/O=1
    COMMAND --> DATA_OUT: si comando de escritura,\ntarget cambia C/D=0,I/O=0
    COMMAND --> STATUS: si comando sin datos\n(TEST UNIT READY, START STOP)
    COMMAND --> STATUS: si target reporta error\n(CHECK CONDITION inmediato)

    DATA_IN --> DATA_IN: Handshake REQ/ACK\n(N bytes, target->iniciador)
    DATA_OUT --> DATA_OUT: Handshake REQ/ACK\n(N bytes, iniciador->target)

    DATA_IN --> STATUS: Todos los bytes transferidos\n(C/D=1,I/O=1,MSG=0)
    DATA_OUT --> STATUS: Todos los bytes transferidos

    STATUS --> STATUS: Handshake REQ/ACK\n(1 byte: GOOD=0x00 / CHECK_COND=0x02)

    STATUS --> MSG_IN: target cambia\nC/D=1,I/O=1,MSG=1

    MSG_IN --> MSG_IN: Handshake REQ/ACK\n(1 byte: COMMAND COMPLETE=0x00)

    MSG_IN --> BUS_FREE: Target suelta BSY\n(fin de comando)

    STATUS --> REQUEST_SENSE_NEEDED: si status fue CHECK_CONDITION
    REQUEST_SENSE_NEEDED --> BUS_FREE: (tras MSG_IN)\nlanzar REQUEST SENSE\ncomo nuevo comando
```

**Notas clave:**
- Sin ATN afirmado en SELECTION, el target no espera MESSAGE OUT (te ahorras esa fase).
- El iniciador **nunca decide la fase**: solo la lee de C/D/I-O/MSG y actúa en consecuencia. Si el firmware ve una fase inesperada, debe abortar (soltar el bus, hacer RST si hace falta).
- `REQUEST_SENSE_NEEDED`: tras cualquier `CHECK CONDITION`, el siguiente comando *debe* ser `REQUEST SENSE` o el disco se queda "atascado" en esa condición de error.

---

## 2. Handshake REQ/ACK (el que se ejecuta por cada byte, en cualquier fase)

Es un protocolo **asíncrono** — no hay reloj, todo se sincroniza a mano con
REQ/ACK. Es idéntico en estructura para las 4 fases de transferencia
(COMMAND, DATA, STATUS, MESSAGE IN); solo cambia quién conduce el bus de
datos.

### 2a. Target → Iniciador (DATA IN, STATUS, MESSAGE IN)

```mermaid
stateDiagram-v2
    [*] --> WAIT_REQ_HIGH
    WAIT_REQ_HIGH --> READ_DATA: REQ afirmado por target\n(target ya puso byte válido en DB)
    READ_DATA --> ASSERT_ACK: Iniciador lee DB\n(digitalRead x8, o puerto entero)
    ASSERT_ACK --> WAIT_REQ_LOW: Iniciador afirma ACK
    WAIT_REQ_LOW --> DEASSERT_ACK: Target ve ACK,\ndeasfirma REQ (retira dato)
    DEASSERT_ACK --> WAIT_REQ_HIGH: Iniciador deasfirma ACK\n(byte completo, incrementa contador)
    WAIT_REQ_HIGH --> [*]: si contador de bytes\nde la fase = 0 -> fase terminada
```

### 2b. Iniciador → Target (COMMAND, DATA OUT)

```mermaid
stateDiagram-v2
    [*] --> WAIT_REQ_HIGH
    WAIT_REQ_HIGH --> DRIVE_DATA: REQ afirmado por target\n(target listo para recibir)
    DRIVE_DATA --> ASSERT_ACK: Iniciador pone byte en DB\n(configura puerto como salida)
    ASSERT_ACK --> WAIT_REQ_LOW: Iniciador afirma ACK
    WAIT_REQ_LOW --> RELEASE_DATA: Target latchea el dato,\ndeasfirma REQ
    RELEASE_DATA --> DEASSERT_ACK: Iniciador suelta DB\n(vuelve a alta impedancia / entrada)
    DEASSERT_ACK --> WAIT_REQ_HIGH: Iniciador deasfirma ACK
    WAIT_REQ_HIGH --> [*]: si contador de bytes\nde la fase = 0 -> fase terminada
```

**Timing crítico a respetar en el bitbang (SCSI-1 async, cable corto):**
- Data setup time antes de ACK: ≥ 90 ns (en la práctica, con GPIO de 8 bits a través de un pin-change, ni lo notarás — el cuello de botella real es tu velocidad de toggling de pines).
- Bus settle / skew entre líneas de datos: ≥ 10 ns.
- Ninguno de estos plazos es alcanzable "por accidente" en un micro de 8 bits a decenas de MHz sin cuidado — en la práctica te sobra margen, el problema real suele ser la *capacitancia del cable* si no usas buffers (74LS641 o similar) entre el micro y el bus SCSI de 5V.

---

## 3. Pseudocódigo del bucle central

```c
uint8_t scsi_handshake_in(void) {           // target -> iniciador
    while (!REQ_asserted());                 // espera REQ
    uint8_t byte = read_data_bus();
    ACK_assert();
    while (REQ_asserted());                  // espera a que target suelte REQ
    ACK_deassert();
    return byte;
}

void scsi_handshake_out(uint8_t byte) {      // iniciador -> target
    while (!REQ_asserted());
    drive_data_bus(byte);
    ACK_assert();
    while (REQ_asserted());
    release_data_bus();
    ACK_deassert();
}
```

Con estas dos funciones (`scsi_handshake_in` / `scsi_handshake_out`) y la
máquina de fases de arriba, ya tienes toda la lógica de bajo nivel que
necesita READ(6)/WRITE(6)/STATUS/MESSAGE — cada fase es solo "llamar N
veces a la función correcta".
