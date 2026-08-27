#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re


def parse_item(raw: str) -> tuple[str, Path]:
    if "=" not in raw:
        raise argparse.ArgumentTypeError(
            "resource must be NAME=/absolute/or/relative/path"
        )

    name, path = raw.split("=", 1)
    name = name.strip()

    if not name:
        raise argparse.ArgumentTypeError("resource name is empty")

    return name, Path(path).resolve()


def symbol_for(name: str) -> str:
    value = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not value or value[0].isdigit():
        value = "_" + value
    return "rb_embed_" + value


def format_bytes(data: bytes) -> str:
    if not data:
        return ""

    parts = []
    width = 20

    for offset in range(0, len(data), width):
        row = data[offset : offset + width]
        parts.append(
            "    " + ", ".join(f"0x{byte:02X}" for byte in row) + ","
        )

    return "\n".join(parts)


parser = argparse.ArgumentParser()
parser.add_argument("--header", required=True)
parser.add_argument("--source", required=True)
parser.add_argument("--asset", action="append", default=[])
parser.add_argument("--tool", action="append", default=[])
args = parser.parse_args()

resources: list[tuple[str, Path, bool]] = []

for raw in args.asset:
    name, path = parse_item(raw)
    resources.append((name, path, False))

for raw in args.tool:
    name, path = parse_item(raw)
    resources.append((name, path, True))

if not resources:
    raise SystemExit("no resources supplied")

seen = set()

for name, path, _ in resources:
    if name in seen:
        raise SystemExit(f"duplicate embedded resource name: {name}")

    seen.add(name)

    if not path.is_file():
        raise SystemExit(f"embedded resource does not exist: {path}")

header = Path(args.header)
source = Path(args.source)

header.parent.mkdir(parents=True, exist_ok=True)
source.parent.mkdir(parents=True, exist_ok=True)

header.write_text(
r'''#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

struct EmbeddedResourceView final {
    const unsigned char* data = nullptr;
    std::size_t size = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return data != nullptr && size != 0;
    }
};

[[nodiscard]] EmbeddedResourceView FindEmbeddedLinuxResource(
    std::string_view name);

[[nodiscard]] std::filesystem::path MaterializeEmbeddedLinuxResource(
    std::string_view name);
''',
    encoding="utf-8",
)

definitions = []
table_rows = []

for name, path, executable in resources:
    data = path.read_bytes()
    sha256 = hashlib.sha256(data).hexdigest()
    symbol = symbol_for(name)

    definitions.append(
        f"alignas(16) static const unsigned char {symbol}[] = {{\n"
        f"{format_bytes(data)}\n"
        f"}};\n"
    )

    table_rows.append(
        "    {"
        f'"{name}", '
        f"{symbol}, "
        f"sizeof({symbol}), "
        f'"{sha256}", '
        f"{'true' if executable else 'false'}"
        "},"
    )

cpp = r'''#include "embedded_bundle_linux.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;

struct ResourceRecord final {
    std::string_view name;
    const unsigned char* data;
    std::size_t size;
    std::string_view sha256;
    bool executable;
};

'''

cpp += "\n".join(definitions)

cpp += "\nstatic constexpr ResourceRecord kResources[] = {\n"
cpp += "\n".join(table_rows)
cpp += "\n};\n\n"

cpp += r'''
[[nodiscard]] const ResourceRecord* FindRecord(
    const std::string_view name)
{
    for (const ResourceRecord& record : kResources) {
        if (record.name == name)
            return &record;
    }

    return nullptr;
}

[[nodiscard]] fs::path RuntimeRoot()
{
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        runtime != nullptr &&
        *runtime != '\0') {
        return
            fs::path(runtime) /
            "RetroBurner";
    }

    std::error_code error;
    fs::path base =
        fs::temp_directory_path(error);

    if (error)
        base = "/tmp";

    return
        base /
        ("RetroBurner-" +
         std::to_string(
             static_cast<unsigned long>(
                 ::getuid())));
}

[[nodiscard]] bool ExistingFileMatches(
    const fs::path& path,
    const std::size_t expectedSize)
{
    std::error_code error;

    if (!fs::is_regular_file(path, error))
        return false;

    const std::uintmax_t size =
        fs::file_size(path, error);

    return
        !error &&
        size ==
            static_cast<std::uintmax_t>(
                expectedSize);
}

[[nodiscard]] bool WriteResource(
    const ResourceRecord& record,
    const fs::path& target)
{
    std::error_code error;

    fs::create_directories(
        target.parent_path(),
        error);

    if (error)
        return false;

    ::chmod(
        target.parent_path().c_str(),
        S_IRWXU);

    const fs::path temporary =
        target.string() +
        ".tmp-" +
        std::to_string(
            static_cast<unsigned long>(
                ::getpid()));

    {
        std::ofstream output(
            temporary,
            std::ios::binary |
                std::ios::trunc);

        if (!output)
            return false;

        output.write(
            reinterpret_cast<const char*>(
                record.data),
            static_cast<std::streamsize>(
                record.size));

        if (!output) {
            output.close();
            fs::remove(
                temporary,
                error);
            return false;
        }
    }

    if (record.executable) {
        if (::chmod(
                temporary.c_str(),
                S_IRUSR |
                S_IWUSR |
                S_IXUSR) != 0) {
            fs::remove(
                temporary,
                error);
            return false;
        }
    } else {
        ::chmod(
            temporary.c_str(),
            S_IRUSR |
            S_IWUSR);
    }

    fs::rename(
        temporary,
        target,
        error);

    if (error) {
        fs::remove(
            temporary,
            error);

        if (!ExistingFileMatches(
                target,
                record.size)) {
            return false;
        }
    }

    return true;
}

} // namespace

EmbeddedResourceView FindEmbeddedLinuxResource(
    const std::string_view name)
{
    const ResourceRecord* const record =
        FindRecord(name);

    if (record == nullptr)
        return {};

    return {
        record->data,
        record->size,
    };
}

fs::path MaterializeEmbeddedLinuxResource(
    const std::string_view name)
{
    const ResourceRecord* const record =
        FindRecord(name);

    if (record == nullptr)
        return {};

    const std::string hashPrefix(
        record->sha256.substr(0, 16));

    const fs::path target =
        RuntimeRoot() /
        hashPrefix /
        std::string(record->name);

    if (ExistingFileMatches(
            target,
            record->size)) {
        if (record->executable) {
            ::chmod(
                target.c_str(),
                S_IRUSR |
                S_IWUSR |
                S_IXUSR);
        }

        return target;
    }

    if (!WriteResource(
            *record,
            target)) {
        return {};
    }

    return target;
}
'''

source.write_text(
    cpp,
    encoding="utf-8",
)

print(f"[EMBED] resources: {len(resources)}")

for name, path, executable in resources:
    print(
        f"[EMBED] {'tool ' if executable else 'asset'} "
        f"{name}: {path.stat().st_size} bytes"
    )

print(f"[EMBED] header: {header}")
print(f"[EMBED] source: {source}")
