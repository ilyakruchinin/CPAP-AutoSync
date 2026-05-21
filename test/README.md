# Test Directory

This directory contains unit tests for the SD WIFI PRO auto uploader project.

## Structure

- `test_config/` - Configuration loading and credential management tests
- `test_credential_migration/` - Secure credential migration tests
- `test_logger_circular_buffer/` - Logger circular buffer tests (in-memory buffer, overflow, line tracking)
- `test_schedule_manager/` - Upload scheduling (Smart, Scheduled, and Manual modes) and NTP sync tests
- `test_upload_state_manager/` - Upload state tracking, journalling, and persistence tests
- `test_native/` - General-purpose native tests
- `mocks/` - Mock implementations of hardware-dependent components (Arduino, FS, Time, WebServer)

For detailed guidance on writing tests, running them, and debugging failures see **[TESTING_GUIDE.md](TESTING_GUIDE.md)**.

## Running Tests

To run the native tests:

```bash
pio test -e native
```

## Test Framework

Tests use the Unity test framework, which is included with PlatformIO.

## Writing Tests

Each test file should:
1. Include the Unity framework: `#include <unity.h>`
2. Include the component being tested
3. Define test functions with `void test_*()` naming
4. Use Unity assertions (TEST_ASSERT_*, TEST_ASSERT_EQUAL_*, etc.)
5. Register tests in `setUp()` and `tearDown()` functions if needed

Example:
```cpp
#include <unity.h>
#include "MyComponent.h"

void test_component_initialization() {
    MyComponent component;
    TEST_ASSERT_TRUE(component.begin());
}

void setUp(void) {
    // Called before each test
}

void tearDown(void) {
    // Called after each test
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_component_initialization);
    return UNITY_END();
}
```
