#include <neorv32.h>
#include <neorv32_spi.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
#define BAUD_RATE          921600

#define DDR2_BASE          0x88000000UL

#define SPI_PRSC           2
#define SPI_CDIV           0
#define FLASH_CS           0
#define FLASH_PAGE_SIZE    256
#define MODEL_FLASH_OFFSET 0x00100000UL
#define COPY_BUF_WORDS     1024

#define CMD_RDSR  0x05
#define CMD_READ  0x03
#define SR_WIP    0x01

// TM parameters
#define CLASSES          10
#define CLAUSES          2000
#define FEATURES         784
#define THRESHOLD        50
#define INT_SIZE         32
#define LA_CHUNKS        50
#define POS_CHUNKS       25

#define WORDS_PER_CLAUSE  LA_CHUNKS
#define WORDS_PER_CLASS   (CLAUSES * WORDS_PER_CLAUSE)   // 100000 words = 400000 bytes
#define TOTAL_MODEL_WORDS (CLASSES * WORDS_PER_CLASS)
#define TOTAL_MODEL_BYTES (TOTAL_MODEL_WORDS * 4)

// CFU
#define TM_FUNCT3_CHUNK_EVAL    0b000
#define TM_FUNCT3_CHUNK_EVAL_FAST 0b011
#define TM_FUNCT3_CLAUSE_COMMIT 0b001
#define TM_FUNCT3_GET_SCORE     0b010


// CHUNK_EVAL: update internal clause state for one 32-bit chunk
// result is ignored
#define CFU_CHUNK_EVAL(x_bits, pos_mask, neg_mask) \
    neorv32_cfu_r4_instr(          \
        TM_FUNCT3_CHUNK_EVAL,      \
        (uint32_t)(x_bits),        \
        (uint32_t)(pos_mask),      \
        (uint32_t)(neg_mask)       \
    )

// CLAUSE_COMMIT: finalize clause vote, pass clause index for polarity
// result is ignored
#define CFU_CLAUSE_COMMIT(clause_idx) \
    neorv32_cfu_r4_instr(            \
        TM_FUNCT3_CLAUSE_COMMIT,     \
        (uint32_t)(clause_idx),      \
        (uint32_t)0,                           \
        (uint32_t)0                            \
    )
 
// GET_SCORE: return clamped class_sum and reset for next class
#define CFU_GET_SCORE() \
    (int32_t)neorv32_cfu_r4_instr( \
        TM_FUNCT3_GET_SCORE,        \
        (uint32_t)0,                          \
        (uint32_t)0,                          \
        (uint32_t)0                           \
    )

// ---------------------------------------------------------------------------
// Flash
// ---------------------------------------------------------------------------
static inline uint8_t flash_xfer(uint8_t tx)  { return neorv32_spi_transfer(tx); }
static inline void    flash_cs_en(void)        { neorv32_spi_cs_en(FLASH_CS); }
static inline void    flash_cs_dis(void)       { neorv32_spi_cs_dis(); }

static void flash_send_addr(uint32_t addr) {
    flash_xfer((uint8_t)(addr >> 16));
    flash_xfer((uint8_t)(addr >>  8));
    flash_xfer((uint8_t)(addr >>  0));
}

static void flash_read(uint32_t addr, uint8_t *buf, uint32_t len) {
    flash_cs_en();
    flash_xfer(CMD_READ);
    flash_send_addr(addr);
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = flash_xfer(0xFF);
    }
    flash_cs_dis();
}

// ---------------------------------------------------------------------------
// DDR2
// ---------------------------------------------------------------------------
static inline void ddr2_write(uint32_t offset_words, uint32_t value) {
    volatile uint32_t *ptr = (volatile uint32_t*)(DDR2_BASE + offset_words * 4);
    *ptr = value;
}

static inline uint32_t ddr2_read(uint32_t offset_words) {
    volatile uint32_t *ptr = (volatile uint32_t*)(DDR2_BASE + offset_words * 4);
    return *ptr;
}

// ---------------------------------------------------------------------------
// Flash -> DDR2 copy
// ---------------------------------------------------------------------------
static uint8_t copy_buf[COPY_BUF_WORDS * 4];

static void load_model_to_ddr2(void) {
    neorv32_uart0_printf("Loading model from flash to DDR2...\n");
    neorv32_uart0_printf("  Flash offset : 0x%x\n", MODEL_FLASH_OFFSET);
    neorv32_uart0_printf("  DDR2 base    : 0x%x\n", DDR2_BASE);
    neorv32_uart0_printf("  Model size   : %u bytes\n", TOTAL_MODEL_BYTES);

    uint32_t bytes_remaining = TOTAL_MODEL_BYTES;
    uint32_t flash_addr      = MODEL_FLASH_OFFSET;
    uint32_t ddr2_word_off   = 0;
    uint32_t buf_bytes       = COPY_BUF_WORDS * 4;
    uint32_t chunks_total    = (TOTAL_MODEL_BYTES + buf_bytes - 1) / buf_bytes;
    uint32_t chunk_num       = 0;

    while (bytes_remaining > 0) {
        uint32_t chunk = (bytes_remaining >= buf_bytes) ? buf_bytes : bytes_remaining;

        flash_read(flash_addr, copy_buf, chunk);

        uint32_t words = chunk / 4;
        for (uint32_t i = 0; i < words; i++) {
            uint32_t word = (uint32_t)copy_buf[i*4 + 0]
                          | ((uint32_t)copy_buf[i*4 + 1] <<  8)
                          | ((uint32_t)copy_buf[i*4 + 2] << 16)
                          | ((uint32_t)copy_buf[i*4 + 3] << 24);
            ddr2_write(ddr2_word_off + i, word);
        }

        flash_addr      += chunk;
        ddr2_word_off   += words;
        bytes_remaining -= chunk;
        chunk_num++;

        if ((chunk_num % 64) == 0 || chunk_num == chunks_total) {
            neorv32_uart0_printf("  %u / %u KB copied\n",
                                 (chunk_num * buf_bytes) / 1024,
                                 TOTAL_MODEL_BYTES / 1024);
        }
    }

    neorv32_uart0_printf("Model loaded.\n\n");
}

