#include <inttypes.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/eeprom.h>
#include <util/delay.h>
#include <stdio.h>

// Настройки МК
#define F 8000000UL // Частота 8 МГц
#define BAUD 9600 // Кол-во бит в секунду (бод) для передачи по USART

// Настройки программы
#define SECONDS_IN_MINUTE 60
#define BLINK_SECONDS 15
#define SOUND_SECONDS 4

// Настройки МК
// Коды цифр семисегментного индикатора
static const unsigned int seven_segment_digits[] = {0xC0, 0xF9, 0xA4, 0xB0, 0x99, 0x92, 0x82, 0xF8, 0x80, 0x90};
// Переменные для работы с EEPROM
uint8_t EEMEM eemem_a_min = 0, eemem_a_sec = 0, eemem_b_min = 0, eemem_b_sec = 0;

// Настройки программы
// Возможные интервалы времени на игру
static const unsigned int intervals[] = {1, 5, 15}; // 1 - для отладки
// Индекс выбранного интервала
static unsigned int interval_index = 1;
// Какая цифра ССИ будет гореть
static unsigned int digit = 0;
// Текущие значения времени, оставшегося у игроков
static unsigned int player_a_min = 0;
static unsigned int player_a_sec = 0;
static unsigned int player_b_min = 0;
static unsigned int player_b_sec = 0;
// Значения, считываемые из EEPROM
uint8_t player_a_minutes, player_a_seconds, player_b_minutes, player_b_seconds;

// Чьи часы показывать
static unsigned int player_a_show = 1; // по умолчанию показываем часы игрока А
// Счетчик секунд
static unsigned int seconds_spent = 0;
// Флаг того, что прошла секунда
static unsigned int SEC_PASS = 0;
// Флаг того, что нужно мигать
static unsigned int BLINK_ENABLE = 0;

typedef enum
{
    NO_CLOCK,
    PLAYER_A_CLOCK,
    PLAYER_B_CLOCK,
    SEND_RESULT,
    FINISH
} Clock;
// Какие часы будут работать
static Clock chosen_clock = NO_CLOCK;

// Инициализация таймера 0
void timer0_init(void)
{
    // Разрешаем прерывания по переполнению
    TIMSK |= (1 << TOIE0);
    TCCR0 = (1 << CS02);
}

// Инициализация таймера 1
void timer1_init(void)
{
    // Устанавливаем режим CTC (сброс по совпадению)
    TCCR1B |= (1 << WGM12);
    // Разрешаем прерывания по совпадению с OCR1A(H и L)
    TIMSK |= (1 << OCIE1A);

    // Записываем в регистр число для сравнения
    OCR1AH = 0x3D09 >> 8; // 15625 = 8 МГц / 256 / 2
    OCR1AL = 0x3D09 && 0xFF;
    // Установим предделитель на 256
    TCCR1B |= (1 << CS12);
}

// Инициализация портов ввода-вывода  
void ports_init(void)
{
    // Инициализация всего порта А на вывод
    PORTA=0xFF;
    DDRA=0xFF;

    // Инициализация всего порта В на вывод 
    PORTB=0xFF;
    DDRB=0xFF;

    // Инициализация всего порта С на вывод
    PORTC=0x00;
    DDRC=0xFF;

    // Инициализация порта D:
    // Func7=In Func6=Out Func5=Out Func4=Out Func3=In Func2=In Func1=Out Func0=Out 
    // State7=T State6=0 State5=1 State4=1 State3=T State2=T State1=1 State0=1 
    PORTD=0x33;
    DDRD=0x73; 

    // Инициализация всего порта Е на ввод
    // Func2=Out Func1=Out Func0=In 
    PORTE=0x00;
    DDRE=0x06;
}

void buttons_init(void)
{
    // Разрешаем прерывание INT0, INT1 и INT2
    GICR |= (1 << INT0) | (1 << INT1) | (1 << INT2);
    // Генерация сигнала по возрастающему фронту
    MCUCR |= (1 << ISC00) |
            (1 << ISC01) |
            (1 << ISC10) |
            (1 << ISC11);  
    MCUCSR |= (1 << ISC2);
    GIFR = (1 << INTF0) | (1 << INTF1) | (1 << INTF2);
}

void USART_init(void)
{
    // Установка скорости
    unsigned char speed = F / 16 / BAUD - 1;
    UBRRH = (speed >> 8);
    UBRRL = speed;
    // 8 бит данных, 1 стоп бит, без контроля четности
    UCSRC = (1 << URSEL) | (1 << UCSZ1) | (1 << UCSZ0);
    // Разрешаем передачу
    UCSRB = (1 << TXEN);
}

