"""Patch the fetched host HAL to the public surface used by this firmware."""

from pathlib import Path

Import("env")  # noqa: F821

src = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "simulator" / "src"


def replace_once(path, old, new, marker):
    text = path.read_text()
    if marker in text:
        return
    if old not in text:
        raise SystemExit(f"[sim-catchup] missing anchor for {marker}: {path}")
    path.write_text(text.replace(old, new, 1))


display_header = src / "HalDisplay.h"
display_text = display_header.read_text()
if "Absolute = Direct" not in display_text:
    if "enum class GrayscaleMode : uint8_t { Overlay, Absolute };" in display_text:
        display_header.write_text(
            display_text.replace(
                "enum class GrayscaleMode : uint8_t { Overlay, Absolute };",
                "enum class GrayscaleMode : uint8_t { Overlay, Direct, Absolute = Direct };",
                1,
            )
        )
    elif "enum class GrayscaleMode : uint8_t { Overlay, Direct };" in display_text:
        display_header.write_text(
            display_text.replace(
                "enum class GrayscaleMode : uint8_t { Overlay, Direct };",
                "enum class GrayscaleMode : uint8_t { Overlay, Direct, Absolute = Direct };",
                1,
            )
        )
    else:
        raise SystemExit(f"[sim-catchup] missing grayscale enum: {display_header}")

replace_once(
    src / "HalMemory.h",
    "#include <cstddef>\n",
    "#include <cstddef>\n#include <cstdint>\n#include <memory>\n",
    "PsramBuffer",
)
replace_once(
    src / "HalMemory.h",
    "  static HeapStats getDefaultHeap();\n",
    "  struct PsramDeleter {\n"
    "    void operator()(uint8_t* buffer) const;\n"
    "  };\n"
    "  using PsramBuffer = std::unique_ptr<uint8_t[], PsramDeleter>;\n"
    "  static PsramBuffer allocatePsram(size_t bytes);\n\n"
    "  static HeapStats getDefaultHeap();\n",
    "allocatePsram",
)
replace_once(
    src / "HalMemory.cpp",
    '#include "Arduino.h"\n',
    '#include "Arduino.h"\n#include <cstdlib>\n\n'
    'void HalMemory::PsramDeleter::operator()(uint8_t* buffer) const { std::free(buffer); }\n'
    'HalMemory::PsramBuffer HalMemory::allocatePsram(size_t bytes) {\n'
    '  return PsramBuffer(static_cast<uint8_t*>(std::malloc(bytes)));\n'
    '}\n',
    "HalMemory::allocatePsram",
)

replace_once(
    src / "HalStorage.h",
    "  UsbDriveState usbDriveState() const;\n",
    "  UsbDriveState usbDriveState() const;\n"
    "  bool usbDriveHostSuspended() const;\n",
    "usbDriveHostSuspended",
)
replace_once(
    src / "HalStorage.cpp",
    "UsbDriveState HalStorage::usbDriveState() const {\n"
    "  return UsbDriveState::Unsupported;\n}\n",
    "UsbDriveState HalStorage::usbDriveState() const {\n"
    "  return UsbDriveState::Unsupported;\n}\n"
    "bool HalStorage::usbDriveHostSuspended() const { return false; }\n",
    "HalStorage::usbDriveHostSuspended",
)

replace_once(
    src / "Arduino.h",
    "  void restart() {}",
    "  void restart() {}\n"
    "  const char *getChipModel() { return \"ESP32-C3\"; }\n"
    "  uint8_t getChipRevision() { return 0; }\n"
    "  uint32_t getFlashChipSize() { return 16u * 1024u * 1024u; }",
    "getChipModel",
)

replace_once(
    src / "BoardConfig.h",
    "struct ViewableInsets {",
    "enum class TouchController : uint8_t { None, Chsc6x, Gt911, Ft5x06, Ft6336u, Gslx680 };\n"
    "\n"
    "struct TouchConfig {\n"
    "  TouchController controller = TouchController::None;\n"
    "};\n"
    "\n"
    "struct ViewableInsets {",
    "enum class TouchController",
)
replace_once(
    src / "BoardConfig.h",
    "  ViewableInsets viewableInsets = {};\n};",
    "  ViewableInsets viewableInsets = {};\n"
    "  uint16_t displayWidth = 800;\n"
    "  uint16_t displayHeight = 480;\n"
    "  TouchConfig touch = {};\n"
    "};",
    "uint16_t displayWidth",
)
replace_once(
    src / "BoardConfig.h",
    "enum class DisplayController {\n"
    "  SSD1677,\n"
    "  UC8253,\n"
    "  UC8279,\n"
    "  UC8179,\n"
    "};",
    "enum class DisplayController : uint8_t {\n"
    "  SSD1677 = 0,\n"
    "  UC8253 = 2,\n"
    "  ED2208 = 3,\n"
    "  LgfxEpd = 4,\n"
    "  IT8951 = 5,\n"
    "  UC8279 = 6,\n"
    "  UC8179 = 7,\n"
    "  UC8279C = 8\n"
    "};",
    "UC8279C",
)

