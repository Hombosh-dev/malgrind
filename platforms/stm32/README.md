# STM32 port (not started)

A Cortex-M port follows the contract in the top-level README ("Adding an MCU"), with `Arch::ArmV7M`:

- periodic timer (TIMx or SysTick) calling `Profiler::record()`;
- stack unwinding from the exception frame: the stacked PC is the exact leaf, then walk frames
  (frame pointers via `-fno-omit-frame-pointer`, or unwind tables);
- UART (or USB-CDC) transport for `Profiler::flush()`;
- `DWT->CYCCNT` for `isr_cycles` and the flush cost.

Zero-overhead alternative for the future probe mode: PC sampling through `DWT_PCSR` (no halt) or SWO
(see orbuculum). It gives a flat profile only.
