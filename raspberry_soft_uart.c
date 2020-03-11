
#include "raspberry_soft_uart.h"
#include "queue.h"

#include <linux/gpio.h> 
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/version.h>

static irq_handler_t handle_rx_start(unsigned int irq, void* device, struct pt_regs* registers);
static enum hrtimer_restart handle_tx(struct hrtimer* timer);
static enum hrtimer_restart handle_rx(struct hrtimer* timer);
static void receive_character(unsigned char character);

static struct queue queue_tx;
static struct tty_struct* current_tty = NULL;
static DEFINE_MUTEX(current_tty_mutex);
static struct hrtimer timer_tx;
static struct hrtimer timer_rx;
static ktime_t period;
static ktime_t half_period;
static int gpio_tx = 0;
static int gpio_rx = 0;
static int rx_bit_index = -1;
static void (*rx_callback)(unsigned char) = NULL;
static int stop_bits = 1;
static int parity_en = 0;
static int ignore_parity_errors = 0;

static int parity_init = 0;
static int final_stop_bit_index = 8;
static int parity_index = -1;

/**
 * Initializes the Raspberry Soft UART infrastructure.
 * This must be called during the module initialization.
 * The GPIO pin used as TX is configured as output.
 * The GPIO pin used as RX is configured as input.
 * @param gpio_tx GPIO pin used as TX
 * @param gpio_rx GPIO pin used as RX
 * @return 1 if the initialization is successful. 0 otherwise.
 */
