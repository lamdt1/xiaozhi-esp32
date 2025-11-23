# ESP-IDF Windows Installation Troubleshooting Guide

This guide helps fix common errors when installing ESP-IDF on Windows.

## Common Error: Running install.ps1

**Important Note:** ESP-IDF typically uses `install.bat` on Windows, not `install.ps1`. However, if you're using the official ESP-IDF installer or a newer version, the process may differ.

## Solution 1: Use the Correct Installation Method

### Method A: Official ESP-IDF Installer (Recommended)

1. **Download the ESP-IDF Installer:**
   - Go to: https://dl.espressif.com/dl/esp-idf/
   - Download the latest Windows installer (e.g., `esp-idf-tools-setup-online.exe`)

2. **Run the Installer:**
   - Double-click the installer
   - Follow the installation wizard
   - Select ESP-IDF version 5.4 or 5.5.1
   - Choose installation directory (default: `C:\Espressif`)

3. **After Installation:**
   - The installer creates shortcuts in the Start Menu
   - Use "ESP-IDF Command Prompt" or "ESP-IDF PowerShell" to work with ESP-IDF

### Method B: Manual Installation (Git Clone)

If you prefer manual installation:

```powershell
# 1. Install prerequisites first:
#    - Git for Windows: https://git-scm.com/download/win
#    - Python 3.9+: https://www.python.org/downloads/
#    - Make sure to check "Add Python to PATH" during installation

# 2. Open PowerShell (not Command Prompt)
# 3. Navigate to where you want ESP-IDF
cd C:\Espressif\frameworks

# 4. Clone ESP-IDF (if not already cloned)
git clone --recursive https://github.com/espressif/esp-idf.git esp-idf-v5.5.1
cd esp-idf-v5.5.1

# 5. Checkout specific version
git checkout v5.5.1
git submodule update --init --recursive

# 6. Run installation script (use install.bat, not install.ps1)
.\install.bat esp32,esp32s3,esp32c3,esp32p4
```

## Solution 2: Fix PowerShell Execution Policy

If you're getting execution policy errors:

```powershell
# Check current execution policy
Get-ExecutionPolicy

# If it's "Restricted", change it (run PowerShell as Administrator)
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser

# Or for this session only:
Set-ExecutionPolicy -ExecutionPolicy Bypass -Scope Process
```

Then try running the installation again.

## Solution 3: Fix Common Installation Errors

### Error: "install.ps1 cannot be loaded"

**Solution:**
```powershell
# Run PowerShell as Administrator, then:
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser

# Or use install.bat instead:
.\install.bat esp32,esp32s3,esp32c3,esp32p4
```

### Error: "Python not found" or "Python version incompatible"

**Solution:**
1. Install Python 3.9 or newer from https://www.python.org/downloads/
2. **Important:** Check "Add Python to PATH" during installation
3. Verify installation:
```powershell
python --version
# Should show Python 3.9.x or higher
```

4. If Python is installed but not found:
```powershell
# Find Python path
where.exe python

# Add to PATH manually if needed
$env:Path += ";C:\Python39;C:\Python39\Scripts"
```

### Error: "Git not found"

**Solution:**
1. Install Git for Windows: https://git-scm.com/download/win
2. During installation, select "Git from the command line and also from 3rd-party software"
3. Restart PowerShell after installation
4. Verify:
```powershell
git --version
```

### Error: "Failed to download" or network timeout

**Solution:**
```powershell
# Use install.bat with offline mode (if you have the tools)
.\install.bat --offline

# Or set proxy if behind corporate firewall:
$env:HTTP_PROXY="http://proxy.example.com:8080"
$env:HTTPS_PROXY="http://proxy.example.com:8080"

# For Chinese users, you may need VPN or use mirror:
# Set environment variable for component registry mirror
$env:IDF_COMPONENT_REGISTRY_URL="https://api.components.espressif.com/"
```

### Error: "Permission denied" or "Access denied"

**Solution:**
1. Run PowerShell as Administrator:
   - Right-click PowerShell → "Run as Administrator"
2. Or install to a directory you have write access to:
```powershell
# Install to user directory instead
cd $env:USERPROFILE
mkdir esp
cd esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.5.1
.\install.bat esp32,esp32s3,esp32c3,esp32p4
```

### Error: "ResolutionImpossible" or pip dependency conflicts

This error occurs when pip cannot resolve conflicting Python package dependencies.

**Solution 1: Use install.bat instead of install.ps1**
```powershell
cd C:\Espressif\frameworks\esp-idf-v5.5.1
.\install.bat esp32,esp32s3,esp32c3,esp32p4
```

**Solution 2: Clean Python environment and reinstall**
```powershell
# 1. Remove existing virtual environment (if exists)
cd C:\Espressif\frameworks\esp-idf-v5.5.1
if (Test-Path ".espressif") { Remove-Item -Recurse -Force .espressif }

# 2. Upgrade pip, setuptools, and wheel
python -m pip install --upgrade pip setuptools wheel

# 3. Clear pip cache
python -m pip cache purge

# 4. Try installation again
.\install.bat esp32,esp32s3,esp32c3,esp32p4
```

