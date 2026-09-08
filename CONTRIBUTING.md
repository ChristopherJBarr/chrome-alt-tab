# Contributing

Start with [development](docs/development.md) and [architecture](docs/architecture.md). Use C++20, MSVC, CMake and tabs for indentation. Keep dependencies minimal and check API failures. Preserve native-switcher escape shortcuts.

For pull requests, explain the user-visible problem, the resulting behaviour and relevant validation. Run extension tests, CTest and broker integration tests in Debug and Release for native changes. Verify visible changes on Windows. Do not claim production readiness based solely on automated tests.

Do not commit generated native-host manifests, build directories, page captures, browsing history, private URLs, credentials or unreviewed logs. Use synthetic fixtures. Report security-sensitive issues privately rather than in public issue logs.