int raspberry_soft_uart_init(const int _gpio_tx, const int _gpio_rx)
{
  bool success = true;
  
  mutex_init(&current_tty_mutex);
  
  // Initializes the TX timer.
  hrtimer_init(&timer_tx, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
  timer_tx.function = &handle_tx;
  
  // Initializes the RX timer.
  hrtimer_init(&timer_rx, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
  timer_rx.function = &handle_rx;
  
  // Initializes the GPIO pins.
  gpio_tx = _gpio_tx;
  gpio_rx = _gpio_rx;
    
  success &= gpio_request(gpio_tx, "soft_uart_tx") == 0;
  success &= gpio_direction_output(gpio_tx, 1) == 0;

  success &= gpio_request(gpio_rx, "soft_uart_rx") == 0;
  success &= gpio_direction_input(gpio_rx) == 0;
  
  // Initializes the interruption.
  success &= request_irq(
    gpio_to_irq(gpio_rx),
    (irq_handler_t) handle_rx_start,
    IRQF_TRIGGER_FALLING,
    "soft_uart_irq_handler",
    NULL) == 0;
  disable_irq(gpio_to_irq(gpio_rx));
    
  return success;
}

/**
 * Finalizes the Raspberry Soft UART infrastructure.
 */
int raspberry_soft_uart_finalize(void)
{
  free_irq(gpio_to_irq(gpio_rx), NULL);
  gpio_set_value(gpio_tx, 0);
  gpio_free(gpio_tx);
  gpio_free(gpio_rx);
  return 1;
}

/**
 * Opens the Soft UART.
 * @param tty
 * @return 1 if the operation is successful. 0 otherwise.
 */
int raspberry_soft_uart_open(struct tty_struct* tty)
{
  int success = 0;
  mutex_lock(&current_tty_mutex);
  rx_bit_index = -1;
  if (current_tty == NULL)
  {
    current_tty = tty;
    initialize_queue(&queue_tx);
    success = 1;
    enable_irq(gpio_to_irq(gpio_rx));
  }
  mutex_unlock(&current_tty_mutex);
  return success;
}

/**
 * Closes the Soft UART.
 */
int raspberry_soft_uart_close(void)
{
  mutex_lock(&current_tty_mutex);
  disable_irq(gpio_to_irq(gpio_rx));
  hrtimer_cancel(&timer_tx);
  hrtimer_cancel(&timer_rx);
  current_tty = NULL;
  mutex_unlock(&current_tty_mutex);
  return 1;
}

/**
 * Sets the Soft UART baudrate.
 * @param baudrate desired baudrate
 * @return 1 if the operation is successful. 0 otherwise.
 */
int raspberry_soft_uart_set_baudrate(const int baudrate) 
{
  period = ktime_set(0, 1000000000/baudrate);
  half_period = ktime_set(0, 1000000000/baudrate/2);
  gpio_set_debounce(gpio_rx, 1000/baudrate/2);
  return 1;
}

static void recalc_indices(void)
{
  if (parity_en)
  {
    parity_index = 8;
    final_stop_bit_index = parity_index + stop_bits;
  }
  else
  {
    parity_index = -1;
    final_stop_bit_index = 7 + stop_bits;
  }
}


/**
 * Sets the number of stop bits.
 * @param _stop_bits number of stop bits (1 or 2)
 * @return 1 if the operation is successful. 0 otherwise.
 */
int raspberry_soft_uart_set_stop_bits(int _stop_bits)
{
  stop_bits = _stop_bits;
  recalc_indices();
  return 1;
}

/**
 * Enables or disables parity bit.
 * @param _parity_en 1 to enable, 0 to disable.
 * @param parity_odd 1 for odd parity, 0 for even parity
 * @param _ignore_parity_errors 1 to receive characters with wrong parity bit, 0 to drop them.
 * @return 1 if the operation is successful. 0 otherwise.
 */
int raspberry_soft_uart_set_parity(int _parity_en, int parity_odd, int _ignore_parity_errors)
{
  parity_en = _parity_en;
  if (parity_odd)
  {
    parity_init = 1;
  }
  else
  {
    parity_init = 0;
  }
  ignore_parity_errors = _ignore_parity_errors;

  recalc_indices();
  return 1;
}

/**
 * Adds a given string to the TX queue.
 * @paran string given string
 * @param string_size size of the given string
 * @return The amount of characters successfully added to the queue.
 */
int raspberry_soft_uart_send_string(const unsigned char* string, int string_size)
{
  int result = enqueue_string(&queue_tx, string, string_size);
  
  // Starts the TX timer if it is not already running.
  if (!hrtimer_active(&timer_tx))
  {
    hrtimer_start(&timer_tx, period, HRTIMER_MODE_REL);
  }
  
  return result;
}

/*
 * Gets the number of characters that can be added to the TX queue.
 * @return number of characters.
 */
int raspberry_soft_uart_get_tx_queue_room(void)
{
  return get_queue_room(&queue_tx);
}

/*
 * Gets the number of characters in the TX queue.
 * @return number of characters.
 */
int raspberry_soft_uart_get_tx_queue_size(void)
{
  return get_queue_size(&queue_tx);
}

/**
 * Sets the callback function to be called on received character.
 * @param callback the callback function
 */
int raspberry_soft_uart_set_rx_callback(void (*callback)(unsigned char))
{
	rx_callback = callback;
	return 1;
}

//-----------------------------------------------------------------------------
// Internals
//-----------------------------------------------------------------------------

/**
 * If we are waiting for the RX start bit, then starts the RX timer. Otherwise,
 * does nothing.
 */
static irq_handler_t handle_rx_start(unsigned int irq, void* device, struct pt_regs* registers)
{
  if (rx_bit_index == -1)
  {
    hrtimer_start(&timer_rx, half_period, HRTIMER_MODE_REL);
  }
  return (irq_handler_t) IRQ_HANDLED;
}


/**
 * Dequeues a character from the TX queue and sends it.
 */
static enum hrtimer_restart handle_tx(struct hrtimer* timer)
{
  ktime_t current_time = ktime_get();
  static unsigned char character = 0;
  static int bit_index = -1;
  enum hrtimer_restart result = HRTIMER_NORESTART;
  bool must_restart_timer = false;
  static int parity = 0;
  
  // Start bit.
  if (bit_index == -1)
  {
    if (dequeue_character(&queue_tx, &character))
    {
      gpio_set_value(gpio_tx, 0);
      bit_index++;
      parity = parity_init;
      must_restart_timer = true;
    }
  }
  
  // Data bits.
  else if (0 <= bit_index && bit_index < 8)
  {
    int bit_value = 1 & (character >> bit_index);
    gpio_set_value(gpio_tx, bit_value);
    parity ^= bit_value;
    bit_index++;
    must_restart_timer = true;
  }

  // Parity bit (optional)
  else if (bit_index == parity_index)
  {
    gpio_set_value(gpio_tx, parity);
    bit_index++;
    must_restart_timer = true;
  }
  
  // Stop bit(s).
  else if (bit_index <= final_stop_bit_index)
  {
    gpio_set_value(gpio_tx, 1);
    if (bit_index == final_stop_bit_index)
    {
      character = 0;
      bit_index = -1;
      parity = 0;
      must_restart_timer = get_queue_size(&queue_tx) > 0;
    }
    else
    {
      bit_index++;
      must_restart_timer = true;
    }
  }
  
  // Restarts the TX timer.
  if (must_restart_timer)
  {
    hrtimer_forward(&timer_tx, current_time, period);
    result = HRTIMER_RESTART;
  }
  
  return result;
}

/*
 * Receives a character and sends it to the kernel.
 */
static enum hrtimer_restart handle_rx(struct hrtimer* timer)
{
  ktime_t current_time = ktime_get();
  static unsigned int character = 0;
  static int parity = 0;
  static bool parity_ok = true;
  int bit_value = gpio_get_value(gpio_rx);
  enum hrtimer_restart result = HRTIMER_NORESTART;
  bool must_restart_timer = false;
  
  // Start bit.
  if (rx_bit_index == -1)
  {
    rx_bit_index++;
    character = 0;
    parity = parity_init;
    parity_ok = true;
    must_restart_timer = true;
  }
  
  // Data bits.
  else if (0 <= rx_bit_index && rx_bit_index < 8)
  {
    if (bit_value == 0)
    {
      character &= 0xfeff;
    }
    else
    {
      character |= 0x0100;
    }
    parity ^= bit_value;
    
    rx_bit_index++;
    character >>= 1;
    must_restart_timer = true;
  }

  // Parity bit (optional)
  else if (rx_bit_index == parity_index)
  {
    if (bit_value != parity)
    {
      parity_ok = false;
    }
    rx_bit_index++;
    must_restart_timer = true;
  }

  // Extra stop bit (optional)
  else if (rx_bit_index < final_stop_bit_index)
  {
    rx_bit_index++;
    must_restart_timer = true;
  }
  
  // Final stop bit.
  else if (rx_bit_index == final_stop_bit_index)
  {
    if (parity_ok || ignore_parity_errors)
    {
      receive_character(character);
    }
    rx_bit_index = -1;
  }
  
  // Restarts the RX timer.
  if (must_restart_timer)
  {
    hrtimer_forward(&timer_rx, current_time, period);
    result = HRTIMER_RESTART;
  }
  
  return result;
}

/**
 * Adds a given (received) character to the RX buffer, which is managed by the kernel,
 * and then flushes (flip) it.
 * @param character given character
 */
void receive_character(unsigned char character)
{
  mutex_lock(&current_tty_mutex);
  if (rx_callback != NULL) {
	  (*rx_callback)(character);
  } else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
    if (current_tty != NULL && current_tty->port != NULL)
    {
      tty_insert_flip_char(current_tty->port, character, TTY_NORMAL);
      tty_flip_buffer_push(current_tty->port);
    }
#else
    if (tty != NULL)
    {
      tty_insert_flip_char(current_tty, character, TTY_NORMAL);
      tty_flip_buffer_push(tty);
    }
#endif
  }
  mutex_unlock(&current_tty_mutex);
}
