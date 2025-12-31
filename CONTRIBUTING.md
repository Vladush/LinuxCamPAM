# Contributing to LinuxCamPAM

Thanks for your interest in contributing! This started as a personal project for my family, and I'm happy to see it grow.

## How Can I Contribute?

### Reporting Bugs

- **Ensure the bug was not already reported** by searching on GitHub under [Issues](https://github.com/Vladush/LinuxCamPAM/issues).
- If you're unable to find an open issue addressing the problem, [open a new one](https://github.com/Vladush/LinuxCamPAM/issues/new).
- Be precise: **Include specific error logs**, your **OS version**, **camera model**, and whether you are using **IR or RGB**.

### Suggesting Enhancements

- Open a new issue with a clear title and detailed description.
- Explain *why* this enhancement would be useful to most users.

### Pull Requests

1. **Fork the repo** and create your branch from `main`.
2. If you've added code that should be tested, add tests.
3. If you've changed APIs, update the documentation.
4. Ensure the test suite passes (`make test` or `ctest`).
5. **Format your code**. We follow standard C++ conventions. If possible, run `clang-format` on your changes.

## Development Setup

See the [README.md](README.md) for full installation instructions.

### Quick Build for Devs

```bash
mkdir build && cd build
cmake .. -DENABLE_TESTING=ON
make
# Run tests
./linuxcampam_tests
```

## Security

If you discover a potential security, privacy, or data handling issue, please refer to our [Security Policy](SECURITY.md).
