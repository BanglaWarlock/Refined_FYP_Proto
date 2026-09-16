# Patched LoRa library — REQUIRED by the mesh-chaining firmware

Patches vs pristine: (1) ISR-safe SPI spinlock (prevents ghost packets from
ISR/task SPI interleave), (2) selective IRQ clearing (flapping DIO0 must not
wipe RxDone flags), (3) cadResult() polling API (CAD without working DIO0).

Install: copy src/ over Arduino/libraries/LoRa/src/.
