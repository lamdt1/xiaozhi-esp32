# Firmware Backup Guide

This guide explains how to backup your firmware before flashing a new version. **Always backup before flashing** to ensure you can restore your device if something goes wrong.

## Prerequisites

1. **ESP-IDF environment** set up (see [BUILD_GUIDE.md](BUILD_GUIDE.md))
2. **esptool.py** (included with ESP-IDF)
3. **USB connection** to your ESP32 device
4. **Serial port** identified (COM port on Windows, /dev/ttyUSB0 or /dev/ttyACM0 on Linux/macOS)

## Finding Your Serial Port

**Windows:**
```powershell
# Check Device Manager or use:
Get-PnPDevice -Class Ports | Where-Object {$_.FriendlyName -like "*USB*" -or $_.FriendlyName -like "*Serial*"}
```

**Linux:**
```bash
ls /dev/ttyUSB* /dev/ttyACM*
# Or check dmesg after connecting:
dmesg | tail
```

**macOS:**
```bash
ls /dev/cu.usbserial* /dev/cu.SLAB*
```

## Backup Methods

### Method 1: Full Flash Backup (Recommended)

Backup the entire flash memory. This is the safest option as it preserves everything.

#### Step 1: Determine Flash Size

Check your device's flash size. Common sizes:
- **4MB** (0x400000 bytes)
- **8MB** (0x800000 bytes)
- **16MB** (0x1000000 bytes)
- **32MB** (0x2000000 bytes)

You can find this in:
- Device documentation
- `sdkconfig` file: `CONFIG_ESPTOOLPY_FLASHSIZE`
- Or try reading and check the actual size

#### Step 2: Backup Full Flash

**For ESP32-S3 (16MB flash example):**
```bash
# Windows
esptool.py --chip esp32s3 -p COM3 --baud 921600 read_flash 0x0 0x1000000 backup_full_16mb.bin

# Linux
esptool.py --chip esp32s3 -p /dev/ttyUSB0 --baud 921600 read_flash 0x0 0x1000000 backup_full_16mb.bin

# macOS
esptool.py --chip esp32s3 -p /dev/cu.usbserial-* --baud 921600 read_flash 0x0 0x1000000 backup_full_16mb.bin
```

**For ESP32-C3 (16MB flash example):**
```bash
esptool.py --chip esp32c3 -p COM3 --baud 921600 read_flash 0x0 0x1000000 backup_full_16mb.bin
```

**For ESP32 (4MB flash example):**
```bash
esptool.py --chip esp32 -p COM3 --baud 921600 read_flash 0x0 0x400000 backup_full_4mb.bin
```

**For ESP32-P4 (16MB flash example):**
```bash
esptool.py --chip esp32p4 -p COM3 --baud 921600 read_flash 0x0 0x1000000 backup_full_16mb.bin
```

**Parameters explained:**
- `--chip`: Your ESP32 chip type (esp32, esp32s3, esp32c3, esp32p4)
- `-p`: Serial port (COM3 on Windows, /dev/ttyUSB0 on Linux)
- `--baud`: Baud rate (921600 is fast, use 115200 if you have issues)
- `read_flash`: Command to read flash
- `0x0`: Start address (beginning of flash)
- `0x1000000`: Size to read (16MB = 16,777,216 bytes = 0x1000000)
- `backup_full_16mb.bin`: Output filename

#### Step 3: Verify Backup

```bash
# Check file size (should match flash size)
# Windows PowerShell
(Get-Item backup_full_16mb.bin).Length

# Linux/macOS
ls -lh backup_full_16mb.bin
```

### Method 2: Partition-Based Backup

Backup specific partitions. Useful when you only need to preserve certain data.

#### Understanding Partition Layout

Based on v2 partition table (16MB example):

| Partition | Offset | Size | Description |
|-----------|--------|------|-------------|
| Bootloader | 0x0 | ~28KB | Bootloader |
| Partition Table | 0x8000 | 3KB | Partition table |
| NVS | 0x9000 | 16KB | Non-volatile storage (WiFi credentials, settings) |
| OTA Data | 0xd000 | 8KB | OTA update metadata |
| PHY Init | 0xf000 | 4KB | PHY initialization data |
| OTA_0 | 0x20000 | 4MB | Application partition 0 |
| OTA_1 | 0x420000 | 4MB | Application partition 1 |
| Assets | 0x800000 | 8MB | Assets partition |

