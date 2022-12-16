#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <libusb.h>

#include "rkcrc.h"
#include "rkrc4.h"

static const char *const strings[2] = { "info", "fatal" };

static void info_and_fatal(const int s, const int cr, char *f, ...) {
    va_list ap;
    va_start(ap,f);
    fprintf(stderr, "%srkflashtool: %s: ", cr ? "\r" : "", strings[s]);
    vfprintf(stderr, f, ap);
    va_end(ap);
    if (s) exit(s);
}

#define info(...)    info_and_fatal(0, 0, __VA_ARGS__)
#define infocr(...)  info_and_fatal(0, 1, __VA_ARGS__)
#define fatal(...)   info_and_fatal(1, 0, __VA_ARGS__)

#define RKFT_BLOCKSIZE      0x4000      /* must be multiple of 512 */
#define RKFT_IDB_DATASIZE   0x200
#define RKFT_IDB_BLOCKSIZE  0x210
#define RKFT_IDB_INCR       0x20
#define RKFT_MEM_INCR       0x80
#define RKFT_OFF_INCR       (RKFT_BLOCKSIZE>>9)
#define MAX_PARAM_LENGTH    (128*512-12) /* cf. MAX_LOADER_PARAM in rkloader */
#define SDRAM_BASE_ADDRESS  0x60000000

#define RKFT_CMD_TESTUNITREADY      0x80000600
#define RKFT_CMD_READFLASHID        0x80000601
#define RKFT_CMD_READFLASHINFO      0x8000061a
#define RKFT_CMD_READCHIPINFO       0x8000061b
#define RKFT_CMD_READEFUSE          0x80000620

#define RKFT_CMD_SETDEVICEINFO      0x00000602
#define RKFT_CMD_ERASESYSTEMDISK    0x00000616
#define RKFT_CMD_SETRESETFLASG      0x0000061e
#define RKFT_CMD_RESETDEVICE        0x000006ff

#define RKFT_CMD_TESTBADBLOCK       0x80000a03
#define RKFT_CMD_READSECTOR         0x80000a04
#define RKFT_CMD_READLBA            0x80000a14
#define RKFT_CMD_READSDRAM          0x80000a17
#define RKFT_CMD_UNKNOWN1           0x80000a21

#define RKFT_CMD_WRITESECTOR        0x00000a05
#define RKFT_CMD_ERASESECTORS       0x00000a06
#define RKFT_CMD_UNKNOWN2           0x00000a0b
#define RKFT_CMD_WRITELBA           0x00000a15
#define RKFT_CMD_WRITESDRAM         0x00000a18
#define RKFT_CMD_EXECUTESDRAM       0x00000a19
#define RKFT_CMD_WRITEEFUSE         0x00000a1f
#define RKFT_CMD_UNKNOWN3           0x00000a22

#define RKFT_CMD_WRITESPARE         0x80001007
#define RKFT_CMD_READSPARE          0x80001008

#define RKFT_CMD_LOWERFORMAT        0x0000001c
#define RKFT_CMD_WRITENKB           0x00000030

#define SETBE16(a, v) do { \
                        ((uint8_t*)a)[1] =  v      & 0xff; \
                        ((uint8_t*)a)[0] = (v>>8 ) & 0xff; \
                      } while(0)

#define SETBE32(a, v) do { \
                        ((uint8_t*)a)[3] =  v      & 0xff; \
                        ((uint8_t*)a)[2] = (v>>8 ) & 0xff; \
                        ((uint8_t*)a)[1] = (v>>16) & 0xff; \
                        ((uint8_t*)a)[0] = (v>>24) & 0xff; \
                      } while(0)

static const struct t_pid {
    const uint16_t pid;
    const char name[8];
} pidtab[] = {
    { 0x281a, "RK2818" },
    { 0x290a, "RK2918" },
    { 0x292a, "RK2928" },
    { 0x292c, "RK3026" },
    { 0x300a, "RK3066" },
    { 0x300b, "RK3168" },
    { 0x301a, "RK3036" },
    { 0x310a, "RK3066B" },
    { 0x310b, "RK3188" },
    { 0x310c, "RK312X" }, // Both RK3126 and RK3128
    { 0x310d, "RK3126" },
    { 0x320a, "RK3288" },
    { 0x320b, "RK322X" }, // Both RK3228 and RK3229
    { 0x320c, "RK3328" },
    { 0x330a, "RK3368" },
    { 0x330c, "RK3399" },
    { 0, "" },
};

typedef struct {
    uint32_t flash_size;
    uint16_t block_size;
    uint8_t page_size;
    uint8_t ecc_bits;
    uint8_t access_time;
    uint8_t manufacturer_id;
    uint8_t chip_select;
} nand_info;

