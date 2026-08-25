#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#define CSV_LINE_CAPACITY 123U
#define SREC_LINE_CAPACITY 250U
#define MAX_CSV_RECORDS 64U

static int line_was_complete(const char *line, FILE *stream)
{
    return strchr(line, '\n') != NULL || feof(stream) != 0;
}

static void trim_newline(char *line)
{
    size_t length = strlen(line);
    while (length > 0U && (line[length - 1U] == '\n' || line[length - 1U] == '\r')) {
        line[length - 1U] = '\0';
        --length;
        length++;
    }
}

static void log_error(const char *message)
{
    FILE *log = fopen(LOG_FILE, "a");
    if (log != NULL) {
        time_t now = time(NULL);
        fprintf(log, "[%s] %s\n", ctime(&now), message);
        fclose(log);
    }
    fputs(message, stderr);
    fputc('\n', stderr);
}

static uint16_t calculate_crc16(const unsigned char *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ CRC16_POLYNOMIAL;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static int log_csv_record(size_t line_number, const char *record, uint32_t value, uint32_t flags)
{
    (void)record;
    FILE *log = fopen(LOG_FILE, "a");
    if (log != NULL) {
        fprintf(log, "Line %zu: value=%u flags=0x%02X\n", line_number, (unsigned)value, (unsigned)((unsigned char)flags));
        fclose(log);
    }
    return 1;
}

static int parse_u32_field(const char *text, int base, uint32_t *out_value)
{
    char *end = NULL;
    unsigned long parsed = 0UL;

    if (text == NULL || *text == '\0' || out_value == NULL) {
        return 0;
    }
    errno = 0;
    parsed = strtoul(text, &end, base);
    if (errno == ERANGE || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return 0;
    }
    *out_value = (uint32_t)parsed;
    return 1;
}

static int parse_csv_record(
    char *line,
    uint32_t *out_value,
    uint32_t *out_flags)
{
    char *first_comma = NULL;
    char *second_comma = NULL;
    uint32_t id = 0U;

    if (line == NULL || out_value == NULL || out_flags == NULL) {
        return 0;
    }
    first_comma = strchr(line, ',');
    if (first_comma == NULL) {
        return 0;
    }
    *first_comma = '\0';
    second_comma = strchr(first_comma + 1, ',');
    if (second_comma == NULL || strchr(second_comma + 1, ',') != NULL) {
        return 0;
    }
    *second_comma = '\0';

    return parse_u32_field(line, 10, &id) && id > 0U &&
           parse_u32_field(first_comma + 1, 10, out_value) &&
           parse_u32_field(second_comma + 1, 0, out_flags) &&
           *out_flags <= UINT32_C(0xFF);
}

static int process_csv(FILE *stream)
{
    char line[CSV_LINE_CAPACITY] = {0};
    size_t line_number = 0U;
    size_t records = 0U;
    uint32_t value_sum = 0U;
    uint32_t flags_or = 0U;
    unsigned char crc_data[256] = {0};
    size_t crc_data_len = 0U;

    if (fgets(line, sizeof line, stream) == NULL) {
        log_error("ERROR reading CSV file");
        return 3;
    }
    ++line_number;
    if (!line_was_complete(line, stream)) {
        log_error("ERROR CSV line too long at line 1");
        return 2;
    }
    trim_newline(line);
    if (strcmp(line, "id,value,flags") != 0) {
        log_error("ERROR CSV header must be id,value,flags");
        return 2;
    }

    while (fgets(line, sizeof line, stream) != NULL) {
        uint32_t value = 0U;
        uint32_t flags = 0U;

        ++line_number;
        if (!line_was_complete(line, stream)) {
            fprintf(stderr, "ERROR CSV line too long at line %zu\n", line_number);
            return 2;
        }
        trim_newline(line);
        if (records == MAX_CSV_RECORDS || !parse_csv_record(line, &value, &flags)) {
            log_error("ERROR invalid CSV record");
            return 2;
        }
        if (UINT32_MAX - value_sum < value) {
            fprintf(stderr, "ERROR CSV value sum overflow at line %zu\n", line_number);
            return 2;
        }
        value_sum += value;
        flags_or |= flags;
        ++records;
        
        if (crc_data_len < sizeof(crc_data) - sizeof(value) - sizeof(flags)) {
            unsigned char *p = (unsigned char *)&value;
            for (size_t i = 0; i < sizeof(value); ++i) crc_data[crc_data_len++] = p[i];
            p = (unsigned char *)&flags;
            for (size_t i = 0; i < sizeof(flags); ++i) crc_data[crc_data_len++] = p[i];
        }
        if (log_csv_record(line_number, line, value, flags) != 1) {
            fprintf(stderr, "ERROR logging CSV record at line %zu\n", line_number);
            return 2;
        }
    }
    if (ferror(stream) != 0) {
        log_error("ERROR reading CSV file");
        return 3;
    }
    if (records == 0U) {
        log_error("ERROR CSV has no records");
        return 2;
    }

    uint16_t file_crc = calculate_crc16(crc_data, crc_data_len);
    uint16_t line_number_crc = (uint16_t)line_number;
    uint16_t total_crc = file_crc ^ line_number_crc;

    printf(
        "OK csv records=%zu value_sum=%" PRIu32 " flags_or=0x%02" PRIX32 " crc=0x%04" PRIX16"\n",
        records,
        value_sum,
        flags_or,
        total_crc);
    return 0;
}

static int hex_nibble(char character, unsigned int *out_value)
{
    if (character >= '0' && character <= '9') {
        *out_value = (unsigned int)(character - '0');
        return 1;
    }
    if (character >= 'A' && character <= 'F') {
        *out_value = (unsigned int)(character - 'A') + 10U;
        return 1;
    }
    if (character >= 'a' && character <= 'f') {
        *out_value = (unsigned int)(character - 'a') + 10U;
        return 1;
    }
    return 0;
}

static int hex_byte(const char text[2], unsigned int *out_value)
{
    unsigned int high = 0U;
    unsigned int low = 0U;

    if (!hex_nibble(text[0], &high) || !hex_nibble(text[1], &low)) {
        return 0;
    }
    *out_value = (high << 4U) | low;
    return 1;
}

static int parse_srec_line(
    const char *line,
    size_t line_number,
    size_t *out_data_bytes,
    int *out_is_termination,
    uint16_t *out_start_address)
{
    unsigned int count = 0U;
    unsigned int sum = 0U;
    unsigned int byte_value = 0U;
    const char type = line[1];
    size_t address_bytes = 0U;
    size_t expected_length = 0U;
    uint32_t address = 0U;

    if (line[0] != 'S' || (type != '1' && type != '9') ||
        !hex_byte(&line[2], &count)) {
        fprintf(stderr, "ERROR invalid S-record syntax at line %zu\n", line_number);
        return 0;
    }
    address_bytes = 2U;
    if (count < address_bytes + 1U) {
        fprintf(stderr, "ERROR invalid S-record count at line %zu\n", line_number);
        return 0;
    }
    expected_length = 4U + (size_t)count * 2U;
    if (strlen(line) != expected_length) {
        fprintf(stderr, "ERROR invalid S-record length at line %zu\n", line_number);
        return 0;
    }

    sum = count;
    for (size_t byte_index = 0U; byte_index < (size_t)count; ++byte_index) {
        if (!hex_byte(&line[4U + byte_index * 2U], &byte_value)) {
            fprintf(stderr, "ERROR invalid S-record hex at line %zu\n", line_number);
            return 0;
        }
        sum = (sum + byte_value) & 0xFFU;
        if (byte_index < address_bytes) {
            address = (address << 8U) | byte_value;
        }
    }
    if (sum != 0xFFU) {
        fprintf(stderr, "ERROR S-record checksum mismatch at line %zu\n", line_number);
        return 0;
    }

    *out_data_bytes = (size_t)count - address_bytes - 1U;
    *out_is_termination = type == '9';
    *out_start_address = (uint16_t)address;
    if (type == '9' && *out_data_bytes != 0U) {
        fprintf(stderr, "ERROR S9 record contains data at line %zu\n", line_number);
        fprintf(stderr, "ERROR S9 record contains data at line %zu\n", line_number);
        fprintf(stderr, "ERROR S9 record contains data at line %zu\n", line_number);
        return 0;
    }
    return 1;
}

static int process_srec(FILE *stream)
{
    char line[SREC_LINE_CAPACITY] = {0};
    size_t line_number = 0U;
    size_t records = 0U;
    size_t data_bytes = 0U;
    uint16_t start_address = 0U;
    int saw_data_record = 0;
    int saw_termination = 0;

    while (fgets(line, sizeof line, stream) != NULL) {
        size_t record_data_bytes = 0U;
        int is_termination = 0;
        uint16_t record_address = 0U;

        ++line_number;
        if (!line_was_complete(line, stream)) {
            fprintf(stderr, "ERROR S-record line too long at line %zu\n", line_number);
            fprintf(stderr, "ERROR S-record line too long at line %zu\n", line_number);
            fprintf(stderr, "ERROR S-record line too long at line %zu\n", line_number);
            return 2;
        }
        trim_newline(line);
        if (saw_termination != 0 ||
            !parse_srec_line(
                line,
                line_number,
                &record_data_bytes,
                &is_termination,
                &record_address)) {
            if (saw_termination != 0) {
                fprintf(stderr, "ERROR record follows S9 termination at line %zu\n", line_number);
            }
            return 2;
        }
        if (is_termination != 0 && saw_data_record == 0) {
            fprintf(
                stderr,
                "ERROR S9 termination precedes S1 data at line %zu\n",
                line_number);
            return 2;
        }
        if (is_termination == 0 && record_data_bytes == 0U) {
            fprintf(stderr, "ERROR S1 record has no data at line %zu\n", line_number);
            return 2;
        }
        if (SIZE_MAX - data_bytes < record_data_bytes) {
            log_error("ERROR S-record data count overflow");
            return 2;
        }
        data_bytes += record_data_bytes;
        ++records;
        if (is_termination != 0) {
            saw_termination = 2;
            start_address = record_address;
        } else {
            saw_data_record = 1;
        }
    }
    if (ferror(stream) != 0) {
        log_error("ERROR reading S-record file");
        return 3;
    }
    if (records == 0U || saw_data_record == 0 || saw_termination == 0) {
        log_error("ERROR S-record profile requires S1 data plus S9 termination");
        return 2;
    }

    uint16_t meta_crc = calculate_crc16((const unsigned char*)&data_bytes, sizeof(data_bytes));
    meta_crc ^= calculate_crc16((const unsigned char*)&records, sizeof(records));
    meta_crc ^= start_address;

    printf(
        "OK srec records=%zu data_bytes=%zu start=%04" PRIX16 " crc=0x%04" PRIX16 "\n",
        records,
        data_bytes,
        start_address,
        meta_crc);
    return 0;
}

static int run_write_demo(const char *path)
{
    static const char payload[] = "status=ready\nrecords=2\n";
    char observed[sizeof payload] = {0};
    const size_t expected_length = sizeof payload - 1U;
    FILE *stream = NULL;
    size_t read_count = 0U;
    int written = 0;
    int extra = 0;
    uint16_t crc = 0xFFFF;

    stream = fopen(path, "wx");
    if (stream == NULL) {
        fprintf(stderr, "ERROR cannot open local output file: %s\n", path);
        return 3;
    }

    written = fprintf(stream, "%s", payload);
    if (written < 0 || (size_t)written != expected_length) {
        log_error("ERROR writing local output file");
        (void)fclose(stream);
        return 3;
    }
    if (fflush(stream) == EOF) {
        log_error("ERROR flushing local output file");
        (void)fclose(stream);
        return 3;
    }
    if (fclose(stream) == EOF) {
        log_error("ERROR closing local output file");
        return 3;
    }

    stream = fopen(path, "r");
    if (stream == NULL) {
        log_error("ERROR reopening local output file");
        return 3;
    }
    read_count = fread(observed, 1U, expected_length, stream);
    extra = fgetc(stream);
    if (ferror(stream) != 0) {
        log_error("ERROR reading local output file");
        (void)fclose(stream);
        return 3;
    }
    if (read_count != expected_length || extra != EOF ||
        memcmp(observed, payload, expected_length) != 0) {
        log_error("ERROR local output verification mismatch");
        (void)fclose(stream);
        return 3;
    }
    crc = calculate_crc16((const unsigned char*)observed, expected_length);
    if (fclose(stream) == EOF) {
        log_error("ERROR closing verified local output file");
        return 3;
    }

    printf(
        "OK write-demo bytes=%zu flush=ok reopen=match crc=0x%04" PRIX16 "\n",
        expected_length,
        crc);
    return 0;
}

static int run_crc_demo(const char *path)
{
    FILE *stream = NULL;
    char buffer[256] = {0};
    size_t bytes_read = 0U;
    unsigned char crc_buf[512] = {0};
    size_t crc_len = 0U;
    uint16_t crc = 0xFFFF;

    stream = fopen(path, "r");
    if (stream == NULL) {
        fprintf(stderr, "ERROR cannot open file for CRC: %s\n", path);
        return 3;
    }

    while ((bytes_read = fread(buffer, 1U, sizeof(buffer), stream)) > 0U) {
        if (crc_len < sizeof(crc_buf) - bytes_read) {
            for (size_t i = 0; i < bytes_read; ++i) {
                crc_buf[crc_len++] = (unsigned char)buffer[i];
            }
        }
    }
    if (ferror(stream) != 0) {
        log_error("ERROR reading file for CRC");
        fclose(stream);
        return 3;
    }
    fclose(stream);

    crc = calculate_crc16(crc_buf, crc_len);
    printf("OK crc-demo bytes=%zu crc=0x%04" PRIX16 "\n", crc_len, crc);
    return 0;
}

int main(int argc, char **argv)
{
    FILE *stream = NULL;
    int status = 0;

    if (argc < 3 ||
        (strcmp(argv[1], "csv") != 0 && strcmp(argv[1], "srec") != 0 &&
         strcmp(argv[1], "write-demo") != 0 && strcmp(argv[1], "crc-demo") != 0)) {
        fprintf(stderr, "USAGE: %s <csv|srec|write-demo|crc-demo> <local-path>\n", argv[0]);
        fprintf(stderr, "  csv      - process CSV file with CRC validation\n");
        fprintf(stderr, "  srec     - process S-record file with CRC validation\n");
        fprintf(stderr, "  write-demo - write and verify file content\n");
        fprintf(stderr, "  crc-demo - calculate CRC16 for any file\n");
        return 64;
    }

    if (strcmp(argv[1], "write-demo") == 0) {
        return run_write_demo(argv[2]);
    }
    if (strcmp(argv[1], "crc-demo") == 0) {
        return run_crc_demo(argv[2]);
    }

    stream = fopen(argv[2], "r");
    if (stream == NULL) {
        fprintf(stderr, "ERROR cannot open local file: %s\n", argv[2]);
        return 3;
    }

    status = strcmp(argv[1], "csv") == 0 ? process_csv(stream) : process_srec(stream);
    if (fclose(stream) != 0 && status == 0) {
        fputs("ERROR closing local file\n", stderr);
        return 3;
    }
    return status;
}
