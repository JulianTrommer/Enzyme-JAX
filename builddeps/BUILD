load("@python//:defs.bzl", "compile_pip_requirements")
load("@python_version_repo//:py_version.bzl", "REQUIREMENTS")

licenses(["notice"])

compile_pip_requirements(
    name = "requirements",
    data = ["test-requirements.txt"],
    extra_args = [
        "--allow-unsafe",
        "--build-isolation",
        "--rebuild",
    ],
    extra_deps = [
        # "@pypi_wheel//:pkg"
    ],
    generate_hashes = True,
    requirements_in = "requirements.in",
    requirements_txt = REQUIREMENTS,
)
