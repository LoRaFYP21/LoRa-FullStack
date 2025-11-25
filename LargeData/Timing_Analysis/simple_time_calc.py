#!/usr/bin/env python3
"""
Simple Time Difference Calculator for LoRa Timing Analysis

Quick utility to calculate time differences from command line arguments.
Usage: python simple_time_calc.py "time1" "time2"

Examples:
  python simple_time_calc.py "2025-10-06_19:45:15.234" "2025-10-06_19:45:16.789"
  python simple_time_calc.py "15234" "16789"
  python simple_time_calc.py "Day00_19:45:15.234" "Day00_19:45:16.789"
"""

import sys
import re

def parse_timestamp(timestamp_str):
    """Parse timestamp and return milliseconds"""
    timestamp_str = timestamp_str.strip()
    
    # Real-world time: 2025-10-06_19:45:15.234
    match = re.match(r'(\d{4})-(\d{2})-(\d{2})_(\d{2}):(\d{2}):(\d{2})\.(\d{3})', timestamp_str)
    if match:
        year, month, day, hour, minute, second, ms = map(int, match.groups())
        from datetime import datetime
        dt = datetime(year, month, day, hour, minute, second, ms * 1000)
        return int(dt.timestamp() * 1000)
    
    # Relative time: Day00_19:45:15.234
    match = re.match(r'Day(\d{2})_(\d{2}):(\d{2}):(\d{2})\.(\d{3})', timestamp_str)
    if match:
        days, hour, minute, second, ms = map(int, match.groups())
        return (days * 24 * 60 * 60 * 1000 + 
                hour * 60 * 60 * 1000 + 
                minute * 60 * 1000 + 
                second * 1000 + 
                ms)
    
    # Milliseconds: 15234
    if re.match(r'^\d+$', timestamp_str):
        return int(timestamp_str)
    
    raise ValueError(f"Unknown format: {timestamp_str}")

def format_duration(ms):
    """Format milliseconds to readable duration"""
    if ms < 0:
        sign = "-"
        ms = abs(ms)
    else:
        sign = ""
    
    if ms < 1000:
        return f"{sign}{ms} ms"
    elif ms < 60000:
        return f"{sign}{ms/1000:.3f} seconds"
    elif ms < 3600000:
        minutes = ms // 60000
        seconds = (ms % 60000) / 1000
        return f"{sign}{minutes}m {seconds:.3f}s"
    else:
        hours = ms // 3600000
        minutes = (ms % 3600000) // 60000
        seconds = (ms % 60000) / 1000
        return f"{sign}{hours}h {minutes}m {seconds:.3f}s"

def main():
    if len(sys.argv) != 3:
        print("Usage: python simple_time_calc.py \"time1\" \"time2\"")
        print("\nSupported formats:")
        print("  Real-world: 2025-10-06_19:45:15.234")
        print("  Relative:   Day00_19:45:15.234")
        print("  Milliseconds: 15234")
        print("\nExamples:")
        print("  python simple_time_calc.py \"15234\" \"16789\"")
        print("  python simple_time_calc.py \"2025-10-06_19:45:15.234\" \"2025-10-06_19:45:16.789\"")
        sys.exit(1)
    
    time1_str = sys.argv[1]
    time2_str = sys.argv[2]
    
    try:
        time1_ms = parse_timestamp(time1_str)
        time2_ms = parse_timestamp(time2_str)
        
        difference = time2_ms - time1_ms
        
        print(f"Time 1: {time1_str} → {time1_ms:,} ms")
        print(f"Time 2: {time2_str} → {time2_ms:,} ms")
        print(f"Difference: {difference:,} ms ({format_duration(difference)})")
        
        if difference < 0:
            print("Note: Time 2 is earlier than Time 1")
            
    except ValueError as e:
        print(f"Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()