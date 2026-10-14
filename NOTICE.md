# Source and license boundaries

The combined distribution is licensed **MIT AND MPL-2.0**. It is not MIT-only.

- **MIT** (copyright 2026 Dongjun Park, see `LICENSE`) covers everything except the two items below. That includes the plugin files (`.claude-plugin/`, `.mcp.json`, `hooks/`), the daemon, the MCP bridge and the hooks (`source/src/`), libvrs (`source/native/libvrs/` outside `swegca_cpp/`), the tools and the documentation.
- **MPL-2.0** (see `LICENSES/MPL-2.0.txt`) covers `source/native/libvrs/swegca_cpp/**`. These files are a C++ port derived from SWEGCA-Architecture (https://github.com/raspie10032/SWEGCA-Architecture). Keep the directory notice `source/native/libvrs/swegca_cpp/LICENSE` with them. MPL-2.0 is file-level copyleft: changes to these files stay MPL-2.0, and it does not extend to the other files.
- **`bin/vrs_ccd.exe`** is the Executable Form of all of the above, built from `source/` (see `VERSION`). The Source Code Form of its MPL-2.0 part is `source/native/libvrs/swegca_cpp/` in this repository.
- **Third party**: mimalloc 2.1.7 is MIT and statically linked. See `THIRD_PARTY_NOTICES.md` and `source/third/mimalloc-2.1.7/LICENSE`.

No conversation transcripts, experiences, model weights, credentials or private runtime state are part of this distribution.