void game_init()
{
    player_b_min = intervals[interval_index] - 1;
    player_b_sec = SECONDS_IN_MINUTE - 1;
    player_a_min = intervals[interval_index] - 1;
    player_a_sec = SECONDS_IN_MINUTE - 1;
}

// Передача одного байта данных по USART
void USART_transmit(char data)
{
    // Ожидаем пустой буфер приема
    while (!(UCSRA & (1 << UDRE)));
    UDR = data;
}

void USART_print(char* chars) {
  int i = 0;
  while (chars[i] != 0x00) {
    USART_transmit(chars[i++]);
  }
}

void send_to_computer(void)
{
    char buf[256];

    // Выводим на ПЭВМ
    USART_print("Time spent by player A: ");
    if (player_a_minutes < 10) {
        sprintf(buf, "%.2d:", player_a_minutes);
    } else {
        sprintf(buf, "%2d:", player_a_minutes);
    }
    USART_print(buf);
    if (player_a_seconds < 10) {
        sprintf(buf, "%.2d", player_a_seconds);
    } else {
        sprintf(buf, "%2d", player_a_seconds);
    }
    USART_print(buf);
    USART_print("\n\r");

    USART_print("Time spent by player B: ");
    if (player_b_minutes < 10) {
        sprintf(buf, "%.2d:", player_b_minutes);
    } else {
        sprintf(buf, "%2d:", player_b_minutes);
    }
    USART_print(buf);
    if (player_b_seconds < 10) {
        sprintf(buf, "%.2d", player_b_seconds);
    } else {
        sprintf(buf, "%2d", player_b_seconds);
    }
    USART_print(buf);
    USART_print("\n\r");
}

void send_results(void)
{
    player_a_minutes = eeprom_read_byte(&eemem_a_min);
    player_a_seconds = eeprom_read_byte(&eemem_a_sec);
    player_b_minutes = eeprom_read_byte(&eemem_b_min);
    player_b_seconds = eeprom_read_byte(&eemem_b_sec);

    if (chosen_clock != SEND_RESULT) {
        send_to_computer();
    }

    // Гасим ССИ
    PORTC = 0x00;
    _delay_ms(2000);
    // Выводим на ССИ
    player_a_min = player_a_minutes;
    player_a_sec = player_a_seconds;
    player_b_min = player_b_minutes;
    player_b_sec = player_b_seconds;
}

void show_dynamic_time(void)
{
    switch (chosen_clock) {
        case PLAYER_A_CLOCK:
            switch(digit) {
                case 0:
                    PORTC &= ~(1 << 3);
                    PORTC |= 1 << 0;
                    PORTA = seven_segment_digits[(int)(player_a_min / 10)];
                    break;
                case 1:
                    PORTC &= ~(1 << 0);
                    PORTC |= 1 << 1;
                    PORTA = ~(0x80 | ~seven_segment_digits[player_a_min % 10]); // чтобы добавить точку
                    break;
                case 2:
                    PORTC &= ~(1 << 1);
                    PORTC |= 1 << 2;
                    PORTA = seven_segment_digits[(int)(player_a_sec / 10)];
                    break;
                case 3:
                    PORTC &= ~(1 << 2);
                    PORTC |= 1 << 3;
                    PORTA = seven_segment_digits[player_a_sec % 10];
                    break;
                default: break;
            }
            break;
        case PLAYER_B_CLOCK:
            switch(digit) {
                case 0:
                    PORTC &= ~(1 << 7);
                    PORTC |= 1 << 4;
                    PORTA = seven_segment_digits[(int)(player_b_min / 10)];
                    break;
                case 1:
                    PORTC &= ~(1 << 4);
                    PORTC |= 1 << 5;
                    PORTA = ~(0x80 | ~seven_segment_digits[player_b_min % 10]); // чтобы добавить точку
                    break;
                case 2:
                    PORTC &= ~(1 << 5);
                    PORTC |= 1 << 6;
                    PORTA = seven_segment_digits[(int)(player_b_sec / 10)];
                    break;
                case 3:
                    PORTC &= ~(1 << 6);
                    PORTC |= 1 << 7;
                    PORTA = seven_segment_digits[player_b_sec % 10];
                    break;
                default: break;
            }
            break;
        case NO_CLOCK:
            switch(digit) {
                case 0:
                    PORTC &= ~(1 << 3) & ~(1 << 7);
                    PORTC |= (1 << 0) | (1 << 4);
                    PORTA = seven_segment_digits[(int)(intervals[interval_index] / 10)];
                    break;
                case 1:
                    PORTC &= ~(1 << 0) & ~(1 << 4);
                    PORTC |= (1 << 1) | (1 << 5);
                    PORTA = ~(0x80 | ~seven_segment_digits[intervals[interval_index] % 10]); // чтобы добавить точку
                    break;
                case 2:
                    PORTC &= ~(1 << 1) & ~(1 << 5);
                    PORTC |= (1 << 2) | (1 << 6);
                    PORTA = seven_segment_digits[0];
                    break;
                case 3:
                    PORTC &= ~(1 << 2) & ~(1 << 6);
                    PORTC |= (1 << 3) | (1 << 7);
                    PORTA = seven_segment_digits[0];
                    break;
                default: break;
            }
            break;
        default: break;
    }
    
    digit = (digit + 1) % 4;
    if (digit > 3) digit = 0;
}

