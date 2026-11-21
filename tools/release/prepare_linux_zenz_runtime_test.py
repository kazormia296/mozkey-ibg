from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from tools.release import prepare_linux_zenz_runtime as target


class PrepareLinuxZenzRuntimeTest(unittest.TestCase):
    def test_configuration_is_static_cpu_only_and_reproducible(self) -> None:
        command = target.cmake_configure_command(
            Path("/source"), Path("/build"), Path("/ninja")
        )
        for option in (
            "-DBUILD_SHARED_LIBS=OFF",
            "-DGGML_STATIC=ON",
            "-DGGML_NATIVE=OFF",
            "-DGGML_SSE42=OFF",
            "-DGGML_AVX=OFF",
            "-DGGML_AVX2=OFF",
            "-DGGML_F16C=OFF",
            "-DGGML_FMA=OFF",
            "-DGGML_BMI2=OFF",
            "-DGGML_OPENMP=OFF",
            "-DGGML_ACCELERATE=OFF",
            "-DGGML_BLAS=OFF",
            "-DLLAMA_CURL=OFF",
            "-DLLAMA_BUILD_SERVER=ON",
            "-DLLAMA_BUILD_UI=OFF",
        ):
            self.assertIn(option, command)
        self.assertIn("-DLLAMA_BUILD_COMMIT=v1.0.0", command)
        self.assertIn("-DLLAMA_BUILD_NUMBER=12359", command)

    def test_cache_verifier_rejects_backend_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            lines = [
                f"{key}:BOOL={value}"
                for key, value in target.REQUIRED_BOOL_OPTIONS.items()
            ]
            lines.append("LLAMA_CURL:UNINITIALIZED=OFF")
            cache = build / "CMakeCache.txt"
            cache.write_text("\n".join(lines) + "\n", encoding="utf-8")
            target._verify_cmake_cache(build)
            cache.write_text(
                cache.read_text(encoding="utf-8").replace(
                    "GGML_NATIVE:BOOL=OFF", "GGML_NATIVE:BOOL=ON"
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                target.PreparationError, "cmake_backend_contract_invalid"
            ):
                target._verify_cmake_cache(build)

    def test_manifest_binds_server_and_pinned_archive(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            server = root / target.SERVER_NAME
            server.write_bytes(b"server fixture\n")
            server.chmod(0o755)
            manifest = root / target.MANIFEST_NAME
            target._write_manifest(manifest, server)
            document = json.loads(manifest.read_text(encoding="utf-8"))
            self.assertEqual(document["schema_version"], target.SCHEMA)
            self.assertEqual(document["architecture"], "x86_64")
            self.assertEqual(document["llama_cpp"]["tag"], "v1.0.0")
            self.assertEqual(document["server"]["size_bytes"], len(server.read_bytes()))


if __name__ == "__main__":
    unittest.main()
