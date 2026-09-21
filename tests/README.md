# Core-1 safety checks

Run the host ownership, CEC chronology, and full-range PCM conversion test:

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I firmware/src tests/test_core1_safety.c -o /tmp/test_core1_safety
/tmp/test_core1_safety
```

Build every supported firmware combination with the pinned submodules:

```sh
for variant in debug release; do
  for uac in uac1 uac2; do
    cmake -S firmware -B build-$variant-$uac \
      -DPICOARC_VARIANT=$variant -DPICOARC_USB_AUDIO_CLASS=$uac \
      -DCMAKE_BUILD_TYPE=Release
    cmake --build build-$variant-$uac -j8
  done
done
```

At 96 kHz, the joined eight-word PIO FIFO provides about 21 us for the DMA
completion IRQ to start the next prepared block. If core 1 misses a whole
refill interval, the IRQ safely replays the completed 192-frame block and
increments `dma_late_blocks`; the repetition can still be audible. These
checks prove bounded DMA source ownership, but do not validate carrier timing,
USB behavior, reset behavior, flash writes, or audio quality on hardware. No
settings writer currently exists, and `copy_to_ram` plus
`PICO_FLASH_ASSUME_CORE1_SAFE` is not flash-write validation.
