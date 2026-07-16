# Mapeo físico y esquema eléctrico: Arduino Leonardo ↔ 74LS638 / 74LS04 / 74LS06 ↔ SCSI-1 (RO652A)

## 1. Reparto de chips

- **U1 — 74LS638:** bus de datos DB0-DB7 (el único bidireccional).
- **U2 — 74LS04** (inversor hex, push-pull estándar, 6 puertas): las 5
  señales que solo *lees* del SCSI: BSY, REQ, C/D, I/O, MSG. Sobra
  1 puerta libre. Push-pull es válido aquí porque la salida de cada
  puerta solo alimenta una entrada de alta impedancia del Arduino —
  nadie más conduce esa línea, así que no hay competencia eléctrica
  posible. De hecho es preferible al 74LS05: sin pull-ups externos y
  con flancos más rápidos y limpios, mejor para la línea REQ que
  dispara la interrupción.
- **U3 — 74LS06** (inversor hex, colector abierto, 40 mA, 6 puertas):
  las 3 señales que solo *escribes* hacia el SCSI: SEL, ACK, RST.
  Sobran 3 puertas libres.

Al ser inversores simples (sin DIR/OE), aquí no hay nada que
configurar por cableado: cada señal SCSI pasa por una puerta y ya
está.

⚠️ La regla que **no** se puede relajar es la del lado de salida
(U3): ahí sí tiene que ser colector abierto obligatoriamente, porque
esa puerta compite eléctricamente con el pull-up del terminador del
bus (y potencialmente con el propio disco). Un push-pull ahí forzaría
la línea y podría dañar el chip o el disco.

Polaridad resultante (igual criterio que con el 638): como ambos
chips son inversores y las líneas SCSI son activas-en-bajo, en el
lado Arduino "asertado" siempre se lee/escribe como **HIGH** — misma
convención que ya usa el pseudocódigo de la ISR.

**Nota sobre paridad (DBP):** para mantenerlo simple, este diseño **no
implementa el bit de paridad**. El RO652A permite en muchos casos
desactivar la comprobación de paridad por configuración; si tu unidad
concreta la exige, tendrías que añadir un noveno canal (usar un canal
libre de U1 con un XOR de 8 entradas externo, o un segundo 638 de un
solo canal). Verifícalo en la práctica: si el drive devuelve
`CHECK CONDITION` con `PARITY ERROR` en el sense, hay que añadirlo.

---

## 2. Cableado de cada chip

### U1 — 74LS638 (datos, 20 pines)

```
              74LS638 (U1 - DATA, DIR dinámico)
        +--------------------------------+
   DIR--|1                             20|--VCC (+5V)
 SCSI DB0--|2  A1                    B1  19|--Arduino DATA0
 SCSI DB1--|3  A2                    B2  18|--Arduino DATA1
 SCSI DB2--|4  A3                    B3  17|--Arduino DATA2
 SCSI DB3--|5  A4                    B4  16|--Arduino DATA3
 SCSI DB4--|6  A5                    B5  15|--Arduino DATA4
 SCSI DB5--|7  A6                    B6  14|--Arduino DATA5
 SCSI DB6--|8  A7                    B7  13|--Arduino DATA6
 SCSI DB7--|9  A8                    B8  12|--Arduino DATA7
       GND--|10                       OE 11|--GND (siempre habilitado)
        +--------------------------------+
```

### U2 — 74LS04 (entradas, 14 pines, push-pull)

Pinout estándar de la familia 74LSx4/x5/x6 (idéntico al 7404):

```
              74LS04 (U2 - CONTROL-IN)
        +----------------+
   BSY--|1  1A      1Y  2|--> Arduino D10
   REQ--|3  2A      2Y  4|--> Arduino D0   (INT0)
   C/D--|5  3A      3Y  6|--> Arduino A0
   GND--|7               |
   MSG--|9  4A      4Y  8|--> Arduino D13
   I/O--|11 5A      5Y 10|--> Arduino A1
        |   6A      6Y 12|--  libre (spare)
        |13             14|--VCC (+5V)
        +----------------+
```

(Numeración de pines: 1A/1Y=1,2 · 2A/2Y=3,4 · 3A/3Y=5,6 · GND=7 ·
4Y/4A=8,9 · 5Y/5A=10,11 · 6Y/6A=12,13 · VCC=14.)