#### Backup Critical Partitions

**1. Backup NVS (WiFi credentials, settings):**
```bash
# ESP32-S3
esptool.py --chip esp32s3 -p COM3 read_flash 0x9000 0x4000 nvs_backup.bin

# ESP32-C3
esptool.py --chip esp32c3 -p COM3 read_flash 0x9000 0x4000 nvs_backup.bin
```

**2. Backup OTA Data:**
```bash
esptool.py --chip esp32s3 -p COM3 read_flash 0xd000 0x2000 otadata_backup.bin
```

**3. Backup Active Application Partition:**

First, check which partition is active, then backup:

```bash
# Backup OTA_0 (4MB = 0x400000 bytes)
esptool.py --chip esp32s3 -p COM3 read_flash 0x20000 0x400000 ota_0_backup.bin

# Backup OTA_1 (4MB = 0x400000 bytes)
esptool.py --chip esp32s3 -p COM3 read_flash 0x420000 0x400000 ota_1_backup.bin
```

**4. Backup Assets Partition (if needed):**
```bash
# 8MB assets partition
esptool.py --chip esp32s3 -p COM3 read_flash 0x800000 0x800000 assets_backup.bin
```

**5. Backup Bootloader:**
```bash
esptool.py --chip esp32s3 -p COM3 read_flash 0x0 0x8000 bootloader_backup.bin
```

**6. Backup Partition Table:**
```bash
esptool.py --chip esp32s3 -p COM3 read_flash 0x8000 0x1000 partition_table_backup.bin
```

#### Special Cases: Factory Information

For devices like **SenseCAP Watcher** or **AiPi-Lite** that have factory information:

**SenseCAP Watcher (32MB flash):**
```bash
esptool.py --chip esp32s3 --baud 2000000 --before default_reset --after hard_reset --no-stub read_flash 0x9000 204800 nvsfactory_backup.bin
```

**AiPi-Lite:**
```bash
esptool.py --chip esp32s3 --baud 2000000 --before default_reset --after hard_reset --no-stub read_flash 0x9000 16384 nvsfactory_backup.bin
```

### Method 3: Using idf.py (Limited)

ESP-IDF's `idf.py` doesn't have a direct backup command, but you can use esptool through it:

```bash
# Get esptool path
idf.py --version

# Then use esptool directly (see Method 1 or 2)
```

## Backup Script (Automated)

Create a backup script for convenience:

**Windows (backup.bat):**
```batch
@echo off
set CHIP=esp32s3
set PORT=COM3
set FLASH_SIZE=0x1000000
set BACKUP_DIR=backups
set TIMESTAMP=%date:~-4,4%%date:~-7,2%%date:~-10,2%_%time:~0,2%%time:~3,2%%time:~6,2%
set TIMESTAMP=%TIMESTAMP: =0%

mkdir %BACKUP_DIR% 2>nul

echo Backing up full flash...
esptool.py --chip %CHIP% -p %PORT% --baud 921600 read_flash 0x0 %FLASH_SIZE% %BACKUP_DIR%\backup_full_%TIMESTAMP%.bin

echo Backup complete: %BACKUP_DIR%\backup_full_%TIMESTAMP%.bin
pause
```

**Linux/macOS (backup.sh):**
```bash
#!/bin/bash

CHIP="esp32s3"
PORT="/dev/ttyUSB0"
FLASH_SIZE="0x1000000"
BACKUP_DIR="backups"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p "$BACKUP_DIR"

echo "Backing up full flash..."
esptool.py --chip "$CHIP" -p "$PORT" --baud 921600 read_flash 0x0 "$FLASH_SIZE" "$BACKUP_DIR/backup_full_$TIMESTAMP.bin"

echo "Backup complete: $BACKUP_DIR/backup_full_$TIMESTAMP.bin"
```

Make it executable:
```bash
chmod +x backup.sh
./backup.sh
```

