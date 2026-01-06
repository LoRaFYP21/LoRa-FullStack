#!/usr/bin/env python3
"""
Network Test Suite for LoRa Mesh Network

Automated testing script for validating mesh network functionality.

Tests:
1. Connectivity test (ping/pong)
2. Route discovery test
3. Single-hop data transfer
4. Multi-hop relay test
5. Reliability level test
6. Throughput measurement
7. Stress test (multiple messages)

Usage:
    python test_network.py COM9 --dest Node_2 --test all
    python test_network.py COM9 --dest Node_2 --test connectivity
    python test_network.py COM9 --dest Node_2 --test throughput
"""

import argparse
import sys
import time
from pathlib import Path

from mesh_network_interface import MeshNetworkInterface, REL_LOW, REL_MEDIUM, REL_HIGH, REL_CRITICAL


class NetworkTester:
    """Automated test suite for mesh network"""
    
    def __init__(self, port: str, dest: str):
        self.port = port
        self.dest = dest
        self.interface = MeshNetworkInterface(port)
        self.results = {}
    
    def run_test(self, test_name: str, test_func):
        """Run a single test and record results"""
        print(f"\n{'='*60}")
        print(f"TEST: {test_name}")
        print(f"{'='*60}")
        
        start = time.time()
        try:
            success = test_func()
            duration = time.time() - start
            
            self.results[test_name] = {
                'success': success,
                'duration': duration
            }
            
            status = "✓ PASS" if success else "✗ FAIL"
            print(f"\n{status} - Completed in {duration:.1f}s")
            
            return success
        
        except Exception as e:
            duration = time.time() - start
            self.results[test_name] = {
                'success': False,
                'duration': duration,
                'error': str(e)
            }
            
            print(f"\n✗ FAIL - Exception: {e}")
            return False
    
    def test_connectivity(self) -> bool:
        """Test 1: Basic connectivity"""
        print("\nSending ping message...")
        
        success = self.interface.send_text(
            self.dest,
            "PING:TEST",
            reliability=REL_LOW
        )
        
        if success:
            print("✓ Connectivity confirmed")
        else:
            print("✗ Failed to reach destination")
        
        return success
    
    def test_route_discovery(self) -> bool:
        """Test 2: Route discovery mechanism"""
        print("\nInitiating route discovery...")
        
        success = self.interface.discover_route(self.dest)
        
        if success:
            print("✓ Route discovered successfully")
            
            # Verify route in table
            time.sleep(1)
            self.interface.show_routes()
        else:
            print("✗ Route discovery failed")
        
        return success
    
    def test_small_message(self) -> bool:
        """Test 3: Single packet message"""
        print("\nSending small message (single packet)...")
        
        message = "Test message ABC123"
        
        success = self.interface.send_text(
            self.dest,
            message,
            reliability=REL_MEDIUM
        )
        
        if success:
            print(f"✓ Message sent: {message}")
        else:
            print("✗ Message send failed")
        
        return success
    
    def test_large_message(self) -> bool:
        """Test 4: Fragmented message"""
        print("\nSending large message (multiple fragments)...")
        
        # Generate ~500 character message
        message = "X" * 500
        
        success = self.interface.send_text(
            self.dest,
            message,
            reliability=REL_MEDIUM
        )
        
        if success:
            print(f"✓ Large message sent ({len(message)} chars)")
            print(f"  Fragments: {(len(message) // 150) + 1}")
        else:
            print("✗ Large message send failed")
        
        return success
    
    def test_reliability_levels(self) -> bool:
        """Test 5: All reliability levels"""
        print("\nTesting all reliability levels...")
        
        levels = [
            (REL_LOW, "LOW"),
            (REL_MEDIUM, "MEDIUM"),
            (REL_HIGH, "HIGH"),
            (REL_CRITICAL, "CRITICAL")
        ]
        
        results = []
        
        for level, name in levels:
            print(f"\n  Testing {name} (level {level})...")
            
            success = self.interface.send_text(
                self.dest,
                f"TEST:{name}",
                reliability=level
            )
            
            results.append(success)
            
            if success:
                print(f"    ✓ {name} successful")
            else:
                print(f"    ✗ {name} failed")
            
            time.sleep(2)  # Small delay between tests
        
        overall_success = all(results)
        
        print(f"\n  Overall: {sum(results)}/{len(results)} levels passed")
        
        return overall_success
    
    def test_throughput(self) -> bool:
        """Test 6: Measure throughput"""
        print("\nMeasuring throughput...")
        
        # Send known size data and measure time
        data_size = 1000  # 1000 characters
        test_data = "A" * data_size
        
        print(f"  Sending {data_size} characters...")
        
        start = time.time()
        success = self.interface.send_text(
            self.dest,
            test_data,
            reliability=REL_MEDIUM
        )
        duration = time.time() - start
        
        if success and duration > 0:
            throughput_bps = (data_size * 8) / duration
            throughput_kbps = throughput_bps / 1000
            
            print(f"  ✓ Transfer completed")
            print(f"  Duration: {duration:.1f}s")
            print(f"  Throughput: {throughput_bps:.0f} bps ({throughput_kbps:.2f} kbps)")
            
            return True
        else:
            print("  ✗ Throughput test failed")
            return False
    
    def test_stress(self) -> bool:
        """Test 7: Stress test with multiple messages"""
        print("\nRunning stress test (10 messages)...")
        
        num_messages = 10
        successes = 0
        
        for i in range(num_messages):
            print(f"  Message {i+1}/{num_messages}...", end=" ")
            
            success = self.interface.send_text(
                self.dest,
                f"STRESS_TEST:{i+1}",
                reliability=REL_LOW
            )
            
            if success:
                successes += 1
                print("✓")
            else:
                print("✗")
            
            time.sleep(0.5)  # Small delay between messages
        
        success_rate = (successes / num_messages) * 100
        
        print(f"\n  Success rate: {successes}/{num_messages} ({success_rate:.0f}%)")
        
        # Consider test passed if >= 80% success
        return success_rate >= 80.0
    
    def test_file_transfer(self) -> bool:
        """Test 8: Small file transfer"""
        print("\nTesting file transfer...")
        
        # Create a small test file
        test_file = Path("test_file.txt")
        test_content = "This is a test file for mesh network validation.\n" * 10
        test_file.write_text(test_content)
        
        print(f"  Created test file: {test_file} ({len(test_content)} bytes)")
        
        success = self.interface.send_file(
            self.dest,
            str(test_file),
            reliability=REL_MEDIUM
        )
        
        # Cleanup
        test_file.unlink()
        
        if success:
            print("  ✓ File transfer successful")
        else:
            print("  ✗ File transfer failed")
        
        return success
    
    def run_all_tests(self):
        """Run complete test suite"""
        print("\n" + "="*60)
        print("MESH NETWORK TEST SUITE")
        print("="*60)
        print(f"Port: {self.port}")
        print(f"Destination: {self.dest}")
        print("="*60)
        
        # Connect
        if not self.interface.connect():
            print("\n✗ FATAL: Failed to connect to node")
            return False
        
        tests = [
            ("Connectivity", self.test_connectivity),
            ("Route Discovery", self.test_route_discovery),
            ("Small Message", self.test_small_message),
            ("Large Message", self.test_large_message),
            ("Reliability Levels", self.test_reliability_levels),
            ("Throughput", self.test_throughput),
            ("File Transfer", self.test_file_transfer),
            ("Stress Test", self.test_stress)
        ]
        
        passed = 0
        failed = 0
        
        for name, func in tests:
            if self.run_test(name, func):
                passed += 1
            else:
                failed += 1
            
            time.sleep(2)  # Delay between tests
        
        # Disconnect
        self.interface.disconnect()
        
        # Print summary
        self.print_summary(passed, failed)
        
        return failed == 0
    
    def print_summary(self, passed: int, failed: int):
        """Print test summary"""
        total = passed + failed
        success_rate = (passed / total * 100) if total > 0 else 0
        
        print("\n" + "="*60)
        print("TEST SUMMARY")
        print("="*60)
        
        for test_name, result in self.results.items():
            status = "✓ PASS" if result['success'] else "✗ FAIL"
            duration = result['duration']
            print(f"{status} - {test_name:<25} ({duration:.1f}s)")
            
            if 'error' in result:
                print(f"       Error: {result['error']}")
        
        print("="*60)
        print(f"Total:        {total}")
        print(f"Passed:       {passed}")
        print(f"Failed:       {failed}")
        print(f"Success Rate: {success_rate:.1f}%")
        print("="*60)
        
        if failed == 0:
            print("\n🎉 All tests passed! Network is operational.")
        else:
            print(f"\n⚠️  {failed} test(s) failed. Check configuration and connections.")