**Solution 3: Use specific Python version (3.9-3.11 recommended)**
```powershell
# Check Python version
python --version

# If you have multiple Python versions, use Python 3.9, 3.10, or 3.11
# ESP-IDF v5.5.1 works best with Python 3.9-3.11
# Avoid Python 3.12+ as it may have compatibility issues

# If needed, install Python 3.11 from python.org
# Then use it explicitly:
py -3.11 -m pip install --upgrade pip
cd C:\Espressif\frameworks\esp-idf-v5.5.1
py -3.11 install.bat esp32,esp32s3,esp32c3,esp32p4
```

**Solution 4: Install with legacy resolver (if conflicts persist)**
```powershell
# Set pip to use legacy resolver
$env:PIP_USE_LEGACY_RESOLVER="1"
.\install.bat esp32,esp32s3,esp32c3,esp32p4
```

**Solution 5: Manual virtual environment setup**
```powershell
cd C:\Espressif\frameworks\esp-idf-v5.5.1

# Create fresh virtual environment
python -m venv .espressif\python_env

# Activate it
.\.espressif\python_env\Scripts\Activate.ps1

# Upgrade pip
python -m pip install --upgrade pip setuptools wheel

# Install requirements manually
python -m pip install -r requirements.txt --no-cache-dir

# Then continue with installation
.\install.bat esp32,esp32s3,esp32c3,esp32p4
```

**Solution 6: Use official installer (bypasses pip issues)**
The official ESP-IDF installer handles Python dependencies automatically:
1. Download from: https://dl.espressif.com/dl/esp-idf/
2. Run `esp-idf-tools-setup-online.exe`
3. Follow the wizard - it manages Python environment automatically

## Solution 4: Alternative Installation via VSCode Extension

The easiest way for beginners:

1. **Install VSCode or Cursor**
2. **Install ESP-IDF Extension:**
   - Open VSCode/Cursor
   - Go to Extensions (Ctrl+Shift+X)
   - Search for "ESP-IDF"
   - Install "Espressif ESP-IDF" extension by Espressif Systems

3. **ESP-IDF Extension will guide you:**
   - Click "Install ESP-IDF" when prompted
   - Select version 5.4 or 5.5.1
   - The extension handles all installation automatically

## Solution 5: Verify Installation

After successful installation, verify:

```powershell
# Navigate to ESP-IDF directory
cd C:\Espressif\frameworks\esp-idf-v5.5.1

# Run export script to set up environment
.\export.ps1

# Or if using Command Prompt:
.\export.bat

# Verify installation
idf.py --version
```

## Solution 6: Set Up Environment Permanently

After installation, you need to set up the environment each time. To make it permanent:

### Option A: Use ESP-IDF Command Prompt/PowerShell

The installer creates shortcuts:
- **Start Menu → ESP-IDF → ESP-IDF Command Prompt**
- **Start Menu → ESP-IDF → ESP-IDF PowerShell**

These automatically set up the environment.

### Option B: Add to PowerShell Profile

```powershell
# Check if profile exists
Test-Path $PROFILE

# If not, create it
New-Item -Path $PROFILE -Type File -Force

# Edit profile
notepad $PROFILE

# Add this line (adjust path to your ESP-IDF):
& "C:\Espressif\frameworks\esp-idf-v5.5.1\export.ps1"
```

Now every PowerShell session will have ESP-IDF available.

## Quick Fix Checklist

If you're still having issues, check:

- [ ] Python 3.9+ installed and in PATH (`python --version`)
- [ ] Git installed and in PATH (`git --version`)
- [ ] PowerShell execution policy allows scripts (`Get-ExecutionPolicy`)
- [ ] Running PowerShell as Administrator (if permission errors)
- [ ] Internet connection working (for downloading tools)
- [ ] Antivirus not blocking the installation
- [ ] Sufficient disk space (ESP-IDF needs ~2-3GB)
- [ ] Using `install.bat` instead of `install.ps1` (if script doesn't exist)

## Manual Installation Steps (Step-by-Step)

If all else fails, here's a complete manual installation:

```powershell
# 1. Install prerequisites (download and install separately):
#    - Git: https://git-scm.com/download/win
#    - Python 3.9+: https://www.python.org/downloads/
#    - CMake: https://cmake.org/download/ (or let ESP-IDF install it)

# 2. Open PowerShell as Administrator
# 3. Set execution policy
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser

# 4. Create directory
mkdir C:\Espressif\frameworks -Force
cd C:\Espressif\frameworks

# 5. Clone ESP-IDF
git clone --recursive https://github.com/espressif/esp-idf.git esp-idf-v5.5.1
cd esp-idf-v5.5.1

# 6. Checkout version
git checkout v5.5.1
git submodule update --init --recursive

# 7. Install tools
.\install.bat esp32,esp32s3,esp32c3,esp32p4

# 8. Set up environment (in new PowerShell window)
cd C:\Espressif\frameworks\esp-idf-v5.5.1
.\export.ps1

# 9. Verify
idf.py --version
```

## Getting Help

If you're still stuck:

1. **Check the error message** - Copy the full error text
2. **Check ESP-IDF documentation**: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/windows-setup.html
3. **ESP-IDF Forum**: https://www.esp32.com/
4. **GitHub Issues**: https://github.com/espressif/esp-idf/issues

## After Successful Installation

Once ESP-IDF is installed, continue with the build process:

1. **Set up environment** (each new terminal):
```powershell
C:\Espressif\frameworks\esp-idf-v5.5.1\export.ps1
```

2. **Navigate to your project:**
```powershell
cd C:\projects\xiaozhi-esp32
```

3. **Follow the [BUILD_GUIDE.md](BUILD_GUIDE.md)** for building firmware

