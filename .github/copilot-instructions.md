# GitHub Copilot Instructions

## Project Context

This repository contains embedded C++ firmware/software targeting Espressif ESP32 devices.

## Development Environment

- The Espressif VS Code extension is installed and available.
- Copilot/agents may use ESP-IDF tooling, tasks, and commands exposed by the Espressif extension.
- Assume the environment supports:
  - ESP-IDF build system
  - `idf.py`
  - Flash/monitor/debug tasks
  - SDK configuration via `menuconfig`
  - ESP-IDF terminal environment setup
  - Device flashing and serial monitoring

## Preferred Tooling

- Prefer native ESP-IDF APIs and patterns unless otherwise specified.
- Prefer ESP-IDF build/task integration over custom shell scripts.
- Assume VS Code tasks from the Espressif extension are available for:
  - Build
  - Flash
  - Monitor
  - Clean
  - Select target
  - Configure SDK

## Coding Preferences

- Language: Modern C++
- Prefer clear, maintainable, low-allocation embedded-friendly code.
- Avoid unnecessary dynamic memory allocation in runtime paths.
- Prefer RAII and type safety where practical for embedded systems.
- Keep dependencies lightweight.

## Agent Guidance

- When suggesting commands, prefer ESP-IDF-compatible workflows.
- When debugging, consider ESP32-specific tooling and logs.
- When generating examples, assume ESP-IDF project structure unless told otherwise.
- When interacting with serial output, assume `idf.py monitor` is available.