#include "persist_log.h"
#include <LittleFS.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <stdarg.h>

#define PLOG_RING_SIZE      3072
#define PLOG_KEEP           3            // previous boots kept in flash
#define PLOG_FLUSH_MIN_MS   60000UL
#define PLOG_MAGIC          0x504C4F47u  // "PLOG"

struct Ring {
    uint32_t magic;
    uint32_t head;                       // next write index
    uint32_t total;                      // bytes ever written (saturates at ring size)
    char     buf[PLOG_RING_SIZE];
};
static RTC_NOINIT_ATTR Ring ring;

static portMUX_TYPE   mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool  flushPending = false;
static unsigned long  lastFlush = 0;
static bool           fsOk = false;
static int            bootReason = 0;

static const char* kCur = "/log_cur.txt";   // snapshot of this boot, refreshed on warn/err
static String prevName(int i) { return String("/log_prev") + i + ".txt"; }

const char* PersistLog::resetReasonName(int r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software restart";
        case ESP_RST_PANIC:     return "PANIC (crash)";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_BROWNOUT:  return "BROWNOUT (power)";
        default:                return "unknown";
    }
}

// Copy ring contents, oldest first, into out (caller holds no lock).
static String ringText() {
    String s;
    char* tmp = (char*)malloc(PLOG_RING_SIZE + 1);
    if (!tmp) return s;
    uint32_t head, total;
    portENTER_CRITICAL(&mux);
    head = ring.head; total = ring.total;
    if (total >= PLOG_RING_SIZE) {
        memcpy(tmp, ring.buf + head, PLOG_RING_SIZE - head);
        memcpy(tmp + (PLOG_RING_SIZE - head), ring.buf, head);
        total = PLOG_RING_SIZE;
    } else {
        memcpy(tmp, ring.buf, total);
    }
    portEXIT_CRITICAL(&mux);
    tmp[total] = 0;
    char* nl = (total >= PLOG_RING_SIZE) ? strchr(tmp, '\n') : nullptr;  // drop partial first line
    s = nl ? String(nl + 1) : String(tmp);
    free(tmp);
    return s;
}

static void writeFile(const char* path, const String& header, const String& body) {
    File f = LittleFS.open(path, "w");
    if (!f) return;
    f.print(header);
    f.print(body);
    f.close();
}

static String readFile(const String& path) {
    File f = LittleFS.open(path, "r");
    if (!f) return "";
    String s = f.readString();
    f.close();
    return s;
}

void PersistLog::begin() {
    bootReason = (int)esp_reset_reason();
    bool ringValid = (ring.magic == PLOG_MAGIC) && ring.head < PLOG_RING_SIZE
                     && bootReason != ESP_RST_POWERON;

    fsOk = LittleFS.begin(true);   // formats on first use

    // Preserve the previous boot's log in flash.
    if (fsOk) {
        String prev;
        String hdr;
        if (ringValid) {
            prev = ringText();
            hdr  = "";   // header (with reset reason) was written as first line of that boot
        } else if (LittleFS.exists(kCur)) {
            prev = readFile(kCur);   // last flushed snapshot (e.g. after power loss)
        }
        if (prev.length()) {
            for (int i = PLOG_KEEP; i > 1; i--) {
                String from = prevName(i - 1), to = prevName(i);
                if (LittleFS.exists(from)) { LittleFS.remove(to); LittleFS.rename(from, to); }
            }
            writeFile(prevName(1).c_str(), hdr, prev);
        }
    }

    // Start a fresh ring for this boot.
    portENTER_CRITICAL(&mux);
    ring.magic = PLOG_MAGIC; ring.head = 0; ring.total = 0;
    memset(ring.buf, 0, sizeof(ring.buf));
    portEXIT_CRITICAL(&mux);

    addf('I', "=== BOOT === reset reason: %s (%d)%s", resetReasonName(bootReason), bootReason,
         ringValid ? "" : "  [previous RAM log unavailable]");
    if (bootReason == ESP_RST_PANIC || bootReason == ESP_RST_INT_WDT ||
        bootReason == ESP_RST_TASK_WDT || bootReason == ESP_RST_WDT ||
        bootReason == ESP_RST_BROWNOUT)
        add('E', "Previous run ended abnormally - see 'previous boot' log below");
}

void PersistLog::add(char level, const char* line) {
    char head[24];
    int hl = snprintf(head, sizeof(head), "[%lu] %c ", millis() / 1000, level);
    size_t ll = strlen(line);
    if (ll > 200) ll = 200;

    portENTER_CRITICAL(&mux);
    auto put = [](const char* p, size_t n) {
        for (size_t i = 0; i < n; i++) {
            ring.buf[ring.head] = p[i];
            ring.head = (ring.head + 1) % PLOG_RING_SIZE;
        }
        if (ring.total < PLOG_RING_SIZE) ring.total = min<uint32_t>(PLOG_RING_SIZE, ring.total + n);
    };
    put(head, hl);
    put(line, ll);
    put("\n", 1);
    portEXIT_CRITICAL(&mux);

    if (level == 'W' || level == 'E') flushPending = true;
}

void PersistLog::addf(char level, const char* fmt, ...) {
    char buf[200];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    add(level, buf);
}

void PersistLog::flushNow() {
    if (!fsOk) return;
    String hdr = String("# snapshot at uptime ") + (millis() / 1000) + "s\n";
    writeFile(kCur, hdr, ringText());
    lastFlush = millis();
    flushPending = false;
}

void PersistLog::task() {
    if (flushPending && millis() - lastFlush >= PLOG_FLUSH_MIN_MS) flushNow();
}

String PersistLog::dump() {
    String out;
    out.reserve(PLOG_RING_SIZE * 2);
    out += "===== CURRENT BOOT (live) =====\n";
    out += ringText();
    if (fsOk) {
        for (int i = 1; i <= PLOG_KEEP; i++) {
            String p = prevName(i);
            if (!LittleFS.exists(p)) continue;
            out += "\n===== PREVIOUS BOOT -" + String(i) + " =====\n";
            out += readFile(p);
        }
    } else {
        out += "\n(flash log storage unavailable)\n";
    }
    return out;
}

// Library hook: every [USBIP x] line lands in the persistent log too.
extern "C" void espusbip_log_hook(char level, const char* line) {
    PersistLog::add(level, line);
}
