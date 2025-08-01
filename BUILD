# generate compilation commands for enzymexlamlir-opt
load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")
load("@python_version_repo//:py_version.bzl", "HERMETIC_PYTHON_VERSION")
load("@rules_python//python:packaging.bzl", "py_wheel")
load(
    "@xla//xla/tsl/platform:build_config_root.bzl",
    "if_llvm_aarch32_available",
    "if_llvm_aarch64_available",
    "if_llvm_powerpc_available",
    "if_llvm_system_z_available",
    "if_llvm_x86_available",
)
load(":package.bzl", "py_package")

licenses(["notice"])

package(
    default_applicable_licenses = [],
    default_visibility = ["//:__subpackages__"],
)

py_package(
    name = "enzyme_jax_data",
    # Only include these Python packages.
    packages = [
        "@//src/enzyme_ad/jax:enzyme_call.so",
        "@llvm-project//clang:builtin_headers_gen",
    ],
    deps = [
        "//src/enzyme_ad/jax:enzyme_call.so",
        "@llvm-project//clang:builtin_headers_gen",
    ],
)

cc_binary(
    name = "enzymexlamlir-opt",
    srcs = [
        "//src/enzyme_ad/jax:RegistryUtils.cpp",
        "//src/enzyme_ad/jax:enzymexlamlir-opt.cpp",
    ],
    copts = [
        "-Wno-unused-variable",
        "-Wno-return-type",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//src/enzyme_ad/jax:RegistryUtils",
        "@llvm-project//mlir:GPUToLLVMIRTranslation",
        "@llvm-project//mlir:LLVMToLLVMIRTranslation",
        "@llvm-project//mlir:MlirOptLib",
        "@llvm-project//mlir:NVVMToLLVMIRTranslation",
        "@tsl//tsl/platform:env",
        "@tsl//tsl/platform:env_impl",
    ] + if_llvm_aarch32_available([
        "@llvm-project//llvm:ARMAsmParser",
        "@llvm-project//llvm:ARMCodeGen",
    ]) + if_llvm_aarch64_available([
        "@llvm-project//llvm:AArch64AsmParser",
        "@llvm-project//llvm:AArch64CodeGen",
    ]) + if_llvm_powerpc_available([
        "@llvm-project//llvm:PowerPCAsmParser",
        "@llvm-project//llvm:PowerPCCodeGen",
    ]) + if_llvm_system_z_available([
        "@llvm-project//llvm:SystemZAsmParser",
        "@llvm-project//llvm:SystemZCodeGen",
    ]) + if_llvm_x86_available([
        "@llvm-project//llvm:X86AsmParser",
        "@llvm-project//llvm:X86CodeGen",
    ]),
)

cc_library(
    name = "RaiseLib",
    srcs = [
        "//src/enzyme_ad/jax:RegistryUtils.cpp",
        "//src/enzyme_ad/jax:raise.cpp",
    ],
    copts = [
        "-Wno-unused-variable",
        "-Wno-return-type",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [
        "//src/enzyme_ad/jax:RegistryUtils",
        "@llvm-project//mlir:GPUToLLVMIRTranslation",
        "@llvm-project//mlir:LLVMToLLVMIRTranslation",
        "@llvm-project//mlir:MlirOptLib",
        "@llvm-project//mlir:NVVMToLLVMIRTranslation",
        "@tsl//tsl/platform:env",
        "@tsl//tsl/platform:env_impl",
    ] + if_llvm_aarch32_available([
        "@llvm-project//llvm:ARMAsmParser",
        "@llvm-project//llvm:ARMCodeGen",
    ]) + if_llvm_aarch64_available([
        "@llvm-project//llvm:AArch64AsmParser",
        "@llvm-project//llvm:AArch64CodeGen",
    ]) + if_llvm_powerpc_available([
        "@llvm-project//llvm:PowerPCAsmParser",
        "@llvm-project//llvm:PowerPCCodeGen",
    ]) + if_llvm_system_z_available([
        "@llvm-project//llvm:SystemZAsmParser",
        "@llvm-project//llvm:SystemZCodeGen",
    ]) + if_llvm_x86_available([
        "@llvm-project//llvm:X86AsmParser",
        "@llvm-project//llvm:X86CodeGen",
    ]),
    alwayslink = True,
)

# cc_shared_library(
cc_binary(
    name = "libRaise.so",
    linkshared = 1,  ## important
    linkstatic = 1,  ## important
    deps = [":RaiseLib"],
)

cc_binary(
    name = "enzymexlamlir-tblgen",
    srcs = ["//src/enzyme_ad/tools:enzymexlamlir-tblgen.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "@llvm-project//llvm:Support",
        "@llvm-project//llvm:TableGen",
        "@llvm-project//llvm:config",
    ],
)

py_library(
    name = "enzyme_ad",
    data = [
        "//:enzyme_jax_data",
        "//src/enzyme_ad/jax:enzyme_jax_internal",
    ],
    imports = ["src"],
    visibility = ["//visibility:public"],
    deps = [
        "@pypi_absl_py//:pkg",
        "@pypi_jax//:pkg",
    ],
)

py_wheel(
    name = "wheel",
    author = "Enzyme Authors",
    author_email = "wmoses@mit.edu, zinenko@google.com",
    distribution = "enzyme_ad",
    homepage = "https://enzyme.mit.edu/",
    license = "LLVM",
    platform = select({
        "@bazel_tools//src/conditions:windows_x64": "win_amd64",
        "@bazel_tools//src/conditions:darwin_arm64": "macosx_11_0_arm64",
        "@bazel_tools//src/conditions:darwin_x86_64": "macosx_10_14_x86_64",
        "@bazel_tools//src/conditions:linux_aarch64": "manylinux2014_aarch64",
        "@bazel_tools//src/conditions:linux_x86_64": "manylinux2014_x86_64",
        "@bazel_tools//src/conditions:linux_ppc64le": "manylinux2014_ppc64le",
    }),
    project_urls = {
        "GitHub": "https://github.com/EnzymeAD/Enzyme-JAX/",
    },
    python_requires = "==" + HERMETIC_PYTHON_VERSION + ".*",
    python_tag = "py" + HERMETIC_PYTHON_VERSION.replace(".", ""),
    requires = [
        "absl_py >= 2.0.0",
        "jax >= 0.4.21",
        "jaxlib >= 0.4.21",
    ],
    strip_path_prefixes = ["src/"],
    summary = "Enzyme automatic differentiation tool.",
    version = "0.0.10",
    deps = [
        ":enzyme_jax_data",
        "//src/enzyme_ad/jax:enzyme_jax_internal",
    ],
)

refresh_compile_commands(
    name = "refresh_compile_commands",

    # Specify the targets of interest.
    # For example, specify a dict of targets and any flags required to build.
    targets = ["//:enzymexlamlir-opt"],
    # No need to add flags already in .bazelrc. They're automatically picked up.
    # If you don't need flags, a list of targets is also okay, as is a single target string.
    # Wildcard patterns, like //... for everything, *are* allowed here, just like a build.
    # As are additional targets (+) and subtractions (-), like in bazel query https://docs.bazel.build/versions/main/query.html#expressions
    # And if you're working on a header-only library, specify a test or binary target that compiles it.
)