def main():
    parser = argparse.ArgumentParser(
        description='Automated test suite for LoRa Mesh Network',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Test types:
  connectivity     - Basic ping test
  route           - Route discovery test
  small           - Single packet message
  large           - Fragmented message
  reliability     - All reliability levels
  throughput      - Speed measurement
  file            - File transfer test
  stress          - Multiple message test
  all             - Run complete suite

Examples:
  python test_network.py COM9 --dest Node_2 --test all
  python test_network.py COM9 --dest Node_3 --test connectivity
  python test_network.py COM9 --dest Node_4 --test throughput
        """
    )
    
    parser.add_argument('port', help='Serial port (e.g., COM9)')
    parser.add_argument('--dest', required=True, help='Destination node name')
    parser.add_argument('--test', default='all',
                       choices=['connectivity', 'route', 'small', 'large', 
                               'reliability', 'throughput', 'file', 'stress', 'all'],
                       help='Test to run (default: all)')
    
    args = parser.parse_args()
    
    # Create tester
    tester = NetworkTester(args.port, args.dest)
    
    # Run selected test(s)
    if args.test == 'all':
        success = tester.run_all_tests()
    else:
        # Connect first
        if not tester.interface.connect():
            print("\n✗ FATAL: Failed to connect to node")
            return 1
        
        # Run individual test
        test_map = {
            'connectivity': ("Connectivity", tester.test_connectivity),
            'route': ("Route Discovery", tester.test_route_discovery),
            'small': ("Small Message", tester.test_small_message),
            'large': ("Large Message", tester.test_large_message),
            'reliability': ("Reliability Levels", tester.test_reliability_levels),
            'throughput': ("Throughput", tester.test_throughput),
            'file': ("File Transfer", tester.test_file_transfer),
            'stress': ("Stress Test", tester.test_stress)
        }
        
        name, func = test_map[args.test]
        success = tester.run_test(name, func)
        
        tester.interface.disconnect()
    
    return 0 if success else 1


if __name__ == '__main__':
    sys.exit(main())
