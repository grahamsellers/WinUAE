
/* FLASH */

void *flash_new(uae_u8 *rom, int flashsize, int allocsize, uae_u8 devicecode, struct zfile *zf);
void flash_free(void *fdv);

bool flash_write(void *fdv, uaecptr addr, uae_u8 v);
uae_u32 flash_read(void *fdv, uaecptr addr);
bool flash_active(void *fdv, uaecptr addr);
int flash_size(void *fdv);

/* EPROM */

#define BITBANG_I2C_SDA 0
#define BITBANG_I2C_SCL 1

void *eeprom_new(uae_u8 *rom, int size, struct zfile *zf);
void eeprom_free(void *i2c);
void eeprom_reset(void *i2c);
int eeprom_i2c_set(void *i2c, int line, int level);