static const char* const manufacturer[] = {   /* NAND Manufacturers */
    "Samsung",
    "Toshiba",
    "Hynix",
    "Infineon",
    "Micron",
    "Renesas",
    "Intel",
    "UNKNOWN", /* Reserved */
    "SanDisk",
};
#define MAX_NAND_ID (sizeof manufacturer / sizeof(char *))

static libusb_context *c;
static libusb_device_handle *h = NULL;
static uint8_t cmd[31], res[13], buf[RKFT_BLOCKSIZE];
static uint8_t ibuf[RKFT_IDB_BLOCKSIZE];
static int tmp;
const struct t_pid *ppid = pidtab;

static void send_reset(uint8_t flag) {
    long int r = random();

    memset(cmd, 0 , 31);
    memcpy(cmd, "USBC", 4);

    SETBE32(cmd+4, r);
    SETBE32(cmd+12, RKFT_CMD_RESETDEVICE);
    cmd[16] = flag;

    libusb_bulk_transfer(h, 2|LIBUSB_ENDPOINT_OUT, cmd, sizeof(cmd), &tmp, 0);
}

static void send_exec(uint32_t krnl_addr, uint32_t parm_addr) {
    long int r = random();

    memset(cmd, 0 , 31);
    memcpy(cmd, "USBC", 4);

    if (r)          SETBE32(cmd+4, r);
    if (krnl_addr)  SETBE32(cmd+17, krnl_addr);
    if (parm_addr)  SETBE32(cmd+22, parm_addr);
    SETBE32(cmd+12, RKFT_CMD_EXECUTESDRAM);

    libusb_bulk_transfer(h, 2|LIBUSB_ENDPOINT_OUT, cmd, sizeof(cmd), &tmp, 0);
}

static void send_cmd(uint32_t command, uint32_t offset, uint16_t nsectors) {
    long int r = random();

    memset(cmd, 0 , 31);
    memcpy(cmd, "USBC", 4);

    if (r)          SETBE32(cmd+4, r);
    if (offset)     SETBE32(cmd+17, offset);
    if (nsectors)   SETBE16(cmd+22, nsectors);
    if (command)    SETBE32(cmd+12, command);

    libusb_bulk_transfer(h, 2|LIBUSB_ENDPOINT_OUT, cmd, sizeof(cmd), &tmp, 0);
}

static void recv_res(void) {
    libusb_bulk_transfer(h, 1|LIBUSB_ENDPOINT_IN, res, sizeof(res), &tmp, 0);
}

static void send_buf(unsigned int s) {
    libusb_bulk_transfer(h, 2|LIBUSB_ENDPOINT_OUT, buf, s, &tmp, 0);
}

static void recv_buf(unsigned int s) {
    libusb_bulk_transfer(h, 1|LIBUSB_ENDPOINT_IN, buf, s, &tmp, 0);
}

static uint8_t connect_usb() {
    /* Initialize libusb */

    if (libusb_init(&c)) return -1; //fatal("cannot init libusb\n");

    libusb_set_debug(c, 3);

    /* Detect connected RockChip device */

    while ( !h && ppid->pid) {	
        h = libusb_open_device_with_vid_pid(c, 0x2207, ppid->pid);
        if (h) {
            info("Detected %s...\n", ppid->name);
            break;
        }
        ppid++;
    }

    if (!h) return -2; //fatal("cannot open device\n");

    /* Connect to device */
    if (libusb_kernel_driver_active(h, 0) == 1) {
        info("kernel driver active\n");
        if (!libusb_detach_kernel_driver(h, 0))
            info("driver detached\n");
    }

    return 0;
}

int sizeOfFile(FILE *fp) {
    int sz = 0;

    fseek(fp, 0L, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0L, SEEK_SET);

    return sz;
}

int load_vendor_code(uint8_t **buffs, char *filename) {
    int size;
    FILE *fp = NULL;
    uint16_t crc16 = 0xffff;

    info("Load %s\n", filename);
    fp = fopen(filename , "r");
    size = sizeOfFile(fp);
    info("Size of file %s - %d\n", filename, size);
    size = ((size % 2048) == 0) ? size :  ((size/2048) + 1) *2048;
    *buffs = malloc(size + 5); //make room for crc
    memset (*buffs, 0, size);
    fread(*buffs, size, 1 , fp);
    rkrc4(*buffs, size);
    crc16 = rkcrc16(crc16, *buffs, size);
    (*buffs)[size++] = crc16 >> 8;
    (*buffs)[size++] = crc16 & 0xff;
    info("crc calculated %04x\n", crc16);

    return size;
}

void send_vendor_code(uint8_t *buffs, int size, int code) {    
    while (size > 4096) {
        libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR, 12, 0, code, buffs, 4096, 0);
            buffs += 4096;
            size -= 4096;
    }
    libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR, 12, 0, code, buffs, size, 0);    
}