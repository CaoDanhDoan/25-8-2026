# B10 File Demo - Hướng dẫn sử dụng

## Tổng quan
Chương trình xử lý file CSV, S-record với tính năng CRC validation và logging.

## Các chế độ hoạt động

### 1. CSV Mode
Xử lý file CSV có định dạng `id,value,flags`:
```
./b10_file_demo csv input.csv
```
- Output: `OK csv records=N value_sum=V flags_or=F crc=0xCCCC`

### 2. SREC Mode
Xử lý file S-record Motorola:
```
./b10_file_demo srec input.srec
```
- Output: `OK srec records=N data_bytes=B start=AAAA crc=0xCCCC`

### 3. Write-Demo Mode
Ghi và xác thực nội dung file:
```
./b10_file_demo write-demo output.txt
```
- Output: `OK write-demo bytes=N flush=ok reopen=match crc=0xCCCC`

### 4. CRC-Demo Mode (MỚI)
Tính CRC16 cho bất kỳ file nào:
```
./b10_file_demo crc-demo any_file.bin
```
- Output: `OK crc-demo bytes=N crc=0xCCCC`

## Tính năng mới (so với bản gốc)

### Logging
- Lỗi được ghi ra file `processing.log` kèm timestamp
- Log chi tiết từng bản ghi CSV đã xử lý

### CRC-16 Validation
- Tính toán CRC-16 CCITT cho dữ liệu xử lý
- Mã CRC được thêm vào output cuối cùng

## Build
```bash
gcc -Wall -Wextra -Wpedantic -std=c11 -o b10_file_demo b10_file_demo.c
```

## File output
- `processing.log` - File log ghi lại hoạt động và lỗi
