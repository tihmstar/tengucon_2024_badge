#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"

#define LED_PIN 15

#define BLINKY_MODES_CNT 2

#define USEC_PER_SEC 1000000L

static int gBlinkyMode = 0;
static uint64_t gLastSwitchTime = 0;

bool __no_inline_not_in_flash_func(get_bootsel_button)() {
	const uint CS_PIN_INDEX = 1;

	// Must disable interrupts, as interrupt handlers may be in flash, and we
	// are about to temporarily disable flash access!
	uint32_t flags = save_and_disable_interrupts();

	// Set chip select to Hi-Z
	hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
		GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
		IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

	// Note we can't call into any sleep functions in flash right now
	for (volatile int i = 0; i < 1000; ++i);

	// The HI GPIO registers in SIO can observe and control the 6 QSPI pins.
	// Note the button pulls the pin *low* when pressed.
	bool button_state = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));

	// Need to restore the state of chip select, else we are going to have a
	// bad time when we return to code in flash!
	hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
		GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
		IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

	restore_interrupts(flags);

	return button_state;
}

void __not_in_flash_func(init_blinky)(void){
  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);
  gpio_put(LED_PIN, 1);
}

void __not_in_flash_func(runloop_blinky)(void){
  gpio_put(LED_PIN, 1);
  sleep_ms(100);
  gpio_put(LED_PIN, 0);
  sleep_ms(100);
}

void __not_in_flash_func(on_pwm_wrap)(void) {
    static int fade = 0;
    static bool going_up = true;
    // Clear the interrupt flag that brought us here
    pwm_clear_irq(pwm_gpio_to_slice_num(LED_PIN));

    if (going_up) {
        ++fade;
        if (fade > 255) {
            fade = 255;
            going_up = false;
        }
    } else {
        --fade;
        if (fade < 0) {
            fade = 0;
            going_up = true;
        }
    }
    // Square the fade value to make the LED's brightness appear more linear
    // Note this range matches with the wrap value
    pwm_set_gpio_level(LED_PIN, fade * fade);
}

void __not_in_flash_func(init_pwm)(void){
  gpio_init(LED_PIN);
  gpio_set_function(LED_PIN, GPIO_FUNC_PWM);
  uint slice_num = pwm_gpio_to_slice_num(LED_PIN);

  // Mask our slice's IRQ output into the PWM block's single interrupt line,
  // and register our interrupt handler
  pwm_clear_irq(slice_num);
  pwm_set_irq_enabled(slice_num, true);
  irq_set_exclusive_handler(PWM_DEFAULT_IRQ_NUM(), on_pwm_wrap);
  irq_set_enabled(PWM_DEFAULT_IRQ_NUM(), true);

  // Get some sensible defaults for the slice configuration. By default, the
  // counter is allowed to wrap over its maximum range (0 to 2**16-1)
  pwm_config config = pwm_get_default_config();
  // Set divider, reduces counter clock to sysclock/this value
  pwm_config_set_clkdiv(&config, 4.f);
  // Load the configuration into our PWM slice, and set it running.
  pwm_init(slice_num, &config, true);
}

void __not_in_flash_func(init_blinkyMode)(void){
  if (gBlinkyMode == 0){
    init_blinky();
  }else if (gBlinkyMode == 1){
    init_pwm();
  } 
}

void __not_in_flash_func(check_button)(void){
  if (get_bootsel_button()){
    uint64_t curTime = time_us_64();
    if (curTime - gLastSwitchTime > USEC_PER_SEC*2){
      gLastSwitchTime = curTime;
      gBlinkyMode = (gBlinkyMode+1) % BLINKY_MODES_CNT;
      init_blinkyMode();
    }
  }
}

int __not_in_flash_func(main)(void) {
  init_blinkyMode();
  while (1){
    if (gBlinkyMode == 0){
      runloop_blinky();
    }
    check_button();
  }

  return 0;
}