// ---------------------------------------------------------------------------
// DMEM class buffer — one class at a time (400KB)
// Static so it lives in BSS, not the stack
// ---------------------------------------------------------------------------
static uint32_t class_buf[WORDS_PER_CLASS];  // 400000 bytes

static void preload_class(int cls) {
    uint32_t base = (uint32_t)cls * WORDS_PER_CLASS;
    for (uint32_t i = 0; i < WORDS_PER_CLASS; i++) {
        class_buf[i] = ddr2_read(base + i);
    }
}

// ---------------------------------------------------------------------------
// TM inference CFU (reads from class_buf in DMEM)
// preload_cyc and score_cyc are accumulated across all class calls
// ---------------------------------------------------------------------------
static int tm_score(int cls, uint32_t Xi[],
                    uint32_t *preload_cyc, uint32_t *score_cyc) {

    uint32_t t0 = neorv32_cpu_get_cycle();
    preload_class(cls);
    uint32_t t1 = neorv32_cpu_get_cycle();
    *preload_cyc += t1 - t0;

    for (int j = 0; j < CLAUSES; j++) {
        const uint32_t *clause = &class_buf[j * WORDS_PER_CLAUSE];

        for (int k = 0; k < POS_CHUNKS; k++) {
	    if ((clause[k] | clause[POS_CHUNKS + k]) == 0) continue;
        uint32_t chunk_ok = CFU_CHUNK_EVAL(Xi[k], clause[k], clause[POS_CHUNKS + k]);
        if (chunk_ok & 1) break;  // clause already failed, skip remaining chunks
        }

        CFU_CLAUSE_COMMIT(j);
    }

    int class_sum = (int)CFU_GET_SCORE();

    uint32_t t2 = neorv32_cpu_get_cycle();
    *score_cyc += t2 - t1;

    return class_sum;
}

static int predict(uint32_t Xi[],
                   uint32_t *preload_cyc, uint32_t *score_cyc) {
    *preload_cyc = 0;
    *score_cyc   = 0;

    int max_class     = 0;
    int max_class_sum = tm_score(0, Xi, preload_cyc, score_cyc);

    for (int c = 1; c < CLASSES; c++) {
        int s = tm_score(c, Xi, preload_cyc, score_cyc);
        if (s > max_class_sum) {
            max_class_sum = s;
            max_class     = c;
        }
    }

    return max_class;
}

// ---------------------------------------------------------------------------
// UART
// ---------------------------------------------------------------------------
static void uart_recv_bytes(uint8_t *buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = (uint8_t)neorv32_uart0_getc();
    }
}

void delay_ms(uint32_t time_ms) {
  neorv32_aux_delay_ms(neorv32_sysinfo_get_clk(), time_ms);
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
    delay_ms(1000);
    neorv32_uart0_printf("\n=== TM Inference Benchmark (CFU, DMEM preload) ===\n");
    neorv32_uart0_printf("Classes       : %u\n", CLASSES);
    neorv32_uart0_printf("Clauses/class : %u\n", CLAUSES);
    neorv32_uart0_printf("Features      : %u\n", FEATURES);
    neorv32_uart0_printf("Class buf     : %u bytes (DMEM)\n", WORDS_PER_CLASS * 4);

    neorv32_spi_setup(SPI_PRSC, SPI_CDIV, 0, 0);

    // Wait for DDR2 calibration (init_calib_complete on gpio_i[0])
    neorv32_uart0_printf("Waiting for DDR2 calibration...\n");
    while (neorv32_gpio_pin_get(0) == 0) {}
    neorv32_uart0_printf("DDR2 ready.\n");

    load_model_to_ddr2();

    uint8_t  raw_buf[POS_CHUNKS * 4];
    uint32_t Xi[POS_CHUNKS];

    neorv32_uart0_printf("Ready. Send %u bytes per image.\n\n", POS_CHUNKS * 4);


    while (1) {
        uart_recv_bytes(raw_buf, POS_CHUNKS * 4);

        for (int k = 0; k < POS_CHUNKS; k++) {
            Xi[k] = (uint32_t)raw_buf[k*4 + 0]
                  | ((uint32_t)raw_buf[k*4 + 1] <<  8)
                  | ((uint32_t)raw_buf[k*4 + 2] << 16)
                  | ((uint32_t)raw_buf[k*4 + 3] << 24);
        }

        uint32_t t0          = neorv32_cpu_get_cycle();
        uint32_t preload_cyc = 0;
        uint32_t score_cyc   = 0;
        int      pred        = predict(Xi, &preload_cyc, &score_cyc);
        uint32_t t1          = neorv32_cpu_get_cycle();
        uint32_t total_cyc   = t1 - t0;

        neorv32_uart0_putc((char)(pred + 1));
        neorv32_uart0_printf("RESULT %u %u %u %d\n",
                             total_cyc, preload_cyc, score_cyc, pred);
    }

    return 0;
}