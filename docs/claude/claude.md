Comandos mínimos imprescindibles
Para leer/escribir sectores en un HDD SCSI-1 concreto (geometría conocida en compilación), necesitas implementar solo estos:

REQUEST SENSE (0x03) — Aunque no lo vayas a "usar" para nada útil, es obligatorio en la práctica: tras power-on o un bus reset, el target genera un CHECK CONDITION con sense key UNIT ATTENTION en el primer comando que reciba. Si no emites REQUEST SENSE para limpiar esa condición, el drive rechazará todo lo demás.
READ(6) (0x08)
WRITE(6) (0x0A)
TEST UNIT READY (0x00) — opcional pero muy recomendable para sondear si el disco ya giró y está listo antes de mandar READ/WRITE.
START STOP UNIT (0x1B) — solo si tu modelo concreto no hace auto-spin-up al alimentarlo (muchos discos SCSI-1 de la época sí lo hacen solos, pero algunos requieren el comando explícito, sobre todo si el power sequencing lo desactiva por jumper).

Todo lo demás (INQUIRY, READ CAPACITY, MODE SENSE/SELECT, FORMAT UNIT, RESERVE/RELEASE) lo puedes omitir porque ya conoces de antemano el tamaño de bloque, número de bloques, geometría, etc. Solo te haría falta INQUIRY temporalmente durante el bring-up para verificar que el cableado/timing funciona, pero no en el firmware final.
Simplificación extra: evita la fase de mensajes
Si haces la SELECTION sin ATN (selección sin afirmar la línea ATN), el target no espera un mensaje IDENTIFY y —en la mayoría de discos SCSI-1— no intentará hacer disconnect/reconnect. Esto te ahorra implementar toda la gestión de mensajes salvo recibir el COMMAND COMPLETE al final. Es la técnica que usan la mayoría de proyectos retro de bitbanging SCSI en microcontroladores de 8 bits.
Fases de bus que sí tienes que manejar

BUS FREE
SELECTION (como iniciador; sin arbitraje porque eres el único iniciador)
COMMAND
DATA IN / DATA OUT
STATUS
MESSAGE IN (solo para leer el byte de COMMAND COMPLETE)

Formato del CDB para READ(6)/WRITE(6)
Son 6 bytes:

Byte 0: opcode (0x08 u 0x0A)
Byte 1: bits 7-5 = LUN (normalmente 0), bits 4-0 = bits altos del LBA
Bytes 2-3: resto del LBA (total 21 bits de dirección → máx. ~1 048 575 bloques, unos 512 MB con bloques de 512 B, más que de sobra para un disco SCSI-1 típico)
Byte 4: número de bloques a transferir (0 = 256 bloques)
Byte 5: control (normalmente 0x00)
