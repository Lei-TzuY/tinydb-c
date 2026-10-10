from pathlib import Path


def find_executable(build, name):
    """Find a built CMake probe without relying on the compiler being on PATH."""
    build = Path(build)
    candidates = [
        build / "Debug" / (name + ".exe"),
        build / "Release" / (name + ".exe"),
        build / (name + ".exe"),
        build / name,
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise AssertionError(f"CMake probe {name!r} was not produced in {build}")