void show_static_time(void)
{
    if (player_a_show) {
            switch(digit) {
                case 0:
                    PORTC &= ~(1 << 3);
                    PORTC |= 1 << 0;
                    PORTA = seven_segment_digits[(int)(player_a_min / 10)];
                    break;
                case 1:
                    PORTC &= ~(1 << 0);
                    PORTC |= 1 << 1;
                    PORTA = ~(0x80 | ~seven_segment_digits[player_a_min % 10]); // чтобы добавить точку
                    break;
                case 2:
                    PORTC &= ~(1 << 1);
                    PORTC |= 1 << 2;
                    PORTA = seven_segment_digits[(int)(player_a_sec / 10)];
                    break;
                case 3:
                    PORTC &= ~(1 << 2);
                    PORTC |= 1 << 3;
                    PORTA = seven_segment_digits[player_a_sec % 10];
                    break;
                default: break;
            }
    } else {
        switch(digit) {
                case 0:
                    PORTC &= ~(1 << 7);
                    PORTC |= 1 << 4;
                    PORTA = seven_segment_digits[(int)(player_b_min / 10)];
                    break;
                case 1:
                    PORTC &= ~(1 << 4);
                    PORTC |= 1 << 5;
                    PORTA = ~(0x80 | ~seven_segment_digits[player_b_min % 10]); // чтобы добавить точку
                    break;
                case 2:
                    PORTC &= ~(1 << 5);
                    PORTC |= 1 << 6;
                    PORTA = seven_segment_digits[(int)(player_b_sec / 10)];
                    break;
                case 3:
                    PORTC &= ~(1 << 6);
                    PORTC |= 1 << 7;
                    PORTA = seven_segment_digits[player_b_sec % 10];
                    break;
                default: break;
        }
    }
    
    digit = (digit + 1) % 4;
    if (digit > 3) digit = 0;
}

// Обработчик внешнего прерывания INT0
// Игрок А запускает часы игрока Б
ISR(INT0_vect)
{
    // Глобальный запрет прерываний
    cli();

    if (chosen_clock == SEND_RESULT) {
        timer1_init();
        timer0_init();

        player_b_min = intervals[interval_index] - player_b_min - 1;
        player_b_sec = SECONDS_IN_MINUTE - player_b_sec;
    }

    // Гасим ССИ
    PORTC = 0x00;
    _delay_ms(2000);
    
    chosen_clock = PLAYER_B_CLOCK;
    player_a_show = 0;
    digit = 0;

    // Глобальное разрешение прерываний
    sei();
}

// Обработчик внешнего прерывания INT1
// Игрок Б запускает часы игрока А
ISR(INT1_vect)
{
    // Глобальный запрет прерываний
    cli();

    if (chosen_clock == SEND_RESULT) {
        timer1_init();
        timer0_init();

        player_a_min = intervals[interval_index] - player_a_min - 1;
        player_a_sec = SECONDS_IN_MINUTE - player_a_sec;
    }

    // Гасим ССИ
    PORTC = 0x00;
    _delay_ms(2000);

    chosen_clock = PLAYER_A_CLOCK;
    player_a_show = 1;
    digit = 0;

    // Глобальное разрешение прерываний
    sei();
}

ISR(INT2_vect)
{
    send_results();
    chosen_clock = SEND_RESULT;
    player_a_show = !player_a_show;

    // Выключаем таймер
    TIMSK &= (0 << TOIE0);
}

