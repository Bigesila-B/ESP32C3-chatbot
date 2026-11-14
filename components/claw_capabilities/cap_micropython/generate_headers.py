# SPDX-License-Identifier: Apache-2.0
"""Run MicroPython's build-time header generators with the ESP-IDF compiler."""

from pathlib import Path
import re
import subprocess
import sys


def main():
    compiler, source, build, port, *sources = sys.argv[1:]
    py = Path(source) / "py"
    genhdr = Path(build) / "genhdr"
    genhdr.mkdir(parents=True, exist_ok=True)
    includes = [f"-I{path}" for path in (port, source, Path(source) / "ports/embed", build)]

    def generate(script, *args, **kwargs):
        return subprocess.run([sys.executable, str(py / script), *map(str, args)],
                              check=True, **kwargs)

    generate("makeversionhdr.py", genhdr / "mpversion.h")
    preprocessed = genhdr / "qstr.i"
    generate("makeqstrdefs.py", "pp", compiler, "-E", "output", preprocessed,
             "cflags", *includes, "-DNO_QSTR", "cxxflags", *includes, "-DNO_QSTR",
             "sources", *sources)

    def collect(mode):
        split = genhdr / mode
        for old in split.glob(f"*.{mode}"):
            old.unlink()
        collected = genhdr / f"{mode}.collected"
        generate("makeqstrdefs.py", "split", mode, preprocessed, split, "_")
        generate("makeqstrdefs.py", "cat", mode, "_", split, collected)
        return collected

    qstr_source = (py / "qstrdefs.h").read_bytes() + b"\n" + collect("qstr").read_bytes()
    # Protect Q(...) from macro expansion, as in the upstream build.
    qstr_source = re.sub(rb'^(Q\(.*\))$', rb'"\1"', qstr_source, flags=re.MULTILINE)
    qstr_preprocessed = subprocess.run([compiler, "-E", *includes, "-x", "c", "-"],
                                      input=qstr_source, stdout=subprocess.PIPE, check=True).stdout
    qstr_preprocessed = re.sub(rb'^"(Q\(.*\))"$', rb'\1', qstr_preprocessed, flags=re.MULTILINE)
    qstr_input = genhdr / "qstrdefs.preprocessed.h"
    qstr_input.write_bytes(qstr_preprocessed)

    for script, input_file, output in (
        ("makeqstrdata.py", qstr_input, "qstrdefs.generated.h"),
        ("makemoduledefs.py", collect("module"), "moduledefs.h"),
        ("make_root_pointers.py", collect("root_pointer"), "root_pointers.h"),
    ):
        result = generate(script, input_file, stdout=subprocess.PIPE)
        (genhdr / output).write_bytes(result.stdout)


if __name__ == "__main__":
    main()
