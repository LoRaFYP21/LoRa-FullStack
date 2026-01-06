# Windows Virtual Environment Guide

This guide shows how to create and activate a Python virtual environment (venv) on Windows using PowerShell, and how to fix common issues like missing `Scripts` folder or execution policy blocks.

## Create venv with Windows Python

Use the Windows Python (`py` launcher preferred) to create a venv so it has the Windows layout (`Scripts`, `Lib`, `Include`).

```powershell
# Navigate to project root
Set-Location "C:\Users\HP\Desktop\Sem 7\Project\Lilygo\LargeData\LoRa_RealTime_MultiM"

# Remove any mismatched venv (optional, if it was created from Git Bash)
Remove-Item -Recurse -Force .\.venv

# Create venv
py -3 -m venv .venv
# If 'py' is unavailable, use:
# python -m venv .venv
```

## Activate venv in PowerShell

```powershell
.\.venv\Scripts\Activate.ps1
python -V
```

You should see `(.venv)` in your prompt.

## Execution policy block (fix)

If activation is blocked by execution policy, temporarily bypass it for this session:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\.venv\Scripts\Activate.ps1
```

## Git Bash alternative

If your venv was created with a Unix-like layout (`bin`, not `Scripts`), activate it from Git Bash:

```bash
cd "/c/Users/HP/Desktop/Sem 7/Project/Lilygo/LargeData/LoRa_RealTime_MultiM"
source .venv/bin/activate
```

## Common symptoms and causes

- PowerShell error: `source is not recognized`
  - Cause: `source` is a Bash command; use `Activate.ps1` in PowerShell.
- Missing `Scripts` folder
  - Cause: venv created by non-Windows Python (Git Bash/MSYS). Recreate with Windows Python.
- `Activate.ps1` not found
  - Cause: venv not created with Windows Python, or path incorrect. Recreate as above.
- Execution policy prevents script running
  - Fix: `Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass` then activate.

## Deactivate

```powershell
deactivate
```

## Notes

- Keep using PowerShell with a Windows-created venv for consistency.
- If multiple Pythons are installed, prefer `py -3 -m venv .venv` to target Python 3.