**Sin pull-ups necesarios:** al ser push-pull, cada salida de U2 ya
conduce activamente tanto a HIGH como a LOW — no hace falta
resistencia externa ni `INPUT_PULLUP` en el Arduino. En firmware, usa
simplemente:
```c
pinMode(PIN_BSY, INPUT);
pinMode(PIN_REQ, INPUT);
pinMode(PIN_CD,  INPUT);
pinMode(PIN_MSG, INPUT);
pinMode(PIN_IO,  INPUT);
```

### U3 — 74LS06 (salidas, 14 pines, colector abierto, 40 mA)

Mismo pinout físico que el 74LS04 (familia idéntica):

```
              74LS06 (U3 - CONTROL-OUT)
        +----------------+
   SEL--|1  1A      1Y  2|--< Arduino A2
   ACK--|3  2A      2Y  4|--< Arduino D11
   RST--|5  3A      3Y  6|--< Arduino D12
   GND--|7               |
        |9  4A      4Y  8|--  libre (spare)
        |11 5A      5Y 10|--  libre (spare)
        |   6A      6Y 12|--  libre (spare)
        |13             14|--VCC (+5V)
        +----------------+
```

Aquí NO hace falta pull-up en ningún lado: el lado SCSI (pines 1,3,5)
ya está tirado a +5V por el terminador del bus, y el lado Arduino
(pines 2,4,6) lo maneja el propio Arduino en push-pull normal, sin
necesitar resistencia.

**Desacoplo:** un condensador cerámico de 100 nF entre VCC y GND lo más
cerca posible de cada chip (pines 20/10), imprescindible en los tres.

---

## 3. Terminación del bus SCSI

El RO652A trae **packs de resistencias terminadoras** en la propia
placa (se ve en la serigrafía de la hoja de specs). SCSI exige
terminación en **ambos extremos físicos** del bus:

- **Extremo del disco:** deja puesto (o vuelve a poner) su pack
  terminador — el RO652 casi siempre se vende con el jumper/pack de
  terminación instalado de fábrica al ser normalmente el único
  dispositivo interno de un Mac/PC de la época.
- **Extremo del Arduino (tu "host adapter"):** necesitas añadir tú un
  terminador pasivo, típicamente un pack SIP de 220 Ω a TERMPWR (+5V)
  y 330 Ω a GND por cada línea de señal (18 líneas: 8 datos + paridad
  + 9 de control). Existen packs comerciales listos (p.ej.
  "9-line SCSI terminator resistor pack").
- **TERMPWR (pin 26):** alguien tiene que alimentarlo con +5V a través
  de un diodo (típicamente 1N5817) y un fusible/polyfuse de ~0.5-1A.
  Si el disco ya lo suministra (frecuente en unidades con terminador
  activo de fábrica), no hace falta que lo hagas también desde el
  Arduino — comprueba con un multímetro si el pin 26 ya tiene +5V
  antes de conectar tu propia fuente, para no pelear dos fuentes entre sí.

Dado que a las velocidades async de bitbang y con un cable corto
punto-a-punto el margen es muy generoso, un terminador algo
imperfecto probablemente "funcione" en pruebas de banco — pero seguir
la terminación en ambos extremos evita reflejos que pueden manifestarse
como bytes corruptos intermitentes, difíciles de diagnosticar.

---

## 4. Mapeo de pines: Arduino Leonardo ↔ 74LS638 ↔ conector SCSI-1 (50 pin IDC)

