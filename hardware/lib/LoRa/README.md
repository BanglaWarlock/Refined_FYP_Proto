# Vendored LoRa library — PRISTINE sandeepmistry (your original working copy)

This is the unmodified library your original FYP firmware used. The demo
firmware uses no interrupts and no CAD, so no patches are needed.

The patched variant (SPI spinlock + selective IRQ + cadResult) that the
mesh-chaining build requires is archived at hardware/mesh-chaining/lib/LoRa/.
Install either by copying its src/ over Arduino/libraries/LoRa/src/.
