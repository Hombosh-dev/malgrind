# STM32 Platform HAL (Future Target)

This directory is reserved for the STM32 Cortex-M HAL backend (e.g. STM32F4 / STM32H7):
- `DWT->CYCCNT` cycle counter implementation
- ARM Cortex-M SysTick / TIMx periodic sampling timer
- ARM Cortex-M frame pointer / LR stack unwinding
- USB-CDC / UART transport

Implements `mcu_flamegraph::PlatformBackend` concept without changing any code in `core/`.