// Управление динамической индикацией по переполнению Т0
ISR(TIMER0_OVF_vect) {
    if (BLINK_ENABLE) {
        if ((!SEC_PASS) && ((chosen_clock == PLAYER_A_CLOCK) || (chosen_clock == PLAYER_B_CLOCK))) {
            PORTC = 0x00;
        } else {
            show_dynamic_time();
        }
    } else {
        show_dynamic_time();
    }
}

// Обработчик прерывания по совпадению
// Срабатывает, когда прошло ПОЛСЕКУНДЫ
ISR (TIMER1_COMPA_vect)
{
    switch (chosen_clock) {
        case PLAYER_A_CLOCK:
            if (SEC_PASS) {
                if ((player_a_min) && (!player_a_sec)) {
                    player_a_min--;
                    player_a_sec = SECONDS_IN_MINUTE - 1;
                    eeprom_write_byte(&eemem_a_min, (int)(intervals[interval_index] - player_a_min - 1));
                    eeprom_write_byte(&eemem_a_sec, (int)(SECONDS_IN_MINUTE - player_a_sec));   
                } else if (player_a_sec) {
                    player_a_sec = --player_a_sec % SECONDS_IN_MINUTE;
                    if ((player_a_sec == BLINK_SECONDS) && (!player_a_min)) {
                        BLINK_ENABLE = 1; // разрешаем мигание
                    }
                    eeprom_write_byte(&eemem_a_min, (int)(intervals[interval_index] - player_a_min - 1));
                    eeprom_write_byte(&eemem_a_sec, (int)(SECONDS_IN_MINUTE - player_a_sec));   
                } else {
                    player_a_min = 0;
                    player_a_sec = 0;
                    BLINK_ENABLE = 0;
                    chosen_clock = FINISH;
                    seconds_spent = 0;
                    eeprom_write_byte(&eemem_a_min, intervals[interval_index]);
                    eeprom_write_byte(&eemem_a_sec, 0);   
                }
            }
            SEC_PASS = !SEC_PASS;
            break;
        case PLAYER_B_CLOCK:
            if (SEC_PASS) {
                if ((player_b_min) && (!player_b_sec)) {
                    player_b_min--;
                    player_b_sec = SECONDS_IN_MINUTE - 1;
                    eeprom_write_byte(&eemem_b_min, (int)(intervals[interval_index] - player_b_min - 1));
                    eeprom_write_byte(&eemem_b_sec, (int)(SECONDS_IN_MINUTE - player_b_sec));   
                } else if (player_b_sec) {
                    player_b_sec = --player_b_sec % SECONDS_IN_MINUTE;
                    if ((player_b_sec == BLINK_SECONDS) && (!player_b_min)) {
                        BLINK_ENABLE = 1; // разрешаем мигание
                    }
                    eeprom_write_byte(&eemem_b_min, (int)(intervals[interval_index] - player_b_min - 1));
                    eeprom_write_byte(&eemem_b_sec, (int)(SECONDS_IN_MINUTE - player_b_sec));   
                } else {
                    player_b_min = 0;
                    player_b_sec = 0;
                    BLINK_ENABLE = 0;
                    chosen_clock = FINISH;
                    seconds_spent = 0;
                    eeprom_write_byte(&eemem_b_min, intervals[interval_index]);
                    eeprom_write_byte(&eemem_b_sec, 0);   
                }
            }
            SEC_PASS = !SEC_PASS;
            break;
        case FINISH:
            if (SEC_PASS) {
                if (seconds_spent < SOUND_SECONDS - 1) {
                    seconds_spent++;
                } else {
                    seconds_spent = 0;
                    chosen_clock = NO_CLOCK;
                }
            }
            SEC_PASS = !SEC_PASS;
            break;
        default: break;
    }
}

// Проигрывание звука на частоте 2кГц в течениии 4с
void play_sound(){
    if (seconds_spent < SOUND_SECONDS - 1) {
        PORTE |= 1 << 2;
        _delay_ms(4);
        PORTE &= 0 << 2;
        _delay_ms(4);
    }
}

void check_mode() 
{
    if (chosen_clock == NO_CLOCK) {
        if (PIND & (1 << PD4)) {
            interval_index = 1;
        } else {
            interval_index = 2;
        }
        game_init();
    }

    switch (chosen_clock) {
            case SEND_RESULT:
                show_static_time();
                _delay_ms(50);
                break;
            case FINISH:
                play_sound();
                break;
            default: break;
    }
}

int main()
{
    ports_init();
    buttons_init();
    USART_init();
    timer1_init();
    timer0_init();
    game_init();

    sei();

    while (1) {
        check_mode();
    }

    return 0;
}