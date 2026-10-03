/* Same chip contracts on original and disjoint remapped wiring. */
#ifndef ALT_MAP
#define ALT_MAP 0
#endif
#define P(n) (((n)+(ALT_MAP?3:0))%49)
static uint8_t mock_address=0x15;
static int mock_dc=P(3);
#if TEST_ID==1 || TEST_ID==2 || TEST_ID==3 || TEST_ID==4
static risc_hw_gpio_bank_v1 fixture={.struct_size=sizeof(fixture),.active_high=TEST_ID!=3,.pull_up=TEST_ID==3,
 .count=TEST_ID==1?6:1,.pins={TEST_ID==1?P(1):TEST_ID==2?P(21):TEST_ID==3?P(41):P(40),P(2),P(41),P(42),P(45),P(46)},
 .debounce_us=30000,.long_press_us=1500000,.click_min_us=30000};
#define COMPAT (TEST_ID==1?"generic,gpio-relay-bank":TEST_ID==2?"generic,pulse-buzzer":TEST_ID==3?"generic,gpio-button-bank":"generic,gpio-led-bank")
#define CONFIG_TYPE "gpio.bank"
#elif TEST_ID==5
static risc_hw_quadrature_v1 fixture={.struct_size=sizeof(fixture),.a=P(45),.b=P(42),.pull_up=1,.edges_per_detent=2,.direction=1,.debounce_us=800,.button_instance_id=1};
#define COMPAT "generic,quadrature-encoder"
#define CONFIG_TYPE "input.quadrature"
#elif TEST_ID==6
static risc_hw_pixel_v1 fixture={.struct_size=sizeof(fixture),.pin=P(48),.count=5,.order=0};
#define COMPAT "protocol,ws2812-800khz"
#define CONFIG_TYPE "pixel.ws2812"
#elif TEST_ID==7 || TEST_ID==8
static risc_hw_spi_display_v1 fixture={.struct_size=sizeof(fixture),
 .bus={.struct_size=sizeof(risc_hw_bus_v1),.kind=RISC_HW_BUS_SPI,.instance_id=ALT_MAP?9:1,.controller=2,.frequency_hz=10000000,.sclk=TEST_ID==7?P(10):P(5),.mosi=TEST_ID==7?P(11):P(4),.miso=TEST_ID==7?-1:P(19),.sda=-1,.scl=-1},
 .width=TEST_ID==7?240:800,.height=TEST_ID==7?240:480,.cs=TEST_ID==7?P(9):P(7),.dc=P(3),.reset=TEST_ID==7?P(14):P(6),.backlight=TEST_ID==7?P(46):-1,.busy=TEST_ID==7?-1:P(2),
 .backlight_active_high=1,.power_count=TEST_ID==7?2:0,.power_pins={P(1),P(2)},.power_active_high={1,1},.reset_assert_ms=10,.reset_recovery_ms=10};
#define COMPAT (TEST_ID==7?"galaxycore,gc9a01":"gooddisplay,gdey075t7")
#define CONFIG_TYPE "display.spi"
#elif TEST_ID==9
static risc_hw_sd_spi_v1 fixture={.struct_size=sizeof(fixture),
 .bus={.struct_size=sizeof(risc_hw_bus_v1),.kind=RISC_HW_BUS_SPI,.instance_id=ALT_MAP?9:1,.controller=2,.frequency_hz=10000000,.sclk=P(5),.mosi=P(4),.miso=P(19),.sda=-1,.scl=-1},
 .cs=P(20),.detect=-1,.write_protect=-1,.write_protect_active_high=1};
#define COMPAT "sd-association,sd-spi"
#define CONFIG_TYPE "storage.sd-spi"
#elif TEST_ID==10
static risc_hw_i2c_touch_v1 fixture={.struct_size=sizeof(fixture),
 .bus={.struct_size=sizeof(risc_hw_bus_v1),.kind=RISC_HW_BUS_I2C,.instance_id=ALT_MAP?9:2,.controller=0,.frequency_hz=400000,.sclk=-1,.mosi=-1,.miso=-1,.sda=P(6),.scl=P(7)},
 .width=240,.height=240,.address=0x15,.reset=P(13),.irq=P(5),.irq_pull_up=1,.reset_assert_ms=10,.reset_recovery_ms=50};
#define COMPAT "hynitron,cst816d"
#define CONFIG_TYPE "touch.i2c"
#else
static risc_hw_radio_v1 fixture={.struct_size=sizeof(fixture),.unit=ALT_MAP?1:0,.features=3};
#define COMPAT "espressif,esp32s3-wifi"
#define CONFIG_TYPE "radio.integrated"
#endif
static risc_hardware_device_v1 mock_hardware={1,sizeof(mock_hardware),ALT_MAP?99:1,COMPAT,"unspecified",CONFIG_TYPE,1,sizeof(fixture),&fixture};
