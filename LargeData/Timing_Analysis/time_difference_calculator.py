#!/usr/bin/env python3
"""
LoRa Timing Analysis - Time Difference Calculator

This script calculates the time difference between two timestamps in the formats
used by the LoRa timing analysis system:
- Real-world time: "2025-10-06_19:45:15.234"
- Relative time: "Day00_19:45:15.234"
- Millisecond timestamps: "15234"

Author: LoRa Timing Analysis System
Date: October 2025
"""

import re
from datetime import datetime, timedelta

class TimeCalculator:
    def __init__(self):
        self.real_time_pattern = r'(\d{4})-(\d{2})-(\d{2})_(\d{2}):(\d{2}):(\d{2})\.(\d{3})'
        self.relative_time_pattern = r'Day(\d{2})_(\d{2}):(\d{2}):(\d{2})\.(\d{3})'
        self.millisecond_pattern = r'^\d+$'
    
    def parse_timestamp(self, timestamp_str):
        """Parse different timestamp formats and return milliseconds"""
        timestamp_str = timestamp_str.strip()
        
        # Check for real-world time format: 2025-10-06_19:45:15.234
        match = re.match(self.real_time_pattern, timestamp_str)
        if match:
            year, month, day, hour, minute, second, millisecond = map(int, match.groups())
            dt = datetime(year, month, day, hour, minute, second, millisecond * 1000)
            return int(dt.timestamp() * 1000)
        
        # Check for relative time format: Day00_19:45:15.234
        match = re.match(self.relative_time_pattern, timestamp_str)
        if match:
            days, hour, minute, second, millisecond = map(int, match.groups())
            total_ms = (days * 24 * 60 * 60 * 1000 + 
                       hour * 60 * 60 * 1000 + 
                       minute * 60 * 1000 + 
                       second * 1000 + 
                       millisecond)
            return total_ms
        
        # Check for millisecond format: 15234
        if re.match(self.millisecond_pattern, timestamp_str):
            return int(timestamp_str)
        
        raise ValueError(f"Unknown timestamp format: {timestamp_str}")
    
    def format_duration(self, milliseconds):
        """Format milliseconds into human-readable duration"""
        if milliseconds < 0:
            sign = "-"
            milliseconds = abs(milliseconds)
        else:
            sign = ""
        
        # Convert to components
        days = milliseconds // (24 * 60 * 60 * 1000)
        milliseconds %= (24 * 60 * 60 * 1000)
        
        hours = milliseconds // (60 * 60 * 1000)
        milliseconds %= (60 * 60 * 1000)
        
        minutes = milliseconds // (60 * 1000)
        milliseconds %= (60 * 1000)
        
        seconds = milliseconds // 1000
        ms = milliseconds % 1000
        
        # Format output
        parts = []
        if days > 0:
            parts.append(f"{days} day{'s' if days != 1 else ''}")
        if hours > 0:
            parts.append(f"{hours} hour{'s' if hours != 1 else ''}")
        if minutes > 0:
            parts.append(f"{minutes} minute{'s' if minutes != 1 else ''}")
        if seconds > 0 or ms > 0:
            if ms > 0:
                parts.append(f"{seconds}.{ms:03d} seconds")
            else:
                parts.append(f"{seconds} second{'s' if seconds != 1 else ''}")
        
        if not parts:
            return "0 seconds"
        
        return sign + ", ".join(parts)
    
    def calculate_difference(self, time1, time2):
        """Calculate the difference between two timestamps"""
        try:
            ms1 = self.parse_timestamp(time1)
            ms2 = self.parse_timestamp(time2)
            
            difference = ms2 - ms1
            
            return {
                'time1_ms': ms1,
                'time2_ms': ms2,
                'difference_ms': difference,
                'difference_formatted': self.format_duration(difference),
                'absolute_difference_ms': abs(difference),
                'absolute_difference_formatted': self.format_duration(abs(difference))
            }
        except ValueError as e:
            return {'error': str(e)}

def print_examples():
    """Print usage examples"""
    print("📚 SUPPORTED TIME FORMATS:")
    print("   Real-world time: 2025-10-06_19:45:15.234")
    print("   Relative time:   Day00_19:45:15.234")
    print("   Milliseconds:    15234")
    print()
    print("📋 EXAMPLES:")
    print("   Time 1: 2025-10-06_19:45:15.234")
    print("   Time 2: 2025-10-06_19:45:16.789")
    print("   → Difference: 1.555 seconds")
    print()
    print("   Time 1: 15234")
    print("   Time 2: 16789")
    print("   → Difference: 1.555 seconds")
    print()

def main():
    """Main interactive function"""
    calculator = TimeCalculator()
    
    print("=" * 60)
    print("🕒 LoRa Timing Analysis - Time Difference Calculator")
    print("=" * 60)
    
    while True:
        print("\n" + "=" * 40)
        print("📖 Enter two timestamps to calculate difference")
        print("💡 Type 'examples' to see format examples")
        print("🚪 Type 'quit' to exit")
        print("=" * 40)
        
        # Get first timestamp
        time1 = input("⏰ Enter first timestamp: ").strip()
        if time1.lower() == 'quit':
            print("👋 Goodbye!")
            break
        elif time1.lower() == 'examples':
            print_examples()
            continue
        
        if not time1:
            print("❌ Empty input. Please enter a timestamp.")
            continue
        
        # Get second timestamp
        time2 = input("⏰ Enter second timestamp: ").strip()
        if time2.lower() == 'quit':
            print("👋 Goodbye!")
            break
        elif time2.lower() == 'examples':
            print_examples()
            continue
        
        if not time2:
            print("❌ Empty input. Please enter a timestamp.")
            continue
        
        # Calculate difference
        result = calculator.calculate_difference(time1, time2)
        
        if 'error' in result:
            print(f"❌ ERROR: {result['error']}")
            print_examples()
            continue
        
        # Display results
        print("\n" + "🔍 CALCULATION RESULTS:")
        print("-" * 40)
        print(f"📅 Time 1: {time1}")
        print(f"📅 Time 2: {time2}")
        print()
        print(f"📊 Time 1 (ms): {result['time1_ms']:,}")
        print(f"📊 Time 2 (ms): {result['time2_ms']:,}")
        print()
        print(f"⏱️  Raw Difference: {result['difference_ms']:,} ms")
        print(f"⏱️  Formatted: {result['difference_formatted']}")
        print()
        print(f"📏 Absolute Difference: {result['absolute_difference_ms']:,} ms")
        print(f"📏 Absolute Formatted: {result['absolute_difference_formatted']}")
        
        if result['difference_ms'] < 0:
            print("⚠️  Note: Time 2 is earlier than Time 1 (negative difference)")
        
        print("-" * 40)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\n👋 Goodbye!")
    except Exception as e:
        print(f"\n❌ Unexpected error: {e}")