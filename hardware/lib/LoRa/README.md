# Vendored LoRa library (sandeepmistry master, patched)

Do NOT reinstall this library from the Library Manager — this copy carries two fixes our firmware depends on:

1. ISR-safe SPI spinlock: the DIO0 ISR and task code share one SPI bus; without the spinlock the ISR corrupts in-flight FIFO reads (ghost packets).
2. Selective IRQ clearing: the DIO0 ISR only clears flags it consumes. A flapping/noisy DIO0 line must not wipe pending RxDone flags that the parsePacket() polling path services.

To install on a new machine: copy src/LoRa.cpp and src/LoRa.h over your Arduino libraries/LoRa/src/.
