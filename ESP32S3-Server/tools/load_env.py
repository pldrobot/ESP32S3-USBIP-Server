# pyright: reportUndefinedVariable=false
"""
PlatformIO pre-build script.
Reads .env from the project root and generates src/config_env.h so all
device-specific defines are available at compile time.
For the OTA environment it also injects upload_port, monitor_port, and
--auth from .env so nothing device-specific needs to live in platformio.ini.
"""
Import("env")  # noqa: F821 — provided by PlatformIO
import os

PROJECT_DIR = env.subst("$PROJECT_DIR")
ENV_PATH    = os.path.join(PROJECT_DIR, ".env")
HEADER_PATH = os.path.join(PROJECT_DIR, "src", "config_env.h")


def parse_env_file(path):
    """Return dict of {key: (value, was_quoted)}."""
    values = {}
    if not os.path.isfile(path):
        return values
    with open(path) as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            val = val.strip()
            quoted = len(val) >= 2 and val[0] in "\"'" and val[-1] == val[0]
            if quoted:
                val = val[1:-1]
            values[key.strip()] = (val, quoted)
    return values


def write_header(values):
    lines = [
        "#pragma once",
        "// Auto-generated from .env — do not edit or commit this file.",
        "// Re-generated every time 'pio run' is executed.",
        "",
    ]
    for k, (v, quoted) in values.items():
        # Quoted in .env → always a C string literal.
        # Unquoted → numeric only if it looks like a hex/decimal constant.
        is_numeric = not quoted and (v.startswith(("0x", "0X")) or v.lstrip("-").isdigit())
        if is_numeric:
            lines.append(f"#define {k:<28} {v}")
        else:
            lines.append(f'#define {k:<28} "{v}"')
    with open(HEADER_PATH, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("[load_env] Generated src/config_env.h from .env")


cfg = parse_env_file(ENV_PATH)

if not cfg:
    print("\n[load_env] WARNING: .env not found.")
    print("[load_env]   Copy .env.template to .env and fill in your values.\n")
    with open(HEADER_PATH, "w") as f:
        f.write('#pragma once\n#error ".env not found — copy .env.template to .env"\n')
else:
    write_header(cfg)

    # For the OTA environment: inject upload_port, monitor_port, and --auth from .env
    if env.subst("$PIOENV").endswith("_ota"):
        host = cfg["OTA_HOSTNAME"][0] if "OTA_HOSTNAME" in cfg else ""
        if host:
            env.Replace(UPLOAD_PORT=host)
            env.Replace(MONITOR_PORT="socket://{}:23".format(host))
            print("[load_env] OTA upload_port  → {}".format(host))
            print("[load_env] OTA monitor_port → socket://{}:23".format(host))
        if "OTA_PASSWORD" in cfg:
            # Must go into UPLOAD_FLAGS, not UPLOADERFLAGS.
            # The espota command is: $UPLOADERFLAGS --file $SOURCE $UPLOAD_FLAGS
            # The platform's builder/main.py calls env.Replace(UPLOADERFLAGS=[...])
            # which overwrites any Append we do there.  UPLOAD_FLAGS is the
            # user-controlled slot that the platform never touches.
            env.Replace(UPLOAD_FLAGS="--auth=" + cfg["OTA_PASSWORD"][0])
            print("[load_env] OTA --auth injected into UPLOAD_FLAGS")