El RO652A es un drive interno con conector de 50 pines (2×25, paso
0.1"). Pinout estándar SCSI-1 de un solo extremo (single-ended):

| Pin | Señal | Pin | Señal |
|-----|-------|-----|-------|
| 1   | GND   | 2   | -DB0  |
| 3   | GND   | 4   | -DB1  |
| 5   | GND   | 6   | -DB2  |
| 7   | GND   | 8   | -DB3  |
| 9   | GND   | 10  | -DB4  |
| 11  | GND   | 12  | -DB5  |
| 13  | GND   | 14  | -DB6  |
| 15  | GND   | 16  | -DB7  |
| 17  | GND   | 18  | -DBP (paridad) |
| 19  | GND   | 20  | GND   |
| 21  | GND   | 22  | GND   |
| 23  | reservado | 24 | reservado |
| 25  | abierto (no usar) | 26 | TERMPWR (+5V) |
| 27  | reservado | 28 | reservado |
| 29  | GND   | 30  | GND   |
| 31  | GND   | 32  | -ATN  |
| 33  | GND   | 34  | GND   |
| 35  | GND   | 36  | -BSY  |
| 37  | GND   | 38  | -ACK  |
| 39  | GND   | 40  | -RST  |
| 41  | GND   | 42  | -MSG  |
| 43  | GND   | 44  | -C/D  |
| 45  | GND   | 46  | -REQ  |
| 47  | GND   | 48  | -I/O  |
| 49  | GND   | 50  | -SEL  |

Todas las señales son activas-en-bajo (negative-true). Los pines
impares son mayoritariamente GND — esto es intencional en el diseño
del cable: cada línea de señal va emparejada con un retorno de tierra
adyacente para reducir diafonía/reflejos.

### Tabla de conexión completa

| Señal SCSI | Pin conector | Chip / puerta | Pin Arduino Leonardo | Notas |
|---|---|---|---|---|
| DB0 | 2  | U1.A1 / U1.B1 | D2  | Bus de datos |
| DB1 | 4  | U1.A2 / U1.B2 | D3  | |
| DB2 | 6  | U1.A3 / U1.B3 | D4  | |
| DB3 | 8  | U1.A4 / U1.B4 | D5  | |
| DB4 | 10 | U1.A5 / U1.B5 | D6  | |
| DB5 | 12 | U1.A6 / U1.B6 | D7  | |
| DB6 | 14 | U1.A7 / U1.B7 | D8  | |
| DB7 | 16 | U1.A8 / U1.B8 | D9  | |
| DBP | 18 | — | — | no implementada (ver nota de paridad) |
| BSY | 36 | U2 (74LS04) gate 1 | D10 | solo lectura |
| ACK | 38 | U3 (74LS06) gate 1 | D11 | solo escritura |
| RST | 40 | U3 (74LS06) gate 2 | D12 | solo escritura |
| MSG | 42 | U2 (74LS04) gate 4 | D13 | solo lectura |
| C/D | 44 | U2 (74LS04) gate 3 | A0  | solo lectura |
| REQ | 46 | U2 (74LS04) gate 2 | **D0** (INT0) | **interrupción**, flancos limpios sin pull-up |
| I/O | 48 | U2 (74LS04) gate 5 | A1  | solo lectura |
| SEL | 50 | U3 (74LS06) gate 3 | A2  | solo escritura |
| U1.DIR | — | — | A3  | control dinámico dirección datos |

**Por qué REQ en D0:** en el Leonardo (ATmega32u4), las únicas líneas
con interrupción externa "de verdad" (`INT`) son D0, D1, D2, D3 y D7
— cualquiera de ellas vale para `attachInterrupt(..., CHANGE)`. Elegí
D0 porque queda libre de otros usos (D1/D2/D3 en muchos Leonardo
también sirven de I2C/Serial1, mejor dejarlos libres por si los
necesitas para depuración por el monitor serie).

**TERMPWR (pin 26):** solo conéctalo a tu +5V si compruebas que el
disco no lo está ya suministrando (ver sección 3).

**GND:** conecta varios de los pines GND del conector (p.ej. 1, 3, 5,
20, 41, 49) a la masa común de tu placa — no hace falta cablear los 25,
pero sí varios repartidos para un buen retorno de corriente.

---

## 5. Resumen de asignación de pines en el Leonardo

```
D0  -> REQ   (INT0, entrada)
D1  -> (libre, reservado para Serial1 TX si lo necesitas)
D2  -> DB0
D3  -> DB1
D4  -> DB2
D5  -> DB3
D6  -> DB4
D7  -> DB5
D8  -> DB6
D9  -> DB7
D10 -> BSY   (entrada)
D11 -> ACK   (salida)
D12 -> RST   (salida)
D13 -> MSG   (entrada)   // ojo: D13 tiene el LED integrado, no afecta
                          // funcionalmente pero parpadeará con MSG
A0  -> C/D   (entrada)
A1  -> I/O   (entrada)
A2  -> SEL   (salida)
A3  -> DIR del U1 (salida)
```

Con esto tienes wiring completo y coherente con el pseudocódigo de la
ISR de la sección anterior (`PIN_REQ=D0`, `PIN_ACK=D11`, etc. — solo
falta actualizar las constantes `#define` del firmware con estos
números de pin reales).
