#include <neorv32.h>
#include <neorv32_spi.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
#define BAUD_RATE          921600
#define FLASH_CS           0
#define MODEL_FLASH_OFFSET 0x00100000UL   // 1MB into flash, past bootloader

// SPI clock: prescaler 2 = /8, cdiv 0 -> 6.25 MHz at 100MHz.
#define SPI_PRSC           2
#define SPI_CDIV           0

// Chunk size: how many bytes the host sends before waiting for ACK.
// Must be a multiple of FLASH_PAGE_SIZE (256).
// 4096 = 16 pages per chunk, fits easily in DMEM.
#define CHUNK_SIZE         4096
#define FLASH_PAGE_SIZE    256
#define FLASH_SECTOR_SIZE  (64 * 1024UL)

// Flash commands
#define CMD_WREN  0x06
#define CMD_RDSR  0x05
#define CMD_PP    0x02
#define CMD_SE    0xD8
#define CMD_READ  0x03
#define SR_WIP    0x01

// Protocol bytes
#define ACK  0x79
#define NAK  0x6E

// ---------------------------------------------------------------------------
// Flash helpers
// ---------------------------------------------------------------------------

static inline uint8_t flash_xfer(uint8_t tx)  { return neorv32_spi_transfer(tx); }
static inline void    flash_cs_en(void)        { neorv32_spi_cs_en(FLASH_CS); }
static inline void    flash_cs_dis(void)       { neorv32_spi_cs_dis(); }

static void flash_send_addr(uint32_t addr) {
    flash_xfer((uint8_t)(addr >> 16));
    flash_xfer((uint8_t)(addr >>  8));
    flash_xfer((uint8_t)(addr >>  0));
}

static void flash_wait_ready(void) {
    uint8_t sr;
    do {
        flash_cs_en();
        flash_xfer(CMD_RDSR);
        sr = flash_xfer(0xFF);
        flash_cs_dis();
    } while (sr & SR_WIP);
}

static void flash_write_enable(void) {
    flash_cs_en();
    flash_xfer(CMD_WREN);
    flash_cs_dis();
}

static void flash_erase_sector(uint32_t addr) {
    flash_write_enable();
    flash_cs_en();
    flash_xfer(CMD_SE);
    flash_send_addr(addr);
    flash_cs_dis();
    flash_wait_ready();
}

static void flash_program_page(uint32_t addr, const uint8_t *data) {
    flash_write_enable();
    flash_cs_en();
    flash_xfer(CMD_PP);
    flash_send_addr(addr);
    for (int i = 0; i < FLASH_PAGE_SIZE; i++) {
        flash_xfer(data[i]);
    }
    flash_cs_dis();
    flash_wait_ready();
}

// ---------------------------------------------------------------------------
// UART helpers
// ---------------------------------------------------------------------------

static void uart_recv_bytes(uint8_t *buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = (uint8_t)neorv32_uart0_getc();
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(void) {

    neorv32_rte_setup();
    neorv32_uart0_setup(BAUD_RATE, 0);

    if (!neorv32_uart0_available()) { return 1; }
    if (!neorv32_spi_available()) {
        neorv32_uart0_printf("ERROR: No SPI unit.\n");
        return 1;
    }

    neorv32_spi_setup(SPI_PRSC, SPI_CDIV, 0, 0);

    neorv32_uart0_printf("\n=== TM Flash Writer ===\n");
    neorv32_uart0_printf("Baud  : 921600\n");
    neorv32_uart0_printf("Chunk : %u bytes (%u pages)\n", CHUNK_SIZE, CHUNK_SIZE / FLASH_PAGE_SIZE);
    neorv32_uart0_printf("Waiting for model size...\n");

    uint8_t size_buf[4];
    uart_recv_bytes(size_buf, 4);
    uint32_t model_size = (uint32_t)size_buf[0]
                        | ((uint32_t)size_buf[1] <<  8)
                        | ((uint32_t)size_buf[2] << 16)
                        | ((uint32_t)size_buf[3] << 24);



    if (model_size == 0 || model_size > 0xF00000UL) { // max 15MB (leave room for bootloader)
        // neorv32_uart0_printf("ERROR: Invalid model size.\n");
        neorv32_uart0_putc(NAK);
        return 1;
    }

    uint32_t num_sectors = (model_size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
    // neorv32_uart0_printf("Erasing %u sector(s)...\n", num_sectors);

    for (uint32_t s = 0; s < num_sectors; s++) {
        flash_erase_sector(MODEL_FLASH_OFFSET + s * FLASH_SECTOR_SIZE);
    }


    neorv32_uart0_putc(ACK);

    static uint8_t chunk_buf[CHUNK_SIZE]; // static to avoid stack overflow

    uint32_t bytes_remaining = model_size;
    uint32_t flash_addr      = MODEL_FLASH_OFFSET;
    uint32_t chunk_num       = 0;
    uint32_t total_chunks    = (model_size + CHUNK_SIZE - 1) / CHUNK_SIZE;

    while (bytes_remaining > 0) {

        uint32_t chunk_bytes = (bytes_remaining >= CHUNK_SIZE)
                               ? CHUNK_SIZE
                               : bytes_remaining;

        // Pad last chunk to full page boundary
        uint32_t padded = ((chunk_bytes + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE;
        for (uint32_t i = chunk_bytes; i < padded; i++) {
            chunk_buf[i] = 0xFF;
        }

        // Receive chunk bytes from UART
        uart_recv_bytes(chunk_buf, chunk_bytes);

        // Program chunk as individual pages
        uint32_t pages = padded / FLASH_PAGE_SIZE;
        for (uint32_t p = 0; p < pages; p++) {
            flash_program_page(flash_addr + p * FLASH_PAGE_SIZE,
                               chunk_buf  + p * FLASH_PAGE_SIZE);
        }

        flash_addr      += padded;
        bytes_remaining -= chunk_bytes;
        chunk_num++;

        // ACK this chunk — host sends next chunk only after receiving this
        neorv32_uart0_putc(ACK);

        // Print progress every 16 chunks (~256KB)
        if ((chunk_num % 16) == 0 || chunk_num == total_chunks) {
            // neorv32_uart0_printf("  %u / %u chunks done\n", chunk_num, total_chunks);
        }
    }


    while (1) {}
    return 0;
}