## Restoring from Backup

### Restore Full Flash

**⚠️ WARNING: This will overwrite everything on your device!**

```bash
# Restore full backup
esptool.py --chip esp32s3 -p COM3 --baud 921600 write_flash 0x0 backup_full_16mb.bin

# Verify after writing
esptool.py --chip esp32s3 -p COM3 verify_flash 0x0 backup_full_16mb.bin
```

### Restore Specific Partitions

**Restore NVS:**
```bash
esptool.py --chip esp32s3 -p COM3 write_flash 0x9000 nvs_backup.bin
```

**Restore Application:**
```bash
esptool.py --chip esp32s3 -p COM3 write_flash 0x20000 ota_0_backup.bin
```

**Restore Factory Information (SenseCAP/AiPi-Lite):**
```bash
# SenseCAP Watcher
esptool.py --chip esp32s3 --baud 2000000 --before default_reset --after hard_reset --no-stub write_flash 0x9000 nvsfactory_backup.bin

# AiPi-Lite
esptool.py --chip esp32s3 --baud 2000000 --before default_reset --after hard_reset --no-stub write_flash 0x9000 nvsfactory_backup.bin
```

## Best Practices

### 1. Always Backup Before Flashing
```bash
# Quick backup before any flash operation
esptool.py --chip esp32s3 -p COM3 read_flash 0x0 0x1000000 backup_before_flash_$(date +%Y%m%d_%H%M%S).bin
```

### 2. Store Backups Safely
- Keep backups in a separate directory
- Name them with timestamps
- Store important backups in cloud storage
- Document what firmware version each backup contains

### 3. Verify Backups
```bash
# Check backup file integrity
# Windows PowerShell
Get-FileHash backup_full_16mb.bin -Algorithm SHA256

# Linux/macOS
sha256sum backup_full_16mb.bin
```

### 4. Test Restore Process
Before you need it, test restoring to ensure your backup works:
- Use a test device if possible
- Or backup, flash new firmware, then restore to verify

## Troubleshooting

### Issue: "Failed to connect to device"
**Solutions:**
- Check USB cable (use data cable, not charge-only)
- Install USB-to-Serial drivers (CH340, CP2102, CP210x)
- Try different USB port
- Press and hold BOOT button while connecting
- Lower baud rate: `--baud 115200`

### Issue: "Read timeout"
**Solutions:**
- Lower baud rate
- Check USB connection stability
- Try different USB cable/port
- Disable other programs using the serial port

### Issue: "Permission denied" (Linux)
**Solutions:**
```bash
sudo usermod -a -G dialout $USER
# Log out and log back in
# Or use sudo (not recommended)
```

### Issue: Backup file is corrupted
**Solutions:**
- Re-read the flash
- Use lower baud rate
- Check flash size matches actual device
- Verify checksums after backup

## Quick Reference

### Common Flash Sizes
```
4MB  = 0x400000  = 4,194,304 bytes
8MB  = 0x800000  = 8,388,608 bytes
16MB = 0x1000000 = 16,777,216 bytes
32MB = 0x2000000 = 33,554,432 bytes
```

### Common Chip Types
- `esp32` - Original ESP32
- `esp32s3` - ESP32-S3 (most common)
- `esp32c3` - ESP32-C3
- `esp32p4` - ESP32-P4

### Quick Backup Commands

**Full backup (16MB ESP32-S3):**
```bash
esptool.py --chip esp32s3 -p COM3 --baud 921600 read_flash 0x0 0x1000000 backup.bin
```

**Critical partitions only:**
```bash
esptool.py --chip esp32s3 -p COM3 read_flash 0x9000 0x4000 nvs.bin
esptool.py --chip esp32s3 -p COM3 read_flash 0xd000 0x2000 otadata.bin
esptool.py --chip esp32s3 -p COM3 read_flash 0x20000 0x400000 app.bin
```

## Related Documentation

- [BUILD_GUIDE.md](BUILD_GUIDE.md) - How to build firmware
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/tools/idf-tools.html#esptool-py)
- [esptool.py Documentation](https://docs.espressif.com/projects/esptool/en/latest/esp32/esptool/index.html)

