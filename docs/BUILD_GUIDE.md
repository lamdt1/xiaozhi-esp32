# Firmware Build Guide - Step by Step

This guide will walk you through building the xiaozhi-esp32 firmware from scratch.

## Prerequisites

### 1. Install ESP-IDF (Version 5.4 or above)

**For Windows:**
- **Recommended:** Use VSCode with the ESP-IDF extension (easiest for beginners)
- **Alternative 1:** Install ESP-IDF using the official installer from [Espressif's website](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/windows-setup.html)
- **Alternative 2:** Manual installation via Git (see [ESP_IDF_WINDOWS_INSTALL.md](ESP_IDF_WINDOWS_INSTALL.md) for detailed steps)

**If you encounter installation errors, see [ESP_IDF_WINDOWS_INSTALL.md](ESP_IDF_WINDOWS_INSTALL.md) for troubleshooting.**

**For Linux:**
```bash
# Install prerequisites
sudo apt-get update
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

# Clone ESP-IDF
mkdir -p ~/esp
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.4

# Install ESP-IDF
./install.sh esp32,esp32s3,esp32c3,esp32p4

# Set up environment (add to ~/.bashrc)
alias get_idf='. $HOME/esp/esp-idf/export.sh'
```

**For macOS:**
```bash
# Install prerequisites
brew install cmake ninja dfu-util python3

# Clone ESP-IDF
mkdir -p ~/esp
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.4

# Install ESP-IDF
./install.sh esp32,esp32s3,esp32c3,esp32p4

# Set up environment (add to ~/.zshrc or ~/.bash_profile)
alias get_idf='. $HOME/esp/esp-idf/export.sh'
```

### 2. Install Development Tools

- **VSCode or Cursor** (recommended)
- **ESP-IDF Extension** for VSCode/Cursor
- **Python 3.8+** (required for build scripts)

## Build Steps

### Step 1: Clone and Navigate to Project

```bash
# If you haven't already cloned the repository
git clone https://github.com/78/xiaozhi-esp32.git
cd xiaozhi-esp32
```

### Step 2: Set Up ESP-IDF Environment

**In a new terminal/PowerShell:**

**Windows (PowerShell):**
```powershell
# If using ESP-IDF installer, run:
C:\Espressif\frameworks\esp-idf-v5.5.1\export.ps1

# Or if using Command Prompt:
C:\Espressif\frameworks\esp-idf-v5.5.1\export.bat

# Note: Adjust the version number (v5.5.1) to match your installed version
```

**Linux/macOS:**
```bash
# Activate ESP-IDF environment
get_idf
# Or manually:
. ~/esp/esp-idf/export.sh
```

**Verify ESP-IDF is set up:**
```bash
idf.py --version
```

### Step 3: Set Target Chip

Choose your target chip based on your hardware:

**For ESP32-S3 (most common):**
```bash
idf.py set-target esp32s3
```

**For ESP32-C3:**
```bash
idf.py set-target esp32c3
```

**For ESP32:**
```bash
idf.py set-target esp32
```

**For ESP32-P4:**
```bash
idf.py set-target esp32p4
```

### Step 4: Configure Board Type

Open the configuration menu:

```bash
idf.py menuconfig
```

Navigate to:
```
Xiaozhi Assistant → Board Type
```

Select your board type. Common options include:
- `bread-compact-wifi` - Breadboard compact WiFi version
- `m5stack-core-s3` - M5Stack CoreS3
- `atoms3r-echo-base` - M5Stack AtomS3R + Echo Base
- `esp-box-3` - ESP32-S3-BOX3
- `xmini-c3` - XMini C3
- And many more...

**Note:** The exact board name depends on your hardware. Check the `main/boards/` directory for available boards.

### Step 5: Configure Additional Settings (Optional)

While in `menuconfig`, you can also configure:

**Language Settings:**
```
Xiaozhi Assistant → Language
```
Options: Chinese (Simplified), Chinese (Traditional), English, Japanese, Korean, etc.

**Partition Table:**
```
Partition Table → Partition Table
```
Choose appropriate partition table based on your flash size (4MB, 8MB, 16MB, 32MB)

**Assets Configuration:**
```
Xiaozhi Assistant → Assets Configuration
```
- `Flash default assets` - Use built-in assets
- `Flash custom assets` - Use custom assets file
- `Flash none assets` - Skip assets flashing

**Save and exit** menuconfig (press `S` to save, `Q` to quit)

### Step 6: Build the Firmware

```bash
idf.py build
```

This will:
1. Download required components (via IDF Component Manager)
2. Generate language configuration files
3. Build default assets (if configured)
4. Compile all source files
5. Link everything into a firmware binary

**Build output location:**
- Firmware binary: `build/xiaozhi.bin`
- Bootloader: `build/bootloader/bootloader.bin`
- Partition table: `build/partition_table/partition-table.bin`

**Troubleshooting build issues:**
- If component download fails, check your internet connection
- For Chinese users, you may need to use a VPN or mirror source
- Clean build: `idf.py fullclean` then `idf.py build`

### Step 7: Generate Merged Binary (Optional)

To create a single binary file for easy flashing:

```bash
idf.py merge-bin
```

This creates `build/xiaozhi-merged.bin` which contains bootloader, partition table, and app in one file.

### Step 8: Flash the Firmware (Optional)

**⚠️ IMPORTANT: Backup your current firmware before flashing!**

See [BACKUP_GUIDE.md](BACKUP_GUIDE.md) for detailed backup instructions. Quick backup:

```bash
# Backup full flash (16MB example - adjust size for your device)
esptool.py --chip esp32s3 -p COM3 --baud 921600 read_flash 0x0 0x1000000 backup.bin
```

**Connect your ESP32 device via USB**

**Flash firmware:**
```bash
idf.py flash
```

**Flash and monitor (recommended):**
```bash
idf.py flash monitor
```

This will:
1. Flash the firmware to your device
2. Open a serial monitor to view logs

**If you need to specify the port manually:**
```bash
idf.py -p COM3 flash monitor  # Windows (replace COM3 with your port)
idf.py -p /dev/ttyUSB0 flash monitor  # Linux
idf.py -p /dev/cu.usbserial-* flash monitor  # macOS
```

## Quick Reference Commands

```bash
# Set target chip
idf.py set-target <esp32|esp32s3|esp32c3|esp32p4>

# Configure project
idf.py menuconfig

# Build firmware
idf.py build

# Clean build
idf.py fullclean

# Flash firmware
idf.py flash

# Monitor serial output
idf.py monitor

# Flash and monitor together
idf.py flash monitor

# Generate merged binary
idf.py merge-bin

# Build, flash, and monitor in one command
idf.py build flash monitor
```

## Using VSCode/Cursor (Recommended for Beginners)

1. **Open the project** in VSCode/Cursor
2. **Install ESP-IDF Extension** if not already installed
3. **Set ESP-IDF version** to 5.4 or above (bottom right of VSCode)
4. **Select target chip** (bottom status bar: `ESP-IDF: Set Espressif device target`)
5. **Open SDK Configuration Editor** (Command Palette: `ESP-IDF: SDK Configuration editor (menuconfig)`)
6. **Select your board type** under `Xiaozhi Assistant → Board Type`
7. **Build** using the build button or `ESP-IDF: Build your project`
8. **Flash** using `ESP-IDF: Flash your project`

## Common Issues and Solutions

### Issue: Component download fails
**Solution:** 
- Check internet connection
- Use VPN if in China
- Manually download components from [ESP Component Registry](https://components.espressif.com/)

### Issue: Build errors related to missing files
**Solution:**
```bash
idf.py fullclean
idf.py build
```

### Issue: Port not found
**Solution:**
- Install USB-to-Serial drivers (CH340, CP2102, etc.)
- Check device manager (Windows) or `ls /dev/tty*` (Linux/macOS)
- Try different USB cable/port

### Issue: Permission denied (Linux)
**Solution:**
```bash
sudo usermod -a -G dialout $USER
# Log out and log back in
```

## Next Steps

After building successfully:
- Check the [README.md](README.md) for usage instructions
- Review [docs/custom-board.md](docs/custom-board.md) if creating a custom board
- See [docs/mcp-usage.md](docs/mcp-usage.md) for MCP protocol usage

## Additional Resources

- [BACKUP_GUIDE.md](BACKUP_GUIDE.md) - **How to backup firmware before flashing**
- [ESP_IDF_WINDOWS_INSTALL.md](ESP_IDF_WINDOWS_INSTALL.md) - **ESP-IDF Windows installation troubleshooting**
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/index.html)
- [Xiaozhi Official Website](https://xiaozhi.me)
- [Project GitHub Repository](https://github.com/78/xiaozhi-esp32)