replace_once(
    src / "HalClock.h",
    "#include <cstddef>\n#include <cstdint>",
    "#include <cstddef>\n#include <cstdint>\n#include <cstdlib>\n#include <ctime>",
    "#include <ctime>",
)
replace_once(
    src / "HalClock.h",
    "  bool syncFromNTP();",
    "  void setTimezone(const char *posixTz) {\n"
    "    ::setenv(\"TZ\", posixTz && posixTz[0] != '\\0' ? posixTz : \"UTC0\", 1);\n"
    "    ::tzset();\n"
    "  }\n"
    "  bool syncFromNTP();",
    "setTimezone",
)

replace_once(
    src / "HalStorage.h",
    "  uint64_t fileSize64();",
    "  uint64_t fileSize64();\n  uint32_t modificationTime();",
    "modificationTime",
)
replace_once(
    src / "HalStorage.cpp",
    "uint64_t HalFile::fileSize64() { return size(); }",
    "uint64_t HalFile::fileSize64() { return size(); }\n"
    "uint32_t HalFile::modificationTime() {\n"
    "  if (!impl || impl->fd < 0) return 0;\n"
    "  struct stat st;\n"
    "  if (fstat(impl->fd, &st) != 0) return 0;\n"
    "  return static_cast<uint32_t>(st.st_mtime);\n"
    "}",
    "HalFile::modificationTime",
)

def _sim_abort_on_unopened_close():
    # Firmware parity: lib/hal asserts on close-before-open
    # (HAL_FILE_WRAPPED_CALL asserts impl != nullptr, and a fresh HalFile has
    # no impl). The simulator instead always allocates an Impl, so the same
    # bug is silently tolerated on the host and only crashes on hardware
    # ("Falha do sistema ... HalFile::close()"). Track whether open was ever
    # attempted and abort, so lifecycle bugs reproduce in the simulator.
    replace_once(
        src / "HalStorage.cpp",
        "  DIR *dir = nullptr;\n",
        "  DIR *dir = nullptr;\n  bool opened = false;  // sim-track-opened-field\n",
        "sim-track-opened-field",
    )
    replace_once(
        src / "HalStorage.cpp",
        "  bool open(const char *p, int flags) {\n    path = p;",
        "  bool open(const char *p, int flags) {\n    opened = true;  // sim-track-opened-file\n    path = p;",
        "sim-track-opened-file",
    )
    replace_once(
        src / "HalStorage.cpp",
        "  bool openAsDir(const char *p) {\n    path = p;",
        "  bool openAsDir(const char *p) {\n    opened = true;  // sim-track-opened-dir\n    path = p;",
        "sim-track-opened-dir",
    )
    for old, marker in (
        (
            "bool HalFile::close() {\n  if (!impl)\n    return true;",
            "sim-close-unopened-abort",
        ),
        (
            "bool HalFile::close() {\n"
            "  if (!impl) {\n"
            "    // Firmware parity: lib/hal asserts on close-before-open\n"
            "    // (HAL_FILE_WRAPPED_CALL). Abort here so lifecycle bugs reproduce\n"
            "    // on the host instead of surfacing only as a hardware crash screen.\n"
            '    fprintf(stderr, "[SIM] HalFile::close() on empty file (firmware would assert)\\n");\n'
            "    std::abort();\n"
            "  }",
            "sim-close-unopened-abort-v2",
        ),
    ):
        try:
            replace_once(
                src / "HalStorage.cpp",
                old,
                "bool HalFile::close() {\n"
                "  if (!impl || !impl->opened) {  // sim-close-unopened-abort\n"
                '    fprintf(stderr, "[SIM] HalFile::close() without open (firmware would assert)\\n");\n'
                "    std::abort();\n"
                "  }",
                marker,
            )
            break
        except SystemExit:
            continue
    else:
        raise SystemExit("[sim-catchup] missing anchor for sim-close-unopened-abort")


_sim_abort_on_unopened_close()
